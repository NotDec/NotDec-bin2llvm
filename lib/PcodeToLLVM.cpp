#include "notdec-bin2llvm/PcodeToLLVM.h"
#include "notdec-bin2llvm/RegisterStorage.h"

#include "llvm/ADT/APInt.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace notdec::bin2llvm {
namespace {

std::string varnodeKey(const VarnodeView &varnode) {
  std::ostringstream os;
  os << varnode.Space << ':' << std::hex << varnode.Offset << ':' << std::dec
     << varnode.Size;
  return os.str();
}

std::string valueName(const VarnodeView &varnode) {
  std::ostringstream os;
  os << varnode.Space << '_' << std::hex << varnode.Offset << '_' << std::dec
     << varnode.Size;
  return os.str();
}

unsigned bitWidth(uint32_t byteSize) {
  return byteSize == 0 ? 1 : byteSize * 8;
}

class PcodeLowerer {
public:
  PcodeLowerer(llvm::LLVMContext &context, llvm::Module &module,
               llvm::Function &function, llvm::IRBuilder<> &builder,
               const PcodeLoweringConfig &config)
      : Context(context), Module(module), Function(function), Builder(builder),
        Config(config) {
  }

  bool lower(const PcodeProgram &program, std::string &errorMessage) {
    Registers = std::make_unique<RegisterStorage>(
        Context, Module, program.Registers, program.IsBigEndian);

    if (program.Ops.empty()) {
      return true;
    }

    buildBasicBlocks(program);
    Builder.CreateBr(BlockForStart[BlockStarts.front()]);

    for (size_t blockIndex = 0; blockIndex < BlockStarts.size(); ++blockIndex) {
      size_t start = BlockStarts[blockIndex];
      size_t end = blockIndex + 1 < BlockStarts.size()
                       ? BlockStarts[blockIndex + 1]
                       : program.Ops.size();
      Builder.SetInsertPoint(BlockForStart[start]);
      Values.clear();

      bool ended = false;
      for (size_t opIndex = start; opIndex < end; ++opIndex) {
        const PcodeOpView &op = program.Ops[opIndex];
        if (isTerminator(op.Opcode)) {
          if (!lowerTerminator(op, nextBlock(blockIndex), errorMessage)) {
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
        if (llvm::BasicBlock *next = nextBlock(blockIndex)) {
          Builder.CreateBr(next);
        } else {
          Builder.CreateRetVoid();
        }
      }
    }

    for (llvm::BasicBlock *target : ExternalTargetBlocks) {
      Builder.SetInsertPoint(target);
      Builder.CreateRetVoid();
    }

    return true;
  }

private:
  static bool isTerminator(PcodeOpcode opcode) {
    return opcode == PcodeOpcode::Branch ||
           opcode == PcodeOpcode::BranchInd ||
           opcode == PcodeOpcode::CBranch || opcode == PcodeOpcode::Return;
  }

  std::string blockName(uint64_t address) {
    std::ostringstream os;
    os << "bb_" << std::hex << address;
    return os.str();
  }

  std::optional<uint64_t> directTarget(const PcodeOpView &op,
                                       size_t inputIndex) {
    if (op.Inputs.size() <= inputIndex ||
        op.Inputs[inputIndex].Space != "ram") {
      return std::nullopt;
    }
    return op.Inputs[inputIndex].Offset;
  }

  void addBlockStart(std::set<size_t> &starts,
                     const std::map<uint64_t, size_t> &firstOpForAddress,
                     uint64_t address) {
    auto it = firstOpForAddress.find(address);
    if (it != firstOpForAddress.end()) {
      starts.insert(it->second);
    }
  }

  void buildBasicBlocks(const PcodeProgram &program) {
    std::map<uint64_t, size_t> firstOpForAddress;
    for (size_t index = 0; index < program.Ops.size(); ++index) {
      firstOpForAddress.try_emplace(program.Ops[index].Address, index);
    }

    std::set<size_t> starts;
    starts.insert(0);
    for (size_t index = 0; index < program.Ops.size(); ++index) {
      const PcodeOpView &op = program.Ops[index];
      if (op.Opcode == PcodeOpcode::Branch ||
          op.Opcode == PcodeOpcode::BranchInd ||
          op.Opcode == PcodeOpcode::CBranch) {
        if (auto target = directTarget(op, 0)) {
          addBlockStart(starts, firstOpForAddress, *target);
        }
      }

      if (isTerminator(op.Opcode) && index + 1 < program.Ops.size()) {
        starts.insert(index + 1);
      }
    }

    BlockStarts.assign(starts.begin(), starts.end());
    for (size_t index = 0; index < BlockStarts.size(); ++index) {
      size_t start = BlockStarts[index];
      uint64_t address = program.Ops[start].Address;
      llvm::BasicBlock *block =
          llvm::BasicBlock::Create(Context, blockName(address), &Function);
      BlockForStart[start] = block;
      BlockForAddress.try_emplace(address, block);
    }
  }

  llvm::BasicBlock *nextBlock(size_t blockIndex) {
    if (blockIndex + 1 >= BlockStarts.size()) {
      return nullptr;
    }
    return BlockForStart[BlockStarts[blockIndex + 1]];
  }

  llvm::BasicBlock *blockForTarget(uint64_t address) {
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

  llvm::BasicBlock *exitBlock() {
    if (!ExitBlock) {
      ExitBlock = llvm::BasicBlock::Create(Context, "notdec_exit", &Function);
      ExternalTargetBlocks.push_back(ExitBlock);
    }
    return ExitBlock;
  }

  llvm::Value *asCondition(llvm::Value *value) {
    if (value->getType()->isIntegerTy(1)) {
      return value;
    }
    return Builder.CreateICmpNE(value,
                                llvm::ConstantInt::get(value->getType(), 0));
  }

  bool lowerTerminator(const PcodeOpView &op, llvm::BasicBlock *fallthrough,
                       std::string &errorMessage) {
    switch (op.Opcode) {
    case PcodeOpcode::Branch: {
      if (!requireInputCount(op, 1, errorMessage)) {
        return false;
      }
      auto target = directTarget(op, 0);
      if (!target) {
        errorMessage = "BRANCH target must be a direct ram address";
        return false;
      }
      Builder.CreateBr(blockForTarget(*target));
      return true;
    }

    case PcodeOpcode::CBranch: {
      if (!requireInputCount(op, 2, errorMessage)) {
        return false;
      }
      auto target = directTarget(op, 0);
      if (!target) {
        errorMessage = "CBRANCH target must be a direct ram address";
        return false;
      }
      llvm::BasicBlock *falseBlock = fallthrough ? fallthrough : exitBlock();
      Builder.CreateCondBr(asCondition(read(op.Inputs[1])),
                           blockForTarget(*target), falseBlock);
      return true;
    }

    case PcodeOpcode::BranchInd:
      if (!requireInputCount(op, 1, errorMessage)) {
        return false;
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

  llvm::Type *intType(uint32_t byteSize) {
    return llvm::IntegerType::get(Context, bitWidth(byteSize));
  }

  llvm::Value *resize(llvm::Value *value, uint32_t byteSize) {
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

  llvm::Value *read(const VarnodeView &varnode) {
    llvm::Type *type = intType(varnode.Size);
    if (varnode.Space == "const") {
      return llvm::ConstantInt::get(
          type, llvm::APInt(bitWidth(varnode.Size), varnode.Offset));
    }

    auto it = Values.find(varnodeKey(varnode));
    if (it != Values.end()) {
      return resize(it->second, varnode.Size);
    }

    if (varnode.IsRegister && Registers != nullptr) {
      RegisterAccess access{varnode.Space, varnode.Offset, varnode.Size,
                            varnode.RegisterName};
      if (llvm::Value *value = Registers->read(Builder, access)) {
        return value;
      }
    }

    return Builder.CreateFreeze(llvm::PoisonValue::get(type),
                                valueName(varnode) + "_in");
  }

  void write(const VarnodeView &varnode, llvm::Value *value) {
    llvm::Value *resized = resize(value, varnode.Size);
    if (varnode.IsRegister && Registers != nullptr) {
      RegisterAccess access{varnode.Space, varnode.Offset, varnode.Size,
                            varnode.RegisterName};
      if (Registers->hasRegister(access)) {
        Registers->write(Builder, access, resized);
        return;
      }
    }

    if (auto *instruction = llvm::dyn_cast<llvm::Instruction>(resized)) {
      instruction->setName(valueName(varnode));
    }
    Values[varnodeKey(varnode)] = resized;
  }

  bool requireInputCount(const PcodeOpView &op, size_t count,
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

  bool lowerBinary(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
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
    case PcodeOpcode::IntRem:
      result = Builder.CreateURem(lhs, rhs);
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

  bool lowerCompare(const PcodeOpView &op, std::string &errorMessage) {
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
    case PcodeOpcode::IntSLess:
      result = Builder.CreateICmpSLT(lhs, rhs);
      break;
    default:
      return false;
    }

    write(*op.Output, result);
    return true;
  }

  bool lowerCast(const PcodeOpView &op, std::string &errorMessage) {
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

  bool lowerSubpiece(const PcodeOpView &op, std::string &errorMessage) {
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

  bool lowerPiece(const PcodeOpView &op, std::string &errorMessage) {
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

  bool lowerPopcount(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
      return false;
    }

    llvm::Value *input = read(op.Inputs[0]);
    llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
        &Module, llvm::Intrinsic::ctpop, {input->getType()});
    write(*op.Output, Builder.CreateCall(intrinsic, {input}));
    return true;
  }

  bool lowerSignedBorrow(const PcodeOpView &op, std::string &errorMessage) {
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

  bool lowerAddOverflow(const PcodeOpView &op, std::string &errorMessage) {
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

  bool lowerBoolNegate(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
      return false;
    }

    llvm::Value *input = asCondition(read(op.Inputs[0]));
    llvm::Value *result = Builder.CreateNot(input);
    write(*op.Output, result);
    return true;
  }

  bool lowerBoolBinary(const PcodeOpView &op, std::string &errorMessage) {
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

  llvm::GlobalVariable *memoryGlobal() {
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

  llvm::Value *memoryPointer(llvm::Value *address) {
    llvm::Value *zero = llvm::ConstantInt::get(address->getType(), 0);
    return Builder.CreateGEP(memoryGlobal()->getValueType(), memoryGlobal(),
                             {zero, address}, "notdec_ram_ptr");
  }

  bool requireConstSpaceSelector(const PcodeOpView &op,
                                 std::string &errorMessage) {
    if (!op.Inputs.empty() && op.Inputs[0].Space == "const") {
      return true;
    }

    errorMessage = op.OpcodeName + " address space selector must be const";
    return false;
  }

  bool lowerLoad(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage) ||
        !requireConstSpaceSelector(op, errorMessage)) {
      return false;
    }

    llvm::Value *address = resize(read(op.Inputs[1]), 8);
    auto *load =
        Builder.CreateLoad(intType(op.Output->Size), memoryPointer(address),
                           valueName(*op.Output));
    load->setAlignment(llvm::Align(1));
    write(*op.Output, load);
    return true;
  }

  bool lowerStore(const PcodeOpView &op, std::string &errorMessage) {
    if (!requireInputCount(op, 3, errorMessage) ||
        !requireConstSpaceSelector(op, errorMessage)) {
      return false;
    }

    llvm::Value *address = resize(read(op.Inputs[1]), 8);
    llvm::Value *value = read(op.Inputs[2]);
    auto *store = Builder.CreateStore(value, memoryPointer(address));
    store->setAlignment(llvm::Align(1));
    return true;
  }

  bool lowerHelperCall(const PcodeOpView &op, std::string &errorMessage) {
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

  bool lowerCall(const PcodeOpView &op, std::string &errorMessage) {
    // Minimal inter-function lowering: when --all-confirmed has already planned
    // a symbol for this direct target, emit a real LLVM call.  Calls with a
    // modeled return value stay on the helper path until native prototype
    // recovery exists.
    if (!op.Output) {
      if (auto target = directTarget(op, 0)) {
        auto externalIt = Config.ExternalCallTargets.find(*target);
        if (externalIt != Config.ExternalCallTargets.end()) {
          auto *calleeType =
              llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
          llvm::FunctionCallee callee =
              Module.getOrInsertFunction(externalIt->second, calleeType);
          Builder.CreateCall(callee, {});
          return true;
        }

        auto it = Config.DirectCallTargets.find(*target);
        if (it != Config.DirectCallTargets.end()) {
          auto *calleeType =
              llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
          llvm::FunctionCallee callee =
              Module.getOrInsertFunction(it->second, calleeType);
          Builder.CreateCall(callee, {});
          return true;
        }
      }
    }

    return lowerHelperCall(op, errorMessage);
  }

  bool lowerOp(const PcodeOpView &op, std::string &errorMessage) {
    switch (op.Opcode) {
    case PcodeOpcode::Copy:
      if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
        return false;
      }
      write(*op.Output, read(op.Inputs[0]));
      return true;
    case PcodeOpcode::Load:
      return lowerLoad(op, errorMessage);
    case PcodeOpcode::Store:
      return lowerStore(op, errorMessage);
    case PcodeOpcode::Call:
      return lowerCall(op, errorMessage);
    case PcodeOpcode::CallInd:
    case PcodeOpcode::CallOther:
      return lowerHelperCall(op, errorMessage);
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
    case PcodeOpcode::IntRem:
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
      return lowerCompare(op, errorMessage);
    case PcodeOpcode::IntZExt:
    case PcodeOpcode::IntSExt:
      return lowerCast(op, errorMessage);
    case PcodeOpcode::Piece:
      return lowerPiece(op, errorMessage);
    case PcodeOpcode::Subpiece:
      return lowerSubpiece(op, errorMessage);
    case PcodeOpcode::Popcount:
      return lowerPopcount(op, errorMessage);
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
    case PcodeOpcode::Unsupported:
      break;
    }

    errorMessage = "unsupported p-code opcode: " + op.OpcodeName;
    return false;
  }

  llvm::LLVMContext &Context;
  llvm::Module &Module;
  llvm::Function &Function;
  llvm::IRBuilder<> &Builder;
  const PcodeLoweringConfig &Config;
  std::unordered_map<std::string, llvm::Value *> Values;
  std::vector<size_t> BlockStarts;
  std::unordered_map<size_t, llvm::BasicBlock *> BlockForStart;
  std::unordered_map<uint64_t, llvm::BasicBlock *> BlockForAddress;
  std::vector<llvm::BasicBlock *> ExternalTargetBlocks;
  llvm::BasicBlock *ExitBlock = nullptr;
  llvm::GlobalVariable *Memory = nullptr;
  std::unique_ptr<RegisterStorage> Registers;
};

} // namespace

bool appendPcodeFunction(llvm::LLVMContext &context, llvm::Module &module,
                         const PcodeProgram &program,
                         const PcodeLoweringConfig &config,
                         std::string &errorMessage) {
  auto *functionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
  llvm::Function *function = module.getFunction(config.EntryFunctionName);
  if (function != nullptr) {
    if (!function->empty()) {
      errorMessage = "duplicate function name: " + config.EntryFunctionName;
      return false;
    }
    if (function->getFunctionType() != functionType) {
      errorMessage =
          "function declaration type mismatch: " + config.EntryFunctionName;
      return false;
    }
  } else {
    function =
        llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage,
                               config.EntryFunctionName, &module);
  }

  auto *entryBlock = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entryBlock);
  PcodeLowerer lowerer(context, module, *function, builder, config);
  if (!lowerer.lower(program, errorMessage)) {
    function->eraseFromParent();
    return false;
  }
  return true;
}

std::unique_ptr<llvm::Module>
buildPcodeModule(llvm::LLVMContext &context, const PcodeProgram &program,
                 const PcodeLoweringConfig &config, std::string &errorMessage) {
  auto module = std::make_unique<llvm::Module>(config.ModuleName, context);
  if (!appendPcodeFunction(context, *module, program, config, errorMessage)) {
    return nullptr;
  }
  return module;
}

} // namespace notdec::bin2llvm
