# 二进制分析中用于寄存器消除的简单高效抽象解释

## 1. 背景

Native 二进制 lifting 从机器状态模型开始。通用寄存器、flags、段基址和向量寄存器在 LLVM IR 里都表示成全局变量。这个表示忠实于 lifted 程序，但不适合作为源码级接口：每个函数看起来都通过全局寄存器内存通信，普通的调用和返回结构也被隐藏在 load/store 后面。

`abstract-interpretation-register-summary.md` 描述了 native register summary pass 使用的第一个抽象。对每个寄存器，分析保留三个事实：

```text
mayEntry     当前值可能仍然是函数入口值
mayNonEntry  当前值可能是在函数内部产生的值
readEntry    函数入口值可能在被覆盖前被读取过
```

状态是稀疏的。map 里缺少某个寄存器 cell 表示默认 untouched 状态：

```text
mayEntry = true
mayNonEntry = false
readEntry = false
```

CFG 合流使用 may-style union。读寄存器时，只有当前值仍可能是入口值，才设置 `readEntry`。写寄存器会清掉 `mayEntry` 并设置 `mayNonEntry`。内部调用使用 callee summary；外部调用或间接调用使用 ABI metadata。这样可以便宜地得到每个 lifted 函数读取、保留、修改哪些寄存器。

当前实现沿着两个方向扩展了这个基础模型。第一，summary 还会计算 top-down demand：只有 caller 真正观察的 changed return register 才会保留。第二，demand 用 bit mask 跟踪，所以 `ZMM0` 这种宽 backing register 可以细化到 `XMM0_Qa` 这种 ABI low-lane slot。对 x86-64 来说这很重要，因为 ABI 通过 XMM alias 描述浮点参数和返回值，而 lifted register model 可能把它们存进更大的 ZMM global。

实现分成三个 pass：

- `NativeRegisterSummary` 计算寄存器 effect 和 demand metadata。
- `NativeRegisterSummarySSA` 消费 metadata，给 live register state 构建 SSA 值，重写函数签名，并移除死的 register residue。
- `NativeRegisterFinalCleanup` 在 SSA rewrite 之后运行。它调用 GlobalDCE，并且只在函数里不再有具体 register residue 时清掉 summary metadata。

## 2. 方法

### 2.1 总览

这条 pipeline 刻意保持简单。它不尝试恢复所有源码类型，也不尝试做完整 memory alias 分析。它只回答寄存器全局变量消除所需的几个问题：

```text
哪些 entry register 是真实函数输入？
哪些 changed register 是真实函数输出？
哪些 register load/store 可以替换成 SSA 值？
哪些残留 register global 和 helper declaration 是死的？
```

分析有两个主要阶段。

1. **Bottom-up effect summary。** 对每个有定义的函数，在它的 CFG 上做分析。结果说明每个寄存器的出口值是否可能仍然是入口值、是否可能是新值，以及入口值是否可能被读取过。递归函数通过 call graph SCC 迭代处理。
2. **Top-down demand summary。** 从外部可见 root 和 caller observation 开始，把 demanded output register 反向传播到内部调用。这样可以过滤掉没人使用的 modified register。demand 是 bit mask，不只是 bool。

得到这些 summary 后，`NativeRegisterSummarySSA` 重写 IR：

1. 为 register load 构建 SSA 值，在 CFG 合流点创建 PHI。
2. 只在需要时 materialize unknown call effect。
3. 重写内部函数和已知外部函数的签名。
4. 移除 rewrite 暴露出来的死 register store 和未使用 helper use。
5. 在签名重写暴露出更多死代码后，运行后期 stack-frame 和 stack-canary cleanup。
6. final cleanup pass 运行 GlobalDCE，并从没有剩余 register load、store 或 helper call 的函数上清掉旧 summary metadata。

设计目标是让每个抽象域都很小。大多数 domain 都只是有限 bool 集合或 bit mask，所以普通 worklist fixpoint iteration 就够了。不需要 widening。

### 2.2 寄存器单元和 ABI Slot

分析首先从 `notdec.register` metadata 收集 register global。每个 global 变成一个 `RegisterUnit`：

```text
RegisterUnit = {
  global,
  space,
  name,
  offset,
  size
}
```

然后把 ABI metadata 映射到这些 unit。这个映射不总是一对一。在 x86-64 上，`XMM0_Qa` 这样的 ABI entry 可能没有 register space metadata，而 lifted IR 里只有更大的 `ZMM0` global。因此 summary 会记录 storage mask：

```text
XMM0_Qa -> ZMM0 low 64 bits
XMMn   -> ZMMn backing unit
RAX    -> RAX full 64 bits
```

mask 会双向使用：

- ABI input 和 output 只对相关 lane seed demand。
- SummarySSA 可以把 demanded ZMM low lane 降成 `float` 或 `double` 签名 slot，而不是暴露 `i512`。

如果 ABI storage 不能精确映射，pass 会退回到 full register mask。这样是保守的，但可能留下更宽的签名。

### 2.3 Bottom-Up Register Effect Analysis

bottom-up 分析是在每个函数 CFG 上做 forward abstract interpretation。每个寄存器的 domain 是：

```text
Cell = {
  mayEntry: bool,
  mayNonEntry: bool,
  readEntry: bool
}
```

block state 包含从 register global 到 cell 的稀疏 map。它还会跟踪少量 stack-local evidence，用于 saved-register recovery：

```text
State = {
  reachable,
  cells: Register -> Cell,
  stackSlots: fixed local slot -> saved entry register,
  valueOrigins: SSA value -> entry register
}
```

主要 transfer rule 是：

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

CFG 合流对三个 cell bit 做 pointwise OR。不可达 predecessor 会被忽略。缺失 cell 按 untouched default 处理，所以 predecessor 枚举顺序不会影响 sparse join。

内部调用应用 callee effect summary：

```text
post.readEntry =
  pre.readEntry OR (callee.readEntry AND pre.mayEntry)

post.mayEntry =
  callee.mayEntry AND pre.mayEntry

post.mayNonEntry =
  (callee.mayEntry AND pre.mayNonEntry) OR callee.mayNonEntry
```

外部调用和间接调用使用 ABI fallback：

- ABI input register 被读取。
- ABI unaffected register 被保留。
- ABI output 或 killed-by-call register 被写入。

这个分析是过程间的。call graph 会被切成 SCC。每个 SCC 会迭代分析，直到所有成员函数的 effect 不再变化，然后 caller 才看到稳定 summary。

### 2.4 Saved-Register Refinement

有些 callee-saved register 会在函数体内被写入，但返回前又恢复。把每次写都当成可见 clobber 会制造虚假的 output。

所以 pass 保留了一个很窄的 stack-local model。它跟踪 entry register value 存入固定 frame slot 的 store，之后识别从同一 slot load 回来的值。当某个寄存器从这种已证明的 saved value 恢复时，它的 cell 被细化回：

```text
mayEntry = true
mayNonEntry = false
```

这个 refinement 有意保持局部。它只用于 stack-frame save/restore pattern，不用于任意 memory aliasing。

### 2.5 Partial-Write Filtering

lifter 经常把 partial register write 表示成：

```llvm
old    = load @REG
keep   = and old, KEEP_MASK
insert = ...
store (or keep, insert), @REG
```

这里读取 `old` 不表示函数语义上使用了 `REG` 的入口值。它可能只是为了保留写入范围之外的 lane。summary pass 会识别这种 keep-high pattern，避免把这个 load 当成输入证据。

之后 SummarySSA 还有更强的 partial-demand rewrite。如果只有写入 lane 被 demand，旧的 preserved lane 可以替换成 0，并用 `notdec.register.summary_ssa.zero_demand_operand` 标记。这样生成的 IR 更容易 debug：由 demand pruning 引入的 0 是显式的。

### 2.6 Top-Down Demand Analysis

bottom-up summary 说明函数可能修改什么。它不说明 caller 是否关心这些修改。top-down pass 计算的就是第二件事。

对每个函数：

```text
FunctionDemand = {
  exitDemand:  Register -> APInt mask,
  entryDemand: Register -> APInt mask
}
```

`exitDemand` 表示 caller 或 root 观察了哪些 changed exit bit。`entryDemand` 表示哪些函数入口 bit 会流到 demanded observation。mask 对宽寄存器很重要。比如 `ZMM0` 上的 `0xffffffffffffffff` demand，只有按 ZMM backing unit 的宽度解释时才表示低 64 bit。

root function 会给第一个整数 ABI output seed demand。浮点 ABI output 不作为默认 root return seed。这样可以避免把普通入口点误判成返回 `double` 或 `i512`。

在 caller 内部，demand 沿 CFG 反向传播：

- demanded register load 会给被加载的 register 增加 demand。
- register store 会 kill 该 register 之前的 demand。
- direct internal call 在 callee 可能产生 non-entry value 时，把 live demand 传播到 callee 的 exit demand。
- external 或 indirect call 处，ABI output 和 killed-by-call effect 会 kill 之前的 demand。

register load 的 bit mask 来自局部 value-demand analysis。这个分析目前从 return、branch、ordinary store、call argument 这类 observer 开始，沿整数运算反向传播。例如：

```llvm
%x = load i512, ptr @ZMM0
%y = trunc i512 %x to i64
ret i64 %y
```

这里只有 `ZMM0` 的低 64 bit 被 demand。

当前实现仍然把所有 call argument 都当成局部 value-demand pass 的 observer。这样比较保守，但可能保留自我支撑的递归 pass-through 值。比如纯循环：

```text
f(zmm) -> f(zmm)
```

它在数学上同时有 empty demand 和 full demand 两个 fixed point。对寄存器消除来说，我们想要 least fixed point：如果没有真实 observer 读取这个值，demand 就应该是空。后续应收紧的地方是：不要在局部分析里直接 seed direct internal call argument；internal call argument 应只从 callee 计算出来的 entry demand 接收 demand。external 和 indirect call argument 仍然保持为保守 root。

### 2.7 Summary Metadata

summary 会作为 metadata 挂到每个函数上：

```text
notdec.register.summary
notdec.register.summary.read_entry
notdec.register.summary.preserves
notdec.register.summary.modifies
notdec.register.summary.demanded_returns
```

每个寄存器 entry 记录：

```text
name
read_entry
may_entry
may_non_entry
exit_demand
entry_demand_mask
exit_demand_mask
```

metadata 驱动 SummarySSA，也方便 debug。它不是 cleanup 之后的新事实。后续 IR rewrite 可能已经从某个函数里移除了所有具体 register use，但旧 summary metadata 仍然提到 `read_entry`。final cleanup pass 只会在 register residue 按 cleanup rule 完全消失后移除 summary metadata。

### 2.8 SummarySSA Construction

`NativeRegisterSummarySSA` 使用 summary fact，把 register memory 替换成 SSA 值。

对每个函数，builder 会惰性创建值：

- 作为函数输入的 register 的 entry load。
- CFG 合流点上的 PHI。
- 还不能重写时，为 demanded call output 创建 call return helper。
- 为仍被使用的 clobbered register 创建 call clobber helper。
- 当某个值未知且不能安全表示时，使用 frozen poison。

builder 从每个 register load 开始向后扫描：

1. 如果当前 block 里前面有同一 register 的 store，使用 store 的值。
2. 如果前面有一个保留该 register 的 call，继续向前扫描。
3. 如果前面的 call 返回或 clobber 该 register，materialize 对应 call value。
4. 否则读取 block entry value。

block entry value 会从 predecessor 递归求解。多个 predecessor 会创建 PHI。trivial PHI 会被简化。整体上接近 pruned SSA construction：只有 survive demand 和 cleanup 的 register load 才会创建值。

### 2.9 Signature Recovery and Rewrite

pass 会为每个函数构造初始 signature shape。

对已知外部函数，小型 prototype table 会给出固定或带类型的参数和返回值。未知外部函数的 arity 会从 call site 推断，但 clobber-derived value 不作为强参数证据。如果推断出来的外部签名不完整或不一致，会输出 warning。

对内部函数，summary 决定哪些寄存器变成参数和返回值：

- entry-read register 在属于 internal parameter register set 时变成参数。
- changed 且 demanded exit register 在属于 internal return register set 时变成返回值。
- float ABI backing unit 使用 demand mask。如果 demand 落在 ABI low lane 内，slot 变成 `float` 或 `double`。如果还存在真实 non-lane demand，pass 可能保留整个 integer backing register。

shape 选好后，rewriter 会：

1. 创建使用普通 LLVM 参数和返回值的 replacement function。
2. 用函数参数替换 entry register load。
3. 多个寄存器返回时构造 aggregate return。
4. 重写 call site，让它们传 SSA 值，而不是写 register global。
5. 用提取出来的返回值替换 summary return helper。
6. 在 old call、old function 和标记过的 call-argument store 变成 unused 时删除它们。

这是很多全局 register access 从 IR 中消失的阶段。

### 2.10 Residue Removal

signature rewrite 会暴露更多 dead store 和 dead helper call。因此 SummarySSA 会运行一个有界 cleanup loop：

1. 启用时运行局部 InstCombine/SimplifyCFG。
2. 使用重写后的 call 信息再次移除 dead register store。
3. 当不再移除 dead store，或者达到较小的迭代上限时停止。

之后，stack-frame cleanup 可以移除变成死代码的 frame-local scaffolding；stack-canary cleanup 也会再次运行，因为 register 和 stack rewrite 经常会暴露 canonical canary pattern。

`NativeRegisterFinalCleanup` 随后运行 LLVM GlobalDCE，扫描每个有定义函数里的 register load、register store 或 `notdec.register.*` helper call，并且只在函数没有这些 residue 时清除 register-summary metadata。它在 metadata cleanup 前后各运行一次 GlobalDCE，所以死 helper declaration 和未引用 register global 可以通过 LLVM 正常的 global-dead-code 逻辑消失。

## 3. 讨论

这个设计有用的性质是每个分析都很小：

- bottom-up effect 每个寄存器只用三个 bool。
- demand 使用有限 APInt mask。
- SSA construction 是惰性的，只围绕实际观察到的 load。
- signature rewrite 消费 summary，不重新求解 register dataflow。

这让整条链路足够快，可以用于批量二进制分析，同时仍能移除大多数人为引入的 register state。

主要精度风险是递归。direct internal call 不应该自己制造 demand；否则递归 pass-through value 会证明自己应该存在。对这种 cycle，least fixed point 应该是空，除非存在某个真实 observer。这是当前实现下一步应该收紧的地方：internal call argument 应从 callee 的 entry demand 接收 demand，而 external 和 indirect call argument 仍然作为保守 root。

另一个风险是 stale metadata。Summary metadata 记录的是后续 IR cleanup 之前的事实。最终 IR 应该通过具体 register access 和函数签名来审计，而不是只看 `read_entry` metadata。
