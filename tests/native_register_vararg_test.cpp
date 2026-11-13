#include "RegisterTestHarness.h"

namespace {

bool testI386NativeFrameOutgoingVarArgPrefixIsBound() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-native-frame-vararg-prefix", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i8 = llvm::Type::getInt8Ty(context);
  auto *i32 = llvm::Type::getInt32Ty(context);
  (void)createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *calleeType = llvm::FunctionType::get(i32, {}, true);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "i386_native_frame_vararg", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  auto *stackType = llvm::ArrayType::get(i8, 60);
  llvm::AllocaInst *stack =
      builder.CreateAlloca(stackType, nullptr, "notdec_stack.native");
  llvm::Value *basePointer = builder.CreateInBoundsGEP(
      i8, stack, llvm::ConstantInt::get(i32, 12), "native.arg.base");
  llvm::Value *baseInteger =
      builder.CreatePtrToInt(basePointer, i32, "native.arg.base.int");
  for (auto [delta, value] :
       {std::pair<int, int>{-4, 33}, std::pair<int, int>{-8, 1},
        std::pair<int, int>{-12, 22}}) {
    llvm::Value *argAddress = builder.CreateAdd(
        baseInteger, llvm::ConstantInt::get(i32, delta), "native.arg.addr");
    llvm::Value *argPointer =
        builder.CreateIntToPtr(argAddress, llvm::PointerType::get(context, 0));
    builder.CreateStore(llvm::ConstantInt::get(i32, value), argPointer);
  }
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenFunction =
      module.getFunction("i386_native_frame_vararg");
  if (!expect(rewrittenFunction != nullptr,
              "i386 native frame vararg caller missing")) {
    return false;
  }
  llvm::CallInst *rewritten = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName() == "__fprintf_chk") {
      rewritten = call;
    }
  }
  auto constantArg = [&](unsigned index) -> llvm::ConstantInt * {
    return rewritten == nullptr || rewritten->arg_size() <= index
               ? nullptr
               : llvm::dyn_cast<llvm::ConstantInt>(
                     rewritten->getArgOperand(index));
  };
  llvm::ConstantInt *arg0 = constantArg(0);
  llvm::ConstantInt *arg1 = constantArg(1);
  llvm::ConstantInt *arg2 = constantArg(2);
  return expect(rewritten != nullptr,
                "i386 native frame vararg call missing") &&
         expect(rewritten->arg_size() >= 3,
                "i386 native frame vararg fixed prefix was not rewritten") &&
         expect(arg0 != nullptr && arg0->getZExtValue() == 22,
                "i386 native frame vararg first stack arg was not bound") &&
         expect(arg1 != nullptr && arg1->getZExtValue() == 1,
                "i386 native frame vararg second stack arg was not bound") &&
         expect(arg2 != nullptr && arg2->getZExtValue() == 33,
                "i386 native frame vararg third stack arg was not bound") &&
         expect(summary.CallArgStoresMarked == 3,
                "i386 native frame vararg stores were not marked") &&
         verifyOk(module,
                  "module failed verifier after native frame vararg rewrite");
}

bool testI386NativeFrameOutgoingFloatVarArgIsWidened() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-native-frame-float-vararg", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i8 = llvm::Type::getInt8Ty(context);
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *doubleTy = llvm::Type::getDoubleTy(context);
  llvm::GlobalVariable *espGlobal =
      createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *calleeType = llvm::FunctionType::get(i32, {}, true);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "i386_native_frame_float_vararg", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  // The outgoing area is 16 bytes and sits at the bottom of a 48-byte
  // frame; mirror the real caller by keeping ESP 48 bytes below entry so the
  // cspec entry-SP slots line up with the alloca offsets.
  auto *stackType = llvm::ArrayType::get(i8, 48);
  llvm::AllocaInst *stack =
      builder.CreateAlloca(stackType, nullptr, "notdec_stack.native");
  llvm::Value *entryEsp = builder.CreateLoad(i32, espGlobal, "ESP.entry");
  llvm::Value *frameEsp = builder.CreateSub(
      entryEsp, llvm::ConstantInt::get(i32, 48), "ESP.frame");
  builder.CreateStore(frameEsp, espGlobal);
  llvm::Value *basePointer = builder.CreateInBoundsGEP(
      i8, stack, llvm::ConstantInt::get(i32, 12), "native.arg.base");
  llvm::Value *baseInteger =
      builder.CreatePtrToInt(basePointer, i32, "native.arg.base.int");
  auto stackPointer = [&](int delta) {
    llvm::Value *address = builder.CreateAdd(
        baseInteger, llvm::ConstantInt::get(i32, delta), "native.arg.addr");
    return builder.CreateIntToPtr(address, llvm::PointerType::get(context, 0));
  };
  // The double occupies the highest vararg grid slot (8 bytes in a 4-byte
  // grid); the three fixed i32 arguments sit below it.
  builder.CreateStore(llvm::ConstantFP::get(doubleTy, 3.14), stackPointer(0));
  for (auto [delta, value] :
       {std::pair<int, int>{-4, 33}, std::pair<int, int>{-8, 1},
        std::pair<int, int>{-12, 22}}) {
    builder.CreateStore(llvm::ConstantInt::get(i32, value), stackPointer(delta));
  }
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenFunction =
      module.getFunction("i386_native_frame_float_vararg");
  if (!expect(rewrittenFunction != nullptr,
              "i386 native frame float vararg caller missing")) {
    return false;
  }
  llvm::CallInst *rewritten = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName() == "__fprintf_chk") {
      rewritten = call;
    }
  }
  if (!expect(rewritten != nullptr,
              "i386 native frame float vararg call missing")) {
    return false;
  }
  auto constantArg = [&](unsigned index) -> llvm::ConstantInt * {
    return rewritten->arg_size() <= index
               ? nullptr
               : llvm::dyn_cast<llvm::ConstantInt>(
                     rewritten->getArgOperand(index));
  };
  llvm::ConstantInt *arg0 = constantArg(0);
  llvm::ConstantInt *arg1 = constantArg(1);
  llvm::ConstantInt *arg2 = constantArg(2);
  llvm::ConstantFP *arg3 =
      rewritten->arg_size() <= 3
          ? nullptr
          : llvm::dyn_cast<llvm::ConstantFP>(rewritten->getArgOperand(3));
  bool doubleOk = arg3 != nullptr &&
                  arg3->getValueAPF().convertToDouble() == 3.14;
  return expect(rewritten->arg_size() >= 4,
                "i386 native frame float vararg tail was not bound") &&
         expect(arg0 != nullptr && arg0->getZExtValue() == 22,
                "i386 native frame float vararg first stack arg was not bound") &&
         expect(arg1 != nullptr && arg1->getZExtValue() == 1,
                "i386 native frame float vararg second stack arg was not bound") &&
         expect(arg2 != nullptr && arg2->getZExtValue() == 33,
                "i386 native frame float vararg third stack arg was not bound") &&
         expect(doubleOk,
                "i386 native frame float vararg double was not bound") &&
         expect(summary.CallArgStoresMarked == 4,
                "i386 native frame float vararg stores were not marked") &&
         verifyOk(module,
                  "module failed verifier after native frame float vararg rewrite");
}

bool testI386StackPointerSummaryOutgoingVarArgPrefixIsBound() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-esp-summary-vararg-prefix", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i32 = llvm::Type::getInt32Ty(context);
  (void)createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *calleeType = llvm::FunctionType::get(i32, {}, true);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "i386_esp_summary_vararg", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *callBlock =
      llvm::BasicBlock::Create(context, "call", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateBr(callBlock);

  builder.SetInsertPoint(callBlock);
  llvm::PHINode *esp = builder.CreatePHI(i32, 1, "ESP.range_summary_ssa.test");
  esp->addIncoming(llvm::ConstantInt::get(i32, 0x1000), entry);
  esp->setMetadata("notdec.register.summary_ssa.phi",
                   summaryPhiMetadata(context, "ESP"));
  for (auto [delta, value] :
       {std::pair<int, int>{-4, 33}, std::pair<int, int>{-8, 1},
        std::pair<int, int>{-12, 22}}) {
    llvm::Value *argAddress = builder.CreateAdd(
        esp, llvm::ConstantInt::get(i32, delta), "esp.summary.arg.addr");
    llvm::Value *argPointer =
        builder.CreateIntToPtr(argAddress, llvm::PointerType::get(context, 0));
    builder.CreateStore(llvm::ConstantInt::get(i32, value), argPointer);
  }
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenFunction =
      module.getFunction("i386_esp_summary_vararg");
  if (!expect(rewrittenFunction != nullptr,
              "i386 ESP summary vararg caller missing")) {
    return false;
  }
  llvm::CallInst *rewritten = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName() == "__fprintf_chk") {
      rewritten = call;
    }
  }
  auto constantArg = [&](unsigned index) -> llvm::ConstantInt * {
    return rewritten == nullptr || rewritten->arg_size() <= index
               ? nullptr
               : llvm::dyn_cast<llvm::ConstantInt>(
                     rewritten->getArgOperand(index));
  };
  llvm::ConstantInt *arg0 = constantArg(0);
  llvm::ConstantInt *arg1 = constantArg(1);
  llvm::ConstantInt *arg2 = constantArg(2);
  return expect(rewritten != nullptr, "i386 ESP summary vararg call missing") &&
         expect(rewritten->arg_size() >= 3,
                "i386 ESP summary vararg prefix was not rewritten") &&
         expect(arg0 != nullptr && arg0->getZExtValue() == 22,
                "i386 ESP summary first stack arg was not bound") &&
         expect(arg1 != nullptr && arg1->getZExtValue() == 1,
                "i386 ESP summary second stack arg was not bound") &&
         expect(arg2 != nullptr && arg2->getZExtValue() == 33,
                "i386 ESP summary third stack arg was not bound") &&
         expect(summary.CallArgStoresMarked == 3,
                "i386 ESP summary vararg stores were not marked") &&
         verifyOk(module,
                  "module failed verifier after ESP summary vararg rewrite");
}

bool testKnownVarArgExternalInfersDefinedAbiInputs() {
  struct KnownVarArgCase {
    const char *Name;
    unsigned FixedArgs;
    unsigned ExpectedCallArgs = 6;
  };
  const KnownVarArgCase cases[] = {
      {"__isoc23_sscanf", 2}, {"__isoc99_sscanf", 2}, {"__asprintf_chk", 3},
      {"__snprintf_chk", 5},  {"__syslog_chk", 2},    {"fscanf", 2},
      {"prctl", 1, 3},
  };

  for (const KnownVarArgCase &testCase : cases) {
    llvm::LLVMContext context;
    llvm::Module module(
        std::string("summary-ssa-known-vararg-") + testCase.Name, context);
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
        calleeType, llvm::GlobalValue::ExternalLinkage, testCase.Name, module);
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

    auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
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
        !expect(call->arg_size() == testCase.ExpectedCallArgs,
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

bool testKnownVarArgFixedOnlyCallDoesNotReadTail() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-vararg-fixed-only", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  (void)createRegisterGlobal(module, "RCX");
  (void)createRegisterGlobal(module, "R8");
  (void)createRegisterGlobal(module, "R9");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "vararg_fixed_only_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                "RDX");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenCaller =
      module.getFunction("vararg_fixed_only_caller");
  llvm::CallInst *rewrittenCall = nullptr;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenCaller)) {
      auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (candidate != nullptr && candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "__fprintf_chk") {
        rewrittenCall = candidate;
      }
    }
  }

  return expect(rewrittenCaller != nullptr,
                "known vararg fixed-only caller missing") &&
         expect(
             rewrittenCaller->arg_empty(),
             "known vararg fixed-only call leaked a tail register argument") &&
         expect(rewrittenCall != nullptr,
                "known vararg fixed-only call missing") &&
         expect(rewrittenCall->arg_size() == 3,
                "known vararg fixed-only call used wrong arity") &&
         verifyOk(module,
                  "module failed verifier after fixed-only vararg rewrite");
}

bool testKnownVarArgCallsitesKeepIndependentArities() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-vararg-callsite-arities", context);
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
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "vararg_two_calls", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                "RDX");
  builder.CreateCall(calleeType, callee);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 4),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 5),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 6),
                "RDX");
  storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 7),
                "RCX");
  storeRegister(builder, r8, llvm::ConstantInt::get(r8->getValueType(), 8),
                "R8");
  storeRegister(builder, r9, llvm::ConstantInt::get(r9->getValueType(), 9),
                "R9");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenCaller = module.getFunction("vararg_two_calls");
  std::vector<unsigned> arities;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() != nullptr &&
          call->getCalledFunction()->getName() == "__fprintf_chk") {
        arities.push_back(call->arg_size());
      }
    }
  }

  return expect(arities.size() == 2,
                "known vararg callsite test lost a call") &&
         expect(arities[0] == 3,
                "known vararg fixed-only callsite used wrong arity") &&
         expect(arities[1] == 6,
                "known vararg extended callsite used wrong arity") &&
         verifyOk(module,
                  "module failed verifier after per-callsite vararg rewrite");
}

bool testKnownVarArgDoesNotReuseStaleCallerSavedTail() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-vararg-stale-tail", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
  llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "vararg_stale_tail", module);
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
  builder.CreateCall(calleeType, callee);

  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 6),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 7),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 8),
                "RDX");
  storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 9),
                "RCX");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenCaller = module.getFunction("vararg_stale_tail");
  std::vector<unsigned> arities;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() != nullptr &&
          call->getCalledFunction()->getName() == "__fprintf_chk") {
        arities.push_back(call->arg_size());
      }
    }
  }

  return expect(arities.size() == 2, "stale-tail test lost a call") &&
         expect(arities[0] == 5, "first vararg call used wrong arity") &&
         expect(arities[1] == 4,
                "second vararg call reused stale R8 as an argument") &&
         verifyOk(module, "module failed verifier after stale-tail rewrite");
}

bool testKnownVarArgUsesSseCountForFloatTail() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-vararg-float-tail", context);
  attachTestIntegerAndFloatVarArgAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
  llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "vararg_float_tail_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                "RDX");
  storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 0),
                "RCX");
  storeRegister(builder, r8, llvm::ConstantInt::get(r8->getValueType(), 0),
                "R8");
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 1),
                "RAX");
  llvm::StoreInst *zmmStore = storeRegister(
      builder, zmm0,
      llvm::ConstantInt::get(zmmType, llvm::APInt(512, 0x3ff0000000000000ULL)),
      "ZMM0");
  // 真实 lifted IR 会给 SSE 浮点写打 notdec.register.float_write 标记；
  // 只有带这个标记的 XMM 槽才算浮点参数证据（movq/punpcklqdq 整数打包不算）。
  zmmStore->setMetadata(
      "notdec.register.float_write",
      llvm::MDNode::get(context, llvm::MDString::get(context, "1")));
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenCaller =
      module.getFunction("vararg_float_tail_caller");
  llvm::CallInst *rewrittenCall = nullptr;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenCaller)) {
      auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (candidate != nullptr && candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "__fprintf_chk") {
        rewrittenCall = candidate;
      }
    }
  }

  return expect(rewrittenCall != nullptr, "known vararg float call missing") &&
         expect(rewrittenCall->arg_size() == 4,
                "known vararg float call used wrong arity") &&
         expect(rewrittenCall->getArgOperand(3)->getType()->isDoubleTy(),
                "known vararg float tail was not rewritten to double") &&
         verifyOk(module,
                  "module failed verifier after known vararg float rewrite");
}

bool testKnownVarArgZeroTailKeepsConstantZero() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-vararg-zero-tail-constant", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "vararg_zero_tail_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                "RDX");
  storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 0),
                "RCX");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);
  llvm::Function *rewrittenCaller =
      module.getFunction("vararg_zero_tail_caller");
  llvm::CallInst *rewrittenCall = nullptr;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenCaller)) {
      auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (candidate != nullptr && candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "__fprintf_chk") {
        rewrittenCall = candidate;
      }
    }
  }
  return expect(rewrittenCall != nullptr, "known vararg zero call missing") &&
         expect(rewrittenCall->arg_size() == 4,
                "known vararg zero call used wrong arity") &&
         expect(
             llvm::isa<llvm::ConstantInt>(rewrittenCall->getArgOperand(3)) &&
                 llvm::cast<llvm::ConstantInt>(rewrittenCall->getArgOperand(3))
                     ->isZero(),
             "known vararg zero tail was not kept as constant zero") &&
         expect(cleanup.VarArgUnknownHelpersLowered == 0,
                "constant zero vararg used an unknown helper") &&
         verifyOk(module,
                  "module failed verifier after known vararg zero rewrite");
}

bool testKnownVarArgUnknownPhiTailUsesPoison() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-vararg-unknown-phi-tail-poison",
                      context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "vararg_unknown_phi_tail_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *unknownBlock =
      llvm::BasicBlock::Create(context, "unknown", function);
  llvm::BasicBlock *zeroBlock =
      llvm::BasicBlock::Create(context, "zero", function);
  llvm::BasicBlock *merge =
      llvm::BasicBlock::Create(context, "merge", function);

  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 3),
                "RDX");
  llvm::Value *cond = builder.CreateFreeze(
      llvm::PoisonValue::get(llvm::Type::getInt1Ty(context)), "unknown_cond");
  builder.CreateCondBr(cond, unknownBlock, zeroBlock);

  builder.SetInsertPoint(unknownBlock);
  auto *tail32Type = llvm::Type::getInt32Ty(context);
  llvm::Value *unknown =
      builder.CreateFreeze(llvm::PoisonValue::get(tail32Type), "tail_unknown");
  builder.CreateBr(merge);

  builder.SetInsertPoint(zeroBlock);
  builder.CreateBr(merge);

  builder.SetInsertPoint(merge);
  llvm::PHINode *tail = builder.CreatePHI(tail32Type, 2, "tail");
  tail->addIncoming(unknown, unknownBlock);
  tail->addIncoming(llvm::ConstantInt::get(tail32Type, 0), zeroBlock);
  llvm::Function *insert =
      notdec::bin2llvm::getOrInsertNativeRegisterValueInsert(
          module, rcx->getValueType(), tail32Type, 64, 32);
  llvm::Value *tail64 = builder.CreateCall(
      insert,
      {llvm::ConstantInt::get(rcx->getValueType(), 0), tail,
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "tail_insert");
  storeRegister(builder, rcx, tail64, "RCX");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);
  llvm::Function *rewrittenCaller =
      module.getFunction("vararg_unknown_phi_tail_caller");
  llvm::CallInst *rewrittenCall = nullptr;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenCaller)) {
      auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (candidate != nullptr && candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "__fprintf_chk") {
        rewrittenCall = candidate;
      }
    }
  }

  return expect(rewrittenCall != nullptr,
                "known vararg unknown-phi call missing") &&
         expect(rewrittenCall->arg_size() == 4,
                "known vararg unknown-phi call used wrong arity") &&
         expect(llvm::isa<llvm::PoisonValue>(rewrittenCall->getArgOperand(3)),
                "known vararg unknown-phi tail was not rewritten to poison") &&
         expect(cleanup.VarArgUnknownHelpersLowered == 1,
                "unknown-phi vararg helper was not lowered") &&
         verifyOk(module,
                  "module failed verifier after unknown-phi vararg rewrite");
}

bool testNonX64VarArgUnknownPhiTailDoesNotUsePoisonHelper() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-non-x64-vararg-unknown-tail", context);
  attachTestAbiWithInputs(module, {"A0", "A1", "A2", "A3"});
  llvm::GlobalVariable *a0 = createRegisterGlobal(module, "A0");
  llvm::GlobalVariable *a1 = createRegisterGlobal(module, "A1");
  llvm::GlobalVariable *a2 = createRegisterGlobal(module, "A2");
  llvm::GlobalVariable *a3 = createRegisterGlobal(module, "A3");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "__fprintf_chk", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "non_x64_vararg_unknown_tail_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *unknownBlock =
      llvm::BasicBlock::Create(context, "unknown", function);
  llvm::BasicBlock *zeroBlock =
      llvm::BasicBlock::Create(context, "zero", function);
  llvm::BasicBlock *merge =
      llvm::BasicBlock::Create(context, "merge", function);

  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, a0, llvm::ConstantInt::get(a0->getValueType(), 1),
                "A0");
  storeRegister(builder, a1, llvm::ConstantInt::get(a1->getValueType(), 2),
                "A1");
  storeRegister(builder, a2, llvm::ConstantInt::get(a2->getValueType(), 3),
                "A2");
  llvm::Value *cond = builder.CreateFreeze(
      llvm::PoisonValue::get(llvm::Type::getInt1Ty(context)), "unknown_cond");
  builder.CreateCondBr(cond, unknownBlock, zeroBlock);

  builder.SetInsertPoint(unknownBlock);
  auto *tail32Type = llvm::Type::getInt32Ty(context);
  llvm::Value *unknown =
      builder.CreateFreeze(llvm::PoisonValue::get(tail32Type), "tail_unknown");
  builder.CreateBr(merge);

  builder.SetInsertPoint(zeroBlock);
  builder.CreateBr(merge);

  builder.SetInsertPoint(merge);
  llvm::PHINode *tail = builder.CreatePHI(tail32Type, 2, "tail");
  tail->addIncoming(unknown, unknownBlock);
  tail->addIncoming(llvm::ConstantInt::get(tail32Type, 0), zeroBlock);
  llvm::Function *insert =
      notdec::bin2llvm::getOrInsertNativeRegisterValueInsert(
          module, a3->getValueType(), tail32Type, 64, 32);
  llvm::Value *tail64 = builder.CreateCall(
      insert,
      {llvm::ConstantInt::get(a3->getValueType(), 0), tail,
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "tail_insert");
  storeRegister(builder, a3, tail64, "A3");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);
  llvm::Function *rewrittenCaller =
      module.getFunction("non_x64_vararg_unknown_tail_caller");
  llvm::CallInst *rewrittenCall = nullptr;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenCaller)) {
      auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (candidate != nullptr && candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "__fprintf_chk") {
        rewrittenCall = candidate;
      }
    }
  }

  return expect(rewrittenCall != nullptr,
                "non-x64 vararg unknown call missing") &&
         expect(rewrittenCall->arg_size() == 4,
                "non-x64 vararg unknown call used wrong arity") &&
         expect(cleanup.VarArgUnknownHelpersLowered == 0,
                "non-x64 vararg used an x64-only unknown helper") &&
         expect(!moduleHasFunctionNamed(module,
                                        "notdec.register.vararg_unknown.i64"),
                "non-x64 vararg unknown helper declaration remained") &&
         verifyOk(module,
                  "module failed verifier after non-x64 vararg rewrite");
}

bool testUnknownExternalArityUsesVarArgTail() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unknown-external-max-arity", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX"});

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external_arity", module);
  // 条件用函数参数：常量真分支会被后面的 SimplifyCFG 折掉，长调用点就看不到了。
  auto *callerType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt1Ty(context)}, false);
  llvm::Function *function =
      llvm::Function::Create(callerType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external_arity_callers", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *shortCall =
      llvm::BasicBlock::Create(context, "short_call", function);
  llvm::BasicBlock *longCall =
      llvm::BasicBlock::Create(context, "long_call", function);
  llvm::BasicBlock *done = llvm::BasicBlock::Create(context, "done", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(function->getArg(0), shortCall, longCall);

  builder.SetInsertPoint(shortCall);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  builder.CreateCall(calleeType, callee);
  builder.CreateBr(done);

  builder.SetInsertPoint(longCall);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 2),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 3),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 4),
                "RDX");
  builder.CreateCall(calleeType, callee);
  builder.CreateBr(done);

  builder.SetInsertPoint(done);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("unknown_external_arity");
  bool hasInconsistentArityWarning = false;
  for (const notdec::bin2llvm::NativeRegisterEliminationWarning &warning :
       summary.Warnings) {
    hasInconsistentArityWarning |=
        warning.CalleeName == "unknown_external_arity" &&
        warning.Reason == "inconsistent_unknown_external_arity";
  }

  llvm::Function *rewrittenCaller =
      module.getFunction("unknown_external_arity_callers");
  std::vector<unsigned> callArities;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() == rewritten) {
        callArities.push_back(call->arg_size());
      }
    }
  }
  std::sort(callArities.begin(), callArities.end());
  bool hasVarArgWarning = false;
  for (const notdec::bin2llvm::NativeRegisterEliminationWarning &warning :
       summary.Warnings) {
    hasVarArgWarning |= warning.CalleeName == "unknown_external_arity" &&
                        warning.Reason == "inferred_unknown_external_vararg";
  }
  std::string arityMessage =
      "unknown external call sites did not keep their own arity (got";
  for (unsigned arity : callArities) {
    arityMessage += " " + std::to_string(arity);
  }
  arityMessage += ")";

  return expect(rewritten != nullptr,
                "unknown external arity callee missing") &&
         expect(rewritten->isVarArg(),
                "inconsistent unknown external arity was not modeled as "
                "vararg") &&
         expect(rewritten->arg_size() == 1,
                "unknown external fixed prefix did not use the min arity") &&
         expect(callArities == std::vector<unsigned>({1, 3}),
                arityMessage.c_str()) &&
         expect(hasInconsistentArityWarning,
                "unknown external arity mismatch warning missing") &&
         expect(hasVarArgWarning,
                "unknown external vararg inference warning missing") &&
         verifyOk(module,
                  "module failed verifier after unknown external arity test");
}

bool testVarArgPrologueWithTwoInitsInDifferentBlocksIsRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("vararg-rewrite-two-inits", context);
  llvm::IRBuilder<> builder(context);
  VarArgTestFixture fixture =
      buildVarArgTestFixture(module, "vararg_two_inits", builder);
  llvm::Value *vaList =
      emitManualVaListInit(builder, fixture.Frame, 0, 8, 208);
  emitVarArgAlTestAndXmmSave(builder, module, fixture.Frame, fixture.Rax,
                             fixture.Zmm0, fixture.Zmm1, "done");
  emitManualVaListInit(builder, fixture.Frame, 0, 8, 208);
  builder.CreateCall(fixture.UseVaList, {vaList});
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeVarArgRewriteSummary summary =
      notdec::bin2llvm::runNativeVarArgRewrite(module);
  std::optional<notdec::bin2llvm::NativeVarArgInfo> info =
      notdec::bin2llvm::nativeVarArgInfo(*fixture.Function);
  return expect(summary.FunctionsDetected == 1,
                "two-init vararg prologue was not detected") &&
         expect(summary.FunctionsRewritten == 1,
                "two-init vararg prologue was not rewritten") &&
         expect(summary.VaStartCallsInserted == 2,
                "two-init prologue did not insert two llvm.va_start") &&
         expect(countVaStartCalls(*fixture.Function) == 2,
                "two-init prologue did not keep two llvm.va_start") &&
         expect(countManualVaListConstantStores(*fixture.Function, 0) == 0,
                "hand-written va_list fields survived the rewrite") &&
         expect(summary.VaListFieldStoresRemoved == 6,
                "two-init prologue did not remove both field store groups") &&
         expect(info.has_value() && info->FixedGp == 1,
                "two-init prologue did not recover fixed_gp");
}

bool testVarArgPrologueWithTwoInitsInOneBlockIsRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("vararg-rewrite-one-block", context);
  llvm::IRBuilder<> builder(context);
  VarArgTestFixture fixture =
      buildVarArgTestFixture(module, "vararg_one_block", builder);
  llvm::Value *vaList =
      emitManualVaListInit(builder, fixture.Frame, 0, 8, 208);
  builder.CreateCall(fixture.UseVaList, {vaList});
  emitManualVaListInit(builder, fixture.Frame, 0, 8, 208);
  emitVarArgAlTestAndXmmSave(builder, module, fixture.Frame, fixture.Rax,
                             fixture.Zmm0, fixture.Zmm1, "done");
  builder.CreateCall(fixture.UseVaList, {vaList});
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeVarArgRewriteSummary summary =
      notdec::bin2llvm::runNativeVarArgRewrite(module);
  // 第二个 va_start 必须插在第一次调用之后：否则中间那次重新初始化被挪到了
  // 调用前面，语义就错了。
  int firstVaStart = -1;
  int lastVaStart = -1;
  int firstUse = -1;
  int index = 0;
  for (llvm::Instruction &inst : fixture.Function->getEntryBlock()) {
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
      if (call->getCalledFunction() != nullptr &&
          call->getCalledFunction()->getName().starts_with("llvm.va_start")) {
        if (firstVaStart < 0) {
          firstVaStart = index;
        }
        lastVaStart = index;
      }
      if (call->getCalledFunction() == fixture.UseVaList && firstUse < 0) {
        firstUse = index;
      }
    }
    ++index;
  }
  return expect(summary.FunctionsDetected == 1,
                "same-block two-init prologue was not detected") &&
         expect(summary.FunctionsRewritten == 1,
                "same-block two-init prologue was not rewritten") &&
         expect(countVaStartCalls(*fixture.Function) == 2,
                "same-block two-init prologue did not keep two va_start") &&
         expect(firstVaStart >= 0 && firstVaStart < firstUse &&
                    firstUse < lastVaStart,
                "second va_start was not placed after the first use") &&
         expect(countManualVaListConstantStores(*fixture.Function, 0) == 0,
                "hand-written va_list fields survived the rewrite");
}

bool testVarArgPrologueWithTwoVaListObjectsIsRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("vararg-rewrite-two-objects", context);
  llvm::IRBuilder<> builder(context);
  VarArgTestFixture fixture =
      buildVarArgTestFixture(module, "vararg_two_objects", builder);
  llvm::Value *first = emitManualVaListInit(builder, fixture.Frame, 0, 8, 208);
  llvm::Value *second =
      emitManualVaListInit(builder, fixture.Frame, 64, 8, 208);
  emitVarArgAlTestAndXmmSave(builder, module, fixture.Frame, fixture.Rax,
                             fixture.Zmm0, fixture.Zmm1, "done");
  builder.CreateCall(fixture.UseVaList, {first});
  builder.CreateCall(fixture.UseVaList, {second});
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeVarArgRewriteSummary summary =
      notdec::bin2llvm::runNativeVarArgRewrite(module);
  std::set<llvm::Value *> startPointers;
  for (llvm::Instruction &inst : llvm::instructions(*fixture.Function)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName().starts_with("llvm.va_start")) {
      startPointers.insert(call->getArgOperand(0));
    }
  }
  return expect(summary.FunctionsDetected == 1,
                "two-va_list prologue was not detected") &&
         expect(summary.FunctionsRewritten == 1,
                "two-va_list prologue was not rewritten") &&
         expect(summary.VaStartCallsInserted == 2,
                "two-va_list prologue did not insert two llvm.va_start") &&
         expect(startPointers.size() == 2 && startPointers.count(first) == 1 &&
                    startPointers.count(second) == 1,
                "two-va_list prologue did not use each va_list pointer") &&
         expect(countManualVaListConstantStores(*fixture.Function, 0) == 0 &&
                    countManualVaListConstantStores(*fixture.Function, 64) == 0,
                "hand-written va_list fields survived the rewrite");
}

bool testVarArgPrologueWithConflictingInitsIsRejected() {
  llvm::LLVMContext context;
  llvm::Module module("vararg-rewrite-conflict", context);
  llvm::IRBuilder<> builder(context);
  VarArgTestFixture fixture =
      buildVarArgTestFixture(module, "vararg_conflict", builder);
  llvm::Value *first = emitManualVaListInit(builder, fixture.Frame, 0, 8, 208);
  emitManualVaListInit(builder, fixture.Frame, 64, 16, 208);
  emitVarArgAlTestAndXmmSave(builder, module, fixture.Frame, fixture.Rax,
                             fixture.Zmm0, fixture.Zmm1, "done");
  builder.CreateCall(fixture.UseVaList, {first});
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeVarArgRewriteSummary summary =
      notdec::bin2llvm::runNativeVarArgRewrite(module);
  return expect(summary.FunctionsDetected == 0,
                "conflicting va_list inits were detected") &&
         expect(summary.FunctionsRewritten == 0,
                "conflicting va_list inits were rewritten") &&
         expect(countVaStartCalls(*fixture.Function) == 0,
                "conflicting va_list inits inserted llvm.va_start") &&
         expect(countManualVaListConstantStores(*fixture.Function, 0) == 2 &&
                    countManualVaListConstantStores(*fixture.Function, 64) == 2,
                "conflicting va_list inits were partially rewritten");
}

bool testVarArgPrologueWithNoGpSavesIsRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("vararg-rewrite-no-gp", context);
  llvm::IRBuilder<> builder(context);
  VarArgTestFixture fixture = buildVarArgTestFixture(
      module, "vararg_no_gp", builder, /*withGpSaves=*/false);
  llvm::Value *vaList =
      emitManualVaListInit(builder, fixture.Frame, 0, 48, 208);
  emitVarArgAlTestAndXmmSave(builder, module, fixture.Frame, fixture.Rax,
                             fixture.Zmm0, fixture.Zmm1, "done");
  builder.CreateCall(fixture.UseVaList, {vaList});
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeVarArgRewriteSummary summary =
      notdec::bin2llvm::runNativeVarArgRewrite(module);
  std::optional<notdec::bin2llvm::NativeVarArgInfo> info =
      notdec::bin2llvm::nativeVarArgInfo(*fixture.Function);
  return expect(summary.FunctionsDetected == 1,
                "no-GP-save vararg prologue was not detected") &&
         expect(summary.FunctionsRewritten == 1,
                "no-GP-save vararg prologue was not rewritten") &&
         expect(info.has_value() && info->FixedGp == 6,
                "no-GP-save case did not recover fixed_gp=6") &&
         expect(summary.VaStartCallsInserted == 1 &&
                    countVaStartCalls(*fixture.Function) == 1,
                "no-GP-save case did not insert one llvm.va_start") &&
         expect(summary.RegisterSavesRemoved == 2,
                "no-GP-save case did not remove the XMM saves") &&
         expect(countManualVaListConstantStores(*fixture.Function, 0) == 0,
                "hand-written va_list fields survived the rewrite");
}

bool testVarArgPrologueWithSpillAndAlNoiseIsDetected() {
  llvm::LLVMContext context;
  llvm::Module module("vararg-rewrite-spill", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  auto *i64Type = llvm::Type::getInt64Ty(context);
  auto *i128Type = llvm::Type::getInt128Ty(context);
  auto *i512Type = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");
  llvm::GlobalVariable *r9 = createRegisterGlobal(module, "R9");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", i512Type, 0, 64);
  auto *sink = new llvm::GlobalVariable(
      module, i64Type, false, llvm::GlobalValue::ExternalLinkage, nullptr,
      "spill_sink");
  llvm::Function *useVaList = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                              {llvm::PointerType::get(context, 0)}, false),
      llvm::GlobalValue::ExternalLinkage, "spill_use_va_list", module);

  llvm::Function *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {i64Type}, false),
      llvm::GlobalValue::InternalLinkage, "native_vararg_wrapper_spill", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *xmmSave =
      llvm::BasicBlock::Create(context, "xmm_save", function);
  llvm::BasicBlock *done = llvm::BasicBlock::Create(context, "done", function);
  llvm::IRBuilder<> builder(entry);
  llvm::AllocaInst *frame = builder.CreateAlloca(
      llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 400), nullptr,
      "frame");
  auto framePointer = [&](uint64_t offset) {
    return builder.CreateGEP(llvm::Type::getInt8Ty(context), frame,
                             llvm::ConstantInt::get(i64Type, offset));
  };
  llvm::Value *fixed = loadRegister(builder, rdi, "RDI", "fixed");
  builder.CreateStore(fixed, sink);
  // 真保存区：R8/R9 落在 base=208 的 +32/+40。
  llvm::Value *savedR8 = loadRegister(builder, r8, "R8", "saved_r8");
  builder.CreateStore(savedR8, framePointer(240));
  llvm::Value *savedR9 = loadRegister(builder, r9, "R9", "saved_r9");
  builder.CreateStore(savedR9, framePointer(248));
  // 普通 spill：同一个 R8 再存到别的帧槽（base=160 ≠ 208），旧逻辑会因此放弃。
  // 槽位要避开 FP 保存区窗口（fpBase=256 起 128 字节）和 va_list 窗口（0..24）。
  llvm::Value *spilledR8 = loadRegister(builder, r8, "R8", "spilled_r8");
  builder.CreateStore(spilledR8, framePointer(192));
  // 无关的 AL 用途：把 AL 当数据存到内存（格式解析里的常见形态），旧逻辑的 AL
  // 闭包会因"非标志用途"整函数放弃。
  llvm::Value *unrelatedAl = builder.CreateCall(
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, llvm::PointerType::get(context, 0),
          llvm::Type::getInt8Ty(context), 64, 8),
      {rax, llvm::ConstantInt::get(i64Type, 0)}, "unrelated_al");
  builder.CreateStore(unrelatedAl, sink);
  // 手写 va_list（+0 gp_offset、+4 fp_offset、+8 overflow、+16 reg_save_area）。
  llvm::GlobalVariable *overflow = new llvm::GlobalVariable(
      module, i64Type, false, llvm::GlobalValue::ExternalLinkage, nullptr,
      "spill_overflow_area");
  llvm::Value *overflowValue = loadRegister(builder, overflow, "overflow");
  builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 8),
                      framePointer(0));
  builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 48),
      framePointer(4));
  builder.CreateStore(overflowValue, framePointer(8));
  builder.CreateStore(
      builder.CreatePtrToInt(framePointer(208), i64Type), framePointer(16));
  llvm::Value *al = builder.CreateCall(
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, llvm::PointerType::get(context, 0),
          llvm::Type::getInt8Ty(context), 64, 8),
      {rax, llvm::ConstantInt::get(i64Type, 0)}, "al");
  llvm::Value *alZero = builder.CreateICmpEQ(
      al, llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 0));
  builder.CreateCondBr(alZero, done, xmmSave);
  builder.SetInsertPoint(xmmSave);
  llvm::Value *savedZmm = builder.CreateCall(
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, llvm::PointerType::get(context, 0), i128Type, 512, 128),
      {zmm0, llvm::ConstantInt::get(i64Type, 0)}, "saved_zmm0");
  builder.CreateStore(savedZmm, framePointer(256));
  builder.CreateBr(done);
  builder.SetInsertPoint(done);
  builder.CreateCall(useVaList, {framePointer(0)});
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeVarArgRewriteOptions options;
  notdec::bin2llvm::NativeVarArgRewriteSummary summary =
      notdec::bin2llvm::runNativeVarArgRewrite(module, options);
  std::optional<notdec::bin2llvm::NativeVarArgInfo> info =
      notdec::bin2llvm::nativeVarArgInfo(*function);
  unsigned vaStarts = 0;
  for (llvm::Instruction &inst : llvm::instructions(*function)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName().starts_with("llvm.va_start")) {
      ++vaStarts;
    }
  }
  return expect(summary.FunctionsDetected == 1,
                "vararg prologue with an ordinary spill was not detected") &&
         expect(summary.FunctionsRewritten == 1,
                "vararg prologue with an ordinary spill was not rewritten") &&
         expect(info.has_value() && info->FixedGp == 1,
                "spill case did not recover fixed_gp") &&
         expect(vaStarts == 1, "spill case did not insert llvm.va_start");
}

bool testVarArgPrologueWithSingleGpSaveIsDetected() {
  llvm::LLVMContext context;
  llvm::Module module("vararg-rewrite-single-gp", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  auto *i64Type = llvm::Type::getInt64Ty(context);
  auto *i32Type = llvm::Type::getInt32Ty(context);
  auto *i128Type = llvm::Type::getInt128Ty(context);
  auto *i512Type = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
  llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");
  llvm::GlobalVariable *r9 = createRegisterGlobal(module, "R9");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", i512Type, 0, 64);
  llvm::GlobalVariable *zmm1 =
      createRegisterGlobal(module, "ZMM1", i512Type, 0, 64);
  auto *sink = new llvm::GlobalVariable(
      module, i64Type, false, llvm::GlobalValue::ExternalLinkage, nullptr,
      "single_gp_sink");
  llvm::Function *useVaList = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                              {llvm::PointerType::get(context, 0)}, false),
      llvm::GlobalValue::ExternalLinkage, "single_gp_use_va_list", module);

  llvm::Function *function = llvm::Function::Create(
      llvm::FunctionType::get(
          llvm::Type::getVoidTy(context),
          {i64Type, i64Type, i64Type, i64Type, i64Type}, false),
      llvm::GlobalValue::InternalLinkage, "native_vararg_single_gp", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *xmmSave =
      llvm::BasicBlock::Create(context, "xmm_save", function);
  llvm::BasicBlock *done = llvm::BasicBlock::Create(context, "done", function);
  llvm::IRBuilder<> builder(entry);
  llvm::AllocaInst *frame = builder.CreateAlloca(
      llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 400), nullptr,
      "frame");
  auto framePointer = [&](uint64_t offset) {
    return builder.CreateGEP(llvm::Type::getInt8Ty(context), frame,
                             llvm::ConstantInt::get(i64Type, offset));
  };
  // 5 个固定参数：RDI..R8 在入口被读。
  for (llvm::GlobalVariable *reg : {rdi, rsi, rdx, rcx, r8}) {
    builder.CreateStore(
        loadRegister(builder, reg, reg->getName().str(), "fixed"), sink);
  }
  // 保存区 base=208：变参从 R9 开始，所以只有 +40 一个槽。
  llvm::Value *savedR9 = loadRegister(builder, r9, "R9", "saved_r9");
  builder.CreateStore(savedR9, framePointer(248));
  // 手写 va_list：gp_offset=40（5 个固定 GP 参数）、fp_offset=48。
  llvm::GlobalVariable *overflow = new llvm::GlobalVariable(
      module, i64Type, false, llvm::GlobalValue::ExternalLinkage, nullptr,
      "single_gp_overflow_area");
  llvm::Value *overflowValue = loadRegister(builder, overflow, "overflow");
  builder.CreateStore(llvm::ConstantInt::get(i32Type, 40), framePointer(0));
  builder.CreateStore(llvm::ConstantInt::get(i32Type, 48), framePointer(4));
  builder.CreateStore(overflowValue, framePointer(8));
  builder.CreateStore(
      builder.CreatePtrToInt(framePointer(208), i64Type), framePointer(16));
  llvm::Value *al = builder.CreateCall(
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, llvm::PointerType::get(context, 0),
          llvm::Type::getInt8Ty(context), 64, 8),
      {rax, llvm::ConstantInt::get(i64Type, 0)}, "al");
  llvm::Value *alZero = builder.CreateICmpEQ(
      al, llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 0));
  builder.CreateCondBr(alZero, done, xmmSave);
  builder.SetInsertPoint(xmmSave);
  llvm::Value *savedZmm0 = builder.CreateCall(
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, llvm::PointerType::get(context, 0), i128Type, 512, 128),
      {zmm0, llvm::ConstantInt::get(i64Type, 0)}, "saved_zmm0");
  builder.CreateStore(savedZmm0, framePointer(256));
  llvm::Value *savedZmm1 = builder.CreateCall(
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, llvm::PointerType::get(context, 0), i128Type, 512, 128),
      {zmm1, llvm::ConstantInt::get(i64Type, 0)}, "saved_zmm1");
  builder.CreateStore(savedZmm1, framePointer(272));
  builder.CreateBr(done);
  builder.SetInsertPoint(done);
  builder.CreateCall(useVaList, {framePointer(0)});
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeVarArgRewriteOptions options;
  notdec::bin2llvm::NativeVarArgRewriteSummary summary =
      notdec::bin2llvm::runNativeVarArgRewrite(module, options);
  std::optional<notdec::bin2llvm::NativeVarArgInfo> info =
      notdec::bin2llvm::nativeVarArgInfo(*function);
  unsigned vaStarts = 0;
  for (llvm::Instruction &inst : llvm::instructions(*function)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName().starts_with("llvm.va_start")) {
      ++vaStarts;
    }
  }
  return expect(summary.FunctionsDetected == 1,
                "single GP save vararg prologue was not detected") &&
         expect(summary.FunctionsRewritten == 1,
                "single GP save vararg prologue was not rewritten") &&
         expect(info.has_value() && info->FixedGp == 5,
                "single GP save case did not recover fixed_gp=5") &&
         expect(vaStarts == 1,
                "single GP save case did not insert llvm.va_start");
}

bool testVarArgPrologueBecomesVaStart() {
  llvm::LLVMContext context;
  llvm::Module module("vararg-rewrite", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  auto *i64Type = llvm::Type::getInt64Ty(context);
  auto *i128Type = llvm::Type::getInt128Ty(context);
  auto *i512Type = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");
  llvm::GlobalVariable *r9 = createRegisterGlobal(module, "R9");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", i512Type, 0, 64);
  auto *sink = new llvm::GlobalVariable(
      module, i64Type, false, llvm::GlobalValue::ExternalLinkage, nullptr,
      "sink");
  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *useVaList = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                              {llvm::PointerType::get(context, 0)}, false),
      llvm::GlobalValue::ExternalLinkage, "use_va_list", module);

  llvm::Function *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {i64Type}, false),
      llvm::GlobalValue::InternalLinkage, "native_vararg_wrapper", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *xmmSave =
      llvm::BasicBlock::Create(context, "xmm_save", function);
  llvm::BasicBlock *done = llvm::BasicBlock::Create(context, "done", function);
  llvm::IRBuilder<> builder(entry);
  llvm::AllocaInst *frame =
      builder.CreateAlloca(llvm::ArrayType::get(llvm::Type::getInt8Ty(context),
                                               400),
                           nullptr, "frame");
  auto framePointer = [&](uint64_t offset) {
    return builder.CreateGEP(llvm::Type::getInt8Ty(context), frame,
                             llvm::ConstantInt::get(i64Type, offset));
  };
  // 固定参数 RDI 直接读（保持它是固定参数）。
  llvm::Value *fixed = loadRegister(builder, rdi, "RDI", "fixed");
  builder.CreateStore(fixed, sink);
  // 保存区：R8/R9 落在 base=208 的 +32/+40（gp_offset=8 → 1 个固定 GP 参数）。
  llvm::Function *partialReadI64 =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
      module, llvm::PointerType::get(context, 0), i64Type, 64, 64);
  llvm::Value *savedR8 = loadRegister(builder, r8, "R8", "saved_r8");
  builder.CreateStore(savedR8, framePointer(240));
  llvm::Value *savedR9 = loadRegister(builder, r9, "R9", "saved_r9");
  builder.CreateStore(savedR9, framePointer(248));
  // 手写 va_list（+0 gp_offset、+4 fp_offset、+8 overflow、+16 reg_save_area）。
  llvm::GlobalVariable *overflow = new llvm::GlobalVariable(
      module, i64Type, false, llvm::GlobalValue::ExternalLinkage, nullptr,
      "overflow_area");
  llvm::Value *overflowValue = loadRegister(builder, overflow, "overflow");
  builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 8),
                      framePointer(0));
  builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 48),
      framePointer(4));
  builder.CreateStore(overflowValue, framePointer(8));
  builder.CreateStore(
      builder.CreatePtrToInt(framePointer(208), i64Type), framePointer(16));
  // AL 测试（test al, al）→ 条件保存 XMM。
  llvm::Value *al = builder.CreateCall(
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, llvm::PointerType::get(context, 0),
          llvm::Type::getInt8Ty(context), 64, 8),
      {rax, llvm::ConstantInt::get(i64Type, 0)}, "al");
  llvm::Value *alZero = builder.CreateICmpEQ(
      al, llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 0));
  builder.CreateCondBr(alZero, done, xmmSave);
  builder.SetInsertPoint(xmmSave);
  llvm::Value *savedZmm = builder.CreateCall(
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, llvm::PointerType::get(context, 0), i128Type, 512, 128),
      {zmm0, llvm::ConstantInt::get(i64Type, 0)}, "saved_zmm0");
  builder.CreateStore(savedZmm, framePointer(256));
  builder.CreateBr(done);
  builder.SetInsertPoint(done);
  builder.CreateCall(useVaList, {framePointer(0)});
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeVarArgRewriteOptions options;
  notdec::bin2llvm::NativeVarArgRewriteSummary summary =
      notdec::bin2llvm::runNativeVarArgRewrite(module, options);

  std::optional<notdec::bin2llvm::NativeVarArgInfo> info =
      notdec::bin2llvm::nativeVarArgInfo(*function);
  unsigned vaStarts = 0;
  for (llvm::Instruction &inst : llvm::instructions(*function)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName().starts_with("llvm.va_start")) {
      ++vaStarts;
    }
  }
  bool rawVarArgReads = false;
  for (llvm::Instruction &inst : llvm::instructions(*function)) {
    auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
    if (load != nullptr &&
        (load->getPointerOperand()->stripPointerCasts() == r8 ||
         load->getPointerOperand()->stripPointerCasts() == r9 ||
         load->getPointerOperand()->stripPointerCasts() == zmm0)) {
      rawVarArgReads = true;
    }
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    std::optional<notdec::bin2llvm::NativeRegisterPartialReadInfo> read =
        call == nullptr ? std::nullopt
                        : notdec::bin2llvm::parseNativeRegisterPartialRead(*call);
    if (read.has_value() && read->Global != nullptr &&
        (read->Global == r8 || read->Global == r9 || read->Global == zmm0)) {
      rawVarArgReads = true;
    }
  }

  return expect(summary.FunctionsDetected == 1,
                "vararg prologue was not detected") &&
         expect(summary.FunctionsRewritten == 1,
                "vararg prologue was not rewritten") &&
         expect(notdec::bin2llvm::isNativeVarArgFunction(*function),
                "vararg metadata missing") &&
         expect(info.has_value() && info->FixedGp == 1,
                "vararg fixed_gp was not recovered") &&
         expect(vaStarts == 1, "llvm.va_start was not inserted") &&
         expect(!rawVarArgReads,
                "vararg register reads survived the rewrite");
  // 注意：此刻函数类型还没改成变参（签名重写在 summary SSA 里做），
  // llvm.va_start 在非变参函数里会被 verifier 拒绝，所以这里先不 verify。

  // 走完整 summary SSA：签名要带 ...（固定参数只剩 RDI）。
  llvm::Function *rewritten = module.getFunction("native_vararg_wrapper");
  if (!expect(rewritten != nullptr, "vararg function missing after SSA")) {
    return false;
  }
  notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *finalFunction = module.getFunction("native_vararg_wrapper");
  return expect(finalFunction != nullptr, "vararg function lost after SSA") &&
         expect(finalFunction->isVarArg(),
                "vararg function signature is not variadic") &&
         expect(finalFunction->arg_size() == 1,
                "vararg function kept garbage fixed parameters") &&
         verifyOk(module, "module failed verifier after vararg SSA");
}

bool testManualVaArgDiamondBecomesVaArgInstruction() {
  llvm::LLVMContext context;
  llvm::Module module("manual-va-arg-fold", context);
  auto *i32Type = llvm::Type::getInt32Ty(context);
  auto *i64Type = llvm::Type::getInt64Ty(context);
  auto *doubleType = llvm::Type::getDoubleTy(context);
  auto *ptrType = llvm::PointerType::get(context, 0);

  auto buildConsumer = [&](llvm::StringRef name, llvm::Type *valueType,
                           uint64_t limit, uint64_t regStride,
                           uint64_t fieldOffset, bool withBump) {
    llvm::Function *function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                {ptrType, ptrType}, false),
        llvm::GlobalValue::ExternalLinkage, name, module);
    llvm::Argument *ap = function->getArg(0);
    llvm::Argument *sink = function->getArg(1);
    llvm::BasicBlock *entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::BasicBlock *reg = llvm::BasicBlock::Create(context, "reg", function);
    llvm::BasicBlock *stack =
        llvm::BasicBlock::Create(context, "stack", function);
    llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);
    llvm::IRBuilder<> builder(entry);
    auto field = [&](uint64_t offset) {
      return builder.CreateGEP(llvm::Type::getInt8Ty(context), ap,
                               llvm::ConstantInt::get(i64Type, offset));
    };
    llvm::Value *offField = field(fieldOffset);
    llvm::Value *overflowField = field(8);
    llvm::Value *regSaveField = field(16);
    llvm::Value *off = builder.CreateLoad(i32Type, offField, "off");
    llvm::Value *cmp = builder.CreateICmpULT(
        off, llvm::ConstantInt::get(i32Type, limit));
    builder.CreateCondBr(cmp, reg, stack);
    builder.SetInsertPoint(reg);
    llvm::Value *base = builder.CreateLoad(ptrType, regSaveField, "reg_save");
    llvm::Value *index = builder.CreateZExt(off, i64Type);
    llvm::Value *addr =
        builder.CreateGEP(llvm::Type::getInt8Ty(context), base, index);
    if (withBump) {
      llvm::Value *next =
          builder.CreateAdd(off, llvm::ConstantInt::get(i32Type, regStride));
      builder.CreateStore(next, offField);
    }
    llvm::Value *v1 = builder.CreateLoad(valueType, addr, "v1");
    builder.CreateBr(join);
    builder.SetInsertPoint(stack);
    llvm::Value *overflow =
        builder.CreateLoad(ptrType, overflowField, "overflow");
    llvm::Value *q2 = builder.CreateGEP(llvm::Type::getInt8Ty(context), overflow,
                                        llvm::ConstantInt::get(i64Type, 8));
    builder.CreateStore(q2, overflowField);
    llvm::Value *v2 = builder.CreateLoad(valueType, q2, "v2");
    builder.CreateBr(join);
    builder.SetInsertPoint(join);
    llvm::PHINode *phi = builder.CreatePHI(valueType, 2, "v");
    phi->addIncoming(v1, reg);
    phi->addIncoming(v2, stack);
    builder.CreateStore(phi, sink);
    builder.CreateRetVoid();
    return function;
  };

  buildConsumer("va_arg_gp", i32Type, 48, 8, 0, /*withBump=*/true);
  buildConsumer("va_arg_fp", doubleType, 176, 16, 4, /*withBump=*/true);
  buildConsumer("va_arg_incomplete", i32Type, 48, 8, 0, /*withBump=*/false);

  llvm::Function *gp = module.getFunction("va_arg_gp");
  llvm::Function *fp = module.getFunction("va_arg_fp");
  llvm::Function *incomplete = module.getFunction("va_arg_incomplete");
  uint64_t folded = notdec::bin2llvm::foldManualVaArgSequences(module);

  auto vaArgs = [](llvm::Function &function) {
    std::vector<llvm::VAArgInst *> found;
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      if (auto *vaArg = llvm::dyn_cast<llvm::VAArgInst>(&inst)) {
        found.push_back(vaArg);
      }
    }
    return found;
  };

  return expect(folded == 2, "manual va_arg diamonds were not folded") &&
         expect(vaArgs(*gp).size() == 1 &&
                    vaArgs(*gp).front()->getType() == i32Type,
                "GP va_arg diamond was not folded to an i32 va_arg") &&
         expect(vaArgs(*fp).size() == 1 &&
                    vaArgs(*fp).front()->getType() == doubleType,
                "FP va_arg diamond was not folded to a double va_arg") &&
         expect(vaArgs(*incomplete).empty(),
                "incomplete va_arg sequence must be left alone") &&
         verifyOk(module, "module failed verifier after va_arg folding");
}

} // namespace

int main() {
  bool ok = true;
  ok &= testI386NativeFrameOutgoingVarArgPrefixIsBound();
  ok &= testI386NativeFrameOutgoingFloatVarArgIsWidened();
  ok &= testI386StackPointerSummaryOutgoingVarArgPrefixIsBound();
  ok &= testKnownVarArgExternalInfersDefinedAbiInputs();
  ok &= testKnownVarArgFixedOnlyCallDoesNotReadTail();
  ok &= testKnownVarArgCallsitesKeepIndependentArities();
  ok &= testKnownVarArgDoesNotReuseStaleCallerSavedTail();
  ok &= testKnownVarArgUsesSseCountForFloatTail();
  ok &= testKnownVarArgZeroTailKeepsConstantZero();
  ok &= testKnownVarArgUnknownPhiTailUsesPoison();
  ok &= testNonX64VarArgUnknownPhiTailDoesNotUsePoisonHelper();
  ok &= testUnknownExternalArityUsesVarArgTail();
  ok &= testVarArgPrologueWithTwoInitsInDifferentBlocksIsRewritten();
  ok &= testVarArgPrologueWithTwoInitsInOneBlockIsRewritten();
  ok &= testVarArgPrologueWithTwoVaListObjectsIsRewritten();
  ok &= testVarArgPrologueWithConflictingInitsIsRejected();
  ok &= testVarArgPrologueWithNoGpSavesIsRewritten();
  ok &= testVarArgPrologueWithSpillAndAlNoiseIsDetected();
  ok &= testVarArgPrologueWithSingleGpSaveIsDetected();
  ok &= testVarArgPrologueBecomesVaStart();
  ok &= testManualVaArgDiamondBecomesVaArgInstruction();
  return ok ? 0 : 1;
}
