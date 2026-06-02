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
  llvm::Function *stackPointerCallEffects =
      createStackPointerCallEffectFunction(module, rsp);
  llvm::Function *repeatedLoadAfterCall =
      createRepeatedLoadAfterCallFunction(module, rax);
  llvm::Function *directCallEffects =
      createDirectCallEffectFunction(module, rbx);
  llvm::Function *callerBeforeClobberingCallee =
      createCallerBeforeClobberingCalleeFunction(module, rbx);
  llvm::Function *staleMetadata = createStaleMetadataFunction(module, rbx);
  llvm::Function *unmarkedRegisterLoad =
      createUnmarkedRegisterLoadFunction(module, rdi);
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
  llvm::Function *partialLoad = createPartialLoadFunction(module, rax);
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
  ok &= expect(summary.DeadStoresRemoved >= 1,
               "register SSA did not remove the expected overwritten store");
  ok &= expect(summary.UnreadFlagStoresRemoved == 6,
               "register SSA did not remove unread flag stores");
  ok &= expect(summary.UnreadRipStoresRemoved == 1,
               "register SSA did not remove unread RIP stores");
  ok &= expect(countRegisterLoads(*callEffects, rbx) == 0,
               "RBX load after call was not propagated");
  ok &= expect(countRegisterLoads(*callEffects, rax) == 1,
               "RAX load after call was incorrectly propagated");
  ok &= expect(countRegisterLoads(*stackPointerCallEffects, rsp) == 0,
               "RSP load after call was not propagated");
  ok &= expect(countRegisterLoads(*repeatedLoadAfterCall, rax) == 1,
               "repeated RAX load after call was not reused");
  ok &= expect(countRegisterLoads(*directCallEffects, rbx) == 0,
               "RBX load after direct preserving call was not propagated");
  ok &= expect(countRegisterLoads(*callerBeforeClobberingCallee, rbx) == 1,
               "RBX load after late direct clobbering callee was propagated");
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
  ok &= expect(countRegisterLoads(*partialMetadataStorage, rax) == 0,
               "partial metadata backing RAX load was not propagated");
  ok &= expect(countRegisterLoads(*partialLoad, rax) == 0,
               "partial RAX load was not replaced with an SSA extract");
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
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
