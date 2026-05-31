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

void attachTestAbi(llvm::Module &module) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__stdcall";
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
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  attachTestAbi(module);
  llvm::Function *preserved =
      createFunction(module, "preserved_rbx", rbx, true);
  llvm::Function *clobbered =
      createFunction(module, "clobbered_rbx", rbx, false);
  llvm::Function *callEffects = createCallEffectFunction(module, rbx, rax);
  llvm::Function *directCallEffects =
      createDirectCallEffectFunction(module, rbx);
  llvm::Function *callerBeforeClobberingCallee =
      createCallerBeforeClobberingCalleeFunction(module, rbx);
  llvm::Function *staleMetadata = createStaleMetadataFunction(module, rbx);
  llvm::Function *unmarkedRegisterLoad =
      createUnmarkedRegisterLoadFunction(module, rdi);

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
  ok &= expect(summary.ClobberedRegisters == 3,
               "register effect summary had unexpected clobber count");
  ok &= expect(countRegisterLoads(*callEffects, rbx) == 0,
               "RBX load after call was not propagated");
  ok &= expect(countRegisterLoads(*callEffects, rax) == 1,
               "RAX load after call was incorrectly propagated");
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
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
