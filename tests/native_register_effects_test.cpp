#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/passes/NativeRegisterSSA.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

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

llvm::MDNode *registerAccessMetadata(llvm::LLVMContext &context,
                                     const std::string &base,
                                     const std::string &name,
                                     uint32_t offset, uint32_t size) {
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "base=" + base),
      llvm::MDString::get(context, "space=register"),
      llvm::MDString::get(context, "offset=" + std::to_string(offset)),
      llvm::MDString::get(context, "size=" + std::to_string(size)),
      llvm::MDString::get(context, "name=" + name),
  };
  return llvm::MDNode::get(context, fields);
}

llvm::GlobalVariable *createRegisterGlobal(llvm::Module &module,
                                           const std::string &name,
                                           uint32_t offset = 0,
                                           uint32_t size = 8) {
  llvm::LLVMContext &context = module.getContext();
  llvm::Type *type = size == 1 ? llvm::Type::getInt8Ty(context)
                               : llvm::Type::getInt64Ty(context);
  auto *global = new llvm::GlobalVariable(
      module, type, false, llvm::GlobalValue::ExternalLinkage, nullptr, name);
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "space=register"),
      llvm::MDString::get(context, "offset=" + std::to_string(offset)),
      llvm::MDString::get(context, "size=" + std::to_string(size)),
      llvm::MDString::get(context, "name=" + name),
  };
  global->setMetadata("notdec.register", llvm::MDNode::get(context, fields));
  return global;
}

void attachTestAbi(llvm::Module &module) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__stdcall";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";
  notdec::bin2llvm::NativeAbiParamEntry input;
  input.MinSize = 1;
  input.MaxSize = 8;
  input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  input.Storage.Name = "RDI";
  abi.Inputs.push_back(std::move(input));

  notdec::bin2llvm::NativeAbiParamEntry output;
  output.MinSize = 1;
  output.MaxSize = 8;
  output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  output.Storage.Name = "RAX";
  abi.Outputs.push_back(std::move(output));
  notdec::bin2llvm::NativeAbiParamEntry secondOutput;
  secondOutput.MinSize = 1;
  secondOutput.MaxSize = 8;
  secondOutput.Storage.Kind =
      notdec::bin2llvm::NativeAbiStorageKind::Register;
  secondOutput.Storage.Name = "RDX";
  abi.Outputs.push_back(std::move(secondOutput));

  notdec::bin2llvm::NativeAbiEffect unaffected;
  unaffected.Kind = notdec::bin2llvm::NativeAbiEffectKind::Unaffected;
  unaffected.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  unaffected.Storage.Name = "RBX";
  abi.Effects.push_back(std::move(unaffected));

  notdec::bin2llvm::NativeAbiEffect killed;
  killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
  killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  killed.Storage.Name = "RAX";
  abi.Effects.push_back(std::move(killed));
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
}

void attachRaxInputOutputAbi(llvm::Module &module) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__test_rax";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";

  notdec::bin2llvm::NativeAbiParamEntry input;
  input.MinSize = 1;
  input.MaxSize = 8;
  input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  input.Storage.Name = "RAX";
  abi.Inputs.push_back(std::move(input));

  notdec::bin2llvm::NativeAbiParamEntry output;
  output.MinSize = 1;
  output.MaxSize = 8;
  output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  output.Storage.Name = "RAX";
  abi.Outputs.push_back(std::move(output));

  notdec::bin2llvm::NativeAbiEffect killed;
  killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
  killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  killed.Storage.Name = "RAX";
  abi.Effects.push_back(std::move(killed));

  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
}

llvm::Function *createFunction(llvm::Module &module, const std::string &name,
                               llvm::GlobalVariable *rbx,
                               bool restoreRbx) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage, name,
                             module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *accessMetadata = registerAccessMetadata(context, "RBX");

  llvm::LoadInst *entryValue =
      builder.CreateLoad(rbx->getValueType(), rbx, "entry_rbx");
  entryValue->setMetadata("notdec.register.access", accessMetadata);

  llvm::StoreInst *clobber = builder.CreateStore(
      llvm::ConstantInt::get(rbx->getValueType(), 0x1234), rbx);
  clobber->setMetadata("notdec.register.access", accessMetadata);

  if (restoreRbx) {
    llvm::StoreInst *restore = builder.CreateStore(entryValue, rbx);
    restore->setMetadata("notdec.register.access", accessMetadata);
  }
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createCallEffectFunction(llvm::Module &module,
                                         llvm::GlobalVariable *rbx,
                                         llvm::GlobalVariable *rax) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "external_call", module);
  auto *funcType = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function = llvm::Function::Create(
      funcType, llvm::GlobalValue::ExternalLinkage, "call_effects", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *rbxMetadata = registerAccessMetadata(context, "RBX");
  llvm::MDNode *raxMetadata = registerAccessMetadata(context, "RAX");

  llvm::StoreInst *storeRbx = builder.CreateStore(
      llvm::ConstantInt::get(rbx->getValueType(), 0x1111), rbx);
  storeRbx->setMetadata("notdec.register.access", rbxMetadata);
  llvm::StoreInst *storeRax = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 0x2222), rax);
  storeRax->setMetadata("notdec.register.access", raxMetadata);
  builder.CreateCall(calleeType, callee);

  llvm::LoadInst *loadRbx =
      builder.CreateLoad(rbx->getValueType(), rbx, "rbx_after_call");
  loadRbx->setMetadata("notdec.register.access", rbxMetadata);
  llvm::LoadInst *loadRax =
      builder.CreateLoad(rax->getValueType(), rax, "rax_after_call");
  loadRax->setMetadata("notdec.register.access", raxMetadata);
  builder.CreateRet(builder.CreateAdd(loadRbx, loadRax));
  return function;
}

llvm::Function *createCallInputCandidateFunction(llvm::Module &module,
                                                 llvm::GlobalVariable *rdi) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "call_input_candidate_callee", module);
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "call_input_candidate", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RDI");

  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(rdi->getValueType(), 0x7777), rdi);
  store->setMetadata("notdec.register.access", metadata);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createArithmeticCallInputFunction(llvm::Module &module,
                                                  llvm::GlobalVariable *rdi) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "arith_call_input_callee", module);
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "arith_call_input", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RDI");
  llvm::AllocaInst *slot = builder.CreateAlloca(rdi->getValueType());
  builder.CreateStore(llvm::ConstantInt::get(rdi->getValueType(), 1), slot);
  llvm::Value *base = builder.CreateLoad(rdi->getValueType(), slot);
  llvm::Value *value =
      builder.CreateAdd(base, llvm::ConstantInt::get(rdi->getValueType(), 2));
  llvm::StoreInst *store = builder.CreateStore(value, rdi);
  store->setMetadata("notdec.register.access", metadata);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createSharedUseCallInputFunction(llvm::Module &module,
                                                 llvm::GlobalVariable *rdi) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "shared_use_call_input_callee", module);
  auto *funcType =
      llvm::FunctionType::get(rdi->getValueType(), {}, false);
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "shared_use_call_input", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RDI");
  llvm::AllocaInst *slot = builder.CreateAlloca(rdi->getValueType());
  builder.CreateStore(llvm::ConstantInt::get(rdi->getValueType(), 3), slot);
  llvm::Value *base = builder.CreateLoad(rdi->getValueType(), slot);
  llvm::Value *value =
      builder.CreateAdd(base, llvm::ConstantInt::get(rdi->getValueType(), 4));
  llvm::StoreInst *store = builder.CreateStore(value, rdi);
  store->setMetadata("notdec.register.access", metadata);
  builder.CreateCall(calleeType, callee);
  builder.CreateRet(value);
  return function;
}

llvm::Function *createTransparentUseCallInputFunction(
    llvm::Module &module, llvm::GlobalVariable *rdi) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "transparent_use_call_input_callee", module);
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "transparent_use_call_input", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RDI");
  llvm::AllocaInst *slot = builder.CreateAlloca(rdi->getValueType());
  builder.CreateStore(llvm::ConstantInt::get(rdi->getValueType(), 9), slot);
  llvm::Value *base = builder.CreateLoad(rdi->getValueType(), slot);
  llvm::Value *value =
      builder.CreateAdd(base, llvm::ConstantInt::get(rdi->getValueType(), 10));
  (void)builder.CreateTrunc(value, llvm::Type::getInt32Ty(context));
  llvm::StoreInst *store = builder.CreateStore(value, rdi);
  store->setMetadata("notdec.register.access", metadata);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createDoubleCallUseCallInputFunction(
    llvm::Module &module, llvm::GlobalVariable *rdi) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *firstCallee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "double_call_use_first_callee", module);
  llvm::Function *secondCallee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "double_call_use_second_callee", module);
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "double_call_use_call_input", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RDI");
  llvm::AllocaInst *slot = builder.CreateAlloca(rdi->getValueType());
  builder.CreateStore(llvm::ConstantInt::get(rdi->getValueType(), 5), slot);
  llvm::Value *base = builder.CreateLoad(rdi->getValueType(), slot);
  llvm::Value *value =
      builder.CreateAdd(base, llvm::ConstantInt::get(rdi->getValueType(), 6));
  llvm::StoreInst *firstStore = builder.CreateStore(value, rdi);
  firstStore->setMetadata("notdec.register.access", metadata);
  builder.CreateCall(calleeType, firstCallee);
  llvm::StoreInst *secondStore = builder.CreateStore(value, rdi);
  secondStore->setMetadata("notdec.register.access", metadata);
  builder.CreateCall(calleeType, secondCallee);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createCastCallInputFunction(llvm::Module &module,
                                            llvm::GlobalVariable *rdi) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "cast_call_input_callee", module);
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "cast_call_input", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RDI");
  llvm::AllocaInst *slot = builder.CreateAlloca(llvm::Type::getInt32Ty(context));
  builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt32Ty(context),
                                             0x1234),
                      slot);
  llvm::Value *small = builder.CreateLoad(llvm::Type::getInt32Ty(context), slot);
  llvm::Value *value = builder.CreateZExt(small, rdi->getValueType());
  llvm::StoreInst *store = builder.CreateStore(value, rdi);
  store->setMetadata("notdec.register.access", metadata);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createLocalLoadCallInputFunction(llvm::Module &module,
                                                 llvm::GlobalVariable *rdi) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "local_load_call_input_callee", module);
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "local_load_call_input", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RDI");
  llvm::AllocaInst *slot = builder.CreateAlloca(rdi->getValueType());
  builder.CreateStore(llvm::ConstantInt::get(rdi->getValueType(), 7), slot);
  llvm::Value *value = builder.CreateLoad(rdi->getValueType(), slot);
  llvm::StoreInst *store = builder.CreateStore(value, rdi);
  store->setMetadata("notdec.register.access", metadata);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createUnknownInstCallInputFunction(llvm::Module &module,
                                                   llvm::GlobalVariable *rdi) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_inst_call_input_callee", module);
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_inst_call_input", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RDI");
  llvm::AllocaInst *conditionSlot =
      builder.CreateAlloca(llvm::Type::getInt1Ty(context));
  builder.CreateStore(llvm::ConstantInt::getFalse(context), conditionSlot);
  llvm::Value *condition =
      builder.CreateLoad(llvm::Type::getInt1Ty(context), conditionSlot);
  llvm::Value *value = builder.CreateSelect(
      condition, llvm::ConstantInt::get(rdi->getValueType(), 1),
      llvm::ConstantInt::get(rdi->getValueType(), 2));
  llvm::StoreInst *store = builder.CreateStore(value, rdi);
  store->setMetadata("notdec.register.access", metadata);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createWeakCallInputFunction(llvm::Module &module,
                                            llvm::GlobalVariable *rdi) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "weak_call_input_callee", module);
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "weak_call_input", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  (void)rdi;
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createBlockedCallInputFunction(llvm::Module &module,
                                               llvm::GlobalVariable *rdi) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *first =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "blocked_call_input_first", module);
  llvm::Function *second =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "blocked_call_input_second", module);
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "blocked_call_input", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  (void)rdi;
  builder.CreateCall(calleeType, first);
  builder.CreateCall(calleeType, second);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createStrongPhiCallInputFunction(llvm::Module &module,
                                                 llvm::GlobalVariable *rdi) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "strong_phi_call_input_callee", module);
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "strong_phi_call_input", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right = llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RDI");

  builder.CreateCondBr(llvm::ConstantInt::getFalse(context), left, right);

  builder.SetInsertPoint(left);
  llvm::StoreInst *leftStore = builder.CreateStore(
      llvm::ConstantInt::get(rdi->getValueType(), 0x1111), rdi);
  leftStore->setMetadata("notdec.register.access", metadata);
  builder.CreateBr(join);

  builder.SetInsertPoint(right);
  llvm::StoreInst *rightStore = builder.CreateStore(
      llvm::ConstantInt::get(rdi->getValueType(), 0x2222), rdi);
  rightStore->setMetadata("notdec.register.access", metadata);
  builder.CreateBr(join);

  builder.SetInsertPoint(join);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createConditionalPhiCallInputFunction(
    llvm::Module &module, llvm::GlobalVariable *rdi) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "conditional_phi_call_input_callee", module);
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "conditional_phi_call_input", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right = llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RDI");

  builder.CreateCondBr(llvm::ConstantInt::getFalse(context), left, right);

  builder.SetInsertPoint(left);
  llvm::StoreInst *leftStore = builder.CreateStore(
      llvm::ConstantInt::get(rdi->getValueType(), 0x3333), rdi);
  leftStore->setMetadata("notdec.register.access", metadata);
  builder.CreateBr(join);

  builder.SetInsertPoint(right);
  builder.CreateBr(join);

  builder.SetInsertPoint(join);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createReturnForwardCallInputFunction(
    llvm::Module &module, llvm::GlobalVariable *rax) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *first =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "return_forward_first", module);
  llvm::Function *second =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "return_forward_second", module);
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "return_forward_call_input", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  (void)rax;
  builder.CreateCall(calleeType, first);
  builder.CreateCall(calleeType, second);
  builder.CreateRetVoid();
  return function;
}

std::unique_ptr<llvm::Module> createMissingPhiIncomingModule(
    llvm::LLVMContext &context) {
  auto module = std::make_unique<llvm::Module>("missing-phi-incoming", context);
  auto *funcType = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "bad_phi", *module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right = llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getFalse(context), left, right);
  builder.SetInsertPoint(left);
  builder.CreateBr(join);
  builder.SetInsertPoint(right);
  builder.CreateBr(join);
  builder.SetInsertPoint(join);
  llvm::PHINode *phi =
      builder.CreatePHI(llvm::Type::getInt64Ty(context), 2, "missing");
  phi->addIncoming(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 1),
                   left);
  builder.CreateRet(phi);
  return module;
}

llvm::Function *createStackPointerCallEffectFunction(
    llvm::Module &module, llvm::GlobalVariable *rsp) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "stack_pointer_call", module);
  auto *funcType = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function = llvm::Function::Create(
      funcType, llvm::GlobalValue::ExternalLinkage,
      "stack_pointer_call_effect", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RSP", "RSP", 32, 8);

  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(rsp->getValueType(), 0x1000), rsp);
  store->setMetadata("notdec.register.access", metadata);
  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *load =
      builder.CreateLoad(rsp->getValueType(), rsp, "rsp_after_call");
  load->setMetadata("notdec.register.access", metadata);
  builder.CreateRet(load);
  return function;
}

llvm::Function *createRepeatedLoadAfterCallFunction(llvm::Module &module,
                                                    llvm::GlobalVariable *rax) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "repeated_load_after_call_callee", module);
  auto *funcType = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "repeated_load_after_call", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RAX");

  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *first =
      builder.CreateLoad(rax->getValueType(), rax, "rax_after_call_first");
  first->setMetadata("notdec.register.access", metadata);
  llvm::LoadInst *second =
      builder.CreateLoad(rax->getValueType(), rax, "rax_after_call_second");
  second->setMetadata("notdec.register.access", metadata);
  builder.CreateRet(builder.CreateAdd(first, second));
  return function;
}

void attachRegisterMetadataToFunction(llvm::Function &function,
                                      llvm::StringRef kind,
                                      llvm::GlobalVariable *global,
                                      const std::string &name) {
  llvm::LLVMContext &context = function.getContext();
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "name=" + name),
      llvm::ValueAsMetadata::get(global),
  };
  llvm::Metadata *entries[] = {llvm::MDNode::get(context, fields)};
  function.setMetadata(kind, llvm::MDNode::get(context, entries));
}

llvm::Function *createDirectCallEffectFunction(llvm::Module &module,
                                               llvm::GlobalVariable *rbx) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "callee_preserves_rbx", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  calleeBuilder.CreateRetVoid();
  attachRegisterMetadataToFunction(*callee, "notdec.register.preserves", rbx,
                                   "RBX");

  auto *funcType = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "direct_call_effects", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *rbxMetadata = registerAccessMetadata(context, "RBX");

  llvm::StoreInst *storeRbx = builder.CreateStore(
      llvm::ConstantInt::get(rbx->getValueType(), 0x3333), rbx);
  storeRbx->setMetadata("notdec.register.access", rbxMetadata);
  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *loadRbx =
      builder.CreateLoad(rbx->getValueType(), rbx, "rbx_after_direct_call");
  loadRbx->setMetadata("notdec.register.access", rbxMetadata);
  builder.CreateRet(loadRbx);
  return function;
}

void attachRecoveredReturnMetadata(llvm::Function &function,
                                   llvm::StringRef registerName) {
  llvm::LLVMContext &context = function.getContext();
  llvm::Metadata *returnFields[] = {
      llvm::MDString::get(context, "storage=register"),
      llvm::MDString::get(context, ("name=" + registerName).str()),
      llvm::MDString::get(context, "size=8"),
      llvm::MDString::get(context, "slot=0"),
  };
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "model=__stdcall"),
      llvm::MDString::get(context, "input_count=0"),
      llvm::MDString::get(context, "return_count=1"),
      llvm::MDNode::get(context, {}),
      llvm::MDNode::get(context,
                        {llvm::MDNode::get(context, returnFields)}),
  };
  function.setMetadata("notdec.prototype.recovered",
                       llvm::MDNode::get(context, fields));
}

llvm::Function *createDirectRecoveredReturnEffectFunction(
    llvm::Module &module, llvm::GlobalVariable *rax,
    llvm::GlobalVariable *rdx) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "callee_recovered_returns_rax", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  calleeBuilder.CreateRetVoid();
  attachRecoveredReturnMetadata(*callee, "RAX");

  auto *funcType = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "direct_recovered_return_effects", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *raxMetadata = registerAccessMetadata(context, "RAX");
  llvm::MDNode *rdxMetadata = registerAccessMetadata(context, "RDX");

  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *loadRax =
      builder.CreateLoad(rax->getValueType(), rax, "rax_after_direct_return");
  loadRax->setMetadata("notdec.register.access", raxMetadata);
  llvm::LoadInst *loadRdx =
      builder.CreateLoad(rdx->getValueType(), rdx, "rdx_after_direct_return");
  loadRdx->setMetadata("notdec.register.access", rdxMetadata);
  builder.CreateRet(builder.CreateAdd(loadRax, loadRdx));
  return function;
}

llvm::Function *createCallerBeforeClobberingCalleeFunction(
    llvm::Module &module, llvm::GlobalVariable *rbx) {
  llvm::LLVMContext &context = module.getContext();
  auto *callerType = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *caller =
      llvm::Function::Create(callerType, llvm::GlobalValue::ExternalLinkage,
                             "caller_before_clobbering_callee", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);

  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "callee_clobbers_rbx_after_caller", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  llvm::MDNode *rbxMetadata = registerAccessMetadata(context, "RBX");
  llvm::LoadInst *entryRbx =
      calleeBuilder.CreateLoad(rbx->getValueType(), rbx, "callee_entry_rbx");
  entryRbx->setMetadata("notdec.register.access", rbxMetadata);
  llvm::StoreInst *clobber = calleeBuilder.CreateStore(
      llvm::ConstantInt::get(rbx->getValueType(), 0x4444), rbx);
  clobber->setMetadata("notdec.register.access", rbxMetadata);
  calleeBuilder.CreateRetVoid();

  llvm::IRBuilder<> callerBuilder(callerEntry);
  llvm::StoreInst *storeRbx = callerBuilder.CreateStore(
      llvm::ConstantInt::get(rbx->getValueType(), 0x5555), rbx);
  storeRbx->setMetadata("notdec.register.access", rbxMetadata);
  callerBuilder.CreateCall(calleeType, callee);
  llvm::LoadInst *loadRbx =
      callerBuilder.CreateLoad(rbx->getValueType(), rbx,
                               "rbx_after_late_clobbering_callee");
  loadRbx->setMetadata("notdec.register.access", rbxMetadata);
  callerBuilder.CreateRet(loadRbx);
  return caller;
}

llvm::Function *createStaleMetadataFunction(llvm::Module &module,
                                            llvm::GlobalVariable *rbx) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "stale_register_metadata", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateRetVoid();
  attachRegisterMetadataToFunction(*function, "notdec.register.external_inputs",
                                   rbx, "RBX");
  attachRegisterMetadataToFunction(*function, "notdec.register.preserves", rbx,
                                   "RBX");
  attachRegisterMetadataToFunction(*function, "notdec.register.clobbers", rbx,
                                   "RBX");
  return function;
}

llvm::Function *createUnmarkedRegisterLoadFunction(llvm::Module &module,
                                                   llvm::GlobalVariable *rdi) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "unmarked_register_load", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *load =
      builder.CreateLoad(rdi->getValueType(), rdi, "unmarked_rdi");
  builder.CreateRet(load);
  return function;
}

llvm::Function *createOverwrittenStoreFunction(llvm::Module &module,
                                               llvm::GlobalVariable *rax) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "overwritten_register_store", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RAX");
  llvm::StoreInst *first = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 1), rax);
  first->setMetadata("notdec.register.access", metadata);
  llvm::StoreInst *second = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 2), rax);
  second->setMetadata("notdec.register.access", metadata);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createUnmarkedRegisterStoreLoadFunction(
    llvm::Module &module, llvm::GlobalVariable *rax) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "unmarked_register_store_load", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateStore(llvm::ConstantInt::get(rax->getValueType(), 5), rax);
  llvm::LoadInst *load =
      builder.CreateLoad(rax->getValueType(), rax, "unmarked_rax");
  builder.CreateRet(load);
  return function;
}

llvm::Function *createCallBetweenStoresFunction(llvm::Module &module,
                                                llvm::GlobalVariable *rax) {
  llvm::LLVMContext &context = module.getContext();
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "dead_store_barrier_call", module);
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "call_between_register_stores", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RAX");
  llvm::StoreInst *first = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 3), rax);
  first->setMetadata("notdec.register.access", metadata);
  builder.CreateCall(calleeType, callee);
  llvm::StoreInst *second = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 4), rax);
  second->setMetadata("notdec.register.access", metadata);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createPartialStoreCoveredByFullStoreFunction(
    llvm::Module &module, llvm::GlobalVariable *rax) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "partial_store_covered_by_full_store", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *alMetadata = registerAccessMetadata(context, "RAX", "AL", 0, 1);
  llvm::MDNode *raxMetadata = registerAccessMetadata(context, "RAX");
  llvm::StoreInst *partial = builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 5), rax);
  partial->setMetadata("notdec.register.access", alMetadata);
  llvm::StoreInst *full = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 6), rax);
  full->setMetadata("notdec.register.access", raxMetadata);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createDeadPartialInputFunction(llvm::Module &module,
                                               llvm::GlobalVariable *rax) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "dead_partial_register_input", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *alMetadata = registerAccessMetadata(context, "RAX", "AL", 0, 1);
  llvm::MDNode *raxMetadata = registerAccessMetadata(context, "RAX");
  llvm::StoreInst *partial = builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 9), rax);
  partial->setMetadata("notdec.register.access", alMetadata);
  llvm::StoreInst *full = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 10), rax);
  full->setMetadata("notdec.register.access", raxMetadata);
  llvm::LoadInst *load =
      builder.CreateLoad(rax->getValueType(), rax, "after_full_store");
  load->setMetadata("notdec.register.access", raxMetadata);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createPartialStoresOnlyFunction(llvm::Module &module,
                                                llvm::GlobalVariable *rax) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "partial_register_stores_only", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *alMetadata = registerAccessMetadata(context, "RAX", "AL", 0, 1);
  llvm::StoreInst *first = builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 7), rax);
  first->setMetadata("notdec.register.access", alMetadata);
  llvm::StoreInst *second = builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 8), rax);
  second->setMetadata("notdec.register.access", alMetadata);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createPartialMetadataStorageValueFunction(
    llvm::Module &module, llvm::GlobalVariable *rax) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "partial_metadata_storage_value", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *eaxMetadata = registerAccessMetadata(context, "RAX", "EAX", 0, 4);
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 0x1234), rax);
  store->setMetadata("notdec.register.access", eaxMetadata);
  llvm::LoadInst *load =
      builder.CreateLoad(rax->getValueType(), rax, "eax_backing");
  load->setMetadata("notdec.register.access", eaxMetadata);
  builder.CreateRet(load);
  return function;
}

llvm::Function *createWidePartialLoadFunction(llvm::Module &module,
                                              llvm::GlobalVariable *rax) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "wide_partial_register_load", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *raxMetadata = registerAccessMetadata(context, "RAX");
  llvm::MDNode *eaxMetadata = registerAccessMetadata(context, "RAX", "EAX", 0, 4);
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 0x12340000abcdULL), rax);
  store->setMetadata("notdec.register.access", raxMetadata);
  llvm::LoadInst *load =
      builder.CreateLoad(rax->getValueType(), rax, "eax_wide");
  load->setMetadata("notdec.register.access", eaxMetadata);
  builder.CreateRet(load);
  return function;
}

llvm::Function *createPartialLoadFunction(llvm::Module &module,
                                          llvm::GlobalVariable *rax) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getInt8Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "partial_register_load", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *raxMetadata = registerAccessMetadata(context, "RAX");
  llvm::MDNode *alMetadata = registerAccessMetadata(context, "RAX", "AL", 0, 1);
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 0x12345678ab), rax);
  store->setMetadata("notdec.register.access", raxMetadata);
  llvm::LoadInst *load =
      builder.CreateLoad(llvm::Type::getInt8Ty(context), rax, "al");
  load->setMetadata("notdec.register.access", alMetadata);
  builder.CreateRet(load);
  return function;
}

llvm::Function *createLoopCarriedRegisterValueFunction(
    llvm::Module &module, llvm::GlobalVariable *rax) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "loop_carried_register_value", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *loop = llvm::BasicBlock::Create(context, "loop", function);
  llvm::BasicBlock *exit = llvm::BasicBlock::Create(context, "exit", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RAX");

  llvm::StoreInst *initial = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 1), rax);
  initial->setMetadata("notdec.register.access", metadata);
  builder.CreateBr(loop);

  builder.SetInsertPoint(loop);
  llvm::LoadInst *load =
      builder.CreateLoad(rax->getValueType(), rax, "loop_rax");
  load->setMetadata("notdec.register.access", metadata);
  llvm::Value *next =
      builder.CreateAdd(load, llvm::ConstantInt::get(rax->getValueType(), 1));
  llvm::StoreInst *store = builder.CreateStore(next, rax);
  store->setMetadata("notdec.register.access", metadata);
  builder.CreateCondBr(llvm::ConstantInt::getFalse(context), loop, exit);

  builder.SetInsertPoint(exit);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createTrivialJoinRegisterPhiFunction(llvm::Module &module,
                                                     llvm::GlobalVariable *rax) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "trivial_join_register_phi", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right = llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RAX");

  builder.CreateCondBr(llvm::ConstantInt::getFalse(context), left, right);

  builder.SetInsertPoint(left);
  llvm::StoreInst *leftStore = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 0x4242), rax);
  leftStore->setMetadata("notdec.register.access", metadata);
  builder.CreateBr(join);

  builder.SetInsertPoint(right);
  llvm::StoreInst *rightStore = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 0x4242), rax);
  rightStore->setMetadata("notdec.register.access", metadata);
  builder.CreateBr(join);

  builder.SetInsertPoint(join);
  llvm::LoadInst *load =
      builder.CreateLoad(rax->getValueType(), rax, "joined_rax");
  load->setMetadata("notdec.register.access", metadata);
  builder.CreateRet(load);
  return function;
}

llvm::Function *createCascadedTrivialRegisterPhiFunction(
    llvm::Module &module, llvm::GlobalVariable *rax) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "cascaded_trivial_register_phi", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right = llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *firstJoin =
      llvm::BasicBlock::Create(context, "first_join", function);
  llvm::BasicBlock *bypass =
      llvm::BasicBlock::Create(context, "bypass", function);
  llvm::BasicBlock *secondJoin =
      llvm::BasicBlock::Create(context, "second_join", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RAX");

  builder.CreateCondBr(llvm::ConstantInt::getFalse(context), left, right);

  builder.SetInsertPoint(left);
  llvm::StoreInst *leftStore = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 0x5151), rax);
  leftStore->setMetadata("notdec.register.access", metadata);
  builder.CreateBr(firstJoin);

  builder.SetInsertPoint(right);
  llvm::StoreInst *rightStore = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 0x5151), rax);
  rightStore->setMetadata("notdec.register.access", metadata);
  builder.CreateBr(firstJoin);

  builder.SetInsertPoint(firstJoin);
  builder.CreateCondBr(llvm::ConstantInt::getFalse(context), secondJoin,
                       bypass);

  builder.SetInsertPoint(bypass);
  llvm::StoreInst *bypassStore = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 0x5151), rax);
  bypassStore->setMetadata("notdec.register.access", metadata);
  builder.CreateBr(secondJoin);

  builder.SetInsertPoint(secondJoin);
  llvm::LoadInst *load =
      builder.CreateLoad(rax->getValueType(), rax, "second_join_rax");
  load->setMetadata("notdec.register.access", metadata);
  builder.CreateRet(load);
  return function;
}

llvm::Function *createUnreachableRegisterLoadFunction(
    llvm::Module &module, llvm::GlobalVariable *rax) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "unreachable_register_load", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *dead = llvm::BasicBlock::Create(context, "dead", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RAX");

  builder.CreateRetVoid();
  builder.SetInsertPoint(dead);
  llvm::LoadInst *load =
      builder.CreateLoad(rax->getValueType(), rax, "dead_rax");
  load->setMetadata("notdec.register.access", metadata);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createUnreadFlagStoresFunction(llvm::Module &module,
                                               llvm::GlobalVariable *cf) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "unread_flag_stores", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "CF", "CF", 512, 1);
  llvm::StoreInst *first = builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 0), cf);
  first->setMetadata("notdec.register.access", metadata);
  llvm::StoreInst *second = builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 1), cf);
  second->setMetadata("notdec.register.access", metadata);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createReadFlagStoresFunction(llvm::Module &module,
                                             llvm::GlobalVariable *cf) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getInt8Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "read_flag_stores", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "CF", "CF", 512, 1);
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 1), cf);
  store->setMetadata("notdec.register.access", metadata);
  llvm::LoadInst *load = builder.CreateLoad(cf->getValueType(), cf);
  load->setMetadata("notdec.register.access", metadata);
  builder.CreateRet(load);
  return function;
}

llvm::Function *createReadOneFlagStoreOtherFlagFunction(
    llvm::Module &module, llvm::GlobalVariable *cf, llvm::GlobalVariable *of) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getInt8Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "read_one_flag_store_other_flag", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *cfMetadata = registerAccessMetadata(context, "CF", "CF", 512, 1);
  llvm::MDNode *ofMetadata = registerAccessMetadata(context, "OF", "OF", 523, 1);
  llvm::StoreInst *cfStore = builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 1), cf);
  cfStore->setMetadata("notdec.register.access", cfMetadata);
  llvm::StoreInst *ofStore = builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 1), of);
  ofStore->setMetadata("notdec.register.access", ofMetadata);
  llvm::LoadInst *load = builder.CreateLoad(cf->getValueType(), cf);
  load->setMetadata("notdec.register.access", cfMetadata);
  builder.CreateRet(load);
  return function;
}

llvm::Function *createFlagRestoreOnlyFunction(llvm::Module &module,
                                              llvm::GlobalVariable *of) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "flag_restore_only", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "OF", "OF", 523, 1);
  llvm::LoadInst *entryOf =
      builder.CreateLoad(of->getValueType(), of, "entry_of");
  entryOf->setMetadata("notdec.register.access", metadata);
  llvm::Value *masked =
      builder.CreateAnd(entryOf, llvm::ConstantInt::get(of->getValueType(), 1));
  llvm::StoreInst *restore = builder.CreateStore(masked, of);
  restore->setMetadata("notdec.register.access", metadata);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createDeadFlagInputBeforeConstantStoreFunction(
    llvm::Module &module, llvm::GlobalVariable *of) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "dead_flag_input_before_constant_store", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "OF", "OF", 523, 1);
  llvm::LoadInst *entryOf =
      builder.CreateLoad(of->getValueType(), of, "entry_of");
  entryOf->setMetadata("notdec.register.access", metadata);
  (void)builder.CreateAnd(entryOf,
                          llvm::ConstantInt::get(of->getValueType(), 1));
  llvm::StoreInst *clear =
      builder.CreateStore(llvm::ConstantInt::get(of->getValueType(), 0), of);
  clear->setMetadata("notdec.register.access", metadata);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createUnreadRipStoresFunction(llvm::Module &module,
                                              llvm::GlobalVariable *rip) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "unread_rip_stores", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RIP", "RIP", 648, 8);
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(rip->getValueType(), 0x400123), rip);
  store->setMetadata("notdec.register.access", metadata);
  builder.CreateRetVoid();
  return function;
}

llvm::Function *createReadRipStoresFunction(llvm::Module &module,
                                            llvm::GlobalVariable *rip) {
  llvm::LLVMContext &context = module.getContext();
  auto *funcType = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "read_rip_stores", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::MDNode *metadata = registerAccessMetadata(context, "RIP", "RIP", 648, 8);
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(rip->getValueType(), 0x400456), rip);
  store->setMetadata("notdec.register.access", metadata);
  llvm::LoadInst *load = builder.CreateLoad(rip->getValueType(), rip);
  load->setMetadata("notdec.register.access", metadata);
  builder.CreateRet(load);
  return function;
}

unsigned countRegisterLoads(const llvm::Function &function,
                            const llvm::GlobalVariable *global) {
  unsigned count = 0;
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &inst : block) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
      if (load == nullptr) {
        continue;
      }
      if (load->getPointerOperand()->stripPointerCasts() == global) {
        ++count;
      }
    }
  }
  return count;
}

bool hasRegisterAccessLoad(const llvm::Function &function,
                           const llvm::GlobalVariable *global) {
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &inst : block) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
      if (load == nullptr ||
          load->getPointerOperand()->stripPointerCasts() != global) {
        continue;
      }
      if (load->getMetadata("notdec.register.access") != nullptr) {
        return true;
      }
    }
  }
  return false;
}

unsigned countRegisterStores(const llvm::Function &function,
                             const llvm::GlobalVariable *global) {
  unsigned count = 0;
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &inst : block) {
      auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
      if (store == nullptr) {
        continue;
      }
      if (store->getPointerOperand()->stripPointerCasts() == global) {
        ++count;
      }
    }
  }
  return count;
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

unsigned countCallEffects(const llvm::Function &function, llvm::StringRef kind,
                          llvm::StringRef name) {
  unsigned count = 0;
  std::string expectedKind = ("kind=" + kind).str();
  std::string expectedRegister = ("register=" + name).str();
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &inst : block) {
      llvm::MDNode *node = inst.getMetadata("notdec.register.call_effect");
      if (node == nullptr) {
        continue;
      }
      bool hasKind = false;
      bool hasRegister = false;
      for (const llvm::MDOperand &operand : node->operands()) {
        auto *field = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
        if (field == nullptr) {
          continue;
        }
        hasKind |= field->getString() == expectedKind;
        hasRegister |= field->getString() == expectedRegister;
      }
      if (hasKind && hasRegister) {
        ++count;
      }
    }
  }
  return count;
}

bool callEffectHasField(const llvm::Function &function,
                        llvm::StringRef kind,
                        llvm::StringRef name,
                        llvm::StringRef expectedField) {
  std::string expectedKind = ("kind=" + kind).str();
  std::string expectedRegister = ("register=" + name).str();
  bool matchPrefix = expectedField.ends_with("=");
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &inst : block) {
      llvm::MDNode *node = inst.getMetadata("notdec.register.call_effect");
      if (node == nullptr) {
        continue;
      }
      bool hasKind = false;
      bool hasRegister = false;
      bool hasField = false;
      for (const llvm::MDOperand &operand : node->operands()) {
        auto *field = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
        if (field == nullptr) {
          continue;
        }
        hasKind |= field->getString() == expectedKind;
        hasRegister |= field->getString() == expectedRegister;
        hasField |= matchPrefix
                        ? field->getString().starts_with(expectedField)
                        : field->getString() == expectedField;
      }
      if (hasKind && hasRegister) {
        return hasField;
      }
    }
  }
  return false;
}

bool callEffectHasCallsiteId(const llvm::Function &function,
                             llvm::StringRef kind,
                             llvm::StringRef name) {
  return callEffectHasField(function, kind, name, "callsite_id=");
}

unsigned countRegisterPhis(const llvm::Function &function,
                           llvm::StringRef name) {
  unsigned count = 0;
  std::string prefix = (name + ".regssa").str();
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::PHINode &phi : block.phis()) {
      if (phi.getName().starts_with(prefix)) {
        ++count;
      }
    }
  }
  return count;
}

unsigned countCallInputCandidates(const llvm::Function &function,
                                  llvm::StringRef name) {
  unsigned count = 0;
  std::string expectedRegister = ("register=" + name).str();
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &inst : block) {
      llvm::MDNode *node =
          inst.getMetadata("notdec.register.call_input_candidate");
      if (node == nullptr) {
        continue;
      }
      for (const llvm::MDOperand &operand : node->operands()) {
        auto *field = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
        if (field != nullptr && field->getString() == expectedRegister) {
          ++count;
          break;
        }
      }
    }
  }
  return count;
}

bool hasCallInputCandidateMetadata(const llvm::Function &function,
                                   llvm::StringRef name) {
  std::string expectedRegister = ("register=" + name).str();
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &inst : block) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr) {
        continue;
      }
      llvm::MDNode *node =
          call->getMetadata("notdec.register.call_input_candidates");
      if (node == nullptr) {
        continue;
      }
      for (const llvm::MDOperand &entryOperand : node->operands()) {
        auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(entryOperand.get());
        if (entry == nullptr) {
          continue;
        }
        for (const llvm::MDOperand &fieldOperand : entry->operands()) {
          auto *field =
              llvm::dyn_cast_or_null<llvm::MDString>(fieldOperand.get());
          if (field != nullptr && field->getString() == expectedRegister) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

bool callInputCandidateHasCallsiteId(const llvm::Function &function,
                                     llvm::StringRef name) {
  std::string expectedRegister = ("register=" + name).str();
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &inst : block) {
      llvm::MDNode *node =
          inst.getMetadata("notdec.register.call_input_candidate");
      if (node == nullptr) {
        continue;
      }
      bool hasRegister = false;
      bool hasCallsite = false;
      for (const llvm::MDOperand &operand : node->operands()) {
        auto *field = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
        if (field == nullptr) {
          continue;
        }
        hasRegister |= field->getString() == expectedRegister;
        hasCallsite |= field->getString().starts_with("callsite_id=");
      }
      if (hasRegister) {
        return hasCallsite;
      }
    }
  }
  return false;
}

bool callInputCandidateHasField(const llvm::Function &function,
                                llvm::StringRef name,
                                llvm::StringRef expectedField) {
  std::string expectedRegister = ("register=" + name).str();
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &inst : block) {
      llvm::MDNode *node =
          inst.getMetadata("notdec.register.call_input_candidate");
      if (node == nullptr) {
        continue;
      }
      bool hasRegister = false;
      bool hasField = false;
      for (const llvm::MDOperand &operand : node->operands()) {
        auto *field = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
        if (field == nullptr) {
          continue;
        }
        hasRegister |= field->getString() == expectedRegister;
        hasField |= field->getString() == expectedField;
      }
      if (hasRegister && hasField) {
        return true;
      }
    }
  }
  return false;
}

bool callInputCandidateUsesHelper(const llvm::Function &function,
                                  llvm::StringRef name) {
  std::string expectedRegister = ("register=" + name).str();
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &inst : block) {
      llvm::MDNode *node =
          inst.getMetadata("notdec.register.call_input_candidate");
      if (node == nullptr) {
        continue;
      }
      bool hasRegister = false;
      for (const llvm::MDOperand &operand : node->operands()) {
        auto *field = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
        hasRegister |= field != nullptr && field->getString() == expectedRegister;
      }
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      llvm::Function *callee = call == nullptr ? nullptr : call->getCalledFunction();
      if (hasRegister && callee != nullptr &&
          callee->getName().starts_with("notdec.register.call_input.")) {
        return true;
      }
    }
  }
  return false;
}

bool callEffectStoresToRegister(const llvm::Function &function,
                                llvm::StringRef kind,
                                llvm::StringRef name,
                                const llvm::GlobalVariable *global) {
  std::string expectedKind = ("kind=" + kind).str();
  std::string expectedRegister = ("register=" + name).str();
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &inst : block) {
      llvm::MDNode *node = inst.getMetadata("notdec.register.call_effect");
      if (node == nullptr) {
        continue;
      }
      bool hasKind = false;
      bool hasRegister = false;
      for (const llvm::MDOperand &operand : node->operands()) {
        auto *field = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
        if (field == nullptr) {
          continue;
        }
        hasKind |= field->getString() == expectedKind;
        hasRegister |= field->getString() == expectedRegister;
      }
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      const llvm::Instruction *next = inst.getNextNode();
      auto *store = llvm::dyn_cast_or_null<llvm::StoreInst>(next);
      if (hasKind && hasRegister && call != nullptr &&
          call->getCalledFunction() != nullptr &&
          call->getCalledFunction()->getName().starts_with(
              "notdec.register.") &&
          store != nullptr && store->getValueOperand() == &inst &&
          store->getPointerOperand()->stripPointerCasts() == global) {
        return true;
      }
    }
  }
  return false;
}

bool moduleHasPhiMissingIncomingEffect(const llvm::Module &module) {
  for (const llvm::Function &function : module) {
    for (const llvm::BasicBlock &block : function) {
      for (const llvm::Instruction &inst : block) {
        llvm::MDNode *node = inst.getMetadata("notdec.register.call_effect");
        if (node == nullptr) {
          continue;
        }
        for (const llvm::MDOperand &operand : node->operands()) {
          auto *field = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
          if (field != nullptr &&
              field->getString() == "source=phi_missing_incoming") {
            return true;
          }
        }
      }
    }
  }
  return false;
}

bool functionReturnsConstant(const llvm::Function &function, uint64_t expected) {
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &inst : block) {
      auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&inst);
      if (ret == nullptr || ret->getReturnValue() == nullptr) {
        continue;
      }
      auto *constant = llvm::dyn_cast<llvm::ConstantInt>(ret->getReturnValue());
      if (constant == nullptr) {
        return false;
      }
      return constant->getZExtValue() == expected;
    }
  }
  return false;
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
  llvm::Module module("native-register-effects-test", context);
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP", 32, 8);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *cf = createRegisterGlobal(module, "CF", 512, 1);
  llvm::GlobalVariable *of = createRegisterGlobal(module, "OF", 523, 1);
  llvm::GlobalVariable *rip = createRegisterGlobal(module, "RIP", 648, 8);
  attachTestAbi(module);
  llvm::Function *preserved =
      createFunction(module, "preserved_rbx", rbx, true);
  llvm::Function *clobbered =
      createFunction(module, "clobbered_rbx", rbx, false);
  llvm::Function *callEffects = createCallEffectFunction(module, rbx, rax);
  llvm::Function *callInputCandidate =
      createCallInputCandidateFunction(module, rdi);
  llvm::Function *arithCallInput =
      createArithmeticCallInputFunction(module, rdi);
  llvm::Function *sharedUseCallInput =
      createSharedUseCallInputFunction(module, rdi);
  llvm::Function *transparentUseCallInput =
      createTransparentUseCallInputFunction(module, rdi);
  llvm::Function *doubleCallUseCallInput =
      createDoubleCallUseCallInputFunction(module, rdi);
  llvm::Function *castCallInput = createCastCallInputFunction(module, rdi);
  llvm::Function *localLoadCallInput =
      createLocalLoadCallInputFunction(module, rdi);
  llvm::Function *unknownInstCallInput =
      createUnknownInstCallInputFunction(module, rdi);
  llvm::Function *weakCallInput = createWeakCallInputFunction(module, rdi);
  llvm::Function *blockedCallInput = createBlockedCallInputFunction(module, rdi);
  llvm::Function *strongPhiCallInput =
      createStrongPhiCallInputFunction(module, rdi);
  llvm::Function *conditionalPhiCallInput =
      createConditionalPhiCallInputFunction(module, rdi);
  llvm::Function *stackPointerCallEffects =
      createStackPointerCallEffectFunction(module, rsp);
  llvm::Function *repeatedLoadAfterCall =
      createRepeatedLoadAfterCallFunction(module, rax);
  llvm::Function *directCallEffects =
      createDirectCallEffectFunction(module, rbx);
  llvm::Function *directRecoveredReturnEffects =
      createDirectRecoveredReturnEffectFunction(module, rax, rdx);
  llvm::Function *callerBeforeClobberingCallee =
      createCallerBeforeClobberingCalleeFunction(module, rbx);
  llvm::Function *staleMetadata = createStaleMetadataFunction(module, rbx);
  llvm::Function *unmarkedRegisterLoad =
      createUnmarkedRegisterLoadFunction(module, rdi);
  llvm::Function *unmarkedRegisterStoreLoad =
      createUnmarkedRegisterStoreLoadFunction(module, rax);
  llvm::Function *overwrittenStore =
      createOverwrittenStoreFunction(module, rax);
  llvm::Function *callBetweenStores =
      createCallBetweenStoresFunction(module, rax);
  llvm::Function *partialCovered =
      createPartialStoreCoveredByFullStoreFunction(module, rax);
  llvm::Function *deadPartialInput =
      createDeadPartialInputFunction(module, rax);
  llvm::Function *partialOnly = createPartialStoresOnlyFunction(module, rax);
  llvm::Function *partialMetadataStorage =
      createPartialMetadataStorageValueFunction(module, rax);
  llvm::Function *widePartialLoad = createWidePartialLoadFunction(module, rax);
  llvm::Function *partialLoad = createPartialLoadFunction(module, rax);
  llvm::Function *loopCarriedRegisterValue =
      createLoopCarriedRegisterValueFunction(module, rax);
  llvm::Function *trivialJoinRegisterPhi =
      createTrivialJoinRegisterPhiFunction(module, rax);
  llvm::Function *cascadedTrivialRegisterPhi =
      createCascadedTrivialRegisterPhiFunction(module, rax);
  llvm::Function *unreachableRegisterLoad =
      createUnreachableRegisterLoadFunction(module, rax);
  llvm::Function *unreadFlags = createUnreadFlagStoresFunction(module, cf);
  llvm::Function *readFlags = createReadFlagStoresFunction(module, cf);
  llvm::Function *readOneFlag =
      createReadOneFlagStoreOtherFlagFunction(module, cf, of);
  llvm::Function *flagRestoreOnly = createFlagRestoreOnlyFunction(module, of);
  llvm::Function *deadFlagInputBeforeConstantStore =
      createDeadFlagInputBeforeConstantStoreFunction(module, of);
  llvm::Function *unreadRip = createUnreadRipStoresFunction(module, rip);
  llvm::Function *readRip = createReadRipStoresFunction(module, rip);

  notdec::bin2llvm::NativeRegisterSSAOptions options;
  options.EnableRewrite = true;
  notdec::bin2llvm::NativeRegisterSSASummary summary =
      notdec::bin2llvm::runNativeRegisterSSA(module, options);

  if (llvm::verifyModule(module, &llvm::errs())) {
    std::cerr << "module verification failed after register SSA\n";
    return EXIT_FAILURE;
  }

  bool ok = true;
  ok &= expect(metadataHasRegister(*preserved, "notdec.register.preserves",
                                   "RBX"),
               "restored RBX was not marked preserved");
  ok &= expect(!metadataHasRegister(*preserved, "notdec.register.clobbers",
                                    "RBX"),
               "restored RBX was marked clobbered");
  ok &= expect(!metadataHasRegister(*clobbered, "notdec.register.preserves",
                                    "RBX"),
               "clobbered RBX was marked preserved");
  ok &= expect(metadataHasRegister(*clobbered, "notdec.register.clobbers",
                                   "RBX"),
               "clobbered RBX was not marked clobbered");
  ok &= expect(metadataHasRegister(*callEffects, "notdec.register.clobbers",
                                   "RAX"),
               "written killed-by-call RAX was not marked clobbered");
  ok &= expect(summary.PreservedRegisters == 1,
               "register effect summary had unexpected preserved count");
  ok &= expect(summary.ClobberedRegisters >= 7,
               "register effect summary missed expected clobbers");
  ok &= expect(summary.CallInputHelpers >= 1,
               "register SSA summary missed call input helpers");
  ok &= expect(summary.CallReturnHelpers >= 1,
               "register SSA summary missed call return helpers");
  ok &= expect(summary.CallEffectHelpers >= 1,
               "register SSA summary missed call effect helpers");
  ok &= expect(summary.StrongCallInputs >= 1,
               "register SSA summary missed strong call inputs");
  ok &= expect(summary.ActiveCallInputTrials >= 1,
               "register SSA summary missed active call input trials");
  ok &= expect(summary.InactiveCallInputTrials >= 1,
               "register SSA summary missed inactive call input trials");
  ok &= expect(summary.NoUseCallInputTrials >= 1,
               "register SSA summary missed no-use call input trials");
  ok &= expect(summary.DefinitelyNotUsedCallInputTrials >= 1,
               "register SSA summary missed definitely-not-used call input "
               "flags");
  ok &= expect(summary.KilledByCallInputTrials >= 1,
               "register SSA summary missed killed-by-call call input flags");
  ok &= expect(summary.ConditionalEffectCallInputTrials >= 1,
               "register SSA summary missed conditional-effect call input "
               "flags");
  ok &= expect(summary.ConditionalFinalCheckCallInputTrials >= 1,
               "register SSA summary missed conditional final-check call "
               "input flags");
  ok &= expect(summary.PathRealisticCallInputTrials >= 1,
               "register SSA summary missed path-realistic call input flags");
  ok &= expect(summary.PathConditionalCallInputTrials >= 1,
               "register SSA summary missed path-conditional call input flags");
  ok &= expect(summary.PathBlockedCallInputTrials >= 1,
               "register SSA summary missed path-blocked call input flags");
  ok &= expect(summary.LocalConstCallInputTrials >= 1,
               "register SSA summary missed local-const call input trials");
  ok &= expect(summary.LocalArithCallInputTrials >= 1,
               "register SSA summary missed local-arith call input trials");
  ok &= expect(summary.LocalCastCallInputTrials >= 1,
               "register SSA summary missed local-cast call input trials");
  ok &= expect(summary.LocalLoadCallInputTrials >= 1,
               "register SSA summary missed local-load call input trials");
  ok &= expect(summary.LocalUnknownCallInputTrials >= 1,
               "register SSA summary missed local-unknown call input trials");
  ok &= expect(summary.LocalSharedUseCallInputTrials >= 1,
               "register SSA summary missed local-shared-use call input "
               "trials");
  ok &= expect(summary.LocalDoubleCallUseCallInputTrials >= 1,
               "register SSA summary missed local-double-call-use call input "
               "trials");
  ok &= expect(summary.PhiCallInputTrials >= 1,
               "register SSA summary missed phi call input trials");
  ok &= expect(summary.EntryInputCallInputTrials >= 1,
               "register SSA summary missed entry-input call input trials");
  ok &= expect(summary.CallEffectCallInputTrials >= 1,
               "register SSA summary missed call-effect call input trials");
  ok &= expect(summary.DeadStoresRemoved >= 1,
               "register SSA did not remove the expected overwritten store");
  ok &= expect(summary.UnreadFlagStoresRemoved == 6,
               "register SSA did not remove unread flag stores");
  ok &= expect(summary.UnreadRipStoresRemoved == 1,
               "register SSA did not remove unread RIP stores");
  ok &= expect(countRegisterLoads(*callEffects, rbx) == 0,
               "RBX load after call was not propagated");
  ok &= expect(countRegisterLoads(*callEffects, rax) == 0,
               "RAX load after call was not rewritten to call effect");
  ok &= expect(countCallEffects(*callEffects, "return", "RAX") == 1,
               "RAX call return was not made explicit");
  ok &= expect(callEffectHasCallsiteId(*callEffects, "return", "RAX"),
               "RAX call return did not record a callsite id");
  ok &= expect(callEffectHasField(*callEffects, "return", "RAX",
                                  "source=abi_output"),
               "RAX call return did not record ABI output source");
  ok &= expect(callEffectStoresToRegister(*callEffects, "return", "RAX", rax),
               "RAX call return was not stored back to register state");
  ok &= expect(countCallInputCandidates(*callInputCandidate, "RDI") == 1,
               "RDI call input candidate value was not made explicit");
  ok &= expect(callInputCandidateUsesHelper(*callInputCandidate, "RDI"),
               "RDI call input candidate was not represented by helper call");
  ok &= expect(hasCallInputCandidateMetadata(*callInputCandidate, "RDI"),
               "RDI call input candidate metadata was not attached to call");
  ok &= expect(callInputCandidateHasCallsiteId(*callInputCandidate, "RDI"),
               "RDI call input candidate did not record a callsite id");
  ok &= expect(callInputCandidateHasField(*callInputCandidate, "RDI",
                                          "strength=strong_local_def"),
               "RDI call input candidate was not marked strong");
  ok &= expect(callInputCandidateHasField(*callInputCandidate, "RDI",
                                          "trial_state=active"),
               "RDI call input candidate was not marked active");
  ok &= expect(callInputCandidateHasField(*callInputCandidate, "RDI",
                                          "trial_reason=local_const"),
               "RDI call input candidate did not record local_const reason");
  ok &= expect(callInputCandidateHasField(*callInputCandidate, "RDI",
                                          "trial_flags=path_realistic"),
               "RDI active call input did not record path_realistic flag");
  ok &= expect(callInputCandidateHasField(*arithCallInput, "RDI",
                                          "trial_state=active"),
               "RDI arithmetic call input was not active");
  ok &= expect(callInputCandidateHasField(*arithCallInput, "RDI",
                                          "trial_reason=local_arith"),
               "RDI arithmetic call input did not record local_arith reason");
  ok &= expect(callInputCandidateHasField(*sharedUseCallInput, "RDI",
                                          "trial_state=inactive"),
               "RDI shared-use call input was not inactive");
  ok &= expect(callInputCandidateHasField(*sharedUseCallInput, "RDI",
                                          "trial_reason=local_shared_use"),
               "RDI shared-use call input reason was missing");
  ok &= expect(callInputCandidateHasField(*transparentUseCallInput, "RDI",
                                          "trial_state=active"),
               "RDI transparent-use call input was not active");
  ok &= expect(callInputCandidateHasField(*transparentUseCallInput, "RDI",
                                          "trial_reason=local_arith"),
               "RDI transparent-use call input did not stay local_arith");
  ok &= expect(callInputCandidateHasField(*doubleCallUseCallInput, "RDI",
                                          "trial_state=inactive"),
               "RDI double-call-use call input was not inactive");
  ok &= expect(callInputCandidateHasField(*doubleCallUseCallInput, "RDI",
                                          "trial_reason=local_double_call_use"),
               "RDI double-call-use call input reason was missing");
  ok &= expect(callInputCandidateHasField(*castCallInput, "RDI",
                                          "trial_state=active"),
               "RDI cast call input was not active");
  ok &= expect(callInputCandidateHasField(*castCallInput, "RDI",
                                          "trial_reason=local_cast"),
               "RDI cast call input did not record local_cast reason");
  ok &= expect(callInputCandidateHasField(*localLoadCallInput, "RDI",
                                          "trial_state=inactive"),
               "RDI local-load call input was not inactive");
  ok &= expect(callInputCandidateHasField(*localLoadCallInput, "RDI",
                                          "trial_reason=local_load"),
               "RDI local-load call input did not record local_load reason");
  ok &= expect(callInputCandidateHasField(*unknownInstCallInput, "RDI",
                                          "trial_state=inactive"),
               "RDI unknown instruction call input was not inactive");
  ok &= expect(callInputCandidateHasField(*unknownInstCallInput, "RDI",
                                          "trial_reason=local_unknown_inst"),
               "RDI unknown instruction call input reason was missing");
  ok &= expect(callInputCandidateHasField(*weakCallInput, "RDI",
                                          "strength=weak_entry_input"),
               "RDI entry-derived call input was not marked weak");
  ok &= expect(callInputCandidateHasField(*weakCallInput, "RDI",
                                          "trial_state=inactive"),
               "RDI entry-derived call input was not marked inactive");
  ok &= expect(callInputCandidateHasField(*weakCallInput, "RDI",
                                          "trial_reason=entry_input"),
               "RDI entry-derived call input did not record entry reason");
  ok &= expect(callInputCandidateHasField(*weakCallInput, "RDI",
                                          "trial_flags=path_blocked"),
               "RDI entry-derived call input did not record path_blocked flag");
  ok &= expect(callInputCandidateHasField(*blockedCallInput, "RDI",
                                          "strength=blocked_call_effect"),
               "RDI call-effect-derived input was not blocked");
  ok &= expect(callInputCandidateHasField(*blockedCallInput, "RDI",
                                          "trial_state=no_use"),
               "RDI call-effect-derived input was not marked no_use");
  ok &= expect(callInputCandidateHasField(*blockedCallInput, "RDI",
                                          "trial_reason=call_effect"),
               "RDI call-effect-derived input did not record call effect "
               "reason");
  ok &= expect(callInputCandidateHasField(
                   *blockedCallInput, "RDI",
                   "trial_flags=definitely_not_used,killed_by_call,path_blocked"),
               "RDI call-effect-derived input did not record trial flags");
  ok &= expect(callInputCandidateHasField(*strongPhiCallInput, "RDI",
                                          "strength=strong_phi"),
               "RDI PHI call input was not marked strong_phi");
  ok &= expect(callInputCandidateHasField(*strongPhiCallInput, "RDI",
                                          "trial_state=active"),
               "RDI PHI call input was not marked active");
  ok &= expect(callInputCandidateHasField(*strongPhiCallInput, "RDI",
                                          "trial_reason=phi"),
               "RDI PHI call input did not record phi reason");
  ok &= expect(callInputCandidateHasField(*conditionalPhiCallInput, "RDI",
                                          "trial_state=no_use"),
               "RDI conditional PHI call input was not no_use");
  ok &= expect(callInputCandidateHasField(*conditionalPhiCallInput, "RDI",
                                          "trial_reason=phi"),
               "RDI conditional PHI call input did not record phi reason");
  ok &= expect(callInputCandidateHasField(
                   *conditionalPhiCallInput, "RDI",
                   "trial_flags=conditional_effect,final_checked,path_conditional"),
               "RDI conditional PHI call input did not record flags");
  ok &= expect(countRegisterLoads(*stackPointerCallEffects, rsp) == 0,
               "RSP load after call was not propagated");
  ok &= expect(countRegisterLoads(*repeatedLoadAfterCall, rax) == 0,
               "repeated RAX loads after call were not rewritten");
  ok &= expect(countCallEffects(*repeatedLoadAfterCall, "clobber_unknown",
                                "RAX") == 0,
               "repeated RAX loads after call used clobber instead of return");
  ok &= expect(countCallEffects(*repeatedLoadAfterCall, "return", "RAX") == 1,
               "repeated RAX loads after call did not reuse call effect");
  ok &= expect(countRegisterLoads(*directCallEffects, rbx) == 0,
               "RBX load after direct preserving call was not propagated");
  ok &= expect(countCallEffects(*directRecoveredReturnEffects, "return",
                                "RAX") == 1,
               "direct recovered RAX return was not used as call return");
  ok &= expect(callEffectHasField(*directRecoveredReturnEffects, "return",
                                  "RAX",
                                  "source=callee_recovered_return"),
               "direct recovered RAX return did not record callee source");
  ok &= expect(countCallEffects(*directRecoveredReturnEffects, "return",
                                "RDX") == 0,
               "direct recovered callee treated non-return RDX as call return");
  ok &= expect(countRegisterLoads(*callerBeforeClobberingCallee, rbx) == 0,
               "RBX load after direct clobbering callee was not rewritten");
  ok &= expect(countCallEffects(*callerBeforeClobberingCallee,
                                "clobber_unknown", "RBX") == 1,
               "direct callee RBX clobber was not made explicit");
  ok &= expect(callEffectHasField(*callerBeforeClobberingCallee,
                                  "clobber_unknown", "RBX",
                                  "source=callee_clobbers"),
               "direct callee RBX clobber did not record callee source");
  ok &= expect(callEffectStoresToRegister(*callerBeforeClobberingCallee,
                                          "clobber_unknown", "RBX", rbx),
               "direct callee RBX clobber was not stored back to register state");
  ok &= expect(!moduleHasPhiMissingIncomingEffect(module),
               "register SSA still created phi_missing_incoming fallback effect");
  ok &= expect(staleMetadata->getMetadata("notdec.register.external_inputs") ==
                   nullptr,
               "stale external input metadata was not cleared");
  ok &= expect(staleMetadata->getMetadata("notdec.register.preserves") ==
                   nullptr,
               "stale preserved metadata was not cleared");
  ok &= expect(staleMetadata->getMetadata("notdec.register.clobbers") ==
                   nullptr,
               "stale clobber metadata was not cleared");
  ok &= expect(metadataHasRegister(*unmarkedRegisterLoad,
                                   "notdec.register.external_inputs", "RDI"),
               "unmarked RDI global load was not marked as external input");
  ok &= expect(countRegisterLoads(*unmarkedRegisterLoad, rdi) == 1,
               "unmarked RDI global load was not rewritten to one entry load");
  ok &= expect(!hasRegisterAccessLoad(*unmarkedRegisterLoad, rdi),
               "external input RDI load unexpectedly got access metadata");
  ok &= expect(countRegisterLoads(*unmarkedRegisterStoreLoad, rax) == 0,
               "unmarked RAX store/load was not propagated");
  ok &= expect(countRegisterStores(*overwrittenStore, rax) == 1,
               "overwritten RAX store was not removed");
  ok &= expect(countRegisterStores(*callBetweenStores, rax) == 2,
               "RAX store before call barrier was removed");
  ok &= expect(countRegisterStores(*partialCovered, rax) == 1,
               "partial RAX store covered by full store was not removed");
  ok &= expect(countRegisterLoads(*deadPartialInput, rax) == 0,
               "dead partial input left an unused RAX load");
  ok &= expect(deadPartialInput->getMetadata("notdec.register.external_inputs") ==
                   nullptr,
               "dead partial input left stale external input metadata");
  ok &= expect(countRegisterStores(*partialOnly, rax) == 1,
               "partial RAX stores were not folded into backing storage SSA");
  ok &= expect(metadataHasRegister(*partialOnly,
                                   "notdec.register.external_inputs", "RAX"),
               "partial RAX write did not keep old backing input");
  ok &= expect(countRegisterLoads(*partialMetadataStorage, rax) == 1,
               "partial metadata backing RAX store did not keep old high bits");
  ok &= expect(metadataHasRegister(*partialMetadataStorage,
                                   "notdec.register.external_inputs", "RAX"),
               "partial metadata backing RAX store did not keep external input");
  ok &= expect(countRegisterLoads(*widePartialLoad, rax) == 0,
               "wide partial RAX load was not propagated");
  ok &= expect(functionReturnsConstant(*widePartialLoad, 0xabcd),
               "wide partial RAX load did not extract the metadata range");
  ok &= expect(countRegisterLoads(*partialLoad, rax) == 0,
               "partial RAX load was not replaced with an SSA extract");
  ok &= expect(countRegisterLoads(*loopCarriedRegisterValue, rax) == 0,
               "loop-carried RAX load was not replaced with a PHI");
  ok &= expect(countRegisterLoads(*trivialJoinRegisterPhi, rax) == 0,
               "trivial join RAX load was not replaced");
  ok &= expect(countRegisterPhis(*trivialJoinRegisterPhi, "RAX") == 0,
               "trivial join RAX PHI was not removed");
  ok &= expect(functionReturnsConstant(*trivialJoinRegisterPhi, 0x4242),
               "trivial join RAX value was not preserved");
  ok &= expect(countRegisterLoads(*cascadedTrivialRegisterPhi, rax) == 0,
               "cascaded trivial RAX load was not replaced");
  ok &= expect(countRegisterPhis(*cascadedTrivialRegisterPhi, "RAX") == 0,
               "cascaded trivial RAX PHIs were not removed");
  ok &= expect(functionReturnsConstant(*cascadedTrivialRegisterPhi, 0x5151),
               "cascaded trivial RAX value was not preserved");
  std::unique_ptr<llvm::Module> missingPhiIncomingModule =
      createMissingPhiIncomingModule(context);
  ok &= expect(llvm::verifyModule(*missingPhiIncomingModule, &llvm::nulls()),
               "LLVM verifier accepted PHI with missing incoming edge");
  ok &= expect(unreachableRegisterLoad->size() == 1 &&
                   countRegisterLoads(*unreachableRegisterLoad, rax) == 0,
               "unreachable register load block was not removed");
  ok &= expect(countRegisterStores(*unreadFlags, cf) == 0,
               "unread CF stores were not removed");
  ok &= expect(countRegisterStores(*readFlags, cf) == 0,
               "resolved CF store was not removed");
  ok &= expect(countRegisterStores(*readOneFlag, cf) == 0,
               "resolved CF store was not removed when another flag was unread");
  ok &= expect(countRegisterStores(*readOneFlag, of) == 0,
               "unread OF store was not removed when CF was read");
  ok &= expect(countRegisterLoads(*flagRestoreOnly, of) == 0,
               "dead OF restore input was not removed");
  ok &= expect(countRegisterStores(*flagRestoreOnly, of) == 0,
               "dead OF restore store was not removed");
  ok &= expect(flagRestoreOnly->getMetadata("notdec.register.external_inputs") ==
                   nullptr,
               "dead OF restore left stale external input metadata");
  ok &= expect(countRegisterLoads(*deadFlagInputBeforeConstantStore, of) == 0,
               "dead OF input before constant store was not removed");
  ok &= expect(countRegisterStores(*deadFlagInputBeforeConstantStore, of) == 0,
               "dead OF constant store was not removed");
  ok &= expect(deadFlagInputBeforeConstantStore->getMetadata(
                   "notdec.register.external_inputs") == nullptr,
               "dead OF constant store left stale external input metadata");
  ok &= expect(countRegisterStores(*unreadRip, rip) == 0,
               "unread RIP store was not removed");
  ok &= expect(countRegisterStores(*readRip, rip) == 1,
               "read RIP store was removed");

  llvm::Module returnForwardModule("native-register-return-forward-test",
                                   context);
  llvm::GlobalVariable *returnForwardRax =
      createRegisterGlobal(returnForwardModule, "RAX");
  attachRaxInputOutputAbi(returnForwardModule);
  llvm::Function *returnForwardCallInput =
      createReturnForwardCallInputFunction(returnForwardModule,
                                           returnForwardRax);
  notdec::bin2llvm::NativeRegisterSSASummary returnForwardSummary =
      notdec::bin2llvm::runNativeRegisterSSA(returnForwardModule, options);
  if (llvm::verifyModule(returnForwardModule, &llvm::errs())) {
    std::cerr << "return-forward module verification failed after register SSA\n";
    return EXIT_FAILURE;
  }
  ok &= expect(callInputCandidateHasField(*returnForwardCallInput, "RAX",
                                          "strength=return_forward"),
               "RAX call input forwarding prior return was not marked");
  ok &= expect(callInputCandidateHasField(*returnForwardCallInput, "RAX",
                                          "trial_state=inactive"),
               "RAX call input forwarding prior return was not inactive");
  ok &= expect(callInputCandidateHasField(*returnForwardCallInput, "RAX",
                                          "trial_reason=return_forward"),
               "RAX call input forwarding prior return reason was missing");
  ok &= expect(returnForwardSummary.WeakCallInputs >= 1,
               "return-forward summary missed non-strong call input");
  ok &= expect(returnForwardSummary.ReturnForwardCallInputTrials >= 1,
               "return-forward summary missed return-forward trial reason");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
