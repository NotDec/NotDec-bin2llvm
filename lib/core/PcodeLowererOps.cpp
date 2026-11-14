#include "PcodeToLLVMInternal.h"

namespace notdec::bin2llvm {

llvm::Type *PcodeLowerer::intType(uint32_t byteSize) {
    return llvm::IntegerType::get(Context, bitWidth(byteSize));
  }

llvm::Type *PcodeLowerer::floatType(uint32_t byteSize) {
    if (byteSize == 4) {
      return llvm::Type::getFloatTy(Context);
    }
    if (byteSize == 8) {
      return llvm::Type::getDoubleTy(Context);
    }
    if (byteSize == 10) {
      return llvm::Type::getX86_FP80Ty(Context);
    }
    return nullptr;
  }

uint32_t PcodeLowerer::floatByteSize(llvm::Type *type) const {
    if (type->isX86_FP80Ty()) {
      return 10;
    }
    return type->isDoubleTy() ? 8 : 4;
  }

llvm::Value *PcodeLowerer::readFloatBits(const VarnodeView &varnode, llvm::Type *floatTy) {
    return Builder.CreateBitCast(resize(read(varnode), floatByteSize(floatTy)),
                                 floatTy);
  }

void PcodeLowerer::writeFloatBits(const VarnodeView &varnode, llvm::Value *value) {
    llvm::Type *bitsTy = intType(floatByteSize(value->getType()));
    write(varnode, Builder.CreateBitCast(value, bitsTy));
  }

llvm::Value *PcodeLowerer::resize(llvm::Value *value, uint32_t byteSize) {
    llvm::Type *targetType = intType(byteSize);
    if (value->getType() == targetType) {
      return value;
    }

    unsigned sourceBits = value->getType()->getIntegerBitWidth();
    unsigned targetBits = targetType->getIntegerBitWidth();
    if (sourceBits < targetBits) {
      return Builder.CreateZExt(value, targetType);
    }
    return Builder.CreateTrunc(value, targetType);
  }

uint32_t PcodeLowerer::pointerByteSize() const {
    return static_cast<uint32_t>(Module.getDataLayout().getPointerSize());
  }

llvm::Value *PcodeLowerer::read(const VarnodeView &varnode) {
    llvm::Type *type = intType(varnode.Size);
    if (varnode.Space == "const") {
      if (varnode.Size == pointerByteSize()) {
        if (llvm::Function *function = functionForCodeAddress(varnode.Offset)) {
          return Builder.CreatePtrToInt(function, type);
        }
      }
      return llvm::ConstantInt::get(
          type, llvm::APInt(bitWidth(varnode.Size), varnode.Offset));
    }

    auto it = Values.find(varnodeKey(varnode));
    if (it != Values.end()) {
      return visibleCachedValue(varnode, it->second);
    }

    if (varnode.Space == "ram") {
      auto *address =
          llvm::ConstantInt::get(intType(pointerByteSize()), varnode.Offset);
      auto *load = Builder.CreateLoad(type, memoryPointer(address),
                                      valueName(varnode) + "_in");
      load->setAlignment(llvm::Align(1));
      return load;
    }

    if (varnode.IsRegister && Registers != nullptr) {
      RegisterAccess access{varnode.Space, varnode.Offset, varnode.Size,
                            varnode.RegisterName};
      if (llvm::Value *value = Registers->read(Builder, access)) {
        if (access.Size > 1) {
          RegisterReads.push_back({value, access});
        }
        return value;
      }
    }

    return unknownValueAt(Builder, Module, type, valueName(varnode) + "_in");
  }

std::optional<uint64_t> PcodeLowerer::sourceRam(const VarnodeView &varnode) const {
    if (varnode.Space == "ram") {
      return varnode.Offset;
    }
    auto it = SourceRamByVarnode.find(varnodeKey(varnode));
    if (it == SourceRamByVarnode.end()) {
      return std::nullopt;
    }
    return it->second;
  }

llvm::Function *PcodeLowerer::functionForCodeAddress(uint64_t address) {
    auto found = Config.CodeAddressTargets.find(address);
    if (found == Config.CodeAddressTargets.end()) {
      return nullptr;
    }
    llvm::Function *function = Module.getFunction(found->second);
    if (function != nullptr) {
      return function;
    }
    auto *functionType =
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
    return llvm::Function::Create(
        functionType, llvm::GlobalValue::ExternalLinkage, found->second,
        &Module);
  }

void PcodeLowerer::setSourceRam(const VarnodeView &varnode, std::optional<uint64_t> source) {
    std::string key = varnodeKey(varnode);
    if (source) {
      SourceRamByVarnode[key] = *source;
    } else {
      SourceRamByVarnode.erase(key);
    }
  }

void PcodeLowerer::write(const VarnodeView &varnode, llvm::Value *value) {
    SourceRamByVarnode.erase(varnodeKey(varnode));
    llvm::Value *resized = resize(value, varnode.Size);
    if (varnode.IsRegister && Registers != nullptr) {
      RegisterAccess access{varnode.Space, varnode.Offset, varnode.Size,
                            varnode.RegisterName};
      if (Registers->hasRegister(access)) {
        Registers->write(Builder, access, resized, CurrentOpIsFloatWrite);
        return;
      }
    }

    // Ghidra 的 sleigh 把静态地址的内存访问建模成 ram 空间 varnode 的读写
    // （例如 `0x5a61: (ram,0x142e0,8) = COPY (register,0x0,8)`），而不是
    // CPUI_STORE/CPUI_LOAD。这里的赋值必须落成真实 store，否则所有静态地址写入
    // 只会停留在 SSA 缓存里：函数指针表、全局计数器这类写会直接从 IR 里消失。
    // 不写回缓存，读侧继续走 load，避免跨动态写/调用读到过期的缓存值。
    if (varnode.Space == "ram") {
      llvm::Value *address = llvm::ConstantInt::get(
          intType(pointerByteSize()), varnode.Offset);
      Builder.CreateStore(resized, memoryPointer(address))
          ->setAlignment(llvm::Align(1));
      return;
    }

    if (auto *instruction = llvm::dyn_cast<llvm::Instruction>(resized)) {
      instruction->setName(valueName(varnode));
    }
    Values[varnodeKey(varnode)] = resized;
  }

llvm::Value *PcodeLowerer::visibleCachedValue(const VarnodeView &varnode,
                                  llvm::Value *value) {
    llvm::Value *resized = resize(value, varnode.Size);
    auto *instruction = llvm::dyn_cast<llvm::Instruction>(resized);
    if (instruction == nullptr || instruction->getFunction() != &Function) {
      return resized;
    }

    llvm::BasicBlock *block = Builder.GetInsertBlock();
    if (block == nullptr) {
      return resized;
    }
    bool directPredDef = instructionDefBlockIsDirectPred(*instruction, *block);
    if ((!directPredDef && instructionDominatesBlock(*instruction, *block)) ||
        (directPredDef && llvm::pred_size(block) == 1)) {
      return resized;
    }
    if (llvm::pred_empty(block)) {
      return unknownValueAt(Builder, Module, resized->getType(),
                            valueName(varnode) + ".missing");
    }

    auto key = std::make_pair(varnodeKey(varnode), block);
    auto cached = NonDominatingReadValues.find(key);
    if (cached != NonDominatingReadValues.end()) {
      return cached->second;
    }

    llvm::IRBuilder<> phiBuilder(block, block->getFirstNonPHIIt());
    llvm::PHINode *phi =
        phiBuilder.CreatePHI(resized->getType(), llvm::pred_size(block),
                             valueName(varnode) + ".join");
    for (llvm::BasicBlock *pred : llvm::predecessors(block)) {
      llvm::Value *incoming = nullptr;
      if (pred == instruction->getParent()) {
        incoming = resized;
      } else if (!directPredDef && instructionDominatesBlock(*instruction,
                                                             *pred)) {
        incoming = resized;
      } else {
        incoming = unknownValueAtEnd(*pred, resized->getType(),
                                     valueName(varnode) + ".missing");
      }
      phi->addIncoming(incoming, pred);
    }
    NonDominatingReadValues.emplace(key, phi);
    return phi;
  }

bool PcodeLowerer::instructionDefBlockIsDirectPred(llvm::Instruction &instruction,
                                       llvm::BasicBlock &block) {
    for (llvm::BasicBlock *pred : llvm::predecessors(&block)) {
      if (pred == instruction.getParent()) {
        return true;
      }
    }
    return false;
  }

bool PcodeLowerer::instructionDominatesBlock(llvm::Instruction &instruction,
                                 llvm::BasicBlock &block) {
    if (instruction.getParent() == &block) {
      llvm::Instruction *terminator = block.getTerminator();
      return terminator == nullptr || instruction.comesBefore(terminator);
    }
    llvm::DominatorTree domTree(Function);
    return domTree.dominates(instruction.getParent(), &block);
  }

llvm::Value *PcodeLowerer::unknownValueAtEnd(llvm::BasicBlock &block, llvm::Type *type,
                                 llvm::Twine name) {
    llvm::Instruction *terminator = block.getTerminator();
    if (terminator == nullptr) {
      return llvm::PoisonValue::get(type);
    }
    llvm::IRBuilder<> builder(terminator);
    return unknownValueAt(builder, Module, type, name);
  }

bool PcodeLowerer::requireInputCount(const PcodeOpView &op, size_t count,
                         std::string &errorMessage) {
    if (op.Inputs.size() == count) {
      return true;
    }

    std::ostringstream os;
    os << op.OpcodeName << " needs " << count << " inputs, got "
       << op.Inputs.size();
    errorMessage = os.str();
    return false;
  }

bool PcodeLowerer::lowerBinary(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }

    if (op.Opcode == PcodeOpcode::IntXor &&
        sameVarnode(op.Inputs[0], op.Inputs[1])) {
      write(*op.Output, llvm::ConstantInt::get(intType(op.Output->Size), 0));
      return true;
    }

    llvm::Value *lhs = resize(read(op.Inputs[0]), op.Output->Size);
    llvm::Value *rhs = resize(read(op.Inputs[1]), op.Output->Size);
    llvm::Value *result = nullptr;

    switch (op.Opcode) {
    case PcodeOpcode::IntAdd:
      result = Builder.CreateAdd(lhs, rhs);
      break;
    case PcodeOpcode::IntSub:
      result = Builder.CreateSub(lhs, rhs);
      break;
    case PcodeOpcode::IntMult:
      result = Builder.CreateMul(lhs, rhs);
      break;
    case PcodeOpcode::IntDiv:
      result = Builder.CreateUDiv(lhs, rhs);
      break;
    case PcodeOpcode::IntSDiv:
      result = Builder.CreateSDiv(lhs, rhs);
      break;
    case PcodeOpcode::IntRem:
      result = Builder.CreateURem(lhs, rhs);
      break;
    case PcodeOpcode::IntSRem:
      result = Builder.CreateSRem(lhs, rhs);
      break;
    case PcodeOpcode::IntAnd:
      result = Builder.CreateAnd(lhs, rhs);
      break;
    case PcodeOpcode::IntOr:
      result = Builder.CreateOr(lhs, rhs);
      break;
    case PcodeOpcode::IntXor:
      result = Builder.CreateXor(lhs, rhs);
      break;
    case PcodeOpcode::IntLeft:
      result = Builder.CreateShl(lhs, rhs);
      break;
    case PcodeOpcode::IntRight:
      result = Builder.CreateLShr(lhs, rhs);
      break;
    case PcodeOpcode::IntSRight:
      result = Builder.CreateAShr(lhs, rhs);
      break;
    default:
      return false;
    }

    write(*op.Output, result);
    return true;
  }

bool PcodeLowerer::lowerCompare(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }

    uint32_t inputSize = op.Inputs[0].Size;
    llvm::Value *lhs = resize(read(op.Inputs[0]), inputSize);
    llvm::Value *rhs = resize(read(op.Inputs[1]), inputSize);
    llvm::Value *result = nullptr;

    switch (op.Opcode) {
    case PcodeOpcode::IntEqual:
      result = Builder.CreateICmpEQ(lhs, rhs);
      break;
    case PcodeOpcode::IntNotEqual:
      result = Builder.CreateICmpNE(lhs, rhs);
      break;
    case PcodeOpcode::IntLess:
      result = Builder.CreateICmpULT(lhs, rhs);
      break;
    case PcodeOpcode::IntLessEqual:
      result = Builder.CreateICmpULE(lhs, rhs);
      break;
    case PcodeOpcode::IntSLess:
      result = Builder.CreateICmpSLT(lhs, rhs);
      break;
    case PcodeOpcode::IntSLessEqual:
      result = Builder.CreateICmpSLE(lhs, rhs);
      break;
    default:
      return false;
    }

    write(*op.Output, result);
    return true;
  }

bool PcodeLowerer::lowerCast(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
      return false;
    }

    llvm::Value *input = read(op.Inputs[0]);
    llvm::Type *outputType = intType(op.Output->Size);
    llvm::Value *result = nullptr;
    if (op.Opcode == PcodeOpcode::IntZExt) {
      result = Builder.CreateZExt(input, outputType);
    } else {
      result = Builder.CreateSExt(input, outputType);
    }
    write(*op.Output, result);
    return true;
  }

bool PcodeLowerer::lowerUnary(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
      return false;
    }

    llvm::Value *input = resize(read(op.Inputs[0]), op.Output->Size);
    llvm::Value *result = nullptr;
    if (op.Opcode == PcodeOpcode::IntNegate) {
      result = Builder.CreateNot(input);
    } else if (op.Opcode == PcodeOpcode::Int2Comp) {
      result = Builder.CreateNeg(input);
    } else {
      errorMessage = "unsupported unary opcode: " + op.OpcodeName;
      return false;
    }
    write(*op.Output, result);
    return true;
  }

bool PcodeLowerer::lowerSubpiece(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }
    if (op.Inputs[1].Space != "const") {
      errorMessage = "SUBPIECE offset must be constant";
      return false;
    }

    llvm::Value *input = read(op.Inputs[0]);
    uint64_t shiftBits = op.Inputs[1].Offset * 8;
    llvm::Value *shift = llvm::ConstantInt::get(input->getType(), shiftBits);
    write(*op.Output, Builder.CreateLShr(input, shift));
    return true;
  }

bool PcodeLowerer::lowerPiece(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }

    llvm::Type *outputType = intType(op.Output->Size);
    llvm::Value *high = Builder.CreateZExt(read(op.Inputs[0]), outputType);
    llvm::Value *low = Builder.CreateZExt(read(op.Inputs[1]), outputType);
    llvm::Value *shift =
        llvm::ConstantInt::get(outputType, bitWidth(op.Inputs[1].Size));
    write(*op.Output, Builder.CreateOr(Builder.CreateShl(high, shift), low));
    return true;
  }

bool PcodeLowerer::lowerCountBits(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
      return false;
    }

    llvm::Value *input = read(op.Inputs[0]);
    llvm::Intrinsic::ID intrinsicId =
        op.Opcode == PcodeOpcode::Popcount ? llvm::Intrinsic::ctpop
                                           : llvm::Intrinsic::ctlz;
    llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
        &Module, intrinsicId, {input->getType()});
    if (op.Opcode == PcodeOpcode::Lzcount) {
      auto *zeroUndef = llvm::ConstantInt::getFalse(Context);
      write(*op.Output, Builder.CreateCall(intrinsic, {input, zeroUndef}));
    } else {
      write(*op.Output, Builder.CreateCall(intrinsic, {input}));
    }
    return true;
  }

bool PcodeLowerer::lowerCopyLike(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
      return false;
    }
    write(*op.Output, read(op.Inputs[0]));
    return true;
  }

bool PcodeLowerer::lowerPtrAdd(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 3, errorMessage)) {
      return false;
    }
    if (op.Inputs[2].Space != "const") {
      errorMessage = "PTRADD element size must be constant";
      return false;
    }

    llvm::Value *base = resize(read(op.Inputs[0]), op.Output->Size);
    llvm::Value *index = resize(read(op.Inputs[1]), op.Output->Size);
    llvm::Value *scale =
        llvm::ConstantInt::get(base->getType(), op.Inputs[2].Offset);
    write(*op.Output, Builder.CreateAdd(base, Builder.CreateMul(index, scale)));
    return true;
  }

bool PcodeLowerer::lowerPtrSub(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }
    if (op.Inputs[1].Space != "const") {
      errorMessage = "PTRSUB offset must be constant";
      return false;
    }

    llvm::Value *base = resize(read(op.Inputs[0]), op.Output->Size);
    llvm::Value *offset =
        llvm::ConstantInt::get(base->getType(), op.Inputs[1].Offset);
    write(*op.Output, Builder.CreateAdd(base, offset));
    return true;
  }

bool PcodeLowerer::lowerSignedBorrow(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }

    llvm::Value *lhs = read(op.Inputs[0]);
    llvm::Value *rhs = resize(read(op.Inputs[1]), op.Inputs[0].Size);
    llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
        &Module, llvm::Intrinsic::ssub_with_overflow, {lhs->getType()});
    llvm::Value *call = Builder.CreateCall(intrinsic, {lhs, rhs});
    write(*op.Output, Builder.CreateExtractValue(call, 1));
    return true;
  }

bool PcodeLowerer::lowerAddOverflow(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }

    llvm::Value *lhs = read(op.Inputs[0]);
    llvm::Value *rhs = resize(read(op.Inputs[1]), op.Inputs[0].Size);
    llvm::Intrinsic::ID intrinsicId =
        op.Opcode == PcodeOpcode::IntCarry
            ? llvm::Intrinsic::uadd_with_overflow
            : llvm::Intrinsic::sadd_with_overflow;
    llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
        &Module, intrinsicId, {lhs->getType()});
    llvm::Value *call = Builder.CreateCall(intrinsic, {lhs, rhs});
    write(*op.Output, Builder.CreateExtractValue(call, 1));
    return true;
  }

bool PcodeLowerer::lowerBoolNegate(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
      return false;
    }

    llvm::Value *input = asCondition(read(op.Inputs[0]));
    llvm::Value *result = Builder.CreateNot(input);
    write(*op.Output, result);
    return true;
  }

bool PcodeLowerer::lowerBoolBinary(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }

    llvm::Value *lhs = asCondition(read(op.Inputs[0]));
    llvm::Value *rhs = asCondition(read(op.Inputs[1]));
    llvm::Value *result = nullptr;
    switch (op.Opcode) {
    case PcodeOpcode::BoolAnd:
      result = Builder.CreateAnd(lhs, rhs);
      break;
    case PcodeOpcode::BoolOr:
      result = Builder.CreateOr(lhs, rhs);
      break;
    case PcodeOpcode::BoolXor:
      result = Builder.CreateXor(lhs, rhs);
      break;
    default:
      return false;
    }
    write(*op.Output, result);
    return true;
  }

bool PcodeLowerer::lowerFloatBinary(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }
    llvm::Type *floatTy = floatType(op.Output->Size);
    if (floatTy == nullptr) {
      return lowerHelperCall(op, errorMessage);
    }

    llvm::Value *lhs = readFloatBits(op.Inputs[0], floatTy);
    llvm::Value *rhs = readFloatBits(op.Inputs[1], floatTy);
    llvm::Value *result = nullptr;
    switch (op.Opcode) {
    case PcodeOpcode::FloatAdd:
      result = Builder.CreateFAdd(lhs, rhs);
      break;
    case PcodeOpcode::FloatSub:
      result = Builder.CreateFSub(lhs, rhs);
      break;
    case PcodeOpcode::FloatMult:
      result = Builder.CreateFMul(lhs, rhs);
      break;
    case PcodeOpcode::FloatDiv:
      result = Builder.CreateFDiv(lhs, rhs);
      break;
    default:
      errorMessage = "unsupported float binary opcode: " + op.OpcodeName;
      return false;
    }
    writeFloatBits(*op.Output, result);
    return true;
  }

bool PcodeLowerer::lowerFloatCompare(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }
    llvm::Type *floatTy = floatType(op.Inputs[0].Size);
    if (floatTy == nullptr) {
      return lowerHelperCall(op, errorMessage);
    }

    llvm::Value *lhs = readFloatBits(op.Inputs[0], floatTy);
    llvm::Value *rhs = readFloatBits(op.Inputs[1], floatTy);
    llvm::Value *result = nullptr;
    switch (op.Opcode) {
    case PcodeOpcode::FloatEqual:
      result = Builder.CreateFCmpOEQ(lhs, rhs);
      break;
    case PcodeOpcode::FloatNotEqual:
      result = Builder.CreateFCmpUNE(lhs, rhs);
      break;
    case PcodeOpcode::FloatLess:
      result = Builder.CreateFCmpOLT(lhs, rhs);
      break;
    case PcodeOpcode::FloatLessEqual:
      result = Builder.CreateFCmpOLE(lhs, rhs);
      break;
    default:
      errorMessage = "unsupported float compare opcode: " + op.OpcodeName;
      return false;
    }
    write(*op.Output, result);
    return true;
  }

bool PcodeLowerer::lowerFloatNan(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
      return false;
    }
    llvm::Type *floatTy = floatType(op.Inputs[0].Size);
    if (floatTy == nullptr) {
      return lowerHelperCall(op, errorMessage);
    }
    llvm::Value *input = readFloatBits(op.Inputs[0], floatTy);
    write(*op.Output, Builder.CreateFCmpUNO(input, input));
    return true;
  }

bool PcodeLowerer::lowerFloatUnary(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
      return false;
    }
    llvm::Type *floatTy = floatType(op.Output->Size);
    if (floatTy == nullptr) {
      return lowerHelperCall(op, errorMessage);
    }

    llvm::Value *input = readFloatBits(op.Inputs[0], floatTy);
    llvm::Value *result = nullptr;
    if (op.Opcode == PcodeOpcode::FloatNeg) {
      result = Builder.CreateFNeg(input);
    } else if (op.Opcode == PcodeOpcode::FloatAbs) {
      llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
          &Module, llvm::Intrinsic::fabs, {floatTy});
      result = Builder.CreateCall(intrinsic, {input});
    } else if (op.Opcode == PcodeOpcode::FloatSqrt) {
      llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
          &Module, llvm::Intrinsic::sqrt, {floatTy});
      result = Builder.CreateCall(intrinsic, {input});
    } else if (op.Opcode == PcodeOpcode::FloatCeil ||
               op.Opcode == PcodeOpcode::FloatFloor ||
               op.Opcode == PcodeOpcode::FloatRound) {
      llvm::Intrinsic::ID intrinsicId = llvm::Intrinsic::not_intrinsic;
      if (op.Opcode == PcodeOpcode::FloatCeil) {
        intrinsicId = llvm::Intrinsic::ceil;
      } else if (op.Opcode == PcodeOpcode::FloatFloor) {
        intrinsicId = llvm::Intrinsic::floor;
      } else {
        intrinsicId = llvm::Intrinsic::round;
      }
      llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
          &Module, intrinsicId, {floatTy});
      result = Builder.CreateCall(intrinsic, {input});
    } else {
      errorMessage = "unsupported float unary opcode: " + op.OpcodeName;
      return false;
    }
    writeFloatBits(*op.Output, result);
    return true;
  }

bool PcodeLowerer::lowerFloatCast(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
      return false;
    }

    llvm::Type *outputFloatTy = floatType(op.Output->Size);
    if (op.Opcode == PcodeOpcode::FloatInt2Float) {
      if (outputFloatTy == nullptr) {
        return lowerHelperCall(op, errorMessage);
      }
      writeFloatBits(*op.Output,
                     Builder.CreateSIToFP(read(op.Inputs[0]), outputFloatTy));
      return true;
    }

    llvm::Type *inputFloatTy = floatType(op.Inputs[0].Size);
    if (inputFloatTy == nullptr) {
      return lowerHelperCall(op, errorMessage);
    }
    llvm::Value *input = readFloatBits(op.Inputs[0], inputFloatTy);
    if (op.Opcode == PcodeOpcode::FloatFloat2Float) {
      if (outputFloatTy == nullptr) {
        return lowerHelperCall(op, errorMessage);
      }
      writeFloatBits(*op.Output, Builder.CreateFPCast(input, outputFloatTy));
      return true;
    }
    if (op.Opcode == PcodeOpcode::FloatTrunc) {
      write(*op.Output, Builder.CreateFPToSI(input, intType(op.Output->Size)));
      return true;
    }

    errorMessage = "unsupported float cast opcode: " + op.OpcodeName;
    return false;
  }

llvm::GlobalVariable *PcodeLowerer::memoryGlobal() {
    if (Memory) {
      return Memory;
    }

    // First memory model: one external byte array.  It keeps LOAD/STORE visible
    // in IR without pretending we already understand binary sections or ABI.
    auto *byteType = llvm::Type::getInt8Ty(Context);
    auto *arrayType = llvm::ArrayType::get(byteType, 1024 * 1024);
    std::string name = "notdec_ram";
    unsigned index = 1;
    while (llvm::Value *existing = Module.getNamedValue(name)) {
      auto *global = llvm::dyn_cast<llvm::GlobalVariable>(existing);
      if (global != nullptr && global->getValueType() == arrayType) {
        Memory = global;
        return Memory;
      }
      name = "notdec_ram." + std::to_string(index++);
    }
    Memory = new llvm::GlobalVariable(Module, arrayType, false,
                                      llvm::GlobalValue::ExternalLinkage,
                                      nullptr, name);
    return Memory;
  }

llvm::Value *PcodeLowerer::memoryPointer(llvm::Value *address) {
    address = resize(address, pointerByteSize());
    if (Config.MemoryModel == PcodeMemoryModel::IntToPtr) {
      return Builder.CreateIntToPtr(address, llvm::PointerType::get(Context, 0),
                                    "notdec_ram_ptr");
    }

    llvm::Value *zero = llvm::ConstantInt::get(address->getType(), 0);
    return Builder.CreateGEP(memoryGlobal()->getValueType(), memoryGlobal(),
                             {zero, address}, "notdec_ram_ptr");
  }

bool PcodeLowerer::requireConstSpaceSelector(const PcodeOpView &op,
                                 std::string &errorMessage) {
    if (!op.Inputs.empty() && op.Inputs[0].Space == "const") {
      return true;
    }

    errorMessage = op.OpcodeName + " address space selector must be const";
    return false;
  }

bool PcodeLowerer::lowerLoad(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage) ||
        !requireConstSpaceSelector(op, errorMessage)) {
      return false;
    }

    llvm::Value *address = resize(read(op.Inputs[1]), pointerByteSize());
    auto *load =
        Builder.CreateLoad(intType(op.Output->Size), memoryPointer(address),
                           valueName(*op.Output));
    load->setAlignment(llvm::Align(1));
    write(*op.Output, load);
    return true;
  }

bool PcodeLowerer::lowerStore(const PcodeOpView &op, std::string &errorMessage) {
    if (!requireInputCount(op, 3, errorMessage) ||
        !requireConstSpaceSelector(op, errorMessage)) {
      return false;
    }

    llvm::Value *address = resize(read(op.Inputs[1]), pointerByteSize());
    llvm::Value *value = read(op.Inputs[2]);
    auto *store = Builder.CreateStore(value, memoryPointer(address));
    store->setAlignment(llvm::Align(1));
    return true;
  }

bool PcodeLowerer::lowerHelperCall(const PcodeOpView &op, std::string &errorMessage) {
    llvm::Type *returnType = llvm::Type::getVoidTy(Context);
    std::string helperName = "notdec_pcode_" + op.OpcodeName + "_void";
    if (op.Output) {
      returnType = intType(op.Output->Size);
      helperName = "notdec_pcode_" + op.OpcodeName + "_i" +
                   std::to_string(bitWidth(op.Output->Size));
    }

    std::vector<llvm::Value *> args;
    args.reserve(op.Inputs.size());
    for (const VarnodeView &input : op.Inputs) {
      if (input.Space == "ram") {
        args.push_back(
            llvm::ConstantInt::get(intType(input.Size), input.Offset));
      } else {
        args.push_back(read(input));
      }
    }

    auto *helperType = llvm::FunctionType::get(returnType, {}, true);
    llvm::FunctionCallee helper =
        Module.getOrInsertFunction(helperName, helperType);
    llvm::CallInst *call = Builder.CreateCall(helper, args);
    if (!op.Output) {
      return true;
    }
    write(*op.Output, call);
    return true;
  }

llvm::CallInst *PcodeLowerer::lowerKnownVoidCall(const std::string &calleeName) {
    auto *calleeType =
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
    llvm::FunctionCallee callee =
        Module.getOrInsertFunction(calleeName, calleeType);
    return Builder.CreateCall(callee, {});
  }

llvm::CallInst *PcodeLowerer::createUnknownVoidIndirectCall(const VarnodeView &target) {
    auto *calleeType =
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
    auto *calleePointer =
        Builder.CreateIntToPtr(resize(read(target), pointerByteSize()),
                               llvm::PointerType::getUnqual(Context));
    return Builder.CreateCall(calleeType, calleePointer, {});
  }

bool PcodeLowerer::lowerUnknownVoidIndirectCall(const VarnodeView &target) {
    createUnknownVoidIndirectCall(target);
    return true;
  }

bool PcodeLowerer::lowerUnknownVoidIndirectTailJump(const VarnodeView &target) {
    llvm::CallInst *call = createUnknownVoidIndirectCall(target);
    call->setTailCallKind(llvm::CallInst::TCK_Tail);
    Builder.CreateRetVoid();
    return true;
  }

bool PcodeLowerer::lowerKnownVoidTailJump(const std::string &calleeName) {
    // Current native lowering has no ABI/prototype model.  For proven external
    // tail jumps, preserve the handoff to the external symbol and end this body.
    llvm::CallInst *call = lowerKnownVoidCall(calleeName);
    call->setTailCallKind(llvm::CallInst::TCK_Tail);
    Builder.CreateRetVoid();
    return true;
  }

bool PcodeLowerer::lowerCall(const PcodeOpView &op, std::string &errorMessage) {
    // Minimal inter-function lowering: when --all-confirmed has already planned
    // a symbol for this direct target, emit a real LLVM call.  Calls with a
    // modeled return value stay on the helper path until native prototype
    // recovery exists.
    if (!op.Output) {
      if (auto target = directTarget(op, 0)) {
        auto externalIt = Config.ExternalCallTargets.find(*target);
        if (externalIt != Config.ExternalCallTargets.end()) {
          lowerKnownVoidCall(externalIt->second);
          return true;
        }

        auto it = Config.DirectCallTargets.find(*target);
        if (it != Config.DirectCallTargets.end()) {
          lowerKnownVoidCall(it->second);
          return true;
        }

        lowerKnownVoidCall(addressFunctionName(*target));
        return true;
      }
    }

    return lowerHelperCall(op, errorMessage);
  }

bool PcodeLowerer::lowerCallInd(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output && requireInputCount(op, 1, errorMessage)) {
      if (auto gotAddress = sourceRam(op.Inputs[0])) {
        auto it = Config.IndirectExternalCallTargets.find(*gotAddress);
        if (it != Config.IndirectExternalCallTargets.end()) {
          lowerKnownVoidCall(it->second);
          return true;
        }
      }
      return lowerUnknownVoidIndirectCall(op.Inputs[0]);
    }
    return lowerHelperCall(op, errorMessage);
  }

bool PcodeLowerer::lowerX86Movmskpd(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 3, errorMessage)) {
      return false;
    }
    if (op.Inputs[2].Size == 0 || op.Inputs[2].Size % 8 != 0) {
      return lowerHelperCall(op, errorMessage);
    }

    llvm::Value *source = read(op.Inputs[2]);
    llvm::Value *mask = llvm::ConstantInt::get(intType(op.Output->Size), 0);
    uint32_t laneCount = op.Inputs[2].Size / 8;
    for (uint32_t lane = 0; lane < laneCount; ++lane) {
      llvm::Value *shifted = Builder.CreateLShr(
          source, llvm::ConstantInt::get(source->getType(), lane * 64 + 63));
      llvm::Value *bit = Builder.CreateTrunc(
          Builder.CreateAnd(shifted,
                            llvm::ConstantInt::get(source->getType(), 1)),
          intType(1));
      llvm::Value *wideBit = resize(bit, op.Output->Size);
      if (lane != 0) {
        wideBit =
            Builder.CreateShl(wideBit,
                              llvm::ConstantInt::get(wideBit->getType(), lane));
      }
      mask = Builder.CreateOr(mask, wideBit);
    }
    write(*op.Output, mask);
    return true;
  }

std::string PcodeLowerer::userOpName(uint64_t index) const {
    if (CurrentProgram == nullptr ||
        index >= CurrentProgram->UserOpNames.size()) {
      return {};
    }
    return CurrentProgram->UserOpNames[index];
  }

bool PcodeLowerer::isUserOp(const PcodeOpView &op, llvm::StringRef name,
                uint64_t index) const {
    if (op.Inputs.empty() || op.Inputs[0].Space != "const") {
      return false;
    }
    std::string resolved = userOpName(op.Inputs[0].Offset);
    return resolved.empty() ? op.Inputs[0].Offset == index : resolved == name;
  }

bool PcodeLowerer::lowerUnknownUserOp(const PcodeOpView &op, std::string &errorMessage) {
    uint64_t index = op.Inputs.empty() ? 0 : op.Inputs[0].Offset;
    std::string name = userOpName(index);
    std::string safeName =
        name.empty() ? "unknown_" + std::to_string(index) : name;
    for (char &c : safeName) {
      if (!llvm::isAlnum(c) && c != '_' && c != '.') {
        c = '_';
      }
    }
    std::string helperName = "notdec.register.userop." + safeName;

    llvm::Type *returnType = llvm::Type::getVoidTy(Context);
    if (op.Output) {
      returnType = intType(op.Output->Size);
      helperName += ".i" + std::to_string(bitWidth(op.Output->Size));
    }

    std::vector<llvm::Value *> args;
    std::vector<llvm::Type *> argTypes;
    for (size_t i = 1; i < op.Inputs.size(); ++i) {
      const VarnodeView &input = op.Inputs[i];
      args.push_back(input.Space == "ram"
                         ? llvm::ConstantInt::get(intType(input.Size),
                                                  input.Offset)
                         : read(input));
      argTypes.push_back(args.back()->getType());
      helperName += ".i" + std::to_string(bitWidth(input.Size));
    }

    auto *helperType = llvm::FunctionType::get(returnType, argTypes, false);
    llvm::FunctionCallee helper =
        Module.getOrInsertFunction(helperName, helperType);
    warnUnsupportedUserOp(index, name, op.Address, helperName);
    llvm::CallInst *call = Builder.CreateCall(helper, args);
    if (!op.Output) {
      return true;
    }
    write(*op.Output, call);
    return true;
  }

void PcodeLowerer::warnUnsupportedUserOp(uint64_t index, const std::string &name,
                             uint64_t address, const std::string &helperName) {
    // 同一条指令在一个二进制里会出现很多次，每个 userop 只提示一次。
    static std::set<std::string> warned;
    std::string key = std::to_string(index) + ":" + name;
    if (!warned.insert(key).second) {
      return;
    }
    llvm::errs() << "warning: unsupported Sleigh userop " << index;
    if (!name.empty()) {
      llvm::errs() << " (" << name << ")";
    }
    llvm::errs() << " at 0x";
    llvm::errs().write_hex(address);
    llvm::errs() << "; emitted " << helperName
                 << " placeholder - add semantic support in "
                    "PcodeToLLVM.cpp lowerCallOther\n";
  }

bool PcodeLowerer::lowerCallOther(const PcodeOpView &op, std::string &errorMessage) {
    // rdtsc 有现成的 LLVM intrinsic 对应，直接映射。
    if (op.Output && requireInputCount(op, 1, errorMessage) &&
        isUserOp(op, "rdtsc", 77)) {
      llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
          &Module, llvm::Intrinsic::readcyclecounter);
      write(*op.Output, resize(Builder.CreateCall(intrinsic, {}), op.Output->Size));
      return true;
    }

    // x86 LOCK/UNLOCK are Sleigh userops around normal memory P-Code.  The
    // read/write ops still carry the value semantics, so keep only that part.
    if (!op.Output && requireInputCount(op, 1, errorMessage) &&
        (isUserOp(op, "LOCK", 17) || isUserOp(op, "UNLOCK", 18))) {
      return true;
    }

    // Ghidra x86 emits MOVMSKPD as a userop.  The operation extracts the sign
    // bit of each packed double lane into the low bits of the integer result.
    if (op.Output && !op.Inputs.empty() && isUserOp(op, "MOVMSKPD", 128)) {
      return lowerX86Movmskpd(op, errorMessage);
    }

    // 其余 userop 还没建模：专用命名占位 + warning。
    return lowerUnknownUserOp(op, errorMessage);
  }

bool PcodeLowerer::lowerOp(const PcodeOpView &op, std::string &errorMessage) {
    switch (op.Opcode) {
    case PcodeOpcode::Copy:
      if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
        return false;
      }
      {
        std::optional<uint64_t> source = sourceRam(op.Inputs[0]);
        write(*op.Output, read(op.Inputs[0]));
        setSourceRam(*op.Output, source);
      }
      return true;
    case PcodeOpcode::Load:
      return lowerLoad(op, errorMessage);
    case PcodeOpcode::Store:
      return lowerStore(op, errorMessage);
    case PcodeOpcode::Call:
      return lowerCall(op, errorMessage);
    case PcodeOpcode::CallInd:
      return lowerCallInd(op, errorMessage);
    case PcodeOpcode::CallOther:
      return lowerCallOther(op, errorMessage);
    case PcodeOpcode::Branch:
    case PcodeOpcode::BranchInd:
    case PcodeOpcode::CBranch:
    case PcodeOpcode::Return:
      errorMessage = op.OpcodeName + " appeared in non-terminator position";
      return false;
    case PcodeOpcode::IntAdd:
    case PcodeOpcode::IntSub:
    case PcodeOpcode::IntMult:
    case PcodeOpcode::IntDiv:
    case PcodeOpcode::IntSDiv:
    case PcodeOpcode::IntRem:
    case PcodeOpcode::IntSRem:
    case PcodeOpcode::IntAnd:
    case PcodeOpcode::IntOr:
    case PcodeOpcode::IntXor:
    case PcodeOpcode::IntLeft:
    case PcodeOpcode::IntRight:
    case PcodeOpcode::IntSRight:
      return lowerBinary(op, errorMessage);
    case PcodeOpcode::IntEqual:
    case PcodeOpcode::IntNotEqual:
    case PcodeOpcode::IntLess:
    case PcodeOpcode::IntSLess:
    case PcodeOpcode::IntLessEqual:
    case PcodeOpcode::IntSLessEqual:
      return lowerCompare(op, errorMessage);
    case PcodeOpcode::IntZExt:
    case PcodeOpcode::IntSExt:
      return lowerCast(op, errorMessage);
    case PcodeOpcode::IntNegate:
    case PcodeOpcode::Int2Comp:
      return lowerUnary(op, errorMessage);
    case PcodeOpcode::Cast:
    case PcodeOpcode::Indirect:
      return lowerCopyLike(op, errorMessage);
    case PcodeOpcode::Piece:
      return lowerPiece(op, errorMessage);
    case PcodeOpcode::Subpiece:
      return lowerSubpiece(op, errorMessage);
    case PcodeOpcode::PtrAdd:
      return lowerPtrAdd(op, errorMessage);
    case PcodeOpcode::PtrSub:
      return lowerPtrSub(op, errorMessage);
    case PcodeOpcode::Popcount:
    case PcodeOpcode::Lzcount:
      return lowerCountBits(op, errorMessage);
    case PcodeOpcode::IntCarry:
    case PcodeOpcode::IntSCarry:
      return lowerAddOverflow(op, errorMessage);
    case PcodeOpcode::IntSBorrow:
      return lowerSignedBorrow(op, errorMessage);
    case PcodeOpcode::BoolNegate:
      return lowerBoolNegate(op, errorMessage);
    case PcodeOpcode::BoolAnd:
    case PcodeOpcode::BoolOr:
    case PcodeOpcode::BoolXor:
      return lowerBoolBinary(op, errorMessage);
    case PcodeOpcode::FloatEqual:
    case PcodeOpcode::FloatNotEqual:
    case PcodeOpcode::FloatLess:
    case PcodeOpcode::FloatLessEqual:
      return lowerFloatCompare(op, errorMessage);
    case PcodeOpcode::FloatNan:
      return lowerFloatNan(op, errorMessage);
    case PcodeOpcode::FloatAdd:
    case PcodeOpcode::FloatDiv:
    case PcodeOpcode::FloatMult:
    case PcodeOpcode::FloatSub:
      return lowerFloatBinary(op, errorMessage);
    case PcodeOpcode::FloatInt2Float:
    case PcodeOpcode::FloatFloat2Float:
    case PcodeOpcode::FloatTrunc:
      return lowerFloatCast(op, errorMessage);
    case PcodeOpcode::FloatNeg:
    case PcodeOpcode::FloatAbs:
    case PcodeOpcode::FloatSqrt:
    case PcodeOpcode::FloatCeil:
    case PcodeOpcode::FloatFloor:
    case PcodeOpcode::FloatRound:
      return lowerFloatUnary(op, errorMessage);
    case PcodeOpcode::SegmentOp:
    case PcodeOpcode::CpoolRef:
    case PcodeOpcode::New:
    case PcodeOpcode::Insert:
    case PcodeOpcode::Extract:
      return lowerHelperCall(op, errorMessage);
    case PcodeOpcode::Multiequal:
      errorMessage = "MULTIEQUAL is not expected in raw Sleigh P-Code";
      return false;
    case PcodeOpcode::Unsupported:
      break;
    }

    errorMessage = "unsupported p-code opcode: " + op.OpcodeName;
    return false;
  }

} // namespace notdec::bin2llvm
