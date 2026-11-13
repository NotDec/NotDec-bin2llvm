#include "RegisterTestHarness.h"

namespace {

bool testPhiIncomingMatchesPredecessors() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-phi", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context),
                                       {llvm::Type::getInt1Ty(context)}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "branch_merge", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right =
      llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(function->getArg(0), left, right);
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

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("branch_merge");
  return expect(summary.LoadsReplaced == 1, "branch load was not replaced") &&
         expect(summary.PhisCreated >= 1, "complete PHI was not created") &&
         expect(rewritten != nullptr, "branch function was not preserved") &&
         expect(!hasLiveReplacedRegisterLoad(*rewritten),
                "replaced load was reused by completed PHI") &&
         verifyOk(module, "module failed verifier after summary SSA PHI test");
}

bool testX87StackReturnBecomesX86Fp80ReturnSlot() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-x87-stack-return", context);
  attachTestAbi(module);

  llvm::GlobalVariable *st0 = createRegisterGlobal(
      module, "ST0", llvm::Type::getIntNTy(context, 80), 0x1100, 10);

  // lifting 形态：函数先建为 void 返回（void (i64)），函数体把值写进 @ST0
  // 后直接 ret，模拟 fldt; ret 的 long double 返回函数。
  auto *type = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::InternalLinkage, "x87_stack_return", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getIntNTy(context, 80),
                                             123),
                      st0);
  builder.CreateRetVoid();

  // 调用者（根函数，无 seed 的 ST0 需求）在调用后读 @ST0 并存进内存，模拟
  // fstpt 接住 long double 返回值，让 x87_stack_return 从调用点获得 ST0 的
  // ExitDemand。
  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *caller = llvm::Function::Create(
      callerType, llvm::GlobalValue::InternalLinkage, "x87_stack_return_caller",
      module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "caller_entry", caller);
  llvm::IRBuilder<> callerBuilder(callerEntry);
  llvm::AllocaInst *slot =
      callerBuilder.CreateAlloca(llvm::Type::getIntNTy(context, 80));
  callerBuilder.CreateCall(
      function->getFunctionType(), function,
      {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::LoadInst *st0Load =
      callerBuilder.CreateLoad(llvm::Type::getIntNTy(context, 80), st0);
  st0Load->setMetadata("notdec.register.access",
                       registerAccessMetadata(context, "ST0"));
  callerBuilder.CreateStore(st0Load, slot);
  callerBuilder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("x87_stack_return");
  bool hasSt0Param = false;
  if (rewritten != nullptr) {
    for (llvm::Argument &argument : rewritten->args()) {
      hasSt0Param |= argument.getName() == "ST0";
    }
  }
  return expect(rewritten != nullptr,
                "x87 stack return function was not preserved") &&
         expect(rewritten->getReturnType()->isX86_FP80Ty(),
                "x87 ST0 store was not recovered as an x86_fp80 return slot") &&
         expect(!hasSt0Param,
                "x87 ST0 must not become a function parameter (SysV)") &&
         verifyOk(module,
                  "module failed verifier after x87 stack return test");
}

bool testDeadFrameSpillPartialReadCreatesNoEntryParam() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-dead-frame-spill-read", context);
  attachTestAbi(module);
  llvm::Type *i512 = llvm::Type::getIntNTy(context, 512);
  llvm::Type *i128 = llvm::Type::getIntNTy(context, 128);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", i512, 0x2000, 64);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, llvm::PointerType::get(context, 0), i128, 512, 128);

  auto *calleeType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::InternalLinkage, "dead_frame_spill", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  llvm::AllocaInst *frame = calleeBuilder.CreateAlloca(
      llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 64), nullptr,
      "notdec_stack.native");
  llvm::Value *slot = calleeBuilder.CreateConstGEP1_64(
      llvm::Type::getInt8Ty(context), frame, 8, "slot");
  llvm::Value *spilled = calleeBuilder.CreateCall(
      partialRead,
      {zmm0, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "spill");
  calleeBuilder.CreateStore(spilled, slot);
  calleeBuilder.CreateRetVoid();

  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *caller = llvm::Function::Create(
      callerType, llvm::GlobalValue::InternalLinkage, "dead_frame_spill_caller",
      module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "caller_entry", caller);
  llvm::IRBuilder<> callerBuilder(callerEntry);
  callerBuilder.CreateCall(
      callee->getFunctionType(), callee,
      {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  callerBuilder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("dead_frame_spill");
  if (rewritten == nullptr) {
    return expect(false, "dead frame spill function was removed");
  }
  bool hasZmmParam = false;
  for (llvm::Argument &argument : rewritten->args()) {
    hasZmmParam |= argument.getName().starts_with("ZMM0");
  }
  return expect(!hasZmmParam,
                "dead frame spill still demanded a ZMM0 entry parameter") &&
         expect(!moduleHasUsedFunctionNamed(module, "notdec.unknown.i128"),
                "dead frame spill created an unknown vector value") &&
         verifyOk(module,
                  "module failed verifier after dead frame spill test");
}

bool testLiveFrameSpillPartialReadKeepsValue() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-live-frame-spill-read", context);
  attachTestAbi(module);
  llvm::Type *i512 = llvm::Type::getIntNTy(context, 512);
  llvm::Type *i128 = llvm::Type::getIntNTy(context, 128);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", i512, 0x2000, 64);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, llvm::PointerType::get(context, 0), i128, 512, 128);
  auto *sink = new llvm::GlobalVariable(
      module, i128, false, llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *calleeType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::InternalLinkage, "live_frame_spill", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  llvm::AllocaInst *frame = calleeBuilder.CreateAlloca(
      llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 64), nullptr,
      "notdec_stack.native");
  llvm::Value *slot = calleeBuilder.CreateConstGEP1_64(
      llvm::Type::getInt8Ty(context), frame, 8, "slot");
  llvm::Value *spilled = calleeBuilder.CreateCall(
      partialRead,
      {zmm0, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "spill");
  calleeBuilder.CreateStore(spilled, slot);
  llvm::Value *reloaded = calleeBuilder.CreateLoad(i128, slot, "reloaded");
  calleeBuilder.CreateStore(reloaded, sink);
  calleeBuilder.CreateRetVoid();

  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *caller = llvm::Function::Create(
      callerType, llvm::GlobalValue::InternalLinkage, "live_frame_spill_caller",
      module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "caller_entry", caller);
  llvm::IRBuilder<> callerBuilder(callerEntry);
  callerBuilder.CreateCall(
      callee->getFunctionType(), callee,
      {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  callerBuilder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("live_frame_spill");
  if (rewritten == nullptr) {
    return expect(false, "live frame spill function was removed");
  }
  bool hasZmmParam = false;
  for (llvm::Argument &argument : rewritten->args()) {
    hasZmmParam |= argument.getName().starts_with("ZMM0");
  }
  bool hasUnknown = moduleHasUsedFunctionNamed(module, "notdec.unknown.i128");
  return expect(hasZmmParam || hasUnknown ||
                    functionReadsRegisterGlobal(*rewritten, zmm0),
                "live frame spill lost the vector value entirely") &&
         verifyOk(module,
                  "module failed verifier after live frame spill test");
}

bool testDirectionFlagIsZeroAtBoundaries() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-direction-flag", context);
  attachTestAbi(module);
  llvm::Type *i8 = llvm::Type::getInt8Ty(context);
  llvm::GlobalVariable *df = createRegisterGlobal(module, "DF", i8, 0x1200, 1);
  auto *sink = new llvm::GlobalVariable(module, i8, false,
                                        llvm::GlobalValue::ExternalLinkage,
                                        nullptr, "sink");

  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {},
                                             false);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::InternalLinkage, "df_touch", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  llvm::StoreInst *set = calleeBuilder.CreateStore(
      llvm::ConstantInt::get(i8, 1), df); // 内部 std：可能写 1
  set->setMetadata("notdec.register.access",
                   registerAccessMetadata(context, "DF", 1));
  calleeBuilder.CreateRetVoid();

  auto *callerType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {},
                                             false);
  llvm::Function *caller = llvm::Function::Create(
      callerType, llvm::GlobalValue::InternalLinkage, "df_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "caller_entry", caller);
  llvm::IRBuilder<> callerBuilder(callerEntry);
  llvm::LoadInst *entryRead = callerBuilder.CreateLoad(i8, df, "df.entry");
  entryRead->setMetadata("notdec.register.access",
                         registerAccessMetadata(context, "DF", 1));
  callerBuilder.CreateStore(entryRead, sink);
  callerBuilder.CreateCall(callee->getFunctionType(), callee);
  llvm::LoadInst *afterCall = callerBuilder.CreateLoad(i8, df, "df.after");
  afterCall->setMetadata("notdec.register.access",
                         registerAccessMetadata(context, "DF", 1));
  callerBuilder.CreateStore(afterCall, sink);
  callerBuilder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("df_caller");
  if (rewritten == nullptr) {
    return expect(false, "direction flag caller was removed");
  }
  return expect(!moduleHasUsedFunctionNamed(module, "notdec.unknown.i8"),
                "direction flag became unknown at a function boundary") &&
         verifyOk(module, "module failed verifier after direction flag test");
}

bool testI386KnownExternalUsesCspecStackOffset() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-stack-known-external", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module, 12);
  auto *i32 = llvm::Type::getInt32Ty(context);
  llvm::GlobalVariable *esp = createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *calleeType = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "puts", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "i386_stack_known", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *espEntry = loadRegister(builder, esp, "ESP", "esp.entry", 4);
  llvm::Value *argAddress =
      builder.CreateAdd(espEntry, llvm::ConstantInt::get(i32, 8), "arg.addr");
  llvm::Value *argPointer =
      builder.CreateIntToPtr(argAddress, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(i32, 42), argPointer);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenFunction = module.getFunction("i386_stack_known");
  if (!expect(rewrittenFunction != nullptr,
              "i386 stack external caller missing")) {
    return false;
  }
  llvm::CallInst *rewritten = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName() == "puts") {
      rewritten = call;
    }
  }
  auto *constant =
      rewritten == nullptr || rewritten->arg_size() != 1
          ? nullptr
          : llvm::dyn_cast<llvm::ConstantInt>(rewritten->getArgOperand(0));
  return expect(rewritten != nullptr, "i386 stack external call missing") &&
         expect(rewritten->arg_size() == 1,
                "i386 stack external did not use one stack arg") &&
         expect(constant != nullptr && constant->getZExtValue() == 42,
                "i386 stack arg did not come from cspec offset") &&
         expect(summary.CallArgStoresMarked == 1,
                "i386 stack arg store was not marked") &&
         verifyOk(module,
                  "module failed verifier after i386 stack external rewrite");
}

bool testI386AlignedStackPointerOutgoingArgIsBound() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-aligned-stack-outgoing", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i32 = llvm::Type::getInt32Ty(context);
  llvm::GlobalVariable *esp = createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *calleeType = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "puts", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "i386_aligned_stack_outgoing", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *espEntry = loadRegister(builder, esp, "ESP", "esp.entry", 4);
  llvm::Value *alignedEsp = builder.CreateAnd(
      espEntry, llvm::ConstantInt::get(i32, -16, true), "esp.aligned");
  storeRegister(builder, esp, alignedEsp, "ESP", 4);
  llvm::Value *argPointer =
      builder.CreateIntToPtr(alignedEsp, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(i32, 42), argPointer);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenFunction =
      module.getFunction("i386_aligned_stack_outgoing");
  llvm::CallInst *rewritten = nullptr;
  if (rewrittenFunction != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() != nullptr &&
          call->getCalledFunction()->getName() == "puts") {
        rewritten = call;
      }
    }
  }
  auto *constant =
      rewritten == nullptr || rewritten->arg_size() != 1
          ? nullptr
          : llvm::dyn_cast<llvm::ConstantInt>(rewritten->getArgOperand(0));
  return expect(rewritten != nullptr,
                "i386 aligned stack external call missing") &&
         expect(rewritten->arg_size() == 1,
                "i386 aligned stack external did not use one stack arg") &&
         expect(constant != nullptr && constant->getZExtValue() == 42,
                "i386 aligned stack arg did not bind the store") &&
         expect(summary.CallArgStoresMarked == 1,
                "i386 aligned stack arg store was not marked") &&
         verifyOk(module,
                  "module failed verifier after aligned stack argument test");
}

bool testI386UnknownExternalDoesNotReusePreviousStackArgs() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-stack-evidence-lifetime", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i32 = llvm::Type::getInt32Ty(context);
  llvm::GlobalVariable *esp = createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *calleeType = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *first = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "first_unknown", module);
  llvm::Function *second = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "second_unknown", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "i386_stack_evidence_lifetime", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *espEntry = loadRegister(builder, esp, "ESP", "esp.entry", 4);
  llvm::Value *alignedEsp = builder.CreateAnd(
      espEntry, llvm::ConstantInt::get(i32, -16, true), "esp.aligned");
  storeRegister(builder, esp, alignedEsp, "ESP", 4);
  llvm::Value *slot0 =
      builder.CreateIntToPtr(alignedEsp, llvm::PointerType::get(context, 0));
  llvm::Value *slot4Address = builder.CreateAdd(
      alignedEsp, llvm::ConstantInt::get(i32, 4), "stack.slot4.addr");
  llvm::Value *slot4 =
      builder.CreateIntToPtr(slot4Address, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(i32, 11), slot0);
  builder.CreateStore(llvm::ConstantInt::get(i32, 22), slot4);
  builder.CreateCall(calleeType, first);
  builder.CreateStore(llvm::ConstantInt::get(i32, 33), slot0);
  builder.CreateCall(calleeType, second);
  builder.CreateRetVoid();

  (void)notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenFunction =
      module.getFunction("i386_stack_evidence_lifetime");
  llvm::CallInst *firstCall = nullptr;
  llvm::CallInst *secondCall = nullptr;
  if (rewrittenFunction != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call == nullptr || call->getCalledFunction() == nullptr) {
        continue;
      }
      if (call->getCalledFunction()->getName() == "first_unknown") {
        firstCall = call;
      } else if (call->getCalledFunction()->getName() == "second_unknown") {
        secondCall = call;
      }
    }
  }
  return expect(firstCall != nullptr && firstCall->arg_size() == 2,
                "first unknown external did not retain its two stack args") &&
         expect(secondCall != nullptr && secondCall->arg_size() == 1,
                "second unknown external reused a stale stack arg") &&
         verifyOk(module,
                  "module failed verifier after stack evidence lifetime test");
}

bool testI386NativeFrameOutgoingStackArgIsBound() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-native-frame-outgoing-stack", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i8 = llvm::Type::getInt8Ty(context);
  auto *i32 = llvm::Type::getInt32Ty(context);
  (void)createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *calleeType = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "puts", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "i386_native_frame_outgoing", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  auto *stackType = llvm::ArrayType::get(i8, 16);
  llvm::AllocaInst *stack =
      builder.CreateAlloca(stackType, nullptr, "notdec_stack.native");
  llvm::Value *basePointer = builder.CreateInBoundsGEP(
      i8, stack, llvm::ConstantInt::get(i32, 12), "native.arg.base");
  llvm::Value *baseInteger =
      builder.CreatePtrToInt(basePointer, i32, "native.arg.base.int");
  llvm::Value *argAddress = builder.CreateSub(
      baseInteger, llvm::ConstantInt::get(i32, 12), "native.arg.addr");
  llvm::Value *argPointer =
      builder.CreateIntToPtr(argAddress, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(i32, 55), argPointer);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenFunction =
      module.getFunction("i386_native_frame_outgoing");
  if (!expect(rewrittenFunction != nullptr,
              "i386 native frame outgoing caller missing")) {
    return false;
  }
  llvm::CallInst *rewritten = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName() == "puts") {
      rewritten = call;
    }
  }
  auto *constant =
      rewritten == nullptr || rewritten->arg_size() != 1
          ? nullptr
          : llvm::dyn_cast<llvm::ConstantInt>(rewritten->getArgOperand(0));
  return expect(rewritten != nullptr,
                "i386 native frame outgoing call missing") &&
         expect(rewritten->arg_size() == 1,
                "i386 native frame outgoing arg was not rewritten") &&
         expect(constant != nullptr && constant->getZExtValue() == 55,
                "i386 native frame outgoing arg did not bind the store") &&
         expect(summary.CallArgStoresMarked == 1,
                "i386 native frame outgoing store was not marked") &&
         verifyOk(module,
                  "module failed verifier after native frame outgoing rewrite");
}

bool testI386NativeFrameLoadDoesNotBecomeStackInput() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-native-frame-load", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i8 = llvm::Type::getInt8Ty(context);
  auto *i32 = llvm::Type::getInt32Ty(context);
  (void)createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *type = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "i386_native_frame_load", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  auto *stackType = llvm::ArrayType::get(i8, 16);
  llvm::AllocaInst *stack =
      builder.CreateAlloca(stackType, nullptr, "notdec_stack.native");
  llvm::Value *slot =
      builder.CreateInBoundsGEP(i8, stack, llvm::ConstantInt::get(i32, 0));
  llvm::LoadInst *localValue = builder.CreateLoad(i32, slot);
  builder.CreateRet(localValue);

  (void)notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenFunction =
      module.getFunction("i386_native_frame_load");
  return expect(rewrittenFunction != nullptr,
                "i386 native frame load function missing") &&
         expect(rewrittenFunction->arg_empty(),
                "native frame load was misclassified as stack input") &&
         verifyOk(module,
                  "module failed verifier after native frame load test");
}

bool testX64KnownExternalUsesCspecStackOverflowOffset() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-x64-stack-known-external", context);
  module.setDataLayout("e-p:64:64");
  attachX64StackTestAbi(module, 16);
  auto *i64 = llvm::Type::getInt64Ty(context);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  (void)createRegisterGlobal(module, "RAX");
  std::vector<llvm::GlobalVariable *> regs;
  for (const char *name : {"RDI", "RSI", "RDX", "RCX", "R8", "R9"}) {
    regs.push_back(createRegisterGlobal(module, name));
  }

  auto *calleeType = llvm::FunctionType::get(i64, {}, false);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "getnameinfo", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "x64_stack_known", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  for (auto [index, reg] : llvm::enumerate(regs)) {
    storeRegister(builder, reg, llvm::ConstantInt::get(i64, index + 1),
                  reg->getName().str());
  }
  llvm::LoadInst *rspEntry = loadRegister(builder, rsp, "RSP", "rsp.entry");
  llvm::Value *argAddress =
      builder.CreateAdd(rspEntry, llvm::ConstantInt::get(i64, 8), "arg.addr");
  llvm::Value *argPointer =
      builder.CreateIntToPtr(argAddress, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(i64, 77), argPointer);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  (void)notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenFunction = module.getFunction("x64_stack_known");
  if (!expect(rewrittenFunction != nullptr,
              "x64 stack external caller missing")) {
    return false;
  }
  llvm::CallInst *rewritten = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName() == "getnameinfo") {
      rewritten = call;
    }
  }
  auto *constant =
      rewritten == nullptr || rewritten->arg_size() != 7
          ? nullptr
          : llvm::dyn_cast<llvm::ConstantInt>(rewritten->getArgOperand(6));
  return expect(rewritten != nullptr, "x64 stack external call missing") &&
         expect(rewritten->arg_size() == 7,
                "x64 stack overflow arg was not rewritten") &&
         expect(constant != nullptr && constant->getZExtValue() == 77,
                "x64 stack overflow arg did not use cspec offset") &&
         verifyOk(module,
                  "module failed verifier after x64 stack external rewrite");
}

bool testX64AlignedStackPointerOutgoingArgIsBound() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-x64-aligned-stack-outgoing", context);
  module.setDataLayout("e-p:64:64");
  attachX64StackTestAbi(module, 16);
  auto *i64 = llvm::Type::getInt64Ty(context);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  (void)createRegisterGlobal(module, "RAX");
  std::vector<llvm::GlobalVariable *> regs;
  for (const char *name : {"RDI", "RSI", "RDX", "RCX", "R8", "R9"}) {
    regs.push_back(createRegisterGlobal(module, name));
  }

  auto *calleeType = llvm::FunctionType::get(i64, {}, false);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "getnameinfo", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "x64_aligned_stack_outgoing", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  for (auto [index, reg] : llvm::enumerate(regs)) {
    storeRegister(builder, reg, llvm::ConstantInt::get(i64, index + 1),
                  reg->getName().str());
  }
  llvm::LoadInst *rspEntry = loadRegister(builder, rsp, "RSP", "rsp.entry");
  llvm::Value *alignedRsp = builder.CreateAnd(
      rspEntry, llvm::ConstantInt::get(i64, -16, true), "rsp.aligned");
  storeRegister(builder, rsp, alignedRsp, "RSP");
  llvm::Value *argAddress =
      builder.CreateAdd(alignedRsp, llvm::ConstantInt::get(i64, 8), "arg.addr");
  llvm::Value *argPointer =
      builder.CreateIntToPtr(argAddress, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(i64, 77), argPointer);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  (void)notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenFunction =
      module.getFunction("x64_aligned_stack_outgoing");
  llvm::CallInst *rewritten = nullptr;
  if (rewrittenFunction != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() != nullptr &&
          call->getCalledFunction()->getName() == "getnameinfo") {
        rewritten = call;
      }
    }
  }
  auto *constant =
      rewritten == nullptr || rewritten->arg_size() != 7
          ? nullptr
          : llvm::dyn_cast<llvm::ConstantInt>(rewritten->getArgOperand(6));
  return expect(rewritten != nullptr,
                "x64 aligned stack external call missing") &&
         expect(rewritten->arg_size() == 7,
                "x64 aligned stack overflow arg was not rewritten") &&
         expect(constant != nullptr && constant->getZExtValue() == 77,
                "x64 aligned stack arg did not bind the store") &&
         verifyOk(module,
                  "module failed verifier after aligned x64 stack rewrite");
}

bool testI386InternalStackInputIsRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-internal-stack", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i32 = llvm::Type::getInt32Ty(context);
  llvm::GlobalVariable *esp = createRegisterGlobal(module, "ESP", i32, 0, 4);
  llvm::GlobalVariable *eax = createRegisterGlobal(module, "EAX", i32, 0, 4);
  auto *eaxSink = new llvm::GlobalVariable(
      module, i32, false, llvm::GlobalValue::ExternalLinkage, nullptr,
      "stack_caller_eax_sink");

  auto *calleeType = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "stack_callee", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  llvm::LoadInst *calleeEsp =
      loadRegister(calleeBuilder, esp, "ESP", "callee.esp", 4);
  llvm::Value *loadAddress = calleeBuilder.CreateAdd(
      calleeEsp, llvm::ConstantInt::get(i32, 4), "arg.addr");
  llvm::Value *loadPointer = calleeBuilder.CreateIntToPtr(
      loadAddress, llvm::PointerType::get(context, 0));
  llvm::LoadInst *stackArg = calleeBuilder.CreateLoad(i32, loadPointer);
  calleeBuilder.CreateRet(stackArg);

  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *caller = llvm::Function::Create(
      callerType, llvm::GlobalValue::ExternalLinkage, "stack_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> callerBuilder(callerEntry);
  llvm::LoadInst *callerEsp =
      loadRegister(callerBuilder, esp, "ESP", "caller.esp", 4);
  llvm::Value *storePointer = callerBuilder.CreateIntToPtr(
      callerEsp, llvm::PointerType::get(context, 0));
  callerBuilder.CreateStore(llvm::ConstantInt::get(i32, 123), storePointer);
  callerBuilder.CreateCall(calleeType, callee);
  // caller 要求 EAX 返回，但 callee 没有本地 EAX 定义。返回绑定会查入口
  // 参数，此时必须安全跳过 Unit == nullptr 的栈参数槽。
  llvm::LoadInst *result =
      loadRegister(callerBuilder, eax, "EAX", "caller.result", 4);
  callerBuilder.CreateStore(result, eaxSink);
  callerBuilder.CreateRetVoid();

  (void)notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenCallee = module.getFunction("stack_callee");
  llvm::Function *rewrittenCaller = module.getFunction("stack_caller");
  llvm::CallInst *rewrittenCall = nullptr;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() == rewrittenCallee) {
        rewrittenCall = call;
      }
    }
  }
  auto *constant =
      rewrittenCall == nullptr || rewrittenCall->arg_size() != 1
          ? nullptr
          : llvm::dyn_cast<llvm::ConstantInt>(rewrittenCall->getArgOperand(0));
  return expect(rewrittenCallee != nullptr,
                "i386 internal stack callee missing") &&
         expect(rewrittenCallee->arg_size() == 1,
                "i386 internal stack callee was not given an arg") &&
         expect(rewrittenCall != nullptr && rewrittenCall->arg_size() == 1,
                "i386 internal stack call was not rewritten") &&
         expect(constant != nullptr && constant->getZExtValue() == 123,
                "i386 internal stack arg did not bind caller store") &&
         verifyOk(module,
                  "module failed verifier after i386 internal stack rewrite");
}

bool testTrimmedInternalStackInputDoesNotLeaveBindingWarning() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-trimmed-internal-stack-warning", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i32 = llvm::Type::getInt32Ty(context);
  llvm::GlobalVariable *esp = createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);
  auto *sink = new llvm::GlobalVariable(module, i32, false,
                                        llvm::GlobalValue::ExternalLinkage,
                                        nullptr, "trimmed_stack_input_sink");

  // The callee initially has a stack+4 input, but the only caller has no
  // outgoing store for it.  Final shape refinement must remove that candidate
  // without preserving the collection phase's missing-binding warning.
  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "trimmed_stack_input_callee", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(calleeEntry);
  llvm::Value *calleeEsp = loadRegister(builder, esp, "ESP", "callee.esp", 4);
  llvm::Value *argAddress =
      builder.CreateAdd(calleeEsp, llvm::ConstantInt::get(i32, 4), "arg.addr");
  llvm::Value *argPointer =
      builder.CreateIntToPtr(argAddress, llvm::PointerType::get(context, 0));
  builder.CreateStore(builder.CreateLoad(i32, argPointer), sink);
  builder.CreateRetVoid();

  llvm::Function *caller =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "trimmed_stack_input_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  builder.CreateCall(voidType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenCallee =
      module.getFunction("trimmed_stack_input_callee");
  llvm::CallInst *rewrittenCall = nullptr;
  if (llvm::Function *rewrittenCaller =
          module.getFunction("trimmed_stack_input_caller")) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() == rewrittenCallee) {
        rewrittenCall = call;
      }
    }
  }
  bool hasCallArgWarning = std::any_of(
      summary.Warnings.begin(), summary.Warnings.end(),
      [](const notdec::bin2llvm::NativeRegisterEliminationWarning &warning) {
        return warning.CalleeName == "trimmed_stack_input_callee" &&
               warning.Kind == "call_arg";
      });

  return expect(rewrittenCallee != nullptr,
                "trimmed internal stack callee missing") &&
         expect(rewrittenCallee->arg_empty(),
                "unbound internal stack input was not trimmed") &&
         expect(rewrittenCall != nullptr && rewrittenCall->arg_empty(),
                "trimmed internal stack call kept an argument") &&
         expect(!hasCallArgWarning,
                "trimmed internal stack input left a call-arg warning") &&
         verifyOk(module,
                  "module failed verifier after stack warning refinement");
}

bool testI386AdjustedEspStackInputIsRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-adjusted-esp-stack", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i32 = llvm::Type::getInt32Ty(context);
  llvm::GlobalVariable *esp = createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *calleeType = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "adjusted_stack_callee", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::BasicBlock *calleeBody =
      llvm::BasicBlock::Create(context, "body", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  llvm::LoadInst *entryEsp =
      loadRegister(calleeBuilder, esp, "ESP", "callee.entry_esp", 4);
  llvm::Value *adjustedEsp = calleeBuilder.CreateSub(
      entryEsp, llvm::ConstantInt::get(i32, 20), "callee.esp.sub");
  storeRegister(calleeBuilder, esp, adjustedEsp, "ESP", 4);
  calleeBuilder.CreateBr(calleeBody);
  calleeBuilder.SetInsertPoint(calleeBody);
  llvm::LoadInst *currentEsp =
      loadRegister(calleeBuilder, esp, "ESP", "callee.current_esp", 4);
  // The prologue made ESP entry-20, so this is the ABI's entry-ESP+4 slot.
  llvm::Value *argAddress = calleeBuilder.CreateAdd(
      currentEsp, llvm::ConstantInt::get(i32, 24), "callee.arg.addr");
  llvm::Value *argPointer = calleeBuilder.CreateIntToPtr(
      argAddress, llvm::PointerType::get(context, 0));
  llvm::LoadInst *stackArg = calleeBuilder.CreateLoad(i32, argPointer);
  calleeBuilder.CreateRet(stackArg);

  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *caller =
      llvm::Function::Create(callerType, llvm::GlobalValue::ExternalLinkage,
                             "adjusted_stack_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> callerBuilder(callerEntry);
  llvm::LoadInst *callerEsp =
      loadRegister(callerBuilder, esp, "ESP", "caller.esp", 4);
  llvm::Value *storePointer = callerBuilder.CreateIntToPtr(
      callerEsp, llvm::PointerType::get(context, 0));
  callerBuilder.CreateStore(llvm::ConstantInt::get(i32, 123), storePointer);
  callerBuilder.CreateCall(calleeType, callee);
  callerBuilder.CreateRetVoid();

  (void)notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenCallee = module.getFunction("adjusted_stack_callee");
  llvm::Function *rewrittenCaller = module.getFunction("adjusted_stack_caller");
  llvm::CallInst *rewrittenCall = nullptr;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() == rewrittenCallee) {
        rewrittenCall = call;
      }
    }
  }
  auto *constant =
      rewrittenCall == nullptr || rewrittenCall->arg_size() != 1
          ? nullptr
          : llvm::dyn_cast<llvm::ConstantInt>(rewrittenCall->getArgOperand(0));
  return expect(rewrittenCallee != nullptr,
                "adjusted i386 stack callee missing") &&
         expect(rewrittenCallee->arg_size() == 1,
                "adjusted i386 stack callee was not given one arg") &&
         expect(rewrittenCallee->getArg(0)->getName() == "stack+4.arg",
                "adjusted i386 ESP did not resolve to stack+4") &&
         expect(rewrittenCall != nullptr && rewrittenCall->arg_size() == 1,
                "adjusted i386 stack call was not rewritten") &&
         expect(constant != nullptr && constant->getZExtValue() == 123,
                "adjusted i386 stack arg did not bind caller store") &&
         verifyOk(module,
                  "module failed verifier after adjusted i386 stack rewrite");
}

bool testI386NegativeEspLoadDoesNotBecomeStackInput() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-negative-esp-local", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i32 = llvm::Type::getInt32Ty(context);
  llvm::GlobalVariable *esp = createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *type = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "negative_esp_local", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *entryEsp = loadRegister(builder, esp, "ESP", "entry.esp", 4);
  llvm::Value *adjustedEsp =
      builder.CreateSub(entryEsp, llvm::ConstantInt::get(i32, 32), "esp.sub");
  storeRegister(builder, esp, adjustedEsp, "ESP", 4);
  llvm::LoadInst *currentEsp =
      loadRegister(builder, esp, "ESP", "current.esp", 4);
  llvm::Value *localAddress = builder.CreateAdd(
      currentEsp, llvm::ConstantInt::get(i32, 8), "local.addr");
  llvm::Value *localPointer =
      builder.CreateIntToPtr(localAddress, llvm::PointerType::get(context, 0));
  llvm::LoadInst *localValue = builder.CreateLoad(i32, localPointer);
  builder.CreateRet(localValue);

  (void)notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("negative_esp_local");
  return expect(rewritten != nullptr, "negative ESP local function missing") &&
         expect(rewritten->arg_empty(),
                "negative ESP local load became a stack parameter") &&
         verifyOk(module,
                  "module failed verifier after negative ESP local test");
}

bool testI386AlignedEspLoadDoesNotBecomeStackInput() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-aligned-esp-local", context);
  module.setDataLayout("e-p:32:32");
  attachI386StackTestAbi(module);
  auto *i32 = llvm::Type::getInt32Ty(context);
  llvm::GlobalVariable *esp = createRegisterGlobal(module, "ESP", i32, 0, 4);
  (void)createRegisterGlobal(module, "EAX", i32, 0, 4);

  auto *type = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "aligned_esp_local", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *entryEsp = loadRegister(builder, esp, "ESP", "entry.esp", 4);
  llvm::Value *alignedEsp = builder.CreateAnd(
      entryEsp, llvm::ConstantInt::get(i32, -16, true), "esp.aligned");
  storeRegister(builder, esp, alignedEsp, "ESP", 4);
  llvm::LoadInst *currentEsp =
      loadRegister(builder, esp, "ESP", "current.esp", 4);
  llvm::Value *localAddress = builder.CreateAdd(
      currentEsp, llvm::ConstantInt::get(i32, 4), "local.addr");
  llvm::Value *localPointer =
      builder.CreateIntToPtr(localAddress, llvm::PointerType::get(context, 0));
  llvm::LoadInst *localValue = builder.CreateLoad(i32, localPointer);
  builder.CreateRet(localValue);

  (void)notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("aligned_esp_local");
  return expect(rewritten != nullptr, "aligned ESP local function missing") &&
         expect(rewritten->arg_empty(),
                "aligned ESP local load became a stack parameter") &&
         verifyOk(module,
                  "module failed verifier after aligned ESP local test");
}

bool testX64AlignedRspLoadDoesNotBecomeStackInput() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-x64-aligned-rsp-local", context);
  module.setDataLayout("e-p:64:64");
  attachX64StackTestAbi(module);
  auto *i64 = llvm::Type::getInt64Ty(context);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  (void)createRegisterGlobal(module, "RAX");

  auto *type = llvm::FunctionType::get(i64, {}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "aligned_rsp_local", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *entryRsp = loadRegister(builder, rsp, "RSP", "entry.rsp");
  llvm::Value *alignedRsp = builder.CreateAnd(
      entryRsp, llvm::ConstantInt::get(i64, -16, true), "rsp.aligned");
  storeRegister(builder, rsp, alignedRsp, "RSP");
  llvm::LoadInst *currentRsp =
      loadRegister(builder, rsp, "RSP", "current.rsp");
  llvm::Value *localAddress = builder.CreateAdd(
      currentRsp, llvm::ConstantInt::get(i64, 8), "local.addr");
  llvm::Value *localPointer =
      builder.CreateIntToPtr(localAddress, llvm::PointerType::get(context, 0));
  llvm::LoadInst *localValue = builder.CreateLoad(i64, localPointer);
  builder.CreateRet(localValue);

  (void)notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("aligned_rsp_local");
  return expect(rewritten != nullptr, "aligned RSP local function missing") &&
         expect(rewritten->arg_empty(),
                "aligned RSP local load became a stack parameter") &&
         verifyOk(module,
                  "module failed verifier after aligned RSP local test");
}

bool testKnownErrnoLocationReturnUsesI386DataLayout() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-errno-location-i386-return", context);
  module.setDataLayout("e-p:32:32");
  attachI386TestAbi(module);
  llvm::GlobalVariable *eax = createRegisterGlobal(
      module, "EAX", llvm::Type::getInt32Ty(context), 0, 4);

  auto *voidCalleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(voidCalleeType, llvm::GlobalValue::ExternalLinkage,
                             "__errno_location", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getInt32Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "errno_location_i386_return_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(voidCalleeType, callee);
  llvm::LoadInst *eaxLoad = loadRegister(builder, eax, "EAX", "eax.after", 4);
  builder.CreateRet(eaxLoad);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("__errno_location");
  llvm::CallInst *call = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (candidate != nullptr && candidate->getCalledFunction() == rewritten) {
      call = candidate;
    }
  }

  return expect(rewritten != nullptr, "i386 errno_location external missing") &&
         expect(rewritten->getReturnType()->isIntegerTy(32),
                "i386 errno_location did not use pointer-width return") &&
         expect(call != nullptr, "i386 errno_location call missing") &&
         expect(call->getType()->isIntegerTy(32),
                "i386 errno_location call did not use pointer-width return") &&
         expect(summary.Warnings.empty(),
                "i386 errno_location left register SSA warning") &&
         verifyOk(module,
                  "module failed verifier after i386 errno_location rewrite");
}

bool testCallArgUsesPartialRangeRead() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-call-arg-partial-range", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "external_callee", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "call_arg_partial_range", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);

  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rdi->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  builder.CreateCall(
      partialWrite,
      {rdi, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 42),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  function = module.getFunction("call_arg_partial_range");
  llvm::CallInst *call = nullptr;
  unsigned i64Phis = 0;
  if (function == nullptr) {
    return expect(false, "partial range argument function missing");
  }
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      if (candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() == "external_callee") {
        call = candidate;
      }
    }
    if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&inst)) {
      if (phi->getType()->isIntegerTy(64)) {
        ++i64Phis;
      }
    }
  }

  return expect(call != nullptr, "partial range argument call missing") &&
         expect(call->arg_size() == 1,
                "partial range argument call was not rewritten") &&
         expect(call->getArgOperand(0)->getType()->isIntegerTy(64),
                "partial range argument kept wrong type") &&
         expect(!hasPartialWriteCall(*function),
                "partial range argument left partial write helper") &&
         expect(i64Phis == 0,
                "partial range argument created whole-register phi") &&
         expect(summary.CallsRewritten >= 1,
                "partial range argument call was not counted as rewritten") &&
         verifyOk(module,
                  "module failed verifier after partial range argument test");
}

bool testStaticRspStackRewriteKeepsSavedRegisterEvidence() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-saved-register", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "stack_save_rbx", module);
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
  const auto *rbxSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RBX");

  return expect(stackSummary.AccessesRewritten >= 1,
                "RSP stack save was not localized") &&
         expect(stackSummary.IgnoredRegisters.count("RSP") != 0,
                "RSP was not marked ignored after stack rewrite") &&
         expect(hasNamedAlloca(*function, "notdec_stack.native"),
                "native stack alloca was not created") &&
         expect(rbxSummary != nullptr,
                "missing RBX summary after stack rewrite") &&
         expect(rbxSummary->MayEntry && !rbxSummary->MayNonEntry,
                "saved RBX was not preserved through native stack alloca") &&
         verifyOk(module, "module failed verifier after stack rewrite test");
}

bool testCallerSavedEntryStackSpillCountsAsReadEntry() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-caller-saved-stack-spill", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "caller_saved_stack_spill", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  auto *stackType = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 16);
  llvm::AllocaInst *stack =
      builder.CreateAlloca(stackType, nullptr, "notdec_stack.native");
  llvm::Value *slot = builder.CreateInBoundsGEP(
      llvm::Type::getInt8Ty(context), stack,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0));
  llvm::LoadInst *entryValue = loadRegister(builder, rdi, "RDI", "rdi.entry");
  builder.CreateStore(entryValue, slot);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module);
  const auto *fn = functionSummary(summary, "caller_saved_stack_spill");
  const auto *rdiSummary =
      fn == nullptr ? nullptr : registerSummary(*fn, "RDI");

  return expect(rdiSummary != nullptr,
                "missing RDI summary for caller-saved stack spill") &&
         expect(rdiSummary->ReadEntry,
                "caller-saved stack spill did not count as ReadEntry") &&
         verifyOk(module,
                  "module failed verifier after caller-saved spill test");
}

bool testFramePointerLoadFeedsStackRewriteWithoutGlobalIgnore() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-frame-pointer-stack", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  llvm::GlobalVariable *rbp = createRegisterGlobal(module, "RBP");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "rbp_frame_stack", module);
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
         expect(
             summary.IgnoredRegisters.count("RBP") == 0,
             "local RBP frame-base match incorrectly ignored RBP globally") &&
         expect(hasNamedAlloca(*function, "notdec_stack.native"),
                "frame-pointer stack alloca was not created") &&
         verifyOk(module,
                  "module failed verifier after RBP stack rewrite test");
}

bool testFramePointerRewriteDoesNotHideRbpRegisterFlow() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-local-frame-pointer-ignore", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  llvm::GlobalVariable *rbp = createRegisterGlobal(module, "RBP");
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *frameFunction = llvm::Function::Create(
      type, llvm::GlobalValue::InternalLinkage, "local_rbp_frame", module);
  llvm::BasicBlock *frameEntry =
      llvm::BasicBlock::Create(context, "entry", frameFunction);
  llvm::IRBuilder<> frameBuilder(frameEntry);
  llvm::LoadInst *rspEntry =
      loadRegister(frameBuilder, rsp, "RSP", "rsp.entry");
  llvm::Value *frameBase = frameBuilder.CreateAdd(
      rspEntry, llvm::ConstantInt::get(rsp->getValueType(), -32, true));
  storeRegister(frameBuilder, rbp, frameBase, "RBP");
  llvm::LoadInst *rbpLoad = loadRegister(frameBuilder, rbp, "RBP", "rbp.frame");
  llvm::Value *slotAddress = frameBuilder.CreateAdd(
      rbpLoad, llvm::ConstantInt::get(rbp->getValueType(), -8, true));
  llvm::Value *slot = frameBuilder.CreateIntToPtr(
      slotAddress, llvm::PointerType::get(context, 0));
  frameBuilder.CreateStore(llvm::ConstantInt::get(rbp->getValueType(), 7),
                           slot);
  frameBuilder.CreateRetVoid();

  llvm::Function *registerFunction = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "rbp_register_flow", module);
  llvm::BasicBlock *registerEntry =
      llvm::BasicBlock::Create(context, "entry", registerFunction);
  llvm::IRBuilder<> registerBuilder(registerEntry);
  llvm::LoadInst *rdiEntry =
      loadRegister(registerBuilder, rdi, "RDI", "rdi.entry");
  storeRegister(registerBuilder, rbp, rdiEntry, "RBP");
  registerBuilder.CreateRetVoid();

  auto stackSummary = notdec::bin2llvm::runNativeStackFrameRewrite(module);
  notdec::bin2llvm::NativeRegisterSummaryOptions options;
  options.IgnoredRegisters = stackSummary.IgnoredRegisters;
  auto summary = notdec::bin2llvm::runNativeRegisterSummary(module, options);
  const auto *functionSummary = ::functionSummary(summary, "rbp_register_flow");
  const auto *rdiSummary = functionSummary == nullptr
                               ? nullptr
                               : registerSummary(*functionSummary, "RDI");

  return expect(stackSummary.FramePointerLoadsReplaced == 1,
                "local RBP frame load was not replaced") &&
         expect(stackSummary.IgnoredRegisters.count("RBP") == 0,
                "local RBP frame use leaked into module ignored registers") &&
         expect(rdiSummary != nullptr,
                "missing RDI summary after local RBP frame rewrite") &&
         expect(rdiSummary->ReadEntry,
                "RDI entry flow through ordinary RBP store was hidden") &&
         verifyOk(module, "module failed verifier after local RBP ignore test");
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

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  (void)summary;
  return expect(!hasStoreInstruction(*function),
                "dead stack-frame store remained in final IR") &&
         verifyOk(module,
                  "module failed verifier after stack-frame cleanup test");
}

bool testNativeFrameContentStoreSurvivesSummarySSA() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-native-frame-keep-alive", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");

  auto *calleeType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "frame_keep_alive_callee", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "frame_keep_alive", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *rspEntry = loadRegister(builder, rsp, "RSP", "rsp.entry");
  llvm::Value *slotAddress = builder.CreateAdd(
      rspEntry, llvm::ConstantInt::get(rsp->getValueType(), -8, true));
  llvm::Value *slot =
      builder.CreateIntToPtr(slotAddress, llvm::PointerType::get(context, 0));
  builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 12345), slot);
  builder.CreateCall(
      callee, {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  (void)summary;
  bool hasKeepCall = false;
  bool contentStoreKept = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      llvm::Function *called = call->getCalledFunction();
      hasKeepCall |= called != nullptr &&
                     called->getName() == "notdec.register.native_frame.keep";
      continue;
    }
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    auto *stored =
        store == nullptr
            ? nullptr
            : llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
    if (stored != nullptr && stored->getZExtValue() == 12345) {
      contentStoreKept = true;
    }
  }

  return expect(hasKeepCall,
                "native frame was not marked address-taken for the pipeline") &&
         expect(contentStoreKept,
                "content store into a possibly-escaping frame was removed") &&
         verifyOk(module, "module failed verifier after frame keep-alive test");
}

bool testStackFrameAddressPassedToCallIsLocalized() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-frame-call-arg", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");

  auto *calleeType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
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
         expect(
             llvm::isa<llvm::PtrToIntInst>(call->getArgOperand(0)),
             "stack call argument was not rewritten through native alloca") &&
         verifyOk(module,
                  "module failed verifier after stack call arg rewrite");
}

bool testLateStackCleanupReusesExistingNativeFrameAlloca() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-late-stack-cleanup-reuse", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "late_stack_sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage,
      "late_stack_cleanup_reuse", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);

  auto *stackType = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 216);
  llvm::AllocaInst *storage =
      builder.CreateAlloca(stackType, nullptr, "notdec_stack.native");
  storage->setAlignment(llvm::Align(16));
  llvm::Value *existingSlot = builder.CreateInBoundsGEP(
      llvm::Type::getInt8Ty(context), storage,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 8));
  builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 7),
                      existingSlot);

  llvm::LoadInst *rspEntry = loadRegister(builder, rsp, "RSP", "rsp.entry");
  llvm::Value *slotAddress = builder.CreateAdd(
      rspEntry, llvm::ConstantInt::get(rsp->getValueType(), -208, true));
  llvm::Value *slot =
      builder.CreateIntToPtr(slotAddress, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(rsp->getValueType(), 11), slot);
  llvm::LoadInst *loaded =
      builder.CreateLoad(rsp->getValueType(), existingSlot);
  builder.CreateStore(loaded, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeStackFrameCleanupOptions options;
  options.StackPointerRegister = "RSP";
  options.Registers = {"RSP"};
  auto summary =
      notdec::bin2llvm::runNativeStackFrameCleanup(module, options);

  unsigned allocaCount = 0;
  bool rawStoreUsesExistingSlot = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst)) {
      allocaCount += alloca->hasName() &&
                     alloca->getName().starts_with("notdec_stack.native");
      continue;
    }
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    if (store == nullptr) {
      continue;
    }
    auto *stored = llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
    if (stored != nullptr && stored->getZExtValue() == 11 &&
        store->getPointerOperand()->stripPointerCasts() == existingSlot) {
      rawStoreUsesExistingSlot = true;
    }
  }

  return expect(summary.AccessesRewritten >= 1,
                "late stack cleanup did not rewrite entry-SP access") &&
         expect(allocaCount == 1,
                "late stack cleanup created a second native frame alloca") &&
         expect(rawStoreUsesExistingSlot,
                "late stack cleanup did not reuse the existing frame slot") &&
         expect(rsp->use_empty(),
                "late stack cleanup left a dead RSP entry read") &&
         verifyOk(module, "module failed verifier after late stack cleanup");
}

bool testLateStackIntegerAccessReusesDominatedFrameSlot() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-late-stack-integer-reuse", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");

  auto *calleeType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "takes_stack_integer", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "late_stack_integer_reuse", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);

  // Simulate the cluster a previous rewrite left behind: the frame alloca and
  // one slot GEP at native offset -208 (frameLow -216 + 8).
  auto *stackType = llvm::ArrayType::get(llvm::Type::getInt8Ty(context), 216);
  llvm::AllocaInst *storage =
      builder.CreateAlloca(stackType, nullptr, "notdec_stack.native");
  storage->setAlignment(llvm::Align(16));
  llvm::Value *existingSlot = builder.CreateInBoundsGEP(
      llvm::Type::getInt8Ty(context), storage,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 8));
  builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 7), existingSlot);

  // A late entry-SP address consumed as an integer forces the cleanup to
  // materialize ptrtoint(slot) in the pointer cluster.
  llvm::LoadInst *rspEntry = loadRegister(builder, rsp, "RSP", "rsp.entry");
  llvm::Value *slotAddress = builder.CreateAdd(
      rspEntry, llvm::ConstantInt::get(rsp->getValueType(), -208, true));
  llvm::CallInst *call = builder.CreateCall(callee, {slotAddress});
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeStackFrameCleanupOptions options;
  options.StackPointerRegister = "RSP";
  options.Registers = {"RSP"};
  auto summary = notdec::bin2llvm::runNativeStackFrameCleanup(module, options);
  auto *integer = llvm::dyn_cast<llvm::PtrToIntInst>(call->getArgOperand(0));

  return expect(summary.AccessesRewritten >= 1,
                "late stack cleanup did not rewrite entry-SP integer access") &&
         expect(integer != nullptr &&
                    integer->getPointerOperand()->stripPointerCasts() ==
                        existingSlot->stripPointerCasts(),
                "late stack cleanup did not reuse the existing frame slot") &&
         verifyOk(module,
                  "module failed verifier after late stack integer reuse");
}

bool testStackCanaryCheckIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-remove", context);
  attachTestAbi(module);
  llvm::Function *function =
      createStackCanaryCheckFunction(module, 40, false, false);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "stack canary check was not counted as removed") &&
         expect(summary.StackCanaryFailBlocksRemoved == 1,
                "stack canary fail block was not counted as removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "stack canary fail call was kept") &&
         expect(!hasRegisterLoad(*function, "FS_OFFSET"),
                "stack canary FS_OFFSET load was kept") &&
         verifyOk(module, "module failed verifier after stack canary removal");
}

bool testI386GsStackCanaryCheckIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-i386-gs-stack-canary-remove", context);
  module.setDataLayout("e-p:32:32");
  attachI386TestAbi(module);
  llvm::Function *function = createStackCanaryCheckFunction(
      module, 20, false, false, false, false, "GS_OFFSET", 4);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "i386 GS stack canary check was not counted as removed") &&
         expect(summary.StackCanaryFailBlocksRemoved == 1,
                "i386 GS stack canary fail block was not counted as removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "i386 GS stack canary fail call was kept") &&
         expect(!hasRegisterLoad(*function, "GS_OFFSET"),
                "i386 GS stack canary GS_OFFSET load was kept") &&
         verifyOk(module,
                  "module failed verifier after i386 GS canary removal");
}

bool testDeadStackCanaryFailThunkIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-fail-thunk-remove", context);
  attachTestAbi(module);
  llvm::Function *function =
      createStackCanaryCheckFunction(module, 40, false, false);
  llvm::Function *directFail = module.getFunction("__stack_chk_fail");
  if (directFail == nullptr) {
    return expect(false, "direct stack check fail declaration missing");
  }

  auto *failType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *failThunk =
      llvm::Function::Create(failType, llvm::GlobalValue::InternalLinkage,
                             "notdec_native_4430", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", failThunk);
  llvm::IRBuilder<> thunkBuilder(entry);
  thunkBuilder.CreateCall(directFail->getFunctionType(), directFail, {});
  thunkBuilder.CreateUnreachable();

  bool rewired = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call != nullptr && call->getCalledFunction() == directFail) {
      call->setCalledFunction(failThunk->getFunctionType(), failThunk);
      rewired = true;
    }
  }
  if (!rewired) {
    return expect(false, "stack canary fail call was not rewired to thunk");
  }

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "thunk stack canary check was not removed") &&
         expect(summary.StackCanaryFailFunctionsRemoved == 1,
                "dead stack canary fail thunk was not counted as removed") &&
         expect(!moduleHasFunctionNamed(module, "notdec_native_4430"),
                "dead stack canary fail thunk was kept") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "stack canary fail thunk call was kept") &&
         verifyOk(module,
                  "module failed verifier after stack canary fail thunk test");
}

bool testStackCanaryPrologueSaveIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-prologue-save-remove", context);
  attachTestAbi(module);
  llvm::Function *function =
      createStackCanaryCheckFunction(module, 40, false, false, false, true);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "stack canary check with prologue save was not removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "stack canary fail call with prologue save was kept") &&
         expect(!hasRegisterLoad(*function, "FS_OFFSET"),
                "stack canary prologue FS_OFFSET load was kept") &&
         verifyOk(module,
                  "module failed verifier after canary prologue save removal");
}

bool testStackCanaryZextConditionIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-zext-remove", context);
  attachTestAbi(module);
  llvm::Function *function =
      createStackCanaryCheckFunction(module, 40, true, false);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "zext stack canary check was not counted as removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "zext stack canary fail call was kept") &&
         verifyOk(module,
                  "module failed verifier after zext stack canary removal");
}

bool testStackCanaryMaskedSavedLoadIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-masked-saved-remove", context);
  attachTestAbi(module);
  llvm::Function *function =
      createStackCanaryCheckFunction(module, 40, false, false, true);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "masked saved stack canary check was not counted as removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "masked saved stack canary fail call was kept") &&
         expect(!hasRegisterLoad(*function, "FS_OFFSET"),
                "masked saved stack canary FS_OFFSET load was kept") &&
         verifyOk(module,
                  "module failed verifier after masked stack canary removal");
}

bool testStackCanaryFailSideEffectIsKept() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-side-effect", context);
  attachTestAbi(module);
  llvm::Function *function =
      createStackCanaryCheckFunction(module, 40, false, true);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.StackCanaryChecksRemoved == 0,
                "side-effecting fail block was incorrectly removed") &&
         expect(hasCallTo(*function, "__stack_chk_fail"),
                "side-effecting stack canary fail call was removed") &&
         verifyOk(module,
                  "module failed verifier after side-effecting canary test");
}

bool testStackCanaryWrongFsOffsetIsKept() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-wrong-offset", context);
  attachTestAbi(module);
  llvm::Function *function =
      createStackCanaryCheckFunction(module, 32, false, false);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.StackCanaryChecksRemoved == 0,
                "non-FS:0x28 canary-like check was incorrectly removed") &&
         expect(hasCallTo(*function, "__stack_chk_fail"),
                "non-FS:0x28 stack check fail call was removed") &&
         verifyOk(module, "module failed verifier after wrong-offset test");
}

bool testRawRspStackCanaryCheckIsRemovedAfterStackCleanup() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-raw-rsp", context);
  attachTestAbi(module);
  llvm::Function *function = createRawRspStackCanaryCheckFunction(module);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "raw RSP stack canary check was not removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "raw RSP stack canary fail call was kept") &&
         expect(!hasRegisterLoad(*function, "FS_OFFSET"),
                "raw RSP stack canary FS_OFFSET load was kept") &&
         verifyOk(module,
                  "module failed verifier after raw RSP stack canary test");
}

bool testRawRspZfStackCanaryCheckIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-raw-rsp-zf", context);
  attachTestAbi(module);
  llvm::Function *function = createRawRspZfStackCanaryCheckFunction(module);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "raw RSP ZF stack canary check was not removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "raw RSP ZF stack canary fail call was kept") &&
         expect(!hasRegisterLoad(*function, "FS_OFFSET"),
                "raw RSP ZF stack canary FS_OFFSET load was kept") &&
         expect(!hasRegisterLoad(*function, "ZF"),
                "raw RSP ZF stack canary flag load was kept") &&
         verifyOk(module,
                  "module failed verifier after raw RSP ZF stack canary test");
}

bool testPhiFsBaseStackCanaryCheckIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-phi-fs", context);
  attachTestAbi(module);
  llvm::Function *function = createPhiFsBaseStackCanaryCheckFunction(module);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "PHI FS stack canary check was not removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "PHI FS stack canary fail call was kept") &&
         expect(!hasRegisterLoad(*function, "FS_OFFSET"),
                "PHI FS stack canary FS_OFFSET load was kept") &&
         verifyOk(module,
                  "module failed verifier after PHI FS stack canary test");
}

bool testZeroBaseStackCanaryCheckIsKept() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-zero-base", context);
  attachTestAbi(module);
  llvm::Function *function = createZeroBaseStackCanaryCheckFunction(module);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.StackCanaryChecksRemoved == 0,
                "zero-base stack canary check was incorrectly removed") &&
         expect(hasCallTo(*function, "__stack_chk_fail"),
                "zero-base stack canary fail call was incorrectly removed") &&
         verifyOk(module,
                  "module failed verifier after zero-base stack canary test");
}

bool testSharedFailStackCanaryChecksAreRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-shared-fail", context);
  attachTestAbi(module);
  llvm::Function *function = createSharedFailStackCanaryCheckFunction(module);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.StackCanaryChecksRemoved == 2,
                "shared-fail stack canary checks were not removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "shared-fail stack canary fail call was kept") &&
         verifyOk(module,
                  "module failed verifier after shared-fail canary test");
}

bool testMixedFailPredecessorKeepsNonCanaryFailPath() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-mixed-fail", context);
  attachTestAbi(module);
  llvm::Function *function =
      createMixedFailPredecessorStackCanaryCheckFunction(module);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "mixed-fail canary predecessor was not removed") &&
         expect(summary.StackCanaryFailBlocksRemoved == 0,
                "mixed-fail block with non-canary predecessor was removed") &&
         expect(hasCallTo(*function, "__stack_chk_fail"),
                "mixed-fail non-canary fail call was removed") &&
         expect(!hasRegisterLoad(*function, "FS_OFFSET"),
                "mixed-fail canary FS_OFFSET load was kept") &&
         verifyOk(module,
                  "module failed verifier after mixed-fail canary test");
}

bool testPhiFsCanaryAddressStackCanaryCheckIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-phi-address", context);
  attachTestAbi(module);
  llvm::Function *function =
      createPhiFsCanaryAddressStackCanaryCheckFunction(module);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "PHI FS canary address check was not removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "PHI FS canary address fail call was kept") &&
         verifyOk(module,
                  "module failed verifier after PHI canary address test");
}

bool testSelfPhiFsCanaryAddressStackCanaryCheckIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-self-phi-address", context);
  attachTestAbi(module);
  llvm::Function *function =
      createSelfPhiFsCanaryAddressStackCanaryCheckFunction(module);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "self-PHI FS canary address check was not removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "self-PHI FS canary address fail call was kept") &&
         verifyOk(module,
                  "module failed verifier after self-PHI canary address test");
}

bool testSelfPhiFsBaseStackCanaryCheckIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-canary-self-phi-base", context);
  attachTestAbi(module);
  llvm::Function *function =
      createSelfPhiFsBaseStackCanaryCheckFunction(module);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.StackCanaryChecksRemoved == 1,
                "self-PHI FS base canary check was not removed") &&
         expect(!hasCallTo(*function, "__stack_chk_fail"),
                "self-PHI FS base canary fail call was kept") &&
         expect(!hasRegisterLoad(*function, "FS_OFFSET"),
                "self-PHI FS base canary FS_OFFSET load was kept") &&
         verifyOk(module,
                  "module failed verifier after self-PHI canary base test");
}

bool testKnownSqrtlUsesLongDoubleStackAndSt0() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-sqrtl-long-double-abi", context);
  module.setDataLayout("e-p:64:64");
  attachX64StackTestAbi(module);

  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *i80 = llvm::Type::getIntNTy(context, 80);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  (void)createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *st0 =
      createRegisterGlobal(module, "ST0", i80, 0x1100, 10);
  auto *sink = new llvm::GlobalVariable(
      module, i80, false, llvm::GlobalValue::ExternalLinkage, nullptr,
      "sqrtl_result_sink");
  auto *leaveLoop = new llvm::GlobalVariable(
      module, llvm::Type::getInt1Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sqrtl_leave_loop");

  auto *oldSqrtlType = llvm::FunctionType::get(i64, {}, false);
  llvm::Function *sqrtlFunction = llvm::Function::Create(
      oldSqrtlType, llvm::GlobalValue::ExternalLinkage, "sqrtl", module);
  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *caller = llvm::Function::Create(
      callerType, llvm::GlobalValue::ExternalLinkage, "sqrtl_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", caller);
  llvm::BasicBlock *loop = llvm::BasicBlock::Create(context, "loop", caller);
  llvm::BasicBlock *tail = llvm::BasicBlock::Create(context, "tail", caller);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *rspValue = loadRegister(builder, rsp, "RSP", "rsp.entry");
  llvm::Value *frameBase = builder.CreateSub(
      rspValue, llvm::ConstantInt::get(i64, 8), "frame.base");
  builder.CreateBr(loop);

  builder.SetInsertPoint(loop);
  llvm::PHINode *loopFrame = builder.CreatePHI(i64, 2, "frame.loop");
  loopFrame->addIncoming(frameBase, entry);
  loopFrame->addIncoming(loopFrame, loop);
  llvm::Value *leave =
      builder.CreateLoad(llvm::Type::getInt1Ty(context), leaveLoop);
  builder.CreateCondBr(leave, tail, loop);

  builder.SetInsertPoint(tail);
  llvm::Value *argAddress = builder.CreateAdd(
      loopFrame, llvm::ConstantInt::get(i64, 16), "sqrtl.arg.addr");
  llvm::Value *argPointer = builder.CreateIntToPtr(
      argAddress, llvm::PointerType::get(context, 0));
  builder.CreateStore(llvm::ConstantInt::get(i80, 42), argPointer);
  llvm::Value *restoredRsp = builder.CreateAdd(
      frameBase, llvm::ConstantInt::get(i64, 8), "rsp.restored");
  storeRegister(builder, rsp, restoredRsp, "RSP");
  llvm::CallInst *sqrtlCall = builder.CreateCall(oldSqrtlType, sqrtlFunction);
  sqrtlCall->setTailCallKind(llvm::CallInst::TCK_Tail);
  llvm::Value *result = loadRegister(builder, st0, "ST0", "sqrtl.result", 10);
  builder.CreateStore(result, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenSqrtl = module.getFunction("sqrtl");
  llvm::Function *rewrittenCaller = module.getFunction("sqrtl_caller");
  llvm::CallInst *rewrittenCall = nullptr;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() == rewrittenSqrtl) {
        rewrittenCall = call;
      }
    }
  }

  bool argumentPreservesI80Bits = false;
  if (rewrittenCall != nullptr && rewrittenCall->arg_size() == 1) {
    llvm::Value *argument = rewrittenCall->getArgOperand(0);
    if (auto *bitcast = llvm::dyn_cast<llvm::BitCastInst>(argument)) {
      argumentPreservesI80Bits =
          bitcast->getOperand(0)->getType()->isIntegerTy(80);
    } else if (auto *constant = llvm::dyn_cast<llvm::ConstantFP>(argument)) {
      // InstCombine folds a constant i80 -> x86_fp80 bitcast.  Compare the
      // payload bits so the test still checks the stack value, not its spelling.
      argumentPreservesI80Bits =
          constant->getValueAPF().bitcastToAPInt() == llvm::APInt(80, 42);
    }
  }
  bool hasSqrtlWarning = std::any_of(
      summary.Warnings.begin(), summary.Warnings.end(),
      [](const notdec::bin2llvm::NativeRegisterEliminationWarning &warning) {
        return warning.CalleeName == "sqrtl";
      });

  return expect(rewrittenSqrtl != nullptr, "sqrtl declaration missing") &&
         expect(rewrittenSqrtl->getReturnType()->isX86_FP80Ty(),
                "sqrtl return was not rewritten to x86_fp80") &&
         expect(rewrittenSqrtl->arg_size() == 1 &&
                    rewrittenSqrtl->getArg(0)->getType()->isX86_FP80Ty(),
                "sqrtl parameter was not rewritten to x86_fp80") &&
         expect(rewrittenCall != nullptr &&
                    rewrittenCall->getType()->isX86_FP80Ty(),
                "sqrtl call return was not rewritten to x86_fp80") &&
         expect(rewrittenCall != nullptr && rewrittenCall->isTailCall(),
                "sqrtl call lost its tail marker") &&
         expect(argumentPreservesI80Bits,
                "sqrtl stack argument did not preserve the i80 payload") &&
         expect(rewrittenCaller != nullptr &&
                    !hasRegisterLoad(*rewrittenCaller, "ST0"),
                "sqrtl return left a raw ST0 load") &&
         expect(!hasSqrtlWarning, "sqrtl left a register SSA warning") &&
         verifyOk(module,
                  "module failed verifier after sqrtl long double rewrite");
}

bool testPlannerSplitsZmmForFloatAbiSlot() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-zmm-abi-range-planner", context);
  attachTestFloatAbi(module, 1);
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "planner_zmm_abi_slot", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  options.EnableRewrite = false;
  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module, options);
  return expect(summary.RangeSegmentsPlanned >= 2,
                "float ABI slot did not split backing ZMM range") &&
         expect(summarySsaMetadataUInt(*function, "range_segments_planned") >=
                    2,
                "float ABI slot split was not exposed in metadata") &&
         verifyOk(module, "module failed verifier after zmm planner test");
}

bool testPlannerSplitsRegisterForDemandMask() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-demand-mask-range-planner", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "planner_demand_mask_callee", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(calleeEntry);
  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 7),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateRetVoid();

  llvm::Function *caller =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "planner_demand_mask_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *after = loadRegister(builder, rax, "RAX", "after");
  llvm::Value *low =
      builder.CreateTrunc(after, llvm::Type::getInt32Ty(context), "low");
  builder.CreateStore(low, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  options.EnableRewrite = false;
  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module, options);
  return expect(summary.RangeSegmentsPlanned >= 2,
                "demand mask did not split RAX range") &&
         expect(summarySsaMetadataUInt(*callee, "range_segments_planned") >= 2,
                "demand mask split was not exposed in callee metadata") &&
         verifyOk(module, "module failed verifier after demand planner test");
}

bool testRecordedReturnValueSurvivesPartialDemandRewrite() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-return-partial-demand", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *condition = new llvm::GlobalVariable(
      module, llvm::Type::getInt1Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "condition");
  auto *sink = new llvm::GlobalVariable(module, rax->getValueType(), false,
                                        llvm::GlobalValue::ExternalLinkage,
                                        nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "return_entry_or_partial", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", callee);
  llvm::BasicBlock *early = llvm::BasicBlock::Create(context, "early", callee);
  llvm::BasicBlock *write = llvm::BasicBlock::Create(context, "write", callee);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *cond = builder.CreateLoad(condition->getValueType(), condition);
  builder.CreateCondBr(cond, early, write);

  builder.SetInsertPoint(early);
  builder.CreateRetVoid();

  builder.SetInsertPoint(write);
  llvm::Value *old = loadRegister(builder, rax, "RAX", "old");
  llvm::Value *keep = builder.CreateAnd(
      old, llvm::ConstantInt::get(rax->getValueType(),
                                  llvm::APInt::getBitsSet(64, 8, 64)));
  llvm::Value *low = llvm::ConstantInt::get(rax->getValueType(), 1);
  storeRegister(builder, rax, builder.CreateOr(keep, low), "RAX");
  builder.CreateRetVoid();

  llvm::Function *caller = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "use_partial_return", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  builder.CreateCall(type, callee);
  llvm::Value *returned = loadRegister(builder, rax, "RAX", "returned");
  builder.CreateStore(returned, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  options.EnablePostRewriteInstCombine = false;
  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module, options);
  llvm::Function *rewrittenCallee =
      module.getFunction("return_entry_or_partial");
  return expect(summary.FunctionsRewritten >= 1,
                "callee signature was not rewritten for RAX return") &&
         expect(summary.PartialDemandCandidates >= 1,
                "partial return path was not seen by demand rewrite") &&
         expect(rewrittenCallee != nullptr,
                "rewritten callee was not found") &&
         expect(!functionReturnGraphsContainPoison(*rewrittenCallee),
                "partial-demand poisoned a delayed return observation") &&
         verifyOk(module,
                  "module failed verifier after return partial-demand test");
}

bool testFramePointerLoadFoldsAcrossLoop() {
  llvm::LLVMContext context;
  llvm::Module module("frame-pointer-loop", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rbp = createRegisterGlobal(module, "RBP");
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "frame_pointer_loop", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *loop = llvm::BasicBlock::Create(context, "loop", function);
  llvm::BasicBlock *exit = llvm::BasicBlock::Create(context, "exit", function);

  llvm::IRBuilder<> builder(entry);
  // 序言：RBP = RSP（编译器里的 mov rbp, rsp）。按编译器模型，RBP 在这个
  // 函数里整函数都是帧基址。
  llvm::Value *stackPointer = loadRegister(builder, rsp, "RSP", "sp");
  storeRegister(builder, rbp, stackPointer, "RBP");
  builder.CreateBr(loop);

  // 循环体里的 load @RBP 必须折成序言存进去的值，而不是留在 IR 里当入口读。
  builder.SetInsertPoint(loop);
  llvm::Value *frame = loadRegister(builder, rbp, "RBP", "frame");
  builder.CreateStore(frame, sink);
  builder.CreateCondBr(llvm::ConstantInt::getFalse(context), loop, exit);

  builder.SetInsertPoint(exit);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeStackFrameRewrite(module);

  return expect(!hasRegisterLoad(*function, "RBP"),
                "frame pointer load in a loop was not folded") &&
         verifyOk(module,
                  "module failed verifier after frame pointer loop fold");
}

bool testStackAdjustmentPushDoesNotCreateEntryParam() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-stack-adjustment-push", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  auto *i64Type = llvm::Type::getInt64Ty(context);
  auto *pointerType = llvm::PointerType::get(context, 0);
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *voidType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *function = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "stack_adjustment_push",
      module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  // push %rcx：SP 先减 8，再把 RCX 写进新的栈顶。
  llvm::Value *stackPointer = loadRegister(builder, rsp, "RSP", "sp");
  llvm::Value *pushed =
      builder.CreateSub(stackPointer, llvm::ConstantInt::get(i64Type, 8));
  storeRegister(builder, rsp, pushed, "RSP");
  llvm::Value *pushedValue = loadRegister(builder, rcx, "RCX", "pushed");
  builder.CreateStore(pushedValue, builder.CreateIntToPtr(pushed, pointerType));
  // pop %rdx：从栈顶读出来写进另一个寄存器，然后 SP 加回 8。
  llvm::Value *popped = builder.CreateLoad(
      i64Type, builder.CreateIntToPtr(pushed, pointerType), "popped");
  storeRegister(builder, rdx, popped, "RDX");
  storeRegister(builder, rsp,
                builder.CreateAdd(pushed, llvm::ConstantInt::get(i64Type, 8)),
                "RSP");
  // 收尾处的间接调用（真机器码里是尾跳）：未知 callee 对目的寄存器（这里是 RDX）
  // 的消费只能保守当成读，这正是旧判据拒绝这个 push/pop 对的原因。
  llvm::Value *target = loadRegister(builder, rax, "RAX", "target");
  builder.CreateCall(voidType, builder.CreateIntToPtr(target, pointerType));
  builder.CreateRetVoid();

  (void)notdec::bin2llvm::runNativeStackFrameRewrite(module);
  (void)notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("stack_adjustment_push");
  bool hasRcxParam = false;
  if (rewritten != nullptr) {
    for (const llvm::Argument &arg : rewritten->args()) {
      hasRcxParam |= arg.getName().starts_with("RCX");
    }
  }
  return expect(rewritten != nullptr, "stack adjustment function missing") &&
         expect(!hasRcxParam,
                "stack adjustment push created an entry parameter") &&
         verifyOk(module,
                  "module failed verifier after stack adjustment push test");
}

bool testFramePointerLoadAfterRestoreIsKept() {
  llvm::LLVMContext context;
  llvm::Module module("frame-pointer-restore", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rbp = createRegisterGlobal(module, "RBP");
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "frame_pointer_restore",
      module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *restore =
      llvm::BasicBlock::Create(context, "restore", function);
  llvm::BasicBlock *after =
      llvm::BasicBlock::Create(context, "after", function);

  llvm::IRBuilder<> builder(entry);
  llvm::AllocaInst *slot =
      builder.CreateAlloca(llvm::Type::getInt64Ty(context), nullptr, "slot");
  llvm::Value *stackPointer = loadRegister(builder, rsp, "RSP", "sp");
  storeRegister(builder, rbp, stackPointer, "RBP");
  builder.CreateBr(restore);

  // 收尾恢复：RBP 被写回调用方的值，之后再读 RBP 就不是帧基址了。
  builder.SetInsertPoint(restore);
  llvm::Value *saved = builder.CreateLoad(llvm::Type::getInt64Ty(context),
                                          slot, "saved");
  storeRegister(builder, rbp, saved, "RBP");
  builder.CreateBr(after);

  builder.SetInsertPoint(after);
  llvm::Value *frame = loadRegister(builder, rbp, "RBP", "frame");
  builder.CreateStore(frame, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeStackFrameRewrite(module);

  return expect(hasRegisterLoad(*function, "RBP"),
                "frame pointer load after the restore was folded") &&
         verifyOk(module,
                  "module failed verifier after frame pointer restore test");
}

bool testTwoFramePointerSettersAreNotFolded() {
  llvm::LLVMContext context;
  llvm::Module module("frame-pointer-two-setters", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rbp = createRegisterGlobal(module, "RBP");
  llvm::GlobalVariable *rsp = createRegisterGlobal(module, "RSP");
  auto *condition = new llvm::GlobalVariable(
      module, llvm::Type::getInt1Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "condition");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage,
      "frame_pointer_two_setters", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right =
      llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *merge =
      llvm::BasicBlock::Create(context, "merge", function);

  // 两次"设置"（互不支配）：编译器模型里不该出现（帧指针整函数只有一次
  // 序言设置），保守起见不折任何 load。
  llvm::IRBuilder<> builder(entry);
  llvm::Value *cond =
      builder.CreateLoad(llvm::Type::getInt1Ty(context), condition, "cond");
  builder.CreateCondBr(cond, left, right);

  builder.SetInsertPoint(left);
  llvm::Value *leftSp = loadRegister(builder, rsp, "RSP", "left.sp");
  storeRegister(builder, rbp, leftSp, "RBP");
  builder.CreateBr(merge);

  builder.SetInsertPoint(right);
  llvm::Value *rightSp = loadRegister(builder, rsp, "RSP", "right.sp");
  storeRegister(builder, rbp, rightSp, "RBP");
  builder.CreateBr(merge);

  builder.SetInsertPoint(merge);
  llvm::Value *frame = loadRegister(builder, rbp, "RBP", "frame");
  builder.CreateStore(frame, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeStackFrameRewrite(module);

  return expect(hasRegisterLoad(*function, "RBP"),
                "load after two frame pointer setters was folded") &&
         verifyOk(module,
                  "module failed verifier after two setter frame test");
}

} // namespace

int main() {
  bool ok = true;
  ok &= testPhiIncomingMatchesPredecessors();
  ok &= testX87StackReturnBecomesX86Fp80ReturnSlot();
  ok &= testDeadFrameSpillPartialReadCreatesNoEntryParam();
  ok &= testLiveFrameSpillPartialReadKeepsValue();
  ok &= testDirectionFlagIsZeroAtBoundaries();
  ok &= testI386KnownExternalUsesCspecStackOffset();
  ok &= testI386AlignedStackPointerOutgoingArgIsBound();
  ok &= testI386UnknownExternalDoesNotReusePreviousStackArgs();
  ok &= testI386NativeFrameOutgoingStackArgIsBound();
  ok &= testI386NativeFrameLoadDoesNotBecomeStackInput();
  ok &= testX64KnownExternalUsesCspecStackOverflowOffset();
  ok &= testX64AlignedStackPointerOutgoingArgIsBound();
  ok &= testI386InternalStackInputIsRewritten();
  ok &= testTrimmedInternalStackInputDoesNotLeaveBindingWarning();
  ok &= testI386AdjustedEspStackInputIsRewritten();
  ok &= testI386NegativeEspLoadDoesNotBecomeStackInput();
  ok &= testI386AlignedEspLoadDoesNotBecomeStackInput();
  ok &= testX64AlignedRspLoadDoesNotBecomeStackInput();
  ok &= testKnownErrnoLocationReturnUsesI386DataLayout();
  ok &= testCallArgUsesPartialRangeRead();
  ok &= testStaticRspStackRewriteKeepsSavedRegisterEvidence();
  ok &= testCallerSavedEntryStackSpillCountsAsReadEntry();
  ok &= testFramePointerLoadFeedsStackRewriteWithoutGlobalIgnore();
  ok &= testFramePointerRewriteDoesNotHideRbpRegisterFlow();
  ok &= testSummarySSARemovesDeadStackFrameStore();
  ok &= testNativeFrameContentStoreSurvivesSummarySSA();
  ok &= testStackFrameAddressPassedToCallIsLocalized();
  ok &= testLateStackCleanupReusesExistingNativeFrameAlloca();
  ok &= testLateStackIntegerAccessReusesDominatedFrameSlot();
  ok &= testStackCanaryCheckIsRemoved();
  ok &= testI386GsStackCanaryCheckIsRemoved();
  ok &= testDeadStackCanaryFailThunkIsRemoved();
  ok &= testStackCanaryPrologueSaveIsRemoved();
  ok &= testStackCanaryZextConditionIsRemoved();
  ok &= testStackCanaryMaskedSavedLoadIsRemoved();
  ok &= testStackCanaryFailSideEffectIsKept();
  ok &= testStackCanaryWrongFsOffsetIsKept();
  ok &= testRawRspStackCanaryCheckIsRemovedAfterStackCleanup();
  ok &= testRawRspZfStackCanaryCheckIsRemoved();
  ok &= testPhiFsBaseStackCanaryCheckIsRemoved();
  ok &= testZeroBaseStackCanaryCheckIsKept();
  ok &= testSharedFailStackCanaryChecksAreRemoved();
  ok &= testMixedFailPredecessorKeepsNonCanaryFailPath();
  ok &= testPhiFsCanaryAddressStackCanaryCheckIsRemoved();
  ok &= testSelfPhiFsCanaryAddressStackCanaryCheckIsRemoved();
  ok &= testSelfPhiFsBaseStackCanaryCheckIsRemoved();
  ok &= testKnownSqrtlUsesLongDoubleStackAndSt0();
  ok &= testPlannerSplitsZmmForFloatAbiSlot();
  ok &= testPlannerSplitsRegisterForDemandMask();
  ok &= testRecordedReturnValueSurvivesPartialDemandRewrite();
  ok &= testFramePointerLoadFoldsAcrossLoop();
  ok &= testStackAdjustmentPushDoesNotCreateEntryParam();
  ok &= testFramePointerLoadAfterRestoreIsKept();
  ok &= testTwoFramePointerSettersAreNotFolded();
  return ok ? 0 : 1;
}
