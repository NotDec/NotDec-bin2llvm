#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/passes/NativePrototypeRecovery.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
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

llvm::Function *createUnmarkedReturnLoadCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *output, llvm::LoadInst **loadOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(callee->getFunctionType(), callee);
  llvm::LoadInst *load =
      builder.CreateLoad(output->getValueType(), output, "return_value");
  builder.CreateAdd(load, llvm::ConstantInt::get(output->getValueType(), 1));
  builder.CreateRetVoid();
  *loadOut = load;
  return function;
}

llvm::Function *createIntermediateCallReturnLoadCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *output, const std::string &registerName,
    llvm::LoadInst **loadOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::Function *intermediate =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             name + "_intermediate", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateCall(intermediate->getFunctionType(), intermediate);
  llvm::LoadInst *load = builder.CreateLoad(output->getValueType(), output,
                                            registerName + ".return_value");
  load->setMetadata("notdec.register.access",
                    registerAccessMetadata(context, registerName));
  builder.CreateAdd(load, llvm::ConstantInt::get(output->getValueType(), 1));
  builder.CreateRetVoid();
  *loadOut = load;
  return function;
}

llvm::Function *createTwoReturnLoadCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *firstOutput, const std::string &firstRegisterName,
    llvm::GlobalVariable *secondOutput, const std::string &secondRegisterName,
    llvm::LoadInst **firstLoadOut, llvm::LoadInst **secondLoadOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(callee->getFunctionType(), callee);
  llvm::LoadInst *firstLoad =
      builder.CreateLoad(firstOutput->getValueType(), firstOutput,
                         firstRegisterName + ".return_value");
  firstLoad->setMetadata("notdec.register.access",
                         registerAccessMetadata(context, firstRegisterName));
  llvm::LoadInst *secondLoad =
      builder.CreateLoad(secondOutput->getValueType(), secondOutput,
                         secondRegisterName + ".return_value");
  secondLoad->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, secondRegisterName));
  builder.CreateAdd(firstLoad, secondLoad);
  builder.CreateRetVoid();
  *firstLoadOut = firstLoad;
  *secondLoadOut = secondLoad;
  return function;
}

llvm::Function *createSharedSuccessorOneReturnLoadCallerFunction(
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

llvm::Function *createSharedSuccessorTwoReturnLoadCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *firstOutput, const std::string &firstRegisterName,
    llvm::GlobalVariable *secondOutput, const std::string &secondRegisterName,
    llvm::LoadInst **firstLoadOut, llvm::LoadInst **secondLoadOut) {
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
  llvm::LoadInst *firstLoad =
      builder.CreateLoad(firstOutput->getValueType(), firstOutput,
                         firstRegisterName + ".return_value");
  firstLoad->setMetadata("notdec.register.access",
                         registerAccessMetadata(context, firstRegisterName));
  llvm::LoadInst *secondLoad =
      builder.CreateLoad(secondOutput->getValueType(), secondOutput,
                         secondRegisterName + ".return_value");
  secondLoad->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, secondRegisterName));
  builder.CreateAdd(firstLoad, secondLoad);
  builder.CreateRetVoid();
  *firstLoadOut = firstLoad;
  *secondLoadOut = secondLoad;
  return function;
}

llvm::Function *createInputStoreTwoReturnLoadCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *input, const std::string &inputRegisterName,
    llvm::GlobalVariable *firstOutput, const std::string &firstRegisterName,
    llvm::GlobalVariable *secondOutput, const std::string &secondRegisterName,
    llvm::CallInst **callOut, llvm::LoadInst **firstLoadOut,
    llvm::LoadInst **secondLoadOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *argument = llvm::ConstantInt::get(input->getValueType(), 0x3456);
  llvm::StoreInst *store = builder.CreateStore(argument, input);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, inputRegisterName));
  llvm::CallInst *call = builder.CreateCall(callee->getFunctionType(), callee);
  llvm::LoadInst *firstLoad =
      builder.CreateLoad(firstOutput->getValueType(), firstOutput,
                         firstRegisterName + ".return_value");
  firstLoad->setMetadata("notdec.register.access",
                         registerAccessMetadata(context, firstRegisterName));
  llvm::LoadInst *secondLoad =
      builder.CreateLoad(secondOutput->getValueType(), secondOutput,
                         secondRegisterName + ".return_value");
  secondLoad->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, secondRegisterName));
  builder.CreateAdd(firstLoad, secondLoad);
  builder.CreateRetVoid();
  *callOut = call;
  *firstLoadOut = firstLoad;
  *secondLoadOut = secondLoad;
  return function;
}

llvm::Function *createInputStoreIntermediateReturnLoadCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *input, const std::string &inputRegisterName,
    llvm::GlobalVariable *output, const std::string &outputRegisterName,
    llvm::CallInst **callOut, llvm::LoadInst **loadOut,
    llvm::Value **argumentOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::Function *intermediate =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             name + "_intermediate", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *argument = llvm::ConstantInt::get(input->getValueType(), 0x2468);
  llvm::StoreInst *store = builder.CreateStore(argument, input);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, inputRegisterName));
  llvm::CallInst *call = builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateCall(intermediate->getFunctionType(), intermediate);
  llvm::LoadInst *load = builder.CreateLoad(output->getValueType(), output,
                                            outputRegisterName + ".return_value");
  load->setMetadata("notdec.register.access",
                    registerAccessMetadata(context, outputRegisterName));
  builder.CreateAdd(load, llvm::ConstantInt::get(output->getValueType(), 1));
  builder.CreateRetVoid();
  *callOut = call;
  *loadOut = load;
  *argumentOut = argument;
  return function;
}

llvm::Function *createTwoInputStoreTwoReturnLoadCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *firstInput, const std::string &firstInputName,
    llvm::GlobalVariable *secondInput, const std::string &secondInputName,
    llvm::GlobalVariable *firstOutput, const std::string &firstOutputName,
    llvm::GlobalVariable *secondOutput, const std::string &secondOutputName,
    llvm::CallInst **callOut, llvm::LoadInst **firstLoadOut,
    llvm::LoadInst **secondLoadOut, llvm::Value **firstArgumentOut,
    llvm::Value **secondArgumentOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *firstArgument =
      llvm::ConstantInt::get(firstInput->getValueType(), 0x3579);
  llvm::StoreInst *firstStore =
      builder.CreateStore(firstArgument, firstInput);
  firstStore->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, firstInputName));
  llvm::Value *secondArgument =
      llvm::ConstantInt::get(secondInput->getValueType(), 0x468a);
  llvm::StoreInst *secondStore =
      builder.CreateStore(secondArgument, secondInput);
  secondStore->setMetadata("notdec.register.access",
                           registerAccessMetadata(context, secondInputName));
  llvm::CallInst *call = builder.CreateCall(callee->getFunctionType(), callee);
  llvm::LoadInst *firstLoad =
      builder.CreateLoad(firstOutput->getValueType(), firstOutput,
                         firstOutputName + ".return_value");
  firstLoad->setMetadata("notdec.register.access",
                         registerAccessMetadata(context, firstOutputName));
  llvm::LoadInst *secondLoad =
      builder.CreateLoad(secondOutput->getValueType(), secondOutput,
                         secondOutputName + ".return_value");
  secondLoad->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, secondOutputName));
  builder.CreateAdd(firstLoad, secondLoad);
  builder.CreateRetVoid();
  *callOut = call;
  *firstLoadOut = firstLoad;
  *secondLoadOut = secondLoad;
  *firstArgumentOut = firstArgument;
  *secondArgumentOut = secondArgument;
  return function;
}

llvm::Function *createTwoInputStoreCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *firstInput, const std::string &firstRegisterName,
    llvm::GlobalVariable *secondInput, const std::string &secondRegisterName,
    llvm::CallInst **callOut, llvm::Value **firstArgumentOut,
    llvm::Value **secondArgumentOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *firstArgument =
      llvm::ConstantInt::get(firstInput->getValueType(), 0x1111);
  llvm::StoreInst *firstStore = builder.CreateStore(firstArgument, firstInput);
  firstStore->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, firstRegisterName));
  llvm::Value *secondArgument =
      llvm::ConstantInt::get(secondInput->getValueType(), 0x2222);
  llvm::StoreInst *secondStore =
      builder.CreateStore(secondArgument, secondInput);
  secondStore->setMetadata("notdec.register.access",
                           registerAccessMetadata(context, secondRegisterName));
  llvm::CallInst *call = builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateRetVoid();
  *callOut = call;
  *firstArgumentOut = firstArgument;
  *secondArgumentOut = secondArgument;
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

llvm::Function *createUnusedReturnMultiSuccessorCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right = llvm::BasicBlock::Create(context, "right", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), left, right);

  builder.SetInsertPoint(left);
  builder.CreateRetVoid();

  builder.SetInsertPoint(right);
  builder.CreateRetVoid();
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

llvm::Function *createUnusedReturnSharedSuccessorCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee) {
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
  llvm::BasicBlock *doneBlock =
      llvm::BasicBlock::Create(context, "done", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), callBlock,
                       otherBlock);

  builder.SetInsertPoint(callBlock);
  builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateBr(doneBlock);

  builder.SetInsertPoint(otherBlock);
  builder.CreateBr(doneBlock);

  builder.SetInsertPoint(doneBlock);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createClobberReturnSharedSuccessorCallerFunction(
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
      llvm::BasicBlock::Create(context, "use_clobbered_return", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), callBlock,
                       otherBlock);

  builder.SetInsertPoint(callBlock);
  builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateBr(useBlock);

  builder.SetInsertPoint(otherBlock);
  builder.CreateBr(useBlock);

  builder.SetInsertPoint(useBlock);
  llvm::StoreInst *store =
      builder.CreateStore(llvm::ConstantInt::get(output->getValueType(), 7),
                          output);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, registerName));
  llvm::LoadInst *load = builder.CreateLoad(output->getValueType(), output,
                                            registerName + ".clobbered_value");
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

llvm::Function *createInputStoreLinearPredecessorCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *input, const std::string &registerName,
    llvm::CallInst **callOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *middle =
      llvm::BasicBlock::Create(context, "before_call", function);
  llvm::BasicBlock *callBlock =
      llvm::BasicBlock::Create(context, "call", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *argument = llvm::ConstantInt::get(input->getValueType(), 0x789a);
  llvm::StoreInst *store = builder.CreateStore(argument, input);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, registerName));
  builder.CreateBr(middle);

  builder.SetInsertPoint(middle);
  builder.CreateBr(callBlock);

  builder.SetInsertPoint(callBlock);
  llvm::CallInst *call = builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateRetVoid();
  *callOut = call;
  return function;
}

llvm::Function *createInputStoreEquivalentPredecessorCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *input, const std::string &registerName,
    llvm::CallInst **callOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right = llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *callBlock =
      llvm::BasicBlock::Create(context, "call", function);

  llvm::Value *argument = llvm::ConstantInt::get(input->getValueType(), 0x789a);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), left, right);

  builder.SetInsertPoint(left);
  llvm::StoreInst *leftStore = builder.CreateStore(argument, input);
  leftStore->setMetadata("notdec.register.access",
                         registerAccessMetadata(context, registerName));
  builder.CreateBr(callBlock);

  builder.SetInsertPoint(right);
  llvm::StoreInst *rightStore = builder.CreateStore(argument, input);
  rightStore->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, registerName));
  builder.CreateBr(callBlock);

  builder.SetInsertPoint(callBlock);
  llvm::CallInst *call = builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateRetVoid();
  *callOut = call;
  return function;
}

llvm::Function *createInputStoreConflictingPredecessorCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *input, const std::string &registerName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right = llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *callBlock =
      llvm::BasicBlock::Create(context, "call", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), left, right);

  builder.SetInsertPoint(left);
  llvm::StoreInst *leftStore = builder.CreateStore(
      llvm::ConstantInt::get(input->getValueType(), 0x789a), input);
  leftStore->setMetadata("notdec.register.access",
                         registerAccessMetadata(context, registerName));
  builder.CreateBr(callBlock);

  builder.SetInsertPoint(right);
  llvm::StoreInst *rightStore = builder.CreateStore(
      llvm::ConstantInt::get(input->getValueType(), 0x789b), input);
  rightStore->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, registerName));
  builder.CreateBr(callBlock);

  builder.SetInsertPoint(callBlock);
  builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateRetVoid();
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

llvm::Function *createInputStoreSharedSuccessorReturnLoadCallerFunction(
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
  llvm::Value *argument = llvm::ConstantInt::get(input->getValueType(), 0x5678);
  llvm::StoreInst *store = builder.CreateStore(argument, input);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, inputRegisterName));
  llvm::CallInst *call = builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateBr(useBlock);

  builder.SetInsertPoint(otherBlock);
  builder.CreateBr(useBlock);

  builder.SetInsertPoint(useBlock);
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

llvm::Function *createInputStoreSharedSuccessorTwoReturnLoadCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *input, const std::string &inputRegisterName,
    llvm::GlobalVariable *firstOutput, const std::string &firstRegisterName,
    llvm::GlobalVariable *secondOutput, const std::string &secondRegisterName,
    llvm::CallInst **callOut, llvm::LoadInst **firstLoadOut,
    llvm::LoadInst **secondLoadOut) {
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
  llvm::Value *argument = llvm::ConstantInt::get(input->getValueType(), 0x6789);
  llvm::StoreInst *store = builder.CreateStore(argument, input);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, inputRegisterName));
  llvm::CallInst *call = builder.CreateCall(callee->getFunctionType(), callee);
  builder.CreateBr(useBlock);

  builder.SetInsertPoint(otherBlock);
  builder.CreateBr(useBlock);

  builder.SetInsertPoint(useBlock);
  llvm::LoadInst *firstLoad =
      builder.CreateLoad(firstOutput->getValueType(), firstOutput,
                         firstRegisterName + ".return_value");
  firstLoad->setMetadata("notdec.register.access",
                         registerAccessMetadata(context, firstRegisterName));
  llvm::LoadInst *secondLoad =
      builder.CreateLoad(secondOutput->getValueType(), secondOutput,
                         secondRegisterName + ".return_value");
  secondLoad->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, secondRegisterName));
  builder.CreateAdd(firstLoad, secondLoad);
  builder.CreateRetVoid();
  *callOut = call;
  *firstLoadOut = firstLoad;
  *secondLoadOut = secondLoad;
  return function;
}

llvm::Function *createTwoInputStoreReturnLoadCallerFunction(
    llvm::Module &module, const std::string &name, llvm::Function *callee,
    llvm::GlobalVariable *firstInput, const std::string &firstRegisterName,
    llvm::GlobalVariable *secondInput, const std::string &secondRegisterName,
    llvm::GlobalVariable *output, const std::string &outputRegisterName,
    llvm::CallInst **callOut, llvm::LoadInst **loadOut,
    llvm::Value **firstArgumentOut, llvm::Value **secondArgumentOut) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *firstArgument =
      llvm::ConstantInt::get(firstInput->getValueType(), 0x1357);
  llvm::StoreInst *firstStore = builder.CreateStore(firstArgument, firstInput);
  firstStore->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, firstRegisterName));
  llvm::Value *secondArgument =
      llvm::ConstantInt::get(secondInput->getValueType(), 0x2468);
  llvm::StoreInst *secondStore =
      builder.CreateStore(secondArgument, secondInput);
  secondStore->setMetadata("notdec.register.access",
                           registerAccessMetadata(context, secondRegisterName));
  llvm::CallInst *call = builder.CreateCall(callee->getFunctionType(), callee);
  llvm::LoadInst *load = builder.CreateLoad(output->getValueType(), output,
                                            outputRegisterName + ".return_value");
  load->setMetadata("notdec.register.access",
                    registerAccessMetadata(context, outputRegisterName));
  builder.CreateAdd(load, llvm::ConstantInt::get(output->getValueType(), 1));
  builder.CreateRetVoid();
  *callOut = call;
  *loadOut = load;
  *firstArgumentOut = firstArgument;
  *secondArgumentOut = secondArgument;
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

llvm::Function *createTwoUsedExternalInputFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *first,
    const std::string &firstRegisterName, llvm::GlobalVariable *second,
    const std::string &secondRegisterName, llvm::LoadInst **firstInputLoad,
    llvm::LoadInst **secondInputLoad) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *firstLoad =
      createExternalInputLoad(builder, first, firstRegisterName);
  llvm::LoadInst *secondLoad =
      createExternalInputLoad(builder, second, secondRegisterName);
  builder.CreateAdd(firstLoad, secondLoad);
  builder.CreateRetVoid();
  *firstInputLoad = firstLoad;
  *secondInputLoad = secondLoad;
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

llvm::Function *createTemporaryReturnStoreFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *global,
    const std::string &registerName, llvm::StoreInst **temporaryStore,
    llvm::StoreInst **returnStore) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::StoreInst *temporary = builder.CreateStore(
      llvm::ConstantInt::get(global->getValueType(), 0xaaaa), global);
  temporary->setMetadata("notdec.register.access",
                         registerAccessMetadata(context, registerName));
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(global->getValueType(), 0xbbbb), global);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, registerName));
  builder.CreateRetVoid();
  *temporaryStore = temporary;
  *returnStore = store;
  return function;
}

llvm::Function *createReturnRegisterLoadFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *global,
    const std::string &registerName, llvm::LoadInst **returnLoad = nullptr) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *load = builder.CreateLoad(global->getValueType(), global);
  load->setMetadata("notdec.register.access",
                    registerAccessMetadata(context, registerName));
  llvm::StoreInst *store = builder.CreateStore(load, global);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, registerName));
  builder.CreateRetVoid();
  if (returnLoad != nullptr) {
    *returnLoad = load;
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

llvm::Function *createInputForwardReturnFunction(
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
  llvm::StoreInst *store = builder.CreateStore(load, output);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, outputRegisterName));
  builder.CreateRetVoid();
  *inputLoad = load;
  *returnStore = store;
  return function;
}

llvm::Function *createTwoInputReturnFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *first,
    const std::string &firstRegisterName, llvm::GlobalVariable *second,
    const std::string &secondRegisterName, llvm::GlobalVariable *output,
    const std::string &outputRegisterName, llvm::LoadInst **firstInputLoad,
    llvm::LoadInst **secondInputLoad, llvm::StoreInst **returnStore) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *firstLoad =
      createExternalInputLoad(builder, first, firstRegisterName);
  llvm::LoadInst *secondLoad =
      createExternalInputLoad(builder, second, secondRegisterName);
  llvm::Value *value = builder.CreateAdd(firstLoad, secondLoad);
  llvm::StoreInst *store = builder.CreateStore(value, output);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, outputRegisterName));
  builder.CreateRetVoid();
  *firstInputLoad = firstLoad;
  *secondInputLoad = secondLoad;
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

llvm::Function *createTwoReturnSameValueStoreFunction(
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
  llvm::AllocaInst *local =
      builder.CreateAlloca(global->getValueType(), nullptr, "local");
  builder.CreateStore(llvm::ConstantInt::get(global->getValueType(), 0x2222),
                      local);
  llvm::Value *loaded = builder.CreateLoad(global->getValueType(), local);
  llvm::Value *value = builder.CreateAdd(
      loaded, llvm::ConstantInt::get(global->getValueType(), 1));
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), left, right);

  builder.SetInsertPoint(left);
  llvm::StoreInst *leftStore = builder.CreateStore(value, global);
  leftStore->setMetadata("notdec.register.access",
                         registerAccessMetadata(context, registerName));
  builder.CreateRetVoid();

  builder.SetInsertPoint(right);
  llvm::StoreInst *rightStore = builder.CreateStore(value, global);
  rightStore->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, registerName));
  builder.CreateRetVoid();

  return function;
}

llvm::Function *createTwoReturnPhiEquivalentStoreFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *global,
    const std::string &registerName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *direct =
      llvm::BasicBlock::Create(context, "direct", function);
  llvm::BasicBlock *split = llvm::BasicBlock::Create(context, "split", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right = llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *merged =
      llvm::BasicBlock::Create(context, "merged", function);

  llvm::Value *value = llvm::ConstantInt::get(global->getValueType(), 0x7777);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), direct, split);

  builder.SetInsertPoint(direct);
  llvm::StoreInst *directStore = builder.CreateStore(value, global);
  directStore->setMetadata("notdec.register.access",
                           registerAccessMetadata(context, registerName));
  builder.CreateRetVoid();

  builder.SetInsertPoint(split);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), left, right);

  builder.SetInsertPoint(left);
  builder.CreateBr(merged);

  builder.SetInsertPoint(right);
  builder.CreateBr(merged);

  builder.SetInsertPoint(merged);
  llvm::PHINode *phi =
      builder.CreatePHI(global->getValueType(), 2, "merged_value");
  phi->addIncoming(value, left);
  phi->addIncoming(value, right);
  llvm::StoreInst *phiStore = builder.CreateStore(phi, global);
  phiStore->setMetadata("notdec.register.access",
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

llvm::Function *createLinearPredecessorReturnStoreFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *global,
    const std::string &registerName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *middle =
      llvm::BasicBlock::Create(context, "middle", function);
  llvm::BasicBlock *exit = llvm::BasicBlock::Create(context, "exit", function);

  llvm::IRBuilder<> builder(entry);
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(global->getValueType(), 0x5555), global);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, registerName));
  builder.CreateBr(middle);

  builder.SetInsertPoint(middle);
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

llvm::Function *createInputTwoOutputReturnStoreFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *input,
    const std::string &inputRegisterName, llvm::GlobalVariable *first,
    const std::string &firstRegisterName, llvm::GlobalVariable *second,
    const std::string &secondRegisterName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *load =
      createExternalInputLoad(builder, input, inputRegisterName);

  llvm::StoreInst *firstStore = builder.CreateStore(
      builder.CreateAdd(load, llvm::ConstantInt::get(input->getValueType(), 1)),
      first);
  firstStore->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, firstRegisterName));

  llvm::StoreInst *secondStore = builder.CreateStore(
      builder.CreateAdd(load, llvm::ConstantInt::get(input->getValueType(), 2)),
      second);
  secondStore->setMetadata("notdec.register.access",
                           registerAccessMetadata(context, secondRegisterName));
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createTwoInputTwoOutputReturnStoreFunction(
    llvm::Module &module, const std::string &name, llvm::GlobalVariable *firstInput,
    const std::string &firstInputName, llvm::GlobalVariable *secondInput,
    const std::string &secondInputName, llvm::GlobalVariable *firstOutput,
    const std::string &firstOutputName, llvm::GlobalVariable *secondOutput,
    const std::string &secondOutputName) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *firstLoad =
      createExternalInputLoad(builder, firstInput, firstInputName);
  llvm::LoadInst *secondLoad =
      createExternalInputLoad(builder, secondInput, secondInputName);

  llvm::StoreInst *firstStore =
      builder.CreateStore(builder.CreateAdd(firstLoad, secondLoad), firstOutput);
  firstStore->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, firstOutputName));

  llvm::StoreInst *secondStore = builder.CreateStore(
      builder.CreateSub(firstLoad, secondLoad), secondOutput);
  secondStore->setMetadata("notdec.register.access",
                           registerAccessMetadata(context, secondOutputName));
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

bool hasRegisterStore(const llvm::Function &function, llvm::StringRef name) {
  std::string expected = ("name=" + name).str();
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &instruction : block) {
      if (!llvm::isa<llvm::StoreInst>(&instruction)) {
        continue;
      }
      llvm::MDNode *metadata = instruction.getMetadata("notdec.register.access");
      if (metadata == nullptr) {
        continue;
      }
      for (const llvm::MDOperand &operand : metadata->operands()) {
        auto *field = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
        if (field != nullptr && field->getString() == expected) {
          return true;
        }
      }
    }
  }
  return false;
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

llvm::MDNode *makeRecoveredPrototypeMetadataWithCounts(
    llvm::LLVMContext &context, llvm::StringRef model, uint64_t inputCount,
    uint64_t returnCount,
    llvm::ArrayRef<std::pair<llvm::StringRef, uint64_t>> inputs,
    llvm::ArrayRef<std::pair<llvm::StringRef, uint64_t>> returns) {
  std::vector<llvm::Metadata *> inputEntries;
  for (const auto &[name, slot] : inputs) {
    llvm::Metadata *fields[] = {
        llvm::MDString::get(context, ("name=" + name).str()),
        llvm::MDString::get(context, "slot=" + std::to_string(slot)),
    };
    inputEntries.push_back(llvm::MDNode::get(context, fields));
  }

  std::vector<llvm::Metadata *> returnEntries;
  for (const auto &[name, slot] : returns) {
    llvm::Metadata *fields[] = {
        llvm::MDString::get(context, ("name=" + name).str()),
        llvm::MDString::get(context, "slot=" + std::to_string(slot)),
    };
    returnEntries.push_back(llvm::MDNode::get(context, fields));
  }

  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, ("model=" + model).str()),
      llvm::MDString::get(context, "input_count=" + std::to_string(inputCount)),
      llvm::MDString::get(context,
                          "return_count=" + std::to_string(returnCount)),
      llvm::MDNode::get(context, inputEntries),
      llvm::MDNode::get(context, returnEntries),
  };
  return llvm::MDNode::get(context, fields);
}

llvm::MDNode *makeRecoveredPrototypeMetadata(
    llvm::LLVMContext &context, llvm::StringRef model,
    llvm::ArrayRef<std::pair<llvm::StringRef, uint64_t>> inputs,
    llvm::ArrayRef<std::pair<llvm::StringRef, uint64_t>> returns) {
  return makeRecoveredPrototypeMetadataWithCounts(
      context, model, inputs.size(), returns.size(), inputs, returns);
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

const notdec::bin2llvm::NativePrototypeModuleRewriteFunctionSummary *
findRewriteFunctionSummary(
    const std::vector<
        notdec::bin2llvm::NativePrototypeModuleRewriteFunctionSummary>
        &functions,
    const std::string &name) {
  for (const auto &function : functions) {
    if (function.FunctionName == name) {
      return &function;
    }
  }
  return nullptr;
}

} // namespace

int main() {
  llvm::LLVMContext context;
  llvm::Module module("native-prototype-recovery-test", context);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
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
  llvm::StoreInst *temporaryReturnStore = nullptr;
  llvm::StoreInst *temporaryRealReturnStore = nullptr;
  llvm::Function *temporaryReturnFunction =
      createTemporaryReturnStoreFunction(module, "return_rax_after_temporary",
                                         rax, "RAX", &temporaryReturnStore,
                                         &temporaryRealReturnStore);
  llvm::Function *usedReturnFunction =
      createReturnStoreFunction(module, "return_rax_used", rax, "RAX");
  createCallerFunction(module, "call_return_rax_used", usedReturnFunction);
  llvm::Function *nonReturnFunction =
      createReturnStoreFunction(module, "return_rbx", rbx, "RBX");
  llvm::Function *twoReturnFunction =
      createTwoReturnStoreFunction(module, "return_rax_twice", rax, "RAX");
  llvm::Function *sameValueReturnFunction =
      createTwoReturnSameValueStoreFunction(module,
                                            "return_rax_same_value_twice", rax,
                                            "RAX");
  llvm::Function *phiReturnFunction =
      createTwoReturnPhiEquivalentStoreFunction(module,
                                                "return_rax_phi_equivalent",
                                                rax, "RAX");
  llvm::Function *partialReturnFunction =
      createPartialReturnStoreFunction(module, "return_rax_partial", rax,
                                       "RAX");
  llvm::Function *uniquePredReturnFunction =
      createUniquePredecessorReturnStoreFunction(module,
                                                 "return_rax_unique_pred", rax,
                                                 "RAX");
  llvm::Function *linearPredReturnFunction =
      createLinearPredecessorReturnStoreFunction(
          module, "return_rax_linear_pred", rax, "RAX");
  llvm::Function *conflictingReturnFunction =
      createConflictingReturnStoreFunction(module, "return_rax_conflict", rax,
                                           "RAX");
  llvm::Function *twoOutputReturnFunction =
      createTwoOutputReturnStoreFunction(module, "return_rdx_rax_order", rdx,
                                         "RDX", rax, "RAX");
  llvm::Function *twoOutputUsedReturnFunction =
      createTwoOutputReturnStoreFunction(module, "return_rdx_rax_used", rdx,
                                         "RDX", rax, "RAX");
  llvm::LoadInst *twoOutputUsedRaxLoad = nullptr;
  llvm::LoadInst *twoOutputUsedRdxLoad = nullptr;
  createTwoReturnLoadCallerFunction(
      module, "call_return_rdx_rax_used", twoOutputUsedReturnFunction, rax,
      "RAX", rdx, "RDX", &twoOutputUsedRaxLoad, &twoOutputUsedRdxLoad);
  llvm::Function *twoOutputSharedReturnFunction =
      createTwoOutputReturnStoreFunction(module, "return_rdx_rax_shared", rdx,
                                         "RDX", rax, "RAX");
  llvm::LoadInst *twoOutputSharedRaxLoad = nullptr;
  createSharedSuccessorOneReturnLoadCallerFunction(
      module, "call_return_rdx_rax_shared", twoOutputSharedReturnFunction, rax,
      "RAX", &twoOutputSharedRaxLoad);
  llvm::Function *twoOutputSharedBothReturnFunction =
      createTwoOutputReturnStoreFunction(module, "return_rdx_rax_shared_both",
                                         rdx, "RDX", rax, "RAX");
  llvm::LoadInst *twoOutputSharedBothRaxLoad = nullptr;
  llvm::LoadInst *twoOutputSharedBothRdxLoad = nullptr;
  createSharedSuccessorTwoReturnLoadCallerFunction(
      module, "call_return_rdx_rax_shared_both",
      twoOutputSharedBothReturnFunction, rax, "RAX", rdx, "RDX",
      &twoOutputSharedBothRaxLoad, &twoOutputSharedBothRdxLoad);
  llvm::Function *inputTwoOutputFunction =
      createInputTwoOutputReturnStoreFunction(
          module, "input_rdi_return_rdx_rax", rdi, "RDI", rdx, "RDX", rax,
          "RAX");
  attachExternalInputs(*inputTwoOutputFunction, {{"RDI", rdi}});
  llvm::CallInst *inputTwoOutputOldCall = nullptr;
  llvm::LoadInst *inputTwoOutputRaxLoad = nullptr;
  llvm::LoadInst *inputTwoOutputRdxLoad = nullptr;
  createInputStoreTwoReturnLoadCallerFunction(
      module, "call_input_rdi_return_rdx_rax", inputTwoOutputFunction, rdi,
      "RDI", rax, "RAX", rdx, "RDX", &inputTwoOutputOldCall,
      &inputTwoOutputRaxLoad, &inputTwoOutputRdxLoad);
  llvm::Function *inputTwoOutputSharedFunction =
      createInputTwoOutputReturnStoreFunction(
          module, "input_rdi_return_rdx_rax_shared", rdi, "RDI", rdx, "RDX",
          rax, "RAX");
  attachExternalInputs(*inputTwoOutputSharedFunction, {{"RDI", rdi}});
  llvm::CallInst *inputTwoOutputSharedOldCall = nullptr;
  llvm::LoadInst *inputTwoOutputSharedRaxLoad = nullptr;
  createInputStoreSharedSuccessorReturnLoadCallerFunction(
      module, "call_input_rdi_return_rdx_rax_shared",
      inputTwoOutputSharedFunction, rdi, "RDI", rax, "RAX",
      &inputTwoOutputSharedOldCall, &inputTwoOutputSharedRaxLoad);
  llvm::Function *inputTwoOutputSharedBothFunction =
      createInputTwoOutputReturnStoreFunction(
          module, "input_rdi_return_rdx_rax_shared_both", rdi, "RDI", rdx,
          "RDX", rax, "RAX");
  attachExternalInputs(*inputTwoOutputSharedBothFunction, {{"RDI", rdi}});
  llvm::CallInst *inputTwoOutputSharedBothOldCall = nullptr;
  llvm::LoadInst *inputTwoOutputSharedBothRaxLoad = nullptr;
  llvm::LoadInst *inputTwoOutputSharedBothRdxLoad = nullptr;
  createInputStoreSharedSuccessorTwoReturnLoadCallerFunction(
      module, "call_input_rdi_return_rdx_rax_shared_both",
      inputTwoOutputSharedBothFunction, rdi, "RDI", rax, "RAX", rdx, "RDX",
      &inputTwoOutputSharedBothOldCall, &inputTwoOutputSharedBothRaxLoad,
      &inputTwoOutputSharedBothRdxLoad);
  llvm::Function *unusedInputTwoOutputFunction =
      createInputTwoOutputReturnStoreFunction(
          module, "input_rdi_return_rdx_rax_unused", rdi, "RDI", rdx, "RDX",
          rax, "RAX");
  attachExternalInputs(*unusedInputTwoOutputFunction, {{"RDI", rdi}});
  llvm::CallInst *unusedInputTwoOutputOldCall = nullptr;
  llvm::LoadInst *unusedInputTwoOutputRaxLoad = nullptr;
  llvm::Value *unusedInputTwoOutputArgument = nullptr;
  createInputStoreIntermediateReturnLoadCallerFunction(
      module, "call_input_rdi_return_rdx_rax_unused",
      unusedInputTwoOutputFunction, rdi, "RDI", rax, "RAX",
      &unusedInputTwoOutputOldCall, &unusedInputTwoOutputRaxLoad,
      &unusedInputTwoOutputArgument);
  llvm::Function *multiInputTwoOutputFunction =
      createTwoInputTwoOutputReturnStoreFunction(
          module, "input_rdi_rsi_return_rdx_rax", rdi, "RDI", rsi, "RSI",
          rdx, "RDX", rax, "RAX");
  attachExternalInputs(*multiInputTwoOutputFunction,
                       {{"RDI", rdi}, {"RSI", rsi}});
  llvm::CallInst *multiInputTwoOutputOldCall = nullptr;
  llvm::LoadInst *multiInputTwoOutputRaxLoad = nullptr;
  llvm::LoadInst *multiInputTwoOutputRdxLoad = nullptr;
  llvm::Value *multiInputTwoOutputRdiArgument = nullptr;
  llvm::Value *multiInputTwoOutputRsiArgument = nullptr;
  createTwoInputStoreTwoReturnLoadCallerFunction(
      module, "call_input_rdi_rsi_return_rdx_rax",
      multiInputTwoOutputFunction, rdi, "RDI", rsi, "RSI", rax, "RAX", rdx,
      "RDX", &multiInputTwoOutputOldCall, &multiInputTwoOutputRaxLoad,
      &multiInputTwoOutputRdxLoad, &multiInputTwoOutputRdiArgument,
      &multiInputTwoOutputRsiArgument);
  llvm::LoadInst *inputReturnLoad = nullptr;
  llvm::StoreInst *inputReturnStore = nullptr;
  llvm::Function *inputReturnFunction = createInputReturnFunction(
      module, "input_rdi_return_rax", rdi, "RDI", rax, "RAX",
      &inputReturnLoad, &inputReturnStore);
  attachExternalInputs(*inputReturnFunction, {{"RDI", rdi}});
  llvm::LoadInst *inputForwardReturnLoad = nullptr;
  llvm::StoreInst *inputForwardReturnStore = nullptr;
  llvm::Function *inputForwardReturnFunction =
      createInputForwardReturnFunction(module, "input_rdi_forward_return_rax",
                                       rdi, "RDI", rax, "RAX",
                                       &inputForwardReturnLoad,
                                       &inputForwardReturnStore);
  attachExternalInputs(*inputForwardReturnFunction, {{"RDI", rdi}});
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
  ok &= expect(summary.FunctionsSeen == 44, "unexpected function count");
  ok &= expect(summary.ExternalInputsSeen == 21,
               "unexpected external input count");
  ok &= expect(summary.InputCandidates == 18,
               "unexpected input candidate count");
  ok &= expect(summary.ReturnCandidates == 30,
               "unexpected return candidate count");
  ok &= expect(summary.RewriteEligibleFunctions == 44,
               "unexpected rewrite eligible function count");
  ok &= expect(summary.SignatureRewriteNeededFunctions == 28,
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
  ok &= expect(metadataHasRegister(*phiReturnFunction,
                                   "notdec.prototype.return_candidates", "RAX"),
               "phi-equivalent RAX return was not marked as a candidate");
  ok &= expect(!metadataHasRegister(*partialReturnFunction,
                                    "notdec.prototype.return_candidates",
                                    "RAX"),
               "partial RAX return was incorrectly marked as a candidate");
  ok &= expect(metadataHasRegister(*uniquePredReturnFunction,
                                   "notdec.prototype.return_candidates", "RAX"),
               "unique predecessor RAX return was not marked as a candidate");
  ok &= expect(metadataHasRegister(*linearPredReturnFunction,
                                   "notdec.prototype.return_candidates", "RAX"),
               "linear predecessor RAX return was not marked as a candidate");
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
  ok &= expect(usedInputRewriteResult.Rewritten,
               "input-only prototype with register global callsite load was not rewritten");
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
  ok &= expect(bindableInputFunction != nullptr &&
                   bindableInputFunction->getMetadata(
                       "notdec.register.external_inputs") == nullptr &&
                   bindableInputFunction->getMetadata(
                       "notdec.prototype.input_candidates") == nullptr,
               "rewritten input-only function kept transient input metadata");
  ok &= expect(bindableInputFunction != nullptr &&
                   bindableInputFunction->getMetadata(
                       "notdec.prototype.recovered") != nullptr,
               "rewritten input-only function lost recovered prototype metadata");

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

  llvm::Module callerEntryInputModule(
      "native-prototype-input-caller-entry-value-test", context);
  llvm::GlobalVariable *callerEntryInputRdi =
      createRegisterGlobal(callerEntryInputModule, "RDI");
  attachTestAbi(callerEntryInputModule);
  llvm::LoadInst *callerEntryInputCalleeLoad = nullptr;
  llvm::Function *callerEntryInputCallee = createUsedExternalInputFunction(
      callerEntryInputModule, "caller_entry_input_rdi", callerEntryInputRdi,
      "RDI", &callerEntryInputCalleeLoad);
  attachExternalInputs(*callerEntryInputCallee, {{"RDI", callerEntryInputRdi}});
  auto *callerEntryInputType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callerEntryInputCaller = llvm::Function::Create(
      callerEntryInputType, llvm::GlobalValue::ExternalLinkage,
      "call_caller_entry_input_rdi", callerEntryInputModule);
  llvm::BasicBlock *callerEntryInputEntry =
      llvm::BasicBlock::Create(context, "entry", callerEntryInputCaller);
  llvm::IRBuilder<> callerEntryInputBuilder(callerEntryInputEntry);
  llvm::LoadInst *callerEntryInputLoad =
      createExternalInputLoad(callerEntryInputBuilder, callerEntryInputRdi, "RDI");
  callerEntryInputBuilder.CreateCall(callerEntryInputCallee->getFunctionType(),
                                     callerEntryInputCallee);
  callerEntryInputBuilder.CreateRetVoid();
  attachExternalInputs(*callerEntryInputCaller, {{"RDI", callerEntryInputRdi}});
  notdec::bin2llvm::runNativePrototypeRecovery(callerEntryInputModule, options);
  notdec::bin2llvm::NativePrototypeRewriteResult callerEntryInputResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputOnly(
          *callerEntryInputCallee);
  ok &= expect(callerEntryInputResult.Rewritten,
               "input-only prototype did not use caller entry input value");
  callerEntryInputCallee = callerEntryInputResult.Function;
  llvm::CallInst *callerEntryInputCall = nullptr;
  for (llvm::BasicBlock &block : *callerEntryInputCaller) {
    for (llvm::Instruction &instruction : block) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
      if (call != nullptr && call->getCalledFunction() == callerEntryInputCallee) {
        callerEntryInputCall = call;
      }
    }
  }
  ok &= expect(callerEntryInputCall != nullptr &&
                   callerEntryInputCall->arg_size() == 1 &&
                   callerEntryInputCall->getArgOperand(0) == callerEntryInputLoad,
               "caller entry input load was not passed to direct callee");
  if (llvm::verifyModule(callerEntryInputModule, &llvm::errs())) {
    std::cerr << "caller entry input module verification failed after "
                 "input-only rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Module rewrittenCallerArgModule(
      "native-prototype-input-rewritten-caller-argument-test", context);
  llvm::GlobalVariable *rewrittenCallerArgRdi =
      createRegisterGlobal(rewrittenCallerArgModule, "RDI");
  attachTestAbi(rewrittenCallerArgModule);
  llvm::LoadInst *rewrittenCallerArgInputLoad = nullptr;
  llvm::Function *rewrittenCallerArgCallee = createUsedExternalInputFunction(
      rewrittenCallerArgModule, "rewritten_caller_arg_input_rdi",
      rewrittenCallerArgRdi, "RDI", &rewrittenCallerArgInputLoad);
  attachExternalInputs(*rewrittenCallerArgCallee,
                       {{"RDI", rewrittenCallerArgRdi}});
  auto *rewrittenCallerArgType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *rewrittenCallerArgCaller = llvm::Function::Create(
      rewrittenCallerArgType, llvm::GlobalValue::ExternalLinkage,
      "call_rewritten_caller_arg_input_rdi", rewrittenCallerArgModule);
  rewrittenCallerArgCaller->getArg(0)->setName("RDI.external_input");
  llvm::BasicBlock *rewrittenCallerArgEntry =
      llvm::BasicBlock::Create(context, "entry", rewrittenCallerArgCaller);
  llvm::IRBuilder<> rewrittenCallerArgBuilder(rewrittenCallerArgEntry);
  rewrittenCallerArgBuilder.CreateCall(rewrittenCallerArgCallee->getFunctionType(),
                                       rewrittenCallerArgCallee);
  rewrittenCallerArgBuilder.CreateRetVoid();
  notdec::bin2llvm::runNativePrototypeRecovery(rewrittenCallerArgModule, options);
  rewrittenCallerArgCaller->setMetadata(
      "notdec.prototype.recovered",
      makeRecoveredPrototypeMetadata(context, "__stdcall", {{"RDI", 0}}, {}));
  notdec::bin2llvm::NativePrototypeRewriteResult rewrittenCallerArgResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputOnly(
          *rewrittenCallerArgCallee);
  ok &= expect(rewrittenCallerArgResult.Rewritten,
               "input-only prototype did not use rewritten caller argument");
  rewrittenCallerArgCallee = rewrittenCallerArgResult.Function;
  llvm::CallInst *rewrittenCallerArgCall = nullptr;
  for (llvm::BasicBlock &block : *rewrittenCallerArgCaller) {
    for (llvm::Instruction &instruction : block) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
      if (call != nullptr &&
          call->getCalledFunction() == rewrittenCallerArgCallee) {
        rewrittenCallerArgCall = call;
      }
    }
  }
  ok &= expect(rewrittenCallerArgCall != nullptr &&
                   rewrittenCallerArgCall->arg_size() == 1 &&
                   rewrittenCallerArgCall->getArgOperand(0) ==
                       rewrittenCallerArgCaller->getArg(0),
               "rewritten caller argument was not passed to direct callee");
  if (llvm::verifyModule(rewrittenCallerArgModule, &llvm::errs())) {
    std::cerr << "rewritten caller argument module verification failed after "
                 "input-only rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Module intrinsicCallerArgModule(
      "native-prototype-input-intrinsic-caller-argument-test", context);
  llvm::GlobalVariable *intrinsicCallerArgRdi =
      createRegisterGlobal(intrinsicCallerArgModule, "RDI");
  attachTestAbi(intrinsicCallerArgModule);
  llvm::LoadInst *intrinsicCallerArgInputLoad = nullptr;
  llvm::Function *intrinsicCallerArgCallee = createUsedExternalInputFunction(
      intrinsicCallerArgModule, "intrinsic_caller_arg_input_rdi",
      intrinsicCallerArgRdi, "RDI", &intrinsicCallerArgInputLoad);
  attachExternalInputs(*intrinsicCallerArgCallee,
                       {{"RDI", intrinsicCallerArgRdi}});
  auto *intrinsicCallerArgType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *intrinsicCallerArgCaller = llvm::Function::Create(
      intrinsicCallerArgType, llvm::GlobalValue::ExternalLinkage,
      "call_intrinsic_caller_arg_input_rdi", intrinsicCallerArgModule);
  intrinsicCallerArgCaller->getArg(0)->setName("RDI.external_input");
  llvm::BasicBlock *intrinsicCallerArgEntry =
      llvm::BasicBlock::Create(context, "entry", intrinsicCallerArgCaller);
  llvm::BasicBlock *intrinsicCallerArgCallBlock =
      llvm::BasicBlock::Create(context, "call", intrinsicCallerArgCaller);
  llvm::BasicBlock *intrinsicCallerArgExitBlock =
      llvm::BasicBlock::Create(context, "exit", intrinsicCallerArgCaller);
  llvm::IRBuilder<> intrinsicCallerArgBuilder(intrinsicCallerArgEntry);
  llvm::FunctionCallee ctpop = llvm::Intrinsic::getOrInsertDeclaration(
      &intrinsicCallerArgModule, llvm::Intrinsic::ctpop,
      {llvm::Type::getInt64Ty(context)});
  intrinsicCallerArgBuilder.CreateCall(
      ctpop.getFunctionType(), ctpop.getCallee(),
      {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 7)});
  intrinsicCallerArgBuilder.CreateCondBr(
      llvm::ConstantInt::getTrue(context), intrinsicCallerArgCallBlock,
      intrinsicCallerArgExitBlock);

  intrinsicCallerArgBuilder.SetInsertPoint(intrinsicCallerArgCallBlock);
  intrinsicCallerArgBuilder.CreateCall(intrinsicCallerArgCallee->getFunctionType(),
                                       intrinsicCallerArgCallee);
  intrinsicCallerArgBuilder.CreateRetVoid();

  intrinsicCallerArgBuilder.SetInsertPoint(intrinsicCallerArgExitBlock);
  intrinsicCallerArgBuilder.CreateRetVoid();
  notdec::bin2llvm::runNativePrototypeRecovery(intrinsicCallerArgModule, options);
  intrinsicCallerArgCaller->setMetadata(
      "notdec.prototype.recovered",
      makeRecoveredPrototypeMetadata(context, "__stdcall", {{"RDI", 0}}, {}));
  notdec::bin2llvm::NativePrototypeRewriteResult intrinsicCallerArgResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputOnly(
          *intrinsicCallerArgCallee);
  ok &= expect(intrinsicCallerArgResult.Rewritten,
               "input-only prototype treated intrinsic as callsite clobber");
  intrinsicCallerArgCallee = intrinsicCallerArgResult.Function;
  llvm::CallInst *intrinsicCallerArgCall = nullptr;
  for (llvm::BasicBlock &block : *intrinsicCallerArgCaller) {
    for (llvm::Instruction &instruction : block) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
      if (call != nullptr &&
          call->getCalledFunction() == intrinsicCallerArgCallee) {
        intrinsicCallerArgCall = call;
      }
    }
  }
  ok &= expect(intrinsicCallerArgCall != nullptr &&
                   intrinsicCallerArgCall->arg_size() == 1 &&
                   intrinsicCallerArgCall->getArgOperand(0) ==
                       intrinsicCallerArgCaller->getArg(0),
               "intrinsic-separated caller argument was not passed to callee");
  if (llvm::verifyModule(intrinsicCallerArgModule, &llvm::errs())) {
    std::cerr << "intrinsic caller argument module verification failed after "
                 "input-only rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Module multiInputCallsiteModule(
      "native-prototype-multi-input-callsite-rewrite-test", context);
  llvm::GlobalVariable *multiInputRdi =
      createRegisterGlobal(multiInputCallsiteModule, "RDI");
  llvm::GlobalVariable *multiInputRsi =
      createRegisterGlobal(multiInputCallsiteModule, "RSI");
  attachTestAbi(multiInputCallsiteModule);
  llvm::LoadInst *multiInputRdiLoad = nullptr;
  llvm::LoadInst *multiInputRsiLoad = nullptr;
  llvm::Function *multiInputFunction = createTwoUsedExternalInputFunction(
      multiInputCallsiteModule, "callsite_input_rdi_rsi", multiInputRdi, "RDI",
      multiInputRsi, "RSI", &multiInputRdiLoad, &multiInputRsiLoad);
  attachExternalInputs(*multiInputFunction,
                       {{"RDI", multiInputRdi}, {"RSI", multiInputRsi}});
  llvm::CallInst *oldMultiInputCall = nullptr;
  llvm::Value *firstMultiInputArgument = nullptr;
  llvm::Value *secondMultiInputArgument = nullptr;
  createTwoInputStoreCallerFunction(
      multiInputCallsiteModule, "call_callsite_input_rdi_rsi",
      multiInputFunction, multiInputRdi, "RDI", multiInputRsi, "RSI",
      &oldMultiInputCall, &firstMultiInputArgument, &secondMultiInputArgument);
  notdec::bin2llvm::runNativePrototypeRecovery(multiInputCallsiteModule,
                                               options);
  llvm::Instruction *multiInputRdiUser =
      multiInputRdiLoad != nullptr && !multiInputRdiLoad->user_empty()
          ? llvm::dyn_cast<llvm::Instruction>(*multiInputRdiLoad->user_begin())
          : nullptr;
  llvm::Instruction *multiInputRsiUser =
      multiInputRsiLoad != nullptr && !multiInputRsiLoad->user_empty()
          ? llvm::dyn_cast<llvm::Instruction>(*multiInputRsiLoad->user_begin())
          : nullptr;
  notdec::bin2llvm::NativePrototypeRewriteResult multiInputRewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototype(*multiInputFunction);
  ok &= expect(multiInputRewriteResult.Rewritten,
               "multi-input prototype with direct callsite was not rewritten");
  multiInputFunction = multiInputRewriteResult.Function;
  llvm::Type *twoI64Params[] = {llvm::Type::getInt64Ty(context),
                                llvm::Type::getInt64Ty(context)};
  ok &= expect(multiInputFunction != nullptr &&
                   functionTypeShape(*multiInputFunction->getFunctionType(),
                                     llvm::Type::getVoidTy(context),
                                     twoI64Params),
               "callsite rewritten multi-input function type was not void(i64, i64)");
  llvm::CallInst *rewrittenMultiInputCall = nullptr;
  llvm::Function *multiInputCaller =
      multiInputCallsiteModule.getFunction("call_callsite_input_rdi_rsi");
  if (multiInputCaller != nullptr) {
    for (llvm::BasicBlock &block : *multiInputCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call != nullptr && call->getCalledFunction() == multiInputFunction) {
          rewrittenMultiInputCall = call;
        }
      }
    }
  }
  ok &= expect(rewrittenMultiInputCall != nullptr,
               "multi-input direct callsite was not rewritten to new callee");
  ok &= expect(rewrittenMultiInputCall != nullptr &&
                   rewrittenMultiInputCall->arg_size() == 2,
               "multi-input direct callsite did not get two arguments");
  ok &= expect(rewrittenMultiInputCall != nullptr &&
                   firstMultiInputArgument ==
                       rewrittenMultiInputCall->getArgOperand(0) &&
                   secondMultiInputArgument ==
                       rewrittenMultiInputCall->getArgOperand(1),
               "multi-input direct callsite arguments were not ABI ordered");
  ok &= expect(multiInputFunction != nullptr &&
                   multiInputFunction->arg_size() == 2 &&
                   multiInputRdiUser != nullptr &&
                   multiInputRdiUser->getOperand(0) ==
                       multiInputFunction->getArg(0) &&
                   multiInputRsiUser != nullptr &&
                   multiInputRsiUser->getOperand(1) ==
                       multiInputFunction->getArg(1),
               "multi-input function did not replace both external input loads");
  bool sawMultiInputLoadAfterRewrite = false;
  if (multiInputFunction != nullptr) {
    for (llvm::BasicBlock &block : *multiInputFunction) {
      for (llvm::Instruction &instruction : block) {
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        if (load != nullptr &&
            load->getMetadata("notdec.register.external_input") != nullptr) {
          sawMultiInputLoadAfterRewrite = true;
        }
      }
    }
  }
  ok &= expect(!sawMultiInputLoadAfterRewrite,
               "rewritten multi-input function kept old input loads");
  if (llvm::verifyModule(multiInputCallsiteModule, &llvm::errs())) {
    std::cerr
        << "callsite module verification failed after multi-input rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Module phiInputCallsiteModule(
      "native-prototype-input-predecessor-phi-callsite-test", context);
  llvm::GlobalVariable *phiInputRsi =
      createRegisterGlobal(phiInputCallsiteModule, "RSI");
  llvm::GlobalVariable *phiInputRdx =
      createRegisterGlobal(phiInputCallsiteModule, "RDX");
  llvm::LoadInst *phiInputRsiLoad = nullptr;
  llvm::LoadInst *phiInputRdxLoad = nullptr;
  llvm::Function *phiInputCallee = createTwoUsedExternalInputFunction(
      phiInputCallsiteModule, "phi_callsite_input_rsi_rdx", phiInputRsi, "RSI",
      phiInputRdx, "RDX", &phiInputRsiLoad, &phiInputRdxLoad);
  phiInputCallee->setMetadata(
      "notdec.prototype.recovered",
      makeRecoveredPrototypeMetadata(context, "__stdcall",
                                     {{"RSI", 1}, {"RDX", 2}}, {}));
  llvm::Function *phiInputCaller =
      createFunction(phiInputCallsiteModule, "call_phi_callsite_input_rsi_rdx");
  llvm::BasicBlock &phiInputEntry = phiInputCaller->getEntryBlock();
  phiInputEntry.getTerminator()->eraseFromParent();
  llvm::BasicBlock *phiInputLeft =
      llvm::BasicBlock::Create(context, "left", phiInputCaller);
  llvm::BasicBlock *phiInputRight =
      llvm::BasicBlock::Create(context, "right", phiInputCaller);
  llvm::BasicBlock *phiInputMerge =
      llvm::BasicBlock::Create(context, "merge", phiInputCaller);
  llvm::BasicBlock *phiInputCallBlock =
      llvm::BasicBlock::Create(context, "call", phiInputCaller);
  llvm::IRBuilder<> phiInputBuilder(&phiInputEntry);
  phiInputBuilder.CreateCondBr(llvm::ConstantInt::getTrue(context), phiInputLeft,
                               phiInputRight);
  phiInputBuilder.SetInsertPoint(phiInputLeft);
  phiInputBuilder.CreateBr(phiInputMerge);
  phiInputBuilder.SetInsertPoint(phiInputRight);
  phiInputBuilder.CreateBr(phiInputMerge);
  phiInputBuilder.SetInsertPoint(phiInputMerge);
  llvm::PHINode *phiInputRdxValue =
      phiInputBuilder.CreatePHI(llvm::Type::getInt64Ty(context), 2, "RDX.regssa");
  phiInputRdxValue->addIncoming(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0x1111),
      phiInputLeft);
  phiInputRdxValue->addIncoming(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0x2222),
      phiInputRight);
  llvm::Value *phiInputRsiValue =
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0x3333);
  llvm::StoreInst *phiInputRsiStore =
      phiInputBuilder.CreateStore(phiInputRsiValue, phiInputRsi);
  phiInputRsiStore->setMetadata("notdec.register.access",
                                registerAccessMetadata(context, "RSI"));
  phiInputBuilder.CreateBr(phiInputCallBlock);
  phiInputBuilder.SetInsertPoint(phiInputCallBlock);
  phiInputBuilder.CreateCall(phiInputCallee->getFunctionType(), phiInputCallee);
  phiInputBuilder.CreateRetVoid();
  notdec::bin2llvm::NativePrototypeRewriteResult phiInputRewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputOnly(*phiInputCallee);
  ok &= expect(phiInputRewriteResult.Rewritten,
               "multi-input prototype did not use predecessor register PHI");
  phiInputCallee = phiInputRewriteResult.Function;
  llvm::CallInst *phiInputCall = nullptr;
  for (llvm::BasicBlock &block : *phiInputCaller) {
    for (llvm::Instruction &instruction : block) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
      if (call != nullptr && call->getCalledFunction() == phiInputCallee) {
        phiInputCall = call;
      }
    }
  }
  ok &= expect(phiInputCall != nullptr && phiInputCall->arg_size() == 2 &&
                   phiInputCall->getArgOperand(0) == phiInputRsiValue &&
                   phiInputCall->getArgOperand(1) == phiInputRdxValue,
               "predecessor register PHI was not passed as callsite input");
  if (llvm::verifyModule(phiInputCallsiteModule, &llvm::errs())) {
    std::cerr << "predecessor PHI callsite module verification failed after "
                 "multi-input rewrite\n";
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

  llvm::Module linearPredecessorCallsiteModule(
      "native-prototype-input-linear-predecessor-callsite-rewrite-test",
      context);
  llvm::GlobalVariable *linearPredecessorCallsiteRdi =
      createRegisterGlobal(linearPredecessorCallsiteModule, "RDI");
  attachTestAbi(linearPredecessorCallsiteModule);
  llvm::LoadInst *linearPredecessorCallsiteInputLoad = nullptr;
  llvm::Function *linearPredecessorCallsiteInputFunction =
      createUsedExternalInputFunction(linearPredecessorCallsiteModule,
                                      "linear_predecessor_callsite_input_rdi",
                                      linearPredecessorCallsiteRdi, "RDI",
                                      &linearPredecessorCallsiteInputLoad);
  attachExternalInputs(*linearPredecessorCallsiteInputFunction,
                       {{"RDI", linearPredecessorCallsiteRdi}});
  llvm::CallInst *oldLinearPredecessorCallsiteCall = nullptr;
  createInputStoreLinearPredecessorCallerFunction(
      linearPredecessorCallsiteModule,
      "call_linear_predecessor_callsite_input_rdi",
      linearPredecessorCallsiteInputFunction, linearPredecessorCallsiteRdi,
      "RDI", &oldLinearPredecessorCallsiteCall);
  notdec::bin2llvm::runNativePrototypeRecovery(linearPredecessorCallsiteModule,
                                               options);
  notdec::bin2llvm::NativePrototypeRewriteResult
      linearPredecessorCallsiteRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputOnly(
              *linearPredecessorCallsiteInputFunction);
  ok &= expect(linearPredecessorCallsiteRewriteResult.Rewritten,
               "input-only prototype with linear predecessor callsite was not rewritten");
  linearPredecessorCallsiteInputFunction =
      linearPredecessorCallsiteRewriteResult.Function;
  llvm::CallInst *rewrittenLinearPredecessorCallsiteCall = nullptr;
  llvm::Function *linearPredecessorCallsiteCaller =
      linearPredecessorCallsiteModule.getFunction(
          "call_linear_predecessor_callsite_input_rdi");
  if (linearPredecessorCallsiteCaller != nullptr) {
    for (llvm::BasicBlock &block : *linearPredecessorCallsiteCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call != nullptr && call->getCalledFunction() ==
                                   linearPredecessorCallsiteInputFunction) {
          rewrittenLinearPredecessorCallsiteCall = call;
        }
      }
    }
  }
  ok &= expect(rewrittenLinearPredecessorCallsiteCall != nullptr,
               "linear predecessor callsite was not rewritten to new callee");
  ok &= expect(rewrittenLinearPredecessorCallsiteCall != nullptr &&
                   rewrittenLinearPredecessorCallsiteCall->arg_size() == 1,
               "linear predecessor callsite did not get one argument");
  ok &= expect(rewrittenLinearPredecessorCallsiteCall != nullptr &&
                   llvm::isa<llvm::ConstantInt>(
                       rewrittenLinearPredecessorCallsiteCall->getArgOperand(0)),
               "linear predecessor callsite argument did not use register store value");
  if (llvm::verifyModule(linearPredecessorCallsiteModule, &llvm::errs())) {
    std::cerr << "linear predecessor callsite module verification failed after "
                 "input-only rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Module equivalentPredecessorCallsiteModule(
      "native-prototype-input-equivalent-predecessor-callsite-rewrite-test",
      context);
  llvm::GlobalVariable *equivalentPredecessorCallsiteRdi =
      createRegisterGlobal(equivalentPredecessorCallsiteModule, "RDI");
  attachTestAbi(equivalentPredecessorCallsiteModule);
  llvm::LoadInst *equivalentPredecessorCallsiteInputLoad = nullptr;
  llvm::Function *equivalentPredecessorCallsiteInputFunction =
      createUsedExternalInputFunction(equivalentPredecessorCallsiteModule,
                                      "equivalent_predecessor_callsite_input_rdi",
                                      equivalentPredecessorCallsiteRdi, "RDI",
                                      &equivalentPredecessorCallsiteInputLoad);
  attachExternalInputs(*equivalentPredecessorCallsiteInputFunction,
                       {{"RDI", equivalentPredecessorCallsiteRdi}});
  llvm::CallInst *oldEquivalentPredecessorCallsiteCall = nullptr;
  createInputStoreEquivalentPredecessorCallerFunction(
      equivalentPredecessorCallsiteModule,
      "call_equivalent_predecessor_callsite_input_rdi",
      equivalentPredecessorCallsiteInputFunction,
      equivalentPredecessorCallsiteRdi, "RDI",
      &oldEquivalentPredecessorCallsiteCall);
  notdec::bin2llvm::runNativePrototypeRecovery(
      equivalentPredecessorCallsiteModule, options);
  notdec::bin2llvm::NativePrototypeRewriteResult
      equivalentPredecessorCallsiteRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputOnly(
              *equivalentPredecessorCallsiteInputFunction);
  ok &= expect(equivalentPredecessorCallsiteRewriteResult.Rewritten,
               "input-only prototype with equivalent predecessors was not rewritten");
  equivalentPredecessorCallsiteInputFunction =
      equivalentPredecessorCallsiteRewriteResult.Function;
  llvm::CallInst *rewrittenEquivalentPredecessorCallsiteCall = nullptr;
  llvm::Function *equivalentPredecessorCallsiteCaller =
      equivalentPredecessorCallsiteModule.getFunction(
          "call_equivalent_predecessor_callsite_input_rdi");
  if (equivalentPredecessorCallsiteCaller != nullptr) {
    for (llvm::BasicBlock &block : *equivalentPredecessorCallsiteCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call != nullptr && call->getCalledFunction() ==
                                   equivalentPredecessorCallsiteInputFunction) {
          rewrittenEquivalentPredecessorCallsiteCall = call;
        }
      }
    }
  }
  ok &= expect(rewrittenEquivalentPredecessorCallsiteCall != nullptr,
               "equivalent predecessor callsite was not rewritten to new callee");
  ok &= expect(rewrittenEquivalentPredecessorCallsiteCall != nullptr &&
                   rewrittenEquivalentPredecessorCallsiteCall->arg_size() == 1,
               "equivalent predecessor callsite did not get one argument");
  ok &= expect(rewrittenEquivalentPredecessorCallsiteCall != nullptr &&
                   llvm::isa<llvm::ConstantInt>(
                       rewrittenEquivalentPredecessorCallsiteCall->getArgOperand(0)),
               "equivalent predecessor callsite argument did not use register store value");
  if (llvm::verifyModule(equivalentPredecessorCallsiteModule, &llvm::errs())) {
    std::cerr << "equivalent predecessor callsite module verification failed "
                 "after input-only rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Module conflictingPredecessorCallsiteModule(
      "native-prototype-input-conflicting-predecessor-callsite-test", context);
  llvm::GlobalVariable *conflictingPredecessorCallsiteRdi =
      createRegisterGlobal(conflictingPredecessorCallsiteModule, "RDI");
  attachTestAbi(conflictingPredecessorCallsiteModule);
  llvm::LoadInst *conflictingPredecessorCallsiteInputLoad = nullptr;
  llvm::Function *conflictingPredecessorCallsiteInputFunction =
      createUsedExternalInputFunction(
          conflictingPredecessorCallsiteModule,
          "conflicting_predecessor_callsite_input_rdi",
          conflictingPredecessorCallsiteRdi, "RDI",
          &conflictingPredecessorCallsiteInputLoad);
  attachExternalInputs(*conflictingPredecessorCallsiteInputFunction,
                       {{"RDI", conflictingPredecessorCallsiteRdi}});
  createInputStoreConflictingPredecessorCallerFunction(
      conflictingPredecessorCallsiteModule,
      "call_conflicting_predecessor_callsite_input_rdi",
      conflictingPredecessorCallsiteInputFunction,
      conflictingPredecessorCallsiteRdi, "RDI");
  notdec::bin2llvm::runNativePrototypeRecovery(
      conflictingPredecessorCallsiteModule, options);
  notdec::bin2llvm::NativePrototypeRewriteResult
      conflictingPredecessorCallsiteRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputOnly(
              *conflictingPredecessorCallsiteInputFunction);
  ok &= expect(!conflictingPredecessorCallsiteRewriteResult.Rewritten,
               "input-only prototype with conflicting predecessors was rewritten");
  ok &= expect(conflictingPredecessorCallsiteRewriteResult.Reason ==
                   "unsafe callsite input value",
               "conflicting predecessor callsite had wrong skip reason");
  if (llvm::verifyModule(conflictingPredecessorCallsiteModule, &llvm::errs())) {
    std::cerr << "conflicting predecessor callsite module verification failed\n";
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
  ok &= expect(missingInputCallsiteRewriteResult.Rewritten,
               "input-only prototype did not use register global callsite load");
  missingInputCallsiteFunction = missingInputCallsiteRewriteResult.Function;
  llvm::CallInst *missingInputCallsiteCall = nullptr;
  llvm::Function *missingInputCallsiteCaller =
      missingInputCallsiteModule.getFunction("call_missing_callsite_input_rdi");
  if (missingInputCallsiteCaller != nullptr) {
    for (llvm::BasicBlock &block : *missingInputCallsiteCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call != nullptr &&
            call->getCalledFunction() == missingInputCallsiteFunction) {
          missingInputCallsiteCall = call;
        }
      }
    }
  }
  auto *missingInputCallsiteArgLoad =
      missingInputCallsiteCall != nullptr && missingInputCallsiteCall->arg_size() == 1
          ? llvm::dyn_cast<llvm::LoadInst>(
                missingInputCallsiteCall->getArgOperand(0))
          : nullptr;
  ok &= expect(missingInputCallsiteArgLoad != nullptr &&
                   missingInputCallsiteArgLoad->getPointerOperand() ==
                       missingInputCallsiteRdi,
               "register global callsite load was not passed to callee");
  if (llvm::verifyModule(missingInputCallsiteModule, &llvm::errs())) {
    std::cerr << "missing input callsite module verification failed\n";
    return EXIT_FAILURE;
  }

  llvm::Module ambiguousInputCallsiteModule(
      "native-prototype-input-ambiguous-register-global-test", context);
  llvm::GlobalVariable *ambiguousInputCallsiteRdi =
      createRegisterGlobal(ambiguousInputCallsiteModule, "RDI");
  attachTestAbi(ambiguousInputCallsiteModule);
  llvm::LoadInst *ambiguousInputCallsiteInputLoad = nullptr;
  llvm::Function *ambiguousInputCallsiteFunction =
      createUsedExternalInputFunction(ambiguousInputCallsiteModule,
                                      "ambiguous_callsite_input_rdi",
                                      ambiguousInputCallsiteRdi, "RDI",
                                      &ambiguousInputCallsiteInputLoad);
  attachExternalInputs(*ambiguousInputCallsiteFunction,
                       {{"RDI", ambiguousInputCallsiteRdi}});
  createCallerFunction(ambiguousInputCallsiteModule,
                       "call_ambiguous_callsite_input_rdi",
                       ambiguousInputCallsiteFunction);
  notdec::bin2llvm::runNativePrototypeRecovery(ambiguousInputCallsiteModule,
                                               options);
  llvm::GlobalVariable *ambiguousInputCallsiteSecondRdi =
      createRegisterGlobal(ambiguousInputCallsiteModule, "RDI_shadow");
  ambiguousInputCallsiteSecondRdi->setMetadata(
      "notdec.register", registerAccessMetadata(context, "RDI"));
  notdec::bin2llvm::NativePrototypeRewriteResult
      ambiguousInputCallsiteRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputOnly(
              *ambiguousInputCallsiteFunction);
  ok &= expect(!ambiguousInputCallsiteRewriteResult.Rewritten,
               "ambiguous register global callsite input was rewritten");
  ok &= expect(ambiguousInputCallsiteRewriteResult.Reason ==
                   "unsafe callsite input value",
               "ambiguous register global callsite input had wrong skip reason");
  if (llvm::verifyModule(ambiguousInputCallsiteModule, &llvm::errs())) {
    std::cerr << "ambiguous input callsite module verification failed\n";
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
  notdec::bin2llvm::NativePrototypeRewriteResult matchingRewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototype(*matchingInputFunction);
  ok &= expect(!matchingRewriteResult.Rewritten,
               "matching input prototype was rewritten");
  ok &= expect(matchingRewriteResult.Reason == "already matches",
               "matching input prototype rewrite had unexpected reason");
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
    ok &= expect((*returnBindings)[0].ReturnStores.size() == 1,
                 "return binding used wrong store list size");
    ok &= expect((*returnBindings)[0].ReturnValue ==
                     returnStore->getValueOperand(),
                 "return binding used wrong value");
  }
  std::optional<std::vector<notdec::bin2llvm::NativePrototypeReturnBinding>>
      temporaryReturnBindings =
          notdec::bin2llvm::getNativePrototypeReturnBindings(
              *temporaryReturnFunction);
  ok &= expect(temporaryReturnBindings.has_value(),
               "temporary return store blocked return binding");
  if (temporaryReturnBindings) {
    ok &= expect(temporaryReturnBindings->size() == 1,
                 "temporary return binding had wrong count");
    ok &= expect((*temporaryReturnBindings)[0].ReturnStore ==
                     temporaryRealReturnStore,
                 "temporary return binding used wrong store");
    ok &= expect((*temporaryReturnBindings)[0].ReturnStore !=
                     temporaryReturnStore,
                 "temporary return binding used temporary store");
    ok &= expect((*temporaryReturnBindings)[0].ReturnStores.size() == 1,
                 "temporary return binding included non-return store");
  }
  std::optional<std::vector<notdec::bin2llvm::NativePrototypeReturnBinding>>
      duplicateReturnBindings =
          notdec::bin2llvm::getNativePrototypeReturnBindings(
              *twoReturnFunction);
  ok &= expect(duplicateReturnBindings.has_value(),
               "equivalent duplicate return stores were not bound");
  if (duplicateReturnBindings) {
    ok &= expect(duplicateReturnBindings->size() == 1,
                 "duplicate return stores had wrong binding count");
    ok &= expect((*duplicateReturnBindings)[0].ReturnStores.size() == 2,
                 "duplicate return stores had wrong store list size");
  }
  std::optional<std::vector<notdec::bin2llvm::NativePrototypeReturnBinding>>
      sameValueReturnBindings =
          notdec::bin2llvm::getNativePrototypeReturnBindings(
              *sameValueReturnFunction);
  ok &= expect(sameValueReturnBindings.has_value(),
               "same-value duplicate return stores were not bound");
  if (sameValueReturnBindings) {
    ok &= expect(sameValueReturnBindings->size() == 1,
                 "same-value duplicate return stores had wrong binding count");
    ok &= expect((*sameValueReturnBindings)[0].ReturnStores.size() == 2,
                 "same-value duplicate return stores had wrong store list size");
  }
  std::optional<std::vector<notdec::bin2llvm::NativePrototypeReturnBinding>>
      phiReturnBindings =
          notdec::bin2llvm::getNativePrototypeReturnBindings(
              *phiReturnFunction);
  ok &= expect(phiReturnBindings.has_value(),
               "phi-equivalent duplicate return stores were not bound");
  if (phiReturnBindings) {
    ok &= expect(phiReturnBindings->size() == 1,
                 "phi-equivalent duplicate return stores had wrong binding count");
    ok &= expect((*phiReturnBindings)[0].ReturnStores.size() == 2,
                 "phi-equivalent duplicate return stores had wrong store list size");
  }
  ok &= expect(!notdec::bin2llvm::getNativePrototypeReturnBindings(
                    *conflictingReturnFunction),
               "conflicting return stores were incorrectly bound");
  notdec::bin2llvm::NativePrototypeRewriteResult duplicateReturnRewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(
          *twoReturnFunction);
  ok &= expect(duplicateReturnRewriteResult.Rewritten,
               "equivalent duplicate return stores were not rewritten");
  twoReturnFunction = duplicateReturnRewriteResult.Function;
  ok &= expect(twoReturnFunction != nullptr &&
                   functionTypeShape(*twoReturnFunction->getFunctionType(),
                                     llvm::Type::getInt64Ty(context),
                                     llvm::ArrayRef<llvm::Type *>{}),
               "rewritten duplicate-return function type was not i64()");
  if (llvm::verifyModule(module, &llvm::errs())) {
    std::cerr << "module verification failed after duplicate-return rewrite\n";
    return EXIT_FAILURE;
  }
  notdec::bin2llvm::NativePrototypeRewriteResult sameValueReturnRewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(
          *sameValueReturnFunction);
  ok &= expect(sameValueReturnRewriteResult.Rewritten,
               "same-value duplicate return stores were not rewritten");
  sameValueReturnFunction = sameValueReturnRewriteResult.Function;
  ok &= expect(sameValueReturnFunction != nullptr &&
                   functionTypeShape(*sameValueReturnFunction->getFunctionType(),
                                     llvm::Type::getInt64Ty(context),
                                     llvm::ArrayRef<llvm::Type *>{}),
               "rewritten same-value duplicate-return function type was not i64()");
  if (llvm::verifyModule(module, &llvm::errs())) {
    std::cerr << "module verification failed after same-value return rewrite\n";
    return EXIT_FAILURE;
  }
  notdec::bin2llvm::NativePrototypeRewriteResult phiReturnRewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(
          *phiReturnFunction);
  ok &= expect(phiReturnRewriteResult.Rewritten,
               "phi-equivalent duplicate return stores were not rewritten");
  phiReturnFunction = phiReturnRewriteResult.Function;
  ok &= expect(phiReturnFunction != nullptr &&
                   functionTypeShape(*phiReturnFunction->getFunctionType(),
                                     llvm::Type::getInt64Ty(context),
                                     llvm::ArrayRef<llvm::Type *>{}),
               "rewritten phi-equivalent duplicate-return function type was not i64()");
  if (llvm::verifyModule(module, &llvm::errs())) {
    std::cerr << "module verification failed after phi-equivalent return rewrite\n";
    return EXIT_FAILURE;
  }
  notdec::bin2llvm::NativePrototypeRewriteResult temporaryReturnRewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(
          *temporaryReturnFunction);
  ok &= expect(temporaryReturnRewriteResult.Rewritten,
               "temporary return store function was not rewritten");
  temporaryReturnFunction = temporaryReturnRewriteResult.Function;
  ok &= expect(temporaryReturnFunction != nullptr &&
                   functionTypeShape(*temporaryReturnFunction->getFunctionType(),
                                     llvm::Type::getInt64Ty(context),
                                     llvm::ArrayRef<llvm::Type *>{}),
               "rewritten temporary-return function type was not i64()");
  if (llvm::verifyModule(module, &llvm::errs())) {
    std::cerr << "module verification failed after temporary return rewrite\n";
    return EXIT_FAILURE;
  }
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
  ok &= expect(returnCallsiteFunction != nullptr &&
                   !hasRegisterStore(*returnCallsiteFunction, "RAX"),
               "callsite rewritten return function kept old RAX store");
  ok &= expect(returnCallsiteFunction != nullptr &&
                   returnCallsiteFunction->getMetadata(
                       "notdec.prototype.return_candidates") == nullptr,
               "callsite rewritten return function kept transient return metadata");
  ok &= expect(returnCallsiteFunction != nullptr &&
                   returnCallsiteFunction->getMetadata(
                       "notdec.prototype.recovered") != nullptr,
               "callsite rewritten return function lost recovered prototype metadata");
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

  llvm::Module unmarkedReturnCallsiteModule(
      "native-prototype-return-unmarked-callsite-rewrite-test", context);
  llvm::GlobalVariable *unmarkedReturnCallsiteRax =
      createRegisterGlobal(unmarkedReturnCallsiteModule, "RAX");
  attachTestAbi(unmarkedReturnCallsiteModule);
  llvm::StoreInst *unmarkedReturnCallsiteStore = nullptr;
  llvm::Function *unmarkedReturnCallsiteFunction = createReturnStoreFunction(
      unmarkedReturnCallsiteModule, "unmarked_callsite_return_rax",
      unmarkedReturnCallsiteRax, "RAX", &unmarkedReturnCallsiteStore);
  llvm::LoadInst *unmarkedReturnCallsiteLoad = nullptr;
  createUnmarkedReturnLoadCallerFunction(
      unmarkedReturnCallsiteModule, "call_unmarked_callsite_return_rax",
      unmarkedReturnCallsiteFunction, unmarkedReturnCallsiteRax,
      &unmarkedReturnCallsiteLoad);
  notdec::bin2llvm::runNativePrototypeRecovery(unmarkedReturnCallsiteModule,
                                               options);
  notdec::bin2llvm::NativePrototypeRewriteResult
      unmarkedReturnCallsiteRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(
              *unmarkedReturnCallsiteFunction);
  ok &= expect(unmarkedReturnCallsiteRewriteResult.Rewritten,
               "return-only prototype with unmarked return load was not "
               "rewritten");
  ok &= expect(unmarkedReturnCallsiteLoad->use_empty(),
               "unmarked return register load was not replaced");
  if (llvm::verifyModule(unmarkedReturnCallsiteModule, &llvm::errs())) {
    std::cerr << "unmarked return callsite module verification failed after "
                 "rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Module intermediateCallReturnCallsiteModule(
      "native-prototype-return-intermediate-call-callsite-rewrite-test",
      context);
  llvm::GlobalVariable *intermediateCallReturnCallsiteRax =
      createRegisterGlobal(intermediateCallReturnCallsiteModule, "RAX");
  attachTestAbi(intermediateCallReturnCallsiteModule);
  llvm::StoreInst *intermediateCallReturnCallsiteStore = nullptr;
  llvm::Function *intermediateCallReturnCallsiteFunction =
      createReturnStoreFunction(intermediateCallReturnCallsiteModule,
                                "intermediate_call_callsite_return_rax",
                                intermediateCallReturnCallsiteRax, "RAX",
                                &intermediateCallReturnCallsiteStore);
  llvm::LoadInst *intermediateCallReturnCallsiteLoad = nullptr;
  createIntermediateCallReturnLoadCallerFunction(
      intermediateCallReturnCallsiteModule,
      "call_intermediate_call_callsite_return_rax",
      intermediateCallReturnCallsiteFunction, intermediateCallReturnCallsiteRax,
      "RAX", &intermediateCallReturnCallsiteLoad);
  notdec::bin2llvm::runNativePrototypeRecovery(
      intermediateCallReturnCallsiteModule, options);
  notdec::bin2llvm::NativePrototypeRewriteResult
      intermediateCallReturnCallsiteRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(
              *intermediateCallReturnCallsiteFunction);
  ok &= expect(intermediateCallReturnCallsiteRewriteResult.Rewritten,
               "return-only prototype with intermediate clobbering call was "
               "not rewritten");
  ok &= expect(!intermediateCallReturnCallsiteLoad->use_empty(),
               "intermediate-call return register load was unexpectedly "
               "replaced");
  if (llvm::verifyModule(intermediateCallReturnCallsiteModule, &llvm::errs())) {
    std::cerr << "intermediate call return callsite module verification failed "
                 "after rewrite\n";
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

  llvm::Module unusedMultiSuccessorReturnCallsiteModule(
      "native-prototype-return-unused-multi-successor-callsite-rewrite-test",
      context);
  llvm::GlobalVariable *unusedMultiSuccessorReturnCallsiteRax =
      createRegisterGlobal(unusedMultiSuccessorReturnCallsiteModule, "RAX");
  attachTestAbi(unusedMultiSuccessorReturnCallsiteModule);
  llvm::StoreInst *unusedMultiSuccessorReturnCallsiteStore = nullptr;
  llvm::Function *unusedMultiSuccessorReturnCallsiteFunction =
      createReturnStoreFunction(unusedMultiSuccessorReturnCallsiteModule,
                                "unused_multi_successor_callsite_return_rax",
                                unusedMultiSuccessorReturnCallsiteRax, "RAX",
                                &unusedMultiSuccessorReturnCallsiteStore);
  createUnusedReturnMultiSuccessorCallerFunction(
      unusedMultiSuccessorReturnCallsiteModule,
      "call_unused_multi_successor_callsite_return_rax",
      unusedMultiSuccessorReturnCallsiteFunction);
  notdec::bin2llvm::runNativePrototypeRecovery(
      unusedMultiSuccessorReturnCallsiteModule, options);
  notdec::bin2llvm::NativePrototypeRewriteResult
      unusedMultiSuccessorReturnCallsiteRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(
              *unusedMultiSuccessorReturnCallsiteFunction);
  ok &= expect(unusedMultiSuccessorReturnCallsiteRewriteResult.Rewritten,
               "return-only prototype with unused multi-successor callsite was not rewritten");
  unusedMultiSuccessorReturnCallsiteFunction =
      unusedMultiSuccessorReturnCallsiteRewriteResult.Function;
  llvm::CallInst *rewrittenUnusedMultiSuccessorReturnCallsiteCall = nullptr;
  llvm::Function *unusedMultiSuccessorReturnCallsiteCaller =
      unusedMultiSuccessorReturnCallsiteModule.getFunction(
          "call_unused_multi_successor_callsite_return_rax");
  if (unusedMultiSuccessorReturnCallsiteCaller != nullptr) {
    for (llvm::BasicBlock &block : *unusedMultiSuccessorReturnCallsiteCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call != nullptr &&
            call->getCalledFunction() ==
                unusedMultiSuccessorReturnCallsiteFunction) {
          rewrittenUnusedMultiSuccessorReturnCallsiteCall = call;
        }
      }
    }
  }
  ok &= expect(rewrittenUnusedMultiSuccessorReturnCallsiteCall != nullptr,
               "unused multi-successor return callsite was not rewritten to new callee");
  ok &= expect(rewrittenUnusedMultiSuccessorReturnCallsiteCall != nullptr &&
                   rewrittenUnusedMultiSuccessorReturnCallsiteCall->use_empty(),
               "unused multi-successor return callsite result was unexpectedly used");
  if (llvm::verifyModule(unusedMultiSuccessorReturnCallsiteModule,
                         &llvm::errs())) {
    std::cerr << "unused multi-successor return callsite module verification "
                 "failed after return-only rewrite\n";
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
  ok &= expect(clobberReturnCallsiteRewriteResult.Rewritten,
               "return-only prototype rewrite rejected unused clobbered return");
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
  notdec::bin2llvm::runNativePrototypeRecovery(
      multiSuccessorReturnCallsiteModule, options);
  notdec::bin2llvm::NativePrototypeRewriteResult
      multiSuccessorReturnCallsiteRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(
              *multiSuccessorReturnCallsiteFunction);
  ok &= expect(multiSuccessorReturnCallsiteRewriteResult.Rewritten,
               "return-only prototype rewrite rejected mixed multi-successor "
               "callsite");
  ok &= expect(multiSuccessorReturnCallsiteLoad->use_empty(),
               "mixed multi-successor return load was not replaced");
  if (llvm::verifyModule(multiSuccessorReturnCallsiteModule, &llvm::errs())) {
    std::cerr << "mixed multi-successor return callsite module verification "
                 "failed after rewrite\n";
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
  notdec::bin2llvm::runNativePrototypeRecovery(
      multiPredecessorReturnCallsiteModule, options);
  notdec::bin2llvm::NativePrototypeRewriteResult
      multiPredecessorReturnCallsiteRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(
              *multiPredecessorReturnCallsiteFunction);
  ok &= expect(multiPredecessorReturnCallsiteRewriteResult.Rewritten,
               "return-only prototype rewrite rejected shared-successor return load");
  multiPredecessorReturnCallsiteFunction =
      multiPredecessorReturnCallsiteRewriteResult.Function;
  llvm::CallInst *rewrittenMultiPredecessorReturnCallsiteCall = nullptr;
  llvm::PHINode *multiPredecessorReturnPhi = nullptr;
  llvm::Function *multiPredecessorReturnCallsiteCaller =
      multiPredecessorReturnCallsiteModule.getFunction(
          "call_multi_predecessor_callsite_return_rax");
  if (multiPredecessorReturnCallsiteCaller != nullptr) {
    for (llvm::BasicBlock &block : *multiPredecessorReturnCallsiteCaller) {
      for (llvm::Instruction &instruction : block) {
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
          if (call->getCalledFunction() ==
              multiPredecessorReturnCallsiteFunction) {
            rewrittenMultiPredecessorReturnCallsiteCall = call;
          }
        }
        if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
          multiPredecessorReturnPhi = phi;
        }
      }
    }
  }
  ok &= expect(rewrittenMultiPredecessorReturnCallsiteCall != nullptr,
               "shared-successor return callsite was not rewritten to new callee");
  bool phiHasCallIncoming = false;
  if (multiPredecessorReturnPhi != nullptr &&
      rewrittenMultiPredecessorReturnCallsiteCall != nullptr) {
    for (llvm::Value *incoming : multiPredecessorReturnPhi->incoming_values()) {
      if (incoming == rewrittenMultiPredecessorReturnCallsiteCall) {
        phiHasCallIncoming = true;
      }
    }
  }
  ok &= expect(multiPredecessorReturnPhi != nullptr &&
                   multiPredecessorReturnPhi->getNumIncomingValues() == 2 &&
                   phiHasCallIncoming,
               "shared-successor return load was not replaced with call-result PHI");
  ok &= expect(multiPredecessorReturnCallsiteLoad->use_empty(),
               "shared-successor return load was not erased");
  if (llvm::verifyModule(multiPredecessorReturnCallsiteModule, &llvm::errs())) {
    std::cerr << "shared-successor return callsite module verification failed "
                 "after rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Module unusedSharedSuccessorReturnCallsiteModule(
      "native-prototype-return-unused-shared-successor-callsite-test", context);
  llvm::GlobalVariable *unusedSharedSuccessorReturnCallsiteRax =
      createRegisterGlobal(unusedSharedSuccessorReturnCallsiteModule, "RAX");
  attachTestAbi(unusedSharedSuccessorReturnCallsiteModule);
  llvm::Function *unusedSharedSuccessorReturnCallsiteFunction =
      createReturnStoreFunction(unusedSharedSuccessorReturnCallsiteModule,
                                "unused_shared_successor_callsite_return_rax",
                                unusedSharedSuccessorReturnCallsiteRax, "RAX");
  createUnusedReturnSharedSuccessorCallerFunction(
      unusedSharedSuccessorReturnCallsiteModule,
      "call_unused_shared_successor_callsite_return_rax",
      unusedSharedSuccessorReturnCallsiteFunction);
  notdec::bin2llvm::runNativePrototypeRecovery(
      unusedSharedSuccessorReturnCallsiteModule, options);
  notdec::bin2llvm::NativePrototypeRewriteResult
      unusedSharedSuccessorReturnCallsiteRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(
              *unusedSharedSuccessorReturnCallsiteFunction);
  ok &= expect(unusedSharedSuccessorReturnCallsiteRewriteResult.Rewritten,
               "return-only prototype with unused shared successor callsite was not rewritten");
  unusedSharedSuccessorReturnCallsiteFunction =
      unusedSharedSuccessorReturnCallsiteRewriteResult.Function;
  llvm::CallInst *rewrittenUnusedSharedSuccessorReturnCallsiteCall = nullptr;
  llvm::Function *unusedSharedSuccessorReturnCallsiteCaller =
      unusedSharedSuccessorReturnCallsiteModule.getFunction(
          "call_unused_shared_successor_callsite_return_rax");
  if (unusedSharedSuccessorReturnCallsiteCaller != nullptr) {
    for (llvm::BasicBlock &block : *unusedSharedSuccessorReturnCallsiteCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call != nullptr &&
            call->getCalledFunction() ==
                unusedSharedSuccessorReturnCallsiteFunction) {
          rewrittenUnusedSharedSuccessorReturnCallsiteCall = call;
        }
      }
    }
  }
  ok &= expect(rewrittenUnusedSharedSuccessorReturnCallsiteCall != nullptr,
               "unused shared successor return callsite was not rewritten to new callee");
  ok &= expect(rewrittenUnusedSharedSuccessorReturnCallsiteCall != nullptr &&
                   rewrittenUnusedSharedSuccessorReturnCallsiteCall->use_empty(),
               "unused shared successor return callsite result was unexpectedly used");
  if (llvm::verifyModule(unusedSharedSuccessorReturnCallsiteModule,
                         &llvm::errs())) {
    std::cerr << "unused shared successor return callsite module verification "
                 "failed after rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Module clobberSharedSuccessorReturnCallsiteModule(
      "native-prototype-return-clobber-shared-successor-callsite-test", context);
  llvm::GlobalVariable *clobberSharedSuccessorReturnCallsiteRax =
      createRegisterGlobal(clobberSharedSuccessorReturnCallsiteModule, "RAX");
  attachTestAbi(clobberSharedSuccessorReturnCallsiteModule);
  llvm::Function *clobberSharedSuccessorReturnCallsiteFunction =
      createReturnStoreFunction(clobberSharedSuccessorReturnCallsiteModule,
                                "clobber_shared_successor_callsite_return_rax",
                                clobberSharedSuccessorReturnCallsiteRax, "RAX");
  llvm::LoadInst *clobberSharedSuccessorReturnCallsiteLoad = nullptr;
  createClobberReturnSharedSuccessorCallerFunction(
      clobberSharedSuccessorReturnCallsiteModule,
      "call_clobber_shared_successor_callsite_return_rax",
      clobberSharedSuccessorReturnCallsiteFunction,
      clobberSharedSuccessorReturnCallsiteRax, "RAX",
      &clobberSharedSuccessorReturnCallsiteLoad);
  notdec::bin2llvm::runNativePrototypeRecovery(
      clobberSharedSuccessorReturnCallsiteModule, options);
  notdec::bin2llvm::NativePrototypeRewriteResult
      clobberSharedSuccessorReturnCallsiteRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(
              *clobberSharedSuccessorReturnCallsiteFunction);
  ok &= expect(clobberSharedSuccessorReturnCallsiteRewriteResult.Rewritten,
               "return-only prototype with clobbered shared successor callsite was not rewritten");
  clobberSharedSuccessorReturnCallsiteFunction =
      clobberSharedSuccessorReturnCallsiteRewriteResult.Function;
  llvm::CallInst *rewrittenClobberSharedSuccessorReturnCallsiteCall = nullptr;
  llvm::Function *clobberSharedSuccessorReturnCallsiteCaller =
      clobberSharedSuccessorReturnCallsiteModule.getFunction(
          "call_clobber_shared_successor_callsite_return_rax");
  if (clobberSharedSuccessorReturnCallsiteCaller != nullptr) {
    for (llvm::BasicBlock &block : *clobberSharedSuccessorReturnCallsiteCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call != nullptr &&
            call->getCalledFunction() ==
                clobberSharedSuccessorReturnCallsiteFunction) {
          rewrittenClobberSharedSuccessorReturnCallsiteCall = call;
        }
      }
    }
  }
  ok &= expect(rewrittenClobberSharedSuccessorReturnCallsiteCall != nullptr,
               "clobbered shared successor return callsite was not rewritten to new callee");
  ok &= expect(rewrittenClobberSharedSuccessorReturnCallsiteCall != nullptr &&
                   rewrittenClobberSharedSuccessorReturnCallsiteCall->use_empty(),
               "clobbered shared successor return callsite result was unexpectedly used");
  ok &= expect(!clobberSharedSuccessorReturnCallsiteLoad->use_empty(),
               "clobbered shared successor return load was unexpectedly replaced");
  if (llvm::verifyModule(clobberSharedSuccessorReturnCallsiteModule,
                         &llvm::errs())) {
    std::cerr << "clobbered shared successor return callsite module verification "
                 "failed after rewrite\n";
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

  llvm::Module returnValueLoadModule(
      "native-prototype-return-value-load-rewrite-test", context);
  llvm::GlobalVariable *returnValueLoadRax =
      createRegisterGlobal(returnValueLoadModule, "RAX");
  attachTestAbi(returnValueLoadModule);
  llvm::LoadInst *returnValueLoad = nullptr;
  llvm::Function *returnValueLoadFunction = createReturnRegisterLoadFunction(
      returnValueLoadModule, "return_value_load_rax", returnValueLoadRax,
      "RAX", &returnValueLoad);
  notdec::bin2llvm::runNativePrototypeRecovery(returnValueLoadModule, options);
  notdec::bin2llvm::NativePrototypeRewriteResult returnValueLoadRewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypeReturnOnly(
          *returnValueLoadFunction);
  ok &= expect(!returnValueLoadRewriteResult.Rewritten,
               "return-only prototype rewrite accepted register-load return");
  ok &= expect(returnValueLoadRewriteResult.Reason ==
                   "unsafe return value load",
               "register-load return had wrong skip reason");
  ok &= expect(returnValueLoad != nullptr && !returnValueLoad->use_empty(),
               "register-load return value was unexpectedly erased");
  if (llvm::verifyModule(returnValueLoadModule, &llvm::errs())) {
    std::cerr << "return-value-load module verification failed after "
                 "return-only rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Value *returnFunctionValue = returnStore->getValueOperand();
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
                   rewrittenRet->getReturnValue() == returnFunctionValue,
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
  notdec::bin2llvm::NativePrototypeRewriteResult inputForwardRewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputReturn(
          *inputForwardReturnFunction);
  ok &= expect(inputForwardRewriteResult.Rewritten,
               "input-forward-return prototype was not rewritten");
  inputForwardReturnFunction = inputForwardRewriteResult.Function;
  llvm::ReturnInst *rewrittenInputForwardRet = nullptr;
  bool sawInputForwardLoadAfterRewrite = false;
  if (inputForwardReturnFunction != nullptr) {
    for (llvm::BasicBlock &block : *inputForwardReturnFunction) {
      if (auto *ret =
              llvm::dyn_cast_or_null<llvm::ReturnInst>(block.getTerminator())) {
        rewrittenInputForwardRet = ret;
      }
      for (llvm::Instruction &instruction : block) {
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        if (load != nullptr &&
            load->getMetadata("notdec.register.external_input") != nullptr) {
          sawInputForwardLoadAfterRewrite = true;
        }
      }
    }
  }
  ok &= expect(rewrittenInputForwardRet != nullptr,
               "rewritten input-forward-return function had no return");
  ok &= expect(inputForwardReturnFunction != nullptr &&
                   rewrittenInputForwardRet != nullptr &&
                   !inputForwardReturnFunction->arg_empty() &&
                   rewrittenInputForwardRet->getReturnValue() ==
                       &*inputForwardReturnFunction->arg_begin(),
               "rewritten input-forward-return did not return new argument");
  ok &= expect(!sawInputForwardLoadAfterRewrite,
               "rewritten input-forward-return kept old input load");
  if (llvm::verifyModule(module, &llvm::errs())) {
    std::cerr << "module verification failed after input-forward-return rewrite\n";
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

  llvm::Module sharedInputReturnCallsiteModule(
      "native-prototype-input-return-shared-successor-callsite-rewrite-test",
      context);
  llvm::GlobalVariable *sharedInputReturnCallsiteRdi =
      createRegisterGlobal(sharedInputReturnCallsiteModule, "RDI");
  llvm::GlobalVariable *sharedInputReturnCallsiteRax =
      createRegisterGlobal(sharedInputReturnCallsiteModule, "RAX");
  attachTestAbi(sharedInputReturnCallsiteModule);
  llvm::LoadInst *sharedCallsiteInputReturnLoad = nullptr;
  llvm::StoreInst *sharedCallsiteInputReturnStore = nullptr;
  llvm::Function *sharedCallsiteInputReturnFunction = createInputReturnFunction(
      sharedInputReturnCallsiteModule,
      "shared_callsite_input_rdi_return_rax",
      sharedInputReturnCallsiteRdi, "RDI", sharedInputReturnCallsiteRax, "RAX",
      &sharedCallsiteInputReturnLoad, &sharedCallsiteInputReturnStore);
  attachExternalInputs(*sharedCallsiteInputReturnFunction,
                       {{"RDI", sharedInputReturnCallsiteRdi}});
  llvm::CallInst *oldSharedInputReturnCallsiteCall = nullptr;
  llvm::LoadInst *oldSharedInputReturnCallsiteLoad = nullptr;
  createInputStoreSharedSuccessorReturnLoadCallerFunction(
      sharedInputReturnCallsiteModule,
      "call_shared_callsite_input_rdi_return_rax",
      sharedCallsiteInputReturnFunction, sharedInputReturnCallsiteRdi, "RDI",
      sharedInputReturnCallsiteRax, "RAX", &oldSharedInputReturnCallsiteCall,
      &oldSharedInputReturnCallsiteLoad);
  notdec::bin2llvm::runNativePrototypeRecovery(sharedInputReturnCallsiteModule,
                                               options);
  notdec::bin2llvm::NativePrototypeRewriteResult
      sharedInputReturnCallsiteRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputReturn(
              *sharedCallsiteInputReturnFunction);
  ok &= expect(sharedInputReturnCallsiteRewriteResult.Rewritten,
               "input-return prototype with shared-successor callsite was not rewritten");
  sharedCallsiteInputReturnFunction =
      sharedInputReturnCallsiteRewriteResult.Function;
  llvm::CallInst *rewrittenSharedInputReturnCallsiteCall = nullptr;
  llvm::PHINode *sharedInputReturnPhi = nullptr;
  llvm::Function *sharedInputReturnCallsiteCaller =
      sharedInputReturnCallsiteModule.getFunction(
          "call_shared_callsite_input_rdi_return_rax");
  if (sharedInputReturnCallsiteCaller != nullptr) {
    for (llvm::BasicBlock &block : *sharedInputReturnCallsiteCaller) {
      for (llvm::Instruction &instruction : block) {
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
          if (call->getCalledFunction() == sharedCallsiteInputReturnFunction) {
            rewrittenSharedInputReturnCallsiteCall = call;
          }
        }
        if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
          sharedInputReturnPhi = phi;
        }
      }
    }
  }
  ok &= expect(rewrittenSharedInputReturnCallsiteCall != nullptr,
               "shared input-return callsite was not rewritten to new callee");
  ok &= expect(rewrittenSharedInputReturnCallsiteCall != nullptr &&
                   rewrittenSharedInputReturnCallsiteCall->arg_size() == 1 &&
                   llvm::isa<llvm::ConstantInt>(
                       rewrittenSharedInputReturnCallsiteCall->getArgOperand(0)),
               "shared input-return callsite did not preserve input argument");
  bool sharedPhiHasCallIncoming = false;
  if (sharedInputReturnPhi != nullptr &&
      rewrittenSharedInputReturnCallsiteCall != nullptr) {
    for (llvm::Value *incoming : sharedInputReturnPhi->incoming_values()) {
      if (incoming == rewrittenSharedInputReturnCallsiteCall) {
        sharedPhiHasCallIncoming = true;
      }
    }
  }
  ok &= expect(sharedInputReturnPhi != nullptr &&
                   sharedInputReturnPhi->getNumIncomingValues() == 2 &&
                   sharedPhiHasCallIncoming,
               "shared input-return load was not replaced with call-result PHI");
  ok &= expect(oldSharedInputReturnCallsiteLoad->use_empty(),
               "shared input-return callsite kept old return register load");
  if (llvm::verifyModule(sharedInputReturnCallsiteModule, &llvm::errs())) {
    std::cerr << "shared input-return callsite module verification failed after "
                 "rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Module multiInputReturnCallsiteModule(
      "native-prototype-multi-input-return-callsite-rewrite-test", context);
  llvm::GlobalVariable *multiInputReturnRdi =
      createRegisterGlobal(multiInputReturnCallsiteModule, "RDI");
  llvm::GlobalVariable *multiInputReturnRsi =
      createRegisterGlobal(multiInputReturnCallsiteModule, "RSI");
  llvm::GlobalVariable *multiInputReturnRax =
      createRegisterGlobal(multiInputReturnCallsiteModule, "RAX");
  attachTestAbi(multiInputReturnCallsiteModule);
  llvm::LoadInst *multiInputReturnRdiLoad = nullptr;
  llvm::LoadInst *multiInputReturnRsiLoad = nullptr;
  llvm::StoreInst *multiInputReturnStore = nullptr;
  llvm::Function *multiInputReturnFunction = createTwoInputReturnFunction(
      multiInputReturnCallsiteModule, "callsite_input_rdi_rsi_return_rax",
      multiInputReturnRdi, "RDI", multiInputReturnRsi, "RSI",
      multiInputReturnRax, "RAX", &multiInputReturnRdiLoad,
      &multiInputReturnRsiLoad, &multiInputReturnStore);
  attachExternalInputs(*multiInputReturnFunction,
                       {{"RDI", multiInputReturnRdi},
                        {"RSI", multiInputReturnRsi}});
  llvm::CallInst *oldMultiInputReturnCall = nullptr;
  llvm::LoadInst *oldMultiInputReturnLoad = nullptr;
  llvm::Value *firstMultiInputReturnArgument = nullptr;
  llvm::Value *secondMultiInputReturnArgument = nullptr;
  createTwoInputStoreReturnLoadCallerFunction(
      multiInputReturnCallsiteModule,
      "call_callsite_input_rdi_rsi_return_rax", multiInputReturnFunction,
      multiInputReturnRdi, "RDI", multiInputReturnRsi, "RSI",
      multiInputReturnRax, "RAX", &oldMultiInputReturnCall,
      &oldMultiInputReturnLoad, &firstMultiInputReturnArgument,
      &secondMultiInputReturnArgument);
  notdec::bin2llvm::runNativePrototypeRecovery(multiInputReturnCallsiteModule,
                                               options);
  llvm::Instruction *multiInputReturnRdiUser =
      multiInputReturnRdiLoad != nullptr && !multiInputReturnRdiLoad->user_empty()
          ? llvm::dyn_cast<llvm::Instruction>(
                *multiInputReturnRdiLoad->user_begin())
          : nullptr;
  llvm::Instruction *multiInputReturnRsiUser =
      multiInputReturnRsiLoad != nullptr && !multiInputReturnRsiLoad->user_empty()
          ? llvm::dyn_cast<llvm::Instruction>(
                *multiInputReturnRsiLoad->user_begin())
          : nullptr;
  notdec::bin2llvm::NativePrototypeRewriteResult
      multiInputReturnRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototypeInputReturn(
              *multiInputReturnFunction);
  ok &= expect(multiInputReturnRewriteResult.Rewritten,
               "multi-input return prototype with direct callsite was not rewritten");
  multiInputReturnFunction = multiInputReturnRewriteResult.Function;
  llvm::Type *multiInputReturnParams[] = {llvm::Type::getInt64Ty(context),
                                          llvm::Type::getInt64Ty(context)};
  ok &= expect(multiInputReturnFunction != nullptr &&
                   functionTypeShape(
                       *multiInputReturnFunction->getFunctionType(),
                       llvm::Type::getInt64Ty(context),
                       multiInputReturnParams),
               "callsite rewritten multi-input return function type was not i64(i64, i64)");
  llvm::CallInst *rewrittenMultiInputReturnCall = nullptr;
  llvm::Function *multiInputReturnCaller =
      multiInputReturnCallsiteModule.getFunction(
          "call_callsite_input_rdi_rsi_return_rax");
  if (multiInputReturnCaller != nullptr) {
    for (llvm::BasicBlock &block : *multiInputReturnCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call != nullptr &&
            call->getCalledFunction() == multiInputReturnFunction) {
          rewrittenMultiInputReturnCall = call;
        }
      }
    }
  }
  ok &= expect(rewrittenMultiInputReturnCall != nullptr,
               "multi-input return direct callsite was not rewritten to new callee");
  ok &= expect(rewrittenMultiInputReturnCall != nullptr &&
                   rewrittenMultiInputReturnCall->arg_size() == 2,
               "multi-input return direct callsite did not get two arguments");
  ok &= expect(rewrittenMultiInputReturnCall != nullptr &&
                   firstMultiInputReturnArgument ==
                       rewrittenMultiInputReturnCall->getArgOperand(0) &&
                   secondMultiInputReturnArgument ==
                       rewrittenMultiInputReturnCall->getArgOperand(1),
               "multi-input return direct callsite arguments were not ABI ordered");
  ok &= expect(rewrittenMultiInputReturnCall != nullptr &&
                   rewrittenMultiInputReturnCall->getType() ==
                       llvm::Type::getInt64Ty(context),
               "multi-input return direct callsite did not return i64");
  ok &= expect(multiInputReturnFunction != nullptr &&
                   multiInputReturnFunction->arg_size() == 2 &&
                   multiInputReturnRdiUser != nullptr &&
                   multiInputReturnRdiUser->getOperand(0) ==
                       multiInputReturnFunction->getArg(0) &&
                   multiInputReturnRsiUser != nullptr &&
                   multiInputReturnRsiUser->getOperand(1) ==
                       multiInputReturnFunction->getArg(1),
               "multi-input return function did not replace both input loads");
  bool sawOldMultiInputReturnLoad = false;
  if (multiInputReturnCaller != nullptr) {
    for (llvm::BasicBlock &block : *multiInputReturnCaller) {
      for (llvm::Instruction &instruction : block) {
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        if (load != nullptr &&
            load->getMetadata("notdec.register.access") != nullptr) {
          sawOldMultiInputReturnLoad = true;
        }
      }
    }
  }
  ok &= expect(!sawOldMultiInputReturnLoad,
               "multi-input return direct callsite kept old return register load");
  ok &= expect(rewrittenMultiInputReturnCall != nullptr &&
                   !rewrittenMultiInputReturnCall->use_empty(),
               "multi-input return direct callsite result was not used");
  if (llvm::verifyModule(multiInputReturnCallsiteModule, &llvm::errs())) {
    std::cerr << "callsite module verification failed after multi-input "
                 "return rewrite\n";
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
  ok &= expect(dispatchMissingResult.Reason ==
                   "already matches",
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

  notdec::bin2llvm::NativePrototypeRewriteResult usedMultiReturnRewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototype(
          *twoOutputUsedReturnFunction);
  ok &= expect(usedMultiReturnRewriteResult.Rewritten,
               "used multi-return prototype was not rewritten");
  twoOutputUsedReturnFunction = usedMultiReturnRewriteResult.Function;
  llvm::Function *twoOutputUsedCaller =
      module.getFunction("call_return_rdx_rax_used");
  llvm::CallInst *rewrittenMultiReturnCall = nullptr;
  uint64_t multiReturnExtracts = 0;
  bool sawOldMultiReturnLoad = false;
  if (twoOutputUsedCaller != nullptr) {
    for (llvm::BasicBlock &block : *twoOutputUsedCaller) {
      for (llvm::Instruction &instruction : block) {
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
          if (call->getCalledFunction() == twoOutputUsedReturnFunction) {
            rewrittenMultiReturnCall = call;
          }
        }
        if (llvm::isa<llvm::ExtractValueInst>(&instruction)) {
          ++multiReturnExtracts;
        }
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        if (load != nullptr &&
            load->getMetadata("notdec.register.access") != nullptr) {
          sawOldMultiReturnLoad = true;
        }
      }
    }
  }
  ok &= expect(rewrittenMultiReturnCall != nullptr,
               "used multi-return callsite was not rewritten to new callee");
  ok &= expect(rewrittenMultiReturnCall != nullptr &&
                   llvm::isa<llvm::StructType>(
                       rewrittenMultiReturnCall->getType()),
               "used multi-return call did not return struct");
  ok &= expect(multiReturnExtracts == 2,
               "used multi-return callsite did not extract both fields");
  ok &= expect(!sawOldMultiReturnLoad,
               "used multi-return callsite kept old return register load");

  notdec::bin2llvm::NativePrototypeRewriteResult
      sharedMultiReturnRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototype(
              *twoOutputSharedReturnFunction);
  ok &= expect(sharedMultiReturnRewriteResult.Rewritten,
               "multi-return shared-successor prototype was not rewritten");
  twoOutputSharedReturnFunction = sharedMultiReturnRewriteResult.Function;
  llvm::Function *twoOutputSharedCaller =
      module.getFunction("call_return_rdx_rax_shared");
  llvm::CallInst *rewrittenSharedMultiReturnCall = nullptr;
  llvm::PHINode *sharedMultiReturnPhi = nullptr;
  uint64_t sharedMultiReturnExtracts = 0;
  if (twoOutputSharedCaller != nullptr) {
    for (llvm::BasicBlock &block : *twoOutputSharedCaller) {
      for (llvm::Instruction &instruction : block) {
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
          if (call->getCalledFunction() == twoOutputSharedReturnFunction) {
            rewrittenSharedMultiReturnCall = call;
          }
        }
        if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
          sharedMultiReturnPhi = phi;
        }
        if (llvm::isa<llvm::ExtractValueInst>(&instruction)) {
          ++sharedMultiReturnExtracts;
        }
      }
    }
  }
  bool sharedMultiReturnPhiHasExtract = false;
  if (sharedMultiReturnPhi != nullptr) {
    for (llvm::Value *incoming : sharedMultiReturnPhi->incoming_values()) {
      if (llvm::isa<llvm::ExtractValueInst>(incoming)) {
        sharedMultiReturnPhiHasExtract = true;
      }
    }
  }
  ok &= expect(rewrittenSharedMultiReturnCall != nullptr,
               "shared multi-return callsite was not rewritten to new callee");
  ok &= expect(rewrittenSharedMultiReturnCall != nullptr &&
                   llvm::isa<llvm::StructType>(
                       rewrittenSharedMultiReturnCall->getType()),
               "shared multi-return call did not return struct");
  ok &= expect(sharedMultiReturnExtracts == 1,
               "shared multi-return callsite did not extract exactly one used field");
  ok &= expect(sharedMultiReturnPhi != nullptr &&
                   sharedMultiReturnPhi->getNumIncomingValues() == 2 &&
                   sharedMultiReturnPhiHasExtract,
               "shared multi-return load was not replaced with extractvalue PHI");
  ok &= expect(twoOutputSharedRaxLoad->use_empty(),
               "shared multi-return callsite kept old return register load");

  notdec::bin2llvm::NativePrototypeRewriteResult
      sharedBothMultiReturnRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototype(
              *twoOutputSharedBothReturnFunction);
  ok &= expect(sharedBothMultiReturnRewriteResult.Rewritten,
               "dual shared multi-return prototype was not rewritten");
  twoOutputSharedBothReturnFunction =
      sharedBothMultiReturnRewriteResult.Function;
  llvm::Function *twoOutputSharedBothCaller =
      module.getFunction("call_return_rdx_rax_shared_both");
  llvm::CallInst *rewrittenSharedBothMultiReturnCall = nullptr;
  uint64_t sharedBothMultiReturnExtracts = 0;
  uint64_t sharedBothMultiReturnPhis = 0;
  uint64_t sharedBothMultiReturnPhiExtracts = 0;
  bool sawOldSharedBothRaxLoad = false;
  bool sawOldSharedBothRdxLoad = false;
  if (twoOutputSharedBothCaller != nullptr) {
    for (llvm::BasicBlock &block : *twoOutputSharedBothCaller) {
      for (llvm::Instruction &instruction : block) {
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
          if (call->getCalledFunction() == twoOutputSharedBothReturnFunction) {
            rewrittenSharedBothMultiReturnCall = call;
          }
        }
        if (llvm::isa<llvm::ExtractValueInst>(&instruction)) {
          ++sharedBothMultiReturnExtracts;
        }
        if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
          ++sharedBothMultiReturnPhis;
          for (llvm::Value *incoming : phi->incoming_values()) {
            if (llvm::isa<llvm::ExtractValueInst>(incoming)) {
              ++sharedBothMultiReturnPhiExtracts;
            }
          }
        }
        if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
          if (load->getName() == "RAX.return_value") {
            sawOldSharedBothRaxLoad = true;
          }
          if (load->getName() == "RDX.return_value") {
            sawOldSharedBothRdxLoad = true;
          }
        }
      }
    }
  }
  ok &= expect(rewrittenSharedBothMultiReturnCall != nullptr,
               "dual shared multi-return callsite was not rewritten to new callee");
  ok &= expect(rewrittenSharedBothMultiReturnCall != nullptr &&
                   llvm::isa<llvm::StructType>(
                       rewrittenSharedBothMultiReturnCall->getType()),
               "dual shared multi-return call did not return struct");
  ok &= expect(sharedBothMultiReturnExtracts == 2,
               "dual shared multi-return callsite did not extract both fields");
  ok &= expect(sharedBothMultiReturnPhis == 2 &&
                   sharedBothMultiReturnPhiExtracts == 2,
               "dual shared multi-return loads were not replaced with extractvalue PHIs");
  ok &= expect(!sawOldSharedBothRaxLoad,
               "dual shared multi-return callsite kept old RAX return register load");
  ok &= expect(!sawOldSharedBothRdxLoad,
               "dual shared multi-return callsite kept old RDX return register load");

  notdec::bin2llvm::NativePrototypeRewriteResult inputMultiReturnRewriteResult =
      notdec::bin2llvm::rewriteNativeRecoveredPrototype(
          *inputTwoOutputFunction);
  ok &= expect(inputMultiReturnRewriteResult.Rewritten,
               "input multi-return prototype was not rewritten");
  inputTwoOutputFunction = inputMultiReturnRewriteResult.Function;
  llvm::Function *inputTwoOutputCaller =
      module.getFunction("call_input_rdi_return_rdx_rax");
  llvm::CallInst *rewrittenInputMultiReturnCall = nullptr;
  uint64_t inputMultiReturnExtracts = 0;
  bool sawOldInputMultiReturnLoad = false;
  if (inputTwoOutputCaller != nullptr) {
    for (llvm::BasicBlock &block : *inputTwoOutputCaller) {
      for (llvm::Instruction &instruction : block) {
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
          if (call->getCalledFunction() == inputTwoOutputFunction) {
            rewrittenInputMultiReturnCall = call;
          }
        }
        if (llvm::isa<llvm::ExtractValueInst>(&instruction)) {
          ++inputMultiReturnExtracts;
        }
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        if (load != nullptr &&
            load->getMetadata("notdec.register.access") != nullptr) {
          sawOldInputMultiReturnLoad = true;
        }
      }
    }
  }
  ok &= expect(inputTwoOutputFunction != nullptr &&
                   functionTypeShape(
                       *inputTwoOutputFunction->getFunctionType(),
                       inputTwoOutputFunction->getReturnType(),
                       llvm::ArrayRef(i64Param)),
               "input multi-return function type did not keep one i64 input");
  ok &= expect(inputTwoOutputFunction != nullptr &&
                   llvm::isa<llvm::StructType>(
                       inputTwoOutputFunction->getReturnType()),
               "input multi-return function did not return struct");
  ok &= expect(rewrittenInputMultiReturnCall != nullptr,
               "input multi-return callsite was not rewritten to new callee");
  ok &= expect(rewrittenInputMultiReturnCall != nullptr &&
                   rewrittenInputMultiReturnCall->arg_size() == 1,
               "input multi-return callsite did not get one argument");
  ok &= expect(rewrittenInputMultiReturnCall != nullptr &&
                   llvm::isa<llvm::StructType>(
                       rewrittenInputMultiReturnCall->getType()),
               "input multi-return direct call did not return struct");
  ok &= expect(inputMultiReturnExtracts == 2,
               "input multi-return callsite did not extract both fields");
  ok &= expect(!sawOldInputMultiReturnLoad,
               "input multi-return callsite kept old return register load");

  notdec::bin2llvm::NativePrototypeRewriteResult
      sharedInputMultiReturnRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototype(
              *inputTwoOutputSharedFunction);
  ok &= expect(sharedInputMultiReturnRewriteResult.Rewritten,
               "shared input multi-return prototype was not rewritten");
  inputTwoOutputSharedFunction = sharedInputMultiReturnRewriteResult.Function;
  llvm::Function *inputTwoOutputSharedCaller =
      module.getFunction("call_input_rdi_return_rdx_rax_shared");
  llvm::CallInst *rewrittenSharedInputMultiReturnCall = nullptr;
  llvm::PHINode *sharedInputMultiReturnPhi = nullptr;
  uint64_t sharedInputMultiReturnExtracts = 0;
  if (inputTwoOutputSharedCaller != nullptr) {
    for (llvm::BasicBlock &block : *inputTwoOutputSharedCaller) {
      for (llvm::Instruction &instruction : block) {
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
          if (call->getCalledFunction() == inputTwoOutputSharedFunction) {
            rewrittenSharedInputMultiReturnCall = call;
          }
        }
        if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
          sharedInputMultiReturnPhi = phi;
        }
        if (llvm::isa<llvm::ExtractValueInst>(&instruction)) {
          ++sharedInputMultiReturnExtracts;
        }
      }
    }
  }
  bool sharedInputMultiReturnPhiHasExtract = false;
  if (sharedInputMultiReturnPhi != nullptr) {
    for (llvm::Value *incoming : sharedInputMultiReturnPhi->incoming_values()) {
      if (llvm::isa<llvm::ExtractValueInst>(incoming)) {
        sharedInputMultiReturnPhiHasExtract = true;
      }
    }
  }
  ok &= expect(rewrittenSharedInputMultiReturnCall != nullptr,
               "shared input multi-return callsite was not rewritten to new callee");
  ok &= expect(rewrittenSharedInputMultiReturnCall != nullptr &&
                   rewrittenSharedInputMultiReturnCall->arg_size() == 1,
               "shared input multi-return callsite did not get one argument");
  ok &= expect(rewrittenSharedInputMultiReturnCall != nullptr &&
                   llvm::isa<llvm::StructType>(
                       rewrittenSharedInputMultiReturnCall->getType()),
               "shared input multi-return direct call did not return struct");
  ok &= expect(sharedInputMultiReturnExtracts == 1,
               "shared input multi-return callsite did not extract exactly one used field");
  ok &= expect(sharedInputMultiReturnPhi != nullptr &&
                   sharedInputMultiReturnPhi->getNumIncomingValues() == 2 &&
                   sharedInputMultiReturnPhiHasExtract,
               "shared input multi-return load was not replaced with extractvalue PHI");
  ok &= expect(inputTwoOutputSharedRaxLoad->use_empty(),
               "shared input multi-return callsite kept old return register load");

  notdec::bin2llvm::NativePrototypeRewriteResult
      sharedBothInputMultiReturnRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototype(
              *inputTwoOutputSharedBothFunction);
  ok &= expect(sharedBothInputMultiReturnRewriteResult.Rewritten,
               "dual shared input multi-return prototype was not rewritten");
  inputTwoOutputSharedBothFunction =
      sharedBothInputMultiReturnRewriteResult.Function;
  llvm::Function *inputTwoOutputSharedBothCaller =
      module.getFunction("call_input_rdi_return_rdx_rax_shared_both");
  llvm::CallInst *rewrittenSharedBothInputMultiReturnCall = nullptr;
  uint64_t sharedBothInputMultiReturnExtracts = 0;
  uint64_t sharedBothInputMultiReturnPhis = 0;
  uint64_t sharedBothInputMultiReturnPhiExtracts = 0;
  bool sawOldSharedBothInputRaxLoad = false;
  bool sawOldSharedBothInputRdxLoad = false;
  if (inputTwoOutputSharedBothCaller != nullptr) {
    for (llvm::BasicBlock &block : *inputTwoOutputSharedBothCaller) {
      for (llvm::Instruction &instruction : block) {
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
          if (call->getCalledFunction() == inputTwoOutputSharedBothFunction) {
            rewrittenSharedBothInputMultiReturnCall = call;
          }
        }
        if (llvm::isa<llvm::ExtractValueInst>(&instruction)) {
          ++sharedBothInputMultiReturnExtracts;
        }
        if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
          ++sharedBothInputMultiReturnPhis;
          for (llvm::Value *incoming : phi->incoming_values()) {
            if (llvm::isa<llvm::ExtractValueInst>(incoming)) {
              ++sharedBothInputMultiReturnPhiExtracts;
            }
          }
        }
        if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
          if (load->getName() == "RAX.return_value") {
            sawOldSharedBothInputRaxLoad = true;
          }
          if (load->getName() == "RDX.return_value") {
            sawOldSharedBothInputRdxLoad = true;
          }
        }
      }
    }
  }
  ok &= expect(rewrittenSharedBothInputMultiReturnCall != nullptr,
               "dual shared input multi-return callsite was not rewritten to new callee");
  ok &= expect(rewrittenSharedBothInputMultiReturnCall != nullptr &&
                   rewrittenSharedBothInputMultiReturnCall->arg_size() == 1,
               "dual shared input multi-return callsite did not get one argument");
  ok &= expect(rewrittenSharedBothInputMultiReturnCall != nullptr &&
                   llvm::isa<llvm::StructType>(
                       rewrittenSharedBothInputMultiReturnCall->getType()),
               "dual shared input multi-return direct call did not return struct");
  ok &= expect(sharedBothInputMultiReturnExtracts == 2,
               "dual shared input multi-return callsite did not extract both fields");
  ok &= expect(sharedBothInputMultiReturnPhis == 2 &&
                   sharedBothInputMultiReturnPhiExtracts == 2,
               "dual shared input multi-return loads were not replaced with extractvalue PHIs");
  ok &= expect(!sawOldSharedBothInputRaxLoad,
               "dual shared input multi-return callsite kept old RAX return register load");
  ok &= expect(!sawOldSharedBothInputRdxLoad,
               "dual shared input multi-return callsite kept old RDX return register load");

  notdec::bin2llvm::NativePrototypeRewriteResult
      unusedInputMultiReturnRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototype(
              *unusedInputTwoOutputFunction);
  ok &= expect(unusedInputMultiReturnRewriteResult.Rewritten,
               "unused input multi-return prototype was not rewritten");
  unusedInputTwoOutputFunction = unusedInputMultiReturnRewriteResult.Function;
  llvm::Function *unusedInputTwoOutputCaller =
      module.getFunction("call_input_rdi_return_rdx_rax_unused");
  llvm::CallInst *rewrittenUnusedInputMultiReturnCall = nullptr;
  uint64_t unusedInputMultiReturnExtracts = 0;
  if (unusedInputTwoOutputCaller != nullptr) {
    for (llvm::BasicBlock &block : *unusedInputTwoOutputCaller) {
      for (llvm::Instruction &instruction : block) {
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
          if (call->getCalledFunction() == unusedInputTwoOutputFunction) {
            rewrittenUnusedInputMultiReturnCall = call;
          }
        }
        if (llvm::isa<llvm::ExtractValueInst>(&instruction)) {
          ++unusedInputMultiReturnExtracts;
        }
      }
    }
  }
  ok &= expect(rewrittenUnusedInputMultiReturnCall != nullptr,
               "unused input multi-return callsite was not rewritten to new callee");
  ok &= expect(rewrittenUnusedInputMultiReturnCall != nullptr &&
                   rewrittenUnusedInputMultiReturnCall->arg_size() == 1,
               "unused input multi-return callsite did not get one argument");
  ok &= expect(rewrittenUnusedInputMultiReturnCall != nullptr &&
                   unusedInputTwoOutputArgument ==
                       rewrittenUnusedInputMultiReturnCall->getArgOperand(0),
               "unused input multi-return callsite argument was not preserved");
  ok &= expect(rewrittenUnusedInputMultiReturnCall != nullptr &&
                   llvm::isa<llvm::StructType>(
                       rewrittenUnusedInputMultiReturnCall->getType()),
               "unused input multi-return direct call did not return struct");
  ok &= expect(rewrittenUnusedInputMultiReturnCall != nullptr &&
                   rewrittenUnusedInputMultiReturnCall->use_empty(),
               "unused input multi-return call result was unexpectedly used");
  ok &= expect(unusedInputMultiReturnExtracts == 0,
               "unused input multi-return callsite extracted unused fields");
  ok &= expect(unusedInputTwoOutputRaxLoad != nullptr &&
                   !unusedInputTwoOutputRaxLoad->use_empty(),
               "intermediate-call return load was unexpectedly replaced");

  notdec::bin2llvm::NativePrototypeRewriteResult
      multiInputMultiReturnRewriteResult =
          notdec::bin2llvm::rewriteNativeRecoveredPrototype(
              *multiInputTwoOutputFunction);
  ok &= expect(multiInputMultiReturnRewriteResult.Rewritten,
               "multi-input multi-return prototype was not rewritten");
  multiInputTwoOutputFunction = multiInputMultiReturnRewriteResult.Function;
  llvm::Function *multiInputTwoOutputCaller =
      module.getFunction("call_input_rdi_rsi_return_rdx_rax");
  llvm::CallInst *rewrittenMultiInputMultiReturnCall = nullptr;
  uint64_t multiInputMultiReturnExtracts = 0;
  bool sawOldMultiInputMultiReturnLoad = false;
  if (multiInputTwoOutputCaller != nullptr) {
    for (llvm::BasicBlock &block : *multiInputTwoOutputCaller) {
      for (llvm::Instruction &instruction : block) {
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
          if (call->getCalledFunction() == multiInputTwoOutputFunction) {
            rewrittenMultiInputMultiReturnCall = call;
          }
        }
        if (llvm::isa<llvm::ExtractValueInst>(&instruction)) {
          ++multiInputMultiReturnExtracts;
        }
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        if (load != nullptr &&
            load->getMetadata("notdec.register.access") != nullptr) {
          sawOldMultiInputMultiReturnLoad = true;
        }
      }
    }
  }
  llvm::Type *multiInputMultiReturnParams[] = {llvm::Type::getInt64Ty(context),
                                               llvm::Type::getInt64Ty(context)};
  ok &= expect(multiInputTwoOutputFunction != nullptr &&
                   functionTypeShape(
                       *multiInputTwoOutputFunction->getFunctionType(),
                       multiInputTwoOutputFunction->getReturnType(),
                       multiInputMultiReturnParams),
               "multi-input multi-return function type did not keep two i64 inputs");
  ok &= expect(multiInputTwoOutputFunction != nullptr &&
                   llvm::isa<llvm::StructType>(
                       multiInputTwoOutputFunction->getReturnType()),
               "multi-input multi-return function did not return struct");
  ok &= expect(multiInputTwoOutputFunction != nullptr &&
                   !hasRegisterStore(*multiInputTwoOutputFunction, "RAX") &&
                   !hasRegisterStore(*multiInputTwoOutputFunction, "RDX"),
               "multi-input multi-return function kept old return stores");
  ok &= expect(rewrittenMultiInputMultiReturnCall != nullptr,
               "multi-input multi-return callsite was not rewritten to new callee");
  ok &= expect(rewrittenMultiInputMultiReturnCall != nullptr &&
                   rewrittenMultiInputMultiReturnCall->arg_size() == 2,
               "multi-input multi-return callsite did not get two arguments");
  ok &= expect(rewrittenMultiInputMultiReturnCall != nullptr &&
                   multiInputTwoOutputRdiArgument ==
                       rewrittenMultiInputMultiReturnCall->getArgOperand(0) &&
                   multiInputTwoOutputRsiArgument ==
                       rewrittenMultiInputMultiReturnCall->getArgOperand(1),
               "multi-input multi-return callsite arguments were not ABI ordered");
  ok &= expect(rewrittenMultiInputMultiReturnCall != nullptr &&
                   llvm::isa<llvm::StructType>(
                       rewrittenMultiInputMultiReturnCall->getType()),
               "multi-input multi-return direct call did not return struct");
  ok &= expect(multiInputMultiReturnExtracts == 2,
               "multi-input multi-return callsite did not extract both fields");
  ok &= expect(!sawOldMultiInputMultiReturnLoad,
               "multi-input multi-return callsite kept old return register load");

  std::optional<notdec::bin2llvm::NativeRecoveredPrototype>
      emptyRecoveredPrototype =
          notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(
              *unusedInputFunction);
  ok &= expect(emptyRecoveredPrototype.has_value(),
               "empty recovered prototype metadata was not readable");
  if (emptyRecoveredPrototype) {
    ok &= expect(emptyRecoveredPrototype->Inputs.empty() &&
                     emptyRecoveredPrototype->Returns.empty(),
                 "empty recovered prototype had unexpected params");
  }
  notdec::bin2llvm::NativePrototypeRewriteEligibility missingEligibility =
      notdec::bin2llvm::getNativePrototypeRewriteEligibility(
          *unusedInputFunction);
  ok &= expect(missingEligibility.Eligible,
               "empty recovered prototype was not rewrite eligible");
  ok &= expect(!missingEligibility.NeedsRewrite,
               "empty recovered prototype incorrectly needed rewrite");
  ok &= expect(missingEligibility.Reason == "already matches",
               "empty recovered prototype had unexpected eligibility reason");

  llvm::Module noAbiModule("native-prototype-no-abi-stale-metadata-test",
                           context);
  llvm::Function *noAbiFunction = createFunction(noAbiModule, "no_abi_stale");
  llvm::MDNode *stalePrototypeMetadata =
      llvm::MDNode::get(context, llvm::MDString::get(context, "stale=true"));
  noAbiFunction->setMetadata("notdec.prototype.input_candidates",
                             stalePrototypeMetadata);
  noAbiFunction->setMetadata("notdec.prototype.return_candidates",
                             stalePrototypeMetadata);
  noAbiFunction->setMetadata("notdec.prototype.recovered",
                             stalePrototypeMetadata);
  notdec::bin2llvm::runNativePrototypeRecovery(noAbiModule, options);
  ok &= expect(noAbiFunction->getMetadata("notdec.prototype.input_candidates") ==
                   nullptr,
               "no-ABI module kept stale input candidate metadata");
  ok &= expect(noAbiFunction->getMetadata("notdec.prototype.return_candidates") ==
                   nullptr,
               "no-ABI module kept stale return candidate metadata");
  ok &= expect(noAbiFunction->getMetadata("notdec.prototype.recovered") ==
                   nullptr,
               "no-ABI module kept stale recovered prototype metadata");

  llvm::Module mismatchedMetadataModule(
      "native-prototype-mismatched-recovered-metadata-test", context);
  attachTestAbi(mismatchedMetadataModule);
  llvm::Function *mismatchedMetadataFunction =
      createFunction(mismatchedMetadataModule, "mismatched_recovered_metadata");
  mismatchedMetadataFunction->setMetadata(
      "notdec.prototype.recovered",
      makeRecoveredPrototypeMetadata(context, "__stdcall", {{"RDI", 0}}, {}));
  notdec::bin2llvm::runNativePrototypeRecovery(mismatchedMetadataModule,
                                               options);
  ok &= expect(mismatchedMetadataFunction->getMetadata(
                   "notdec.prototype.recovered") != nullptr,
               "mismatched recovered prototype metadata was not replaced");
  notdec::bin2llvm::NativePrototypeRewriteEligibility
      mismatchedMetadataEligibility =
          notdec::bin2llvm::getNativePrototypeRewriteEligibility(
              *mismatchedMetadataFunction);
  ok &= expect(mismatchedMetadataEligibility.Eligible,
               "replacement empty recovered metadata was not rewrite eligible");
  ok &= expect(!mismatchedMetadataEligibility.NeedsRewrite,
               "replacement empty recovered metadata incorrectly needed rewrite");
  ok &= expect(mismatchedMetadataEligibility.Reason == "already matches",
               "replacement empty recovered metadata had unexpected reason");

  llvm::Module abiModelMismatchModule(
      "native-prototype-abi-model-mismatch-test", context);
  attachTestAbi(abiModelMismatchModule);
  llvm::Function *abiModelMismatchFunction =
      createFunction(abiModelMismatchModule, "abi_model_mismatch");
  abiModelMismatchFunction->setMetadata(
      "notdec.prototype.recovered",
      makeRecoveredPrototypeMetadata(context, "__fastcall", {{"RDI", 0}}, {}));
  notdec::bin2llvm::NativePrototypeRewriteEligibility
      abiModelMismatchEligibility =
          notdec::bin2llvm::getNativePrototypeRewriteEligibility(
              *abiModelMismatchFunction);
  ok &= expect(!abiModelMismatchEligibility.Eligible,
               "ABI model mismatch was incorrectly rewrite eligible");
  ok &= expect(abiModelMismatchEligibility.Reason ==
                   "recovered prototype ABI model mismatch",
               "ABI model mismatch had unexpected ineligible reason");
  notdec::bin2llvm::NativePrototypeRewriteResult abiModelMismatchRewrite =
      notdec::bin2llvm::rewriteNativeRecoveredPrototype(
          *abiModelMismatchFunction);
  ok &= expect(!abiModelMismatchRewrite.Rewritten,
               "ABI model mismatch was incorrectly rewritten");
  ok &= expect(abiModelMismatchRewrite.Reason ==
                   "recovered prototype ABI model mismatch",
               "ABI model mismatch rewrite had unexpected reason");

  llvm::Module mismatchedCountModule(
      "native-prototype-mismatched-recovered-count-test", context);
  llvm::Function *mismatchedCountFunction =
      createFunction(mismatchedCountModule, "mismatched_recovered_count");
  mismatchedCountFunction->setMetadata(
      "notdec.prototype.recovered",
      makeRecoveredPrototypeMetadataWithCounts(context, "__stdcall", 2, 0,
                                               {{"RDI", 0}}, {}));
  ok &= expect(!notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(
                    *mismatchedCountFunction),
               "mismatched recovered prototype input count was read");
  notdec::bin2llvm::NativePrototypeRewriteEligibility
      mismatchedCountEligibility =
          notdec::bin2llvm::getNativePrototypeRewriteEligibility(
              *mismatchedCountFunction);
  ok &= expect(!mismatchedCountEligibility.Eligible,
               "mismatched recovered count was incorrectly rewrite eligible");
  ok &= expect(mismatchedCountEligibility.Reason ==
                   "missing recovered prototype",
               "mismatched recovered count had unexpected ineligible reason");
  llvm::Function *mismatchedReturnCountFunction = createFunction(
      mismatchedCountModule, "mismatched_recovered_return_count");
  mismatchedReturnCountFunction->setMetadata(
      "notdec.prototype.recovered",
      makeRecoveredPrototypeMetadataWithCounts(context, "__stdcall", 0, 2, {},
                                               {{"RAX", 0}}));
  ok &= expect(!notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(
                    *mismatchedReturnCountFunction),
               "mismatched recovered prototype return count was read");

  llvm::Module mismatchedSlotModule(
      "native-prototype-mismatched-recovered-slot-test", context);
  llvm::Function *unsortedInputSlotFunction =
      createFunction(mismatchedSlotModule, "unsorted_recovered_input_slots");
  unsortedInputSlotFunction->setMetadata(
      "notdec.prototype.recovered",
      makeRecoveredPrototypeMetadata(context, "__stdcall",
                                     {{"RSI", 1}, {"RDI", 0}}, {}));
  ok &= expect(!notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(
                    *unsortedInputSlotFunction),
               "unsorted recovered prototype input slots were read");
  notdec::bin2llvm::NativePrototypeRewriteEligibility
      unsortedInputSlotEligibility =
          notdec::bin2llvm::getNativePrototypeRewriteEligibility(
              *unsortedInputSlotFunction);
  ok &= expect(!unsortedInputSlotEligibility.Eligible,
               "unsorted recovered input slots were incorrectly rewrite eligible");
  ok &= expect(unsortedInputSlotEligibility.Reason ==
                   "missing recovered prototype",
               "unsorted recovered input slots had unexpected ineligible reason");
  llvm::Function *duplicateReturnSlotFunction =
      createFunction(mismatchedSlotModule, "duplicate_recovered_return_slots");
  duplicateReturnSlotFunction->setMetadata(
      "notdec.prototype.recovered",
      makeRecoveredPrototypeMetadata(context, "__stdcall", {},
                                     {{"RAX", 0}, {"RDX", 0}}));
  ok &= expect(!notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(
                    *duplicateReturnSlotFunction),
               "duplicate recovered prototype return slots were read");

  llvm::Module emptyFieldModule(
      "native-prototype-empty-recovered-field-test", context);
  llvm::Function *emptyModelFunction =
      createFunction(emptyFieldModule, "empty_recovered_model");
  emptyModelFunction->setMetadata(
      "notdec.prototype.recovered",
      makeRecoveredPrototypeMetadata(context, "", {{"RDI", 0}}, {}));
  ok &= expect(!notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(
                    *emptyModelFunction),
               "empty recovered prototype model was read");
  notdec::bin2llvm::NativePrototypeRewriteEligibility emptyModelEligibility =
      notdec::bin2llvm::getNativePrototypeRewriteEligibility(
          *emptyModelFunction);
  ok &= expect(!emptyModelEligibility.Eligible,
               "empty recovered model was incorrectly rewrite eligible");
  ok &= expect(emptyModelEligibility.Reason == "missing recovered prototype",
               "empty recovered model had unexpected ineligible reason");
  llvm::Function *emptyInputNameFunction =
      createFunction(emptyFieldModule, "empty_recovered_input_name");
  emptyInputNameFunction->setMetadata(
      "notdec.prototype.recovered",
      makeRecoveredPrototypeMetadata(context, "__stdcall", {{"", 0}}, {}));
  ok &= expect(!notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(
                    *emptyInputNameFunction),
               "empty recovered prototype input name was read");
  llvm::Function *emptyReturnNameFunction =
      createFunction(emptyFieldModule, "empty_recovered_return_name");
  emptyReturnNameFunction->setMetadata(
      "notdec.prototype.recovered",
      makeRecoveredPrototypeMetadata(context, "__stdcall", {}, {{"", 0}}));
  ok &= expect(!notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(
                    *emptyReturnNameFunction),
               "empty recovered prototype return name was read");

  llvm::Module emptyPrototypeModule(
      "native-prototype-empty-recovered-prototype-test", context);
  llvm::Function *emptyPrototypeFunction =
      createFunction(emptyPrototypeModule, "empty_recovered_prototype");
  emptyPrototypeFunction->setMetadata(
      "notdec.prototype.recovered",
      makeRecoveredPrototypeMetadata(context, "__stdcall", {}, {}));
  ok &= expect(notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(
                    *emptyPrototypeFunction)
                   .has_value(),
               "empty recovered prototype was not read");
  notdec::bin2llvm::NativePrototypeRewriteEligibility
      emptyPrototypeEligibility =
          notdec::bin2llvm::getNativePrototypeRewriteEligibility(
              *emptyPrototypeFunction);
  ok &= expect(emptyPrototypeEligibility.Eligible,
               "empty recovered prototype was not rewrite eligible");
  ok &= expect(emptyPrototypeEligibility.Reason == "already matches",
               "empty recovered prototype had unexpected ineligible reason");

  llvm::Module extraOperandModule(
      "native-prototype-extra-recovered-operand-test", context);
  llvm::Function *extraOperandFunction =
      createFunction(extraOperandModule, "extra_recovered_operand");
  llvm::MDNode *baseRecoveredMetadata =
      makeRecoveredPrototypeMetadata(context, "__stdcall", {{"RDI", 0}}, {});
  std::vector<llvm::Metadata *> extraOperandFields;
  for (const llvm::MDOperand &operand : baseRecoveredMetadata->operands()) {
    extraOperandFields.push_back(operand.get());
  }
  extraOperandFields.push_back(llvm::MDString::get(context, "extra=true"));
  extraOperandFunction->setMetadata(
      "notdec.prototype.recovered",
      llvm::MDNode::get(context, extraOperandFields));
  ok &= expect(!notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(
                    *extraOperandFunction),
               "extra recovered prototype operand was ignored");
  notdec::bin2llvm::NativePrototypeRewriteEligibility extraOperandEligibility =
      notdec::bin2llvm::getNativePrototypeRewriteEligibility(
          *extraOperandFunction);
  ok &= expect(!extraOperandEligibility.Eligible,
               "extra recovered operand was incorrectly rewrite eligible");
  ok &= expect(extraOperandEligibility.Reason == "missing recovered prototype",
               "extra recovered operand had unexpected ineligible reason");

  llvm::Module duplicateNameModule(
      "native-prototype-duplicate-recovered-name-test", context);
  llvm::Function *duplicateInputNameFunction =
      createFunction(duplicateNameModule, "duplicate_recovered_input_name");
  duplicateInputNameFunction->setMetadata(
      "notdec.prototype.recovered",
      makeRecoveredPrototypeMetadata(context, "__stdcall",
                                     {{"RDI", 0}, {"RDI", 1}}, {}));
  ok &= expect(!notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(
                    *duplicateInputNameFunction),
               "duplicate recovered prototype input names were read");
  notdec::bin2llvm::NativePrototypeRewriteEligibility
      duplicateInputNameEligibility =
          notdec::bin2llvm::getNativePrototypeRewriteEligibility(
              *duplicateInputNameFunction);
  ok &= expect(!duplicateInputNameEligibility.Eligible,
               "duplicate recovered input names were incorrectly rewrite eligible");
  ok &= expect(duplicateInputNameEligibility.Reason ==
                   "missing recovered prototype",
               "duplicate recovered input names had unexpected ineligible reason");
  llvm::Function *duplicateReturnNameFunction =
      createFunction(duplicateNameModule, "duplicate_recovered_return_name");
  duplicateReturnNameFunction->setMetadata(
      "notdec.prototype.recovered",
      makeRecoveredPrototypeMetadata(context, "__stdcall", {},
                                     {{"RAX", 0}, {"RAX", 1}}));
  ok &= expect(!notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(
                    *duplicateReturnNameFunction),
               "duplicate recovered prototype return names were read");

  llvm::Module extraParamFieldModule(
      "native-prototype-extra-recovered-param-field-test", context);
  llvm::MDNode *extraInputParam = llvm::MDNode::get(
      context, {llvm::MDString::get(context, "name=RDI"),
                llvm::MDString::get(context, "slot=0"),
                llvm::MDString::get(context, "extra=true")});
  llvm::MDNode *emptyParamList = llvm::MDNode::get(context, {});
  llvm::Metadata *extraInputFields[] = {
      llvm::MDString::get(context, "model=__stdcall"),
      llvm::MDString::get(context, "input_count=1"),
      llvm::MDString::get(context, "return_count=0"),
      llvm::MDNode::get(context, {extraInputParam}),
      emptyParamList,
  };
  llvm::Function *extraInputParamFieldFunction =
      createFunction(extraParamFieldModule, "extra_input_param_field");
  extraInputParamFieldFunction->setMetadata(
      "notdec.prototype.recovered",
      llvm::MDNode::get(context, extraInputFields));
  ok &= expect(!notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(
                    *extraInputParamFieldFunction),
               "extra recovered input param field was ignored");
  notdec::bin2llvm::NativePrototypeRewriteEligibility
      extraInputParamFieldEligibility =
          notdec::bin2llvm::getNativePrototypeRewriteEligibility(
              *extraInputParamFieldFunction);
  ok &= expect(!extraInputParamFieldEligibility.Eligible,
               "extra recovered input param field was rewrite eligible");
  ok &= expect(extraInputParamFieldEligibility.Reason ==
                   "missing recovered prototype",
               "extra recovered input param field had unexpected ineligible reason");

  llvm::MDNode *extraReturnParam = llvm::MDNode::get(
      context, {llvm::MDString::get(context, "name=RAX"),
                llvm::MDString::get(context, "slot=0"),
                llvm::MDString::get(context, "extra=true")});
  llvm::Metadata *extraReturnFields[] = {
      llvm::MDString::get(context, "model=__stdcall"),
      llvm::MDString::get(context, "input_count=0"),
      llvm::MDString::get(context, "return_count=1"),
      emptyParamList,
      llvm::MDNode::get(context, {extraReturnParam}),
  };
  llvm::Function *extraReturnParamFieldFunction =
      createFunction(extraParamFieldModule, "extra_return_param_field");
  extraReturnParamFieldFunction->setMetadata(
      "notdec.prototype.recovered",
      llvm::MDNode::get(context, extraReturnFields));
  ok &= expect(!notdec::bin2llvm::readNativeRecoveredPrototypeMetadata(
                    *extraReturnParamFieldFunction),
               "extra recovered return param field was ignored");

  llvm::Module batchModule("native-prototype-batch-rewrite-test", context);
  llvm::GlobalVariable *batchRdi = createRegisterGlobal(batchModule, "RDI");
  llvm::GlobalVariable *batchRax = createRegisterGlobal(batchModule, "RAX");
  attachTestAbi(batchModule);

  llvm::LoadInst *batchInputLoad = nullptr;
  llvm::Function *batchInputFunction = createUsedExternalInputFunction(
      batchModule, "batch_input_rdi", batchRdi, "RDI", &batchInputLoad);
  attachExternalInputs(*batchInputFunction, {{"RDI", batchRdi}});

  llvm::FunctionType *batchMatchingInputType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *batchMatchingInputFunction = createFunctionWithType(
      batchModule, "batch_input_rdi_already_typed", batchMatchingInputType);
  attachExternalInputs(*batchMatchingInputFunction, {{"RDI", batchRdi}});

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
  ok &= expect(batchRewriteSummary.FunctionsSeen == 11,
               "batch rewrite saw unexpected function count");
  ok &= expect(batchRewriteSummary.FunctionsRewritten == 6,
               "batch rewrite rewrote unexpected function count");
  ok &= expect(batchRewriteSummary.FunctionsSkipped == 5,
               "batch rewrite skipped unexpected function count");
  ok &= expect(batchRewriteSummary.SkippedByReason["already matches"] == 5,
               "batch rewrite did not count already-matches skip reason");
  ok &= expect(batchRewriteSummary.SkippedByReason["function has uses"] == 0,
               "batch rewrite did not count function-use skip reason");
  ok &= expect(
      batchRewriteSummary.SkippedByReason["missing recovered prototype"] == 0,
      "batch rewrite did not count missing-prototype skip reason");
  ok &= expect(batchRewriteSummary
                   .SkippedByReason["no recovered prototype candidates"] == 0,
               "batch rewrite did not count no-candidate skip reason");
  ok &= expect(
      batchRewriteSummary.SkippedByReason["unsafe callsite return load"] == 0,
      "batch rewrite did not count unsafe-return-load skip reason");
  ok &= expect(
      batchRewriteSummary.SkippedByReason["unsafe callsite input value"] == 0,
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
                   functionTypeShape(
                       *batchModule.getFunction("batch_input_rdi_used")
                            ->getFunctionType(),
                       llvm::Type::getVoidTy(context),
                       llvm::ArrayRef(i64Param)),
               "batch function with register global callsite load was not rewritten");
  if (llvm::verifyModule(batchModule, &llvm::errs())) {
    std::cerr << "batch module verification failed after prototype rewrite\n";
    return EXIT_FAILURE;
  }

  llvm::Module calleeFirstModule(
      "native-prototype-callee-first-batch-rewrite-test", context);
  llvm::GlobalVariable *calleeFirstRax =
      createRegisterGlobal(calleeFirstModule, "RAX");
  attachTestAbi(calleeFirstModule);
  auto *voidFunctionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *calleeFirstCaller = llvm::Function::Create(
      voidFunctionType, llvm::GlobalValue::ExternalLinkage,
      "callee_first_caller", calleeFirstModule);
  llvm::StoreInst *calleeFirstCalleeStore = nullptr;
  llvm::Function *calleeFirstCallee = createReturnStoreFunction(
      calleeFirstModule, "callee_first_callee", calleeFirstRax, "RAX",
      &calleeFirstCalleeStore);
  llvm::BasicBlock *calleeFirstCallerEntry =
      llvm::BasicBlock::Create(context, "entry", calleeFirstCaller);
  llvm::IRBuilder<> calleeFirstBuilder(calleeFirstCallerEntry);
  calleeFirstBuilder.CreateCall(calleeFirstCallee->getFunctionType(),
                                calleeFirstCallee);
  llvm::LoadInst *calleeFirstReturnLoad = calleeFirstBuilder.CreateLoad(
      calleeFirstRax->getValueType(), calleeFirstRax, "RAX.return_value");
  calleeFirstReturnLoad->setMetadata("notdec.register.access",
                                     registerAccessMetadata(context, "RAX"));
  llvm::StoreInst *calleeFirstCallerStore =
      calleeFirstBuilder.CreateStore(calleeFirstReturnLoad, calleeFirstRax);
  calleeFirstCallerStore->setMetadata("notdec.register.access",
                                      registerAccessMetadata(context, "RAX"));
  calleeFirstBuilder.CreateRetVoid();

  notdec::bin2llvm::runNativePrototypeRecovery(calleeFirstModule, options);
  notdec::bin2llvm::NativePrototypeModuleRewriteSummary calleeFirstSummary =
      notdec::bin2llvm::rewriteNativeRecoveredPrototypes(calleeFirstModule);
  ok &= expect(calleeFirstSummary.FunctionsRewritten == 2,
               "callee-first batch rewrite did not rewrite both functions");
  ok &= expect(calleeFirstModule.getFunction("callee_first_callee") != nullptr &&
                   functionTypeShape(
                       *calleeFirstModule.getFunction("callee_first_callee")
                            ->getFunctionType(),
                       llvm::Type::getInt64Ty(context),
                       llvm::ArrayRef<llvm::Type *>{}),
               "callee-first callee function type was not i64()");
  ok &= expect(calleeFirstModule.getFunction("callee_first_caller") != nullptr &&
                   functionTypeShape(
                       *calleeFirstModule.getFunction("callee_first_caller")
                            ->getFunctionType(),
                       llvm::Type::getInt64Ty(context),
                       llvm::ArrayRef<llvm::Type *>{}),
               "callee-first caller function type was not i64()");
  if (llvm::verifyModule(calleeFirstModule, &llvm::errs())) {
    std::cerr << "callee-first module verification failed after prototype "
                 "rewrite\n";
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
  llvm::FunctionType *optInMatchingInputType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *optInMatchingInputFunction = createFunctionWithType(
      optInModule, "opt_in_input_rdi_already_typed", optInMatchingInputType);
  attachExternalInputs(*optInMatchingInputFunction, {{"RDI", optInRdi}});
  createFunction(optInModule, "opt_in_missing_recovered");

  notdec::bin2llvm::NativePrototypeRecoveryOptions rewriteOptions;
  rewriteOptions.RewriteSignatures = true;
  notdec::bin2llvm::NativePrototypeRecoverySummary optInSummary =
      notdec::bin2llvm::runNativePrototypeRecovery(optInModule, rewriteOptions);
  ok &= expect(optInSummary.SignatureRewriteFunctionsSeen == 3,
               "opt-in rewrite saw unexpected function count");
  ok &= expect(optInSummary.SignatureRewriteFunctionsRewritten == 1,
               "opt-in rewrite rewrote unexpected function count");
  ok &= expect(optInSummary.SignatureRewriteFunctionsSkipped == 2,
               "opt-in rewrite skipped unexpected function count");
  ok &= expect(optInSummary.SignatureRewriteSkippedByReason
                   ["already matches"] == 2,
               "opt-in rewrite did not count already-matches skip reason");
  ok &= expect(optInSummary.SignatureRewriteSkippedByReason
                   ["missing recovered prototype"] == 0,
               "opt-in rewrite did not count missing-prototype skip reason");
  ok &= expect(optInSummary.SignatureRewriteSkippedByReason
                   ["no recovered prototype candidates"] == 0,
               "opt-in rewrite did not count no-candidate skip reason");
  ok &= expect(optInSummary.SignatureRewriteFunctions.size() == 3,
               "opt-in rewrite did not keep per-function rewrite results");
  const auto *optInRewrittenSummary = findRewriteFunctionSummary(
      optInSummary.SignatureRewriteFunctions, "opt_in_input_rdi_return_rax");
  ok &= expect(optInRewrittenSummary != nullptr &&
                   optInRewrittenSummary->Rewritten &&
                   optInRewrittenSummary->Reason == "rewritten",
               "opt-in rewrite did not record rewritten function reason");
  const auto *optInAlreadyTypedSummary = findRewriteFunctionSummary(
      optInSummary.SignatureRewriteFunctions, "opt_in_input_rdi_already_typed");
  ok &= expect(optInAlreadyTypedSummary != nullptr &&
                   !optInAlreadyTypedSummary->Rewritten &&
                   optInAlreadyTypedSummary->Reason == "already matches",
               "opt-in rewrite did not record already-matches function reason");
  const auto *optInMissingSummary = findRewriteFunctionSummary(
      optInSummary.SignatureRewriteFunctions, "opt_in_missing_recovered");
  ok &= expect(optInMissingSummary != nullptr &&
                   !optInMissingSummary->Rewritten &&
                   optInMissingSummary->Reason == "already matches",
               "opt-in rewrite did not record empty-prototype function reason");
  ok &= expect(optInModule.getFunction("opt_in_input_rdi_return_rax") !=
                       nullptr &&
                   functionTypeShape(
                       *optInModule.getFunction("opt_in_input_rdi_return_rax")
                            ->getFunctionType(),
                       llvm::Type::getInt64Ty(context),
                       llvm::ArrayRef(i64Param)),
               "opt-in input-return function type was not i64(i64)");
  notdec::bin2llvm::NativePrototypeRecoverySummary optInRerunSummary =
      notdec::bin2llvm::runNativePrototypeRecovery(optInModule, rewriteOptions);
  ok &= expect(optInRerunSummary.SignatureRewriteFunctionsSeen == 3,
               "opt-in rerun rewrite saw unexpected function count");
  ok &= expect(optInRerunSummary.SignatureRewriteFunctionsRewritten == 0,
               "opt-in rerun rewrote already matching functions");
  ok &= expect(optInRerunSummary.SignatureRewriteFunctionsSkipped == 3,
               "opt-in rerun skipped unexpected function count");
  ok &= expect(optInRerunSummary.SignatureRewriteSkippedByReason
                   ["already matches"] == 3,
               "opt-in rerun did not preserve already-matches prototypes");
  ok &= expect(optInRerunSummary.SignatureRewriteSkippedByReason
                   ["missing recovered prototype"] == 0,
               "opt-in rerun missing-prototype count changed");
  ok &= expect(optInRerunSummary.SignatureRewriteSkippedByReason
                   ["no recovered prototype candidates"] == 0,
               "opt-in rerun no-candidate count changed");
  ok &= expect(optInModule.getFunction("opt_in_input_rdi_return_rax") !=
                       nullptr &&
                   optInModule.getFunction("opt_in_input_rdi_return_rax")
                           ->getMetadata("notdec.prototype.recovered") !=
                       nullptr,
               "opt-in rerun dropped recovered metadata from rewritten function");
  if (llvm::verifyModule(optInModule, &llvm::errs())) {
    std::cerr << "opt-in module verification failed after prototype rewrite\n";
    return EXIT_FAILURE;
  }
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
