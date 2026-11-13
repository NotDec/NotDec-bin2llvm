#include "NativeAnalysisInternal.h"

namespace notdec::bin2llvm {

class SleighSeedInstructionAnalyzer final : public NativeAnalyzer {
public:
  explicit SleighSeedInstructionAnalyzer(NativeSleighDecodeOptions options)
      : Options(options) {}

  std::string name() const override { return "SleighSeedInstructionAnalyzer"; }
  int priority() const override { return 60; }

  void run(NativeProgramState &state, NativeAnalysisManager &) override {
    if (!resolveSpecOptions(state)) {
      return;
    }

    LiefElfLoadImage loadImage(state.binary(), NullErrors);
    if (!loadImage.hasExecutableBytes()) {
      state.addNote("sleigh instruction decode skipped: no executable bytes");
      return;
    }

    addRelocationCodeSeeds(state);
    SleighInstructionDecoder decoder(loadImage, SpecOptions, NullErrors);
    if (!decoder.isValid()) {
      state.addNote("sleigh instruction decode skipped: engine init failed");
      return;
    }
    if (Options.DecodeExistingBlocksOnly) {
      decodeExistingBlocks(state, decoder);
      return;
    }

    std::deque<DecodeQueueItem> decodeQueue;
    std::set<std::pair<uint64_t, uint64_t>> queuedSeeds;
    std::set<std::pair<uint64_t, uint64_t>> decodedSeeds;
    enqueueInitialSeeds(state, Options, decodeQueue, queuedSeeds);

    uint64_t decodedSeedCount = 0;
    while (!decodeQueue.empty()) {
      if (Options.MaxDecodedSeeds &&
          decodedSeedCount >= *Options.MaxDecodedSeeds) {
        state.addNote("sleigh instruction decode stopped after explicit seed "
                      "limit " +
                      std::to_string(*Options.MaxDecodedSeeds));
        break;
      }
      DecodeQueueItem item = decodeQueue.front();
      decodeQueue.pop_front();
      std::pair<uint64_t, uint64_t> seedKey{item.FunctionEntry,
                                            item.BlockAddress};
      if (!decodedSeeds.insert(seedKey).second ||
          !state.isExecutableAddress(item.BlockAddress)) {
        continue;
      }
      DecodeSeedResult result =
          decodeSeed(state, decoder, item.FunctionEntry, item.BlockAddress);
      ++decodedSeedCount;
      for (uint64_t target : result.CallTargets) {
        if (Options.RuntimeFilter.SkipRuntimeFunctions &&
            isNativeRuntimeAddress(state, target)) {
          continue;
        }
        state.addFunctionSeed(target, 0, "", "sleigh-direct-call",
                              NativeFunctionConfidence::High);
        enqueueSeed(state, target, target, decodeQueue, queuedSeeds);
      }
      for (uint64_t target : result.TailBranchTargets) {
        if (Options.RuntimeFilter.SkipRuntimeFunctions &&
            isNativeRuntimeAddress(state, target)) {
          continue;
        }
        state.addFunctionSeed(target, 0, "", "sleigh-tail-branch",
                              NativeFunctionConfidence::High);
        enqueueSeed(state, target, target, decodeQueue, queuedSeeds);
      }
      for (uint64_t target : result.BranchTargets) {
        if (targetBelongsToFunctionRange(state, item.FunctionEntry, target)) {
          enqueueSeed(state, item.FunctionEntry, target, decodeQueue,
                      queuedSeeds);
        }
      }
      if (result.FallthroughTarget) {
        if (targetBelongsToFunctionRange(state, item.FunctionEntry,
                                         *result.FallthroughTarget)) {
          enqueueSeed(state, item.FunctionEntry, *result.FallthroughTarget,
                      decodeQueue, queuedSeeds);
        }
      }
      enqueueRecoveredJumpTableTargets(state, item.FunctionEntry, decodeQueue,
                                       queuedSeeds);
    }
  }

private:
  // Decode every known entry seed, but keep each local decode small.  That makes
  // module-wide discovery cover real Bench2 binaries without letting one bad
  // function consume a large linear range.
  static constexpr uint64_t MaxInstructionsPerSeed = 8;
  static constexpr uint64_t MaxBytesPerSeed = 64;

  struct DecodedFlowInfo {
    std::vector<uint64_t> BranchTargets;
    std::vector<uint64_t> CallTargets;
    std::vector<uint64_t> InternalCallTargets;
    bool HasConditionalBranch = false;
    bool HasUnconditionalBranch = false;
    bool HasIndirectBranch = false;
    bool HasReturn = false;
    bool HasIndirectCall = false;
  };

  struct DirectControlFlowResult {
    std::map<uint64_t, DecodedFlowInfo> FlowInfos;
    std::vector<NativeXref> Xrefs;
    std::vector<NativeUnresolvedFlow> UnresolvedFlows;
  };

  // A seed decode is intentionally small. If the window ends without a real
  // control-flow terminator, the next address is still part of the same
  // function and should be decoded as another block.
  struct DecodeSeedResult {
    std::vector<uint64_t> CallTargets;
    // A direct jump can hand off to another function instead of staying inside
    // the current CFG.  Decode that target separately so this function does not
    // absorb the callee's blocks.
    std::vector<uint64_t> TailBranchTargets;
    std::vector<uint64_t> BranchTargets;
    std::optional<uint64_t> FallthroughTarget;
  };

  // Direct calls start a new function.  Direct branches keep the same function
  // entry and decode another block for that function.
  struct DecodeQueueItem {
    uint64_t FunctionEntry = 0;
    uint64_t BlockAddress = 0;
  };

  std::ostringstream NullErrors;
  SleighSpecOptions SpecOptions;
  NativeSleighDecodeOptions Options;

  struct ExistingDecodeRange {
    uint64_t FunctionEntry = 0;
    uint64_t Start = 0;
    uint64_t End = 0;
  };

  void decodeExistingBlocks(NativeProgramState &state,
                            SleighInstructionDecoder &decoder) {
    std::vector<ExistingDecodeRange> ranges;
    for (const auto &[entry, function] : state.functions()) {
      for (const NativeBasicBlock &block : function.Blocks) {
        ranges.push_back({entry, block.Start, block.End});
      }
    }
    for (const ExistingDecodeRange &range : ranges) {
      decodeExistingBlock(state, decoder, range.FunctionEntry, range.Start,
                          range.End);
    }
  }

  void decodeExistingBlock(NativeProgramState &state,
                           SleighInstructionDecoder &decoder,
                           uint64_t functionEntry, uint64_t start,
                           uint64_t end) {
    if (start >= end || !state.isExecutableAddress(start)) {
      return;
    }
    std::optional<uint64_t> availableBytes = executableBytesFrom(state, start);
    if (!availableBytes || *availableBytes == 0) {
      return;
    }
    uint64_t requestedBytes = std::min<uint64_t>(end - start, *availableBytes);
    SleighInstructionDecode decode =
        decoder.decode(start, std::numeric_limits<uint64_t>::max(),
                       requestedBytes, NullErrors);
    std::vector<NativeInstruction> decodedInstructions;
    for (const SleighInstructionSummary &summary : decode.Instructions) {
      if (summary.Address < start || summary.Address + summary.Size > end) {
        continue;
      }
      NativeInstruction instruction;
      instruction.Address = summary.Address;
      instruction.Size = summary.Size;
      instruction.Mnemonic = summary.Body.empty()
                                  ? summary.Mnemonic
                                  : summary.Mnemonic + " " + summary.Body;
      instruction.Source = "sleigh-gtirb-block";
      (void)readBytes(state, summary.Address, summary.Size, instruction.Bytes);
      decodedInstructions.push_back(std::move(instruction));
    }
    bool collectMachineFlow = !state.hasGtirbControlFlowAuthority();
    DirectControlFlowResult flowResult =
        collectDirectControlFlow(state, decode.Pcode, collectMachineFlow);
    annotateDecodedInstructionFlows(decodedInstructions, flowResult.FlowInfos);
    std::vector<uint64_t> tailBranchTargets;
    if (collectMachineFlow) {
      collectTailBranchTargets(state, functionEntry, decodedInstructions,
                               tailBranchTargets);
    }
    for (const NativeInstruction &instruction : decodedInstructions) {
      state.addInstruction(instruction);
    }
    for (const NativeXref &xref : flowResult.Xrefs) {
      state.addXref(xref);
    }
    for (const NativeUnresolvedFlow &flow : flowResult.UnresolvedFlows) {
      state.addUnresolvedFlow(flow);
    }
    if (collectMachineFlow && !decodedInstructions.empty()) {
      uint64_t rangeStart = decodedInstructions.front().Address;
      uint64_t rangeEnd =
          decodedInstructions.back().Address + decodedInstructions.back().Size;
      std::vector<uint64_t> branchTargets;
      addDecodedFunctionBlocks(state, functionEntry, rangeStart, rangeEnd,
                               decodedInstructions, std::nullopt,
                               branchTargets);
    }
  }

  static void addRelocationCodeSeeds(NativeProgramState &state) {
    for (const auto &[slot, target] : state.relocatedPointers()) {
      (void)slot;
      if (!state.isExecutableAddress(target)) {
        continue;
      }
      state.addFunctionSeed(target, 0, "", "elf-relocation-code",
                            NativeFunctionConfidence::Low);
    }
  }

  bool resolveSpecOptions(NativeProgramState &state) {
    const LIEF::ELF::Binary &binary = state.binary();
    std::optional<NativeElfArchitectureSpec> archSpec =
        nativeElfArchitectureSpec(binary);
    if (!archSpec) {
      state.addNote(unsupportedNativeElfArchitectureMessage(
          binary, "SLEIGH instruction decode"));
      return false;
    }

    std::filesystem::path specRoot =
        std::filesystem::path(NOTDEC_BIN2LLVM_DEFAULT_GHIDRA_SOURCE_DIR) /
        "Ghidra/Processors/x86/data/languages";
    std::filesystem::path slaPath = specRoot / archSpec->SlaFileName;
    std::filesystem::path pspecPath = specRoot / archSpec->PspecFileName;
    if (!std::filesystem::exists(slaPath)) {
      state.addNote("sleigh instruction decode missing spec: " +
                    slaPath.string());
      return false;
    }

    SpecOptions.SlaFileName = slaPath.string();
    if (std::filesystem::exists(pspecPath)) {
      SpecOptions.PspecFileName = pspecPath.string();
    }
    return true;
  }

  static void enqueueInitialSeeds(NativeProgramState &state,
                                  const NativeSleighDecodeOptions &options,
                                  std::deque<DecodeQueueItem> &decodeQueue,
                                  std::set<std::pair<uint64_t, uint64_t>>
                                      &queuedSeeds) {
    if (!options.InitialFunctionEntries.empty()) {
      for (uint64_t entry : options.InitialFunctionEntries) {
        enqueueSeed(state, entry, entry, decodeQueue, queuedSeeds);
      }
      return;
    }

    for (NativeFunctionConfidence confidence :
         {NativeFunctionConfidence::High, NativeFunctionConfidence::Medium,
          NativeFunctionConfidence::Low}) {
      for (const NativeFunctionWorkItem &item : state.functionWorklist()) {
        auto seedIterator = state.functionSeeds().find(item.Address);
        if (seedIterator == state.functionSeeds().end() ||
            seedIterator->second.Confidence != confidence) {
          continue;
        }
        if (options.RuntimeFilter.SkipRuntimeFunctions &&
            isNativeRuntimeSeed(state, seedIterator->second)) {
          continue;
        }
        enqueueSeed(state, item.Address, item.Address, decodeQueue,
                    queuedSeeds);
      }
    }
  }

  static bool enqueueSeed(NativeProgramState &state, uint64_t functionEntry,
                          uint64_t blockAddress,
                          std::deque<DecodeQueueItem> &decodeQueue,
                          std::set<std::pair<uint64_t, uint64_t>>
                              &queuedSeeds) {
    if (!state.isExecutableAddress(functionEntry) ||
        !state.isExecutableAddress(blockAddress) ||
        !queuedSeeds.insert({functionEntry, blockAddress}).second) {
      return false;
    }
    decodeQueue.push_back({functionEntry, blockAddress});
    return true;
  }

  static void enqueueRecoveredJumpTableTargets(
      NativeProgramState &state, uint64_t functionEntry,
      std::deque<DecodeQueueItem> &decodeQueue,
      std::set<std::pair<uint64_t, uint64_t>> &queuedSeeds) {
    const NativeFunction *function = state.functionAt(functionEntry);
    if (function == nullptr) {
      return;
    }

    for (const NativeUnresolvedFlow &flow : state.unresolvedFlows()) {
      if (flow.Kind != NativeUnresolvedFlowKind::IndirectBranch ||
          !functionContainsAddress(*function, flow.Address)) {
        continue;
      }

      std::optional<X86PicI32JumpDispatch> dispatch =
          matchX86PicI32OffsetDispatch(state, *function, flow.Address);
      if (!dispatch) {
        continue;
      }

      std::vector<uint64_t> targets;
      bool complete = false;
      if (!readX86PicI32Targets(state, dispatch->TableBase,
                                dispatch->EntryCount,
                                /*onlyExistingBlocks=*/false, targets,
                                complete)) {
        continue;
      }
      for (uint64_t target : targets) {
        if (targetBelongsToFunctionRange(state, functionEntry, target)) {
          enqueueSeed(state, functionEntry, target, decodeQueue, queuedSeeds);
        }
      }
    }
  }

  DecodeSeedResult decodeSeed(NativeProgramState &state,
                              SleighInstructionDecoder &decoder,
                              uint64_t functionEntry, uint64_t address) {
    DecodeSeedResult result;
    std::optional<uint64_t> availableBytes = executableBytesFrom(state, address);
    if (!availableBytes || *availableBytes == 0) {
      return result;
    }

    uint64_t decodeBytes = std::min(
        MaxBytesPerSeed,
        boundedBytesForFunctionSeed(state, functionEntry, address,
                                    *availableBytes));
    SleighInstructionDecode decode =
        decoder.decode(address, MaxInstructionsPerSeed, decodeBytes,
                       NullErrors);
    std::vector<NativeInstruction> decodedInstructions;
    uint64_t rangeStart = 0;
    uint64_t rangeEnd = 0;
    for (const SleighInstructionSummary &summary : decode.Instructions) {
      if (rangeStart == 0) {
        rangeStart = summary.Address;
      }
      rangeEnd = summary.Address + summary.Size;

      NativeInstruction instruction;
      instruction.Address = summary.Address;
      instruction.Size = summary.Size;
      instruction.Mnemonic = summary.Body.empty()
                                  ? summary.Mnemonic
                                  : summary.Mnemonic + " " + summary.Body;
      instruction.Source = "sleigh-seed-linear";
      (void)readBytes(state, summary.Address, summary.Size, instruction.Bytes);
      decodedInstructions.push_back(std::move(instruction));
    }
    DirectControlFlowResult flowResult;
    std::map<uint64_t, DecodedFlowInfo> flowInfos;
    if (rangeStart == address && rangeStart < rangeEnd) {
      flowResult = collectDirectControlFlow(state, decode.Pcode,
                                            /*collectMachineFlow=*/true);
      flowInfos = flowResult.FlowInfos;
    }
    annotateDecodedInstructionFlows(decodedInstructions, flowInfos);
    collectTailBranchTargets(state, functionEntry, decodedInstructions,
                             result.TailBranchTargets);
    // A seed is decoded linearly, but only locally reachable instructions should
    // become block facts for this function entry.
    std::set<uint64_t> reachableStarts =
        reachableInstructionStarts(decodedInstructions, rangeStart);
    decodedInstructions.erase(
        std::remove_if(decodedInstructions.begin(), decodedInstructions.end(),
                       [&](const NativeInstruction &instruction) {
                         return reachableStarts.count(instruction.Address) == 0;
                       }),
        decodedInstructions.end());
    if (decodedInstructions.empty()) {
      return result;
    }
    for (uint64_t target : reachableCallTargets(flowResult, reachableStarts)) {
      addUniqueAddress(result.CallTargets, target);
    }
    for (const NativeXref &xref : reachableXrefs(flowResult, reachableStarts)) {
      state.addXref(xref);
    }
    std::set<uint64_t> resolvedIndirectFlowStarts =
        collectExternalFunctionPointerXrefs(state, decodedInstructions);
    std::set<uint64_t> trapStarts = trapInstructionStarts(decodedInstructions);
    for (const NativeUnresolvedFlow &flow :
         reachableUnresolvedFlows(flowResult, reachableStarts, trapStarts,
                                  resolvedIndirectFlowStarts)) {
      state.addUnresolvedFlow(flow);
    }
    rangeStart = decodedInstructions.front().Address;
    rangeEnd =
        decodedInstructions.back().Address + decodedInstructions.back().Size;
    result.FallthroughTarget = fallthroughTargetForDecodedWindow(
        state, functionEntry, rangeStart, rangeEnd, decodeBytes,
        decodedInstructions);
    if (result.FallthroughTarget) {
      decodedInstructions.back().Fallthrough = *result.FallthroughTarget;
    }
    for (const NativeInstruction &instruction : decodedInstructions) {
      state.addInstruction(instruction);
    }
    addDecodedFunctionBlocks(state, functionEntry, rangeStart, rangeEnd,
                             decodedInstructions, result.FallthroughTarget,
                             result.BranchTargets);
    return result;
  }

  static void collectTailBranchTargets(
      const NativeProgramState &state, uint64_t functionEntry,
      std::vector<NativeInstruction> &instructions,
      std::vector<uint64_t> &tailBranchTargets) {
    for (NativeInstruction &instruction : instructions) {
      std::vector<uint64_t> localTargets;
      for (uint64_t target : instruction.DirectFlowTargets) {
        if (isTailBranchTarget(state, functionEntry, instruction, target)) {
          addUniqueAddress(tailBranchTargets, target);
          addUniqueAddress(instruction.TailFlowTargets, target);
          continue;
        }
        addUniqueAddress(localTargets, target);
      }
      instruction.DirectFlowTargets = std::move(localTargets);
    }
  }

  static bool isTailBranchTarget(const NativeProgramState &state,
                                 uint64_t functionEntry,
                                 const NativeInstruction &instruction,
                                 uint64_t target) {
    if (instruction.FlowKind != NativeInstructionFlowKind::UnconditionalBranch) {
      return false;
    }
    if (state.lookupPltExternal(target)) {
      return true;
    }
    if (isKnownOtherFunctionEntry(state, functionEntry, target)) {
      return true;
    }
    if (!isDynamicArrayThunkSeed(state, functionEntry)) {
      return false;
    }
    return target < functionEntry;
  }

  static bool isDynamicArrayThunkSeed(const NativeProgramState &state,
                                      uint64_t functionEntry) {
    auto seedIterator = state.functionSeeds().find(functionEntry);
    if (seedIterator == state.functionSeeds().end()) {
      return false;
    }
    const NativeFunctionSeed &seed = seedIterator->second;
    if (seed.RangeStart != 0 && seed.RangeEnd > seed.RangeStart) {
      return false;
    }
    return std::find(seed.Sources.begin(), seed.Sources.end(),
                     "dt-init-array") != seed.Sources.end() ||
           std::find(seed.Sources.begin(), seed.Sources.end(),
                     "dt-fini-array") != seed.Sources.end();
  }

  static std::set<uint64_t> reachableInstructionStarts(
      const std::vector<NativeInstruction> &instructions, uint64_t entry) {
    std::map<uint64_t, const NativeInstruction *> instructionByAddress;
    for (const NativeInstruction &instruction : instructions) {
      instructionByAddress[instruction.Address] = &instruction;
    }

    std::set<uint64_t> reachable;
    std::vector<uint64_t> worklist;
    if (instructionByAddress.count(entry) != 0) {
      worklist.push_back(entry);
    }
    while (!worklist.empty()) {
      uint64_t address = worklist.back();
      worklist.pop_back();
      if (!reachable.insert(address).second) {
        continue;
      }

      const NativeInstruction &instruction = *instructionByAddress[address];
      for (uint64_t target : instruction.DirectFlowTargets) {
        if (instructionByAddress.count(target) != 0) {
          worklist.push_back(target);
        }
      }
      if (instruction.Fallthrough &&
          isInstructionFallthroughTo(instruction, *instruction.Fallthrough) &&
          instructionByAddress.count(*instruction.Fallthrough) != 0) {
        worklist.push_back(*instruction.Fallthrough);
      }
    }
    return reachable;
  }

  static std::vector<uint64_t>
  reachableCallTargets(const DirectControlFlowResult &flowResult,
                       const std::set<uint64_t> &reachableStarts) {
    std::vector<uint64_t> targets;
    for (const auto &[address, info] : flowResult.FlowInfos) {
      if (reachableStarts.count(address) == 0) {
        continue;
      }
      for (uint64_t target : info.InternalCallTargets) {
        addUniqueAddress(targets, target);
      }
    }
    return targets;
  }

  static std::vector<NativeXref>
  reachableXrefs(const DirectControlFlowResult &flowResult,
                 const std::set<uint64_t> &reachableStarts) {
    std::vector<NativeXref> xrefs;
    for (const NativeXref &xref : flowResult.Xrefs) {
      if (reachableStarts.count(xref.From) != 0) {
        xrefs.push_back(xref);
      }
    }
    return xrefs;
  }

  static std::set<uint64_t> collectExternalFunctionPointerXrefs(
      NativeProgramState &state,
      const std::vector<NativeInstruction> &instructions) {
    std::set<uint64_t> resolved;
    for (size_t index = 0; index < instructions.size(); ++index) {
      const NativeInstruction &instruction = instructions[index];
      std::optional<X86ExternalFunctionPointerMatch> match =
          matchX86ExternalFunctionPointerInstruction(state, instructions,
                                                     index);
      if (!match) {
        continue;
      }

      NativeXref xref;
      xref.From = instruction.Address;
      xref.To = match->SlotAddress;
      xref.Kind = match->Kind;
      xref.Source = match->Kind == NativeXrefKind::Call
                        ? "x86-call-external-function-pointer"
                        : "x86-tail-branch-external-function-pointer";
      state.addXref(std::move(xref));
      resolved.insert(instruction.Address);
    }
    return resolved;
  }

  static std::set<uint64_t>
  trapInstructionStarts(const std::vector<NativeInstruction> &instructions) {
    std::set<uint64_t> starts;
    for (const NativeInstruction &instruction : instructions) {
      if (instruction.FlowKind == NativeInstructionFlowKind::Trap) {
        starts.insert(instruction.Address);
      }
    }
    return starts;
  }

  static std::vector<NativeUnresolvedFlow> reachableUnresolvedFlows(
      const DirectControlFlowResult &flowResult,
      const std::set<uint64_t> &reachableStarts,
      const std::set<uint64_t> &trapStarts,
      const std::set<uint64_t> &resolvedIndirectFlowStarts) {
    std::vector<NativeUnresolvedFlow> flows;
    for (const NativeUnresolvedFlow &flow : flowResult.UnresolvedFlows) {
      if (reachableStarts.count(flow.Address) != 0 &&
          trapStarts.count(flow.Address) == 0 &&
          resolvedIndirectFlowStarts.count(flow.Address) == 0) {
        flows.push_back(flow);
      }
    }
    return flows;
  }

  static void annotateDecodedInstructionFlows(
      std::vector<NativeInstruction> &instructions,
      const std::map<uint64_t, DecodedFlowInfo> &flowInfos) {
    for (size_t index = 0; index < instructions.size(); ++index) {
      NativeInstruction &instruction = instructions[index];
      auto flowIterator = flowInfos.find(instruction.Address);
      if (flowIterator != flowInfos.end()) {
        const DecodedFlowInfo &flowInfo = flowIterator->second;
        instruction.DirectFlowTargets = flowInfo.BranchTargets;
        instruction.DirectCallTargets = flowInfo.CallTargets;
        instruction.HasIndirectCall = flowInfo.HasIndirectCall;
        if (flowInfo.HasConditionalBranch) {
          instruction.FlowKind = NativeInstructionFlowKind::ConditionalBranch;
        } else if (flowInfo.HasUnconditionalBranch) {
          instruction.FlowKind = NativeInstructionFlowKind::UnconditionalBranch;
        } else if (flowInfo.HasIndirectBranch) {
          instruction.FlowKind = NativeInstructionFlowKind::IndirectBranch;
        } else if (flowInfo.HasReturn) {
          instruction.FlowKind = NativeInstructionFlowKind::Return;
        }
      }
      if (instruction.Mnemonic == "UD2") {
        instruction.FlowKind = NativeInstructionFlowKind::Trap;
        instruction.DirectFlowTargets.clear();
        instruction.TailFlowTargets.clear();
      }

      bool hasNextInstruction = index + 1 != instructions.size();
      bool hasFallthrough =
          instruction.FlowKind !=
              NativeInstructionFlowKind::UnconditionalBranch &&
          instruction.FlowKind != NativeInstructionFlowKind::IndirectBranch &&
          instruction.FlowKind != NativeInstructionFlowKind::Trap &&
          instruction.FlowKind != NativeInstructionFlowKind::Return;
      if (hasNextInstruction && hasFallthrough &&
          instructions[index + 1].Address == instruction.end()) {
        instruction.Fallthrough = instructions[index + 1].Address;
      } else if (!hasNextInstruction &&
                 instruction.FlowKind ==
                     NativeInstructionFlowKind::ConditionalBranch) {
        instruction.Fallthrough = instruction.end();
      }
    }
  }

  static std::optional<uint64_t> fallthroughTargetForDecodedWindow(
      NativeProgramState &state, uint64_t entry, uint64_t rangeStart,
      uint64_t rangeEnd, uint64_t decodeBytes,
      const std::vector<NativeInstruction> &instructions) {
    if (instructions.empty() || rangeStart == 0 || rangeStart >= rangeEnd) {
      return std::nullopt;
    }
    const NativeInstruction &last = instructions.back();
    if (last.Address + last.Size != rangeEnd) {
      return std::nullopt;
    }
    if (last.FlowKind == NativeInstructionFlowKind::UnconditionalBranch ||
        last.FlowKind == NativeInstructionFlowKind::IndirectBranch ||
        last.FlowKind == NativeInstructionFlowKind::Trap ||
        last.FlowKind == NativeInstructionFlowKind::Return) {
      return std::nullopt;
    }
    if (!state.isExecutableAddress(rangeEnd) ||
        isKnownOtherFunctionEntry(state, entry, rangeEnd)) {
      return std::nullopt;
    }
    auto seedIterator = state.functionSeeds().find(entry);
    if (seedIterator != state.functionSeeds().end()) {
      const NativeFunctionSeed &seed = seedIterator->second;
      if (seed.RangeStart != 0 && seed.RangeEnd > seed.RangeStart &&
          rangeEnd == seed.RangeEnd) {
        return std::nullopt;
      }
    }
    bool hitInstructionLimit = instructions.size() == MaxInstructionsPerSeed;
    bool hitByteLimit = rangeEnd == rangeStart + decodeBytes;
    if (!hitInstructionLimit && !hitByteLimit) {
      return std::nullopt;
    }
    return rangeEnd;
  }

  static void addDecodedFunctionBlocks(
      NativeProgramState &state, uint64_t entry, uint64_t rangeStart,
      uint64_t rangeEnd,
      const std::vector<NativeInstruction> &instructions,
      std::optional<uint64_t> fallthroughTarget,
      std::vector<uint64_t> &branchTargets) {
    if (rangeStart == 0 || rangeStart >= rangeEnd) {
      return;
    }

    std::vector<NativeBasicBlock> blocks =
        buildDecodedBlocks(instructions, rangeStart, rangeEnd);
    for (NativeBasicBlock &block : blocks) {
      if (fallthroughTarget && block.End == rangeEnd &&
          block.Successors.empty()) {
        addUniqueAddress(block.Successors, *fallthroughTarget);
      }
      eraseOutOfRangeFunctionSuccessors(state, entry, block.Successors);
      eraseKnownOtherFunctionSuccessors(state, entry, block.Successors);
      for (uint64_t successor : block.Successors) {
        if (successor < rangeStart || successor >= rangeEnd) {
          addUniqueAddress(branchTargets, successor);
        }
      }
    }
    if (blocks.empty()) {
      return;
    }

    if (state.functionAt(entry) != nullptr) {
      for (NativeBasicBlock &block : blocks) {
        state.addBasicBlock(entry, std::move(block));
      }
      return;
    }

    NativeFunction function;
    function.Entry = entry;
    function.RangeStart = rangeStart;
    function.RangeEnd = rangeEnd;
    function.Source = "sleigh-seed-linear";
    auto seedIterator = state.functionSeeds().find(entry);
    if (seedIterator != state.functionSeeds().end()) {
      function.Name = seedIterator->second.PrimaryName;
      function.IsExternallyVisible = seedIterator->second.IsExternallyVisible;
    }

    function.Blocks = std::move(blocks);
    state.addFunction(std::move(function));
  }

  static DirectControlFlowResult
  collectDirectControlFlow(NativeProgramState &state,
                           const PcodeProgram &program,
                           bool collectMachineFlow) {
    DirectControlFlowResult result;
    std::set<std::tuple<uint64_t, uint64_t, NativeXrefKind>> seenXrefs;
    // This is intentionally local to one decoded P-Code range.  It only keeps
    // enough provenance to recognize guarded GOT indirect calls.
    std::map<std::string, uint64_t> sourceRamByVarnode;
    for (const PcodeOpView &op : program.Ops) {
      if (op.Opcode == PcodeOpcode::Load) {
        trackLoadSourceRam(sourceRamByVarnode, op);
      }
      trackCopySourceRam(sourceRamByVarnode, op);

      std::optional<uint64_t> target = directRamTarget(op);
      if (op.Opcode == PcodeOpcode::Call) {
        if (target && state.isExecutableAddress(*target)) {
          DecodedFlowInfo &info = result.FlowInfos[op.Address];
          if (state.lookupPltExternal(*target)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *target,
                           NativeXrefKind::Call, "sleigh-pcode-plt-call");
            addUniqueAddress(info.CallTargets, *target);
            continue;
          }
          addPendingXref(result.Xrefs, seenXrefs, op.Address, *target,
                         NativeXrefKind::Call, "sleigh-pcode-direct-call");
          addUniqueAddress(info.CallTargets, *target);
          addUniqueAddress(info.InternalCallTargets, *target);
        }
      } else if (op.Opcode == PcodeOpcode::CallInd) {
        result.FlowInfos[op.Address].HasIndirectCall = true;
        if (op.Inputs.size() == 1 && op.Inputs[0].Space == "ram") {
          if (auto pointerTarget =
                  resolvedExecutablePointerTargetAt(state, op.Inputs[0].Offset)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *pointerTarget,
                           NativeXrefKind::Call,
                           "sleigh-pcode-pointer-indirect-call");
            continue;
          }
        }
        if (auto gotAddress = callIndGotSource(sourceRamByVarnode, op)) {
          if (auto pointerTarget =
                  resolvedExecutablePointerTargetAt(state, *gotAddress)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *pointerTarget,
                           NativeXrefKind::Call,
                           "sleigh-pcode-pointer-indirect-call");
            continue;
          }
          if (isExternalFunctionPointerRelocationAt(state, *gotAddress)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *gotAddress,
                           NativeXrefKind::Call,
                           "sleigh-pcode-got-indirect-call");
            continue;
          }
        }
        NativeUnresolvedFlow flow;
        flow.Address = op.Address;
        flow.Kind = NativeUnresolvedFlowKind::IndirectCall;
        flow.Source = "sleigh-pcode-indirect-flow";
        result.UnresolvedFlows.push_back(std::move(flow));
      } else if (op.Opcode == PcodeOpcode::Branch ||
                 op.Opcode == PcodeOpcode::CBranch) {
        if (!collectMachineFlow) {
          continue;
        }
        DecodedFlowInfo &info = result.FlowInfos[op.Address];
        if (op.Opcode == PcodeOpcode::CBranch) {
          info.HasConditionalBranch = true;
        } else {
          info.HasUnconditionalBranch = true;
        }
        if (target && state.isExecutableAddress(*target)) {
          if (state.lookupPltExternal(*target)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *target,
                           NativeXrefKind::Flow,
                           "sleigh-pcode-plt-tail-branch");
            addUniqueAddress(info.BranchTargets, *target);
            continue;
          }
          addPendingXref(result.Xrefs, seenXrefs, op.Address, *target,
                         NativeXrefKind::Flow, "sleigh-pcode-direct-flow");
          addUniqueAddress(info.BranchTargets, *target);
        }
      } else if (op.Opcode == PcodeOpcode::BranchInd) {
        if (!collectMachineFlow) {
          continue;
        }
        result.FlowInfos[op.Address].HasIndirectBranch = true;
        if (op.Inputs.size() == 1 && op.Inputs[0].Space == "ram") {
          if (auto pointerTarget =
                  resolvedExecutablePointerTargetAt(state, op.Inputs[0].Offset)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *pointerTarget,
                           NativeXrefKind::Flow,
                           "sleigh-pcode-pointer-indirect-branch");
            continue;
          }
        }
        if (auto gotAddress = branchIndGotTarget(op)) {
          if (isPltGotSlot(state, *gotAddress)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *gotAddress,
                           NativeXrefKind::Flow,
                           "sleigh-pcode-plt-indirect-branch");
            continue;
          }
          if (isPlt0ResolverSlot(state, op.Address, *gotAddress)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *gotAddress,
                           NativeXrefKind::Flow,
                           "sleigh-pcode-plt0-resolver-branch");
            continue;
          }
        }
        // Guarded external tail jumps load the GOT slot into a register before
        // BRANCHIND. Only accept slots proven by relocation as external
        // function pointers.
        if (auto gotAddress = branchIndGotSource(sourceRamByVarnode, op)) {
          if (auto pointerTarget =
                  resolvedExecutablePointerTargetAt(state, *gotAddress)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *pointerTarget,
                           NativeXrefKind::Flow,
                           "sleigh-pcode-pointer-indirect-branch");
            continue;
          }
          if (isExternalFunctionPointerRelocationAt(state, *gotAddress)) {
            addPendingXref(result.Xrefs, seenXrefs, op.Address, *gotAddress,
                           NativeXrefKind::Flow,
                           "sleigh-pcode-got-indirect-branch");
            continue;
          }
        }
        NativeUnresolvedFlow flow;
        flow.Address = op.Address;
        flow.Kind = NativeUnresolvedFlowKind::IndirectBranch;
        flow.Source = "sleigh-pcode-indirect-flow";
        result.UnresolvedFlows.push_back(std::move(flow));
      } else if (op.Opcode == PcodeOpcode::Return) {
        if (collectMachineFlow) {
          result.FlowInfos[op.Address].HasReturn = true;
        }
      } else {
        addPendingDirectDataXrefs(state, result.Xrefs, seenXrefs, op);
      }
    }
    return result;
  }

  static void trackCopySourceRam(std::map<std::string, uint64_t> &sources,
                                 const PcodeOpView &op) {
    if (op.Opcode != PcodeOpcode::Copy || !op.Output ||
        op.Inputs.size() != 1) {
      return;
    }

    std::string outputKey = varnodeStorageKey(*op.Output);
    if (auto source = sourceRam(sources, op.Inputs[0])) {
      sources[outputKey] = *source;
      return;
    }
    sources.erase(outputKey);
  }

  static void trackLoadSourceRam(std::map<std::string, uint64_t> &sources,
                                 const PcodeOpView &op) {
    if (op.Opcode != PcodeOpcode::Load || !op.Output ||
        op.Inputs.size() != 2 || op.Inputs[0].Space != "const") {
      return;
    }

    std::string outputKey = varnodeStorageKey(*op.Output);
    if (auto source = sourceRam(sources, op.Inputs[1])) {
      sources[outputKey] = *source;
      return;
    }
    sources.erase(outputKey);
  }

  static std::optional<uint64_t>
  callIndGotSource(const std::map<std::string, uint64_t> &sources,
                   const PcodeOpView &op) {
    if (op.Inputs.size() != 1) {
      return std::nullopt;
    }
    return sourceRam(sources, op.Inputs[0]);
  }

  static std::optional<uint64_t> branchIndGotTarget(const PcodeOpView &op) {
    if (op.Inputs.size() != 1 || op.Inputs[0].Space != "ram") {
      return std::nullopt;
    }
    return op.Inputs[0].Offset;
  }

  static std::optional<uint64_t>
  branchIndGotSource(const std::map<std::string, uint64_t> &sources,
                     const PcodeOpView &op) {
    if (op.Inputs.size() != 1) {
      return std::nullopt;
    }
    return sourceRam(sources, op.Inputs[0]);
  }

  static std::optional<uint64_t>
  resolvedExecutablePointerTargetAt(const NativeProgramState &state,
                                    uint64_t address) {
    std::optional<uint64_t> target = state.readPointer(address);
    if (!target || !state.isExecutableAddress(*target)) {
      return std::nullopt;
    }
    return target;
  }

  static std::optional<uint64_t>
  sourceRam(const std::map<std::string, uint64_t> &sources,
            const VarnodeView &varnode) {
    if (varnode.Space == "ram") {
      return varnode.Offset;
    }
    auto it = sources.find(varnodeStorageKey(varnode));
    if (it == sources.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  static std::string varnodeStorageKey(const VarnodeView &varnode) {
    std::ostringstream stream;
    stream << varnode.Space << ':' << std::hex << varnode.Offset << ':'
           << std::dec << varnode.Size;
    return stream.str();
  }

  static bool isPltGotSlot(const NativeProgramState &state, uint64_t address) {
    for (const NativePltEntry &entry : state.pltEntries()) {
      if (entry.GotAddress == address && !entry.SymbolName.empty()) {
        return true;
      }
    }
    return false;
  }

  static bool isPlt0ResolverSlot(const NativeProgramState &state,
                                 uint64_t branchAddress, uint64_t gotAddress) {
    std::optional<NativeSectionInfo> plt = findSection(state, ".plt");
    std::optional<NativeSectionInfo> got = findSection(state, ".got");
    if (!plt || !got) {
      return false;
    }
    return branchAddress == plt->Address + 6 && gotAddress == got->Address + 16;
  }

  static void addPendingDirectDataXrefs(
      NativeProgramState &state, std::vector<NativeXref> &xrefs,
      std::set<std::tuple<uint64_t, uint64_t, NativeXrefKind>> &seenXrefs,
      const PcodeOpView &op) {
    if (op.Output) {
      addPendingDirectDataXref(state, xrefs, seenXrefs, op.Address, *op.Output);
    }
    for (const VarnodeView &input : op.Inputs) {
      addPendingDirectDataXref(state, xrefs, seenXrefs, op.Address, input);
    }
  }

  static void addPendingDirectDataXref(
      NativeProgramState &state, std::vector<NativeXref> &xrefs,
      std::set<std::tuple<uint64_t, uint64_t, NativeXrefKind>> &seenXrefs,
      uint64_t from, const VarnodeView &varnode) {
    if (varnode.Space != "ram" || state.isExecutableAddress(varnode.Offset)) {
      return;
    }
    NativeXrefKind kind = looksLikeReadOnlyCString(state, varnode.Offset)
                              ? NativeXrefKind::String
                              : NativeXrefKind::Data;
    const char *source = kind == NativeXrefKind::String
                             ? "sleigh-pcode-direct-string"
                             : "sleigh-pcode-direct-data";
    addPendingXref(xrefs, seenXrefs, from, varnode.Offset, kind, source);
  }

  static std::optional<uint64_t> directRamTarget(const PcodeOpView &op) {
    if (op.Inputs.empty()) {
      return std::nullopt;
    }
    if (op.Opcode != PcodeOpcode::Call && op.Opcode != PcodeOpcode::Branch &&
        op.Opcode != PcodeOpcode::CBranch) {
      return std::nullopt;
    }
    const VarnodeView &target = op.Inputs.front();
    if (target.Space != "ram") {
      return std::nullopt;
    }
    return target.Offset;
  }

  static void addPendingXref(
      std::vector<NativeXref> &xrefs,
      std::set<std::tuple<uint64_t, uint64_t, NativeXrefKind>> &seenXrefs,
      uint64_t from, uint64_t to, NativeXrefKind kind, const char *source) {
    if (!seenXrefs.insert({from, to, kind}).second) {
      return;
    }
    NativeXref xref;
    xref.From = from;
    xref.To = to;
    xref.Kind = kind;
    xref.Source = source;
    xrefs.push_back(std::move(xref));
  }

  static std::vector<NativeBasicBlock>
  buildDecodedBlocks(const std::vector<NativeInstruction> &instructions,
                     uint64_t rangeStart, uint64_t rangeEnd) {
    std::vector<NativeBasicBlock> blocks;
    if (instructions.empty()) {
      return blocks;
    }

    std::set<uint64_t> instructionStarts;
    for (const NativeInstruction &instruction : instructions) {
      instructionStarts.insert(instruction.Address);
    }

    std::set<uint64_t> blockStarts;
    blockStarts.insert(rangeStart);
    for (size_t index = 0; index < instructions.size(); ++index) {
      const NativeInstruction &instruction = instructions[index];
      for (uint64_t target : instruction.DirectFlowTargets) {
        if (instructionStarts.count(target) != 0) {
          blockStarts.insert(target);
        }
      }
      if (index + 1 != instructions.size() &&
          instruction.FlowKind != NativeInstructionFlowKind::None) {
        blockStarts.insert(instructions[index + 1].Address);
      }
    }

    uint64_t blockStart = rangeStart;
    for (size_t index = 0; index < instructions.size(); ++index) {
      const NativeInstruction &instruction = instructions[index];
      uint64_t instructionEnd = instruction.Address + instruction.Size;
      bool isLastInstruction = index + 1 == instructions.size();
      bool nextStartsBlock =
          !isLastInstruction &&
          blockStarts.count(instructions[index + 1].Address) != 0;

      std::vector<uint64_t> successors;
      bool endBlock = isLastInstruction || nextStartsBlock;
      if (instruction.FlowKind != NativeInstructionFlowKind::None) {
        if (instruction.FlowKind ==
            NativeInstructionFlowKind::ConditionalBranch) {
          successors = instruction.DirectFlowTargets;
          if (instruction.Fallthrough &&
              isInstructionFallthroughTo(instruction,
                                         *instruction.Fallthrough)) {
            addUniqueAddress(successors, *instruction.Fallthrough);
          }
        } else if (instruction.FlowKind ==
                   NativeInstructionFlowKind::UnconditionalBranch) {
          successors = instruction.DirectFlowTargets;
        } else if (instruction.FlowKind ==
                   NativeInstructionFlowKind::IndirectBranch) {
          successors = instruction.DirectFlowTargets;
        } else if (instruction.FlowKind == NativeInstructionFlowKind::Trap) {
          successors.clear();
        }
        endBlock = true;
      }
      if (nextStartsBlock && successors.empty() &&
          instruction.FlowKind == NativeInstructionFlowKind::None &&
          isInstructionFallthroughTo(instruction,
                                     instructions[index + 1].Address)) {
        addUniqueAddress(successors, instructions[index + 1].Address);
      }

      if (!endBlock) {
        continue;
      }

      NativeBasicBlock block;
      block.Start = blockStart;
      block.End = std::min(instructionEnd, rangeEnd);
      block.Successors = std::move(successors);
      if (block.Start < block.End) {
        blocks.push_back(std::move(block));
      }
      if (!isLastInstruction) {
        blockStart = instructions[index + 1].Address;
      }
    }
    return blocks;
  }

  static void
  eraseOutOfRangeFunctionSuccessors(const NativeProgramState &state,
                                    uint64_t entry,
                                    std::vector<uint64_t> &successors) {
    successors.erase(
        std::remove_if(successors.begin(), successors.end(),
                       [&](uint64_t successor) {
                         return !targetBelongsToFunctionRange(state, entry,
                                                              successor);
                       }),
        successors.end());
  }

  static void
  eraseKnownOtherFunctionSuccessors(NativeProgramState &state, uint64_t entry,
                                    std::vector<uint64_t> &successors) {
    successors.erase(
        std::remove_if(successors.begin(), successors.end(),
                       [&](uint64_t successor) {
                         return isKnownOtherFunctionEntry(state, entry,
                                                          successor);
                       }),
        successors.end());
  }

  static bool isKnownOtherFunctionEntry(const NativeProgramState &state,
                                        uint64_t entry, uint64_t address) {
    if (address == entry) {
      return false;
    }
    auto seedIterator = state.functionSeeds().find(address);
    return seedIterator != state.functionSeeds().end() &&
           seedIterator->second.IsEntry &&
           isBoundarySeed(seedIterator->second);
  }

  static std::optional<uint64_t>
  executableBytesFrom(const NativeProgramState &state, uint64_t address) {
    for (const NativeMemoryRange &range : state.memoryRanges()) {
      if (range.Start > address) {
        break;
      }
      if (!range.Executable ||
          !containsAddress(range.Start, range.Size, address)) {
        continue;
      }
      return range.Size - (address - range.Start);
    }
    return std::nullopt;
  }

  static bool targetBelongsToFunctionRange(const NativeProgramState &state,
                                           uint64_t functionEntry,
                                           uint64_t target) {
    auto seedIterator = state.functionSeeds().find(functionEntry);
    if (seedIterator == state.functionSeeds().end()) {
      return true;
    }

    const NativeFunctionSeed &seed = seedIterator->second;
    if (seed.RangeStart == 0 || seed.RangeEnd <= seed.RangeStart) {
      return true;
    }
    return target >= seed.RangeStart && target < seed.RangeEnd;
  }

  static uint64_t boundedBytesForFunctionSeed(const NativeProgramState &state,
                                              uint64_t functionEntry,
                                              uint64_t blockAddress,
                                              uint64_t availableBytes) {
    auto seedIterator = state.functionSeeds().find(functionEntry);
    if (seedIterator == state.functionSeeds().end()) {
      return capBytesAtNextFunctionSeed(state, blockAddress, availableBytes);
    }

    const NativeFunctionSeed &seed = seedIterator->second;
    // Known seed ranges come from stronger boundaries such as symbol sizes or
    // .eh_frame FDEs.  Use them as a decode cap, but only when the queued block
    // is actually inside the recorded half-open range.
    if (seed.RangeStart == 0 || seed.RangeEnd <= seed.RangeStart ||
        blockAddress < seed.RangeStart || blockAddress >= seed.RangeEnd) {
      return capBytesAtNextFunctionSeed(state, blockAddress, availableBytes);
    }
    return std::min(availableBytes, seed.RangeEnd - blockAddress);
  }

  static uint64_t capBytesAtNextFunctionSeed(const NativeProgramState &state,
                                             uint64_t blockAddress,
                                             uint64_t availableBytes) {
    uint64_t cappedBytes = availableBytes;
    for (auto iterator = state.functionSeeds().upper_bound(blockAddress);
         iterator != state.functionSeeds().end(); ++iterator) {
      const auto &[address, seed] = *iterator;
      if (!isBoundarySeed(seed)) {
        continue;
      }
      uint64_t distance = address - blockAddress;
      if (distance < cappedBytes) {
        cappedBytes = distance;
      }
      break;
    }
    return cappedBytes;
  }

  static bool isBoundarySeed(const NativeFunctionSeed &seed) {
    return seed.Confidence != NativeFunctionConfidence::Low;
  }
};

std::unique_ptr<NativeAnalyzer> createSleighSeedInstructionAnalyzer(
    NativeSleighDecodeOptions options) {
  return std::make_unique<SleighSeedInstructionAnalyzer>(options);
}

} // namespace notdec::bin2llvm
