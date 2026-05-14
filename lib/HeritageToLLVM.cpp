#include "notdec-bin2llvm/HeritageToLLVM.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"

#include <sstream>
#include <unordered_map>

namespace notdec::bin2llvm {
namespace {

unsigned bitWidth(uint32_t byteSize) { return byteSize == 0 ? 1 : byteSize * 8; }

bool isIntLikeType(const std::string &type) {
  return type == "int" || type == "uint" || type == "undefined4";
}

class HeritageLowerer {
public:
  HeritageLowerer(llvm::LLVMContext &context, llvm::Module &module,
                  const HeritageProgram &program)
      : Context(context), Module(module), Program(program), Builder(context) {}

  bool lower(std::string &errorMessage) {
    if (!createFunction(errorMessage)) {
      return false;
    }
    createBlocks();
    mapParameters(errorMessage);
    if (!errorMessage.empty()) {
      return false;
    }

    for (const HeritageBlock &block : Program.Blocks) {
      Builder.SetInsertPoint(BlockMap.at(block.Id));
      if (!lowerBlock(block, errorMessage)) {
        return false;
      }
    }
    return true;
  }

private:
  llvm::Type *intType(uint32_t byteSize) {
    return llvm::IntegerType::get(Context, bitWidth(byteSize));
  }

  llvm::Type *typeForSourceType(const std::string &type) {
    if (isIntLikeType(type)) {
      return intType(4);
    }
    return intType(4);
  }

  bool createFunction(std::string &errorMessage) {
    std::vector<llvm::Type *> paramTypes;
    for (const HeritageParam &param : Program.Function.Params) {
      paramTypes.push_back(typeForSourceType(param.Type));
    }

    llvm::Type *returnType = nullptr;
    if (Program.Function.ReturnType == "void") {
      returnType = llvm::Type::getVoidTy(Context);
    } else {
      returnType = typeForSourceType(Program.Function.ReturnType);
    }

    auto *functionType = llvm::FunctionType::get(returnType, paramTypes, false);
    Function = llvm::Function::Create(functionType,
                                      llvm::GlobalValue::ExternalLinkage,
                                      Program.Function.Name, &Module);
    unsigned index = 0;
    for (llvm::Argument &argument : Function->args()) {
      if (index < Program.Function.Params.size()) {
        argument.setName(Program.Function.Params[index].Name);
      }
      ++index;
    }

    if (Program.Blocks.empty()) {
      errorMessage = "heritage program has no blocks";
      return false;
    }
    return true;
  }

  void createBlocks() {
    for (const HeritageBlock &block : Program.Blocks) {
      std::string name = block.Id == Program.Blocks.front().Id ? "entry"
                                                               : block.Id;
      BlockMap.emplace(block.Id,
                       llvm::BasicBlock::Create(Context, name, Function));
    }
  }

  void mapParameters(std::string &errorMessage) {
    auto arg = Function->arg_begin();
    for (const HeritageParam &param : Program.Function.Params) {
      if (!param.Varnode) {
        std::ostringstream os;
        os << "parameter " << param.Name << " has no varnode";
        errorMessage = os.str();
        return;
      }
      if (arg == Function->arg_end()) {
        errorMessage = "LLVM function argument mismatch";
        return;
      }
      Values[*param.Varnode] = &*arg;
      ++arg;
    }
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

  const HeritageVarnode *varnodeFor(const std::string &id) const {
    auto it = Program.VarnodeById.find(id);
    return it != Program.VarnodeById.end() ? it->second : nullptr;
  }

  llvm::Value *read(const std::string &id) {
    if (auto it = Values.find(id); it != Values.end()) {
      const HeritageVarnode *varnode = varnodeFor(id);
      return varnode ? resize(it->second, varnode->Size) : it->second;
    }

    const HeritageVarnode *varnode = varnodeFor(id);
    if (varnode == nullptr) {
      return nullptr;
    }
    if (varnode->IsConstant) {
      return llvm::ConstantInt::get(intType(varnode->Size), varnode->Offset);
    }

    return Builder.CreateFreeze(llvm::PoisonValue::get(intType(varnode->Size)),
                                id + "_in");
  }

  bool write(const std::string &id, llvm::Value *value,
             std::string &errorMessage) {
    const HeritageVarnode *varnode = varnodeFor(id);
    if (varnode == nullptr) {
      errorMessage = "unknown output varnode: " + id;
      return false;
    }
    Values[id] = resize(value, varnode->Size);
    return true;
  }

  bool requireInputs(const HeritageOp &op, size_t count,
                     std::string &errorMessage) {
    if (op.Inputs.size() == count) {
      return true;
    }
    std::ostringstream os;
    os << op.Mnemonic << " needs " << count << " inputs, got "
       << op.Inputs.size();
    errorMessage = os.str();
    return false;
  }

  bool lowerCopy(const HeritageOp &op, std::string &errorMessage) {
    if (!op.Output || !requireInputs(op, 1, errorMessage)) {
      return false;
    }
    llvm::Value *input = read(op.Inputs[0]);
    if (input == nullptr) {
      errorMessage = "COPY reads unknown varnode: " + op.Inputs[0];
      return false;
    }
    return write(*op.Output, input, errorMessage);
  }

  bool lowerIntAdd(const HeritageOp &op, std::string &errorMessage) {
    if (!op.Output || !requireInputs(op, 2, errorMessage)) {
      return false;
    }
    const HeritageVarnode *output = varnodeFor(*op.Output);
    if (output == nullptr) {
      errorMessage = "INT_ADD output is unknown";
      return false;
    }
    llvm::Value *lhs = resize(read(op.Inputs[0]), output->Size);
    llvm::Value *rhs = resize(read(op.Inputs[1]), output->Size);
    return write(*op.Output, Builder.CreateAdd(lhs, rhs), errorMessage);
  }

  bool lowerCompare(const HeritageOp &op, std::string &errorMessage) {
    if (!op.Output || !requireInputs(op, 2, errorMessage)) {
      return false;
    }
    const HeritageVarnode *lhsVarnode = varnodeFor(op.Inputs[0]);
    if (lhsVarnode == nullptr) {
      errorMessage = "compare input is unknown";
      return false;
    }
    llvm::Value *lhs = resize(read(op.Inputs[0]), lhsVarnode->Size);
    llvm::Value *rhs = resize(read(op.Inputs[1]), lhsVarnode->Size);
    llvm::Value *result = nullptr;
    if (op.Mnemonic == "INT_SLESSEQUAL") {
      result = Builder.CreateICmpSLE(lhs, rhs);
    } else if (op.Mnemonic == "INT_SLESS") {
      result = Builder.CreateICmpSLT(lhs, rhs);
    } else if (op.Mnemonic == "INT_EQUAL") {
      result = Builder.CreateICmpEQ(lhs, rhs);
    } else {
      errorMessage = "unsupported compare opcode: " + op.Mnemonic;
      return false;
    }
    return write(*op.Output, result, errorMessage);
  }

  bool lowerPhi(const HeritageOp &op, std::string &errorMessage) {
    if (!op.Output) {
      errorMessage = "MULTIEQUAL has no output";
      return false;
    }
    const HeritageBlock *block = Program.BlockById.at(op.Parent);
    const HeritageVarnode *output = varnodeFor(*op.Output);
    if (output == nullptr) {
      errorMessage = "MULTIEQUAL output is unknown";
      return false;
    }
    if (block->In.size() != op.Inputs.size()) {
      errorMessage = "MULTIEQUAL input count does not match predecessor count";
      return false;
    }

    auto *phi = Builder.CreatePHI(intType(output->Size), op.Inputs.size(),
                                  *op.Output);
    Values[*op.Output] = phi;
    for (size_t index = 0; index < op.Inputs.size(); ++index) {
      llvm::BasicBlock *incomingBlock = BlockMap.at(block->In[index]);
      llvm::Value *incoming = read(op.Inputs[index]);
      phi->addIncoming(resize(incoming, output->Size), incomingBlock);
    }
    return true;
  }

  bool lowerCall(const HeritageOp &op, std::string &errorMessage) {
    if (!op.Output || !op.CallTargetName || op.Inputs.empty()) {
      errorMessage = "CALL needs output, target name, and target input";
      return false;
    }

    std::vector<llvm::Value *> args;
    std::vector<llvm::Type *> argTypes;
    for (size_t index = 1; index < op.Inputs.size(); ++index) {
      llvm::Value *arg = read(op.Inputs[index]);
      if (arg == nullptr) {
        errorMessage = "CALL reads unknown argument varnode";
        return false;
      }
      args.push_back(arg);
      argTypes.push_back(arg->getType());
    }

    const HeritageVarnode *output = varnodeFor(*op.Output);
    if (output == nullptr) {
      errorMessage = "CALL output is unknown";
      return false;
    }
    auto *calleeType = llvm::FunctionType::get(intType(output->Size), argTypes,
                                               false);
    llvm::FunctionCallee callee =
        Module.getOrInsertFunction(*op.CallTargetName, calleeType);
    return write(*op.Output, Builder.CreateCall(callee, args), errorMessage);
  }

  llvm::Value *returnValueFor(const HeritageOp &op, std::string &errorMessage) {
    if (Program.Function.ReturnType == "void") {
      return nullptr;
    }
    if (op.Inputs.size() < 2) {
      errorMessage = "RETURN with non-void function needs value input";
      return nullptr;
    }
    llvm::Value *value = read(op.Inputs[1]);
    if (value == nullptr) {
      errorMessage = "RETURN reads unknown varnode";
      return nullptr;
    }
    return resize(value, 4);
  }

  bool lowerBranch(const HeritageOp &op, const HeritageBlock &block,
                   std::string &errorMessage) {
    if (op.Mnemonic == "CBRANCH") {
      if (!requireInputs(op, 2, errorMessage)) {
        return false;
      }
      const HeritageVarnode *target = varnodeFor(op.Inputs[0]);
      if (target == nullptr) {
        errorMessage = "CBRANCH target varnode is unknown";
        return false;
      }
      auto trueBlockIt = Program.BlockByStart.find(target->Address);
      if (trueBlockIt == Program.BlockByStart.end()) {
        errorMessage = "CBRANCH target block is unknown: " + target->Address;
        return false;
      }
      if (block.Out.empty()) {
        errorMessage = "CBRANCH block has no false successor";
        return false;
      }
      llvm::BasicBlock *trueBlock = BlockMap.at(trueBlockIt->second->Id);
      llvm::BasicBlock *falseBlock = nullptr;
      for (const std::string &successor : block.Out) {
        if (successor != trueBlockIt->second->Id) {
          falseBlock = BlockMap.at(successor);
          break;
        }
      }
      if (falseBlock == nullptr) {
        errorMessage = "CBRANCH false successor is unknown";
        return false;
      }
      llvm::Value *condition = read(op.Inputs[1]);
      if (!condition->getType()->isIntegerTy(1)) {
        condition = Builder.CreateICmpNE(
            condition, llvm::ConstantInt::get(condition->getType(), 0));
      }
      Builder.CreateCondBr(condition, trueBlock, falseBlock);
      return true;
    }

    if (op.Mnemonic == "RETURN") {
      llvm::Value *value = returnValueFor(op, errorMessage);
      if (!errorMessage.empty()) {
        return false;
      }
      if (value == nullptr) {
        Builder.CreateRetVoid();
      } else {
        Builder.CreateRet(value);
      }
      return true;
    }

    errorMessage = "unsupported terminator: " + op.Mnemonic;
    return false;
  }

  bool lowerOp(const HeritageOp &op, std::string &errorMessage) {
    if (op.Mnemonic == "COPY") {
      return lowerCopy(op, errorMessage);
    }
    if (op.Mnemonic == "INT_ADD") {
      return lowerIntAdd(op, errorMessage);
    }
    if (op.Mnemonic == "INT_SLESSEQUAL" || op.Mnemonic == "INT_SLESS" ||
        op.Mnemonic == "INT_EQUAL") {
      return lowerCompare(op, errorMessage);
    }
    if (op.Mnemonic == "MULTIEQUAL") {
      return lowerPhi(op, errorMessage);
    }
    if (op.Mnemonic == "CALL") {
      return lowerCall(op, errorMessage);
    }

    errorMessage = "unsupported heritage opcode: " + op.Mnemonic;
    return false;
  }

  bool lowerBlock(const HeritageBlock &block, std::string &errorMessage) {
    for (const std::string &opId : block.Ops) {
      const HeritageOp *op = Program.OpById.at(opId);
      if (op->Mnemonic == "CBRANCH" || op->Mnemonic == "RETURN") {
        return lowerBranch(*op, block, errorMessage);
      }
      if (!lowerOp(*op, errorMessage)) {
        return false;
      }
    }

    if (!block.Out.empty()) {
      Builder.CreateBr(BlockMap.at(block.Out.front()));
    } else if (Function->getReturnType()->isVoidTy()) {
      Builder.CreateRetVoid();
    } else {
      Builder.CreateRet(llvm::PoisonValue::get(Function->getReturnType()));
    }
    return true;
  }

  llvm::LLVMContext &Context;
  llvm::Module &Module;
  const HeritageProgram &Program;
  llvm::IRBuilder<> Builder;
  llvm::Function *Function = nullptr;
  std::unordered_map<std::string, llvm::BasicBlock *> BlockMap;
  std::unordered_map<std::string, llvm::Value *> Values;
};

} // namespace

std::unique_ptr<llvm::Module>
buildHeritageModule(llvm::LLVMContext &context, const HeritageProgram &program,
                    const HeritageLoweringConfig &config,
                    std::string &errorMessage) {
  auto module = std::make_unique<llvm::Module>(config.ModuleName, context);
  HeritageLowerer lowerer(context, *module, program);
  if (!lowerer.lower(errorMessage)) {
    return nullptr;
  }
  return module;
}

} // namespace notdec::bin2llvm
