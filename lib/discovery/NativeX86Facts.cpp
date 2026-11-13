#include "NativeAnalysisInternal.h"

namespace notdec::bin2llvm {

class X86PcThunkAnalyzer final : public NativeAnalyzer {
public:
  std::string name() const override { return "X86PcThunkAnalyzer"; }
  int priority() const override { return 70; }

  void run(NativeProgramState &state, NativeAnalysisManager &) override {
    if (state.pointerSize() != 4) {
      return;
    }
    std::set<uint64_t> callTargets;
    for (const auto &[address, instruction] : state.instructions()) {
      (void)address;
      for (uint64_t target : instruction.DirectCallTargets) {
        callTargets.insert(target);
      }
    }
    for (uint64_t target : callTargets) {
      std::optional<std::string> registerName = pcThunkRegister(state, target);
      if (!registerName || state.functionContaining(target) != nullptr ||
          state.functions().count(target) != 0) {
        continue;
      }
      state.addFunctionSeed(target, 5, "", "x86-pc-thunk",
                            NativeFunctionConfidence::High);
      NativeFunction function;
      function.Entry = target;
      function.RangeStart = target;
      function.RangeEnd = target + 5;
      NativeBasicBlock block;
      block.Start = target;
      block.End = target + 5;
      function.Blocks.push_back(block);
      function.Source = "x86-pc-thunk";
      function.IsPcThunk = true;
      function.PcThunkRegister = *registerName;
      state.addFunction(std::move(function));
    }
  }

private:
  // `mov (%esp), %reg; ret`: 8b <modrm:00 reg 100> 24 c3.  Returns the
  // destination register name for the two common PIC base registers.
  static std::optional<std::string> pcThunkRegister(NativeProgramState &state,
                                                    uint64_t address) {
    if (!state.isExecutableAddress(address)) {
      return std::nullopt;
    }
    std::vector<uint8_t> bytes;
    if (!readBytes(state, address, 5, bytes)) {
      return std::nullopt;
    }
    if (bytes.size() != 5 || bytes[0] != 0x8b ||
        (bytes[1] & 0xc7) != 0x04 || bytes[2] != 0x24 || bytes[3] != 0xc3) {
      return std::nullopt;
    }
    static const char *const names[] = {"EAX", "ECX", "EDX", "EBX",
                                        "ESP", "EBP", "ESI", "EDI"};
    unsigned reg = (bytes[1] >> 3) & 0x7;
    if (reg == 4) {
      return std::nullopt;
    }
    return std::string(names[reg]);
  }
};

class X86JumpTableAnalyzer final : public NativeAnalyzer {
public:
  std::string name() const override { return "X86JumpTableAnalyzer"; }
  int priority() const override { return 70; }

  void run(NativeProgramState &state, NativeAnalysisManager &) override {
    std::vector<NativeUnresolvedFlow> unresolvedFlows = state.unresolvedFlows();
    for (const NativeUnresolvedFlow &flow : unresolvedFlows) {
      if (flow.Kind != NativeUnresolvedFlowKind::IndirectBranch) {
        continue;
      }
      recoverJumpTableAt(state, flow.Address);
    }
  }

private:
  static void recoverJumpTableAt(NativeProgramState &state,
                                 uint64_t branchAddress) {
    bool completeForAnyFunction = false;
    for (const auto &[entry, function] : state.functions()) {
      (void)entry;
      if (!functionHasBlockContaining(function, branchAddress)) {
        continue;
      }
      completeForAnyFunction |=
          recoverJumpTableInFunction(state, function, branchAddress);
    }
    if (completeForAnyFunction) {
      state.removeUnresolvedFlow(branchAddress,
                                 NativeUnresolvedFlowKind::IndirectBranch);
    }
  }

  static bool recoverJumpTableInFunction(NativeProgramState &state,
                                         const NativeFunction &function,
                                         uint64_t branchAddress) {
    std::optional<X86PicI32JumpDispatch> dispatch =
        matchX86PicI32OffsetDispatch(state, function, branchAddress);
    if (!dispatch) {
      return false;
    }

    std::vector<uint64_t> targets;
    bool complete = false;
    if (!readX86PicI32Targets(state, dispatch->TableBase,
                              dispatch->EntryCount,
                              /*onlyExistingBlocks=*/false, targets,
                              complete)) {
      return false;
    }
    if (targets.empty()) {
      return false;
    }

    std::vector<uint64_t> decodedTargets;
    for (uint64_t target : targets) {
      if (state.instructionAt(target) != nullptr) {
        decodedTargets.push_back(target);
      }
    }
    if (decodedTargets.empty()) {
      return false;
    }

    state.addInstructionDirectFlowTargets(branchAddress, decodedTargets);
    std::vector<uint64_t> existingBlockTargets;
    for (uint64_t target : decodedTargets) {
      if (functionContainsBlockStart(state, target)) {
        existingBlockTargets.push_back(target);
      }
    }
    state.addBasicBlockSuccessors(function.Entry, dispatch->BlockStart,
                                  existingBlockTargets);
    for (uint64_t target : decodedTargets) {
      NativeXref xref;
      xref.From = branchAddress;
      xref.To = target;
      xref.Kind = NativeXrefKind::Flow;
      xref.Source = "x86-jump-table";
      state.addXref(std::move(xref));
    }
    (void)complete;
    return true;
  }

  static bool functionHasBlockContaining(const NativeFunction &function,
                                         uint64_t address) {
    for (const NativeBasicBlock &block : function.Blocks) {
      if (address >= block.Start && address < block.End) {
        return true;
      }
    }
    return false;
  }
};

class FlowFactNormalizer final : public NativeAnalyzer {
public:
  std::string name() const override { return "FlowFactNormalizer"; }
  int priority() const override { return 80; }

  void run(NativeProgramState &state, NativeAnalysisManager &) override {
    if (state.hasGtirbControlFlowAuthority()) {
      foldEhFrameOnlyBranchTargets(state);
      splitGtirbFlowXrefBlocks(state);
      restoreIntraFunctionFlowXrefSuccessors(state);
      recoverExternalFunctionPointerFlows(state);
      return;
    }

    std::vector<std::pair<uint64_t, NativeBasicBlock>> missingBlocks;
    for (const auto &[entry, function] : state.functions()) {
      appendMissingBlocks(state, function, missingBlocks);
    }
    for (auto &[entry, block] : missingBlocks) {
      state.addBasicBlock(entry, std::move(block));
    }
    std::vector<std::pair<uint64_t, NativeBasicBlock>> splitBlocks;
    for (const auto &[entry, function] : state.functions()) {
      appendFlowBoundarySplitBlocks(state, function, splitBlocks);
    }
    for (auto &[entry, block] : splitBlocks) {
      state.addBasicBlock(entry, std::move(block));
    }
    std::vector<std::pair<uint64_t, NativeBasicBlock>> targetBlocks;
    for (const auto &[entry, function] : state.functions()) {
      appendDecodedDirectTargetBlocks(state, function, targetBlocks);
    }
    for (auto &[entry, block] : targetBlocks) {
      state.addBasicBlock(entry, std::move(block));
    }
    foldEhFrameOnlyBranchTargets(state);
    for (const auto &[entry, function] : state.functions()) {
      state.removeInvalidBasicBlockSuccessors(entry);
      for (const NativeBasicBlock &block : function.Blocks) {
        normalizeBlockSuccessors(state, function, block);
      }
    }
    resolveCfgBackedIndirectBranches(state);
    classifyIndirectTailExits(state);
    recoverExternalFunctionPointerFlows(state);
  }

private:
  // GTIRB/ddisasm can resolve an indirect branch through CFG edges even when
  // SLEIGH p-code still reports BRANCHIND.  Keep the instruction-level flow
  // facts and unresolved diagnostics aligned with the already-known block CFG.
  static void resolveCfgBackedIndirectBranches(NativeProgramState &state) {
    std::vector<NativeUnresolvedFlow> unresolvedFlows = state.unresolvedFlows();
    for (const NativeUnresolvedFlow &flow : unresolvedFlows) {
      if (flow.Kind != NativeUnresolvedFlowKind::IndirectBranch) {
        continue;
      }

      const NativeFunction *function = state.functionContaining(flow.Address);
      if (function == nullptr) {
        continue;
      }
      const NativeBasicBlock *block =
          functionBlockContaining(*function, flow.Address);
      const NativeInstruction *instruction = state.instructionAt(flow.Address);
      if (block == nullptr || instruction == nullptr ||
          instruction->FlowKind != NativeInstructionFlowKind::IndirectBranch ||
          block->Successors.empty()) {
        continue;
      }

      std::vector<uint64_t> localTargets;
      for (uint64_t successor : block->Successors) {
        if (functionHasBlockStartingAt(*function, successor)) {
          localTargets.push_back(successor);
        }
      }
      if (localTargets.empty()) {
        continue;
      }

      state.addInstructionDirectFlowTargets(flow.Address, localTargets);
      state.removeUnresolvedFlow(flow.Address, flow.Kind);
    }
  }

  static void classifyIndirectTailExits(NativeProgramState &state) {
    std::vector<NativeUnresolvedFlow> unresolvedFlows = state.unresolvedFlows();
    for (const NativeUnresolvedFlow &flow : unresolvedFlows) {
      if (flow.Kind != NativeUnresolvedFlowKind::IndirectBranch) {
        continue;
      }

      const NativeFunction *function = state.functionContaining(flow.Address);
      if (function == nullptr) {
        continue;
      }
      const NativeBasicBlock *block =
          functionBlockContaining(*function, flow.Address);
      const NativeInstruction *instruction = state.instructionAt(flow.Address);
      if (block == nullptr || instruction == nullptr ||
          instruction->FlowKind != NativeInstructionFlowKind::IndirectBranch ||
          instruction->end() != block->End || block->End != function->RangeEnd ||
          !block->Successors.empty()) {
        continue;
      }

      NativeUnresolvedFlow tail = flow;
      tail.Kind = NativeUnresolvedFlowKind::IndirectTailBranch;
      tail.Source = "native-block-indirect-tail-exit";
      state.removeUnresolvedFlow(flow.Address, flow.Kind);
      state.addUnresolvedFlow(std::move(tail));
    }
  }

  static void recoverExternalFunctionPointerFlows(NativeProgramState &state) {
    std::vector<NativeUnresolvedFlow> unresolvedFlows = state.unresolvedFlows();
    for (const NativeUnresolvedFlow &flow : unresolvedFlows) {
      if (flow.Kind != NativeUnresolvedFlowKind::IndirectBranch &&
          flow.Kind != NativeUnresolvedFlowKind::IndirectCall &&
          flow.Kind != NativeUnresolvedFlowKind::IndirectTailBranch) {
        continue;
      }

      const NativeFunction *function = state.functionContaining(flow.Address);
      if (function == nullptr) {
        continue;
      }

      std::vector<NativeInstruction> instructions;
      std::map<uint64_t, size_t> indexByAddress;
      for (const NativeInstruction *instruction :
           state.instructionsInRange(function->RangeStart, function->RangeEnd)) {
        indexByAddress[instruction->Address] = instructions.size();
        instructions.push_back(*instruction);
      }
      auto indexIterator = indexByAddress.find(flow.Address);
      if (indexIterator == indexByAddress.end()) {
        continue;
      }

      std::optional<X86ExternalFunctionPointerMatch> match =
          matchX86ExternalFunctionPointerInstruction(state, instructions,
                                                     indexIterator->second);
      if (!match) {
        continue;
      }

      NativeXref xref;
      xref.From = flow.Address;
      xref.To = match->SlotAddress;
      xref.Kind = match->Kind;
      xref.Source = match->Kind == NativeXrefKind::Call
                        ? "x86-call-external-function-pointer"
                        : "x86-tail-branch-external-function-pointer";
      state.addXref(std::move(xref));
      state.removeUnresolvedFlow(flow.Address, flow.Kind);
    }
  }

  static void foldEhFrameOnlyBranchTargets(NativeProgramState &state) {
    std::vector<std::pair<uint64_t, uint64_t>> folds;
    for (const auto &[entry, function] : state.functions()) {
      for (const NativeBasicBlock &block : function.Blocks) {
        const NativeInstruction *terminator = nullptr;
        for (const NativeInstruction *instruction :
             state.instructionsInRange(block.Start, block.End)) {
          terminator = instruction;
        }
        if (terminator == nullptr ||
            terminator->FlowKind == NativeInstructionFlowKind::UnconditionalBranch) {
          continue;
        }
        std::vector<uint64_t> targets = terminator->DirectFlowTargets;
        if (terminator->FlowKind == NativeInstructionFlowKind::ConditionalBranch) {
          for (uint64_t target : terminator->TailFlowTargets) {
            addUniqueAddress(targets, target);
          }
        }
        for (uint64_t target : targets) {
          if (target == entry || functionHasBlockStartingAt(function, target)) {
            continue;
          }
          if (!isFoldableEhFrameBranchTarget(state, entry, target)) {
            continue;
          }
          folds.push_back({entry, target});
        }
      }
    }
    for (const NativeXref &xref : state.xrefs()) {
      if (xref.Kind != NativeXrefKind::Flow) {
        continue;
      }
      const NativeFunction *owner = state.functionContaining(xref.From);
      if (owner == nullptr ||
          !isFoldableEhFrameBranchTarget(state, owner->Entry, xref.To)) {
        continue;
      }
      folds.push_back({owner->Entry, xref.To});
    }
    for (const auto &[entry, function] : state.functions()) {
      auto seedIterator = state.functionSeeds().find(entry);
      if (seedIterator == state.functionSeeds().end() ||
          !hasSource(seedIterator->second, "eh-frame") ||
          seedIterator->second.Sources.size() != 1) {
        continue;
      }
      if (function.Source != "gtirb-seed-range-fallback") {
        continue;
      }
      for (const NativeBasicBlock &block : function.Blocks) {
        const NativeInstruction *terminator = nullptr;
        for (const NativeInstruction *instruction :
             state.instructionsInRange(block.Start, block.End)) {
          terminator = instruction;
        }
        if (terminator == nullptr ||
            terminator->FlowKind !=
                NativeInstructionFlowKind::UnconditionalBranch) {
          continue;
        }
        for (uint64_t target : terminator->TailFlowTargets) {
          const NativeFunction *owner = state.functionContaining(target);
          if (owner != nullptr && owner->Entry != entry) {
            folds.push_back({owner->Entry, entry});
          }
        }
      }
    }

    for (const auto &[ownerEntry, targetEntry] : folds) {
      const NativeFunction *targetFunction = state.functionAt(targetEntry);
      if (targetFunction == nullptr) {
        continue;
      }
      std::vector<NativeBasicBlock> blocks = targetFunction->Blocks;
      for (NativeBasicBlock &block : blocks) {
        state.addBasicBlock(ownerEntry, std::move(block));
      }
      restoreFoldedEhFrameFlowTargets(state, ownerEntry, targetEntry);
      state.demoteFunctionSeedToRangeHint(targetEntry);
      state.removeFunction(targetEntry);
    }
    restoreIntraFunctionFlowXrefSuccessors(state);
  }

  static bool isFoldableEhFrameBranchTarget(const NativeProgramState &state,
                                            uint64_t ownerEntry,
                                            uint64_t target) {
    const NativeFunction *targetFunction = state.functionAt(target);
    if (targetFunction == nullptr) {
      return false;
    }
    auto seedIterator = state.functionSeeds().find(target);
    if (seedIterator == state.functionSeeds().end() ||
        !hasSource(seedIterator->second, "eh-frame")) {
      return false;
    }
    if (targetFunction->Source == "gtirb-seed-range-fallback" &&
        seedIterator->second.Sources.size() == 1) {
      return true;
    }
    if (targetFunction->Source != "gtirb-ddisasm") {
      return false;
    }
    if (isNativeRuntimeFunction(state, *targetFunction)) {
      return false;
    }

    // ddisasm may split compiler generated cold blocks into separate
    // functions when .eh_frame has a separate FDE for the block.  Treat such a
    // range as a function boundary hint only when there is no real call edge to
    // the entry; otherwise it may be an address-taken function.
    std::optional<uint64_t> flowOwner;
    for (const NativeXref *xref : state.xrefsTo(target)) {
      if (xref->Kind == NativeXrefKind::Call) {
        return false;
      }
      if (xref->Kind == NativeXrefKind::Flow) {
        const NativeFunction *owner = state.functionContaining(xref->From);
        if (owner == nullptr) {
          return false;
        }
        if (flowOwner && *flowOwner != owner->Entry) {
          return false;
        }
        flowOwner = owner->Entry;
      }
    }
    return flowOwner && *flowOwner == ownerEntry;
  }

  static void restoreFoldedEhFrameFlowTargets(NativeProgramState &state,
                                              uint64_t ownerEntry,
                                              uint64_t foldedEntry) {
    const NativeFunction *owner = state.functionAt(ownerEntry);
    if (owner == nullptr) {
      return;
    }
    // Folding can split copied blocks again, so keep terminator addresses and
    // resolve their current source blocks only after all needed target splits.
    struct FoldedEdge {
      uint64_t TerminatorAddress = 0;
      uint64_t Target = 0;
      bool WasTail = false;
    };
    std::vector<FoldedEdge> edges;
    for (const NativeBasicBlock &block : owner->Blocks) {
      const NativeInstruction *terminator = nullptr;
      for (const NativeInstruction *instruction :
           state.instructionsInRange(block.Start, block.End)) {
        terminator = instruction;
      }
      if (terminator == nullptr) {
        continue;
      }
      for (uint64_t target : terminator->DirectFlowTargets) {
        edges.push_back({terminator->Address, target, false});
      }
      const std::vector<uint64_t> tailTargets = terminator->TailFlowTargets;
      for (uint64_t target : tailTargets) {
        edges.push_back({terminator->Address, target, true});
      }
    }
    for (const FoldedEdge &edge : edges) {
      if (edge.Target != foldedEntry) {
        ensureFunctionBlockStartsAt(state, ownerEntry, edge.Target);
      }
    }
    owner = state.functionAt(ownerEntry);
    if (owner == nullptr) {
      return;
    }
    for (const FoldedEdge &edge : edges) {
      if (edge.Target != foldedEntry &&
          !functionHasBlockStartingAt(*owner, edge.Target)) {
        continue;
      }
      const NativeBasicBlock *sourceBlock =
          functionBlockContaining(*owner, edge.TerminatorAddress);
      if (sourceBlock == nullptr) {
        continue;
      }
      if (edge.WasTail) {
        state.restoreInstructionTailFlowTarget(edge.TerminatorAddress,
                                               edge.Target);
      }
      state.addBasicBlockSuccessors(ownerEntry, sourceBlock->Start,
                                    {edge.Target});
    }
    for (const NativeXref *xref : state.xrefsTo(foldedEntry)) {
      if (xref->Kind != NativeXrefKind::Flow) {
        continue;
      }
      const NativeFunction *sourceOwner = state.functionContaining(xref->From);
      if (sourceOwner == nullptr || sourceOwner->Entry != ownerEntry) {
        continue;
      }
      const NativeBasicBlock *sourceBlock =
          functionBlockContaining(*owner, xref->From);
      if (sourceBlock == nullptr) {
        continue;
      }
      state.addBasicBlockSuccessors(ownerEntry, sourceBlock->Start,
                                    {foldedEntry});
    }
  }

  static void restoreIntraFunctionFlowXrefSuccessors(
      NativeProgramState &state) {
    for (const NativeXref &xref : state.xrefs()) {
      if (xref.Kind != NativeXrefKind::Flow) {
        continue;
      }
      const NativeFunction *sourceOwner = state.functionContaining(xref.From);
      const NativeFunction *targetOwner = state.functionContaining(xref.To);
      if (sourceOwner == nullptr || targetOwner == nullptr ||
          sourceOwner->Entry != targetOwner->Entry ||
          !functionHasBlockStartingAt(*sourceOwner, xref.To)) {
        continue;
      }
      const NativeBasicBlock *sourceBlock =
          functionBlockContaining(*sourceOwner, xref.From);
      if (sourceBlock == nullptr) {
        continue;
      }
      state.addBasicBlockSuccessors(sourceOwner->Entry, sourceBlock->Start,
                                    {xref.To});
    }
  }

  static void splitGtirbFlowXrefBlocks(NativeProgramState &state) {
    std::vector<std::pair<uint64_t, uint64_t>> splits;
    auto addSplit = [&](uint64_t entry, uint64_t address) {
      if (address == 0) {
        return;
      }
      auto item = std::make_pair(entry, address);
      if (std::find(splits.begin(), splits.end(), item) == splits.end()) {
        splits.push_back(item);
      }
    };

    for (const NativeXref &xref : state.xrefs()) {
      if (xref.Kind != NativeXrefKind::Flow ||
          xref.Source != "gtirb-ddisasm-flow") {
        continue;
      }
      const NativeFunction *sourceOwner = state.functionContaining(xref.From);
      const NativeFunction *targetOwner = state.functionContaining(xref.To);
      if (sourceOwner == nullptr || targetOwner == nullptr ||
          sourceOwner->Entry != targetOwner->Entry) {
        continue;
      }
      if (!functionHasBlockStartingAt(*sourceOwner, xref.From)) {
        addSplit(sourceOwner->Entry, xref.From);
      }
      if (!functionHasBlockStartingAt(*sourceOwner, xref.To)) {
        addSplit(sourceOwner->Entry, xref.To);
      }
    }

    for (const auto &[entry, address] : splits) {
      ensureFunctionBlockStartsAt(state, entry, address);
    }
  }

  static bool ensureFunctionBlockStartsAt(NativeProgramState &state,
                                          uint64_t functionEntry,
                                          uint64_t address) {
    const NativeFunction *function = state.functionAt(functionEntry);
    if (function == nullptr) {
      return false;
    }
    if (functionHasBlockStartingAt(*function, address)) {
      return true;
    }
    const NativeBasicBlock *containing =
        functionBlockContaining(*function, address);
    if (containing == nullptr || containing->Start == address) {
      return false;
    }

    NativeBasicBlock block;
    block.Start = address;
    block.End = containing->End;
    block.Successors = containing->Successors;
    state.addBasicBlock(functionEntry, std::move(block));

    function = state.functionAt(functionEntry);
    return function != nullptr && functionHasBlockStartingAt(*function, address);
  }

  static void appendMissingBlocks(
      const NativeProgramState &state, const NativeFunction &function,
      std::vector<std::pair<uint64_t, NativeBasicBlock>> &result) {
    std::vector<const NativeInstruction *> instructions =
        state.instructionsInRange(function.RangeStart, function.RangeEnd);
    for (size_t index = 0; index < instructions.size(); ++index) {
      const NativeInstruction *instruction = instructions[index];
      if (functionHasBlockContaining(function, instruction->Address)) {
        continue;
      }

      NativeBasicBlock block;
      block.Start = instruction->Address;
      size_t endIndex = index;
      while (endIndex < instructions.size()) {
        const NativeInstruction *current = instructions[endIndex];
        if (endIndex != index &&
            functionHasBlockContaining(function, current->Address)) {
          break;
        }
        block.End = current->end();
        if (current->FlowKind != NativeInstructionFlowKind::None) {
          addInstructionSuccessors(function, *current, block.Successors);
          break;
        }
        if (endIndex + 1 == instructions.size() ||
            !isInstructionFallthroughTo(
                *current, instructions[endIndex + 1]->Address)) {
          addInstructionSuccessors(function, *current, block.Successors);
          break;
        }
        ++endIndex;
      }

      if (block.Start < block.End) {
        result.push_back({function.Entry, std::move(block)});
      }
      index = endIndex;
    }
  }

  static void appendFlowBoundarySplitBlocks(
      const NativeProgramState &state, const NativeFunction &function,
      std::vector<std::pair<uint64_t, NativeBasicBlock>> &result) {
    for (const NativeInstruction *instruction :
         state.instructionsInRange(function.RangeStart, function.RangeEnd)) {
      for (uint64_t target : instruction->DirectFlowTargets) {
        appendSplitBlockAt(function, target, result);
      }
      if (instruction->FlowKind == NativeInstructionFlowKind::ConditionalBranch &&
          instruction->Fallthrough &&
          isInstructionFallthroughTo(*instruction, *instruction->Fallthrough)) {
        appendSplitBlockAt(function, *instruction->Fallthrough, result);
      }
    }
  }

  static void appendDecodedDirectTargetBlocks(
      const NativeProgramState &state, const NativeFunction &function,
      std::vector<std::pair<uint64_t, NativeBasicBlock>> &result) {
    // Some decoded cold blocks are outside the seed range but have already been
    // reached by a direct edge. Import only unowned decoded targets.
    for (const NativeInstruction *instruction :
         state.instructionsInRange(function.RangeStart, function.RangeEnd)) {
      for (uint64_t target : instruction->DirectFlowTargets) {
        appendDecodedDirectTargetBlockAt(state, function, target, result);
      }
    }
  }

  static void appendDecodedDirectTargetBlockAt(
      const NativeProgramState &state, const NativeFunction &function,
      uint64_t address,
      std::vector<std::pair<uint64_t, NativeBasicBlock>> &result) {
    if (!state.isExecutableAddress(address) ||
        state.instructionAt(address) == nullptr ||
        functionHasBlockContaining(function, address)) {
      return;
    }
    if (auto owner = state.functionContaining(address)) {
      if (owner->Entry != function.Entry) {
        return;
      }
    }
    for (const auto &[entry, block] : result) {
      if (entry == function.Entry && block.Start == address) {
        return;
      }
    }

    std::vector<const NativeInstruction *> instructions =
        state.instructionsInRange(address, executableDecodeLimit(state, address));
    if (instructions.empty() || instructions.front()->Address != address) {
      return;
    }

    NativeBasicBlock block;
    block.Start = address;
    for (size_t index = 0; index < instructions.size(); ++index) {
      const NativeInstruction *current = instructions[index];
      if (index != 0 && functionHasBlockContaining(function, current->Address)) {
        break;
      }
      block.End = current->end();
      if (current->FlowKind != NativeInstructionFlowKind::None) {
        break;
      }
      if (index + 1 == instructions.size() ||
          !isInstructionFallthroughTo(*current, instructions[index + 1]->Address)) {
        break;
      }
    }
    if (block.Start < block.End) {
      result.push_back({function.Entry, std::move(block)});
    }
  }

  static uint64_t executableDecodeLimit(const NativeProgramState &state,
                                        uint64_t address) {
    for (const NativeMemoryRange &range : state.memoryRanges()) {
      if (!range.Executable ||
          !containsAddress(range.Start, range.Size, address)) {
        continue;
      }
      return range.Start + range.Size;
    }
    return address;
  }

  static void appendSplitBlockAt(
      const NativeFunction &function, uint64_t address,
      std::vector<std::pair<uint64_t, NativeBasicBlock>> &result) {
    if (functionHasBlockStartingAt(function, address)) {
      return;
    }
    const NativeBasicBlock *containing =
        functionBlockContaining(function, address);
    if (containing == nullptr || address == containing->Start) {
      return;
    }
    NativeBasicBlock block;
    block.Start = address;
    block.End = containing->End;
    block.Successors = containing->Successors;
    result.push_back({function.Entry, std::move(block)});
  }

  static void normalizeBlockSuccessors(NativeProgramState &state,
                                       const NativeFunction &function,
                                       const NativeBasicBlock &block) {
    const NativeInstruction *terminator = nullptr;
    for (const NativeInstruction *instruction :
         state.instructionsInRange(block.Start, block.End)) {
      terminator = instruction;
    }
    if (terminator == nullptr) {
      return;
    }

    if (terminator->FlowKind == NativeInstructionFlowKind::None &&
        terminator->Fallthrough &&
        isInstructionFallthroughTo(*terminator, *terminator->Fallthrough) &&
        functionHasBlockStartingAt(function, *terminator->Fallthrough)) {
      state.addBasicBlockSuccessors(function.Entry, block.Start,
                                    {*terminator->Fallthrough});
    }

    for (uint64_t target : terminator->DirectFlowTargets) {
      if (std::find(block.Successors.begin(), block.Successors.end(),
                    target) != block.Successors.end()) {
        continue;
      }
      if (functionHasBlockStartingAt(function, target)) {
        state.addBasicBlockSuccessors(function.Entry, block.Start, {target});
        continue;
      }
      state.markInstructionTailFlowTarget(terminator->Address, target);
    }
  }

  static bool functionHasBlockStartingAt(const NativeFunction &function,
                                         uint64_t address) {
    for (const NativeBasicBlock &block : function.Blocks) {
      if (block.Start == address) {
        return true;
      }
    }
    return false;
  }

  static bool functionHasBlockContaining(const NativeFunction &function,
                                         uint64_t address) {
    return functionBlockContaining(function, address) != nullptr;
  }

  static const NativeBasicBlock *
  functionBlockContaining(const NativeFunction &function, uint64_t address) {
    for (const NativeBasicBlock &block : function.Blocks) {
      if (address >= block.Start && address < block.End) {
        return &block;
      }
    }
    return nullptr;
  }

  static void addInstructionSuccessors(
      const NativeFunction &function, const NativeInstruction &instruction,
      std::vector<uint64_t> &successors) {
    if (instruction.FlowKind == NativeInstructionFlowKind::ConditionalBranch) {
      addLocalSuccessors(function, instruction.DirectFlowTargets, successors);
      if (instruction.Fallthrough &&
          isInstructionFallthroughTo(instruction, *instruction.Fallthrough)) {
        addLocalSuccessor(function, *instruction.Fallthrough, successors);
      }
      return;
    }
    if (instruction.FlowKind == NativeInstructionFlowKind::UnconditionalBranch ||
        instruction.FlowKind == NativeInstructionFlowKind::IndirectBranch) {
      addLocalSuccessors(function, instruction.DirectFlowTargets, successors);
      return;
    }
    if (instruction.FlowKind == NativeInstructionFlowKind::Trap) {
      return;
    }
    if (instruction.FlowKind == NativeInstructionFlowKind::None &&
        instruction.Fallthrough &&
        isInstructionFallthroughTo(instruction, *instruction.Fallthrough)) {
      addLocalSuccessor(function, *instruction.Fallthrough, successors);
    }
  }

  static void addLocalSuccessors(const NativeFunction &function,
                                 const std::vector<uint64_t> &targets,
                                 std::vector<uint64_t> &successors) {
    for (uint64_t target : targets) {
      addLocalSuccessor(function, target, successors);
    }
  }

  static void addLocalSuccessor(const NativeFunction &function, uint64_t target,
                                std::vector<uint64_t> &successors) {
    if (!functionHasBlockStartingAt(function, target)) {
      return;
    }
    addUniqueAddress(successors, target);
  }
};

std::unique_ptr<NativeAnalyzer> createX86PcThunkAnalyzer() {
  return std::make_unique<X86PcThunkAnalyzer>();
}

std::unique_ptr<NativeAnalyzer> createX86JumpTableAnalyzer() {
  return std::make_unique<X86JumpTableAnalyzer>();
}

std::unique_ptr<NativeAnalyzer> createFlowFactNormalizer() {
  return std::make_unique<FlowFactNormalizer>();
}

} // namespace notdec::bin2llvm
