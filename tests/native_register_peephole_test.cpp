#include "RegisterTestHarness.h"

namespace {

bool testDeadFlagStoreBeforeCallIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-dead-flag-before-call", context);
  attachTestAbi(module);
  llvm::GlobalVariable *of = createRegisterGlobal(
      module, "OF", llvm::Type::getInt8Ty(context), 523, 1);

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "external_callee", module);

  llvm::Function *function =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "dead_flag_store_before_call", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, of, llvm::ConstantInt::get(of->getValueType(), 1),
                "OF");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  (void)summary;
  // 现在这条 store 在 summary 之前的 peephole 就判死删掉了（见
  // NativeRegisterPreSummaryPeephole.cpp 的 dead flag value 一段）；这里只断言最终没有留下
  // 这条死 store。
  return expect(countStoresTo(*function, of) == 0,
                "dead flag store before call was not removed") &&
         verifyOk(module, "module failed verifier after dead flag store test");
}

bool testPostRewriteInstCombineExposesDeadFlagStore() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-post-rewrite-instcombine", context);
  attachTestAbi(module);
  llvm::GlobalVariable *of = createRegisterGlobal(
      module, "OF", llvm::Type::getInt8Ty(context), 523, 1);

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "external_callee", module);
  auto *functionType =
      llvm::FunctionType::get(llvm::Type::getInt8Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage,
                             "post_instcombine_flag_store", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, of, llvm::ConstantInt::get(of->getValueType(), 1),
                "OF");
  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *loaded = loadRegister(builder, of, "OF", "of.after");
  llvm::Value *deadUse =
      builder.CreateSelect(llvm::ConstantInt::getFalse(context), loaded,
                           llvm::ConstantInt::get(of->getValueType(), 0));
  builder.CreateRet(deadUse);

  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  options.EnablePostRewriteInstCombine = true;
  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module, options);
  unsigned ofStores = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    if (store != nullptr &&
        store->getPointerOperand()->stripPointerCasts() == of) {
      ++ofStores;
    }
  }

  (void)summary;
  // 这条 store 现在在 summary 之前的 peephole 就被判死删掉了；只断言最终没留下。
  return expect(ofStores == 0, "dead OF store remained after cleanup loop") &&
         verifyOk(module,
                  "module failed verifier after post-rewrite cleanup test");
}

bool testPartialEntryMaskDoesNotCreateNarrowParam() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-entry-param", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  auto *i8Type = llvm::Type::getInt8Ty(context);
  auto *i32Type = llvm::Type::getInt32Ty(context);
  auto *i64Type = llvm::Type::getInt64Ty(context);
  llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");
  (void)createRegisterGlobal(module, "RDI");
  auto *sink = new llvm::GlobalVariable(
      module, i32Type, false, llvm::GlobalValue::ExternalLinkage, nullptr,
      "partial_entry_sink");
  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, r8->getType(), i8Type, 64, 8);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, r8->getType(), i32Type, 64, 32);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                       {llvm::Type::getInt1Ty(context)}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "partial_entry_param", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  // 先无条件写低字节，再读 32 位：入口值只剩 bit 8..32 可能还活着，
  // ReadEntryMask = 0xFFFFFF00（真正的"部分入口读"）。
  builder.CreateCall(partialWrite, {r8, llvm::ConstantInt::get(i8Type, 7),
                                    llvm::ConstantInt::get(i64Type, 0)});
  llvm::Value *value =
      builder.CreateCall(partialRead, {r8, llvm::ConstantInt::get(i64Type, 0)});
  builder.CreateStore(value, sink);
  builder.CreateRetVoid();

  (void)notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("partial_entry_param");
  bool hasR8Param = false;
  if (rewritten != nullptr) {
    for (const llvm::Argument &arg : rewritten->args()) {
      hasR8Param |= arg.getName().starts_with("R8");
    }
  }
  return expect(rewritten != nullptr, "partial entry param function missing") &&
         expect(!hasR8Param,
                "partial-entry R8 read created a narrow parameter") &&
         verifyOk(module,
                  "module failed verifier after partial entry param test");
}

bool testInternalSignatureRewriteUsesNarrowEntryRangeArg() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-narrow-entry-range-arg", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rdi->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *function =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_read_entry_rdi32", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *input = builder.CreateCall(
      partialRead,
      {rdi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "input32");
  builder.CreateStore(input, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten =
      module.getFunction("notdec_native_read_entry_rdi32");

  return expect(rewritten != nullptr, "narrow RDI entry callee missing") &&
         expect(rewritten->arg_size() == 1,
                "narrow RDI entry was not added as argument") &&
         expect(rewritten->getArg(0)->getType()->isIntegerTy(32),
                "narrow RDI entry argument was not i32") &&
         expect(!hasRegisterLoad(*rewritten, "RDI"),
                "narrow RDI entry left raw RDI load") &&
         expect(!hasPartialReadCall(*rewritten),
                "narrow RDI entry left partial read helper") &&
         verifyOk(
             module,
             "module failed verifier after narrow RDI entry argument test");
}

bool testNarrowEntryRangeDoesNotCreateWholeEntryLoad() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-narrow-entry-range-no-full-load", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rdi->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *function =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_entry_rdi32_no_full_load", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *input = builder.CreateCall(
      partialRead,
      {rdi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "input32");
  builder.CreateStore(input, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module, options);
  // runNativeRegisterElimination 会重写签名替换函数对象，旧指针会悬空，
  // 必须按名字重新取。
  llvm::Function *rewritten =
      module.getFunction("notdec_native_entry_rdi32_no_full_load");

  return expect(rewritten != nullptr,
                "narrow entry range callee missing after rewrite") &&
         expect(summary.RangeEntryInputs == 1,
                "narrow entry range did not create one segment entry input") &&
         expect(rewritten == nullptr || !hasRegisterLoad(*rewritten, "RDI"),
                "narrow entry range created whole RDI entry load") &&
         expect(rewritten == nullptr || !hasPartialReadCall(*rewritten),
                "narrow entry range left partial read helper") &&
         verifyOk(module,
                  "module failed verifier after narrow entry range input test");
}

bool testBranchPartialReadUsesNarrowRangePhi() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-read-range-phi", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *condition = new llvm::GlobalVariable(
      module, llvm::Type::getInt1Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "condition");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "branch_partial_read_range_phi", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right =
      llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *merge =
      llvm::BasicBlock::Create(context, "merge", function);

  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);

  llvm::IRBuilder<> builder(entry);
  llvm::Value *cond =
      builder.CreateLoad(llvm::Type::getInt1Ty(context), condition, "cond");
  builder.CreateCondBr(cond, left, right);

  builder.SetInsertPoint(left);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 7),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateBr(merge);

  builder.SetInsertPoint(right);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 11),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateBr(merge);

  builder.SetInsertPoint(merge);
  llvm::Value *after = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "after_partial");
  builder.CreateStore(after, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  // runNativeRegisterElimination 会重写签名替换函数对象，旧指针会悬空，
  // 必须按名字重新取。
  llvm::Function *rewritten =
      module.getFunction("branch_partial_read_range_phi");
  unsigned i64Phis = 0;
  if (rewritten != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewritten)) {
      auto *phi = llvm::dyn_cast<llvm::PHINode>(&inst);
      if (phi == nullptr) {
        continue;
      }
      if (phi->getType()->isIntegerTy(64)) {
        ++i64Phis;
      }
    }
  }

  return expect(summary.LoadsReplaced >= 1,
                "branch partial read was not replaced") &&
         // 原读（offset 0）必须被折进 range phi；没被写过的位保留成入口裸读。
         expect(rewritten == nullptr ||
                    countPartialReadCalls(*rewritten, rax, 0, 32) == 0,
                "branch partial read helper call remained") &&
         expect(summary.PhisCreated >= 1,
                "branch partial read did not create a range phi") &&
         expect(i64Phis == 0,
                "branch partial read created a whole-register i64 phi") &&
         verifyOk(module, "module failed verifier after branch range phi test");
}

bool testBranchFullLoadAfterPartialWriteUsesNarrowRangePhi() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-full-load-range-phi", context);
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_no_return_test";
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *condition = new llvm::GlobalVariable(
      module, llvm::Type::getInt1Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "condition");
  auto *sink = new llvm::GlobalVariable(module, rax->getValueType(), false,
                                        llvm::GlobalValue::ExternalLinkage,
                                        nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "branch_full_load_range_phi", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right =
      llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *merge =
      llvm::BasicBlock::Create(context, "merge", function);

  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);

  llvm::IRBuilder<> builder(entry);
  llvm::Value *cond =
      builder.CreateLoad(llvm::Type::getInt1Ty(context), condition, "cond");
  builder.CreateCondBr(cond, left, right);

  builder.SetInsertPoint(left);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 7),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateBr(merge);

  builder.SetInsertPoint(right);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 11),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateBr(merge);

  builder.SetInsertPoint(merge);
  llvm::LoadInst *after = loadRegister(builder, rax, "RAX", "after_partial");
  builder.CreateStore(after, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  unsigned i64Phis = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *phi = llvm::dyn_cast<llvm::PHINode>(&inst);
    if (phi == nullptr) {
      continue;
    }
    if (phi->getType()->isIntegerTy(64)) {
      ++i64Phis;
    }
  }

  return expect(summary.LoadsReplaced >= 1,
                "branch full load was not replaced") &&
         expect(!hasRegisterLoad(*function, "RAX"),
                "branch full load left raw RAX load") &&
         expect(!hasPartialWriteCall(*function),
                "branch full load left partial write helper") &&
         expect(functionHasInstructionNameContaining(*function, "full_range"),
                "branch full load did not assemble from ranges") &&
         expect(i64Phis == 0,
                "branch full load created a whole-register i64 phi") &&
         verifyOk(
             module,
             "module failed verifier after branch full-load range phi test");
}

bool testLoopPartialWriteUsesNarrowRangePhi() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-loop-partial-write-range-phi", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *condition = new llvm::GlobalVariable(
      module, llvm::Type::getInt1Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "condition");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "loop_partial_write_range_phi", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *header =
      llvm::BasicBlock::Create(context, "header", function);
  llvm::BasicBlock *body = llvm::BasicBlock::Create(context, "body", function);
  llvm::BasicBlock *exit = llvm::BasicBlock::Create(context, "exit", function);

  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateBr(header);

  builder.SetInsertPoint(header);
  llvm::Value *cond =
      builder.CreateLoad(llvm::Type::getInt1Ty(context), condition, "cond");
  builder.CreateCondBr(cond, body, exit);

  builder.SetInsertPoint(body);
  llvm::Value *current = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "current");
  llvm::Value *next = builder.CreateAdd(
      current, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 1),
      "next");
  builder.CreateCall(
      partialWrite,
      {rax, next, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateBr(header);

  builder.SetInsertPoint(exit);
  llvm::Value *after = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "after");
  builder.CreateStore(after, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  function = module.getFunction("loop_partial_write_range_phi");
  if (function == nullptr) {
    return expect(false, "loop partial write function missing after rewrite");
  }
  unsigned i32Phis = 0;
  unsigned i64Phis = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *phi = llvm::dyn_cast<llvm::PHINode>(&inst);
    if (phi == nullptr) {
      continue;
    }
    if (phi->getType()->isIntegerTy(32)) {
      ++i32Phis;
    }
    if (phi->getType()->isIntegerTy(64)) {
      ++i64Phis;
    }
  }

  return expect(summary.LoadsReplaced >= 2,
                "loop partial reads were not replaced") &&
         // 原读（offset 0）必须被折进 range phi；没被写过的位保留成入口裸读。
         expect(countPartialReadCalls(*function, rax, 0, 32) == 0,
                "loop partial read helper call remained") &&
         expect(!hasPartialWriteCall(*function),
                "loop partial write helper call remained") &&
         expect(i32Phis >= 1, "loop partial write did not create i32 phi") &&
         expect(i64Phis == 0,
                "loop partial write created a whole-register i64 phi") &&
         verifyOk(module, "module failed verifier after loop range phi test");
}

bool testLowBitPeepholeRewritesFullLoadTrunc() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-low-bit-peephole-trunc", context);
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "low_bit_trunc", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *full = loadRegister(builder, rsi, "RSI", "full_rsi");
  llvm::Value *low =
      builder.CreateTrunc(full, llvm::Type::getInt32Ty(context), "low_rsi");
  builder.CreateStore(low, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterPreSummaryPeephole(module);

  return expect(summary.Rewrites == 1,
                "low-bit peephole did not rewrite full load trunc") &&
         expect(summary.DirectTruncRewrites == 1,
                "low-bit peephole did not count direct trunc rewrite") &&
         expect(!hasRegisterLoad(*function, "RSI"),
                "low-bit peephole left full RSI load") &&
         expect(countPartialReadCalls(*function, rsi, 0, 32) == 1,
                "low-bit peephole did not create low32 partial read") &&
         verifyOk(module, "module failed verifier after low-bit trunc test");
}

bool testLowBitPeepholeRewritesShiftTrunc() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-low-bit-peephole-shift-trunc", context);
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "low_bit_shift_trunc", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *full = loadRegister(builder, rsi, "RSI", "full_rsi");
  llvm::Value *shifted = builder.CreateLShr(
      full, llvm::ConstantInt::get(rsi->getValueType(), 32), "high_shift");
  llvm::Value *high =
      builder.CreateTrunc(shifted, llvm::Type::getInt32Ty(context), "high_rsi");
  builder.CreateStore(high, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterPreSummaryPeephole(module);

  return expect(summary.Rewrites == 1,
                "low-bit peephole did not rewrite shift trunc") &&
         expect(summary.ShiftTruncRewrites == 1,
                "low-bit peephole did not count shift trunc rewrite") &&
         expect(!hasRegisterLoad(*function, "RSI"),
                "low-bit peephole left shifted full RSI load") &&
         expect(countPartialReadCalls(*function, rsi, 32, 32) == 1,
                "low-bit peephole did not create high32 partial read") &&
         verifyOk(module,
                  "module failed verifier after low-bit shift trunc test");
}

bool testLowBitPeepholeSkipsMultiUseFullLoad() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-low-bit-peephole-multi-use", context);
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  auto *narrowSink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "narrow_sink");
  auto *fullSink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "full_sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "low_bit_multi_use", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *full = loadRegister(builder, rsi, "RSI", "full_rsi");
  llvm::Value *low =
      builder.CreateTrunc(full, llvm::Type::getInt32Ty(context), "low_rsi");
  builder.CreateStore(low, narrowSink);
  builder.CreateStore(full, fullSink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterPreSummaryPeephole(module);

  return expect(summary.Rewrites == 0,
                "low-bit peephole rewrote a multi-use full load") &&
         expect(summary.MultiUseLoadsSkipped == 1,
                "low-bit peephole did not count multi-use skip") &&
         expect(hasRegisterLoad(*function, "RSI"),
                "low-bit peephole removed full load with live full use") &&
         expect(!hasPartialReadCall(*function),
                "low-bit peephole created partial read for multi-use load") &&
         verifyOk(module,
                  "module failed verifier after low-bit multi-use test");
}

bool testByteIndexPeepholeNarrowsByteOnlyWideRead() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-byte-index-narrow", context);
  llvm::Function *function =
      buildByteIndexFunction(module, "byte_index_narrow", ByteIndexShape::ByteOnly);
  llvm::GlobalVariable *source = module.getGlobalVariable("RSI");
  auto summary = notdec::bin2llvm::runNativeRegisterPreSummaryPeephole(module);
  return expect(summary.ByteIndexCandidates >= 1,
                "byte index peephole did not see the index write") &&
         expect(summary.ByteIndexRewrites == 1,
                "byte index peephole did not narrow the wide read") &&
         expect(countPartialReadCalls(*function, source, 0, 32) == 0,
                "byte index peephole left the 32-bit source read") &&
         expect(countPartialReadCalls(*function, source, 0, 8) == 1,
                "byte index peephole did not create the 8-bit source read") &&
         verifyOk(module, "module failed verifier after byte index narrowing");
}

bool testByteIndexPeepholeSkipsWideReadInRegion() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-byte-index-wide-read", context);
  llvm::Function *function = buildByteIndexFunction(
      module, "byte_index_wide_read", ByteIndexShape::WideReadInRegion);
  llvm::GlobalVariable *source = module.getGlobalVariable("RSI");
  auto summary = notdec::bin2llvm::runNativeRegisterPreSummaryPeephole(module);
  return expect(summary.ByteIndexRewrites == 0,
                "byte index peephole narrowed across a 32-bit read") &&
         expect(countPartialReadCalls(*function, source, 0, 32) == 1,
                "byte index peephole changed the source read on skip") &&
         verifyOk(module, "module failed verifier after byte index skip");
}

bool testByteIndexPeepholeSkipsRealCallInRegion() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-byte-index-call", context);
  llvm::Function *function = buildByteIndexFunction(
      module, "byte_index_call", ByteIndexShape::RealCallInRegion);
  llvm::GlobalVariable *source = module.getGlobalVariable("RSI");
  auto summary = notdec::bin2llvm::runNativeRegisterPreSummaryPeephole(module);
  return expect(summary.ByteIndexRewrites == 0,
                "byte index peephole narrowed across a real call") &&
         expect(countPartialReadCalls(*function, source, 0, 32) == 1,
                "byte index peephole changed the source read on skip") &&
         verifyOk(module, "module failed verifier after byte index skip");
}

bool testByteIndexPeepholeSkipsWithoutByteRead() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-byte-index-no-read", context);
  llvm::Function *function = buildByteIndexFunction(
      module, "byte_index_no_read", ByteIndexShape::NoByteRead);
  llvm::GlobalVariable *source = module.getGlobalVariable("RSI");
  auto summary = notdec::bin2llvm::runNativeRegisterPreSummaryPeephole(module);
  return expect(summary.ByteIndexRewrites == 0,
                "byte index peephole matched without any byte read") &&
         expect(countPartialReadCalls(*function, source, 0, 32) == 1,
                "byte index peephole changed the source read on skip") &&
         verifyOk(module, "module failed verifier after byte index skip");
}

bool testByteIndexPeepholeSkipsMultiUseWideRead() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-byte-index-multi-use", context);
  llvm::Function *function = buildByteIndexFunction(
      module, "byte_index_multi_use", ByteIndexShape::WideMultipleUses);
  llvm::GlobalVariable *source = module.getGlobalVariable("RSI");
  auto summary = notdec::bin2llvm::runNativeRegisterPreSummaryPeephole(module);
  return expect(summary.ByteIndexRewrites == 0,
                "byte index peephole narrowed a multi-use wide read") &&
         expect(countPartialReadCalls(*function, source, 0, 32) == 1,
                "byte index peephole changed the source read on skip") &&
         verifyOk(module, "module failed verifier after byte index skip");
}

bool testWidthNarrowNarrowsRegisterMove() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-width-narrow-move-byte", context);
  llvm::Function *function = buildWidthNarrowFunction(
      module, "width_narrow_move_byte", WidthNarrowShape::MoveByte);
  llvm::GlobalVariable *source = module.getGlobalVariable("RSI");
  auto summary = notdec::bin2llvm::runNativeRegisterPreSummaryPeephole(module);
  return expect(summary.ByteIndexRewrites == 1,
                "register move was not narrowed to the observed byte") &&
         expect(countPartialReadCalls(*function, source, 0, 32) == 0,
                "wide read of the moved register remained") &&
         expect(countPartialReadCalls(*function, source, 0, 8) == 1,
                "narrow byte read of the moved register missing") &&
         verifyOk(module, "module failed verifier after register move narrowing");
}

bool testWidthNarrowNarrowsToHalfWordObservation() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-width-narrow-half", context);
  llvm::Function *function = buildWidthNarrowFunction(
      module, "width_narrow_move_half", WidthNarrowShape::MoveHalf);
  llvm::GlobalVariable *source = module.getGlobalVariable("RSI");
  auto summary = notdec::bin2llvm::runNativeRegisterPreSummaryPeephole(module);
  return expect(summary.ByteIndexRewrites == 1,
                "half word observation did not narrow the source read") &&
         expect(countPartialReadCalls(*function, source, 0, 32) == 0,
                "wide read remained after half word narrowing") &&
         expect(countPartialReadCalls(*function, source, 0, 16) == 1,
                "narrow half word read missing") &&
         verifyOk(module, "module failed verifier after half word narrowing");
}

bool testWidthNarrowHandlesConstantShift() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-width-narrow-shift", context);
  llvm::Function *function = buildWidthNarrowFunction(
      module, "width_narrow_shift_right", WidthNarrowShape::ShiftRight);
  llvm::GlobalVariable *source = module.getGlobalVariable("RSI");
  auto summary = notdec::bin2llvm::runNativeRegisterPreSummaryPeephole(module);
  return expect(summary.ByteIndexRewrites == 1,
                "constant right shift did not narrow the source read") &&
         expect(countPartialReadCalls(*function, source, 0, 16) == 1,
                "right shift narrowing produced the wrong read width") &&
         verifyOk(module, "module failed verifier after shift narrowing");
}

bool testWidthNarrowSkipsDivision() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-width-narrow-division", context);
  llvm::Function *function = buildWidthNarrowFunction(
      module, "width_narrow_division", WidthNarrowShape::Division);
  llvm::GlobalVariable *source = module.getGlobalVariable("RSI");
  auto summary = notdec::bin2llvm::runNativeRegisterPreSummaryPeephole(module);
  return expect(summary.ByteIndexRewrites == 0,
                "division chain was narrowed although it is not low bit closed") &&
         expect(countPartialReadCalls(*function, source, 0, 32) == 1,
                "wide source read disappeared on a division chain") &&
         verifyOk(module, "module failed verifier after division skip");
}

bool testDeadFlagStorePeepholeRemovesDeadChain() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-dead-flag-store", context);
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  auto *zf = new llvm::GlobalVariable(
      module, llvm::Type::getInt8Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "ZF");
  llvm::Function *read32 = notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
      module, rsi->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "dead_flag_store", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *value = builder.CreateCall(
      read32, {rsi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "value");
  llvm::Value *isZero = builder.CreateICmpEQ(
      value, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
  builder.CreateStore(builder.CreateZExt(isZero, llvm::Type::getInt8Ty(context)),
                      zf);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterPreSummaryPeephole(module);
  return expect(summary.DeadFlagStoresRemoved == 1,
                "dead flag store was not removed") &&
         expect(summary.DeadFlagValuesRemoved >= 4,
                "dead flag chain was not erased") &&
         expect(countPartialReadCalls(*function, rsi, 0, 32) == 0,
                "dead flag chain kept the register read") &&
         verifyOk(module, "module failed verifier after dead flag elimination");
}

bool testDeadFlagStorePeepholeKeepsReadFlag() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-live-flag-store", context);
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  auto *zf = new llvm::GlobalVariable(
      module, llvm::Type::getInt8Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "ZF");
  llvm::Function *read32 = notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
      module, rsi->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "live_flag_store", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *taken = llvm::BasicBlock::Create(context, "taken", function);
  llvm::BasicBlock *other = llvm::BasicBlock::Create(context, "other", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *value = builder.CreateCall(
      read32, {rsi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "value");
  llvm::Value *isZero = builder.CreateICmpEQ(
      value, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
  builder.CreateStore(builder.CreateZExt(isZero, llvm::Type::getInt8Ty(context)),
                      zf);
  llvm::Value *flag = builder.CreateLoad(llvm::Type::getInt8Ty(context), zf);
  llvm::Value *flagIsZero = builder.CreateICmpEQ(
      flag, llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 0));
  builder.CreateCondBr(flagIsZero, taken, other);
  builder.SetInsertPoint(taken);
  builder.CreateRetVoid();
  builder.SetInsertPoint(other);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterPreSummaryPeephole(module);
  return expect(summary.DeadFlagStoresRemoved == 0,
                "live flag store was removed") &&
         expect(countPartialReadCalls(*function, rsi, 0, 32) == 1,
                "live flag chain was erased") &&
         verifyOk(module, "module failed verifier after live flag check");
}

bool testPostRewritePeepholeSimplifiesExtractFromInsertedRange() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-post-rewrite-peephole-inserted-extract",
                      context);
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");
  auto *type = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt32Ty(context)}, false);
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "final_cleanup_inserted_extract", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Argument *lowArg = function->getArg(0);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rbx->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *insert =
      notdec::bin2llvm::getOrInsertNativeRegisterValueInsert(
          module, llvm::Type::getInt64Ty(context),
          llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Value *high = builder.CreateCall(
      partialRead,
      {rbx, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 32)});
  llvm::Value *full = builder.CreateCall(
      insert,
      {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0), lowArg,
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  full = builder.CreateCall(
      insert, {full, high,
               llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 32)});
  llvm::Value *lowAgain =
      builder.CreateTrunc(full, llvm::Type::getInt32Ty(context), "low_again");
  builder.CreateStore(lowAgain, sink);
  builder.CreateRetVoid();

  auto peephole =
      notdec::bin2llvm::runNativeRegisterPostRewritePeephole(module);
  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  bool storeUsesArg = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    storeUsesArg |= store != nullptr && store->getPointerOperand() == sink &&
                    store->getValueOperand() == lowArg;
  }

  return expect(peephole.ValueRangeExtractsSimplified == 1,
                "inserted range extract was not simplified") &&
         expect(storeUsesArg,
                "low range did not rewrite to the inserted value") &&
         expect(
             cleanup.DeadRegisterReadsRemoved == 1,
             "dead high partial read remained after extract simplification") &&
         expect(module.getGlobalVariable("RBX") == nullptr,
                "dead RBX global remained after inserted extract cleanup") &&
         expect(cleanup.RemainingRegisterAccesses == 0,
                "inserted extract cleanup left register residue") &&
         verifyOk(module,
                  "module failed verifier after inserted extract cleanup test");
}

bool testConstantDivisionMagicFold() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-constant-division-magic", context);
  const MagicDivisionCase cases[] = {
      {3, 0xAAAAAAAAAAAAAAABULL, 1},
      {5, 0xCCCCCCCCCCCCCCCDULL, 2},
      {6, 0xAAAAAAAAAAAAAAABULL, 2},
      {9, 0xE38E38E38E38E38FULL, 3},
      {10, 0xCCCCCCCCCCCCCCCDULL, 3},
      {72, 0xE38E38E38E38E38FULL, 6},
      // 除数没有上界，4097 也要认出来。
      {4097, 0xFFF000FFF000FFF1ULL, 12},
      // x/1000000000：clang 先 shrq $9 再对 1953125 用魔数，被除数范围收到
      // 55 位才成立（L 不收紧的话这个魔数验证不过）。
      {1953125, 19342813113834067ULL, 11, 9},
  };
  bool built = true;
  for (const MagicDivisionCase &testCase : cases) {
    std::string suffix = std::to_string(testCase.ExpectedDivisor);
    built &= buildMagicDivisionFunction(module, "div_" + suffix, testCase,
                                        /*foldCorrectionShift=*/false) !=
             nullptr;
    built &= buildMagicDivisionFunction(module, "folded_div_" + suffix,
                                        testCase,
                                        /*foldCorrectionShift=*/true) !=
             nullptr;
  }

  auto peephole =
      notdec::bin2llvm::runNativeRegisterPostRewritePeephole(module);

  bool allFolded = built;
  for (const MagicDivisionCase &testCase : cases) {
    std::string suffix = std::to_string(testCase.ExpectedDivisor);
    for (const std::string &name :
         {"div_" + suffix, "folded_div_" + suffix}) {
      llvm::Function *function = module.getFunction(name);
      allFolded &=
          expect(function != nullptr, "magic division function is missing") &&
          expect(functionHasUdivBy(*function, testCase.ExpectedDivisor),
                 "magic multiply chain was not folded back to udiv") &&
          expect(!functionHasMul(*function),
                 "magic multiply chain survived the udiv fold");
    }
  }

  return expect(peephole.ConstantDivisionsRecognized == 2 * std::size(cases),
                "unexpected number of recognized constant divisions") &&
         allFolded &&
         verifyOk(module, "module failed verifier after magic division fold");
}

bool testConstantDivisionUnknownMagicIsLeftAlone() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-constant-division-unknown-magic", context);
  // 不对应任何除数的魔数（既不是 ceil(2^K/d) 也过不了误差条件）：必须整体跳过，
  // 不能猜一个邻近的除数。
  const MagicDivisionCase testCase = {0, 1234567890123456789ULL, 3};
  llvm::Function *function =
      buildMagicDivisionFunction(module, "not_a_division", testCase,
                                 /*foldCorrectionShift=*/false);

  auto peephole =
      notdec::bin2llvm::runNativeRegisterPostRewritePeephole(module);

  bool sawUdiv = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    sawUdiv |= inst.getOpcode() == llvm::Instruction::UDiv;
  }

  return expect(peephole.ConstantDivisionsRecognized == 0,
                "non-division magic must not be recognized") &&
         expect(functionHasMul(*function),
                "non-division magic lost its multiply") &&
         expect(!sawUdiv, "non-division magic was folded to a udiv") &&
         verifyOk(module, "module failed verifier after unknown magic skip");
}

bool testConstantDivisionMagicFoldI386() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-constant-division-magic-i386", context);
  const MagicDivisionCase cases[] = {
      {3, 0xAAAAAAABULL, 1},
      {5, 0xCCCCCCCDULL, 2},
      {6, 0xAAAAAAABULL, 2},
      {9, 0x38E38E39ULL, 1},
      {10, 0xCCCCCCCDULL, 3},
      {100, 0x51EB851FULL, 5},
      {1000, 0x10624DD3ULL, 6},
      // x/10^9：clang -m32 先 shrl $9 再对 1953125 用 M = 0x44B83、s = 7。
      {1953125, 0x44B83ULL, 7, 9},
  };
  bool built = true;
  for (const MagicDivisionCase &testCase : cases) {
    std::string suffix = std::to_string(testCase.ExpectedDivisor);
    built &= buildMagicDivisionFunction(module, "i386_div_" + suffix, testCase,
                                        /*foldCorrectionShift=*/false,
                                        32) != nullptr;
    built &= buildMagicDivisionFunction(module, "i386_folded_div_" + suffix,
                                        testCase,
                                        /*foldCorrectionShift=*/true, 32) !=
             nullptr;
  }

  auto peephole =
      notdec::bin2llvm::runNativeRegisterPostRewritePeephole(module);

  bool allFolded = built;
  for (const MagicDivisionCase &testCase : cases) {
    std::string suffix = std::to_string(testCase.ExpectedDivisor);
    for (const std::string &name :
         {"i386_div_" + suffix, "i386_folded_div_" + suffix}) {
      llvm::Function *function = module.getFunction(name);
      allFolded &=
          expect(function != nullptr, "i386 magic division function is missing") &&
          expect(functionHasUdivBy(*function, testCase.ExpectedDivisor),
                 "i386 magic multiply chain was not folded back to udiv") &&
          expect(!functionHasMul(*function),
                 "i386 magic multiply chain survived the udiv fold");
    }
  }

  return expect(peephole.ConstantDivisionsRecognized == 2 * std::size(cases),
                "unexpected number of recognized i386 constant divisions") &&
         allFolded &&
         verifyOk(module,
                  "module failed verifier after i386 magic division fold");
}

bool testSignedConstantDivisionMagicFoldI386() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-signed-division-i386", context);
  const MagicDivisionCase cases[] = {
      {3, 1431655766ULL, 0},
      {10, 1717986919ULL, 2},
      {13, 1321528399ULL, 2},
      {100, 1374389535ULL, 5},
      {1000, 274877907ULL, 6},
  };
  for (const MagicDivisionCase &testCase : cases) {
    buildSignedMagicDivisionFunction(module,
                                     "i386_sdiv_" +
                                         std::to_string(testCase.ExpectedDivisor),
                                     testCase, 32);
  }

  auto peephole =
      notdec::bin2llvm::runNativeRegisterPostRewritePeephole(module);

  bool allFolded = true;
  for (const MagicDivisionCase &testCase : cases) {
    llvm::Function *function = module.getFunction(
        "i386_sdiv_" + std::to_string(testCase.ExpectedDivisor));
    allFolded &=
        expect(function != nullptr, "i386 signed division function is missing") &&
        expect(functionHasSdivBy(*function, testCase.ExpectedDivisor),
               "i386 signed magic chain was not folded back to sdiv") &&
        expect(!functionHasMul(*function),
               "i386 signed magic chain survived the sdiv fold");
  }

  return expect(peephole.SignedConstantDivisionsRecognized == std::size(cases),
                "unexpected number of recognized i386 signed divisions") &&
         expect(peephole.ConstantDivisionsRecognized == 0,
                "i386 signed fold counted as unsigned") &&
         allFolded &&
         verifyOk(module, "module failed verifier after i386 signed fold");
}

bool testSignedConstantDivisionMagicFoldX86_64() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-signed-division-x86-64", context);
  // clang -m64 -O2 -S 的 `int64_t x/d`：imulq + sarq + 加符号位。
  const MagicDivisionCase cases[] = {
      {3, 0x5555555555555556ULL, 0},
      {5, 0x6666666666666667ULL, 1},
      {7, 0x4924924924924925ULL, 1},
      {10, 0x6666666666666667ULL, 2},
      {13, 0x4EC4EC4EC4EC4EC5ULL, 2},
  };
  for (const MagicDivisionCase &testCase : cases) {
    buildSignedMagicDivisionFunction(
        module, "x64_sdiv_" + std::to_string(testCase.ExpectedDivisor),
        testCase, 64);
  }

  auto peephole =
      notdec::bin2llvm::runNativeRegisterPostRewritePeephole(module);

  bool allFolded = true;
  for (const MagicDivisionCase &testCase : cases) {
    llvm::Function *function = module.getFunction(
        "x64_sdiv_" + std::to_string(testCase.ExpectedDivisor));
    allFolded &=
        expect(function != nullptr, "x64 signed division function is missing") &&
        expect(functionHasSdivBy(*function, testCase.ExpectedDivisor),
               "x64 signed magic chain was not folded back to sdiv") &&
        expect(!functionHasMul(*function),
               "x64 signed magic chain survived the sdiv fold");
  }

  return expect(peephole.SignedConstantDivisionsRecognized == std::size(cases),
                "unexpected number of recognized x64 signed divisions") &&
         expect(peephole.ConstantDivisionsRecognized == 0,
                "x64 signed fold counted as unsigned") &&
         allFolded &&
         verifyOk(module, "module failed verifier after x64 signed fold");
}

bool testNarrowProductMagicFold16Bit() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-narrow-product-16", context);
  llvm::Type *i16 = llvm::Type::getInt16Ty(context);
  llvm::Type *i32 = llvm::Type::getInt32Ty(context);

  auto build = [&](llvm::StringRef name, bool isSigned, uint64_t magic,
                   unsigned shift) {
    auto *type = llvm::FunctionType::get(i32, {i16}, false);
    llvm::Function *function = llvm::Function::Create(
        type, llvm::GlobalValue::ExternalLinkage, name, module);
    llvm::BasicBlock *entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::IRBuilder<> builder(entry);
    llvm::Value *wide = isSigned
                            ? builder.CreateSExt(function->getArg(0), i32)
                            : builder.CreateZExt(function->getArg(0), i32);
    llvm::Value *product =
        builder.CreateMul(wide, llvm::ConstantInt::get(i32, magic));
    llvm::Value *shifted =
        builder.CreateLShr(product, llvm::ConstantInt::get(i32, shift));
    if (!isSigned) {
      builder.CreateRet(shifted);
      return function;
    }
    llvm::Value *sign =
        builder.CreateLShr(product, llvm::ConstantInt::get(i32, 31));
    builder.CreateRet(builder.CreateAdd(shifted, sign, "quotient"));
    return function;
  };
  build("u16_div3", /*isSigned=*/false, 43691, 17);
  build("s16_div3", /*isSigned=*/true, 21846, 16);

  auto peephole =
      notdec::bin2llvm::runNativeRegisterPostRewritePeephole(module);

  llvm::Function *unsignedFn = module.getFunction("u16_div3");
  llvm::Function *signedFn = module.getFunction("s16_div3");
  return expect(peephole.ConstantDivisionsRecognized == 1,
                "narrow-product unsigned fold was not counted") &&
         expect(peephole.SignedConstantDivisionsRecognized == 1,
                "narrow-product signed fold was not counted") &&
         expect(unsignedFn != nullptr && functionHasUdivBy(*unsignedFn, 3),
                "16-bit unsigned division was not folded to udiv 3") &&
         expect(signedFn != nullptr && functionHasSdivBy(*signedFn, 3),
                "16-bit signed division was not folded to sdiv 3") &&
         expect(!functionHasMul(*unsignedFn) && !functionHasMul(*signedFn),
                "narrow-product chain survived the fold") &&
         verifyOk(module, "module failed verifier after narrow-product fold");
}

bool testNarrowSourceWideningFold() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-narrow-source", context);
  llvm::Type *i32 = llvm::Type::getInt32Ty(context);
  llvm::Type *i64 = llvm::Type::getInt64Ty(context);
  llvm::Type *i128 = llvm::Type::getInt128Ty(context);
  auto *type = llvm::FunctionType::get(i64, {i32}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "narrow_source_div1000", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *narrow = function->getArg(0);
  llvm::Value *dividend = builder.CreateSExt(narrow, i64, "dividend");
  llvm::Value *wide = builder.CreateSExt(narrow, i128);
  llvm::Value *product = builder.CreateNSWMul(
      wide, llvm::ConstantInt::get(i128, 2361183241434822607ULL));
  llvm::Value *high =
      builder.CreateLShr(product, llvm::ConstantInt::get(i128, 64));
  llvm::Value *truncated = builder.CreateTrunc(high, i64);
  llvm::Value *shifted =
      builder.CreateAShr(truncated, llvm::ConstantInt::get(i64, 7));
  llvm::Value *sign =
      builder.CreateLShr(dividend, llvm::ConstantInt::get(i64, 63));
  builder.CreateRet(builder.CreateAdd(shifted, sign, "quotient"));

  auto peephole =
      notdec::bin2llvm::runNativeRegisterPostRewritePeephole(module);

  return expect(peephole.SignedConstantDivisionsRecognized == 1,
                "narrow-source signed division was not recognized") &&
         expect(functionHasSdivBy(*function, 1000),
                "narrow-source signed division did not fold to sdiv 1000") &&
         expect(!functionHasMul(*function),
                "narrow-source chain survived the fold") &&
         verifyOk(module, "module failed verifier after narrow-source fold");
}

} // namespace

int main() {
  bool ok = true;
  ok &= testDeadFlagStoreBeforeCallIsRemoved();
  ok &= testPostRewriteInstCombineExposesDeadFlagStore();
  ok &= testPartialEntryMaskDoesNotCreateNarrowParam();
  ok &= testInternalSignatureRewriteUsesNarrowEntryRangeArg();
  ok &= testNarrowEntryRangeDoesNotCreateWholeEntryLoad();
  ok &= testBranchPartialReadUsesNarrowRangePhi();
  ok &= testBranchFullLoadAfterPartialWriteUsesNarrowRangePhi();
  ok &= testLoopPartialWriteUsesNarrowRangePhi();
  ok &= testLowBitPeepholeRewritesFullLoadTrunc();
  ok &= testLowBitPeepholeRewritesShiftTrunc();
  ok &= testLowBitPeepholeSkipsMultiUseFullLoad();
  ok &= testByteIndexPeepholeNarrowsByteOnlyWideRead();
  ok &= testByteIndexPeepholeSkipsWideReadInRegion();
  ok &= testByteIndexPeepholeSkipsRealCallInRegion();
  ok &= testByteIndexPeepholeSkipsWithoutByteRead();
  ok &= testByteIndexPeepholeSkipsMultiUseWideRead();
  ok &= testWidthNarrowNarrowsRegisterMove();
  ok &= testWidthNarrowNarrowsToHalfWordObservation();
  ok &= testWidthNarrowHandlesConstantShift();
  ok &= testWidthNarrowSkipsDivision();
  ok &= testDeadFlagStorePeepholeRemovesDeadChain();
  ok &= testDeadFlagStorePeepholeKeepsReadFlag();
  ok &= testPostRewritePeepholeSimplifiesExtractFromInsertedRange();
  ok &= testConstantDivisionMagicFold();
  ok &= testConstantDivisionUnknownMagicIsLeftAlone();
  ok &= testConstantDivisionMagicFoldI386();
  ok &= testSignedConstantDivisionMagicFoldI386();
  ok &= testSignedConstantDivisionMagicFoldX86_64();
  ok &= testNarrowProductMagicFold16Bit();
  ok &= testNarrowSourceWideningFold();
  return ok ? 0 : 1;
}
