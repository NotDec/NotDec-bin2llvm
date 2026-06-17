#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/passes/NativeRegisterSummary.h"

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
  abi.PrototypeName = "__summary_test";
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
  abi.Outputs.push_back(output);
  output.Storage.Name = "RDX";
  abi.Outputs.push_back(output);

  notdec::bin2llvm::NativeAbiEffect killed;
  killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
  killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  killed.Storage.Name = "RAX";
  abi.Effects.push_back(killed);
  killed.Storage.Name = "RDX";
  abi.Effects.push_back(killed);

  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
}

llvm::StoreInst *storeRegister(llvm::IRBuilder<> &builder,
                               llvm::GlobalVariable *reg, uint64_t value,
                               const std::string &name) {
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(reg->getValueType(), value), reg);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(reg->getContext(), name));
  return store;
}

llvm::LoadInst *loadRegister(llvm::IRBuilder<> &builder,
                             llvm::GlobalVariable *reg, const std::string &name,
                             const std::string &valueName = "") {
  llvm::LoadInst *load =
      builder.CreateLoad(reg->getValueType(), reg, valueName);
  load->setMetadata("notdec.register.access",
                    registerAccessMetadata(reg->getContext(), name));
  return load;
}

const notdec::bin2llvm::NativeRegisterSummaryFunction *
functionSummary(const notdec::bin2llvm::NativeRegisterSummary &summary,
                const std::string &name) {
  for (const auto &function : summary.Functions) {
    if (function.FunctionName == name) {
      return &function;
    }
  }
  return nullptr;
}

const notdec::bin2llvm::NativeRegisterSummaryRegister *
registerSummary(const notdec::bin2llvm::NativeRegisterSummaryFunction &function,
                const std::string &name) {
  for (const auto &reg : function.Registers) {
    if (reg.Name == name) {
      return &reg;
    }
  }
  return nullptr;
}

bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool testKilledReadDoesNotBecomeInput() {
  llvm::LLVMContext context;
  llvm::Module module("summary-killed-read", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "killed_read", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, 7, "RDI");
  (void)loadRegister(builder, rdi, "RDI", "after_kill");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module);
  const auto *fn = functionSummary(summary, "killed_read");
  const auto *rdiSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RDI");
  return expect(rdiSummary != nullptr, "missing RDI summary") &&
         expect(!rdiSummary->ReadEntry,
                "killed RDI read was incorrectly marked readEntry") &&
         expect(rdiSummary->MayNonEntry, "RDI write was not marked modified");
}

bool testCalleeReadPropagatesToCallerEntry() {
  llvm::LLVMContext context;
  llvm::Module module("summary-call-read", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "reads_rdi", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  (void)loadRegister(calleeBuilder, rdi, "RDI", "arg");
  calleeBuilder.CreateRetVoid();

  llvm::Function *caller =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "caller_entry_forwards_rdi", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> callerBuilder(callerEntry);
  callerBuilder.CreateCall(type, callee);
  callerBuilder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module);
  const auto *fn = functionSummary(summary, "caller_entry_forwards_rdi");
  const auto *rdiSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RDI");
  return expect(rdiSummary != nullptr, "missing caller RDI summary") &&
         expect(rdiSummary->ReadEntry,
                "callee readEntry did not propagate to caller entry RDI");
}

bool testSparseJoinKeepsUntouchedPath() {
  llvm::LLVMContext context;
  llvm::Module module("summary-sparse-join", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "branch_write_then_read", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *writeBlock =
      llvm::BasicBlock::Create(context, "write", function);
  llvm::BasicBlock *keepBlock =
      llvm::BasicBlock::Create(context, "keep", function);
  llvm::BasicBlock *joinBlock =
      llvm::BasicBlock::Create(context, "join", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), writeBlock,
                       keepBlock);
  builder.SetInsertPoint(writeBlock);
  storeRegister(builder, rdi, 7, "RDI");
  builder.CreateBr(joinBlock);
  builder.SetInsertPoint(keepBlock);
  builder.CreateBr(joinBlock);
  builder.SetInsertPoint(joinBlock);
  (void)loadRegister(builder, rdi, "RDI", "maybe_entry");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module);
  const auto *fn = functionSummary(summary, "branch_write_then_read");
  const auto *rdiSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RDI");
  return expect(rdiSummary != nullptr, "missing branch RDI summary") &&
         expect(rdiSummary->ReadEntry,
                "sparse join lost untouched path before read") &&
         expect(rdiSummary->MayEntry,
                "sparse join lost mayEntry from untouched path") &&
         expect(rdiSummary->MayNonEntry,
                "sparse join lost mayNonEntry from write path");
}

bool testTopDownDemandKeepsOnlyUsedReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-demanded-return", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "writes_rax_rdx", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  storeRegister(calleeBuilder, rax, 1, "RAX");
  storeRegister(calleeBuilder, rdx, 2, "RDX");
  calleeBuilder.CreateRetVoid();

  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *caller = llvm::Function::Create(
      callerType, llvm::GlobalValue::ExternalLinkage, "uses_only_rax", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> callerBuilder(callerEntry);
  callerBuilder.CreateCall(calleeType, callee);
  llvm::LoadInst *loaded = loadRegister(callerBuilder, rax, "RAX", "ret_rax");
  callerBuilder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module);
  const auto *fn = functionSummary(summary, "writes_rax_rdx");
  const auto *raxSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RAX");
  const auto *rdxSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RDX");
  return expect(raxSummary != nullptr, "missing callee RAX summary") &&
         expect(rdxSummary != nullptr, "missing callee RDX summary") &&
         expect(raxSummary->MayNonEntry, "RAX write was not marked modified") &&
         expect(rdxSummary->MayNonEntry, "RDX write was not marked modified") &&
         expect(raxSummary->ExitDemand,
                "used RAX return was not marked demanded") &&
         expect(!rdxSummary->ExitDemand,
                "unused RDX return was incorrectly marked demanded");
}

} // namespace

int main() {
  bool ok = true;
  ok &= testKilledReadDoesNotBecomeInput();
  ok &= testCalleeReadPropagatesToCallerEntry();
  ok &= testSparseJoinKeepsUntouchedPath();
  ok &= testTopDownDemandKeepsOnlyUsedReturn();
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
