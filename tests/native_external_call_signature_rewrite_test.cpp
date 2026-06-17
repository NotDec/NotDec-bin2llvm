#include "notdec-bin2llvm/passes/NativeExternalCallSignatureRewrite.h"
#include "notdec-bin2llvm/passes/NativeRegisterSummarySSA.h"
#include "notdec-bin2llvm/NativeAbi.h"

#include "llvm/IR/Constants.h"
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
  auto *global = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, name);
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
  abi.PrototypeName = "__external_call_rewrite_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";

  notdec::bin2llvm::NativeAbiParamEntry input;
  input.MinSize = 1;
  input.MaxSize = 8;
  input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  input.Storage.Name = "RDI";
  abi.Inputs.push_back(input);

  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
}

llvm::StoreInst *storeRegister(llvm::IRBuilder<> &builder,
                               llvm::GlobalVariable *reg, llvm::Value *value,
                               const std::string &name) {
  llvm::StoreInst *store = builder.CreateStore(value, reg);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(reg->getContext(), name));
  return store;
}

bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool verifyOk(llvm::Module &module, const char *message) {
  std::string error;
  llvm::raw_string_ostream os(error);
  if (!llvm::verifyModule(module, &os)) {
    return true;
  }
  os.flush();
  std::cerr << message << "\n" << error << '\n';
  return false;
}

bool testExternalCallSignatureRewrite() {
  llvm::LLVMContext context;
  llvm::Module module("external-call-rewrite", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "external_callee",
      module);
  llvm::Function *caller = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *arg =
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 42);
  llvm::StoreInst *store = storeRegister(builder, rdi, arg, "RDI");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  if (!expect(summary.CallArgStoresMarked == 1,
              "summary did not mark external call argument store")) {
    return false;
  }
  auto rewriteSummary =
      notdec::bin2llvm::runNativeExternalCallSignatureRewrite(module);
  return expect(rewriteSummary.CallsRewritten == 1,
                "external call was not rewritten") &&
         expect(module.getFunction("external_callee") != nullptr,
                "callee signature was not updated") &&
         expect(module.getFunction("external_callee")->getFunctionType()->
                        getNumParams() == 1,
                "callee signature was not updated") &&
         expect(store->getParent() == nullptr,
                "parameter store was not removed") &&
         verifyOk(module,
                  "module failed verifier after external call rewrite test");
}

} // namespace

int main() {
  return testExternalCallSignatureRewrite() ? EXIT_SUCCESS : EXIT_FAILURE;
}
