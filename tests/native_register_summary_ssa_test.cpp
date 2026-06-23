#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/passes/summary/NativeRegisterSummary.h"
#include "notdec-bin2llvm/passes/summary/NativeRegisterSummarySSA.h"
#include "notdec-bin2llvm/passes/summary/NativeStackFrame.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/APInt.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Intrinsics.h"
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

llvm::GlobalVariable *createRegisterGlobal(llvm::Module &module,
                                           const std::string &name,
                                           llvm::Type *type, uint64_t offset,
                                           uint64_t size) {
  llvm::LLVMContext &context = module.getContext();
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

void attachTestAbiWithInputs(llvm::Module &module,
                             std::initializer_list<llvm::StringRef> inputs) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";

  for (llvm::StringRef name : inputs) {
    notdec::bin2llvm::NativeAbiParamEntry input;
    input.MinSize = 1;
    input.MaxSize = 8;
    input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    input.Storage.Name = name.str();
    abi.Inputs.push_back(input);
  }

  notdec::bin2llvm::NativeAbiParamEntry output;
  output.MinSize = 1;
  output.MaxSize = 8;
  output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  output.Storage.Name = "RAX";
  abi.Outputs.push_back(output);

  notdec::bin2llvm::NativeAbiEffect killed;
  killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
  killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  killed.Storage.Name = "RAX";
  abi.Effects.push_back(killed);

  for (llvm::StringRef name : {"RBX", "RBP"}) {
    notdec::bin2llvm::NativeAbiEffect unaffected;
    unaffected.Kind = notdec::bin2llvm::NativeAbiEffectKind::Unaffected;
    unaffected.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    unaffected.Storage.Name = name.str();
    abi.Effects.push_back(unaffected);
  }

  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
}

void attachTestAbi(llvm::Module &module) {
  attachTestAbiWithInputs(module, {"RDI"});
}

llvm::StoreInst *storeRegister(llvm::IRBuilder<> &builder,
                               llvm::GlobalVariable *reg, llvm::Value *value,
                               const std::string &name) {
  llvm::StoreInst *store = builder.CreateStore(value, reg);
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

bool hasNamedAlloca(llvm::Function &function, llvm::StringRef prefix) {
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst);
    if (alloca != nullptr && alloca->hasName() &&
        alloca->getName().starts_with(prefix)) {
      return true;
    }
  }
  return false;
}

const notdec::bin2llvm::NativeRegisterSummaryFunction *
functionSummary(const notdec::bin2llvm::NativeRegisterSummary &summary,
                llvm::StringRef name) {
  for (const auto &function : summary.Functions) {
    if (function.FunctionName == name) {
      return &function;
    }
  }
  return nullptr;
}

const notdec::bin2llvm::NativeRegisterSummaryRegister *
registerSummary(const notdec::bin2llvm::NativeRegisterSummaryFunction &function,
                llvm::StringRef name) {
  for (const auto &reg : function.Registers) {
    if (reg.Name == name) {
      return &reg;
    }
  }
  return nullptr;
}

bool hasCompletePhi(llvm::Function &function) {
  for (llvm::BasicBlock &block : function) {
    unsigned predecessors = llvm::pred_size(&block);
    for (llvm::Instruction &inst : block) {
      auto *phi = llvm::dyn_cast<llvm::PHINode>(&inst);
      if (phi != nullptr && phi->getNumIncomingValues() == predecessors) {
        return true;
      }
    }
  }
  return false;
}

bool hasPhiIncomingCount(llvm::Function &function, unsigned count) {
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &inst : block) {
      auto *phi = llvm::dyn_cast<llvm::PHINode>(&inst);
      if (phi != nullptr && phi->getNumIncomingValues() == count) {
        return true;
      }
    }
  }
  return false;
}

bool hasLiveReplacedRegisterLoad(llvm::Function &function) {
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
    if (load != nullptr &&
        load->getMetadata("notdec.register.summary_ssa.replaced") != nullptr &&
        !load->use_empty()) {
      return true;
    }
  }
  return false;
}

bool testPhiIncomingMatchesPredecessors() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-phi", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "branch_merge", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right =
      llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), left, right);
  builder.SetInsertPoint(left);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 1),
                "RAX");
  builder.CreateBr(join);
  builder.SetInsertPoint(right);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 2),
                "RAX");
  builder.CreateBr(join);
  builder.SetInsertPoint(join);
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "merged");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.LoadsReplaced == 1, "branch load was not replaced") &&
         expect(hasCompletePhi(*function), "complete PHI was not created") &&
         expect(!hasLiveReplacedRegisterLoad(*function),
                "replaced load was reused by completed PHI") &&
         verifyOk(module, "module failed verifier after summary SSA PHI test");
}

bool testDuplicatePredecessorEdgesKeepPhiComplete() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-duplicate-edge-phi", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "duplicate_edge_phi", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *other =
      llvm::BasicBlock::Create(context, "other", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);

  llvm::IRBuilder<> builder(entry);
  llvm::Value *selector =
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0);
  llvm::SwitchInst *switchInst = builder.CreateSwitch(selector, join, 2);
  switchInst->addCase(
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 1), join);
  switchInst->addCase(
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 2), other);
  builder.SetInsertPoint(other);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 3),
                "RAX");
  builder.CreateBr(join);
  builder.SetInsertPoint(join);
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "merged");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.LoadsReplaced == 1,
                "duplicate-edge load was not replaced") &&
         expect(hasPhiIncomingCount(*function, 3),
                "duplicate predecessor edge PHI was not completed") &&
         verifyOk(module,
                  "module failed verifier after duplicate-edge PHI test");
}

bool testUnknownPhiIncomingUsesFrozenPoison() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-frozen-unknown", context);
  attachTestAbi(module);
  llvm::GlobalVariable *r10 = createRegisterGlobal(module, "R10");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *unknownExternal = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "unknown_external", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "unknown_phi", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right =
      llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), left, right);
  builder.SetInsertPoint(left);
  storeRegister(builder, r10, llvm::ConstantInt::get(r10->getValueType(), 42),
                "R10");
  builder.CreateBr(join);
  builder.SetInsertPoint(right);
  builder.CreateCall(voidType, unknownExternal);
  builder.CreateBr(join);
  builder.SetInsertPoint(join);
  llvm::LoadInst *loaded = loadRegister(builder, r10, "R10", "merged");
  builder.CreateRet(loaded);

  notdec::bin2llvm::NativeRegisterSummarySSAOptions options;
  options.EnableResidueRemoval = false;
  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module, options);

  bool hasFrozenIncoming = false;
  bool hasUndefIncoming = false;
  for (llvm::Instruction &inst : llvm::instructions(*function)) {
    auto *phi = llvm::dyn_cast<llvm::PHINode>(&inst);
    if (phi == nullptr) {
      continue;
    }
    for (llvm::Value *incoming : phi->incoming_values()) {
      hasFrozenIncoming |= llvm::isa<llvm::FreezeInst>(incoming);
      hasUndefIncoming |= llvm::isa<llvm::UndefValue>(incoming);
    }
  }

  return expect(summary.LoadsReplaced == 1,
                "unknown incoming load was not replaced") &&
         expect(summary.UnknownCallEffects >= 1,
                "unknown call effect was not observed") &&
         expect(hasFrozenIncoming,
                "unknown incoming was not materialized as freeze poison") &&
         expect(!hasUndefIncoming,
                "unknown incoming still used bare undef") &&
         verifyOk(module,
                  "module failed verifier after frozen unknown incoming test");
}

bool testPreservedCallKeepsPreviousValue() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-preserved-call", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "preserves_rax", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  calleeBuilder.CreateRetVoid();

  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *caller = llvm::Function::Create(
      callerType, llvm::GlobalValue::ExternalLinkage, "caller", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *stored = llvm::ConstantInt::get(rax->getValueType(), 42);
  storeRegister(builder, rax, stored, "RAX");
  builder.CreateCall(voidType, callee);
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "after_call");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.LoadsReplaced == 1,
                "preserved call load not replaced") &&
         expect(summary.DeadLoadsRemoved == 1,
                "preserved call replaced load was not removed") &&
         verifyOk(module, "module failed verifier after preserved call test");
}

bool testDemandedReturnCreatesCallValue() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-demanded-return", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "writes_rax", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  storeRegister(calleeBuilder, rax,
                llvm::ConstantInt::get(rax->getValueType(), 7), "RAX");
  calleeBuilder.CreateRetVoid();

  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *caller = llvm::Function::Create(
      callerType, llvm::GlobalValue::ExternalLinkage, "uses_return", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "ret_rax");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.CallReturnValues == 1,
                "demanded return helper was not created") &&
         expect(summary.DeadLoadsRemoved == 1,
                "return replaced load was not removed") &&
         verifyOk(module, "module failed verifier after demanded return test");
}

bool testIntrinsicDoesNotCreateCallValue() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-intrinsic-call", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "intrinsic_between_registers", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *stored = llvm::ConstantInt::get(rax->getValueType(), 42);
  storeRegister(builder, rax, stored, "RAX");
  llvm::Function *ctpop = llvm::Intrinsic::getOrInsertDeclaration(
      &module, llvm::Intrinsic::ctpop, {rax->getValueType()});
  builder.CreateCall(ctpop->getFunctionType(), ctpop, {stored});
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "after_intrinsic");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.LoadsReplaced == 1,
                "intrinsic-separated load was not replaced") &&
         expect(summary.CallReturnValues == 0,
                "intrinsic call created a register return helper") &&
         verifyOk(module, "module failed verifier after intrinsic call test");
}

bool testOverwrittenStoreIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-dead-store", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "overwritten_store", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 1),
                "RAX");
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 2),
                "RAX");
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "final_rax");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.LoadsReplaced == 1, "final load was not replaced") &&
         expect(summary.DeadLoadsRemoved == 1,
                "final replaced load was not removed") &&
         expect(summary.DeadStoresRemoved == 2,
                "overwritten stores were not removed") &&
         verifyOk(module, "module failed verifier after dead store test");
}

bool testCrossBlockDeadStoreIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-cross-block-dead-store", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "cross_block_store", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *next = llvm::BasicBlock::Create(context, "next", function);

  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 1),
                "RAX");
  builder.CreateBr(next);
  builder.SetInsertPoint(next);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 2),
                "RAX");
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "final_rax");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.LoadsReplaced == 1,
                "cross-block final load was not replaced") &&
         expect(summary.DeadStoresRemoved == 2,
                "cross-block dead stores were not removed") &&
         verifyOk(module, "module failed verifier after cross-block test");
}

bool testAbiInputStoreBeforeCallIsKept() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-call-input-store", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "external_callee", module);

  llvm::Function *function =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "call_input_store", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 42),
                "RDI");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  bool callRewritten = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName() == "external_callee" &&
        call->arg_size() == 1) {
      callRewritten = true;
    }
  }
  return expect(summary.DeadStoresRemoved == 1,
                "ABI input store before call was not removed") &&
         expect(summary.CallArgStoresMarked == 1,
                "ABI input store before call was not collected") &&
         expect(callRewritten, "ABI input call was not rewritten") &&
         verifyOk(module, "module failed verifier after call input test");
}

bool testKnownZeroArgExternalDropsAbiInputs() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-zero-arg-external", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "random", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "known_zero_arg_call", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 42),
                "RDI");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::CallInst *call = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      if (candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "random") {
        call = candidate;
      }
    }
  }

  return expect(call != nullptr, "known zero-arg external call missing") &&
         expect(call->arg_empty(),
                "known zero-arg external kept ABI arguments") &&
         expect(summary.DeadStoresRemoved == 1,
                "dead ABI store before zero-arg external was not removed") &&
         verifyOk(module,
                  "module failed verifier after zero-arg external rewrite");
}

bool testKnownFixedArgExternalTruncatesAbiInputs() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-fixed-arg-external", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "lseek", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "known_fixed_arg_call", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                "RDX");
  storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 4),
                "RCX");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::CallInst *call = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      if (candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "lseek") {
        call = candidate;
      }
    }
  }

  return expect(call != nullptr, "known fixed-arg external call missing") &&
         expect(call->arg_size() == 3,
                "known fixed-arg external used wrong arity") &&
         expect(summary.DeadStoresRemoved == 4,
                "dead ABI stores before fixed-arg external were not removed") &&
         verifyOk(module,
                  "module failed verifier after fixed-arg external rewrite");
}

bool testKnownFiveArgExternalUsesFiveInputs() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-five-arg-external", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
  llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");
  llvm::GlobalVariable *r9 = createRegisterGlobal(module, "R9");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "setsockopt", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "known_five_arg_call", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                "RDX");
  storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 4),
                "RCX");
  storeRegister(builder, r8, llvm::ConstantInt::get(r8->getValueType(), 5),
                "R8");
  storeRegister(builder, r9, llvm::ConstantInt::get(r9->getValueType(), 6),
                "R9");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::CallInst *call = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      if (candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "setsockopt") {
        call = candidate;
      }
    }
  }

  return expect(call != nullptr, "known five-arg external call missing") &&
         expect(call->arg_size() == 5,
                "known five-arg external used wrong arity") &&
         expect(summary.DeadStoresRemoved == 6,
                "dead ABI stores before five-arg external were not removed") &&
         verifyOk(module,
                  "module failed verifier after five-arg external rewrite");
}

bool testKnownFixedExternalArities() {
  struct KnownArityCase {
    const char *Name;
    unsigned Args;
  };
  const KnownArityCase cases[] = {
      {"__memmove_chk", 4},
      {"__open64_2", 2},
      {"__cxa_atexit", 3},
      {"__fgets_chk", 4},
      {"__getdelim", 4},
      {"__read_chk", 4},
      {"__register_atfork", 4},
      {"__sched_cpucount", 2},
      {"__isoc23_strtoll", 3},
      {"__isoc23_strtoul", 3},
      {"__isoc23_strtoull", 3},
      {"__poll_chk", 4},
      {"__longjmp_chk", 2},
      {"__sigsetjmp", 2},
      {"__strcpy_chk", 3},
      {"__sysconf", 1},
      {"__xpg_strerror_r", 3},
      {"ERR_error_string_n", 3},
      {"ERR_clear_error", 0},
      {"ERR_get_error", 0},
      {"ERR_peek_last_error", 0},
      {"ERR_print_errors_fp", 1},
      {"ERR_reason_error_string", 1},
      {"OPENSSL_init_crypto", 2},
      {"OPENSSL_init_ssl", 2},
      {"accept", 3},
      {"accept4", 4},
      {"arc4random", 0},
      {"cfmakeraw", 1},
      {"chmod", 2},
      {"chown", 3},
      {"closedir", 1},    {"closelog", 0},    {"fork", 0},
      {"clock_getres", 2},
      {"dlclose", 1},
      {"dlerror", 0},
      {"dlopen", 2},
      {"dup", 1},
      {"dup3", 3},
      {"endutxent", 0},
      {"epoll_create", 1},
      {"epoll_create1", 1},
      {"epoll_ctl", 4},
      {"epoll_pwait", 5},
      {"epoll_wait", 4},
      {"execv", 2},
      {"execvp", 2},
      {"eventfd", 2},
      {"event_base_free", 1},
      {"event_base_loop", 2},
      {"event_base_loopexit", 2},
      {"event_base_new_with_config", 1},
      {"event_config_free", 1},
      {"event_config_new", 0},
      {"event_config_set_flag", 2},
      {"event_get_version", 0},
      {"fdatasync", 1},
      {"freeifaddrs", 1},
      {"fgetc", 1},
      {"fileno", 1},
      {"fopen64", 2},
      {"fsync", 1},
      {"fstat", 2},
      {"fstatfs64", 2},
      {"ftruncate", 2},
      {"futimens", 2},
      {"gai_strerror", 1},
      {"getaddrinfo", 4},
      {"getegid", 0},
      {"getentropy", 2},
      {"getgid", 0},
      {"getgrgid", 1},
      {"getgrgid_r", 5},
      {"getgrnam", 1},
      {"gethostbyname", 1},
      {"gethostname", 2},
      {"getopt_long", 5},
      {"getifaddrs", 1},
      {"getloadavg", 2},
      {"getpagesize", 0},
      {"getpeername", 3},
      {"getpwuid", 1},
      {"getpwuid_r", 5},
      {"getrlimit", 2},
      {"getrlimit64", 2},
      {"getrusage", 2},
      {"getsockopt", 5},
      {"getsubopt", 3},
      {"getxattr", 4},
      {"getc", 1},
      {"gnu_get_libc_version", 0},
      {"glob64", 4},
      {"globfree64", 1},
      {"if_indextoname", 2},
      {"inet_ntoa", 1},
      {"inet_pton", 3},
      {"initgroups", 2},
      {"inotify_add_watch", 3},
      {"inotify_init1", 1},
      {"inotify_rm_watch", 2},
      {"isatty", 1},
      {"kill", 2},
      {"lchown", 3},
      {"link", 2},
      {"localtime", 1},
      {"localtime_r", 2},
      {"lstat", 2},
      {"lstat64", 2},
      {"madvise", 3},
      {"memchr", 3},
      {"mempcpy", 3},
      {"mkdir", 2},
      {"mkdtemp", 1},
      {"mkostemp64", 2},
      {"mkstemp64", 1},
      {"mlockall", 1},
      {"msync", 3},
      {"nettle_arcfour_crypt", 4},
      {"nettle_arcfour_set_key", 3},
      {"nettle_knuth_lfib_get", 1},
      {"nettle_knuth_lfib_init", 2},
      {"nettle_sha256_digest", 3},
      {"nettle_sha256_init", 1},
      {"nettle_sha256_update", 3},
      {"nettle_yarrow256_init", 3},
      {"pathconf", 2},
      {"pcre2_code_free_8", 1},
      {"pcre2_get_error_message_8", 3},
      {"pcre2_get_ovector_pointer_8", 1},
      {"pcre2_jit_compile_8", 2},
      {"pcre2_match_data_create_8", 2},
      {"pcre2_match_data_create_from_pattern_8", 2},
      {"pcre2_match_data_free_8", 1},
      {"pcre2_pattern_info_8", 3},
      {"pipe", 1},
      {"pipe2", 2},
      {"posix_memalign", 3},
      {"posix_spawn_file_actions_destroy", 1},
      {"posix_spawn_file_actions_addclosefrom_np", 2},
      {"posix_spawn_file_actions_adddup2", 3},
      {"posix_spawn_file_actions_addfchdir_np", 2},
      {"posix_spawn_file_actions_init", 1},
      {"posix_spawnattr_destroy", 1},
      {"posix_spawnattr_init", 1},
      {"posix_spawnattr_setflags", 2},
      {"posix_spawnattr_setsigdefault", 2},
      {"posix_spawnattr_setsigmask", 2},
      {"poll", 3},
      {"pread", 4},
      {"pread64", 4},
      {"preadv", 4},
      {"preadv64", 4},
      {"preadv64v2", 5},
      {"pthread_attr_destroy", 1},
      {"pthread_attr_init", 1},
      {"pthread_attr_setstacksize", 2},
      {"pthread_barrier_destroy", 1},
      {"pthread_barrier_init", 3},
      {"pthread_barrier_wait", 1},
      {"pthread_cond_broadcast", 1},
      {"pthread_cond_destroy", 1},
      {"pthread_cond_init", 2},
      {"pthread_cond_timedwait", 3},
      {"pthread_cond_wait", 2},
      {"pthread_condattr_destroy", 1},
      {"pthread_condattr_init", 1},
      {"pthread_condattr_setclock", 2},
      {"pthread_create", 4},
      {"pthread_getaffinity_np", 3},
      {"pthread_getschedparam", 3},
      {"pthread_getspecific", 1},
      {"pthread_mutex_destroy", 1},
      {"pthread_mutex_init", 2},
      {"pthread_mutex_trylock", 1},
      {"pthread_mutexattr_destroy", 1},
      {"pthread_mutexattr_init", 1},
      {"pthread_mutexattr_settype", 2},
      {"pthread_once", 2},
      {"pthread_rwlock_destroy", 1},
      {"pthread_rwlock_init", 2},
      {"pthread_rwlock_rdlock", 1},
      {"pthread_rwlock_tryrdlock", 1},
      {"pthread_rwlock_trywrlock", 1},
      {"pthread_rwlock_unlock", 1},
      {"pthread_rwlock_wrlock", 1},
      {"pthread_self", 0},
      {"pthread_setaffinity_np", 3},
      {"pthread_setschedparam", 3},
      {"pthread_setname_np", 2},
      {"putchar", 1},
      {"pututxline", 1},
      {"pwrite", 4},
      {"pwrite64", 4},
      {"pwritev64", 4},
      {"qsort", 4},
      {"readv", 3},
      {"recvmmsg", 5},
      {"recv", 4},
      {"sched_get_priority_max", 1},
      {"sched_get_priority_min", 1},
      {"sched_getaffinity", 3},
      {"sched_getcpu", 0},
      {"sched_yield", 0},
      {"sasl_dispose", 1},
      {"sasl_server_init", 2},
      {"sasl_server_step", 5},
      {"scandir64", 4},
      {"select", 5},
      {"sem_destroy", 1},
      {"sem_init", 3},
      {"sem_post", 1},
      {"sem_trywait", 1},
      {"sem_wait", 1},
      {"send", 4},
      {"sendfile", 4},
      {"sendfile64", 4},
      {"sendmmsg", 4},
      {"sendmsg", 3},
      {"SSL_accept", 1},
      {"SSL_clear", 1},
      {"SSL_connect", 1},
      {"SSL_CTX_check_private_key", 1},
      {"SSL_CTX_load_verify_locations", 3},
      {"SSL_CTX_new", 1},
      {"SSL_CTX_sess_set_new_cb", 2},
      {"SSL_CTX_set_cipher_list", 2},
      {"SSL_CTX_set_ciphersuites", 2},
      {"SSL_CTX_set_client_CA_list", 2},
      {"SSL_CTX_set_default_verify_paths", 1},
      {"SSL_CTX_set_options", 2},
      {"SSL_CTX_set_session_id_context", 3},
      {"SSL_CTX_set_verify", 3},
      {"SSL_CTX_set_verify_depth", 2},
      {"SSL_CTX_use_certificate_chain_file", 2},
      {"SSL_CTX_use_PrivateKey_file", 3},
      {"SSL_CTX_free", 1},
      {"setenv", 3},
      {"setgid", 1},
      {"setgroups", 2},
      {"setpriority", 3},
      {"setregid", 2},
      {"setreuid", 2},
      {"setbuf", 2},
      {"setrlimit", 2},
      {"setuid", 1},
      {"sigaddset", 2},
      {"sigemptyset", 1},
      {"socketpair", 4},
      {"SSL_free", 1},
      {"SSL_get_error", 2},
      {"SSL_load_client_CA_file", 1},
      {"SSL_new", 1},
      {"SSL_pending", 1},
      {"SSL_read", 3},
      {"SSL_set_connect_state", 1},
      {"SSL_set_fd", 2},
      {"SSL_set_info_callback", 2},
      {"SSL_shutdown", 1},
      {"SSL_write", 3},
      {"stat64", 2},
      {"statfs64", 2},
      {"strerror_r", 3},
      {"strndup", 2},
      {"strnlen", 2},
      {"strtoll", 3},
      {"strtoul", 3},
      {"strtoull", 3},
      {"strtok_r", 3},
      {"symlink", 2},
      {"sysconf", 1},
      {"tcgetattr", 2},
      {"tcsetattr", 3},
      {"timegm", 1},
      {"TLS_client_method", 0},
      {"TLS_server_method", 0},
      {"ttyname_r", 3},
      {"tzset", 0},
      {"umask", 1},
      {"unsetenv", 1},
      {"updwtmpx", 2},
      {"utimensat", 4},
      {"usleep", 1},
      {"wait", 1},
      {"writev", 3},
  };

  for (const KnownArityCase &testCase : cases) {
    llvm::LLVMContext context;
    llvm::Module module(std::string("summary-ssa-known-") + testCase.Name,
                        context);
    attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
    llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
    llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
    llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
    llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
    llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");
    llvm::GlobalVariable *r9 = createRegisterGlobal(module, "R9");

    auto *calleeType =
        llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
    llvm::Function *callee =
        llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                               testCase.Name, module);
    auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
    llvm::Function *function = llvm::Function::Create(
        type, llvm::GlobalValue::ExternalLinkage,
        std::string("known_call_") + testCase.Name, module);
    llvm::BasicBlock *entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::IRBuilder<> builder(entry);
    storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                  "RDI");
    storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                  "RSI");
    storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                  "RDX");
    storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 4),
                  "RCX");
    storeRegister(builder, r8, llvm::ConstantInt::get(r8->getValueType(), 5),
                  "R8");
    storeRegister(builder, r9, llvm::ConstantInt::get(r9->getValueType(), 6),
                  "R9");
    builder.CreateCall(calleeType, callee);
    builder.CreateRetVoid();

    notdec::bin2llvm::runNativeRegisterSummarySSA(module);
    llvm::CallInst *call = nullptr;
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      if (auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (candidate->getCalledFunction() != nullptr &&
            candidate->getCalledFunction()->getName() == testCase.Name) {
          call = candidate;
        }
      }
    }

    if (!expect(call != nullptr, "known fixed external call missing") ||
        !expect(call->arg_size() == testCase.Args,
                "known fixed external used wrong arity") ||
        !verifyOk(module,
                  "module failed verifier after fixed external rewrite")) {
      return false;
    }
  }
  return true;
}

bool testKnownVarArgExternalKeepsAbiInputs() {
  struct KnownVarArgCase {
    const char *Name;
    unsigned FixedArgs;
  };
  const KnownVarArgCase cases[] = {
      {"__isoc23_sscanf", 2},
      {"__isoc99_sscanf", 2},
      {"__snprintf_chk", 4},
      {"__syslog_chk", 2},
      {"fscanf", 2},
      {"prctl", 1},
  };

  for (const KnownVarArgCase &testCase : cases) {
    llvm::LLVMContext context;
    llvm::Module module(std::string("summary-ssa-known-vararg-") +
                            testCase.Name,
                        context);
    attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
    llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
    llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
    llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
    llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
    llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");
    llvm::GlobalVariable *r9 = createRegisterGlobal(module, "R9");

    auto *calleeType =
        llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
    llvm::Function *callee =
        llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                               testCase.Name, module);
    auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
    llvm::Function *function = llvm::Function::Create(
        type, llvm::GlobalValue::ExternalLinkage,
        std::string("known_vararg_call_") + testCase.Name, module);
    llvm::BasicBlock *entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::IRBuilder<> builder(entry);
    storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                  "RDI");
    storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                  "RSI");
    storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                  "RDX");
    storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 4),
                  "RCX");
    storeRegister(builder, r8, llvm::ConstantInt::get(r8->getValueType(), 5),
                  "R8");
    storeRegister(builder, r9, llvm::ConstantInt::get(r9->getValueType(), 6),
                  "R9");
    builder.CreateCall(calleeType, callee);
    builder.CreateRetVoid();

    auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
    llvm::CallInst *call = nullptr;
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      if (auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (candidate->getCalledFunction() != nullptr &&
            candidate->getCalledFunction()->getName() == testCase.Name) {
          call = candidate;
        }
      }
    }

    bool calleeIsVarArg = false;
    if (call != nullptr && call->getCalledFunction() != nullptr) {
      llvm::FunctionType *rewrittenType =
          call->getCalledFunction()->getFunctionType();
      calleeIsVarArg = rewrittenType->isVarArg() &&
                       rewrittenType->getNumParams() == testCase.FixedArgs;
    }

    if (!expect(call != nullptr, "known vararg external call missing") ||
        !expect(call->arg_size() == 6,
                "known vararg external dropped ABI varargs") ||
        !expect(calleeIsVarArg,
                "known vararg external did not keep vararg function type") ||
        !expect(summary.DeadStoresRemoved == 6,
                "dead ABI stores before vararg external were not removed") ||
        !verifyOk(module,
                  "module failed verifier after vararg external rewrite")) {
      return false;
    }
  }
  return true;
}

bool testMismatchedDirectCallUseUsesReturnExtract() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-direct-call-use-return-extract", context);

  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_multi_return_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";
  notdec::bin2llvm::NativeAbiParamEntry input;
  input.MinSize = 1;
  input.MaxSize = 8;
  input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  input.Storage.Name = "RDI";
  abi.Inputs.push_back(input);
  for (llvm::StringRef name : {"RAX", "RBX"}) {
    notdec::bin2llvm::NativeAbiParamEntry output;
    output.MinSize = 1;
    output.MaxSize = 8;
    output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    output.Storage.Name = name.str();
    abi.Outputs.push_back(output);

    notdec::bin2llvm::NativeAbiEffect killed;
    killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
    killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    killed.Storage.Name = name.str();
    abi.Effects.push_back(killed);
  }
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "strlen", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "uses_direct_call_result",
      module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  llvm::CallInst *call = builder.CreateCall(calleeType, callee);
  llvm::LoadInst *raxLoad = loadRegister(builder, rax, "RAX", "rax.after");
  llvm::LoadInst *rbxLoad = loadRegister(builder, rbx, "RBX", "rbx.after");
  llvm::Value *sum = builder.CreateAdd(call, raxLoad);
  sum = builder.CreateAdd(sum, rbxLoad);
  builder.CreateRet(sum);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  bool hasStructCall = false;
  bool hasExtractedDirectUse = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (candidate == nullptr || candidate->getCalledFunction() == nullptr ||
        candidate->getCalledFunction()->getName() != "strlen") {
      continue;
    }
    hasStructCall = candidate->getType()->isStructTy();
    for (llvm::User *user : candidate->users()) {
      auto *extract = llvm::dyn_cast<llvm::ExtractValueInst>(user);
      if (extract != nullptr && extract->getType()->isIntegerTy(64)) {
        hasExtractedDirectUse = true;
      }
    }
  }

  return expect(summary.CallsRewritten >= 1,
                "multi-return direct-use call was not rewritten") &&
         expect(hasStructCall, "multi-return call did not return struct") &&
         expect(hasExtractedDirectUse,
                "old direct call use was not replaced by extract") &&
         verifyOk(module,
                  "module failed verifier after direct call use rewrite");
}

bool testRecordedCallArgValueSurvivesDeadStoreCleanup() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-call-arg-value-survives-cleanup", context);
  attachTestAbiWithInputs(module, {"RDI"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "unlink", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                       {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "call_arg_cleanup", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *path = builder.CreateAdd(
      function->getArg(0), llvm::ConstantInt::get(rdi->getValueType(), 7),
      "path");
  llvm::Value *deadValue = builder.CreateXor(
      path, llvm::ConstantInt::get(rdi->getValueType(), 1), "dead.value");
  storeRegister(builder, rbx, deadValue, "RBX");
  storeRegister(builder, rdi, path, "RDI");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::CallInst *call = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      if (candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "unlink") {
        call = candidate;
      }
    }
  }

  bool argIsLocalInstruction = false;
  if (call != nullptr && call->arg_size() == 1) {
    auto *instruction =
        llvm::dyn_cast<llvm::Instruction>(call->getArgOperand(0));
    argIsLocalInstruction =
        instruction != nullptr && instruction->getFunction() == function;
  }

  return expect(call != nullptr, "known one-arg external call missing") &&
         expect(call->arg_size() == 1,
                "known one-arg external used wrong arity") &&
         expect(argIsLocalInstruction,
                "recorded call arg instruction was deleted before rewrite") &&
         expect(summary.DeadStoresRemoved >= 1,
                "dead ABI store before one-arg external was not removed") &&
         verifyOk(module,
                  "module failed verifier after call arg cleanup rewrite");
}

bool testInternalSignatureRewriteUsesArgsAndReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-internal-signature", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_callee", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(calleeEntry);
  llvm::LoadInst *input = loadRegister(builder, rdi, "RDI", "input");
  storeRegister(builder, rax, input, "RAX");
  builder.CreateRetVoid();

  llvm::Function *caller =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 7),
                "RDI");
  builder.CreateCall(voidType, callee);
  llvm::LoadInst *result = loadRegister(builder, rax, "RAX", "result");
  (void)result;
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten = module.getFunction("notdec_native_callee");
  llvm::Function *rewrittenCaller = module.getFunction("notdec_native_caller");
  bool callRewritten = false;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() == rewritten &&
          call->arg_size() == 1 && call->getType()->isIntegerTy(64)) {
        callRewritten = true;
      }
    }
  }

  return expect(rewritten != nullptr, "rewritten callee missing") &&
         expect(rewrittenCaller != nullptr, "rewritten caller missing") &&
         expect(rewritten->arg_size() == 1,
                "internal callee argument was not rewritten") &&
         expect(rewritten->getReturnType()->isIntegerTy(64),
                "internal callee return was not rewritten") &&
         expect(callRewritten, "internal callsite was not rewritten") &&
         expect(summary.FunctionsRewritten >= 1,
                "internal function rewrite was not counted") &&
         expect(summary.CallsRewritten >= 1,
                "internal call rewrite was not counted") &&
         verifyOk(module,
                  "module failed verifier after internal signature rewrite");
}

bool testInternalSignatureRewriteUsesNonAbiReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-internal-non-abi-return", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_non_abi_return", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(calleeEntry);
  storeRegister(builder, rbx, llvm::ConstantInt::get(rbx->getValueType(), 9),
                "RBX");
  builder.CreateRetVoid();

  llvm::Function *caller =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_non_abi_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  builder.CreateCall(voidType, callee);
  llvm::LoadInst *result = loadRegister(builder, rbx, "RBX", "result");
  (void)result;
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten = module.getFunction("notdec_native_non_abi_return");
  bool helperLeft = false;
  bool callReturnsRBX = false;
  for (llvm::Function &function : module) {
    if (function.getName().starts_with("notdec.register.summary_return")) {
      helperLeft = !function.use_empty();
    }
  }
  if (llvm::Function *rewrittenCaller =
          module.getFunction("notdec_native_non_abi_caller")) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() == rewritten &&
          call->getType() == rbx->getValueType()) {
        callReturnsRBX = true;
      }
    }
  }

  return expect(rewritten != nullptr, "non-ABI return callee missing") &&
         expect(rewritten->getReturnType() == rbx->getValueType(),
                "internal non-ABI return was not added to signature") &&
         expect(callReturnsRBX,
                "internal non-ABI return callsite was not rewritten") &&
         expect(!helperLeft, "non-ABI return helper was left in IR") &&
         expect(summary.CallsRewritten >= 1,
                "non-ABI return call rewrite was not counted") &&
         verifyOk(module,
                  "module failed verifier after non-ABI return rewrite");
}

bool testForeignArgumentInMovedBodyIsReplaced() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-foreign-argument", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                       {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *callee = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "notdec_native_foreign_arg",
      module);
  callee->getArg(0)->setName("R8.arg");
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rax, callee->getArg(0), "RAX");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewritten =
      module.getFunction("notdec_native_foreign_arg");
  bool hasForeignArgumentOperand = false;
  bool hasForeignInstructionOperand = false;
  if (rewritten != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewritten)) {
      for (llvm::Use &operand : inst.operands()) {
        auto *argument = llvm::dyn_cast<llvm::Argument>(operand.get());
        if (argument != nullptr && argument->getParent() != rewritten) {
          hasForeignArgumentOperand = true;
        }
        auto *instruction = llvm::dyn_cast<llvm::Instruction>(operand.get());
        if (instruction != nullptr && instruction->getFunction() != rewritten) {
          hasForeignInstructionOperand = true;
        }
      }
    }
  }

  return expect(rewritten != nullptr, "foreign-argument callee missing") &&
         expect(!hasForeignArgumentOperand,
                "moved body still referenced foreign argument") &&
         expect(!hasForeignInstructionOperand,
                "moved body still referenced foreign instruction") &&
         expect(summary.FunctionsRewritten >= 1,
                "foreign-argument function was not rewritten") &&
         verifyOk(module,
                  "module failed verifier after foreign argument rewrite");
}

bool testForeignMappedCallArgumentIsLocalized() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-foreign-call-arg", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_foreign_call_arg_callee", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(calleeEntry);
  llvm::LoadInst *foreignInput =
      loadRegister(builder, rdi, "RDI", "foreign.input");
  storeRegister(builder, rax, foreignInput, "RAX");
  builder.CreateRetVoid();

  llvm::Function *caller =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_foreign_call_arg_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  // This matches real bad IR seen after summary rewrite: the recorded ABI input
  // may chase through ValueMap into the rewritten callee's argument.
  storeRegister(builder, rdi, foreignInput, "RDI");
  builder.CreateCall(voidType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  llvm::Function *rewrittenCaller =
      module.getFunction("notdec_native_foreign_call_arg_caller");
  bool hasForeignCallOperand = false;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call == nullptr) {
        continue;
      }
      for (llvm::Value *arg : call->args()) {
        auto *argument = llvm::dyn_cast<llvm::Argument>(arg);
        if (argument != nullptr && argument->getParent() != rewrittenCaller) {
          hasForeignCallOperand = true;
        }
        auto *instruction = llvm::dyn_cast<llvm::Instruction>(arg);
        if (instruction != nullptr &&
            instruction->getFunction() != rewrittenCaller) {
          hasForeignCallOperand = true;
        }
      }
    }
  }

  return expect(rewrittenCaller != nullptr,
                "foreign-call-arg caller missing") &&
         expect(!hasForeignCallOperand,
                "rewritten call still referenced a foreign value") &&
         expect(summary.CallsRewritten >= 1,
                "foreign-call-arg call rewrite was not counted") &&
         verifyOk(module,
                  "module failed verifier after foreign call arg rewrite");
}

bool testStaticRspStackRewriteKeepsSavedRegisterEvidence() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-saved-register", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "stack_save_rbx", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *rspEntry = loadRegister(builder, rsp, "RSP", "rsp.entry");
  llvm::LoadInst *rbxEntry = loadRegister(builder, rbx, "RBX", "rbx.entry");
  llvm::Value *slotAddress = builder.CreateAdd(
      rspEntry, llvm::ConstantInt::get(rsp->getValueType(), -8, true));
  llvm::Value *slot =
      builder.CreateIntToPtr(slotAddress, llvm::PointerType::get(context, 0));
  builder.CreateStore(rbxEntry, slot);
  storeRegister(builder, rbx, llvm::ConstantInt::get(rbx->getValueType(), 42),
                "RBX");
  builder.CreateRetVoid();

  auto stackSummary = notdec::bin2llvm::runNativeStackFrameRewrite(module);
  notdec::bin2llvm::NativeRegisterSummaryOptions options;
  options.IgnoredRegisters = stackSummary.IgnoredRegisters;
  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module, options);
  const auto *fn = functionSummary(summary, "stack_save_rbx");
  const auto *rbxSummary = fn == nullptr ? nullptr : registerSummary(*fn, "RBX");

  return expect(stackSummary.AccessesRewritten >= 1,
                "RSP stack save was not localized") &&
         expect(stackSummary.IgnoredRegisters.count("RSP") != 0,
                "RSP was not marked ignored after stack rewrite") &&
         expect(hasNamedAlloca(*function, "notdec_stack.native"),
                "native stack alloca was not created") &&
         expect(rbxSummary != nullptr, "missing RBX summary after stack rewrite") &&
         expect(rbxSummary->MayEntry && !rbxSummary->MayNonEntry,
                "saved RBX was not preserved through native stack alloca") &&
         verifyOk(module, "module failed verifier after stack rewrite test");
}

bool testFramePointerLoadFeedsStackRewriteAndIgnoredSet() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-frame-pointer-stack", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  llvm::GlobalVariable *rbp = createRegisterGlobal(module, "RBP");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "rbp_frame_stack", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *rspEntry = loadRegister(builder, rsp, "RSP", "rsp.entry");
  llvm::Value *frameBase = builder.CreateAdd(
      rspEntry, llvm::ConstantInt::get(rsp->getValueType(), -32, true));
  storeRegister(builder, rbp, frameBase, "RBP");
  llvm::LoadInst *rbpLoad = loadRegister(builder, rbp, "RBP", "rbp.frame");
  llvm::Value *slotAddress = builder.CreateAdd(
      rbpLoad, llvm::ConstantInt::get(rbp->getValueType(), -8, true));
  llvm::Value *slot =
      builder.CreateIntToPtr(slotAddress, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(rbp->getValueType(), 7), slot);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeStackFrameRewrite(module);
  return expect(summary.FramePointerLoadsReplaced == 1,
                "RBP frame load was not replaced") &&
         expect(summary.AccessesRewritten >= 1,
                "RBP-derived stack access was not localized") &&
         expect(summary.IgnoredRegisters.count("RBP") != 0,
                "RBP was not marked ignored after frame-base match") &&
         expect(hasNamedAlloca(*function, "notdec_stack.native"),
                "frame-pointer stack alloca was not created") &&
         verifyOk(module, "module failed verifier after RBP stack rewrite test");
}

bool testSummarySSARemovesDeadStackFrameStore() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-dead-stack-frame-store", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "dead_stack_frame_store", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *rspEntry = loadRegister(builder, rsp, "RSP", "rsp.entry");
  llvm::Value *slotAddress = builder.CreateAdd(
      rspEntry, llvm::ConstantInt::get(rsp->getValueType(), -8, true));
  llvm::Value *slot =
      builder.CreateIntToPtr(slotAddress, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(rsp->getValueType(), 11), slot);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.StackFrameAllocaStoresRemoved == 1,
                "dead stack-frame alloca store was not removed") &&
         verifyOk(module,
                  "module failed verifier after stack-frame cleanup test");
}

bool testStackFrameAddressPassedToCallIsLocalized() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-frame-call-arg", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");

  auto *calleeType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)},
      false);
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "takes_stack_pointer", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "stack_pointer_call_arg", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *rspEntry = loadRegister(builder, rsp, "RSP", "rsp.entry");
  llvm::Value *frameBase = builder.CreateAdd(
      rspEntry, llvm::ConstantInt::get(rsp->getValueType(), -64, true));
  llvm::Value *buffer = builder.CreateAdd(
      frameBase, llvm::ConstantInt::get(rsp->getValueType(), 16, true));
  builder.CreateCall(callee, {buffer});
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeStackFrameRewrite(module);
  llvm::CallInst *call = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      call = candidate;
    }
  }

  return expect(summary.AccessesRewritten >= 2,
                "stack-derived call argument was not localized") &&
         expect(call != nullptr, "missing call after stack rewrite") &&
         expect(llvm::isa<llvm::PtrToIntInst>(call->getArgOperand(0)),
                "stack call argument was not rewritten through native alloca") &&
         verifyOk(module, "module failed verifier after stack call arg rewrite");
}

bool testPostSignatureCleanupDropsAbiStoreBeforeUnrewrittenCall() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-post-cleanup-abi-store", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *calleeType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)},
      false);
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "unrewritten_external", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "abi_store_before_unrewritten_call", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *callBlock =
      llvm::BasicBlock::Create(context, "call", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *arg = llvm::ConstantInt::get(rdi->getValueType(), 123);
  storeRegister(builder, rdi, arg, "RDI");
  builder.CreateBr(callBlock);
  builder.SetInsertPoint(callBlock);
  builder.CreateCall(callee, {arg});
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  unsigned rdiStores = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      if (store->getPointerOperand()->stripPointerCasts() == rdi) {
        ++rdiStores;
      }
    }
  }

  return expect(rdiStores == 0,
                "post-signature cleanup kept a dead ABI argument store") &&
         expect(summary.DeadStoresRemoved >= 1,
                "dead ABI argument store was not counted") &&
         verifyOk(module,
                  "module failed verifier after post-signature cleanup test");
}

bool testNoReturnExternalDoesNotCreateSummaryReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-noreturn-external", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *noreturnType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *stackChkFail = llvm::Function::Create(
      noreturnType, llvm::GlobalValue::ExternalLinkage, "__stack_chk_fail",
      module);

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "calls_noreturn", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(stackChkFail->getFunctionType(), stackChkFail, {});
  llvm::LoadInst *loaded =
      loadRegister(builder, rax, "RAX", "unreachable_rax");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  return expect(summary.CallReturnValues == 0,
                "noreturn external path created a summary return helper") &&
         verifyOk(module, "module failed verifier after noreturn cleanup test");
}

bool testXmmAbiEffectUsesZmmBackingWithoutSignatureReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-zmm-abi-effect", context);

  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_zmm_test";
  notdec::bin2llvm::NativeAbiParamEntry output;
  output.MetaType = "float";
  output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  output.Storage.Name = "XMM0_Qa";
  abi.Outputs.push_back(output);
  notdec::bin2llvm::NativeAbiEffect killed;
  killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
  killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  killed.Storage.Name = "XMM0";
  abi.Effects.push_back(killed);
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);

  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                             {}, false);
  llvm::Function *external =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "external_float_return", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "zmm_partial_after_call", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(external);
  llvm::Value *old = builder.CreateLoad(zmmType, zmm0);
  llvm::Value *keep = builder.CreateAnd(
      old, llvm::ConstantInt::get(
               zmmType, llvm::APInt::getBitsSet(512, 64, 512)));
  llvm::Value *low = llvm::ConstantInt::get(zmmType, 7);
  storeRegister(builder, zmm0, builder.CreateOr(keep, low), "ZMM0");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummarySSA(module);
  unsigned zmmStores = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    if (store != nullptr && store->getPointerOperand()->stripPointerCasts() ==
                                zmm0) {
      ++zmmStores;
    }
  }

  return expect(function->getReturnType()->isVoidTy(),
                "float ABI output was rewritten as an i512 return") &&
         expect(zmmStores == 0,
                "dead ZMM partial store after external call was not removed") &&
         expect(summary.CallReturnValues == 0,
                "float ABI output created a summary return helper") &&
         verifyOk(module, "module failed verifier after ZMM ABI effect test");
}

} // namespace

int main() {
  bool ok = true;
  ok &= testPhiIncomingMatchesPredecessors();
  ok &= testDuplicatePredecessorEdgesKeepPhiComplete();
  ok &= testUnknownPhiIncomingUsesFrozenPoison();
  ok &= testPreservedCallKeepsPreviousValue();
  ok &= testDemandedReturnCreatesCallValue();
  ok &= testIntrinsicDoesNotCreateCallValue();
  ok &= testOverwrittenStoreIsRemoved();
  ok &= testCrossBlockDeadStoreIsRemoved();
  ok &= testAbiInputStoreBeforeCallIsKept();
  ok &= testKnownZeroArgExternalDropsAbiInputs();
  ok &= testKnownFixedArgExternalTruncatesAbiInputs();
  ok &= testKnownFiveArgExternalUsesFiveInputs();
  ok &= testKnownFixedExternalArities();
  ok &= testKnownVarArgExternalKeepsAbiInputs();
  ok &= testMismatchedDirectCallUseUsesReturnExtract();
  ok &= testRecordedCallArgValueSurvivesDeadStoreCleanup();
  ok &= testInternalSignatureRewriteUsesArgsAndReturn();
  ok &= testInternalSignatureRewriteUsesNonAbiReturn();
  ok &= testForeignArgumentInMovedBodyIsReplaced();
  ok &= testForeignMappedCallArgumentIsLocalized();
  ok &= testStaticRspStackRewriteKeepsSavedRegisterEvidence();
  ok &= testFramePointerLoadFeedsStackRewriteAndIgnoredSet();
  ok &= testSummarySSARemovesDeadStackFrameStore();
  ok &= testStackFrameAddressPassedToCallIsLocalized();
  ok &= testPostSignatureCleanupDropsAbiStoreBeforeUnrewrittenCall();
  ok &= testNoReturnExternalDoesNotCreateSummaryReturn();
  ok &= testXmmAbiEffectUsesZmmBackingWithoutSignatureReturn();
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
