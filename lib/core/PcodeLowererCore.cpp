#include "PcodeToLLVMInternal.h"

namespace notdec::bin2llvm {

PcodeLowerer::PcodeLowerer(llvm::LLVMContext &context, llvm::Module &module,
               llvm::Function &function, llvm::IRBuilder<> &builder,
               const PcodeLoweringConfig &config)
      : Context(context), Module(module), Function(function), Builder(builder),
        Config(config) {
  }

bool PcodeLowerer::lower(const PcodeProgram &program, std::string &errorMessage) {
    std::vector<RegisterInfo> windowRegisters =
        x87WindowRegisters(program.Registers);
    Registers = std::make_unique<RegisterStorage>(Context, Module,
                                                  windowRegisters,
                                                  program.IsBigEndian);
    StVarnodes[0] = x87StackVarnode(windowRegisters, 0);
    StVarnodes[1] = x87StackVarnode(windowRegisters, 1);

    if (program.Ops.empty() && !usesNativeCfg()) {
      return true;
    }

    CurrentProgramOps = &program.Ops;
    CurrentProgram = &program;
    prepareX86CallReturnStackSuppression(program);
    prepareX86PcThunkSuppression(program);
    prepareSseFloatWriteMarkers(program);
    prepareX87Window(program);
    if (!buildBasicBlocks(program, errorMessage)) {
      return false;
    }
    llvm::BasicBlock *entryBlock = entryBlockForProgram(errorMessage);
    if (entryBlock == nullptr) {
      return false;
    }
    // x87 入口深度 0（x86-64 SysV：long double 参数走栈传递，见
    // NativeRegisterElimination 的 isX87StackRegisterName）。整函数深度都能静态
    // 证明时启用精确窗口模型；证不出来就退回旧模型（入口空槽读已由 poison
    // 规则兜住，不会再产生 unknown）。
    if (pointerByteSize() == 8) {
      PreciseX87Depth = computeX87BlockDepths(program, *entryBlock);
    }
    Builder.CreateBr(entryBlock);

    for (size_t blockIndex = 0; blockIndex < BlockStarts.size(); ++blockIndex) {
      size_t start = BlockStarts[blockIndex];
      size_t end = BlockEnds[blockIndex];
      Builder.SetInsertPoint(BlockForStart[start]);
      CurrentNativeBlockAddress = blockAddressForIndex(blockIndex);
      X87Depth.reset();
      if (PreciseX87Depth) {
        auto depthIt = X87BlockEntryDepth.find(start);
        if (depthIt != X87BlockEntryDepth.end()) {
          X87Depth = depthIt->second;
        }
      }
      // Native mode may split one machine block into several LLVM blocks for
      // instruction-internal p-code control flow, e.g. CMOV. These internal
      // blocks still belong to the same native block, so unique temporaries
      // computed before the split must remain visible.
      if (!isInternalPcodeBlock(blockIndex)) {
        Values.clear();
      }
      llvm::BasicBlock *nativeFallback = usesNativeCfg() ? nullptr
                                                         : nextBlock(blockIndex);

      bool ended = false;
      for (size_t opIndex = start; opIndex < end; ++opIndex) {
        CurrentOpIsFloatWrite = FloatWriteOpIndices.count(opIndex) != 0;
        auto x87It = X87Groups.find(opIndex);
        if (x87It != X87Groups.end()) {
          if (!lowerX87Window(x87It->second, errorMessage)) {
            return false;
          }
          opIndex = x87It->second.End - 1;
          continue;
        }
        if (SuppressedPcodeOpIndices.count(opIndex) != 0) {
          auto baseIt = ThunkBaseWrites.find(opIndex);
          if (baseIt != ThunkBaseWrites.end()) {
            llvm::Value *value = llvm::ConstantInt::get(
                intType(baseIt->second.first.Size), baseIt->second.second);
            write(baseIt->second.first, value);
          }
          continue;
        }

        const PcodeOpView &op = program.Ops[opIndex];
        if (isTerminator(op.Opcode)) {
          if (!lowerTerminator(blockIndex, opIndex, op, nativeFallback,
                               errorMessage)) {
            return false;
          }
          ended = true;
          break;
        }

        if (!lowerOp(op, errorMessage)) {
          return false;
        }
      }

      if (!ended) {
        llvm::BasicBlock *next = nullptr;
        if (!nativeFallthroughBlock(blockIndex, next, errorMessage)) {
          return false;
        }
        if (next != nullptr) {
          Builder.CreateBr(next);
        } else {
          Builder.CreateRetVoid();
        }
      }
    }

    for (uint64_t blockAddress : EmptyNativeBlockAddresses) {
      auto blockIt = BlockForAddress.find(blockAddress);
      if (blockIt == BlockForAddress.end()) {
        errorMessage = "empty native block is missing an LLVM block";
        return false;
      }
      Builder.SetInsertPoint(blockIt->second);

      llvm::BasicBlock *successor = nullptr;
      if (!nativeEmptyBlockSuccessor(blockAddress, successor, errorMessage)) {
        return false;
      }
      if (successor != nullptr) {
        Builder.CreateBr(successor);
      } else {
        Builder.CreateRetVoid();
      }
    }

    for (llvm::BasicBlock *target : ExternalTargetBlocks) {
      Builder.SetInsertPoint(target);
      Builder.CreateRetVoid();
    }

    narrowMaskedRegisterReads();
    llvm::EliminateUnreachableBlocks(Function);
    return true;
  }

void PcodeLowerer::narrowMaskedRegisterReads() {
    for (const RegisterRead &entry : RegisterReads) {
      auto *readInst = llvm::dyn_cast<llvm::Instruction>(entry.Value);
      if (readInst == nullptr || readInst->getParent() == nullptr) {
        continue;
      }
      llvm::Value *value = entry.Value;
      auto *readType = llvm::dyn_cast<llvm::IntegerType>(value->getType());
      if (readType == nullptr || value->use_empty()) {
        continue;
      }
      unsigned width = readType->getBitWidth();
      if (width <= 8) {
        continue; // 已经是一个字节，没什么可收窄
      }
      llvm::APInt demanded = llvm::APInt::getZero(width);
      bool allConstantMasks = true;
      for (llvm::User *user : value->users()) {
        auto *mask = llvm::dyn_cast<llvm::BinaryOperator>(user);
        if (mask == nullptr || mask->getOpcode() != llvm::Instruction::And) {
          allConstantMasks = false;
          break;
        }
        auto *lhs = llvm::dyn_cast<llvm::ConstantInt>(mask->getOperand(0));
        auto *rhs = llvm::dyn_cast<llvm::ConstantInt>(mask->getOperand(1));
        llvm::ConstantInt *constant = lhs != nullptr ? lhs : rhs;
        if (constant == nullptr || constant->getType() != readType) {
          allConstantMasks = false;
          break;
        }
        demanded |= constant->getValue();
      }
      if (!allConstantMasks) {
        continue;
      }
      unsigned needed = demanded.getActiveBits();
      if (needed == 0 || needed >= width) {
        continue; // 掩码要全部位（或掩码为 0），收窄没有意义
      }
      unsigned narrowBits =
          needed <= 8 ? 8 : (needed <= 16 ? 16 : (needed <= 32 ? 32 : 64));
      uint32_t narrowSize = narrowBits / 8;
      if (narrowBits >= width || narrowSize >= entry.Access.Size) {
        continue;
      }
      RegisterAccess access = entry.Access;
      access.Size = narrowSize;
      llvm::IRBuilder<> builder(readInst);
      llvm::Value *narrow = Registers->read(builder, access);
      if (narrow == nullptr ||
          narrow->getType() != llvm::IntegerType::get(Context, narrowBits)) {
        continue;
      }
      llvm::Value *extended = builder.CreateZExt(narrow, value->getType());
      value->replaceAllUsesWith(extended);
      for (auto &pair : Values) {
        if (pair.second == value) {
          pair.second = extended; // 缓存里指向旧读的条目换成新值，避免悬空
        }
      }
      readInst->eraseFromParent();
      ++MaskedRegisterReadsNarrowed;
    }
    RegisterReads.clear();
  }

bool PcodeLowerer::usesNativeCfg() const {
    return !Config.BlockRanges.empty() || !Config.BlockSuccessors.empty();
  }

std::vector<std::pair<uint64_t, uint64_t>> PcodeLowerer::sortedNativeRanges() const {
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    ranges.reserve(Config.BlockRanges.size());
    for (const auto &[blockAddress, blockEnd] : Config.BlockRanges) {
      ranges.push_back({blockAddress, blockEnd});
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const auto &lhs, const auto &rhs) {
                return lhs.first < rhs.first;
              });
    return ranges;
  }

bool PcodeLowerer::nativeRangesCoverAddress(uint64_t address) const {
    for (const auto &[blockAddress, blockEnd] : Config.BlockRanges) {
      if (address >= blockAddress && address < blockEnd) {
        return true;
      }
    }
    return false;
  }

std::optional<std::pair<uint64_t, uint64_t>>
  PcodeLowerer::nativeRangeForAddress(uint64_t address) const {
    for (const auto &[blockAddress, blockEnd] : Config.BlockRanges) {
      if (address >= blockAddress && address < blockEnd) {
        return std::make_pair(blockAddress, blockEnd);
      }
    }
    return std::nullopt;
  }

llvm::BasicBlock *PcodeLowerer::entryBlockForProgram(std::string &errorMessage) {
    if (!Config.EntryAddress) {
      return BlockForStart[BlockStarts.front()];
    }

    uint64_t entryAddress = *Config.EntryAddress;
    if (usesNativeCfg()) {
      for (const auto &[blockAddress, blockEnd] : sortedNativeRanges()) {
        if (entryAddress >= blockAddress && entryAddress < blockEnd) {
          auto blockIt = BlockForAddress.find(blockAddress);
          if (blockIt != BlockForAddress.end()) {
            return blockIt->second;
          }
          break;
        }
      }
    } else {
      auto blockIt = BlockForAddress.find(entryAddress);
      if (blockIt != BlockForAddress.end()) {
        return blockIt->second;
      }
    }

    std::ostringstream os;
    os << "entry address 0x" << std::hex << entryAddress
       << " is not covered by lowered p-code blocks";
    errorMessage = os.str();
    return nullptr;
  }

bool PcodeLowerer::isTerminator(PcodeOpcode opcode) {
    return opcode == PcodeOpcode::Branch ||
           opcode == PcodeOpcode::BranchInd ||
           opcode == PcodeOpcode::CBranch || opcode == PcodeOpcode::Return;
  }

bool PcodeLowerer::sameVarnode(const VarnodeView &lhs, const VarnodeView &rhs) {
    return lhs.Space == rhs.Space && lhs.Offset == rhs.Offset &&
           lhs.Size == rhs.Size;
  }

bool PcodeLowerer::isConstValue(const VarnodeView &varnode, uint64_t value,
                           uint32_t size) {
    return varnode.Space == "const" && varnode.Size == size &&
           varnode.Offset == value;
  }

bool PcodeLowerer::isRegisterNamed(const VarnodeView &varnode,
                              const char *registerName, uint32_t size) {
    return varnode.IsRegister && varnode.RegisterName &&
           *varnode.RegisterName == registerName && varnode.Size == size;
  }

std::optional<X86CallStackSpec>
  PcodeLowerer::x86CallStackSpec(const PcodeProgram &program) {
    if (program.IsBigEndian) {
      return std::nullopt;
    }

    bool hasRsp = false;
    bool hasRip = false;
    bool hasEsp = false;
    bool hasEip = false;
    for (const RegisterInfo &reg : program.Registers) {
      hasRsp |= reg.Name == "RSP" && reg.Size == 8;
      hasRip |= reg.Name == "RIP" && reg.Size == 8;
      hasEsp |= reg.Name == "ESP" && reg.Size == 4;
      hasEip |= reg.Name == "EIP" && reg.Size == 4;
    }
    if (hasRsp && hasRip) {
      return X86CallStackSpec{"x64", "RSP", 8};
    }
    if (hasEsp && hasEip) {
      return X86CallStackSpec{"i386", "ESP", 4};
    }
    return std::nullopt;
  }

bool PcodeLowerer::isX86StackPointer(const VarnodeView &varnode,
                                const X86CallStackSpec &spec) {
    return isRegisterNamed(varnode, spec.StackPointerRegister,
                           spec.PointerSize);
  }

bool PcodeLowerer::isX86StackPointerAdjust(const PcodeOpView &op,
                                      PcodeOpcode opcode,
                                      const X86CallStackSpec &spec) {
    return op.Opcode == opcode && op.Output &&
           isX86StackPointer(*op.Output, spec) && op.Inputs.size() == 2 &&
           isX86StackPointer(op.Inputs[0], spec) &&
           isConstValue(op.Inputs[1], spec.PointerSize, spec.PointerSize);
  }

bool PcodeLowerer::isX86ReturnAddressStore(const PcodeOpView &op,
                                      const VarnodeView &stackPointer,
                                      uint64_t fallthrough,
                                      const X86CallStackSpec &spec) {
    return op.Opcode == PcodeOpcode::Store && op.Inputs.size() == 3 &&
           sameVarnode(op.Inputs[1], stackPointer) &&
           isConstValue(op.Inputs[2], fallthrough, spec.PointerSize);
  }

bool PcodeLowerer::isX86ReturnAddressLoad(const PcodeOpView &op,
                                     const VarnodeView &returnTarget,
                                     const X86CallStackSpec &spec) {
    return op.Opcode == PcodeOpcode::Load && op.Output &&
           sameVarnode(*op.Output, returnTarget) && op.Inputs.size() == 2 &&
           isX86StackPointer(op.Inputs[1], spec);
  }

bool PcodeLowerer::suppressX86CallStackEffect(size_t start, size_t end,
                                  const X86CallStackSpec &spec) {
    std::optional<size_t> stackSubtractIndex;
    std::optional<size_t> storeIndex;
    uint64_t fallthrough = 0;

    for (size_t index = start; index < end; ++index) {
      const PcodeOpView &op = (*CurrentProgramOps)[index];
      if (!isX86StackPointerAdjust(op, PcodeOpcode::IntSub, spec) ||
          op.InstructionSize == 0) {
        continue;
      }
      stackSubtractIndex = index;
      fallthrough = op.Address + op.InstructionSize;
      break;
    }
    if (!stackSubtractIndex) {
      return false;
    }

    const VarnodeView &stackPointer =
        *(*CurrentProgramOps)[*stackSubtractIndex].Output;
    for (size_t index = *stackSubtractIndex + 1; index < end; ++index) {
      if (isX86ReturnAddressStore((*CurrentProgramOps)[index], stackPointer,
                                  fallthrough, spec)) {
        storeIndex = index;
        break;
      }
    }
    if (!storeIndex) {
      return false;
    }

    SuppressedPcodeOpIndices.insert(*stackSubtractIndex);
    SuppressedPcodeOpIndices.insert(*storeIndex);
    return true;
  }

bool PcodeLowerer::suppressX86ReturnStackEffect(size_t start, size_t end,
                                    const X86CallStackSpec &spec) {
    std::optional<size_t> returnIndex;
    for (size_t index = start; index < end; ++index) {
      if ((*CurrentProgramOps)[index].Opcode == PcodeOpcode::Return) {
        returnIndex = index;
        break;
      }
    }
    if (!returnIndex || (*CurrentProgramOps)[*returnIndex].Inputs.size() != 1) {
      return false;
    }

    const VarnodeView &returnTarget =
        (*CurrentProgramOps)[*returnIndex].Inputs[0];
    std::optional<size_t> loadIndex;
    for (size_t index = start; index < *returnIndex; ++index) {
      if (isX86ReturnAddressLoad((*CurrentProgramOps)[index], returnTarget,
                                 spec)) {
        loadIndex = index;
        break;
      }
    }
    if (!loadIndex) {
      return false;
    }

    std::optional<size_t> rspAddIndex;
    for (size_t index = *loadIndex + 1; index < *returnIndex; ++index) {
      if (isX86StackPointerAdjust((*CurrentProgramOps)[index],
                                  PcodeOpcode::IntAdd, spec)) {
        rspAddIndex = index;
        break;
      }
    }
    if (!rspAddIndex) {
      return false;
    }

    SuppressedPcodeOpIndices.insert(*loadIndex);
    SuppressedPcodeOpIndices.insert(*rspAddIndex);
    return true;
  }

void PcodeLowerer::warnX86StackPatternMiss(uint64_t address, const char *kind,
                               const X86CallStackSpec &spec) {
    llvm::errs() << "warning: " << spec.ArchName << ' ' << kind << " at 0x";
    llvm::errs().write_hex(address);
    llvm::errs() << " did not match implicit return-address stack pattern\n";
  }

void PcodeLowerer::prepareX86CallReturnStackSuppression(const PcodeProgram &program) {
    SuppressedPcodeOpIndices.clear();
    std::optional<X86CallStackSpec> spec = x86CallStackSpec(program);
    if (!spec) {
      return;
    }

    for (size_t start = 0; start < program.Ops.size();) {
      size_t end = start + 1;
      while (end < program.Ops.size() &&
             program.Ops[end].Address == program.Ops[start].Address) {
        ++end;
      }

      bool hasCall = false;
      bool hasReturn = false;
      for (size_t index = start; index < end; ++index) {
        PcodeOpcode opcode = program.Ops[index].Opcode;
        hasCall |= opcode == PcodeOpcode::Call ||
                   opcode == PcodeOpcode::CallInd;
        hasReturn |= opcode == PcodeOpcode::Return;
      }

      if (hasCall) {
        if (!suppressX86CallStackEffect(start, end, *spec)) {
          warnX86StackPatternMiss(program.Ops[start].Address, "CALL", *spec);
        }
      }
      if (hasReturn && !suppressX86ReturnStackEffect(start, end, *spec)) {
        warnX86StackPatternMiss(program.Ops[start].Address, "RET", *spec);
      }

      start = end;
    }
  }

void PcodeLowerer::prepareX86PcThunkSuppression(const PcodeProgram &program) {
    ThunkBaseWrites.clear();
    if (Config.ThunkCallTargets.empty()) {
      return;
    }

    for (size_t start = 0; start < program.Ops.size();) {
      size_t end = start + 1;
      while (end < program.Ops.size() &&
             program.Ops[end].Address == program.Ops[start].Address) {
        ++end;
      }

      std::optional<uint64_t> callTarget;
      for (size_t index = start; index < end; ++index) {
        if (program.Ops[index].Opcode == PcodeOpcode::Call) {
          if (std::optional<uint64_t> target = directTarget(program.Ops[index], 0)) {
            callTarget = *target;
            break;
          }
        }
      }
      auto thunkIt = callTarget
                         ? Config.ThunkCallTargets.find(*callTarget)
                         : Config.ThunkCallTargets.end();
      if (thunkIt == Config.ThunkCallTargets.end()) {
        start = end;
        continue;
      }

      uint64_t fallthrough =
          program.Ops[start].Address + program.Ops[start].InstructionSize;
      std::optional<size_t> addIndex;
      std::optional<VarnodeView> baseRegister;
      uint64_t immediate = 0;
      for (size_t index = end; index < program.Ops.size(); ++index) {
        const PcodeOpView &op = program.Ops[index];
        if (isTerminator(op.Opcode) || op.Opcode == PcodeOpcode::Call ||
            op.Opcode == PcodeOpcode::CallInd) {
          break;
        }
        if (op.Opcode != PcodeOpcode::IntAdd || !op.Output ||
            !op.Output->IsRegister ||
            op.Output->RegisterName != thunkIt->second) {
          continue;
        }
        bool hasBaseInput = false;
        std::optional<uint64_t> constInput;
        for (const VarnodeView &input : op.Inputs) {
          if (input.IsRegister && input.RegisterName == thunkIt->second) {
            hasBaseInput = true;
          } else if (input.Space == "const") {
            constInput = input.Offset;
          }
        }
        if (!hasBaseInput || !constInput) {
          continue;
        }
        addIndex = index;
        baseRegister = *op.Output;
        immediate = *constInput;
        break;
      }
      if (!addIndex || !baseRegister) {
        start = end;
        continue;
      }

      for (size_t index = start; index < end; ++index) {
        SuppressedPcodeOpIndices.insert(index);
      }
      size_t addEnd = *addIndex + 1;
      while (addEnd < program.Ops.size() &&
             program.Ops[addEnd].Address == program.Ops[*addIndex].Address) {
        ++addEnd;
      }
      for (size_t index = *addIndex; index < addEnd; ++index) {
        SuppressedPcodeOpIndices.insert(index);
      }
      ThunkBaseWrites.emplace(start,
                              std::make_pair(*baseRegister,
                                             fallthrough + immediate));
      start = end;
    }
  }

const PcodeOpView *PcodeLowerer::findProducer(const PcodeProgram &program,
                                         size_t start, size_t end,
                                         const VarnodeView &target) {
    for (size_t index = start; index < end; ++index) {
      const PcodeOpView &op = program.Ops[index];
      if (op.Output && sameVarnode(*op.Output, target)) {
        return &op;
      }
    }
    return nullptr;
  }

std::optional<X87WindowSpec>
  PcodeLowerer::classifyX87Intrinsic(const PcodeProgram &program, size_t start,
                       size_t end) {
    const std::string &mnemonic = program.Ops[start].Mnemonic;
    if (!mnemonic.empty()) {
      return classifyX87ByMnemonic(program, start, end, mnemonic);
    }
    return classifyX87ByShape(program, start, end);
  }

void PcodeLowerer::prepareSseFloatWriteMarkers(const PcodeProgram &program) {
    FloatWriteOpIndices.clear();
    for (size_t start = 0; start < program.Ops.size();) {
      size_t end = start + 1;
      while (end < program.Ops.size() &&
             program.Ops[end].Address == program.Ops[start].Address) {
        ++end;
      }
      const std::string &mnemonic = program.Ops[start].Mnemonic;
      if (!mnemonic.empty() && isSseFloatWriteMnemonic(mnemonic)) {
        for (size_t index = start; index < end; ++index) {
          const PcodeOpView &op = program.Ops[index];
          if (op.Output && op.Output->IsRegister && op.Output->RegisterName &&
              llvm::StringRef(*op.Output->RegisterName).starts_with("XMM")) {
            FloatWriteOpIndices.insert(index);
          }
        }
      }
      start = end;
    }
  }

void PcodeLowerer::prepareX87Window(const PcodeProgram &program) {
    X87Groups.clear();
    for (size_t start = 0; start < program.Ops.size();) {
      size_t end = start + 1;
      while (end < program.Ops.size() &&
             program.Ops[end].Address == program.Ops[start].Address) {
        ++end;
      }
      if (std::optional<X87WindowSpec> spec =
              classifyX87Intrinsic(program, start, end)) {
        X87Groups.emplace(start, std::move(*spec));
      } else if (!program.Ops[start].Mnemonic.empty() &&
                 touchesX87Stack(program, start, end)) {
        llvm::errs() << "warning: x87 instruction "
                     << program.Ops[start].Mnemonic << " at 0x";
        llvm::errs().write_hex(program.Ops[start].Address);
        llvm::errs() << " was not lowered into the x87 window model\n";
      }
      start = end;
    }
  }

const PcodeOpView *
  PcodeLowerer::findStackWrite(const PcodeProgram &program, size_t start, size_t end,
                 PcodeOpcode opcode, int outputIndex) {
    for (size_t index = start; index < end; ++index) {
      const PcodeOpView &op = program.Ops[index];
      if (!op.Output || !isX87StackVarnode(*op.Output) ||
          op.Opcode != opcode) {
        continue;
      }
      if (outputIndex >= 0 &&
          static_cast<int>(x87StackIndex(*op.Output)) != outputIndex) {
        continue;
      }
      return &op;
    }
    return nullptr;
  }

std::optional<uint32_t>
  PcodeLowerer::stIndexFromSt0Copy(const PcodeProgram &program, size_t start, size_t end) {
    for (size_t index = start; index < end; ++index) {
      const PcodeOpView &op = program.Ops[index];
      if (op.Opcode != PcodeOpcode::Copy || op.Inputs.size() != 1 ||
          !op.Output || !isX87StackVarnode(*op.Output) ||
          !isX87StackVarnode(op.Inputs[0]) ||
          x87StackIndex(op.Inputs[0]) != 0) {
        continue;
      }
      return x87StackIndex(*op.Output);
    }
    return std::nullopt;
  }

std::optional<uint32_t>
  PcodeLowerer::stIndexFromSt0WriteCopy(const PcodeProgram &program, size_t start,
                          size_t end) {
    for (size_t index = start; index < end; ++index) {
      const PcodeOpView &op = program.Ops[index];
      if (op.Opcode != PcodeOpcode::Copy || op.Inputs.size() != 1 ||
          !op.Output || !isX87StackVarnode(*op.Output) ||
          x87StackIndex(*op.Output) != 0 ||
          !isX87StackVarnode(op.Inputs[0])) {
        continue;
      }
      return x87StackIndex(op.Inputs[0]);
    }
    return std::nullopt;
  }

VarnodeView PcodeLowerer::stIndexVarnode(uint32_t index) {
    VarnodeView varnode;
    varnode.Space = "const";
    varnode.Offset = index;
    varnode.Size = 1;
    return varnode;
  }

} // namespace notdec::bin2llvm
