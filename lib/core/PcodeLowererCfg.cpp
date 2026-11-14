#include "PcodeToLLVMInternal.h"

namespace notdec::bin2llvm {

std::string PcodeLowerer::addressFunctionName(uint64_t address) {
    std::ostringstream os;
    os << "notdec_native_" << std::hex << address;
    return os.str();
  }

std::optional<uint64_t> PcodeLowerer::directTarget(const PcodeOpView &op,
                                       size_t inputIndex) {
    if (op.Inputs.size() <= inputIndex ||
        op.Inputs[inputIndex].Space != "ram") {
      return std::nullopt;
    }
    return op.Inputs[inputIndex].Offset;
  }

std::optional<size_t> PcodeLowerer::relativeTargetIndex(size_t opIndex,
                                            const PcodeOpView &op,
                                            size_t inputIndex,
                                            size_t opCount) {
    if (op.Inputs.size() <= inputIndex ||
        op.Inputs[inputIndex].Space != "const") {
      return std::nullopt;
    }

    int64_t offset = relativeBranchOffset(op.Inputs[inputIndex]);
    if (offset < 0) {
      uint64_t magnitude = static_cast<uint64_t>(-(offset + 1)) + 1;
      if (opIndex < magnitude) {
        return std::nullopt;
      }
    }
    if (offset > 0 &&
        static_cast<uint64_t>(offset) > opCount - opIndex) {
      return std::nullopt;
    }
    size_t target =
        static_cast<size_t>(static_cast<int64_t>(opIndex) + offset);
    if (target >= opCount) {
      return std::nullopt;
    }
    return target;
  }

int64_t PcodeLowerer::relativeBranchOffset(const VarnodeView &target) {
    uint32_t bits = bitWidth(target.Size);
    if (bits >= 64) {
      return static_cast<int64_t>(target.Offset);
    }
    uint64_t signBit = uint64_t{1} << (bits - 1);
    uint64_t mask = (uint64_t{1} << bits) - 1;
    uint64_t value = target.Offset & mask;
    if ((value & signBit) == 0) {
      return static_cast<int64_t>(value);
    }
    return -static_cast<int64_t>((~value & mask) + 1);
  }

void PcodeLowerer::addBlockStart(std::set<size_t> &starts,
                     const std::map<uint64_t, size_t> &firstOpForAddress,
                     uint64_t address) {
    auto it = firstOpForAddress.find(address);
    if (it != firstOpForAddress.end()) {
      starts.insert(it->second);
    }
  }

void PcodeLowerer::addNativeBlockStart(std::set<size_t> &starts, size_t start,
                           uint64_t blockAddress, uint64_t blockEnd,
                           bool internalPcodeBlock ) {
    // A p-code branch can target the start of its own machine block, for
    // example a tight loop ending in `jae block_start`.  That is still a real
    // native block, not an instruction-internal p-code split.  Keep the first
    // native mapping so conditional lowering will consult native CFG facts for
    // the false edge.
    if (internalPcodeBlock && starts.count(start) != 0 &&
        NativeInternalPcodeStarts.count(start) == 0) {
      return;
    }
    starts.insert(start);
    NativeBlockAddressForStart[start] =
        internalPcodeBlock ? (*CurrentProgramOps)[start].Address : blockAddress;
    NativeParentBlockAddressForStart[start] = blockAddress;
    NativeBlockEndForStart[start] = blockEnd;
    if (internalPcodeBlock) {
      NativeInternalPcodeStarts.insert(start);
    }
  }

bool PcodeLowerer::buildBasicBlocks(const PcodeProgram &program,
                        std::string &errorMessage) {
    std::map<uint64_t, size_t> firstOpForAddress;
    for (size_t index = 0; index < program.Ops.size(); ++index) {
      firstOpForAddress.try_emplace(program.Ops[index].Address, index);
    }

    std::set<size_t> starts;
    // 排序一次：覆盖率检查会对每个 p-code op 遍历一遍 block ranges，
    // 之前每次遍历都重新构造并排序 vector（main 这类函数上是 O(ops*blocks log blocks)）。
    const std::vector<std::pair<uint64_t, uint64_t>> nativeRanges =
        usesNativeCfg() ? sortedNativeRanges()
                        : std::vector<std::pair<uint64_t, uint64_t>>();
    if (usesNativeCfg()) {
      for (const auto &[blockAddress, blockEnd] : nativeRanges) {
        if (blockEnd <= blockAddress) {
          std::ostringstream os;
          os << "native block 0x" << std::hex << blockAddress
             << " has an invalid block range ending at 0x" << blockEnd;
          errorMessage = os.str();
          return false;
        }
        bool foundPcodeInBlock = false;
        for (size_t index = 0; index < program.Ops.size(); ++index) {
          uint64_t opAddress = program.Ops[index].Address;
          if (opAddress >= blockAddress && opAddress < blockEnd) {
            addNativeBlockStart(starts, index, blockAddress, blockEnd);
            foundPcodeInBlock = true;
            break;
          }
        }
        if (!foundPcodeInBlock) {
          EmptyNativeBlockAddresses.push_back(blockAddress);
        }
      }
      if (starts.empty() && EmptyNativeBlockAddresses.empty()) {
        errorMessage = "native block ranges do not cover any p-code op";
        return false;
      }
      for (size_t index = 0; index < program.Ops.size(); ++index) {
        const PcodeOpView &op = program.Ops[index];
        if (!isTerminator(op.Opcode)) {
          continue;
        }
        auto range = nativeRangeForAddress(op.Address);
        if (!range) {
          continue;
        }
        auto [blockAddress, blockEnd] = *range;
        if (index + 1 < program.Ops.size() &&
            program.Ops[index + 1].Address >= blockAddress &&
            program.Ops[index + 1].Address < blockEnd) {
          addNativeBlockStart(starts, index + 1, blockAddress, blockEnd,
                              /*internalPcodeBlock=*/true);
        }
        if (op.Opcode == PcodeOpcode::Branch ||
            op.Opcode == PcodeOpcode::CBranch) {
          if (auto target = directTarget(op, 0)) {
            auto targetIt = firstOpForAddress.find(*target);
            if (targetIt != firstOpForAddress.end() &&
                *target >= blockAddress && *target < blockEnd) {
              addNativeBlockStart(starts, targetIt->second, blockAddress,
                                  blockEnd,
                                  /*internalPcodeBlock=*/true);
            }
          } else if (auto targetIndex = relativeTargetIndex(
                         index, op, 0, program.Ops.size())) {
            uint64_t targetAddress = program.Ops[*targetIndex].Address;
            if (targetAddress >= blockAddress && targetAddress < blockEnd) {
              addNativeBlockStart(starts, *targetIndex, blockAddress, blockEnd,
                                  /*internalPcodeBlock=*/true);
            }
          }
        }
      }
    }
    if (!usesNativeCfg()) {
      starts.insert(0);
      for (size_t index = 0; index < program.Ops.size(); ++index) {
        const PcodeOpView &op = program.Ops[index];
        if (op.Opcode == PcodeOpcode::Branch ||
            op.Opcode == PcodeOpcode::BranchInd ||
            op.Opcode == PcodeOpcode::CBranch) {
          if (auto target = directTarget(op, 0)) {
            addBlockStart(starts, firstOpForAddress, *target);
          } else if (auto targetIndex = relativeTargetIndex(
                         index, op, 0, program.Ops.size())) {
            starts.insert(*targetIndex);
          }
        }

        if (isTerminator(op.Opcode) && index + 1 < program.Ops.size()) {
          starts.insert(index + 1);
        }
      }
    }
    if (usesNativeCfg()) {
      for (const PcodeOpView &op : program.Ops) {
        bool covered = false;
        for (const auto &[blockAddress, blockEnd] : nativeRanges) {
          if (op.Address >= blockAddress && op.Address < blockEnd) {
            covered = true;
            break;
          }
        }
        if (!covered) {
          std::ostringstream os;
          os << "p-code op at 0x" << std::hex << op.Address
             << " is outside native block ranges";
          errorMessage = os.str();
          return false;
        }
      }
    }

    BlockStarts.assign(starts.begin(), starts.end());
    for (size_t index = 0; index < BlockStarts.size(); ++index) {
      size_t start = BlockStarts[index];
      uint64_t address = blockAddressForStart(start);
      auto end = blockEndForStart(program, index, errorMessage);
      if (!end) {
        return false;
      }
      BlockEnds.push_back(*end);
      llvm::BasicBlock *block =
          llvm::BasicBlock::Create(Context, blockName(address), &Function);
      BlockForStart[start] = block;
      BlockForAddress.try_emplace(address, block);
    }
    for (uint64_t blockAddress : EmptyNativeBlockAddresses) {
      llvm::BasicBlock *block =
          llvm::BasicBlock::Create(Context, blockName(blockAddress), &Function);
      BlockForAddress.try_emplace(blockAddress, block);
    }
    return true;
  }

std::optional<size_t> PcodeLowerer::blockEndForStart(const PcodeProgram &program,
                                         size_t blockIndex,
                                         std::string &errorMessage) {
    size_t start = BlockStarts[blockIndex];
    if (!usesNativeCfg()) {
      return blockIndex + 1 < BlockStarts.size() ? BlockStarts[blockIndex + 1]
                                                 : program.Ops.size();
    }

    auto rangeIt = NativeBlockEndForStart.find(start);
    if (rangeIt == NativeBlockEndForStart.end()) {
      std::ostringstream os;
      os << "native block 0x" << std::hex << blockAddressForStart(start)
         << " is missing a block range";
      errorMessage = os.str();
      return std::nullopt;
    }

    uint64_t blockEndAddress = rangeIt->second;
    uint64_t blockStartAddress = parentBlockAddressForIndex(blockIndex);
    if (blockIndex + 1 < BlockStarts.size()) {
      size_t nextStart = BlockStarts[blockIndex + 1];
      auto nextRangeIt = NativeBlockEndForStart.find(nextStart);
      if (nextRangeIt != NativeBlockEndForStart.end() &&
          nextRangeIt->second == blockEndAddress &&
          (*CurrentProgramOps)[nextStart].Address < blockEndAddress) {
        return nextStart;
      }
    }
    size_t end = start;
    while (end < program.Ops.size() &&
           program.Ops[end].Address >= blockStartAddress &&
           program.Ops[end].Address < blockEndAddress) {
      ++end;
    }
    return end;
  }

uint64_t PcodeLowerer::blockAddressForStart(size_t start) const {
    auto it = NativeBlockAddressForStart.find(start);
    if (it != NativeBlockAddressForStart.end()) {
      return it->second;
    }
    return (*CurrentProgramOps)[start].Address;
  }

uint64_t PcodeLowerer::blockAddressForIndex(size_t blockIndex) const {
    return blockAddressForStart(BlockStarts[blockIndex]);
  }

uint64_t PcodeLowerer::parentBlockAddressForIndex(size_t blockIndex) const {
    size_t start = BlockStarts[blockIndex];
    auto it = NativeParentBlockAddressForStart.find(start);
    if (it != NativeParentBlockAddressForStart.end()) {
      return it->second;
    }
    return blockAddressForStart(start);
  }

const std::vector<uint64_t> *
  PcodeLowerer::nativeSuccessorsForBlockIndex(size_t blockIndex,
                                uint64_t &factBlockAddress) const {
    factBlockAddress = blockAddressForIndex(blockIndex);
    auto successorIt = Config.BlockSuccessors.find(factBlockAddress);
    if (successorIt != Config.BlockSuccessors.end()) {
      return &successorIt->second;
    }

    // Native CFG facts are keyed by machine block start. P-code can split that
    // block internally for instruction semantics, so internal blocks must read
    // the parent machine block's successors.
    uint64_t parentAddress = parentBlockAddressForIndex(blockIndex);
    if (parentAddress == factBlockAddress) {
      return nullptr;
    }
    successorIt = Config.BlockSuccessors.find(parentAddress);
    if (successorIt == Config.BlockSuccessors.end()) {
      return nullptr;
    }
    factBlockAddress = parentAddress;
    return &successorIt->second;
  }

bool PcodeLowerer::isInternalPcodeBlock(size_t blockIndex) const {
    return NativeInternalPcodeStarts.count(BlockStarts[blockIndex]) != 0;
  }

llvm::BasicBlock *PcodeLowerer::nextBlock(size_t blockIndex) {
    if (blockIndex + 1 >= BlockStarts.size()) {
      return nullptr;
    }
    return BlockForStart[BlockStarts[blockIndex + 1]];
  }

llvm::BasicBlock *PcodeLowerer::internalPcodeContinuation(size_t blockIndex,
                                              size_t opIndex) {
    if (!usesNativeCfg() || opIndex + 1 >= CurrentProgramOps->size()) {
      return nullptr;
    }
    auto blockIt = BlockForStart.find(opIndex + 1);
    if (blockIt == BlockForStart.end()) {
      return nullptr;
    }
    auto rangeIt = NativeBlockEndForStart.find(BlockStarts[blockIndex]);
    if (rangeIt == NativeBlockEndForStart.end()) {
      return nullptr;
    }
    uint64_t parentAddress = parentBlockAddressForIndex(blockIndex);
    uint64_t blockEnd = rangeIt->second;
    uint64_t nextAddress = (*CurrentProgramOps)[opIndex + 1].Address;
    if (nextAddress < parentAddress || nextAddress >= blockEnd) {
      return nullptr;
    }
    return blockIt->second;
  }

bool PcodeLowerer::nativeFallthroughBlock(size_t blockIndex, llvm::BasicBlock *&result,
                              std::string &errorMessage) {
    result = nullptr;
    if (isInternalPcodeBlock(blockIndex)) {
      result = nextBlock(blockIndex);
      return true;
    }
    uint64_t blockAddress = blockAddressForIndex(blockIndex);
    auto successorIt = Config.BlockSuccessors.find(blockAddress);
    if (successorIt == Config.BlockSuccessors.end()) {
      if (usesNativeCfg()) {
        std::ostringstream os;
        os << "native block 0x" << std::hex << blockAddress
           << " is missing successor facts";
        errorMessage = os.str();
        return false;
      }
      result = nextBlock(blockIndex);
      return true;
    }
    if (successorIt->second.empty()) {
      return true;
    }
    if (successorIt->second.size() > 1) {
      std::ostringstream os;
      os << "native block 0x" << std::hex << blockAddress << " has "
         << std::dec << successorIt->second.size()
         << " successors but no p-code terminator";
      errorMessage = os.str();
      return false;
    }
    result = blockForNativeTarget(successorIt->second.front(), errorMessage);
    return result != nullptr;
  }

bool PcodeLowerer::nativeConditionalFalseBlock(size_t blockIndex, uint64_t trueTarget,
                                   llvm::BasicBlock *fallback,
                                   llvm::BasicBlock *&result,
                                   std::string &errorMessage) {
    result = nullptr;
    if (isInternalPcodeBlock(blockIndex) && fallback != nullptr) {
      result = fallback;
      return true;
    }
    uint64_t blockAddress = blockAddressForIndex(blockIndex);
    uint64_t factBlockAddress = blockAddress;
    const std::vector<uint64_t> *successors =
        nativeSuccessorsForBlockIndex(blockIndex, factBlockAddress);
    if (successors == nullptr) {
      if (usesNativeCfg()) {
        std::ostringstream os;
        os << "native conditional block 0x" << std::hex << blockAddress
           << " is missing successor facts";
        errorMessage = os.str();
        return false;
      }
      result = usesNativeCfg() ? nullptr : fallback;
      return true;
    }

    // SLEIGH CBRANCH only records the taken target.  Native discovery already
    // knows the machine-level false edge, which is safer than assuming the next
    // p-code block is the fallthrough for sparse or out-of-order ranges.
    bool sawTrueTarget = !nativeRangesCoverAddress(trueTarget);
    std::optional<uint64_t> falseTarget;
    for (uint64_t successor : *successors) {
      if (successor == trueTarget) {
        sawTrueTarget = true;
        continue;
      }
      if (falseTarget) {
        std::ostringstream os;
        os << "native conditional block 0x" << std::hex << factBlockAddress
           << " has multiple false successors";
        errorMessage = os.str();
        return false;
      }
      falseTarget = successor;
    }

    if (!sawTrueTarget) {
      std::ostringstream os;
      os << "native conditional block 0x" << std::hex << factBlockAddress
         << " is missing true successor 0x" << trueTarget;
      errorMessage = os.str();
      return false;
    }
    if (falseTarget) {
      result = blockForNativeTarget(*falseTarget, errorMessage);
      return result != nullptr;
    }
    if (usesNativeCfg()) {
      std::ostringstream os;
      os << "native conditional block 0x" << std::hex << blockAddress
         << " is missing false successor for true target 0x" << trueTarget;
      errorMessage = os.str();
      return false;
    }
    result = fallback;
    return true;
  }

bool PcodeLowerer::nativeEmptyBlockSuccessor(uint64_t blockAddress,
                                 llvm::BasicBlock *&result,
                                 std::string &errorMessage) {
    result = nullptr;
    auto successorIt = Config.BlockSuccessors.find(blockAddress);
    if (successorIt == Config.BlockSuccessors.end()) {
      if (usesNativeCfg()) {
        std::ostringstream os;
        os << "empty native block 0x" << std::hex << blockAddress
           << " is missing successor facts";
        errorMessage = os.str();
        return false;
      }
      return true;
    }
    if (successorIt->second.empty()) {
      return true;
    }
    if (successorIt->second.size() != 1) {
      std::ostringstream os;
      os << "empty native block 0x" << std::hex << blockAddress
         << " has multiple successors";
      errorMessage = os.str();
      return false;
    }
    result = blockForNativeTarget(successorIt->second.front(), errorMessage);
    return result != nullptr;
  }

bool PcodeLowerer::nativeIndirectBranchSuccessor(
      size_t blockIndex,
      std::vector<std::pair<uint64_t, llvm::BasicBlock *>> &result,
      std::string &errorMessage) {
    result.clear();
    if (!usesNativeCfg()) {
      return true;
    }

    uint64_t blockAddress = blockAddressForIndex(blockIndex);
    uint64_t factBlockAddress = blockAddress;
    const std::vector<uint64_t> *successors =
        nativeSuccessorsForBlockIndex(blockIndex, factBlockAddress);
    if (successors == nullptr) {
      std::ostringstream os;
      os << "native indirect branch block 0x" << std::hex << blockAddress
         << " is missing successor facts";
      errorMessage = os.str();
      return false;
    }
    if (successors->empty()) {
      return true;
    }
    for (uint64_t successor : *successors) {
      llvm::BasicBlock *block = blockForNativeTarget(successor, errorMessage);
      if (block == nullptr) {
        return false;
      }
      result.push_back({successor, block});
    }
    return true;
  }

llvm::BasicBlock *PcodeLowerer::blockForTarget(uint64_t address) {
    auto it = BlockForAddress.find(address);
    if (it != BlockForAddress.end()) {
      return it->second;
    }

    auto *block =
        llvm::BasicBlock::Create(Context, blockName(address), &Function);
    BlockForAddress[address] = block;
    ExternalTargetBlocks.push_back(block);
    return block;
  }

llvm::BasicBlock *PcodeLowerer::blockForNativeTarget(uint64_t address,
                                         std::string &errorMessage) {
    auto it = BlockForAddress.find(address);
    if (it != BlockForAddress.end()) {
      return it->second;
    }

    std::ostringstream os;
    os << "native branch target 0x" << std::hex << address
       << " is missing a native block";
    errorMessage = os.str();
    return nullptr;
  }

bool PcodeLowerer::isInternalPcodeTarget(size_t blockIndex, llvm::BasicBlock *targetBlock) {
    if (targetBlock == nullptr || !usesNativeCfg()) {
      return false;
    }
    for (size_t start : NativeInternalPcodeStarts) {
      auto blockIt = BlockForStart.find(start);
      if (blockIt == BlockForStart.end() || blockIt->second != targetBlock) {
        continue;
      }
      auto parentIt = NativeParentBlockAddressForStart.find(start);
      return parentIt != NativeParentBlockAddressForStart.end() &&
             parentIt->second == parentBlockAddressForIndex(blockIndex);
    }
    return false;
  }

bool PcodeLowerer::nativeDirectBranchTarget(size_t blockIndex, uint64_t target,
                                llvm::BasicBlock *targetBlock,
                                std::string &errorMessage) {
    if (!usesNativeCfg() || isInternalPcodeTarget(blockIndex, targetBlock)) {
      return true;
    }
    auto targetBlockIt = BlockForAddress.find(target);
    if (targetBlockIt == BlockForAddress.end() ||
        targetBlockIt->second != targetBlock) {
      std::ostringstream os;
      os << "native branch target 0x" << std::hex << target
         << " is missing a native block";
      errorMessage = os.str();
      return false;
    }
    uint64_t blockAddress = blockAddressForIndex(blockIndex);
    uint64_t factBlockAddress = blockAddress;
    const std::vector<uint64_t> *successors =
        nativeSuccessorsForBlockIndex(blockIndex, factBlockAddress);
    if (successors == nullptr) {
      std::ostringstream os;
      os << "native direct branch block 0x" << std::hex << blockAddress
         << " is missing successor facts";
      errorMessage = os.str();
      return false;
    }
    if (std::find(successors->begin(), successors->end(), target) !=
        successors->end()) {
      return true;
    }
    std::ostringstream os;
    os << "native direct branch block 0x" << std::hex << factBlockAddress
       << " is missing successor 0x" << target;
    errorMessage = os.str();
    return false;
  }

bool PcodeLowerer::nativeConditionalTrueTarget(size_t blockIndex, uint64_t target,
                                   llvm::BasicBlock *targetBlock,
                                   std::string &errorMessage) {
    if (!usesNativeCfg() || !nativeRangesCoverAddress(target)) {
      return true;
    }
    return nativeDirectBranchTarget(blockIndex, target, targetBlock,
                                    errorMessage);
  }

llvm::BasicBlock *
  PcodeLowerer::tailJumpBlockForKnownFunction(uint64_t address,
                                const std::string &calleeName) {
    auto it = TailJumpBlockForAddress.find(address);
    if (it != TailJumpBlockForAddress.end()) {
      return it->second;
    }

    llvm::BasicBlock *currentBlock = Builder.GetInsertBlock();
    auto *block =
        llvm::BasicBlock::Create(Context, "tail_" + blockName(address),
                                 &Function);
    TailJumpBlockForAddress[address] = block;

    Builder.SetInsertPoint(block);
    lowerKnownVoidTailJump(calleeName);
    Builder.SetInsertPoint(currentBlock);
    return block;
  }

llvm::BasicBlock *PcodeLowerer::blockForRelativeTarget(size_t opIndex,
                                           const PcodeOpView &op,
                                           size_t inputIndex,
                                           std::string &errorMessage) {
    auto targetIndex = relativeTargetIndex(opIndex, op, inputIndex,
                                           CurrentProgramOps->size());
    if (!targetIndex) {
      return nullptr;
    }
    auto it = BlockForStart.find(*targetIndex);
    if (it != BlockForStart.end()) {
      if (usesNativeCfg()) {
        uint64_t targetAddress = (*CurrentProgramOps)[*targetIndex].Address;
        uint64_t targetBlockAddress = blockAddressForStart(*targetIndex);
        if (targetBlockAddress != targetAddress) {
          std::ostringstream os;
          os << "native relative branch target 0x" << std::hex
             << targetAddress << " is missing a native block";
          errorMessage = os.str();
          return nullptr;
        }
      }
      return it->second;
    }
    if (usesNativeCfg()) {
      std::ostringstream os;
      os << "native relative branch target 0x" << std::hex
         << (*CurrentProgramOps)[*targetIndex].Address
         << " is missing a native block";
      errorMessage = os.str();
      return nullptr;
    }
    return blockForTarget((*CurrentProgramOps)[*targetIndex].Address);
  }

llvm::BasicBlock *PcodeLowerer::exitBlock() {
    if (!ExitBlock) {
      ExitBlock = llvm::BasicBlock::Create(Context, "notdec_exit", &Function);
      ExternalTargetBlocks.push_back(ExitBlock);
    }
    return ExitBlock;
  }

llvm::BasicBlock *PcodeLowerer::nativeIndirectSwitchDefaultBlock(uint64_t blockAddress) {
    // Native successor facts are authoritative here; a missing switch case is
    // a bad fact or unsupported dynamic path, not a normal function return.
    std::ostringstream name;
    name << "native_indirect_default_" << std::hex << blockAddress;
    llvm::BasicBlock *block =
        llvm::BasicBlock::Create(Context, name.str(), &Function);
    llvm::IRBuilder<> trapBuilder(block);
    llvm::Function *trap = llvm::Intrinsic::getOrInsertDeclaration(
        &Module, llvm::Intrinsic::trap);
    trapBuilder.CreateCall(trap);
    trapBuilder.CreateUnreachable();
    return block;
  }

llvm::Value *PcodeLowerer::asCondition(llvm::Value *value) {
    if (value->getType()->isIntegerTy(1)) {
      return value;
    }
    return Builder.CreateICmpNE(value,
                                llvm::ConstantInt::get(value->getType(), 0));
  }

bool PcodeLowerer::lowerTerminator(size_t blockIndex, size_t opIndex, const PcodeOpView &op,
                       llvm::BasicBlock *fallthrough,
                       std::string &errorMessage) {
    switch (op.Opcode) {
    case PcodeOpcode::Branch: {
      if (!requireInputCount(op, 1, errorMessage)) {
        return false;
      }
      auto target = directTarget(op, 0);
      if (target) {
        auto blockIt = BlockForAddress.find(*target);
        if (blockIt == BlockForAddress.end()) {
          auto externalIt = Config.ExternalCallTargets.find(*target);
          if (externalIt != Config.ExternalCallTargets.end()) {
            return lowerKnownVoidTailJump(externalIt->second);
          }
          auto directIt = Config.DirectCallTargets.find(*target);
          if (directIt != Config.DirectCallTargets.end()) {
            return lowerKnownVoidTailJump(directIt->second);
          }
          if (usesNativeCfg()) {
            if (!nativeRangesCoverAddress(*target)) {
              return lowerKnownVoidTailJump(addressFunctionName(*target));
            }
            (void)blockForNativeTarget(*target, errorMessage);
            return false;
          }
        }
        llvm::BasicBlock *targetBlock =
            blockIt != BlockForAddress.end() ? blockIt->second
                                             : blockForTarget(*target);
        if (!nativeDirectBranchTarget(blockIndex, *target, targetBlock,
                                      errorMessage)) {
          return false;
        }
        Builder.CreateBr(targetBlock);
        return true;
      }
      auto relativeIndex =
          relativeTargetIndex(opIndex, op, 0, CurrentProgramOps->size());
      llvm::BasicBlock *relativeTarget =
          relativeIndex ? blockForRelativeTarget(opIndex, op, 0, errorMessage)
                        : nullptr;
      if (relativeTarget == nullptr || !relativeIndex) {
        if (errorMessage.empty()) {
          errorMessage = "BRANCH target must be direct ram or relative const";
        }
        return false;
      }
      uint64_t relativeAddress = (*CurrentProgramOps)[*relativeIndex].Address;
      if (!nativeDirectBranchTarget(blockIndex, relativeAddress, relativeTarget,
                                    errorMessage)) {
        return false;
      }
      Builder.CreateBr(relativeTarget);
      return true;
    }

    case PcodeOpcode::CBranch: {
      if (!requireInputCount(op, 2, errorMessage)) {
        return false;
      }
      auto target = directTarget(op, 0);
      llvm::BasicBlock *trueBlock = nullptr;
      std::optional<uint64_t> trueAddress;
      if (target) {
        trueAddress = *target;
        if (usesNativeCfg()) {
          if (auto blockIt = BlockForAddress.find(*target);
              blockIt != BlockForAddress.end()) {
            trueBlock = blockIt->second;
          } else if (auto externalIt = Config.ExternalCallTargets.find(*target);
                     externalIt != Config.ExternalCallTargets.end()) {
            trueBlock =
                tailJumpBlockForKnownFunction(*target, externalIt->second);
          } else if (auto directIt = Config.DirectCallTargets.find(*target);
                     directIt != Config.DirectCallTargets.end()) {
            trueBlock =
                tailJumpBlockForKnownFunction(*target, directIt->second);
          } else if (!nativeRangesCoverAddress(*target)) {
            trueBlock = tailJumpBlockForKnownFunction(
                *target, addressFunctionName(*target));
          } else {
            trueBlock = blockForNativeTarget(*target, errorMessage);
          }
        } else {
          trueBlock = blockForTarget(*target);
        }
        if (trueBlock == nullptr) {
          return false;
        }
      } else {
        auto targetIndex = relativeTargetIndex(opIndex, op, 0,
                                               CurrentProgramOps->size());
        if (targetIndex) {
          trueAddress = (*CurrentProgramOps)[*targetIndex].Address;
          trueBlock =
              blockForRelativeTarget(opIndex, op, 0, errorMessage);
        }
      }
      if (trueBlock == nullptr) {
        if (errorMessage.empty()) {
          errorMessage = "CBRANCH target must be direct ram or relative const";
        }
        return false;
      }
      llvm::BasicBlock *falseBlock = fallthrough;
      if (trueAddress) {
        if (!nativeConditionalTrueTarget(blockIndex, *trueAddress, trueBlock,
                                         errorMessage)) {
          return false;
        }
        if (llvm::BasicBlock *internalContinuation =
                internalPcodeContinuation(blockIndex, opIndex)) {
          falseBlock = internalContinuation;
        } else {
          llvm::BasicBlock *nativeFalseBlock = nullptr;
          if (!nativeConditionalFalseBlock(blockIndex, *trueAddress, falseBlock,
                                           nativeFalseBlock, errorMessage)) {
            return false;
          }
          falseBlock = nativeFalseBlock;
        }
      }
      if (falseBlock == nullptr) {
        if (usesNativeCfg()) {
          std::ostringstream os;
          os << "native conditional block 0x" << std::hex
             << blockAddressForIndex(blockIndex)
             << " is missing false successor";
          if (trueAddress) {
            os << " for true target 0x" << *trueAddress;
          }
          errorMessage = os.str();
          return false;
        }
        falseBlock = exitBlock();
      }
      Builder.CreateCondBr(asCondition(read(op.Inputs[1])), trueBlock,
                           falseBlock);
      return true;
    }

    case PcodeOpcode::BranchInd:
      if (!requireInputCount(op, 1, errorMessage)) {
        return false;
      }
      if (auto gotAddress = sourceRam(op.Inputs[0])) {
        auto it = Config.IndirectExternalCallTargets.find(*gotAddress);
        if (it != Config.IndirectExternalCallTargets.end()) {
          return lowerKnownVoidTailJump(it->second);
        }
      }
      if (usesNativeCfg()) {
        std::vector<std::pair<uint64_t, llvm::BasicBlock *>> successors;
        if (!nativeIndirectBranchSuccessor(blockIndex, successors,
                                           errorMessage)) {
          return false;
        }
        if (successors.size() == 1) {
          Builder.CreateBr(successors.front().second);
          return true;
        }
        if (successors.empty()) {
          return lowerUnknownVoidIndirectTailJump(op.Inputs[0]);
        }
        if (!successors.empty()) {
          llvm::Value *target = resize(read(op.Inputs[0]), pointerByteSize());
          auto *switchInst =
              Builder.CreateSwitch(target,
                                   nativeIndirectSwitchDefaultBlock(
                                       blockAddressForIndex(blockIndex)),
                                   successors.size());
          for (const auto &[address, successor] : successors) {
            switchInst->addCase(
                llvm::cast<llvm::ConstantInt>(
                    llvm::ConstantInt::get(target->getType(), address)),
                successor);
          }
          return true;
        }
      }
      Builder.CreateBr(exitBlock());
      return true;

    case PcodeOpcode::Return:
      if (!requireInputCount(op, 1, errorMessage)) {
        return false;
      }
      Builder.CreateRetVoid();
      return true;

    default:
      errorMessage = "not a terminator opcode: " + op.OpcodeName;
      return false;
    }
  }

} // namespace notdec::bin2llvm
