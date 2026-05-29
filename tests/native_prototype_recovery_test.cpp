#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/passes/NativePrototypeRecovery.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

llvm::GlobalVariable *createRegisterGlobal(llvm::Module &module,
                                           const std::string &name) {
  llvm::LLVMContext &context = module.getContext();
  auto *type = llvm::Type::getInt64Ty(context);
  auto *global = new llvm::GlobalVariable(
      module, type, false, llvm::GlobalValue::ExternalLinkage, nullptr, name);
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "space=register"),
      llvm::MDString::get(context, "offset=0"),
      llvm::MDString::get(context, "size=8"),
      llvm::MDString::get(context, "name=" + name),
  };
  global->setMetadata("notdec.register", llvm::MDNode::get(context, fields));
  return global;
}

notdec::bin2llvm::NativeAbiParamEntry inputRegister(const std::string &name) {
  notdec::bin2llvm::NativeAbiParamEntry entry;
  entry.MinSize = 1;
  entry.MaxSize = 8;
  entry.Align = 8;
  entry.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  entry.Storage.Name = name;
  return entry;
}

llvm::MDNode *registerAccessMetadata(llvm::LLVMContext &context,
                                     const std::string &name) {
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "base=" + name),
      llvm::MDString::get(context, "space=register"),
      llvm::MDString::get(context, "offset=0"),
      llvm::MDString::get(context, "size=8"),
      llvm::MDString::get(context, "name=" + name),
  };
  return llvm::MDNode::get(context, fields);
}

void attachTestAbi(llvm::Module &module) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__stdcall";
  abi.Inputs.push_back(inputRegister("RDI"));
  abi.Inputs.push_back(inputRegister("RSI"));

  notdec::bin2llvm::NativeAbiParamEntry output;
  output.MinSize = 1;
  output.MaxSize = 8;
  output.Align = 8;
  output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  output.Storage.Name = "RAX";
  abi.Outputs.push_back(std::move(output));

  notdec::bin2llvm::NativeAbiParamEntry secondOutput;
  secondOutput.MinSize = 1;
  secondOutput.MaxSize = 8;
  secondOutput.Align = 8;
  secondOutput.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  secondOutput.Storage.Name = "RDX";
  abi.Outputs.push_back(std::move(secondOutput));

  notdec::bin2llvm::NativeAbiEffect unaffected;
  unaffected.Kind = notdec::bin2llvm::NativeAbiEffectKind::Unaffected;
  unaffected.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  unaffected.Storage.Name = "RBX";
  abi.Effects.push_back(std::move(unaffected));

  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
}

llvm::Function *createFunction(llvm::Module &module, const std::string &name) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createFunctionWithType(llvm::Module &module,
                                       const std::string &name,
                                       llvm::FunctionType *funcType) {
  llvm::LLVMContext &context = module.getContext();
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createCallerFunction(llvm::Module &module,
                                     const std::string &name,
                                     llvm::Function *callee) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createReturnLoadCallerFunction(llvm::Module &module,
                                               const std::string &name,
                                               llvm::Function *callee,
                                               llvm::GlobalVariable *output,
                                               const std::string &registerName,
                                               llvm::LoadInst **loadOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(callee->getFunctionType(), callee);
  llvm::LoadInst *load = builder.CreateLoad(output->getValueType(), output,
                                            registerName + ".return_value");
  load->setMetadata("notdec.register.access",
                    registerAccessMetadata(context, registerName));
  builder.CreateAdd(load, llvm::ConstantInt::get(output->getValueType(), 1));
  builder.CreateRetVoid();
  *loadOut = load;
  return function;
}

llvm::Function *createReturnLoadUniqueSuccessorCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *output, const std::string &registerName,
    llvm::LoadInst **loadOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *useBlock =
      llvm::BasicBlock::Create(context, "use_return", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateBr(useBlock);

  builder.SetInsertPoint(useBlock);
  llvm::LoadInst *load = builder.CreateLoad(output->getValueType(), output,
                                            registerName + ".return_value");
  load->setMetadata("notdec.register.access",
                    registerAccessMetadata(context, registerName));
  builder.CreateAdd(load, llvm::ConstantInt::get(output->getValueType(), 1));
  builder.CreateRetVoid();
  *loadOut = load;
  return function;
}

llvm::Function *createReturnLoadLinearSuccessorCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *output, const std::string &registerName,
    llvm::LoadInst **loadOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *middle =
      llvm::BasicBlock::Create(context, "after_call", function);
  llvm::BasicBlock *useBlock =
      llvm::BasicBlock::Create(context, "use_return", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateBr(middle);

  builder.SetInsertPoint(middle);
  builder.CreateBr(useBlock);

  builder.SetInsertPoint(useBlock);
  llvm::LoadInst *load = builder.CreateLoad(output->getValueType(), output,
                                            registerName + ".return_value");
  load->setMetadata("notdec.register.access",
                    registerAccessMetadata(context, registerName));
  builder.CreateAdd(load, llvm::ConstantInt::get(output->getValueType(), 1));
  builder.CreateRetVoid();
  *loadOut = load;
  return function;
}

llvm::Function *createReturnClobberLinearSuccessorCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *output, const std::string &registerName,
    llvm::LoadInst **loadOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *middle =
      llvm::BasicBlock::Create(context, "after_call", function);
  llvm::BasicBlock *useBlock =
      llvm::BasicBlock::Create(context, "use_return", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateBr(middle);

  builder.SetInsertPoint(middle);
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(output->getValueType(), 0x9999), output);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, registerName));
  builder.CreateBr(useBlock);

  builder.SetInsertPoint(useBlock);
  llvm::LoadInst *load = builder.CreateLoad(output->getValueType(), output,
                                            registerName + ".return_value");
  load->setMetadata("notdec.register.access",
                    registerAccessMetadata(context, registerName));
  builder.CreateAdd(load, llvm::ConstantInt::get(output->getValueType(), 1));
  builder.CreateRetVoid();
  *loadOut = load;
  return function;
}

llvm::Function *createReturnLoadMultiSuccessorCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *output, const std::string &registerName,
    llvm::LoadInst **loadOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *useBlock =
      llvm::BasicBlock::Create(context, "use_return", function);
  llvm::BasicBlock *skipBlock =
      llvm::BasicBlock::Create(context, "skip_return", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), useBlock, skipBlock);

  builder.SetInsertPoint(useBlock);
  llvm::LoadInst *load = builder.CreateLoad(output->getValueType(), output,
                                            registerName + ".return_value");
  load->setMetadata("notdec.register.access",
                    registerAccessMetadata(context, registerName));
  builder.CreateAdd(load, llvm::ConstantInt::get(output->getValueType(), 1));
  builder.CreateRetVoid();

  builder.SetInsertPoint(skipBlock);
  builder.CreateRetVoid();
  *loadOut = load;
  return function;
}

llvm::Function *createReturnLoadMultiPredecessorCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *output, const std::string &registerName,
    llvm::LoadInst **loadOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *callBlock =
      llvm::BasicBlock::Create(context, "call", function);
  llvm::BasicBlock *otherBlock =
      llvm::BasicBlock::Create(context, "other_pred", function);
  llvm::BasicBlock *useBlock =
      llvm::BasicBlock::Create(context, "use_return", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), callBlock,
                       otherBlock);

  builder.SetInsertPoint(callBlock);
  builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateBr(useBlock);

  builder.SetInsertPoint(otherBlock);
  builder.CreateBr(useBlock);

  builder.SetInsertPoint(useBlock);
  llvm::LoadInst *load = builder.CreateLoad(output->getValueType(), output,
                                            registerName + ".return_value");
  load->setMetadata("notdec.register.access",
                    registerAccessMetadata(context, registerName));
  builder.CreateAdd(load, llvm::ConstantInt::get(output->getValueType(), 1));
  builder.CreateRetVoid();
  *loadOut = load;
  return function;
}

llvm::Function *createReturnLoadLoopCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *loop = llvm::BasicBlock::Create(context, "loop", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateBr(loop);

  builder.SetInsertPoint(loop);
  builder.CreateBr(loop);
  return function;
}

llvm::Function *createInputStoreCallerFunction(llvm::Module &module,
                                               const std::string &name,
                                               llvm::Function *callee,
                                               llvm::GlobalVariable *input,
                                               const std::string &registerName,
                                               llvm::CallInst **callOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *argument = llvm::ConstantInt::get(input->getValueType(), 0x4567);
  llvm::StoreInst *store = builder.CreateStore(argument, input);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, registerName));
  llvm::CallInst *call = builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateRetVoid();
  *callOut = call;
  return function;
}

llvm::Function *createInputStoreUniquePredecessorCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *input, const std::string &registerName,
    llvm::CallInst **callOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *callBlock =
      llvm::BasicBlock::Create(context, "call", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *argument = llvm::ConstantInt::get(input->getValueType(), 0x6789);
  llvm::StoreInst *store = builder.CreateStore(argument, input);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, registerName));
  builder.CreateBr(callBlock);

  builder.SetInsertPoint(callBlock);
  llvm::CallInst *call = builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateRetVoid();
  *callOut = call;
  return function;
}

llvm::Function *createInputStoreReturnLoadCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *input, const std::string &inputRegisterName,
    llvm::GlobalVariable *output, const std::string &outputRegisterName,
    llvm::CallInst **callOut, llvm::LoadInst **loadOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *argument = llvm::ConstantInt::get(input->getValueType(), 0x4567);
  llvm::StoreInst *store = builder.CreateStore(argument, input);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, inputRegisterName));
  llvm::CallInst *call = builder.CreateCall(callee->getFunctionType(), callee);
  llvm::LoadInst *load = builder.CreateLoad(output->getValueType(), output,
                                            outputRegisterName + ".return_value");
  load->setMetadata("notdec.register.access",
                    registerAccessMetadata(context, outputRegisterName));
  builder.CreateAdd(load, llvm::ConstantInt::get(output->getValueType(), 1));
  builder.CreateRetVoid();
  *callOut = call;
  *loadOut = load;
  return function;
}

llvm::Function *createUnusedExternalInputFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *global,
    const std::string &registerName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *load =
      builder.CreateLoad(global->getValueType(), global,
                         registerName + ".external_input");
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "name=" + registerName),
      llvm::ValueAsMetadata::get(global),
  };
  load->setMetadata("notdec.register.external_input",
                    llvm::MDNode::get(context, fields));
  builder.CreateRetVoid();
  return function;
}

llvm::LoadInst *createExternalInputLoad(llvm::IRBuilder<> &builder,
                                        llvm::GlobalVariable *global,
                                        const std::string &registerName) {
  llvm::LLVMContext &context = global->getContext();
  llvm::LoadInst *load =
      builder.CreateLoad(global->getValueType(), global,
                         registerName + ".external_input");
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "name=" + registerName),
      llvm::ValueAsMetadata::get(global),
  };
  load->setMetadata("notdec.register.external_input",
                    llvm::MDNode::get(context, fields));
  return load;
}

llvm::Function *createUsedExternalInputFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *global,
    const std::string &registerName, llvm::LoadInst **inputLoad) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *load = createExternalInputLoad(builder, global, registerName);
  builder.CreateAdd(load, llvm::ConstantInt::get(global->getValueType(), 1));
  builder.CreateRetVoid();
  *inputLoad = load;
  return function;
}

llvm::Function *createDuplicateExternalInputLoadFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *global,
    const std::string &registerName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *first = createExternalInputLoad(builder, global, registerName);
  llvm::LoadInst *second = createExternalInputLoad(builder, global, registerName);
  builder.CreateAdd(first, second);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createReturnStoreFunction(llvm::Module &module,
                                          const std::string &name,
                                          llvm::GlobalVariable *global,
                                          const std::string &registerName,
                                          llvm::StoreInst **returnStore =
                                              nullptr) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(global->getValueType(), 0x1234), global);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, registerName));
  builder.CreateRetVoid();
  if (returnStore != nullptr) {
    *returnStore = store;
  }
  return function;
}

llvm::Function *createInputReturnFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *input,
    const std::string &inputRegisterName, llvm::GlobalVariable *output,
    const std::string &outputRegisterName, llvm::LoadInst **inputLoad,
    llvm::StoreInst **returnStore) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *load =
      createExternalInputLoad(builder, input, inputRegisterName);
  llvm::Value *value =
      builder.CreateAdd(load, llvm::ConstantInt::get(input->getValueType(), 7));
  llvm::StoreInst *store = builder.CreateStore(value, output);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, outputRegisterName));
  builder.CreateRetVoid();
  *inputLoad = load;
  *returnStore = store;
  return function;
}

llvm::Function *createTwoReturnStoreFunction(llvm::Module &module,
                                             const std::string &name,
                                             llvm::GlobalVariable *global,
                                             const std::string &registerName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right = llvm::BasicBlock::Create(context, "right", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), left, right);

  builder.SetInsertPoint(left);
  llvm::StoreInst *leftStore = builder.CreateStore(
      llvm::ConstantInt::get(global->getValueType(), 0x1111), global);
  leftStore->setMetadata("notdec.register.access",
                         registerAccessMetadata(context, registerName));
  builder.CreateRetVoid();

  builder.SetInsertPoint(right);
  llvm::StoreInst *rightStore = builder.CreateStore(
      llvm::ConstantInt::get(global->getValueType(), 0x1111), global);
  rightStore->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, registerName));
  builder.CreateRetVoid();

  return function;
}

llvm::Function *createPartialReturnStoreFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *global,
    const std::string &registerName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *withReturn =
      llvm::BasicBlock::Create(context, "with_return", function);
  llvm::BasicBlock *withoutReturn =
      llvm::BasicBlock::Create(context, "without_return", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), withReturn,
                       withoutReturn);

  builder.SetInsertPoint(withReturn);
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(global->getValueType(), 0x3333), global);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, registerName));
  builder.CreateRetVoid();

  builder.SetInsertPoint(withoutReturn);
  builder.CreateRetVoid();

  return function;
}

llvm::Function *createUniquePredecessorReturnStoreFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *global,
    const std::string &registerName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *exit = llvm::BasicBlock::Create(context, "exit", function);

  llvm::IRBuilder<> builder(entry);
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(global->getValueType(), 0x4444), global);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, registerName));
  builder.CreateBr(exit);

  builder.SetInsertPoint(exit);
  builder.CreateRetVoid();

  return function;
}

llvm::Function *createConflictingReturnStoreFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *global,
    const std::string &registerName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right = llvm::BasicBlock::Create(context, "right", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), left, right);

  builder.SetInsertPoint(left);
  llvm::StoreInst *leftStore = builder.CreateStore(
      llvm::ConstantInt::get(global->getValueType(), 0x5555), global);
  leftStore->setMetadata("notdec.register.access",
                         registerAccessMetadata(context, registerName));
  builder.CreateRetVoid();

  builder.SetInsertPoint(right);
  llvm::StoreInst *rightStore = builder.CreateStore(
      llvm::ConstantInt::get(global->getValueType(), 0x6666), global);
  rightStore->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, registerName));
  builder.CreateRetVoid();

  return function;
}

llvm::Function *createTwoOutputReturnStoreFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *first,
    const std::string &firstRegisterName, llvm::GlobalVariable *second,
    const std::string &secondRegisterName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);

  llvm::StoreInst *firstStore = builder.CreateStore(
      llvm::ConstantInt::get(first->getValueType(), 0x7777), first);
  firstStore->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, firstRegisterName));

  llvm::StoreInst *secondStore = builder.CreateStore(
      llvm::ConstantInt::get(second->getValueType(), 0x8888), second);
  secondStore->setMetadata("notdec.register.access",
                           registerAccessMetadata(context, secondRegisterName));
  builder.CreateRetVoid();
  return function;
}

void attachExternalInputs(llvm::Function &function,
                          llvm::ArrayRef<std::pair<std::string,
                                                   llvm::GlobalVariable *>>
                              inputs) {
  llvm::LLVMContext &context = function.getContext();
  std::vector<llvm::Metadata *> entries;
  for (const auto &[name, global] : inputs) {
    llvm::Metadata *fields[] = {
        llvm::MDString::get(context, "name=" + name),
        llvm::ValueAsMetadata::get(global),
    };
    entries.push_back(llvm::MDNode::get(context, fields));
  }
  function.setMetadata("notdec.register.external_inputs",
                       llvm::MDNode::get(context, entries));
}

bool metadataHasRegister(const llvm::Function &function, llvm::StringRef kind,
                         llvm::StringRef name) {
  llvm::MDNode *node = function.getMetadata(kind);
  if (node == nullptr) {
    return false;
  }
  std::string prefix = ("name=" + name).str();
  for (const llvm::MDOperand &operand : node->operands()) {
    auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(operand.get());
    if (entry == nullptr) {
      continue;
    }
    for (const llvm::MDOperand &fieldOperand : entry->operands()) {
      auto *field = llvm::dyn_cast_or_null<llvm::MDString>(fieldOperand.get());
      if (field != nullptr && field->getString() == prefix) {
        return true;
      }
    }
  }
  return false;
}

bool metadataRegisterAt(const llvm::Function &function, llvm::StringRef kind,
                        uint64_t index, llvm::StringRef name) {
  llvm::MDNode *node = function.getMetadata(kind);
  if (node == nullptr || index >= node->getNumOperands()) {
    return false;
  }
  auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(node->getOperand(index));
  if (entry == nullptr) {
    return false;
  }
  std::string prefix = ("name=" + name).str();
  for (const llvm::MDOperand &fieldOperand : entry->operands()) {
    auto *field = llvm::dyn_cast_or_null<llvm::MDString>(fieldOperand.get());
    if (field != nullptr && field->getString() == prefix) {
      return true;
    }
  }
  return false;
}

uint64_t countMetadataRegister(const llvm::Function &function,
                               llvm::StringRef kind, llvm::StringRef name) {
  llvm::MDNode *node = function.getMetadata(kind);
  if (node == nullptr) {
    return 0;
  }
  uint64_t count = 0;
  std::string prefix = ("name=" + name).str();
  for (const llvm::MDOperand &operand : node->operands()) {
    auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(operand.get());
    if (entry == nullptr) {
      continue;
    }
    for (const llvm::MDOperand &fieldOperand : entry->operands()) {
      auto *field = llvm::dyn_cast_or_null<llvm::MDString>(fieldOperand.get());
      if (field != nullptr && field->getString() == prefix) {
        ++count;
      }
    }
  }
  return count;
}

bool recoveredHasField(const llvm::Function &function, llvm::StringRef field) {
  llvm::MDNode *node = function.getMetadata("notdec.prototype.recovered");
  if (node == nullptr) {
    return false;
  }
  for (const llvm::MDOperand &operand : node->operands()) {
    auto *metadata = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
    if (metadata != nullptr && metadata->getString() == field) {
      return true;
    }
  }
  return false;
}

bool recoveredRegisterAt(const llvm::Function &function, uint64_t listIndex,
                         uint64_t paramIndex, llvm::StringRef name) {
  llvm::MDNode *node = function.getMetadata("notdec.prototype.recovered");
  if (node == nullptr || listIndex >= node->getNumOperands()) {
    return false;
  }
  auto *list = llvm::dyn_cast_or_null<llvm::MDNode>(node->getOperand(listIndex));
  if (list == nullptr || paramIndex >= list->getNumOperands()) {
    return false;
  }
  auto *param = llvm::dyn_cast_or_null<llvm::MDNode>(list->getOperand(paramIndex));
  if (param == nullptr) {
    return false;
  }

  std::string prefix = ("name=" + name).str();
  for (const llvm::MDOperand &fieldOperand : param->operands()) {
    auto *field = llvm::dyn_cast_or_null<llvm::MDString>(fieldOperand.get());
    if (field != nullptr && field->getString() == prefix) {
      return true;
    }
  }
  return false;
}

bool recoveredPrototypeParamAt(
    const std::vector<notdec::bin2llvm::NativeRecoveredPrototypeParam> &params,
    uint64_t index, llvm::StringRef name) {
  return index < params.size() && params[index].RegisterName == name;
}

bool functionTypeShape(llvm::FunctionType &type, llvm::Type *returnType,
                       llvm::ArrayRef<llvm::Type *> params) {
  if (type.getReturnType() != returnType || type.getNumParams() != params.size()) {
    return false;
  }
  for (unsigned index = 0; index < params.size(); ++index) {
    if (type.getParamType(index) != params[index]) {
      return false;
    }
  }
  return true;
}

bool expect(bool condition, const std::string &message) {
  if (condition) {
    return true;
  }
  std::cerr << message << '\n';
  return false;
}

} // namespace

int main() {
  llvm::LLVMContext context;
  llvm::Module module("native-prototype-recovery-test", context);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  attachTestAbi(module);

  llvm::Function *inputFunction = createFunction(module, "input_rdi");
  attachExternalInputs(*inputFunction, {{"RDI", rdi}});

  llvm::LoadInst *bindableInputLoad = nullptr;
  llvm::Function *bindableInputFunction =
      createUsedExternalInputFunction(module, "input_rdi_bindable", rdi, "RDI",
                                      &bindableInputLoad);
  attachExternalInputs(*bindableInputFunction, {{"RDI", rdi}});

  llvm::LoadInst *usedBindableInputLoad = nullptr;
  llvm::Function *usedBindableInputFunction =
      createUsedExternalInputFunction(module, "input_rdi_bindable_used", rdi,
                                      "RDI", &usedBindableInputLoad);
  attachExternalInputs(*usedBindableInputFunction, {{"RDI", rdi}});
  createCallerFunction(module, "call_input_rdi_bindable_used",
                       usedBindableInputFunction);

  llvm::Function *duplicateInputLoadFunction =
      createDuplicateExternalInputLoadFunction(module, "input_rdi_duplicate_load",
                                               rdi, "RDI");
  attachExternalInputs(*duplicateInputLoadFunction, {{"RDI", rdi}});

  llvm::FunctionType *matchingInputType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *matchingInputFunction =
      createFunctionWithType(module, "input_rdi_already_typed",
                             matchingInputType);
  attachExternalInputs(*matchingInputFunction, {{"RDI", rdi}});

  llvm::Function *reversedInputFunction =
      createFunction(module, "input_reversed");
  attachExternalInputs(*reversedInputFunction, {{"RSI", rdi}, {"RDI", rdi}});

  llvm::Function *duplicateInputFunction =
      createFunction(module, "input_duplicate");
  attachExternalInputs(*duplicateInputFunction, {{"RDI", rdi}, {"RDI", rdi}});

  llvm::Function *unusedInputFunction =
      createUnusedExternalInputFunction(module, "unused_rdi", rdi, "RDI");
  attachExternalInputs(*unusedInputFunction, {{"RDI", rdi}});

  llvm::Function *savedRegisterFunction = createFunction(module, "saved_rbx");
  attachExternalInputs(*savedRegisterFunction, {{"RBX", rbx}});
  llvm::StoreInst *returnStore = nullptr;
  llvm::Function *returnFunction =
      createReturnStoreFunction(module, "return_rax", rax, "RAX",
                                &returnStore);
  llvm::Function *usedReturnFunction =
      createReturnStoreFunction(module, "return_rax_used", rax, "RAX");
  createCallerFunction(module, "call_return_rax_used", usedReturnFunction);
  llvm::Function *nonReturnFunction =
      createReturnStoreFunction(module, "return_rbx", rbx, "RBX");
  llvm::Function *twoReturnFunction =
      createTwoReturnStoreFunction(module, "return_rax_twice", rax, "RAX");
  llvm::Function *partialReturnFunction =
      createPartialReturnStoreFunction(module, "return_rax_partial", rax,
                                       "RAX");
  llvm::Function *uniquePredReturnFunction =
      createUniquePredecessorReturnStoreFunction(module,
                                                 "return_rax_unique_pred", rax,
                                                 "RAX");
  llvm::Function *conflictingReturnFunction =
      createConflictingReturnStoreFunction(module, "return_rax_conflict", rax,
                                           "RAX");
  llvm::Function *twoOutputReturnFunction =
      createTwoOutputReturnStoreFunction(module, "return_rdx_rax_order", rdx,
                                         "RDX", rax, "RAX");
  llvm::LoadInst *inputReturnLoad = nullptr;
  llvm::StoreInst *inputReturnStore = nullptr;
  llvm::Function *inputReturnFunction = createInputReturnFunction(
      module, "input_rdi_return_rax", rdi, "RDI", rax, "RAX",
      &inputReturnLoad, &inputReturnStore);
  attachExternalInputs(*inputReturnFunction, {{"RDI", rdi}});
  llvm::LoadInst *dispatchInputLoad = nullptr;
  llvm::Function *dispatchInputFunction = createUsedExternalInputFunction(
      module, "dispatch_input_rdi", rdi, "RDI", &dispatchInputLoad);
  attachExternalInputs(*dispatchInputFunction, {{"RDI", rdi}});
  llvm::StoreInst *dispatchReturnStore = nullptr;
  llvm::Function *dispatchReturnFunction = createReturnStoreFunction(
      module, "dispatch_return_rax", rax, "RAX", &dispatchReturnStore);
  llvm::LoadInst *dispatchInputReturnLoad = nullptr;
  llvm::StoreInst *dispatchInputReturnStore = nullptr;
  llvm::Function *dispatchInputReturnFunction = createInputReturnFunction(
      module, "dispatch_input_rdi_return_rax", rdi, "RDI", rax, "RAX",
      &dispatchInputReturnLoad, &dispatchInputReturnStore);
  attachExternalInputs(*dispatchInputReturnFunction, {{"RDI", rdi}});

  notdec::bin2llvm::NativePrototypeRecoveryOptions options;
  notdec::bin2llvm::NativePrototypeRecoverySummary summary =
      notdec::bin2llvm::runNativePrototypeRecovery(module, options);

  if (llvm::verifyModule(module, &llvm::errs())) {
    std::cerr << "module verification failed after prototype recovery\n";
    return EXIT_FAILURE;
  }

  bool ok = true;
  ok &= expect(summary.FunctionsSeen == 23, "unexpected function count");
  ok &= expect(summary.ExternalInputsSeen == 14,
               "unexpected external input count");
  ok &= expect(summary.InputCandidates == 11,
               "unexpected input candidate count");
  ok &= expect(summary.ReturnCandidates == 9,
               "unexpected return candidate count");
  ok &= expect(summary.RewriteEligibleFunctions == 16,
               "unexpected rewrite eligible function count");
  ok &= expect(summary.SignatureRewriteNeededFunctions == 15,
               "unexpected signature rewrite needed function count");
  ok &= expect(summary.SignatureRewriteFunctionsSeen == 0,
               "default recovery unexpectedly ran signature rewrite");
  ok &= expect(summary.SignatureRewriteFunctionsRewritten == 0,
               "default recovery unexpectedly rewrote signatures");
  ok &= expect(summary.SignatureRewriteFunctionsSkipped == 0,
               "default recovery unexpectedly skipped signature rewrites");
  ok &= expect(metadataHasRegister(*inputFunction,
                                   "notdec.prototype.input_candidates", "RDI"),
               "RDI was not marked as an input candidate");
  ok &= expect(metadataRegisterAt(*reversedInputFunction,
                                  "notdec.prototype.input_candidates", 0,
                                  "RDI"),
               "RDI input candidate was not sorted before RSI");
  ok &= expect(metadataRegisterAt(*reversedInputFunction,
                                  "notdec.prototype.input_candidates", 1,
                                  "RSI"),
               "RSI input candidate was not sorted after RDI");
  ok &= expect(countMetadataRegister(*duplicateInputFunction,
                                     "notdec.prototype.input_candidates",
                                     "RDI") == 1,
               "duplicate RDI input candidate was not deduplicated");
  ok &= expect(!metadataHasRegister(*unusedInputFunction,
                                    "notdec.prototype.input_candidates", "RDI"),
               "unused RDI was incorrectly marked as an input candidate");
  ok &= expect(!metadataHasRegister(*savedRegisterFunction,
                                    "notdec.prototype.input_candidates", "RBX"),
               "RBX was incorrectly marked as an input candidate");
  ok &= expect(metadataHasRegister(*returnFunction,
                                   "notdec.prototype.return_candidates", "RAX"),
               "RAX was not marked as a return candidate");
  ok &= expect(!metadataHasRegister(*nonReturnFunction,
                                    "notdec.prototype.return_candidates", "RBX"),
               "RBX was incorrectly marked as a return candidate");
  ok &= expect(countMetadataRegister(*twoReturnFunction,
                                     "notdec.prototype.return_candidates",
                                     "RAX") == 1,
               "RAX return candidate was not deduplicated");
  ok &= expect(!metadataHasRegister(*partialReturnFunction,
                                    "notdec.prototype.return_candidates",
                                    "RAX"),
               "partial RAX return was incorrectly marked as a candidate");
  ok &= expect(metadataHasRegister(*uniquePredReturnFunction,
                                   "notdec.prototype.return_candidates", "RAX"),
               "unique predecessor RAX return was not marked as a candidate");
  ok &= expect(!metadataHasRegister(*conflictingReturnFunction,
                                    "notdec.prototype.return_candidates",
                                    "RAX"),
               "conflicting RAX return was incorrectly marked as a candidate");
  ok &= expect(metadataRegisterAt(*twoOutputReturnFunction,
                                  "notdec.prototype.return_candidates", 0,
                                  "RAX"),
               "RAX return candidate was not sorted before RDX");
  ok &= expect(metadataRegisterAt(*twoOutputReturnFunction,
                                  "notdec.prototype.return_candidates", 1,
                                  "RDX"),
               "RDX return candidate was not sorted after RAX");
  ok &= expect(recoveredHasField(*inputFunction, "model=__stdcall"),
               "recovered prototype model was not written");
  ok &= expect(recoveredHasField(*inputFunction, "input_count=1"),
               "recovered prototype input count was not written");
  ok &= expect(recoveredHasField(*inputFunction, "return_count=0"),
               "recovered prototype return count was not written");
  ok &= expect(recoveredRegisterAt(*inputFunction, 3, 0, "RDI"),
               "recovered prototype input register was not written");
  std::optional<notdec::bin2llvm::NativeRecoveredPrototype> inputPrototype =
      notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(*inputFunction);
  ok &= expect(inputPrototype.has_value(),
               "recovered input prototype was not readable");
  if (inputPrototype) {
    ok &= expect(inputPrototype->ModelName == "__stdcall",
                 "recovered input prototype model was not read");
    ok &= expect(inputPrototype->Inputs.size() == 1,
                 "recovered input prototype input count was not read");
    ok &= expect(inputPrototype->Returns.empty(),
                 "recovered input prototype return count was not read");
    ok &= expect(recoveredPrototypeParamAt(inputPrototype->Inputs, 0, "RDI"),
                 "recovered input prototype register was not read");
    std::optional<llvm::FunctionType *> type =
        notdec::bin2llvm::buildNativeRecoveredPrototypeFunctionType(
            context, *inputPrototype);
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    llvm::Type *voidType = llvm::Type::getVoidTy(context);
    ok &= expect(type.has_value() &&
                     functionTypeShape(**type, voidType, llvm::ArrayRef(i64)),
                 "recovered input prototype type was not void(i64)");
    notdec::bin2llvm::NativePrototypeRewriteEligibility eligibility =
        notdec::bin2llvm::getNativePrototypeRewriteEligibility(*inputFunction);
    ok &= expect(eligibility.Eligible,
                 "input prototype was not marked rewrite eligible");
    ok &= expect(eligibility.NeedsRewrite,
                 "input prototype did not request signature rewrite");
    ok &= expect(eligibility.RecoveredType == *type,
                 "input prototype rewrite type did not match recovered type");
  }
  std::optional<std::vector<notdec::bin2llvm::NativePrototypeInputBinding>>
      inputBindings =
          notdec::bin2llvm::getNativePrototypeInputBindings(
              *bindableInputFunction);
  ok &= expect(inputBindings.has_value(),
               "bindable input prototype had no input bindings");
  if (inputBindings) {
    ok &= expect(inputBindings->size() == 1,
                 "bindable input prototype had unexpected binding count");
    ok &= expect((*inputBindings)[0].Param.RegisterName == "RDI",
                 "bindable input binding used wrong register");
    ok &= expect((*inputBindings)[0].Param.Slot == 0,
                 "bindable input binding used wrong slot");
    ok &= expect((*inputBindings)[0].ExternalInputLoad == bindableInputLoad,
                 "bindable input binding used wrong load");
  }
  ok &= expect(!notdec::bin2llvm::getNativePrototypeInputBindings(
                    *inputFunction),
               "input prototype without external input load was bound");
  ok &= expect(!notdec::bin2llvm::getNativePrototypeInputBindings(
                    *duplicateInputLoadFunction),
               "duplicate external input loads were incorrectly bound");
  notdec::bin2llvm::NativePrototypeRewriteResult usedInputRewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputOnly(
          *usedBindableInputFunction);
  ok &= expect(!usedInputRewriteResult.Rewritten,
               "input-only prototype with uses was rewritten");
  ok &= expect(usedInputRewriteResult.Reason == "unsafe callsite input value",
               "input-only prototype with uses had unexpected rewrite reason");
  llvm::Instruction *inputLoadUser =
      llvm::dyn_cast<llvm::Instruction>(*bindableInputLoad->user_begin());
  notdec::bin2llvm::NativePrototypeRewriteResult inputRewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputOnly(
          *bindableInputFunction);
  ok &= expect(inputRewriteResult.Rewritten,
               "input-only prototype was not rewritten");
  bindableInputFunction = inputRewriteResult.Function;
  llvm::Type *i64Param = llvm::Type::getInt64Ty(context);
  ok &= expect(bindableInputFunction != nullptr &&
                   functionTypeShape(*bindableInputFunction->getFunctionType(),
                                     llvm::Type::getVoidTy(context),
                                     llvm::ArrayRef(i64Param)),
               "rewritten input-only function type was not void(i64)");
  ok &= expect(bindableInputFunction != nullptr &&
                   !bindableInputFunction->arg_empty() &&
                   inputLoadUser != nullptr &&
                   inputLoadUser->getOperand(0) ==
                       &*bindableInputFunction->arg_begin(),
               "rewritten input-only function did not use new argument");
  bool sawInputLoadAfterRewrite = false;
  if (bindableInputFunction != nullptr) {
    for (llvm::BasicBlock &block : *bindableInputFunction) {
      for (llvm::Instruction &instruction : block) {
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        if (load != nullptr &&
            load->getMetadata("notdec.register.external_input") != nullptr) {
          sawInputLoadAfterRewrite = true;
        }
      }
    }
  }
  ok &= expect(!sawInputLoadAfterRewrite,
               "rewritten input-only function kept old input load");

  llvm::Module callsiteModule("native-prototype-input-callsite-rewrite-test",
                              context);
  llvm::GlobalVariable *callsiteRdi =
      createRegisterGlobal(callsiteModule, "RDI");
  attachTestAbi(callsiteModule);
  llvm::LoadInst *callsiteInputLoad = nullptr;
  llvm::Function *callsiteInputFunction = createUsedExternalInputFunction(
      callsiteModule, "callsite_input_rdi", callsiteRdi, "RDI",
      &callsiteInputLoad);
  attachExternalInputs(*callsiteInputFunction, {{"RDI", callsiteRdi}});
  llvm::CallInst *oldCallsiteCall = nullptr;
  createInputStoreCallerFunction(callsiteModule, "call_callsite_input_rdi",
                                 callsiteInputFunction, callsiteRdi, "RDI",
                                 &oldCallsiteCall);
  notdec::bin2llvm::runNativePrototypeRecovery(callsiteModule, options);
  notdec::bin2llvm::NativePrototypeRewriteResult callsiteRewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputOnly(
          *callsiteInputFunction);
  ok &= expect(callsiteRewriteResult.Rewritten,
               "input-only prototype with direct callsite was not rewritten");
  callsiteInputFunction = callsiteRewriteResult.Function;
  ok &= expect(callsiteInputFunction != nullptr &&
                   functionTypeShape(*callsiteInputFunction->getFunctionType(),
                                     llvm::Type::getVoidTy(context),
                                     llvm::ArrayRef(i64Param)),
               "callsite rewritten input function type was not void(i64)");
  llvm::CallInst *rewrittenCallsiteCall = nullptr;
  llvm::Function *callsiteCaller =
      callsiteModule.getFunction("call_callsite_input_rdi");
  if (callsiteCaller != nullptr) {
    for (llvm::BasicBlock &block : *callsiteCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call != nullptr && call->getCalledFunction() ==
                                   callsiteInputFunction) {
          rewrittenCallsiteCall = call;
        }
      }
    }
  }
  ok &= expect(rewrittenCallsiteCall != nullptr,
               "direct callsite was not rewritten to new callee");
  ok &= expect(rewrittenCallsiteCall != nullptr &&
                   rewrittenCallsiteCall->arg_size() == 1,
               "direct callsite did not get one argument");
  ok &= expect(rewrittenCallsiteCall != nullptr &&
                   llvm::isa<llvm::ConstantInt>(
                       rewrittenCallsiteCall->getArgOperand(0)),
               "direct callsite argument did not use register store value");
  if (llvm::verifyModule(callsiteModule, &llvm::errs())) {
    std::cerr
        << "callsite module verification failed after input-only rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Module predecessorCallsiteModule(
      "native-prototype-input-predecessor-callsite-rewrite-test", context);
  llvm::GlobalVariable *predecessorCallsiteRdi =
      createRegisterGlobal(predecessorCallsiteModule, "RDI");
  attachTestAbi(predecessorCallsiteModule);
  llvm::LoadInst *predecessorCallsiteInputLoad = nullptr;
  llvm::Function *predecessorCallsiteInputFunction =
      createUsedExternalInputFunction(predecessorCallsiteModule,
                                      "predecessor_callsite_input_rdi",
                                      predecessorCallsiteRdi, "RDI",
                                      &predecessorCallsiteInputLoad);
  attachExternalInputs(*predecessorCallsiteInputFunction,
                       {{"RDI", predecessorCallsiteRdi}});
  llvm::CallInst *oldPredecessorCallsiteCall = nullptr;
  createInputStoreUniquePredecessorCallerFunction(
      predecessorCallsiteModule, "call_predecessor_callsite_input_rdi",
      predecessorCallsiteInputFunction, predecessorCallsiteRdi, "RDI",
      &oldPredecessorCallsiteCall);
  notdec::bin2llvm::runNativePrototypeRecovery(predecessorCallsiteModule,
                                               options);
  notdec::bin2llvm::NativePrototypeRewriteResult
      predecessorCallsiteRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputOnly(
              *predecessorCallsiteInputFunction);
  ok &= expect(predecessorCallsiteRewriteResult.Rewritten,
               "input-only prototype with predecessor callsite was not rewritten");
  predecessorCallsiteInputFunction = predecessorCallsiteRewriteResult.Function;
  llvm::CallInst *rewrittenPredecessorCallsiteCall = nullptr;
  llvm::Function *predecessorCallsiteCaller =
      predecessorCallsiteModule.getFunction("call_predecessor_callsite_input_rdi");
  if (predecessorCallsiteCaller != nullptr) {
    for (llvm::BasicBlock &block : *predecessorCallsiteCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call != nullptr && call->getCalledFunction() ==
                                   predecessorCallsiteInputFunction) {
          rewrittenPredecessorCallsiteCall = call;
        }
      }
    }
  }
  ok &= expect(rewrittenPredecessorCallsiteCall != nullptr,
               "predecessor callsite was not rewritten to new callee");
  ok &= expect(rewrittenPredecessorCallsiteCall != nullptr &&
                   rewrittenPredecessorCallsiteCall->arg_size() == 1,
               "predecessor callsite did not get one argument");
  ok &= expect(rewrittenPredecessorCallsiteCall != nullptr &&
                   llvm::isa<llvm::ConstantInt>(
                       rewrittenPredecessorCallsiteCall->getArgOperand(0)),
               "predecessor callsite argument did not use register store value");
  if (llvm::verifyModule(predecessorCallsiteModule, &llvm::errs())) {
    std::cerr << "predecessor callsite module verification failed after "
                 "input-only rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Module missingInputCallsiteModule(
      "native-prototype-input-missing-callsite-value-test", context);
  llvm::GlobalVariable *missingInputCallsiteRdi =
      createRegisterGlobal(missingInputCallsiteModule, "RDI");
  attachTestAbi(missingInputCallsiteModule);
  llvm::LoadInst *missingInputCallsiteInputLoad = nullptr;
  llvm::Function *missingInputCallsiteFunction =
      createUsedExternalInputFunction(missingInputCallsiteModule,
                                      "missing_callsite_input_rdi",
                                      missingInputCallsiteRdi, "RDI",
                                      &missingInputCallsiteInputLoad);
  attachExternalInputs(*missingInputCallsiteFunction,
                       {{"RDI", missingInputCallsiteRdi}});
  createCallerFunction(missingInputCallsiteModule,
                       "call_missing_callsite_input_rdi",
                       missingInputCallsiteFunction);
  notdec::bin2llvm::runNativePrototypeRecovery(missingInputCallsiteModule,
                                               options);
  notdec::bin2llvm::NativePrototypeRewriteResult
      missingInputCallsiteRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputOnly(
              *missingInputCallsiteFunction);
  ok &= expect(!missingInputCallsiteRewriteResult.Rewritten,
               "input-only prototype rewrite ignored missing callsite argument");
  ok &= expect(missingInputCallsiteRewriteResult.Reason ==
                   "unsafe callsite input value",
               "missing callsite input value had wrong skip reason");
  if (llvm::verifyModule(missingInputCallsiteModule, &llvm::errs())) {
    std::cerr << "missing input callsite module verification failed\n";
    return EXIT_FAILURE;
  }

  llvm::Module addressTakenInputModule(
      "native-prototype-input-address-taken-test", context);
  llvm::GlobalVariable *addressTakenInputRdi =
      createRegisterGlobal(addressTakenInputModule, "RDI");
  attachTestAbi(addressTakenInputModule);
  llvm::LoadInst *addressTakenInputLoad = nullptr;
  llvm::Function *addressTakenInputFunction =
      createUsedExternalInputFunction(addressTakenInputModule,
                                      "address_taken_input_rdi",
                                      addressTakenInputRdi, "RDI",
                                      &addressTakenInputLoad);
  attachExternalInputs(*addressTakenInputFunction,
                       {{"RDI", addressTakenInputRdi}});
  notdec::bin2llvm::runNativePrototypeRecovery(addressTakenInputModule,
                                               options);
  new llvm::GlobalVariable(
      addressTakenInputModule, addressTakenInputFunction->getType(), true,
      llvm::GlobalValue::ExternalLinkage, addressTakenInputFunction,
      "address_taken_input_rdi_ptr");
  notdec::bin2llvm::NativePrototypeRewriteResult addressTakenRewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputOnly(
          *addressTakenInputFunction);
  ok &= expect(!addressTakenRewriteResult.Rewritten,
               "address-taken input-only prototype was rewritten");
  ok &= expect(addressTakenRewriteResult.Reason == "function has uses",
               "address-taken input-only prototype had unexpected reason");
  if (llvm::verifyModule(addressTakenInputModule, &llvm::errs())) {
    std::cerr << "address-taken input module verification failed\n";
    return EXIT_FAILURE;
  }

  notdec::bin2llvm::NativePrototypeRewriteEligibility matchingEligibility =
      notdec::bin2llvm::getNativePrototypeRewriteEligibility(
          *matchingInputFunction);
  ok &= expect(matchingEligibility.Eligible,
               "matching input prototype was not rewrite eligible");
  ok &= expect(!matchingEligibility.NeedsRewrite,
               "matching input prototype incorrectly requested rewrite");
  ok &= expect(matchingEligibility.Reason == "already matches",
               "matching input prototype had unexpected eligibility reason");
  ok &= expect(recoveredHasField(*returnFunction, "input_count=0"),
               "return-only recovered prototype input count was not written");
  ok &= expect(recoveredHasField(*returnFunction, "return_count=1"),
               "return-only recovered prototype return count was not written");
  ok &= expect(recoveredRegisterAt(*returnFunction, 4, 0, "RAX"),
               "recovered prototype return register was not written");
  std::optional<notdec::bin2llvm::NativeRecoveredPrototype> returnPrototype =
      notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(*returnFunction);
  ok &= expect(returnPrototype.has_value(),
               "recovered return prototype was not readable");
  if (returnPrototype) {
    ok &= expect(returnPrototype->Inputs.empty(),
                 "recovered return prototype input count was not read");
    ok &= expect(returnPrototype->Returns.size() == 1,
                 "recovered return prototype return count was not read");
    ok &= expect(recoveredPrototypeParamAt(returnPrototype->Returns, 0, "RAX"),
                 "recovered return prototype register was not read");
    std::optional<llvm::FunctionType *> type =
        notdec::bin2llvm::buildNativeRecoveredPrototypeFunctionType(
            context, *returnPrototype);
    llvm::Type *i64 = llvm::Type::getInt64Ty(context);
    ok &= expect(type.has_value() &&
                     functionTypeShape(**type, i64, llvm::ArrayRef<llvm::Type *>{}),
                 "recovered return prototype type was not i64()");
    notdec::bin2llvm::NativePrototypeRewriteEligibility eligibility =
        notdec::bin2llvm::getNativePrototypeRewriteEligibility(*returnFunction);
    ok &= expect(eligibility.Eligible,
                 "return prototype was not marked rewrite eligible");
    ok &= expect(eligibility.NeedsRewrite,
                 "return prototype did not request signature rewrite");
    ok &= expect(eligibility.RecoveredType == *type,
                 "return prototype rewrite type did not match recovered type");
  }
  std::optional<std::vector<notdec::bin2llvm::NativePrototypeReturnBinding>>
      returnBindings =
          notdec::bin2llvm::getNativePrototypeReturnBindings(*returnFunction);
  ok &= expect(returnBindings.has_value(),
               "return prototype had no return bindings");
  if (returnBindings) {
    ok &= expect(returnBindings->size() == 1,
                 "return prototype had unexpected binding count");
    ok &= expect((*returnBindings)[0].Param.RegisterName == "RAX",
                 "return binding used wrong register");
    ok &= expect((*returnBindings)[0].Param.Slot == 0,
                 "return binding used wrong slot");
    ok &= expect((*returnBindings)[0].ReturnStore == returnStore,
                 "return binding used wrong store");
    ok &= expect((*returnBindings)[0].ReturnValue ==
                     returnStore->getValueOperand(),
                 "return binding used wrong value");
  }
  ok &= expect(!notdec::bin2llvm::getNativePrototypeReturnBindings(
                    *twoReturnFunction),
               "duplicate return stores were incorrectly bound");
  notdec::bin2llvm::NativePrototypeRewriteResult usedRewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(
          *usedReturnFunction);
  ok &= expect(usedRewriteResult.Rewritten,
               "return-only prototype with direct caller was not rewritten");
  usedReturnFunction = usedRewriteResult.Function;
  ok &= expect(usedReturnFunction != nullptr &&
                   functionTypeShape(*usedReturnFunction->getFunctionType(),
                                     llvm::Type::getInt64Ty(context),
                                     llvm::ArrayRef<llvm::Type *>{}),
               "return-only prototype with direct caller was not i64()");

  llvm::Module returnCallsiteModule(
      "native-prototype-return-callsite-rewrite-test", context);
  llvm::GlobalVariable *returnCallsiteRax =
      createRegisterGlobal(returnCallsiteModule, "RAX");
  attachTestAbi(returnCallsiteModule);
  llvm::StoreInst *returnCallsiteStore = nullptr;
  llvm::Function *returnCallsiteFunction = createReturnStoreFunction(
      returnCallsiteModule, "callsite_return_rax", returnCallsiteRax, "RAX",
      &returnCallsiteStore);
  llvm::LoadInst *returnCallsiteLoad = nullptr;
  createReturnLoadCallerFunction(returnCallsiteModule, "call_callsite_return_rax",
                                 returnCallsiteFunction, returnCallsiteRax,
                                 "RAX", &returnCallsiteLoad);
  notdec::bin2llvm::runNativePrototypeRecovery(returnCallsiteModule, options);
  notdec::bin2llvm::NativePrototypeRewriteResult returnCallsiteRewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(
          *returnCallsiteFunction);
  ok &= expect(returnCallsiteRewriteResult.Rewritten,
               "return-only prototype with direct callsite was not rewritten");
  returnCallsiteFunction = returnCallsiteRewriteResult.Function;
  ok &= expect(returnCallsiteFunction != nullptr &&
                   functionTypeShape(*returnCallsiteFunction->getFunctionType(),
                                     llvm::Type::getInt64Ty(context),
                                     llvm::ArrayRef<llvm::Type *>{}),
               "callsite rewritten return function type was not i64()");
  llvm::CallInst *rewrittenReturnCallsiteCall = nullptr;
  llvm::Function *returnCallsiteCaller =
      returnCallsiteModule.getFunction("call_callsite_return_rax");
  if (returnCallsiteCaller != nullptr) {
    for (llvm::BasicBlock &block : *returnCallsiteCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call != nullptr && call->getCalledFunction() ==
                                   returnCallsiteFunction) {
          rewrittenReturnCallsiteCall = call;
        }
      }
    }
  }
  ok &= expect(rewrittenReturnCallsiteCall != nullptr,
               "return direct callsite was not rewritten to new callee");
  ok &= expect(rewrittenReturnCallsiteCall != nullptr &&
                   rewrittenReturnCallsiteCall->arg_size() == 0,
               "return direct callsite got unexpected arguments");
  ok &= expect(rewrittenReturnCallsiteCall != nullptr &&
                   rewrittenReturnCallsiteCall->getType() ==
                       llvm::Type::getInt64Ty(context),
               "return direct callsite did not return i64");
  bool sawOldReturnLoad = false;
  if (returnCallsiteCaller != nullptr) {
    for (llvm::BasicBlock &block : *returnCallsiteCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        if (load != nullptr &&
            load->getMetadata("notdec.register.access") != nullptr) {
          sawOldReturnLoad = true;
        }
      }
    }
  }
  ok &= expect(!sawOldReturnLoad,
               "return direct callsite kept old return register load");
  ok &= expect(rewrittenReturnCallsiteCall != nullptr &&
                   !rewrittenReturnCallsiteCall->use_empty(),
               "return direct callsite result was not used");
  if (llvm::verifyModule(returnCallsiteModule, &llvm::errs())) {
    std::cerr
        << "callsite module verification failed after return-only rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Module successorReturnCallsiteModule(
      "native-prototype-return-successor-callsite-rewrite-test", context);
  llvm::GlobalVariable *successorReturnCallsiteRax =
      createRegisterGlobal(successorReturnCallsiteModule, "RAX");
  attachTestAbi(successorReturnCallsiteModule);
  llvm::StoreInst *successorReturnCallsiteStore = nullptr;
  llvm::Function *successorReturnCallsiteFunction = createReturnStoreFunction(
      successorReturnCallsiteModule, "successor_callsite_return_rax",
      successorReturnCallsiteRax, "RAX", &successorReturnCallsiteStore);
  llvm::LoadInst *successorReturnCallsiteLoad = nullptr;
  createReturnLoadUniqueSuccessorCallerFunction(
      successorReturnCallsiteModule, "call_successor_callsite_return_rax",
      successorReturnCallsiteFunction, successorReturnCallsiteRax, "RAX",
      &successorReturnCallsiteLoad);
  notdec::bin2llvm::runNativePrototypeRecovery(successorReturnCallsiteModule,
                                               options);
  notdec::bin2llvm::NativePrototypeRewriteResult
      successorReturnCallsiteRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(
              *successorReturnCallsiteFunction);
  ok &= expect(successorReturnCallsiteRewriteResult.Rewritten,
               "return-only prototype with successor callsite was not rewritten");
  successorReturnCallsiteFunction = successorReturnCallsiteRewriteResult.Function;
  llvm::CallInst *rewrittenSuccessorReturnCallsiteCall = nullptr;
  llvm::Function *successorReturnCallsiteCaller =
      successorReturnCallsiteModule.getFunction("call_successor_callsite_return_rax");
  if (successorReturnCallsiteCaller != nullptr) {
    for (llvm::BasicBlock &block : *successorReturnCallsiteCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call != nullptr && call->getCalledFunction() ==
                                   successorReturnCallsiteFunction) {
          rewrittenSuccessorReturnCallsiteCall = call;
        }
      }
    }
  }
  ok &= expect(rewrittenSuccessorReturnCallsiteCall != nullptr,
               "successor return callsite was not rewritten to new callee");
  bool sawOldSuccessorReturnLoad = false;
  if (successorReturnCallsiteCaller != nullptr) {
    for (llvm::BasicBlock &block : *successorReturnCallsiteCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        if (load != nullptr &&
            load->getMetadata("notdec.register.access") != nullptr) {
          sawOldSuccessorReturnLoad = true;
        }
      }
    }
  }
  ok &= expect(!sawOldSuccessorReturnLoad,
               "successor return callsite kept old return register load");
  ok &= expect(rewrittenSuccessorReturnCallsiteCall != nullptr &&
                   !rewrittenSuccessorReturnCallsiteCall->use_empty(),
               "successor return callsite result was not used");
  if (llvm::verifyModule(successorReturnCallsiteModule, &llvm::errs())) {
    std::cerr << "successor callsite module verification failed after "
                 "return-only rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Module linearReturnCallsiteModule(
      "native-prototype-return-linear-callsite-rewrite-test", context);
  llvm::GlobalVariable *linearReturnCallsiteRax =
      createRegisterGlobal(linearReturnCallsiteModule, "RAX");
  attachTestAbi(linearReturnCallsiteModule);
  llvm::StoreInst *linearReturnCallsiteStore = nullptr;
  llvm::Function *linearReturnCallsiteFunction = createReturnStoreFunction(
      linearReturnCallsiteModule, "linear_callsite_return_rax",
      linearReturnCallsiteRax, "RAX", &linearReturnCallsiteStore);
  llvm::LoadInst *linearReturnCallsiteLoad = nullptr;
  createReturnLoadLinearSuccessorCallerFunction(
      linearReturnCallsiteModule, "call_linear_callsite_return_rax",
      linearReturnCallsiteFunction, linearReturnCallsiteRax, "RAX",
      &linearReturnCallsiteLoad);
  notdec::bin2llvm::runNativePrototypeRecovery(linearReturnCallsiteModule,
                                               options);
  notdec::bin2llvm::NativePrototypeRewriteResult
      linearReturnCallsiteRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(
              *linearReturnCallsiteFunction);
  ok &= expect(linearReturnCallsiteRewriteResult.Rewritten,
               "return-only prototype with linear callsite was not rewritten");
  linearReturnCallsiteFunction = linearReturnCallsiteRewriteResult.Function;
  llvm::CallInst *rewrittenLinearReturnCallsiteCall = nullptr;
  llvm::Function *linearReturnCallsiteCaller =
      linearReturnCallsiteModule.getFunction("call_linear_callsite_return_rax");
  if (linearReturnCallsiteCaller != nullptr) {
    for (llvm::BasicBlock &block : *linearReturnCallsiteCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call != nullptr &&
            call->getCalledFunction() == linearReturnCallsiteFunction) {
          rewrittenLinearReturnCallsiteCall = call;
        }
      }
    }
  }
  ok &= expect(rewrittenLinearReturnCallsiteCall != nullptr,
               "linear return callsite was not rewritten to new callee");
  bool sawOldLinearReturnLoad = false;
  if (linearReturnCallsiteCaller != nullptr) {
    for (llvm::BasicBlock &block : *linearReturnCallsiteCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        if (load != nullptr &&
            load->getMetadata("notdec.register.access") != nullptr) {
          sawOldLinearReturnLoad = true;
        }
      }
    }
  }
  ok &= expect(!sawOldLinearReturnLoad,
               "linear return callsite kept old return register load");
  ok &= expect(rewrittenLinearReturnCallsiteCall != nullptr &&
                   !rewrittenLinearReturnCallsiteCall->use_empty(),
               "linear return callsite result was not used");
  if (llvm::verifyModule(linearReturnCallsiteModule, &llvm::errs())) {
    std::cerr << "linear callsite module verification failed after "
                 "return-only rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Module clobberReturnCallsiteModule(
      "native-prototype-return-clobber-callsite-rewrite-test", context);
  llvm::GlobalVariable *clobberReturnCallsiteRax =
      createRegisterGlobal(clobberReturnCallsiteModule, "RAX");
  attachTestAbi(clobberReturnCallsiteModule);
  llvm::StoreInst *clobberReturnCallsiteStore = nullptr;
  llvm::Function *clobberReturnCallsiteFunction = createReturnStoreFunction(
      clobberReturnCallsiteModule, "clobber_callsite_return_rax",
      clobberReturnCallsiteRax, "RAX", &clobberReturnCallsiteStore);
  llvm::LoadInst *clobberReturnCallsiteLoad = nullptr;
  createReturnClobberLinearSuccessorCallerFunction(
      clobberReturnCallsiteModule, "call_clobber_callsite_return_rax",
      clobberReturnCallsiteFunction, clobberReturnCallsiteRax, "RAX",
      &clobberReturnCallsiteLoad);
  notdec::bin2llvm::runNativePrototypeRecovery(clobberReturnCallsiteModule,
                                               options);
  notdec::bin2llvm::NativePrototypeRewriteResult
      clobberReturnCallsiteRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(
              *clobberReturnCallsiteFunction);
  ok &= expect(!clobberReturnCallsiteRewriteResult.Rewritten,
               "return-only prototype rewrite ignored callsite clobber");
  ok &= expect(clobberReturnCallsiteRewriteResult.Reason ==
                   "unsafe callsite return load",
               "clobbered return callsite had wrong skip reason");
  ok &= expect(!clobberReturnCallsiteLoad->use_empty(),
               "clobbered return load was unexpectedly replaced");
  if (llvm::verifyModule(clobberReturnCallsiteModule, &llvm::errs())) {
    std::cerr << "clobber callsite module verification failed after "
                 "return-only rewrite\n";
    return EXIT_FAILURE;
  }

  auto expectReturnOnlyRewriteRejected =
      [&](llvm::Module &module, llvm::Function &function,
          llvm::LoadInst *load, const char *message) {
        notdec::bin2llvm::runNativePrototypeRecovery(module, options);
        notdec::bin2llvm::NativePrototypeRewriteResult result =
            notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(function);
        ok &= expect(!result.Rewritten, message);
        ok &= expect(result.Reason == "unsafe callsite return load",
                     "unsafe return callsite had wrong skip reason");
        if (load != nullptr) {
          ok &= expect(!load->use_empty(),
                       "unsafe callsite return load was unexpectedly replaced");
        }
        if (llvm::verifyModule(module, &llvm::errs())) {
          std::cerr << "unsafe return callsite module verification failed\n";
          return false;
        }
        return true;
      };

  llvm::Module multiSuccessorReturnCallsiteModule(
      "native-prototype-return-multi-successor-callsite-test", context);
  llvm::GlobalVariable *multiSuccessorReturnCallsiteRax =
      createRegisterGlobal(multiSuccessorReturnCallsiteModule, "RAX");
  attachTestAbi(multiSuccessorReturnCallsiteModule);
  llvm::Function *multiSuccessorReturnCallsiteFunction =
      createReturnStoreFunction(multiSuccessorReturnCallsiteModule,
                                "multi_successor_callsite_return_rax",
                                multiSuccessorReturnCallsiteRax, "RAX");
  llvm::LoadInst *multiSuccessorReturnCallsiteLoad = nullptr;
  createReturnLoadMultiSuccessorCallerFunction(
      multiSuccessorReturnCallsiteModule,
      "call_multi_successor_callsite_return_rax",
      multiSuccessorReturnCallsiteFunction, multiSuccessorReturnCallsiteRax,
      "RAX", &multiSuccessorReturnCallsiteLoad);
  if (!expectReturnOnlyRewriteRejected(
          multiSuccessorReturnCallsiteModule,
          *multiSuccessorReturnCallsiteFunction,
          multiSuccessorReturnCallsiteLoad,
          "return-only prototype rewrite accepted multi-successor callsite")) {
    return EXIT_FAILURE;
  }

  llvm::Module multiPredecessorReturnCallsiteModule(
      "native-prototype-return-multi-predecessor-callsite-test", context);
  llvm::GlobalVariable *multiPredecessorReturnCallsiteRax =
      createRegisterGlobal(multiPredecessorReturnCallsiteModule, "RAX");
  attachTestAbi(multiPredecessorReturnCallsiteModule);
  llvm::Function *multiPredecessorReturnCallsiteFunction =
      createReturnStoreFunction(multiPredecessorReturnCallsiteModule,
                                "multi_predecessor_callsite_return_rax",
                                multiPredecessorReturnCallsiteRax, "RAX");
  llvm::LoadInst *multiPredecessorReturnCallsiteLoad = nullptr;
  createReturnLoadMultiPredecessorCallerFunction(
      multiPredecessorReturnCallsiteModule,
      "call_multi_predecessor_callsite_return_rax",
      multiPredecessorReturnCallsiteFunction, multiPredecessorReturnCallsiteRax,
      "RAX", &multiPredecessorReturnCallsiteLoad);
  if (!expectReturnOnlyRewriteRejected(
          multiPredecessorReturnCallsiteModule,
          *multiPredecessorReturnCallsiteFunction,
          multiPredecessorReturnCallsiteLoad,
          "return-only prototype rewrite accepted multi-predecessor callsite")) {
    return EXIT_FAILURE;
  }

  llvm::Module loopReturnCallsiteModule(
      "native-prototype-return-loop-callsite-test", context);
  llvm::GlobalVariable *loopReturnCallsiteRax =
      createRegisterGlobal(loopReturnCallsiteModule, "RAX");
  attachTestAbi(loopReturnCallsiteModule);
  llvm::Function *loopReturnCallsiteFunction = createReturnStoreFunction(
      loopReturnCallsiteModule, "loop_callsite_return_rax",
      loopReturnCallsiteRax, "RAX");
  createReturnLoadLoopCallerFunction(loopReturnCallsiteModule,
                                     "call_loop_callsite_return_rax",
                                     loopReturnCallsiteFunction);
  if (!expectReturnOnlyRewriteRejected(
          loopReturnCallsiteModule, *loopReturnCallsiteFunction, nullptr,
          "return-only prototype rewrite accepted loop callsite")) {
    return EXIT_FAILURE;
  }

  notdec::bin2llvm::NativePrototypeRewriteResult rewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(
          *returnFunction);
  ok &= expect(rewriteResult.Rewritten,
               "return-only prototype was not rewritten");
  returnFunction = rewriteResult.Function;
  ok &= expect(returnFunction != nullptr &&
                   functionTypeShape(*returnFunction->getFunctionType(),
                                     llvm::Type::getInt64Ty(context),
                                     llvm::ArrayRef<llvm::Type *>{}),
               "rewritten return-only function type was not i64()");
  llvm::ReturnInst *rewrittenRet = nullptr;
  if (returnFunction != nullptr) {
    for (llvm::BasicBlock &block : *returnFunction) {
      if (auto *ret =
              llvm::dyn_cast_or_null<llvm::ReturnInst>(block.getTerminator())) {
        rewrittenRet = ret;
      }
    }
  }
  ok &= expect(rewrittenRet != nullptr,
               "rewritten return-only function had no return instruction");
  ok &= expect(rewrittenRet != nullptr &&
                   rewrittenRet->getReturnValue() ==
                       returnStore->getValueOperand(),
               "rewritten return-only function returned wrong value");
  if (llvm::verifyModule(module, &llvm::errs())) {
    std::cerr << "module verification failed after return-only rewrite\n";
    return EXIT_FAILURE;
  }
  llvm::Instruction *inputReturnUser =
      llvm::dyn_cast<llvm::Instruction>(*inputReturnLoad->user_begin());
  llvm::Value *inputReturnValue = inputReturnStore->getValueOperand();
  notdec::bin2llvm::NativePrototypeRewriteResult inputReturnRewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputReturn(
          *inputReturnFunction);
  ok &= expect(inputReturnRewriteResult.Rewritten,
               "input-return prototype was not rewritten");
  inputReturnFunction = inputReturnRewriteResult.Function;
  ok &= expect(inputReturnFunction != nullptr &&
                   functionTypeShape(*inputReturnFunction->getFunctionType(),
                                     llvm::Type::getInt64Ty(context),
                                     llvm::ArrayRef(i64Param)),
               "rewritten input-return function type was not i64(i64)");
  ok &= expect(inputReturnFunction != nullptr &&
                   !inputReturnFunction->arg_empty() &&
                   inputReturnUser != nullptr &&
                   inputReturnUser->getOperand(0) ==
                       &*inputReturnFunction->arg_begin(),
               "rewritten input-return function did not use new argument");
  llvm::ReturnInst *rewrittenInputReturnRet = nullptr;
  bool sawInputReturnLoadAfterRewrite = false;
  if (inputReturnFunction != nullptr) {
    for (llvm::BasicBlock &block : *inputReturnFunction) {
      if (auto *ret =
              llvm::dyn_cast_or_null<llvm::ReturnInst>(block.getTerminator())) {
        rewrittenInputReturnRet = ret;
      }
      for (llvm::Instruction &instruction : block) {
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        if (load != nullptr &&
            load->getMetadata("notdec.register.external_input") != nullptr) {
          sawInputReturnLoadAfterRewrite = true;
        }
      }
    }
  }
  ok &= expect(!sawInputReturnLoadAfterRewrite,
               "rewritten input-return function kept old input load");
  ok &= expect(rewrittenInputReturnRet != nullptr,
               "rewritten input-return function had no return instruction");
  ok &= expect(rewrittenInputReturnRet != nullptr &&
                   rewrittenInputReturnRet->getReturnValue() ==
                       inputReturnValue,
               "rewritten input-return function returned wrong value");
  if (llvm::verifyModule(module, &llvm::errs())) {
    std::cerr << "module verification failed after input-return rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Module inputReturnCallsiteModule(
      "native-prototype-input-return-callsite-rewrite-test", context);
  llvm::GlobalVariable *inputReturnCallsiteRdi =
      createRegisterGlobal(inputReturnCallsiteModule, "RDI");
  llvm::GlobalVariable *inputReturnCallsiteRax =
      createRegisterGlobal(inputReturnCallsiteModule, "RAX");
  attachTestAbi(inputReturnCallsiteModule);
  llvm::LoadInst *callsiteInputReturnLoad = nullptr;
  llvm::StoreInst *callsiteInputReturnStore = nullptr;
  llvm::Function *callsiteInputReturnFunction = createInputReturnFunction(
      inputReturnCallsiteModule, "callsite_input_rdi_return_rax",
      inputReturnCallsiteRdi, "RDI", inputReturnCallsiteRax, "RAX",
      &callsiteInputReturnLoad, &callsiteInputReturnStore);
  attachExternalInputs(*callsiteInputReturnFunction,
                       {{"RDI", inputReturnCallsiteRdi}});
  llvm::CallInst *oldInputReturnCallsiteCall = nullptr;
  llvm::LoadInst *oldInputReturnCallsiteLoad = nullptr;
  createInputStoreReturnLoadCallerFunction(
      inputReturnCallsiteModule, "call_callsite_input_rdi_return_rax",
      callsiteInputReturnFunction, inputReturnCallsiteRdi, "RDI",
      inputReturnCallsiteRax, "RAX", &oldInputReturnCallsiteCall,
      &oldInputReturnCallsiteLoad);
  notdec::bin2llvm::runNativePrototypeRecovery(inputReturnCallsiteModule,
                                               options);
  notdec::bin2llvm::NativePrototypeRewriteResult
      inputReturnCallsiteRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputReturn(
              *callsiteInputReturnFunction);
  ok &= expect(inputReturnCallsiteRewriteResult.Rewritten,
               "input-return prototype with direct callsite was not rewritten");
  callsiteInputReturnFunction = inputReturnCallsiteRewriteResult.Function;
  ok &= expect(callsiteInputReturnFunction != nullptr &&
                   functionTypeShape(
                       *callsiteInputReturnFunction->getFunctionType(),
                       llvm::Type::getInt64Ty(context),
                       llvm::ArrayRef(i64Param)),
               "callsite rewritten input-return function type was not i64(i64)");
  llvm::CallInst *rewrittenInputReturnCallsiteCall = nullptr;
  llvm::Function *inputReturnCallsiteCaller =
      inputReturnCallsiteModule.getFunction("call_callsite_input_rdi_return_rax");
  if (inputReturnCallsiteCaller != nullptr) {
    for (llvm::BasicBlock &block : *inputReturnCallsiteCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call != nullptr && call->getCalledFunction() ==
                                   callsiteInputReturnFunction) {
          rewrittenInputReturnCallsiteCall = call;
        }
      }
    }
  }
  ok &= expect(rewrittenInputReturnCallsiteCall != nullptr,
               "input-return direct callsite was not rewritten to new callee");
  ok &= expect(rewrittenInputReturnCallsiteCall != nullptr &&
                   rewrittenInputReturnCallsiteCall->arg_size() == 1,
               "input-return direct callsite did not get one argument");
  ok &= expect(rewrittenInputReturnCallsiteCall != nullptr &&
                   llvm::isa<llvm::ConstantInt>(
                       rewrittenInputReturnCallsiteCall->getArgOperand(0)),
               "input-return direct callsite argument did not use store value");
  ok &= expect(rewrittenInputReturnCallsiteCall != nullptr &&
                   rewrittenInputReturnCallsiteCall->getType() ==
                       llvm::Type::getInt64Ty(context),
               "input-return direct callsite did not return i64");
  bool sawOldInputReturnCallsiteLoad = false;
  if (inputReturnCallsiteCaller != nullptr) {
    for (llvm::BasicBlock &block : *inputReturnCallsiteCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        if (load != nullptr &&
            load->getMetadata("notdec.register.access") != nullptr) {
          sawOldInputReturnCallsiteLoad = true;
        }
      }
    }
  }
  ok &= expect(!sawOldInputReturnCallsiteLoad,
               "input-return direct callsite kept old return register load");
  ok &= expect(rewrittenInputReturnCallsiteCall != nullptr &&
                   !rewrittenInputReturnCallsiteCall->use_empty(),
               "input-return direct callsite result was not used");
  if (llvm::verifyModule(inputReturnCallsiteModule, &llvm::errs())) {
    std::cerr
        << "callsite module verification failed after input-return rewrite\n";
    return EXIT_FAILURE;
  }
  notdec::bin2llvm::NativePrototypeRewriteResult dispatchInputResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototype(*dispatchInputFunction);
  ok &= expect(dispatchInputResult.Rewritten,
               "dispatch input-only prototype was not rewritten");
  dispatchInputFunction = dispatchInputResult.Function;
  ok &= expect(dispatchInputFunction != nullptr &&
                   functionTypeShape(*dispatchInputFunction->getFunctionType(),
                                     llvm::Type::getVoidTy(context),
                                     llvm::ArrayRef(i64Param)),
               "dispatch input-only function type was not void(i64)");

  notdec::bin2llvm::NativePrototypeRewriteResult dispatchReturnResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototype(*dispatchReturnFunction);
  ok &= expect(dispatchReturnResult.Rewritten,
               "dispatch return-only prototype was not rewritten");
  dispatchReturnFunction = dispatchReturnResult.Function;
  ok &= expect(dispatchReturnFunction != nullptr &&
                   functionTypeShape(*dispatchReturnFunction->getFunctionType(),
                                     llvm::Type::getInt64Ty(context),
                                     llvm::ArrayRef<llvm::Type *>{}),
               "dispatch return-only function type was not i64()");

  notdec::bin2llvm::NativePrototypeRewriteResult dispatchInputReturnResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototype(
          *dispatchInputReturnFunction);
  ok &= expect(dispatchInputReturnResult.Rewritten,
               "dispatch input-return prototype was not rewritten");
  dispatchInputReturnFunction = dispatchInputReturnResult.Function;
  ok &= expect(dispatchInputReturnFunction != nullptr &&
                   functionTypeShape(
                       *dispatchInputReturnFunction->getFunctionType(),
                       llvm::Type::getInt64Ty(context),
                       llvm::ArrayRef(i64Param)),
               "dispatch input-return function type was not i64(i64)");

  notdec::bin2llvm::NativePrototypeRewriteResult dispatchMissingResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototype(*unusedInputFunction);
  ok &= expect(!dispatchMissingResult.Rewritten,
               "dispatch missing prototype was rewritten");
  ok &= expect(dispatchMissingResult.Reason == "missing recovered prototype",
               "dispatch missing prototype had unexpected reason");

  notdec::bin2llvm::NativePrototypeRewriteResult dispatchMultiReturnResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototype(
          *twoOutputReturnFunction);
  ok &= expect(dispatchMultiReturnResult.Rewritten,
               "dispatch multi-return prototype was not rewritten");
  twoOutputReturnFunction = dispatchMultiReturnResult.Function;
  auto *dispatchMultiReturnStruct =
      twoOutputReturnFunction != nullptr
          ? llvm::dyn_cast<llvm::StructType>(
                twoOutputReturnFunction->getReturnType())
          : nullptr;
  ok &= expect(dispatchMultiReturnStruct != nullptr &&
                   dispatchMultiReturnStruct->getNumElements() == 2,
               "dispatch multi-return function type was not a two-field struct");
  if (llvm::verifyModule(module, &llvm::errs())) {
    std::cerr << "module verification failed after dispatch rewrite\n";
    return EXIT_FAILURE;
  }
  ok &= expect(recoveredRegisterAt(*twoOutputReturnFunction, 4, 0, "RAX"),
               "recovered RAX return was not sorted before RDX");
  ok &= expect(recoveredRegisterAt(*twoOutputReturnFunction, 4, 1, "RDX"),
               "recovered RDX return was not sorted after RAX");
  std::optional<notdec::bin2llvm::NativeRecoveredPrototype> twoOutputPrototype =
      notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(
          *twoOutputReturnFunction);
  ok &= expect(twoOutputPrototype.has_value(),
               "recovered two-output prototype was not readable");
  if (twoOutputPrototype) {
    ok &= expect(twoOutputPrototype->Returns.size() == 2,
                 "recovered two-output prototype return count was not read");
    ok &= expect(
        recoveredPrototypeParamAt(twoOutputPrototype->Returns, 0, "RAX"),
        "recovered RAX return was not read before RDX");
    ok &= expect(
        recoveredPrototypeParamAt(twoOutputPrototype->Returns, 1, "RDX"),
        "recovered RDX return was not read after RAX");
    std::optional<llvm::FunctionType *> multiReturnType =
        notdec::bin2llvm::buildNativeRecoveredPrototypeFunctionType(
            context, *twoOutputPrototype);
    auto *multiReturnStruct = multiReturnType
                                  ? llvm::dyn_cast<llvm::StructType>(
                                        (*multiReturnType)->getReturnType())
                                  : nullptr;
    ok &= expect(multiReturnStruct != nullptr &&
                     multiReturnStruct->getNumElements() == 2 &&
                     multiReturnStruct->getElementType(0)->isIntegerTy(64) &&
                     multiReturnStruct->getElementType(1)->isIntegerTy(64),
                 "multi-return recovered prototype type was not {i64, i64}()");
    notdec::bin2llvm::NativePrototypeRewriteEligibility eligibility =
        notdec::bin2llvm::getNativePrototypeRewriteEligibility(
            *twoOutputReturnFunction);
    ok &= expect(eligibility.Eligible,
                 "multi-return prototype was not rewrite eligible");
    ok &= expect(!eligibility.NeedsRewrite,
                 "rewritten multi-return prototype still requested rewrite");
    ok &= expect(multiReturnType && eligibility.RecoveredType == *multiReturnType,
                 "multi-return prototype rewrite type did not match");
    llvm::ReturnInst *multiReturnRet = nullptr;
    for (llvm::BasicBlock &block : *twoOutputReturnFunction) {
      if (auto *ret =
              llvm::dyn_cast_or_null<llvm::ReturnInst>(block.getTerminator())) {
        multiReturnRet = ret;
      }
    }
    auto *secondInsert = multiReturnRet != nullptr
                             ? llvm::dyn_cast<llvm::InsertValueInst>(
                                   multiReturnRet->getReturnValue())
                             : nullptr;
    auto *firstInsert =
        secondInsert != nullptr
            ? llvm::dyn_cast<llvm::InsertValueInst>(
                  secondInsert->getAggregateOperand())
            : nullptr;
    ok &= expect(firstInsert != nullptr && secondInsert != nullptr,
                 "multi-return rewrite did not build insertvalue aggregate");
  }
  ok &= expect(unusedInputFunction->getMetadata("notdec.prototype.recovered") ==
                   nullptr,
               "empty recovered prototype metadata was not cleared");
  ok &= expect(!notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(
                    *unusedInputFunction),
               "empty recovered prototype metadata was incorrectly read");
  notdec::bin2llvm::NativePrototypeRewriteEligibility missingEligibility =
      notdec::bin2llvm::getNativePrototypeRewriteEligibility(
          *unusedInputFunction);
  ok &= expect(!missingEligibility.Eligible,
               "missing recovered prototype was incorrectly rewrite eligible");
  ok &= expect(missingEligibility.Reason == "missing recovered prototype",
               "missing recovered prototype had unexpected ineligible reason");

  llvm::Module batchModule("native-prototype-batch-rewrite-test", context);
  llvm::GlobalVariable *batchRdi = createRegisterGlobal(batchModule, "RDI");
  llvm::GlobalVariable *batchRax = createRegisterGlobal(batchModule, "RAX");
  attachTestAbi(batchModule);

  llvm::LoadInst *batchInputLoad = nullptr;
  llvm::Function *batchInputFunction = createUsedExternalInputFunction(
      batchModule, "batch_input_rdi", batchRdi, "RDI", &batchInputLoad);
  attachExternalInputs(*batchInputFunction, {{"RDI", batchRdi}});

  llvm::StoreInst *batchReturnStore = nullptr;
  createReturnStoreFunction(batchModule, "batch_return_rax", batchRax, "RAX",
                            &batchReturnStore);

  llvm::LoadInst *batchInputReturnLoad = nullptr;
  llvm::StoreInst *batchInputReturnStore = nullptr;
  llvm::Function *batchInputReturnFunction = createInputReturnFunction(
      batchModule, "batch_input_rdi_return_rax", batchRdi, "RDI", batchRax,
      "RAX", &batchInputReturnLoad, &batchInputReturnStore);
  attachExternalInputs(*batchInputReturnFunction, {{"RDI", batchRdi}});

  llvm::LoadInst *batchUsedInputLoad = nullptr;
  llvm::Function *batchUsedInputFunction = createUsedExternalInputFunction(
      batchModule, "batch_input_rdi_used", batchRdi, "RDI",
      &batchUsedInputLoad);
  attachExternalInputs(*batchUsedInputFunction, {{"RDI", batchRdi}});
  createCallerFunction(batchModule, "call_batch_input_rdi_used",
                       batchUsedInputFunction);

  llvm::Function *batchUnsafeReturnFunction = createReturnStoreFunction(
      batchModule, "batch_return_rax_unsafe_callsite", batchRax, "RAX");
  llvm::LoadInst *batchUnsafeReturnLoad = nullptr;
  createReturnLoadMultiSuccessorCallerFunction(
      batchModule, "call_batch_return_rax_unsafe_callsite",
      batchUnsafeReturnFunction, batchRax, "RAX", &batchUnsafeReturnLoad);

  llvm::LoadInst *batchUnsafeInputLoad = nullptr;
  llvm::Function *batchUnsafeInputFunction = createUsedExternalInputFunction(
      batchModule, "batch_input_rdi_unsafe_callsite", batchRdi, "RDI",
      &batchUnsafeInputLoad);
  attachExternalInputs(*batchUnsafeInputFunction, {{"RDI", batchRdi}});
  createCallerFunction(batchModule, "call_batch_input_rdi_unsafe_callsite",
                       batchUnsafeInputFunction);
  createFunction(batchModule, "batch_missing_recovered");

  notdec::bin2llvm::runNativePrototypeRecovery(batchModule, options);
  notdec::bin2llvm::NativePrototypeModuleRewriteSummary batchRewriteSummary =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypes(batchModule);
  ok &= expect(batchRewriteSummary.FunctionsSeen == 10,
               "batch rewrite saw unexpected function count");
  ok &= expect(batchRewriteSummary.FunctionsRewritten == 3,
               "batch rewrite rewrote unexpected function count");
  ok &= expect(batchRewriteSummary.FunctionsSkipped == 7,
               "batch rewrite skipped unexpected function count");
  ok &= expect(batchRewriteSummary.SkippedByReason["function has uses"] == 0,
               "batch rewrite did not count function-use skip reason");
  ok &= expect(
      batchRewriteSummary.SkippedByReason["missing recovered prototype"] == 4,
      "batch rewrite did not count missing-prototype skip reason");
  ok &= expect(
      batchRewriteSummary.SkippedByReason["unsafe callsite return load"] == 1,
      "batch rewrite did not count unsafe-return-load skip reason");
  ok &= expect(
      batchRewriteSummary.SkippedByReason["unsafe callsite input value"] == 2,
      "batch rewrite did not count unsafe-input-value skip reason");
  ok &= expect(batchModule.getFunction("batch_input_rdi") != nullptr &&
                   functionTypeShape(
                       *batchModule.getFunction("batch_input_rdi")
                            ->getFunctionType(),
                       llvm::Type::getVoidTy(context),
                       llvm::ArrayRef(i64Param)),
               "batch input-only function type was not void(i64)");
  ok &= expect(batchModule.getFunction("batch_return_rax") != nullptr &&
                   functionTypeShape(
                       *batchModule.getFunction("batch_return_rax")
                            ->getFunctionType(),
                       llvm::Type::getInt64Ty(context),
                       llvm::ArrayRef<llvm::Type *>{}),
               "batch return-only function type was not i64()");
  ok &= expect(batchModule.getFunction("batch_input_rdi_return_rax") !=
                       nullptr &&
                   functionTypeShape(
                       *batchModule.getFunction("batch_input_rdi_return_rax")
                            ->getFunctionType(),
                       llvm::Type::getInt64Ty(context),
                       llvm::ArrayRef(i64Param)),
               "batch input-return function type was not i64(i64)");
  ok &= expect(batchModule.getFunction("batch_input_rdi_used") != nullptr &&
                   batchModule.getFunction("batch_input_rdi_used")
                       ->getFunctionType()
                       ->getNumParams() == 0,
               "batch function with caller was incorrectly rewritten");
  if (llvm::verifyModule(batchModule, &llvm::errs())) {
    std::cerr << "batch module verification failed after prototype rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Module optInModule("native-prototype-opt-in-rewrite-test", context);
  llvm::GlobalVariable *optInRdi = createRegisterGlobal(optInModule, "RDI");
  llvm::GlobalVariable *optInRax = createRegisterGlobal(optInModule, "RAX");
  attachTestAbi(optInModule);

  llvm::LoadInst *optInInputReturnLoad = nullptr;
  llvm::StoreInst *optInInputReturnStore = nullptr;
  createInputReturnFunction(optInModule, "opt_in_input_rdi_return_rax",
                            optInRdi, "RDI", optInRax, "RAX",
                            &optInInputReturnLoad, &optInInputReturnStore);
  attachExternalInputs(*optInModule.getFunction("opt_in_input_rdi_return_rax"),
                       {{"RDI", optInRdi}});
  createFunction(optInModule, "opt_in_missing_recovered");

  notdec::bin2llvm::NativePrototypeRecoveryOptions rewriteOptions;
  rewriteOptions.RewriteSignatures = true;
  notdec::bin2llvm::NativePrototypeRecoverySummary optInSummary =
      notdec::bin2llvm::runNativePrototypeRecovery(optInModule, rewriteOptions);
  ok &= expect(optInSummary.SignatureRewriteFunctionsSeen == 2,
               "opt-in rewrite saw unexpected function count");
  ok &= expect(optInSummary.SignatureRewriteFunctionsRewritten == 1,
               "opt-in rewrite rewrote unexpected function count");
  ok &= expect(optInSummary.SignatureRewriteFunctionsSkipped == 1,
               "opt-in rewrite skipped unexpected function count");
  ok &= expect(optInSummary.SignatureRewriteSkippedByReason
                   ["missing recovered prototype"] == 1,
               "opt-in rewrite did not count missing-prototype skip reason");
  ok &= expect(optInModule.getFunction("opt_in_input_rdi_return_rax") !=
                       nullptr &&
                   functionTypeShape(
                       *optInModule.getFunction("opt_in_input_rdi_return_rax")
                            ->getFunctionType(),
                       llvm::Type::getInt64Ty(context),
                       llvm::ArrayRef(i64Param)),
               "opt-in input-return function type was not i64(i64)");
  if (llvm::verifyModule(optInModule, &llvm::errs())) {
    std::cerr << "opt-in module verification failed after prototype rewrite\n";
    return EXIT_FAILURE;
  }
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
