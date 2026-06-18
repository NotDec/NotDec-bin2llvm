#include "notdec-bin2llvm/passes/NativeExternalCallSignatureRewrite.h"
#include "notdec-bin2llvm/passes/NativeRegisterSummarySSA.h"
#include "notdec-bin2llvm/NativeAbi.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
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

  for (const char *name : {"RDI", "RSI", "RDX"}) {
    notdec::bin2llvm::NativeAbiParamEntry input;
    input.MinSize = 1;
    input.MaxSize = 8;
    input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    input.Storage.Name = name;
    abi.Inputs.push_back(input);
  }

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

bool callsFunction(const llvm::CallInst &call, const llvm::Function &function) {
  return call.getCalledOperand()->stripPointerCasts() == &function;
}

bool callsFunctionNamed(const llvm::CallInst &call, llvm::StringRef name) {
  llvm::Function *callee = call.getCalledFunction();
  return callee != nullptr && callee->getName() == name;
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

bool testUnknownConflictUsesMinimumArgs() {
  llvm::LLVMContext context;
  llvm::Module module("external-call-conflict-min", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "unknown_external",
      module);
  llvm::Function *caller = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> builder(entry);

  llvm::Value *one =
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 1);
  llvm::Value *two =
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 2);
  storeRegister(builder, rdi, one, "RDI");
  builder.CreateCall(calleeType, callee);
  storeRegister(builder, rdi, one, "RDI");
  llvm::StoreInst *extraStore = storeRegister(builder, rsi, two, "RSI");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  auto rewriteSummary =
      notdec::bin2llvm::runNativeExternalCallSignatureRewrite(module);
  unsigned rewrittenCalls = 0;
  for (llvm::Instruction &inst : llvm::instructions(caller)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && callsFunction(*call, *callee)) {
      rewrittenCalls += call->arg_size() == 1 ? 1 : 0;
    }
  }

  return expect(rewriteSummary.CallsRewritten == 2,
                "conflicting unknown external calls were not rewritten") &&
         expect(rewriteSummary.SymbolsResolvedWithMinimumArgs == 1,
                "conflicting unknown external symbol did not use minimum args") &&
         expect(rewrittenCalls == 2,
                "conflicting unknown external did not use one-arg calls") &&
         expect(extraStore->getParent() != nullptr,
                "extra argument store should not be removed when using minimum args") &&
         verifyOk(module, "module failed verifier after conflict-min rewrite");
}

bool testKnownFixedPrototypeSkipsIncompleteCallsite() {
  llvm::LLVMContext context;
  llvm::Module module("external-call-known-fixed", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *strcmpFn = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "strcmp", module);
  llvm::Function *caller = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> builder(entry);

  llvm::Value *one =
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 1);
  llvm::Value *two =
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 2);
  storeRegister(builder, rdi, one, "RDI");
  builder.CreateCall(calleeType, strcmpFn);
  storeRegister(builder, rdi, one, "RDI");
  storeRegister(builder, rsi, two, "RSI");
  builder.CreateCall(calleeType, strcmpFn);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  auto rewriteSummary =
      notdec::bin2llvm::runNativeExternalCallSignatureRewrite(module);
  unsigned noArgCalls = 0;
  unsigned twoArgCalls = 0;
  for (llvm::Instruction &inst : llvm::instructions(caller)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && callsFunction(*call, *strcmpFn)) {
      noArgCalls += call->arg_empty() ? 1 : 0;
      twoArgCalls += call->arg_size() == 2 ? 1 : 0;
    }
  }

  return expect(rewriteSummary.CallsRewritten == 1,
                "known fixed prototype should rewrite complete callsite only") &&
         expect(rewriteSummary.SymbolsResolvedWithKnownPrototype == 1,
                "known fixed prototype was not used") &&
         expect(rewriteSummary.CallsSkippedForMissingKnownArgs == 1,
                "incomplete known callsite was not skipped") &&
         expect(noArgCalls == 1 && twoArgCalls == 1,
                "known fixed prototype callsites were not rewritten as expected") &&
         verifyOk(module, "module failed verifier after known fixed rewrite");
}

bool testCrossBlockPreparedArgIsRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("external-call-cross-block-arg", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "unknown_cross_block",
      module);
  llvm::Function *caller = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", caller);
  llvm::BasicBlock *callBlock =
      llvm::BasicBlock::Create(context, "call", caller);
  llvm::IRBuilder<> builder(entry);

  storeRegister(builder, rdi,
                llvm::ConstantInt::get(rdi->getValueType(), 42), "RDI");
  builder.CreateBr(callBlock);
  builder.SetInsertPoint(callBlock);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  auto rewriteSummary =
      notdec::bin2llvm::runNativeExternalCallSignatureRewrite(module);
  unsigned oneArgCalls = 0;
  for (llvm::Instruction &inst : llvm::instructions(caller)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && callsFunctionNamed(*call, "unknown_cross_block")) {
      oneArgCalls += call->arg_size() == 1 ? 1 : 0;
    }
  }

  return expect(rewriteSummary.CallsRewritten == 1,
                "cross-block prepared argument was not rewritten") &&
         expect(oneArgCalls == 1,
                "cross-block prepared argument did not become call operand") &&
         verifyOk(module, "module failed verifier after cross-block rewrite");
}

} // namespace

int main() {
  return testExternalCallSignatureRewrite() &&
                 testUnknownConflictUsesMinimumArgs() &&
                 testKnownFixedPrototypeSkipsIncompleteCallsite() &&
                 testCrossBlockPreparedArgIsRewritten()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
