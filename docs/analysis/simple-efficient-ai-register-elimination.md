# Simple and Efficient Abstract Interpretation for Register Elimination in Binary Analysis

## 1. Background

Native binary lifting starts from a machine-state model.  General-purpose
registers, flags, segment bases, and vector registers are represented as global
variables in LLVM IR.  This is faithful to the lifted program, but it is not a
good source-level interface: every function appears to communicate through
global register memory, and ordinary call/return structure is hidden behind
loads and stores.

`abstract-interpretation-register-summary.md` describes the first abstraction
used by the native register summary pass.  For each register, the analysis keeps
three facts:

```text
mayEntry     the current value may still be the function entry value
mayNonEntry  the current value may have been produced inside the function
readEntry    the function entry value may have been read before being killed
```

The state is sparse.  A missing register cell means the default untouched state:

```text
mayEntry = true
mayNonEntry = false
readEntry = false
```

CFG joins use a may-style union.  Reads set `readEntry` only if the current value
may still be the entry value.  Writes kill `mayEntry` and set `mayNonEntry`.
Internal calls use the callee summary; external or indirect calls use ABI
metadata.  This gives a cheap bottom-up summary of which registers are read,
preserved, and modified by each lifted function.

The current implementation extends that base model in two directions.  First,
the summary also computes a top-down demand: changed return registers are kept
only if callers observe them.  Second, demand is tracked as a bit mask, so wide
backing registers such as `ZMM0` can be refined to ABI low-lane slots such as
`XMM0_Qa`.  This matters for x86-64 because the ABI describes floating-point
arguments and returns through XMM aliases, while the lifted register model may
store them in larger ZMM globals.

The implementation is split across three passes:

- `NativeRegisterSummary` computes register effects and demand metadata.
- `NativeRegisterSummarySSA` consumes the metadata, builds SSA values for live
  register state, rewrites function signatures, and removes dead register
  residue.
- `NativeRegisterFinalCleanup` runs after SSA rewrite.  It invokes GlobalDCE and
  clears summary metadata only on functions that no longer contain concrete
  register residue.

## 2. Method

### 2.1 Overview

The pipeline is deliberately simple.  It does not try to recover all source
types or memory aliases.  It only answers the questions needed to eliminate
register globals:

```text
Which entry registers are real function inputs?
Which changed registers are real function outputs?
Which register loads/stores can be replaced by SSA values?
Which residual register globals and helper declarations are dead?
```

The analysis has two main stages.

1. **Bottom-up effect summary.**  Each defined function is analyzed over its CFG.
   The result says, for every register, whether the exit value may be the entry
   value, whether it may be a new value, and whether the entry value may have
   been read.  Recursive functions are handled by SCC iteration on the call
   graph.
2. **Top-down demand summary.**  Starting from externally visible roots and
   caller observations, demanded output registers are propagated back through
   internal calls.  This filters out modified registers that nobody uses.  The
   demand is a bit mask, not just a boolean.

After these summaries, `NativeRegisterSummarySSA` rewrites the IR:

1. It builds SSA values for register loads, with PHIs at CFG joins.
2. It materializes unknown call effects only when needed.
3. It rewrites internal and known external function signatures.
4. It removes dead register stores and unused helper uses exposed by rewrite.
5. It runs late stack-frame and stack-canary cleanup after signature rewrite has
   exposed more dead code.
6. A final cleanup pass runs GlobalDCE and drops stale summary metadata from
   functions with no remaining register loads, stores, or helper calls.

The design goal is to keep each abstract domain small.  Most domains are finite
sets of booleans or bit masks, so ordinary worklist fixpoint iteration is enough.
No widening is needed.

### 2.2 Register Units and ABI Slots

The analysis first collects register globals from `notdec.register` metadata.
Each global becomes a `RegisterUnit`:

```text
RegisterUnit = {
  global,
  space,
  name,
  offset,
  size
}
```

ABI metadata is then mapped onto these units.  This mapping is not always
one-to-one.  On x86-64, ABI entries such as `XMM0_Qa` may have no register
space in metadata, while the lifted IR only has a larger `ZMM0` global.  The
summary therefore records a storage mask:

```text
XMM0_Qa -> ZMM0 low 64 bits
XMMn   -> ZMMn backing unit
RAX    -> RAX full 64 bits
```

The mask is used in both directions:

- ABI inputs and outputs seed demand only for the relevant lane.
- SummarySSA can lower a demanded ZMM low lane into a `float` or `double`
  signature slot instead of exposing `i512`.

If an ABI storage cannot be mapped precisely, the pass falls back to a full
register mask.  That is conservative but can leave wider signatures.

### 2.3 Bottom-Up Register Effect Analysis

The bottom-up analysis is a forward abstract interpretation over each function's
CFG.  The per-register domain is:

```text
Cell = {
  mayEntry: bool,
  mayNonEntry: bool,
  readEntry: bool
}
```

The block state contains a sparse map from register globals to cells.  It also
tracks a small amount of stack-local evidence for saved-register recovery:

```text
State = {
  reachable,
  cells: Register -> Cell,
  stackSlots: fixed local slot -> saved entry register,
  valueOrigins: SSA value -> entry register
}
```

The main transfer rules are:

```text
read R:
  readEntry[R] = readEntry[R] OR mayEntry[R]

write R:
  mayEntry[R] = false
  mayNonEntry[R] = true

restore R from proven saved entry value:
  mayEntry[R] = true
  mayNonEntry[R] = false
```

CFG joins use pointwise OR for the three cell bits.  Unreachable predecessors
are ignored.  Missing cells are treated as the untouched default, so predecessor
order does not affect sparse joins.

Internal calls apply the callee effect summary:

```text
post.readEntry =
  pre.readEntry OR (callee.readEntry AND pre.mayEntry)

post.mayEntry =
  callee.mayEntry AND pre.mayEntry

post.mayNonEntry =
  (callee.mayEntry AND pre.mayNonEntry) OR callee.mayNonEntry
```

External and indirect calls use ABI fallback:

- ABI input registers are read.
- ABI unaffected registers are preserved.
- ABI output or killed-by-call registers are written.

The analysis is interprocedural.  The call graph is partitioned into SCCs.  Each
SCC is analyzed until all member function effects stop changing, then callers
see a stable summary.

### 2.4 Saved-Register Refinement

Some callee-saved registers are written in the function body but restored before
return.  Treating every write as a visible clobber would create false outputs.

The pass therefore keeps a narrow stack-local model.  It tracks stores of entry
register values into fixed frame slots, and later recognizes loads from the same
slots.  When a register is restored from such a proven saved value, its cell is
refined back to:

```text
mayEntry = true
mayNonEntry = false
```

This refinement is intentionally local.  It is used for stack-frame save/restore
patterns, not for arbitrary memory aliasing.

### 2.5 Partial-Write Filtering

Lifters often model partial register writes as:

```llvm
old    = load @REG
keep   = and old, KEEP_MASK
insert = ...
store (or keep, insert), @REG
```

The load of `old` does not mean the function semantically uses the entry value
of `REG`.  It may only preserve lanes outside the write.  The summary pass
recognizes this keep-high pattern and avoids turning that load into input
evidence.

Later, SummarySSA has a stronger partial-demand rewrite.  If only the written
lane is demanded, the old preserved lane can be replaced by zero and marked with
`notdec.register.summary_ssa.zero_demand_operand`.  This makes the generated IR
easier to debug: zeros introduced by demand pruning are explicit.

### 2.6 Top-Down Demand Analysis

The bottom-up summary says what a function may change.  It does not say whether
callers care.  The top-down pass computes that second fact.

For every function:

```text
FunctionDemand = {
  exitDemand:  Register -> APInt mask,
  entryDemand: Register -> APInt mask
}
```

`exitDemand` says which changed exit bits are observed by callers or roots.
`entryDemand` says which function-entry bits feed demanded observations.  The
mask is important for wide registers.  A demand of `0xffffffffffffffff` on
`ZMM0` means low 64 bits only if the mask width is interpreted against the ZMM
backing unit.

Root functions seed demand for the first integer ABI output.  Float ABI outputs
are not seeded as default root returns.  This avoids turning ordinary entry
points into accidental `double` or `i512` returns.

Inside a caller, demand is propagated backwards through the CFG:

- A demanded register load adds demand for the loaded register.
- A register store kills previous demand for that register.
- A direct internal call propagates live demand to the callee's exit demand when
  the callee may produce a non-entry value.
- ABI outputs and killed-by-call effects kill previous demand across external or
  indirect calls.

The bit mask for a register load comes from a local value-demand analysis.
This analysis currently walks from observers such as returns, branches, ordinary
stores, and call arguments back through integer operations.  For
example:

```llvm
%x = load i512, ptr @ZMM0
%y = trunc i512 %x to i64
ret i64 %y
```

Only the low 64 bits of `ZMM0` are demanded.

The current implementation still treats all call arguments as observers in the
local value-demand pass.  This is conservative, but it can keep self-sustaining
recursive pass-through values.  A pure cycle such as:

```text
f(zmm) -> f(zmm)
```

has both the empty demand and the full demand as mathematical fixed points.  For
register elimination we want the least fixed point: if no real observer reads
the value, the demand should be empty.  The intended refinement is to avoid
seeding direct internal call arguments locally; instead, internal call arguments
should receive demand only from the callee's computed entry demand.  External
and indirect call arguments remain real observers.

### 2.7 Summary Metadata

The summary is attached to each function as metadata:

```text
notdec.register.summary
notdec.register.summary.read_entry
notdec.register.summary.preserves
notdec.register.summary.modifies
notdec.register.summary.demanded_returns
```

Each per-register entry records:

```text
name
read_entry
may_entry
may_non_entry
exit_demand
entry_demand_mask
exit_demand_mask
```

The metadata drives SummarySSA and is also useful for debugging.  It is not a
fresh post-cleanup fact.  Later IR rewrites can remove all concrete register
uses from a function while old summary metadata still mentions `read_entry`.
The final cleanup pass removes summary metadata only when register residue has
fully disappeared according to the cleanup rules.

### 2.8 SummarySSA Construction

`NativeRegisterSummarySSA` uses the summary facts to replace register memory
with SSA values.

For each function, the builder lazily creates values:

- Entry loads for registers that are function inputs.
- PHIs at CFG joins.
- Call return helpers for demanded call outputs that cannot yet be rewritten.
- Call clobber helpers for still-used clobbered registers.
- Frozen poison when a value is unknown and cannot be represented safely.

The builder scans backwards from each register load:

1. If a previous store to the same register exists in the block, use that value.
2. If a previous call preserves the register, keep scanning.
3. If a previous call returns or clobbers the register, materialize that call
   value.
4. Otherwise, read the block entry value.

Block entry values are recursively resolved from predecessors.  Multiple
predecessors create a PHI.  Trivial PHIs are simplified.  This is close in
spirit to pruned SSA construction: values are created only for register loads
that survive demand and cleanup.

### 2.9 Signature Recovery and Rewrite

The pass builds an initial signature shape for each function.

For known external functions, a small prototype table gives fixed or typed
arguments and returns.  Unknown external function arity is inferred from call
sites, but clobber-derived values are not used as strong parameter evidence.
Warnings are emitted when inferred external signatures are incomplete or
inconsistent.

For internal functions, the summary decides which registers become parameters
and returns:

- Entry-read registers become parameters when they are in the internal parameter
  register set.
- Changed and demanded exit registers become returns when they are in the
  internal return register set.
- Float ABI backing units use demand masks.  If the demand fits the ABI low
  lane, the slot becomes `float` or `double`.  If a real non-lane demand remains,
  the pass may keep the whole integer backing register.

After shapes are chosen, the rewriter:

1. Creates replacement functions with normal LLVM parameters and returns.
2. Replaces entry register loads with function arguments.
3. Builds aggregate returns when multiple registers are returned.
4. Rewrites call sites to pass SSA values instead of storing register globals.
5. Replaces summary return helpers with extracted return values.
6. Deletes old calls, old functions, and marked call-argument stores when they
   become unused.

This is the point where many global register accesses disappear from the IR.

### 2.10 Residue Removal

Signature rewrite exposes more dead stores and dead helper calls.  SummarySSA
therefore runs a bounded cleanup loop:

1. Run local InstCombine/SimplifyCFG when enabled.
2. Remove dead register stores again using the rewritten call information.
3. Stop when no more dead stores are removed, or after a small iteration limit.

After that, stack-frame cleanup can remove frame-local scaffolding that became
dead, and stack-canary cleanup is run again because register and stack rewrites
often expose the canonical canary pattern.

`NativeRegisterFinalCleanup` then runs LLVM GlobalDCE, scans each defined
function for register loads, register stores, or `notdec.register.*` helper
calls, and clears register-summary metadata only on functions with no such
residue.  It runs GlobalDCE once before metadata cleanup and once after it, so
dead helper declarations and unreferenced register globals can disappear through
LLVM's normal global-dead-code logic.

## 3. Discussion

The useful property of this design is that each analysis is small:

- Bottom-up effects use three booleans per register.
- Demand uses finite APInt masks.
- SSA construction is lazy and local to observed loads.
- Signature rewrite consumes summaries instead of re-solving register dataflow.

This makes the chain fast enough for batch binary analysis while still removing
most artificial register state.

The main precision risk is recursion.  A direct internal call should not create
demand by itself; otherwise a recursive pass-through value can justify its own
existence.  The least fixed point for such a cycle is empty unless some real
observer exists.  This is the next place where the current implementation should
be tightened: internal call arguments should be demanded from the callee's entry
demand, while external and indirect call arguments remain conservative roots.

Another risk is stale metadata.  Summary metadata records facts before later IR
cleanup.  Final IR should be audited by concrete register accesses and function
signatures, not by `read_entry` metadata alone.
