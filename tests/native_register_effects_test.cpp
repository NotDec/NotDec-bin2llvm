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

llvm::MDNode *registerAccessMetadata(llvm::LLVMContext &context) {
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "base=RBX"),
      llvm::MDString::get(context, "space=register"),
      llvm::MDString::get(context, "offset=0"),
      llvm::MDString::get(context, "size=8"),
      llvm::MDString::get(context, "name=RBX"),
  };
  return llvm::MDNode::get(context, fields);
}

llvm::GlobalVariable *createRbxGlobal(llvm::Module &module) {
  llvm::LLVMContext &context = module.getContext();
  auto *type = llvm::Type::getInt64Ty(context);
  auto *global = new llvm::GlobalVariable(
      module, type, false, llvm::GlobalValue::ExternalLinkage, nullptr, "RBX");
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "space=register"),
      llvm::MDString::get(context, "offset=0"),
      llvm::MDString::get(context, "size=8"),
      llvm::MDString::get(context, "name=RBX"),
  };
  global->setMetadata("notdec.register", llvm::MDNode::get(context, fields));
  return global;
}

void attachRbxUnaffectedAbi(llvm::Module &module) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__stdcall";
  notdec::bin2llvm::NativeAbiEffect effect;
  effect.Kind = notdec::bin2llvm::NativeAbiEffectKind::Unaffected;
  effect.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  effect.Storage.Name = "RBX";
  abi.Effects.push_back(std::move(effect));
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
  llvm::MDNode *accessMetadata = registerAccessMetadata(context);

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
  llvm::GlobalVariable *rbx = createRbxGlobal(module);
  attachRbxUnaffectedAbi(module);
  llvm::Function *preserved =
      createFunction(module, "preserved_rbx", rbx, true);
  llvm::Function *clobbered =
      createFunction(module, "clobbered_rbx", rbx, false);

  notdec::bin2llvm::NativeRegisterSSAOptions options;
  options.EnableRewrite = true;
  notdec::bin2llvm::runNativeRegisterSSA(module, options);

  if (llvm::verifyModule(module, &llvm::errs())) {
    std::cerr << "module verification failed after register SSA\n";
    return EXIT_FAILURE;
  }

  bool ok = true;
  ok &= expect(metadataHasRegister(*preserved, "notdec.register.preserves",
                                   "RBX"),
               "restored RBX was not marked preserved");
  ok &= expect(!metadataHasRegister(*clobbered, "notdec.register.preserves",
                                    "RBX"),
               "clobbered RBX was marked preserved");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
