#include "RegisterTestHarness.h"

#include "notdec-bin2llvm/passes/register/NativeRegisterValueDce.h"

namespace {

bool testDuplicatePredecessorEdgesKeepPhiComplete() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-duplicate-edge-phi", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *type =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context),
                              {llvm::Type::getInt32Ty(context)}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "duplicate_edge_phi", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *other =
      llvm::BasicBlock::Create(context, "other", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);

  llvm::IRBuilder<> builder(entry);
  llvm::SwitchInst *switchInst =
      builder.CreateSwitch(function->getArg(0), join, 2);
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

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.LoadsReplaced == 1,
                "duplicate-edge load was not replaced") &&
         expect(summary.PhisCreated >= 1,
                "duplicate predecessor edge PHI was not completed") &&
         verifyOk(module,
                  "module failed verifier after duplicate-edge PHI test");
}

bool testPartialReadLoopPassthroughUsesDominatorTree() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-read-loop-dt", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");

  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rsi->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rsi->getType(), llvm::Type::getInt32Ty(context), 64, 32);

  auto *type = llvm::FunctionType::get(llvm::Type::getInt32Ty(context),
                                       {llvm::Type::getInt1Ty(context)}, false);
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_read_loop_passthrough", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *header =
      llvm::BasicBlock::Create(context, "header", function);
  llvm::BasicBlock *updateRcx =
      llvm::BasicBlock::Create(context, "update_rcx", function);
  llvm::BasicBlock *updateRsi =
      llvm::BasicBlock::Create(context, "update_rsi", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);
  llvm::BasicBlock *exit = llvm::BasicBlock::Create(context, "exit", function);

  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 0),
                "RCX");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 0),
                "RSI");
  builder.CreateBr(header);

  builder.SetInsertPoint(header);
  llvm::Value *headerRsi = builder.CreateCall(
      partialRead,
      {rsi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *done = builder.CreateICmpEQ(
      headerRsi, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 3));
  builder.CreateCondBr(done, exit, updateRcx);

  builder.SetInsertPoint(updateRcx);
  llvm::Value *rcxLow = builder.CreateCall(
      partialRead,
      {rcx, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *rcxNext =
      builder.CreateAdd(rcxLow, llvm::ConstantInt::get(rcxLow->getType(), 1));
  builder.CreateCall(partialWrite, {rcx, rcxNext,
                                    llvm::ConstantInt::get(
                                        llvm::Type::getInt64Ty(context), 0)});
  builder.CreateCondBr(function->getArg(0), join, updateRsi);

  builder.SetInsertPoint(updateRsi);
  llvm::Value *rsiLow = builder.CreateCall(
      partialRead,
      {rsi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *rsiNext =
      builder.CreateAdd(rsiLow, llvm::ConstantInt::get(rsiLow->getType(), 1));
  builder.CreateCall(partialWrite, {rsi, rsiNext,
                                    llvm::ConstantInt::get(
                                        llvm::Type::getInt64Ty(context), 0)});
  builder.CreateBr(join);

  builder.SetInsertPoint(join);
  llvm::Value *joinedRsi = builder.CreateCall(
      partialRead,
      {rsi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *keepGoing = builder.CreateICmpULT(
      joinedRsi, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 5));
  builder.CreateCondBr(keepGoing, header, exit);

  builder.SetInsertPoint(exit);
  llvm::Value *result = builder.CreateCall(
      partialRead,
      {rsi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateRet(result);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  bool hasUnknown = false;
  for (llvm::Instruction &inst : llvm::instructions(*function)) {
    hasUnknown |= isUnknownValueCall(&inst);
  }

  return expect(summary.LoadsReplaced >= 4,
                "loop partial reads were not rewritten") &&
         expect(!hasPartialReadCall(*function),
                "loop partial read helper remained") &&
         expect(!hasUnknown,
                "loop passthrough partial read created an unknown value") &&
         verifyOk(module,
                  "module failed verifier after partial read loop DT test");
}

bool testX87Fp80StoreStillFeedsRange() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-x87-fp80-store", context);
  attachTestAbi(module);

  llvm::GlobalVariable *st0 = createRegisterGlobal(
      module, "ST0", llvm::Type::getIntNTy(context, 80), 0x1100, 10);

  auto *type = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::InternalLinkage, "x87_fp80_store", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *value = builder.CreateSIToFP(
      function->getArg(0), llvm::Type::getX86_FP80Ty(context), "fp80");
  builder.CreateStore(value, st0);
  builder.CreateRetVoid();

  // 调用者（root 函数）在调用后读 @ST0 存进内存，让被调函数拿到 ST0 的 ExitDemand。
  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *caller = llvm::Function::Create(
      callerType, llvm::GlobalValue::InternalLinkage, "x87_fp80_store_caller",
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
  llvm::Function *rewritten = module.getFunction("x87_fp80_store");
  return expect(rewritten != nullptr, "x87 fp80 store function missing") &&
         expect(rewritten->getReturnType()->isX86_FP80Ty(),
                "x86_fp80 ST0 store was not recovered as a return slot") &&
         expect(!moduleHasUsedFunctionNamed(module, "notdec.unknown.i80"),
                "x86_fp80 store into the i80 ST0 global was dropped") &&
         verifyOk(module, "module failed verifier after x87 fp80 store test");
}

bool testX87WindowShiftLoadFollowsRangeState() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-x87-window-shift-load", context);
  attachTestAbi(module);

  llvm::Type *int80 = llvm::Type::getIntNTy(context, 80);
  llvm::Type *fp80 = llvm::Type::getX86_FP80Ty(context);
  llvm::GlobalVariable *st0 =
      createRegisterGlobal(module, "ST0", int80, 0x1100, 10);
  llvm::GlobalVariable *st1 =
      createRegisterGlobal(module, "ST1", int80, 0x1110, 10);

  llvm::Function *push = notdec::bin2llvm::getOrInsertNativeX87Intrinsic(
      module, "notdec.x87.push", llvm::Type::getVoidTy(context), {fp80},
      false);

  auto *type = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::InternalLinkage, "x87_window_shift_load", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);

  auto newSt0Value = [&](llvm::StringRef name) {
    return builder.CreateBitCast(
        builder.CreateSIToFP(function->getArg(0), fp80, name), int80,
        name + ".bits");
  };
  auto shiftStore = [&](llvm::GlobalVariable *target, llvm::Value *value,
                        llvm::StringRef name) {
    llvm::StoreInst *store = builder.CreateStore(value, target);
    store->setMetadata("notdec.register.access",
                       registerAccessMetadata(context, name.str(), 10));
  };

  // 第一次 push：@ST0/@ST1 在本块内还没有定义，两条入口搬移读保持原样。
  llvm::Value *old0 = builder.CreateLoad(int80, st0, "old0");
  llvm::Value *old1 = builder.CreateLoad(int80, st1, "old1");
  llvm::Value *v1 = newSt0Value("v1");
  shiftStore(st0, v1, "ST0");
  shiftStore(st1, old0, "ST1");
  builder.CreateCall(push, {builder.CreateBitCast(old1, fp80, "old1.fp80")});

  // 第二次 push：搬移读的旧 ST1 = 上一次写进 @ST1 的 %old0，必须解析成 SSA 值
  // 而不是重新读 @ST1。
  llvm::Value *prev0 = builder.CreateLoad(int80, st0, "prev0");
  llvm::Value *prev1 = builder.CreateLoad(int80, st1, "prev1");
  llvm::Value *v2 = newSt0Value("v2");
  shiftStore(st0, v2, "ST0");
  shiftStore(st1, prev0, "ST1");
  builder.CreateCall(push, {builder.CreateBitCast(prev1, fp80, "prev1.fp80")});

  // 第三次 push：InstCombine 把 i80 load + bitcast 折叠成 x86_fp80 load，
  // 位宽相同但类型与 range 状态里的 i80 值不同。
  llvm::LoadInst *shifted = builder.CreateLoad(fp80, st1, "shifted");
  shifted->setMetadata("notdec.x87.window.shift", llvm::MDNode::get(context, {}));
  llvm::Value *prev0b = builder.CreateLoad(int80, st0, "prev0b");
  llvm::Value *v3 = newSt0Value("v3");
  shiftStore(st0, v3, "ST0");
  shiftStore(st1, prev0b, "ST1");
  builder.CreateCall(push, {shifted});
  builder.CreateRetVoid();

  // 调用者：让被改写的函数有真实调用点（与其它 x87 测试一致）。
  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *caller = llvm::Function::Create(
      callerType, llvm::GlobalValue::InternalLinkage, "x87_window_shift_caller",
      module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "caller_entry", caller);
  llvm::IRBuilder<> callerBuilder(callerEntry);
  callerBuilder.CreateCall(
      function->getFunctionType(), function,
      {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 7)});
  callerBuilder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("x87_window_shift_load");
  if (rewritten == nullptr) {
    return expect(false, "x87 window shift function was removed");
  }

  std::vector<llvm::Value *> pushArgs;
  for (llvm::BasicBlock &block : *rewritten) {
    for (llvm::Instruction &inst : block) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() != nullptr &&
          call->getCalledFunction()->getName() == "notdec.x87.push") {
        pushArgs.push_back(call->getArgOperand(0));
      }
    }
  }

  uint32_t st1Loads = countLoadsFromGlobal(*rewritten, st1);

  return expect(pushArgs.size() == 3,
                "x87 window shift pushes were not preserved") &&
         // 继承下来的窗口槽没有 SSA 定义时，按寄存器值表示准则保留成入口裸读
         // （一次），不再落成 notdec.unknown；窗口模型自身不应该反复重读 @ST1。
         expect(st1Loads <= 1,
                "x87 window shift read re-reads the @ST1 global more than once") &&
         verifyOk(module,
                  "module failed verifier after x87 window shift load test");
}

bool testCalleeSavedRegisterSurvivesInternalCall() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-callee-saved-call", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {},
                                             false);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::InternalLinkage, "rbx_touch", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  llvm::LoadInst *read = calleeBuilder.CreateLoad(
      llvm::Type::getInt64Ty(context), rbx, "rbx.entry");
  read->setMetadata("notdec.register.access",
                    registerAccessMetadata(context, "RBX"));
  llvm::StoreInst *write = calleeBuilder.CreateStore(read, rbx);
  write->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, "RBX"));
  calleeBuilder.CreateRetVoid();

  auto *callerType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {},
                                             false);
  llvm::Function *caller = llvm::Function::Create(
      callerType, llvm::GlobalValue::InternalLinkage, "rbx_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "caller_entry", caller);
  llvm::IRBuilder<> callerBuilder(callerEntry);
  llvm::StoreInst *seed = callerBuilder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 1234), rbx);
  seed->setMetadata("notdec.register.access",
                    registerAccessMetadata(context, "RBX"));
  callerBuilder.CreateCall(callee->getFunctionType(), callee);
  llvm::LoadInst *reload = callerBuilder.CreateLoad(
      llvm::Type::getInt64Ty(context), rbx, "rbx.after");
  reload->setMetadata("notdec.register.access",
                      registerAccessMetadata(context, "RBX"));
  callerBuilder.CreateStore(reload, sink);
  callerBuilder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("rbx_caller");
  if (rewritten == nullptr) {
    return expect(false, "callee-saved caller was removed");
  }
  return expect(!moduleHasUsedFunctionNamed(module, "notdec.unknown.i64"),
                "callee-saved register after an internal call became unknown") &&
         verifyOk(module, "module failed verifier after callee-saved call test");
}

bool testRegisterPointerPhiLoadIsCanonicalized() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-register-pointer-phi-load", context);
  attachTestAbiWithInputs(module, {"R12", "R13"});
  llvm::GlobalVariable *r12 = createRegisterGlobal(module, "R12");
  llvm::GlobalVariable *r13 = createRegisterGlobal(module, "R13");

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context),
                                       {llvm::Type::getInt1Ty(context)}, false);
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "register_pointer_phi_load", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right =
      llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(function->getArg(0), left, right);
  builder.SetInsertPoint(left);
  builder.CreateBr(join);
  builder.SetInsertPoint(right);
  builder.CreateBr(join);
  builder.SetInsertPoint(join);
  llvm::PHINode *pointerPhi =
      builder.CreatePHI(llvm::PointerType::get(context, 0), 2, "reg.ptr");
  pointerPhi->addIncoming(r12, left);
  pointerPhi->addIncoming(r13, right);
  llvm::LoadInst *loaded =
      builder.CreateLoad(llvm::Type::getInt64Ty(context), pointerPhi, "merged");
  loaded->setMetadata("notdec.register.access",
                      registerAccessMetadata(context, "R12"));
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(!hasLoadFromPhiPointer(*function),
                "register pointer PHI load remained after SummarySSA") &&
         expect(summary.LoadsReplaced >= 1,
                "canonicalized register load was not consumed") &&
         verifyOk(
             module,
             "module failed verifier after register pointer PHI load test");
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

  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  options.EnableResidueRemoval = false;
  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module, options);

  bool hasClobberIncoming = false;
  bool hasUndefIncoming = false;
  bool hasZeroIncoming = false;
  for (llvm::Instruction &inst : llvm::instructions(*function)) {
    auto *phi = llvm::dyn_cast<llvm::PHINode>(&inst);
    if (phi == nullptr) {
      continue;
    }
    for (llvm::Value *incoming : phi->incoming_values()) {
      hasClobberIncoming |= valueNameContains(incoming, "summary_clobber") ||
                            valueNameContains(incoming, "R10.clobber");
      hasUndefIncoming |= llvm::isa<llvm::UndefValue>(incoming);
      auto *constant = llvm::dyn_cast<llvm::ConstantInt>(incoming);
      hasZeroIncoming |= constant != nullptr && constant->isZero();
    }
  }

  return expect(summary.LoadsReplaced == 1,
                "unknown incoming load was not replaced") &&
         expect(summary.CallClobberValues >= 1,
                "external clobber effect was not observed") &&
         expect(hasClobberIncoming,
                "clobber incoming was not materialized explicitly") &&
         expect(!hasUndefIncoming, "unknown incoming still used bare undef") &&
         expect(!hasZeroIncoming, "unknown incoming was folded to zero") &&
         verifyOk(module,
                  "module failed verifier after opaque unknown incoming test");
}

bool testSelfOnlyPhiBecomesOpaqueUnknown() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-self-only-phi", context);
  attachTestAbi(module);
  llvm::GlobalVariable *r10 = createRegisterGlobal(module, "R10");

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context),
                                       {llvm::Type::getInt1Ty(context)}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "self_only_phi", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *loop = llvm::BasicBlock::Create(context, "loop", function);
  llvm::BasicBlock *exit = llvm::BasicBlock::Create(context, "exit", function);
  llvm::BasicBlock *unreachableLoop =
      llvm::BasicBlock::Create(context, "unreachable_loop", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(function->getArg(0), loop, exit);
  builder.SetInsertPoint(loop);
  llvm::LoadInst *loaded = loadRegister(builder, r10, "R10", "known_value");
  builder.CreateRet(loaded);
  builder.SetInsertPoint(exit);
  builder.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0));
  builder.SetInsertPoint(unreachableLoop);
  llvm::LoadInst *selfLoaded =
      loadRegister(builder, r10, "R10", "self_loop_value");
  llvm::Value *sinkPointer = builder.CreateIntToPtr(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 4096),
      llvm::PointerType::get(context, 0), "self_loop_sink");
  builder.CreateStore(selfLoaded, sinkPointer);
  builder.CreateCondBr(function->getArg(0), unreachableLoop, unreachableLoop);

  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  options.EnableResidueRemoval = false;
  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module, options);

  bool hasSelfLoopUnknown = false;
  for (llvm::Instruction &inst : *unreachableLoop) {
    hasSelfLoopUnknown |= isUnknownValueCall(&inst);
  }

  return expect(summary.LoadsReplaced >= 1,
                "self-only PHI test did not replace any load") &&
         expect(summary.PhisSimplified >= 1,
                "self-only PHI was not simplified") &&
         expect(hasSelfLoopUnknown,
                "self-only PHI did not become opaque unknown") &&
         verifyOk(module,
                  "module failed verifier after self-only PHI unknown test");
}

bool testFsOffsetPreservedAcrossExternalCall() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-fs-offset-preserve", context);
  attachTestAbi(module);
  llvm::GlobalVariable *fsOffset = createRegisterGlobal(module, "FS_OFFSET");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *external = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "unknown_external", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context),
                                       {llvm::Type::getInt1Ty(context)}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "fs_preserved", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right =
      llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(function->getArg(0), left, right);
  builder.SetInsertPoint(left);
  llvm::LoadInst *direct =
      loadRegister(builder, fsOffset, "FS_OFFSET", "fs_direct");
  (void)direct;
  builder.CreateBr(join);
  builder.SetInsertPoint(right);
  builder.CreateCall(voidType, external);
  builder.CreateBr(join);
  builder.SetInsertPoint(join);
  llvm::LoadInst *merged =
      loadRegister(builder, fsOffset, "FS_OFFSET", "fs_merged");
  builder.CreateRet(merged);

  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  options.EnableResidueRemoval = false;
  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module, options);

  bool hasZeroFsPhiIncoming = false;
  bool hasFreezeFsPhiIncoming = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *phi = llvm::dyn_cast<llvm::PHINode>(&inst);
    if (phi == nullptr ||
        phi->getMetadata("notdec.register.summary_ssa.phi") == nullptr) {
      continue;
    }
    for (llvm::Value *incoming : phi->incoming_values()) {
      auto *constant = llvm::dyn_cast<llvm::ConstantInt>(incoming);
      hasZeroFsPhiIncoming |= constant != nullptr && constant->isZero();
      hasFreezeFsPhiIncoming |= llvm::isa<llvm::FreezeInst>(incoming);
    }
  }

  return expect(summary.LoadsReplaced >= 1,
                "FS_OFFSET load was not replaced") &&
         expect(summary.UnknownCallEffects == 0,
                "external call clobbered FS_OFFSET") &&
         expect(!hasZeroFsPhiIncoming,
                "FS_OFFSET PHI used zero as unknown incoming") &&
         expect(!hasFreezeFsPhiIncoming,
                "FS_OFFSET PHI used frozen poison as incoming") &&
         verifyOk(module,
                  "module failed verifier after FS_OFFSET preserve test");
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

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.LoadsReplaced == 1,
                "preserved call load not replaced") &&
         expect(summary.DeadLoadsRemoved == 1,
                "preserved call replaced load was not removed") &&
         verifyOk(module, "module failed verifier after preserved call test");
}

bool testRewrittenExternalCallPreservesUnaffectedRegister() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-rewritten-external-preserve", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "__errno_location", module);
  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *caller =
      llvm::Function::Create(callerType, llvm::GlobalValue::ExternalLinkage,
                             "rewritten_external_preserves_rbx", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rbx, llvm::ConstantInt::get(rbx->getValueType(), 42),
                "RBX");
  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *raxValue = loadRegister(builder, rax, "RAX", "rax.ret");
  llvm::LoadInst *rbxValue = loadRegister(builder, rbx, "RBX", "rbx.after");
  builder.CreateRet(builder.CreateAdd(raxValue, rbxValue));

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenCaller =
      module.getFunction("rewritten_external_preserves_rbx");
  llvm::Function *rewrittenCallee = module.getFunction("__errno_location");

  return expect(rewrittenCaller != nullptr, "rewritten caller missing") &&
         expect(rewrittenCallee != nullptr, "rewritten errno callee missing") &&
         expect(rewrittenCallee->getReturnType()->isIntegerTy(64),
                "errno call was not rewritten to a typed return") &&
         expect(!hasRegisterLoad(*rewrittenCaller, "RBX"),
                "RBX load remained after rewritten preserved call") &&
         expect(!hasRegisterStore(*rewrittenCaller, "RBX"),
                "RBX store remained after rewritten preserved call") &&
         expect(summary.CallsRewritten >= 1,
                "external call rewrite was not counted") &&
         verifyOk(module,
                  "module failed verifier after rewritten preserved call test");
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

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  // 第一遍统一生成 clobber 占位，返回槽由签名决定（见
  // logs/20260805-12-native-unify-call-return-plan.md）；这里断言占位
  // 证据存在、返回链把寄存器 load 换掉即可。
  return expect(summary.CallClobberValues >= 1,
                "demanded return helper was not created") &&
         expect(summary.CallReturnValues == 0,
                "first pass must not distinguish return helpers") &&
         expect(summary.DeadLoadsRemoved == 1,
                "return replaced load was not removed") &&
         verifyOk(module, "module failed verifier after demanded return test");
}

bool testExternalReturnUsesRangeCallValue() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-external-return-range", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *external =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "external_return_range", module);
  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *caller =
      llvm::Function::Create(callerType, llvm::GlobalValue::ExternalLinkage,
                             "uses_external_return", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(calleeType, external);
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "ret_rax");
  builder.CreateRet(loaded);

  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  options.EnableResidueRemoval = false;
  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module, options);

  llvm::Function *rewritten = module.getFunction("external_return_range");
  bool typedExternalReturn = false;
  for (llvm::Instruction &inst : llvm::instructions(*caller)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call != nullptr && call->getCalledFunction() == rewritten &&
        call->getType()->isIntegerTy(64)) {
      typedExternalReturn = true;
    }
  }

  return expect(summary.CallClobberValues >= 1,
                "external return range helper was not created") &&
         expect(rewritten != nullptr,
                "rewritten external return callee missing") &&
         expect(!hasLiveReplacedRegisterLoad(*caller),
                "external return range left live raw RAX load") &&
         expect(typedExternalReturn,
                "external return call was not rebuilt with integer return") &&
         expect(!moduleHasUsedFunctionNamed(
                    module, "notdec.register.summary_return.i64"),
                "external return helper remained after rewrite") &&
         verifyOk(module,
                  "module failed verifier after external return range test");
}

bool testIndirectCallReturnHelperIsRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-indirect-call-return", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *caller =
      llvm::Function::Create(callerType, llvm::GlobalValue::ExternalLinkage,
                             "uses_indirect_return", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 7),
                "RDI");
  llvm::Value *callee = builder.CreateIntToPtr(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0x1234),
      llvm::PointerType::get(context, 0), "callee.ptr");
  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  builder.CreateCall(voidType, callee);
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "ret_rax");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);

  bool typedIndirectReturn = false;
  for (llvm::Instruction &inst : llvm::instructions(*caller)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call != nullptr && call->getCalledFunction() == nullptr &&
        call->getType()->isIntegerTy(64)) {
      typedIndirectReturn = true;
    }
  }

  return expect(summary.CallClobberValues >= 1,
                "indirect call did not create return helper before rewrite") &&
         expect(summary.CallsRewritten >= 1,
                "indirect callsite was not rewritten") &&
         expect(typedIndirectReturn,
                "indirect call was not rebuilt with integer return type") &&
         expect(!moduleHasUsedFunctionNamed(
                    module, "notdec.register.summary_return.i64"),
                "indirect call return helper remained") &&
         verifyOk(module,
                  "module failed verifier after indirect call return rewrite");
}

bool testIndirectConservativeReturnShapeExcludesRdx() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-indirect-conservative-return", context);
  attachMultiOutputTestAbi(module);
  createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  createRegisterGlobal(module, "XMM0", llvm::Type::getIntNTy(context, 128),
                        0x1200, 16);
  createRegisterGlobal(module, "ST0", llvm::Type::getIntNTy(context, 80),
                        0x1100, 10);

  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *caller =
      llvm::Function::Create(callerType, llvm::GlobalValue::ExternalLinkage,
                             "uses_conservative_indirect_return", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *callee = builder.CreateIntToPtr(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0x1234),
      llvm::PointerType::get(context, 0), "callee.ptr");
  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  builder.CreateCall(voidType, callee);
  llvm::Value *returnedRax = loadRegister(builder, rax, "RAX", "ret_rax");
  llvm::Value *returnedRdx = loadRegister(builder, rdx, "RDX", "ret_rdx");
  builder.CreateRet(builder.CreateAdd(returnedRax, returnedRdx));

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);

  bool typedIndirectReturn = false;
  for (llvm::Instruction &inst : llvm::instructions(*caller)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call != nullptr && call->getCalledFunction() == nullptr &&
        call->getType()->isIntegerTy(64)) {
      typedIndirectReturn = true;
    }
  }

  return expect(summary.CallsRewritten >= 1,
                "indirect callsite was not rewritten") &&
         expect(typedIndirectReturn,
                "live RDX was incorrectly promoted to a second return") &&
         expect(!moduleHasUsedFunctionNamed(
                    module, "notdec.register.summary_return.i64"),
                "indirect return helper remained") &&
         verifyOk(module,
                  "module failed verifier after indirect conservative return test");
}

bool testIndirectCallInputsUseLocalDefinitionEvidence() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-indirect-inputs", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  createRegisterGlobal(module, "RCX");
  createRegisterGlobal(module, "R8");
  createRegisterGlobal(module, "R9");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *caller = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "uses_indirect_input_evidence",
      module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 11),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 22),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 33),
                "RDX");
  llvm::Value *callee = builder.CreateIntToPtr(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0x1234),
      llvm::PointerType::get(context, 0), "callee.ptr");
  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  builder.CreateCall(voidType, callee);
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "ret_rax");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);

  unsigned indirectArgCount = 0;
  bool rebuilt = false;
  for (llvm::Instruction &inst : llvm::instructions(*caller)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call != nullptr && call->getCalledFunction() == nullptr) {
      indirectArgCount = call->arg_size();
      rebuilt = true;
    }
  }

  return expect(summary.CallsRewritten >= 1,
                "indirect callsite was not rewritten") &&
         expect(rebuilt, "indirect call was not found after rewrite") &&
         expect(indirectArgCount == 3,
                "indirect arity did not follow LocalDefinition evidence") &&
         verifyOk(module,
                  "module failed verifier after indirect input evidence test");
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

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
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

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
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

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
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

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
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

bool testFinalCallArgClobberWarningIsKept() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-call-arg-warning", context);
  attachTestAbi(module);
  (void)createRegisterGlobal(module, "RDI");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *clobber = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "warning_clobber", module);
  llvm::Function *freeFn = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "free", module);
  llvm::Function *caller =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "final_call_arg_warning_caller", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(voidType, clobber);
  builder.CreateCall(voidType, freeFn);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  bool hasFinalWarning = false;
  bool hasCollectionWarning = false;
  for (const notdec::bin2llvm::NativeRegisterEliminationWarning &warning :
       summary.Warnings) {
    if (warning.CalleeName != "free" || warning.Kind != "call_arg") {
      continue;
    }
    hasFinalWarning |= warning.Reason == "call_arg_uses_clobber_value";
    hasCollectionWarning |=
        llvm::StringRef(warning.Reason).starts_with("call_arg_binding_");
  }

  return expect(hasFinalWarning,
                "final clobber-derived call argument was not reported") &&
         expect(!hasCollectionWarning,
                "collection-phase call argument warning remained") &&
         verifyOk(module,
                  "module failed verifier after final call-arg warning test");
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

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
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

bool testKnownZeroArgExternalTypedReturnIsMaterialized() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-zero-arg-typed-return", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *voidCalleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(voidCalleeType, llvm::GlobalValue::ExternalLinkage,
                             "__ctype_b_loc", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "known_zero_arg_typed_return", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(voidCalleeType, callee);
  llvm::LoadInst *raxLoad = loadRegister(builder, rax, "RAX", "rax.after");
  builder.CreateRet(raxLoad);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("__ctype_b_loc");
  llvm::CallInst *call = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (candidate != nullptr && candidate->getCalledFunction() == rewritten) {
      call = candidate;
    }
  }

  return expect(rewritten != nullptr, "typed zero-arg external missing") &&
         expect(rewritten->getReturnType()->isIntegerTy(64),
                "typed zero-arg external kept void return") &&
         expect(call != nullptr, "typed zero-arg external call missing") &&
         expect(call->getType()->isIntegerTy(64),
                "typed zero-arg external call kept void return") &&
         expect(summary.Warnings.empty(),
                "typed zero-arg external left register SSA warning") &&
         verifyOk(
             module,
             "module failed verifier after typed zero-arg external rewrite");
}

bool testKnownErrnoLocationReturnIsMaterialized() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-errno-location-return", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *voidCalleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(voidCalleeType, llvm::GlobalValue::ExternalLinkage,
                             "__errno_location", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "errno_location_return_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(voidCalleeType, callee);
  llvm::LoadInst *raxLoad = loadRegister(builder, rax, "RAX", "rax.after");
  builder.CreateRet(raxLoad);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("__errno_location");
  llvm::CallInst *call = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (candidate != nullptr && candidate->getCalledFunction() == rewritten) {
      call = candidate;
    }
  }

  return expect(rewritten != nullptr, "errno_location external missing") &&
         expect(rewritten->getReturnType()->isIntegerTy(64),
                "errno_location kept void return") &&
         expect(call != nullptr, "errno_location call missing") &&
         expect(call->getType()->isIntegerTy(64),
                "errno_location call kept void return") &&
         expect(summary.Warnings.empty(),
                "errno_location left register SSA warning") &&
         verifyOk(module,
                  "module failed verifier after errno_location rewrite");
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

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
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

bool testFlagStoreBeforeVolatileCallIsRemoved() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-clobbered-flag-after-call", context);
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
                             "clobbered_flag_store_after_call", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, of, llvm::ConstantInt::get(of->getValueType(), 1),
                "OF");
  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *loaded = loadRegister(builder, of, "OF", "of.after");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(countStoresTo(*function, of) == 0,
                "clobbered flag store before call was not removed") &&
         expect(summary.CallClobberValues >= 1,
                "flag read after call did not materialize clobber value") &&
         verifyOk(module, "module failed verifier after clobbered flag test");
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

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
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
      {"getdelim", 4},
      {"__poll_chk", 4},
      {"__longjmp_chk", 2},
      {"__sigsetjmp", 2},
      {"__strcpy_chk", 3},
      {"__sysconf", 1},
      {"__vfprintf_chk", 4},
      {"__vsnprintf_chk", 6},
      {"__xpg_strerror_r", 3},
      {"ERR_error_string_n", 3},
      {"ERR_clear_error", 0},
      {"ERR_get_error", 0},
      {"ERR_peek_last_error", 0},
      {"ERR_print_errors_fp", 1},
      {"ERR_reason_error_string", 1},
      {"OPENSSL_init_crypto", 2},
      {"OPENSSL_init_ssl", 2},
      {"BN_sub", 3},
      {"accept", 3},
      {"accept4", 4},
      {"arc4random", 0},
      {"cfmakeraw", 1},
      {"chmod", 2},
      {"chown", 3},
      {"closedir", 1},
      {"closelog", 0},
      {"fork", 0},
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
      {"av_freep", 1},
      {"fgetc", 1},
      {"fileno", 1},
      {"fopen64", 2},
      {"fputs", 2},
      {"fsync", 1},
      {"fstat", 2},
      {"fstatfs64", 2},
      {"ftruncate", 2},
      {"futimens", 2},
      {"gai_strerror", 1},
      {"getaddrinfo", 4},
      {"getdelim", 4},
      {"getnameinfo", 7},
      {"getegid", 0},
      {"getentropy", 2},
      {"getgid", 0},
      {"getservbyname", 2},
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
      {"getline", 3},
      {"mkdir", 2},
      {"mkdtemp", 1},
      {"mmap", 6},
      {"mmap64", 6},
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
      {"av_packet_free", 1},
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
      {"popen", 2},
      {"readv", 3},
      {"recvmmsg", 5},
      {"recv", 4},
      {"recvfrom", 6},
      {"sched_get_priority_max", 1},
      {"sched_get_priority_min", 1},
      {"sched_getaffinity", 3},
      {"sched_getcpu", 0},
      {"sched_yield", 0},
      {"sasl_dispose", 1},
      {"sasl_server_init", 2},
      {"sasl_server_start", 6},
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
      {"strchrnul", 2},
      {"av_strerror", 3},
      {"av_usleep", 1},
      {"SSL_accept", 1},
      {"SSL_clear", 1},
      {"SSL_connect", 1},
      {"SSL_CTX_check_private_key", 1},
      {"SSL_CTX_ctrl", 4},
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
      {"RSA_set0_key", 4},
      {"setenv", 3},
      {"setgid", 1},
      {"setgroups", 2},
      {"setpriority", 3},
      {"setregid", 2},
      {"setreuid", 2},
      {"setbuf", 2},
      {"setrlimit", 2},
      {"setvbuf", 4},
      {"setuid", 1},
      {"sigaddset", 2},
      {"sigemptyset", 1},
      {"__sysv_signal", 2},
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
      {"splice", 6},
      {"stat64", 2},
      {"statfs64", 2},
      {"strerror_r", 3},
      {"strndup", 2},
      {"strnlen", 2},
      {"strtok", 2},
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
    llvm::Function *callee = llvm::Function::Create(
        calleeType, llvm::GlobalValue::ExternalLinkage, testCase.Name, module);
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

    notdec::bin2llvm::runNativeRegisterElimination(module);
    llvm::CallInst *call = nullptr;
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      if (auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (candidate->getCalledFunction() != nullptr &&
            candidate->getCalledFunction()->getName() == testCase.Name) {
          call = candidate;
        }
      }
    }

    unsigned expectedArgs = std::min<unsigned>(testCase.Args, 6);
    std::string contextMessage =
        std::string(" for known fixed external ") + testCase.Name;
    std::string missingMessage =
        "known fixed external call missing" + contextMessage;
    std::string arityMessage =
        "known fixed external used wrong arity" + contextMessage;
    if (!expect(call != nullptr, missingMessage.c_str()) ||
        !expect(call->arg_size() == expectedArgs, arityMessage.c_str()) ||
        !verifyOk(module,
                  "module failed verifier after fixed external rewrite")) {
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
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "uses_direct_call_result", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  llvm::CallInst *call = builder.CreateCall(calleeType, callee);
  storeRegister(builder, rax, call, "RAX");
  llvm::LoadInst *raxLoad = loadRegister(builder, rax, "RAX", "rax.after");
  llvm::LoadInst *rbxLoad = loadRegister(builder, rbx, "RBX", "rbx.after");
  llvm::Value *sum = builder.CreateAdd(call, raxLoad);
  sum = builder.CreateAdd(sum, rbxLoad);
  builder.CreateRet(sum);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  bool hasStructCall = false;
  bool hasExtractedDirectUse = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (candidate == nullptr || candidate->getCalledFunction() == nullptr ||
        candidate->getCalledFunction()->getName() != "unknown_external") {
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

bool testKnownExternalUsesSingleIntegerReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-known-external-single-return", context);

  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_external_single_return_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";
  notdec::bin2llvm::NativeAbiParamEntry input;
  input.MinSize = 1;
  input.MaxSize = 8;
  input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  input.Storage.Name = "RDI";
  abi.Inputs.push_back(input);
  for (llvm::StringRef name : {"RAX", "RDX"}) {
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
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "fclose", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "uses_fclose_rdx_after", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *raxLoad = loadRegister(builder, rax, "RAX", "rax.after");
  llvm::LoadInst *rdxLoad = loadRegister(builder, rdx, "RDX", "rdx.after");
  builder.CreateRet(builder.CreateAdd(raxLoad, rdxLoad));

  notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::CallInst *call = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (candidate != nullptr && candidate->getCalledFunction() != nullptr &&
        candidate->getCalledFunction()->getName() == "fclose") {
      call = candidate;
    }
  }

  return expect(call != nullptr, "known external fclose call missing") &&
         expect(
             call->getType()->isIntegerTy(64),
             "known external fclose was widened to a multi-register return") &&
         verifyOk(module,
                  "module failed verifier after known external return rewrite");
}

bool testUnknownExternalTreatsRdxAsClobberNotReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unknown-external-rdx-clobber", context);

  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_unknown_external_rdx_clobber_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";
  for (llvm::StringRef name : {"RAX", "RDX"}) {
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

  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  auto *sinkType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *callee = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "unknown_external", module);
  llvm::Function *sink = llvm::Function::Create(
      sinkType, llvm::GlobalValue::ExternalLinkage, "consume_rdx", module);
  llvm::Function *function =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external_rdx_after", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(voidType, callee);
  llvm::LoadInst *rdxLoad = loadRegister(builder, rdx, "RDX", "rdx.after");
  builder.CreateCall(sinkType, sink, {rdxLoad});
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  bool hasRdxReturnWarning = false;
  for (const notdec::bin2llvm::NativeRegisterEliminationWarning &warning :
       summary.Warnings) {
    if (warning.RegisterName != "RDX") {
      continue;
    }
    hasRdxReturnWarning |= warning.Kind == "return";
  }
  // 重写会用 takeName 替换函数对象，按名字重新取。
  llvm::Function *rewrittenFunction =
      module.getFunction("unknown_external_rdx_after");
  llvm::Value *rdxReturn = nullptr;
  if (rewrittenFunction != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenFunction)) {
      if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
        rdxReturn = ret->getReturnValue();
      }
    }
  }

  // RDX 被调用 clobber 后读出来，不能变成 unknown_external 的返回槽：
  // 值应显式降级成 unknown，而不是在 IR 里残留 summary_clobber helper。
  return expect(!hasRdxReturnWarning,
                "unknown external RDX was still treated as return") &&
         expect(isUnknownValueCall(rdxReturn),
                "unknown external RDX value was not materialized as unknown") &&
         expect(!moduleHasUsedFunctionNamed(
                    module, "notdec.register.summary_clobber.i64"),
                "unknown external RDX left a clobber helper in the IR") &&
         verifyOk(module,
                  "module failed verifier after unknown external RDX test");
}

bool testUnknownExternalFloatReturnPicksSingleXmmSlot() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unknown-float-return-single-slot", context);

  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_unknown_float_return_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";
  notdec::bin2llvm::NativeAbiParamEntry integerOutput;
  integerOutput.MinSize = 1;
  integerOutput.MaxSize = 8;
  integerOutput.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  integerOutput.Storage.Name = "RAX";
  abi.Outputs.push_back(integerOutput);
  notdec::bin2llvm::NativeAbiParamEntry floatOutput;
  floatOutput.MinSize = 1;
  floatOutput.MaxSize = 8;
  floatOutput.MetaType = "float";
  floatOutput.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  floatOutput.Storage.Name = "XMM0_Qa";
  abi.Outputs.push_back(floatOutput);
  for (llvm::StringRef name : {"RAX", "XMM0"}) {
    notdec::bin2llvm::NativeAbiEffect killed;
    killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
    killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    killed.Storage.Name = name.str();
    abi.Effects.push_back(killed);
  }
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);

  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *external =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external_float_return", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "reads_float_and_rax_after_call", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::AllocaInst *intSink = builder.CreateAlloca(llvm::Type::getInt64Ty(context));
  llvm::AllocaInst *zmmSink = builder.CreateAlloca(zmmType);
  builder.CreateCall(calleeType, external);
  // RAX 和 XMM0（ZMM0 低 64 位）调用后都被读：RAX 是 clobber 复用，XMM0
  // 是真实浮点返回。未知外部只能取一个返回槽，浮点优先，RAX 不能混进
  // 返回结构体（否则 round() 会变成 { i64, double } 之类）。
  llvm::Value *raxLoad = loadRegister(builder, rax, "RAX", "rax.after");
  builder.CreateStore(raxLoad, intSink);
  llvm::Value *zmmLoad = loadRegister(builder, zmm0, "ZMM0", "zmm.after");
  builder.CreateStore(zmmLoad, zmmSink);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenFunction =
      module.getFunction("reads_float_and_rax_after_call");
  llvm::CallInst *call = nullptr;
  if (rewrittenFunction != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenFunction)) {
      auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (candidate != nullptr && candidate->getCalledFunction() != nullptr &&
          candidate->getCalledFunction()->getName() ==
              "unknown_external_float_return") {
        call = candidate;
      }
    }
  }

  // 未知外部浮点返回：只取 XMM0 一个 double 槽，RAX 不被带进来。
  return expect(call != nullptr,
                "unknown external float return call missing") &&
         expect(call != nullptr && call->getType()->isDoubleTy(),
                "unknown external float return was not rewritten to double") &&
         expect(call != nullptr && !call->getType()->isStructTy(),
                "unknown external float return picked extra integer slots") &&
         verifyOk(module,
                  "module failed verifier after float return slot test");
}

bool testUnknownExternalClobberArgBecomesUnknown() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unknown-external-clobber-arg", context);

  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_unknown_external_clobber_arg_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";
  for (llvm::StringRef name : {"RDI", "RSI", "RDX"}) {
    notdec::bin2llvm::NativeAbiParamEntry input;
    input.MinSize = 1;
    input.MaxSize = 8;
    input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    input.Storage.Name = name.str();
    abi.Inputs.push_back(input);
  }
  for (llvm::StringRef name : {"RAX", "RDX"}) {
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
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *first = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "first_unknown", module);
  llvm::Function *second = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "second_unknown", module);
  llvm::Function *function =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "clobber_arg_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  builder.CreateCall(voidType, first);
  llvm::LoadInst *rdxAfter = loadRegister(builder, rdx, "RDX", "rdx.after");
  storeRegister(builder, rdx, rdxAfter, "RDX");
  builder.CreateCall(voidType, second);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  // runNativeRegisterElimination 会重写签名替换函数对象，旧指针会悬空，
  // 必须按名字重新取。
  llvm::Function *rewritten = module.getFunction("clobber_arg_caller");
  bool secondUsesClobber = false;
  if (rewritten != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewritten)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call == nullptr || call->getCalledFunction() == nullptr ||
          call->getCalledFunction()->getName() != "second_unknown") {
        continue;
      }
      for (llvm::Value *arg : call->args()) {
        secondUsesClobber |= valueNameContains(arg, "summary_clobber") ||
                             valueNameContains(arg, "RDX.clobber");
      }
    }
  }

  return expect(rewritten != nullptr, "clobber_arg_caller missing after rewrite") &&
         expect(!secondUsesClobber,
                "unknown external call kept clobber-derived argument") &&
         verifyOk(module,
                  "module failed verifier after clobber arg cleanup test");
}

bool testInternalReturnDoesNotExposeExternalClobber() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-internal-clobber-return", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *external = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "unknown_external", module);
  llvm::Function *callee =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_clobber_return", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(calleeEntry);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 7),
                "RAX");
  builder.CreateCall(voidType, external);
  llvm::LoadInst *rdxAfter = loadRegister(builder, rdx, "RDX", "rdx.after");
  storeRegister(builder, rdx, rdxAfter, "RDX");
  builder.CreateRetVoid();

  llvm::Function *caller =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_clobber_return_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  builder.CreateCall(voidType, callee);
  llvm::LoadInst *rdxResult = loadRegister(builder, rdx, "RDX", "rdx.result");
  llvm::AllocaInst *sink = builder.CreateAlloca(rdx->getValueType());
  builder.CreateStore(rdxResult, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  bool helperLeft = false;
  for (llvm::Function &function : module) {
    if (function.getName().starts_with("notdec.register.summary_clobber")) {
      helperLeft |= !function.use_empty();
    }
  }

  return expect(!helperLeft,
                "internal return exposed external clobber helper") &&
         verifyOk(module,
                  "module failed verifier after clobber return cleanup test");
}

bool testClobberReturnPhiPreservesMixedPath() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-clobber-return-phi", context);
  attachTestAbiWithInputs(module, {"RDI"});
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  auto *freeType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *freeFn = llvm::Function::Create(
      freeType, llvm::GlobalValue::ExternalLinkage, "free", module);
  llvm::Function *callee =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_clobber_return_phi", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", callee);
  llvm::BasicBlock *clobberPath =
      llvm::BasicBlock::Create(context, "clobber_path", callee);
  llvm::BasicBlock *knownPath =
      llvm::BasicBlock::Create(context, "known_path", callee);
  llvm::BasicBlock *done = llvm::BasicBlock::Create(context, "done", callee);

  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 7),
                "RAX");
  llvm::LoadInst *branchInput = loadRegister(builder, rdi, "RDI", "branch");
  llvm::Value *condition = builder.CreateICmpNE(
      branchInput, llvm::ConstantInt::get(rdi->getValueType(), 0), "condition");
  builder.CreateCondBr(condition, clobberPath, knownPath);

  builder.SetInsertPoint(clobberPath);
  builder.CreateCall(freeType, freeFn, {branchInput});
  builder.CreateBr(done);

  builder.SetInsertPoint(knownPath);
  builder.CreateBr(done);

  builder.SetInsertPoint(done);
  builder.CreateRetVoid();

  llvm::Function *caller =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_clobber_return_phi_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  builder.CreateCall(voidType, callee);
  llvm::LoadInst *result = loadRegister(builder, rax, "RAX", "result");
  llvm::AllocaInst *sink = builder.CreateAlloca(rax->getValueType());
  builder.CreateStore(result, sink);
  builder.CreateRetVoid();

  llvm::SmallString<128> jsonPath;
  int jsonFd = -1;
  std::error_code error = llvm::sys::fs::createTemporaryFile(
      "notdec-mixed-clobber-return", "json", jsonFd, jsonPath);
  if (error) {
    std::cerr << "failed to create mixed clobber return prototype JSON: "
              << error.message() << '\n';
    return false;
  }
  {
    llvm::raw_fd_ostream json(jsonFd, true);
    json << R"({"free":{"return":"void","fixed_args":1}})";
  }
  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  options.ExternalPrototypeJsonPath = jsonPath.str().str();
  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module, options);
  (void)llvm::sys::fs::remove(jsonPath);
  llvm::Function *rewritten =
      module.getFunction("notdec_native_clobber_return_phi");
  bool helperLeft = false;
  for (llvm::Function &function : module) {
    if (function.getName().starts_with("notdec.register.summary_clobber")) {
      helperLeft |= !function.use_empty();
    }
  }
  bool hasMixedReturnPhi = false;
  if (rewritten != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewritten)) {
      auto *phi = llvm::dyn_cast<llvm::PHINode>(&inst);
      if (phi == nullptr || !phi->getType()->isIntegerTy(64) ||
          phi->getNumIncomingValues() < 2) {
        continue;
      }
      bool hasConcreteIncoming = false;
      bool hasClobberIncoming = false;
      for (llvm::Value *incoming : phi->incoming_values()) {
        hasConcreteIncoming |= llvm::isa<llvm::ConstantInt>(incoming);
        hasClobberIncoming |= valueNameContains(incoming, "summary_clobber") ||
                              valueNameContains(incoming, "RAX.clobber");
      }
      hasMixedReturnPhi |= hasConcreteIncoming && hasClobberIncoming;
    }
  }
  bool hasMixedWarning = std::any_of(
      summary.Warnings.begin(), summary.Warnings.end(),
      [](const notdec::bin2llvm::NativeRegisterEliminationWarning &warning) {
        return warning.FunctionName == "notdec_native_clobber_return_phi" &&
               warning.RegisterName == "RAX" &&
               warning.Reason == "return_binding_preserved_mixed_clobber";
      });
  return expect(rewritten != nullptr,
                "mixed clobber return callee missing after rewrite") &&
         expect(rewritten->getReturnType()->isIntegerTy(64),
                "mixed clobber return did not infer an i64 return") &&
         expect(hasMixedReturnPhi,
                "mixed clobber return path was collapsed instead of preserving PHI") &&
         expect(helperLeft,
                "mixed clobber return path did not preserve summary_clobber") &&
         expect(hasMixedWarning,
                "mixed clobber return preservation warning was not emitted") &&
         verifyOk(module,
                  "module failed verifier after mixed clobber return phi test");
}

bool testConsumedScratchRegisterIsNotAnExternalArgument() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-scratch-register-arity", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX"});

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "scratch_arity", module);
  llvm::Function *function =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "scratch_arity_callers", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::AllocaInst *saved =
      builder.CreateAlloca(llvm::Type::getInt64Ty(context), nullptr, "saved");
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 4),
                "RDX");
  // RCX is only materialized for the frame store; at the call it is stale.
  storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 3),
                "RCX");
  llvm::LoadInst *scratch = loadRegister(builder, rcx, "RCX", "scratch");
  builder.CreateStore(scratch, saved);
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  (void)notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("scratch_arity");
  return expect(rewritten != nullptr, "scratch arity callee missing") &&
         expect(rewritten->arg_size() == 3,
                "consumed scratch register was counted as an argument") &&
         verifyOk(module,
                  "module failed verifier after scratch register arity test");
}

bool testFlagOnlyRegisterReadKeepsArgumentArity() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-flag-only-read-arity", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX"});

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "flag_only_arity", module);
  llvm::Function *function = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "flag_only_arity_callers",
      module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 4),
                "RDX");
  storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 3),
                "RCX");
  llvm::LoadInst *checked = loadRegister(builder, rcx, "RCX", "checked");
  builder.CreateICmpEQ(checked,
                       llvm::ConstantInt::get(rcx->getValueType(), 0));
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  (void)notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("flag_only_arity");
  return expect(rewritten != nullptr, "flag-only arity callee missing") &&
         expect(rewritten->arg_size() == 4,
                "flag-only (test/cmp) read consumed the argument register") &&
         verifyOk(module, "module failed verifier after flag-only arity test");
}

bool testIndirectCallWithoutEvidenceHasNoArguments() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-indirect-no-evidence", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  (void)createRegisterGlobal(module, "RDI");
  (void)createRegisterGlobal(module, "RSI");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "indirect_no_evidence",
      module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  // 只取函数指针，不写任何实参寄存器。
  llvm::LoadInst *target = loadRegister(builder, rax, "RAX", "target");
  llvm::Value *targetPointer =
      builder.CreateIntToPtr(target, llvm::PointerType::get(context, 0));
  builder.CreateCall(voidType, targetPointer);
  builder.CreateRetVoid();

  (void)notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::CallInst *rewrittenCall = nullptr;
  if (llvm::Function *rewritten = module.getFunction("indirect_no_evidence")) {
    for (llvm::Instruction &inst : llvm::instructions(*rewritten)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() == nullptr) {
        rewrittenCall = call;
      }
    }
  }
  return expect(rewrittenCall != nullptr, "indirect call disappeared") &&
         expect(rewrittenCall->arg_empty(),
                "indirect call without argument evidence kept ABI-input "
                "arguments") &&
         verifyOk(module,
                  "module failed verifier after indirect no-evidence test");
}

bool testConsumedRegisterBeforeLaterArgumentKeepsArity() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-consumed-register-middle", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
  llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");
  llvm::GlobalVariable *r9 = createRegisterGlobal(module, "R9");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "middle_arity", module);
  llvm::Function *function =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "middle_arity_callers", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  storeRegister(builder, rcx, llvm::ConstantInt::get(rcx->getValueType(), 3),
                "RCX");
  storeRegister(builder, r8, llvm::ConstantInt::get(r8->getValueType(), 4),
                "R8");
  llvm::LoadInst *copied = loadRegister(builder, rcx, "RCX", "copied");
  storeRegister(builder, rdx, copied, "RDX");
  storeRegister(builder, r9, llvm::ConstantInt::get(r9->getValueType(), 5),
                "R9");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  (void)notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("middle_arity");
  return expect(rewritten != nullptr, "middle arity callee missing") &&
         expect(rewritten->arg_size() == 6,
                "consumed argument register dropped later argument evidence") &&
         verifyOk(module, "module failed verifier after middle arity test");
}

bool testExternalPrototypeJsonOverridesInferredArity() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-external-json-priority", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI"});

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "json_known_external", module);
  llvm::Function *function =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "json_known_external_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  llvm::SmallString<128> jsonPath;
  int jsonFd = -1;
  std::error_code error = llvm::sys::fs::createTemporaryFile(
      "notdec-external-prototype", "json", jsonFd, jsonPath);
  if (error) {
    std::cerr << "failed to create external prototype JSON: " << error.message()
              << '\n';
    return false;
  }
  {
    llvm::raw_fd_ostream json(jsonFd, true);
    json << R"({"json_known_external":{"return":"void","fixed_args":1}})";
  }

  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  options.ExternalPrototypeJsonPath = jsonPath.str().str();
  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module, options);
  (void)llvm::sys::fs::remove(jsonPath);

  llvm::Function *rewritten = module.getFunction("json_known_external");
  bool inferredWarning = false;
  for (const notdec::bin2llvm::NativeRegisterEliminationWarning &warning :
       summary.Warnings) {
    inferredWarning |= warning.CalleeName == "json_known_external" &&
                       warning.Reason == "inferred_unknown_external_arity";
  }
  return expect(rewritten != nullptr,
                "JSON prototype external callee missing") &&
         expect(rewritten->arg_size() == 1,
                "callsite inference overrode JSON prototype arity") &&
         expect(!inferredWarning,
                "JSON prototype external emitted inference warning") &&
         verifyOk(module,
                  "module failed verifier after JSON prototype priority test");
}

bool testExternalPrototypeJsonAcceptsLongDouble() {
  llvm::SmallString<128> jsonPath;
  int jsonFd = -1;
  std::error_code error = llvm::sys::fs::createTemporaryFile(
      "notdec-external-long-double-prototype", "json", jsonFd, jsonPath);
  if (error) {
    std::cerr << "failed to create long double prototype JSON: "
              << error.message() << '\n';
    return false;
  }
  {
    llvm::raw_fd_ostream json(jsonFd, true);
    json << R"({"json_sqrtl":{"params":["long_double"],"return":"long_double"}})";
  }

  std::string errorMessage;
  std::optional<notdec::bin2llvm::NativeExternalPrototypeMap> prototypes =
      notdec::bin2llvm::loadNativeExternalPrototypesJson(jsonPath,
                                                         errorMessage);
  (void)llvm::sys::fs::remove(jsonPath);
  if (!prototypes) {
    std::cerr << "failed to parse long double prototype JSON: " << errorMessage
              << '\n';
    return false;
  }

  const notdec::bin2llvm::NativeExternalPrototype *prototype =
      notdec::bin2llvm::lookupNativeExternalPrototype(*prototypes,
                                                      "json_sqrtl");
  using ValueType = notdec::bin2llvm::NativeExternalPrototype::ValueType;
  return expect(prototype != nullptr,
                "long double JSON prototype was not loaded") &&
         expect(prototype->FixedArgs == 1 &&
                    prototype->TypedParams ==
                        std::vector<ValueType>{ValueType::LongDouble},
                "long double JSON parameter was not parsed") &&
         expect(prototype->TypedReturn == ValueType::LongDouble,
                "long double JSON return was not parsed");
}

bool testExternalPrototypeJsonAcceptsVector128() {
  llvm::SmallString<128> jsonPath;
  int jsonFd = -1;
  std::error_code error = llvm::sys::fs::createTemporaryFile(
      "notdec-external-vector128-prototype", "json", jsonFd, jsonPath);
  if (error) {
    std::cerr << "failed to create vector128 prototype JSON: "
              << error.message() << '\n';
    return false;
  }
  {
    llvm::raw_fd_ostream json(jsonFd, true);
    json << R"({"json_get128":{"fixed_args":1,"return":"xmm128"}})";
  }

  std::string errorMessage;
  std::optional<notdec::bin2llvm::NativeExternalPrototypeMap> prototypes =
      notdec::bin2llvm::loadNativeExternalPrototypesJson(jsonPath,
                                                         errorMessage);
  (void)llvm::sys::fs::remove(jsonPath);
  if (!prototypes) {
    std::cerr << "failed to parse vector128 prototype JSON: " << errorMessage
              << '\n';
    return false;
  }

  const notdec::bin2llvm::NativeExternalPrototype *prototype =
      notdec::bin2llvm::lookupNativeExternalPrototype(*prototypes,
                                                      "json_get128");
  using ValueType = notdec::bin2llvm::NativeExternalPrototype::ValueType;
  return expect(prototype != nullptr,
                "vector128 JSON prototype was not loaded") &&
         expect(prototype->FixedArgs == 1,
                "vector128 JSON prototype lost fixed_args") &&
         expect(prototype->TypedReturn == ValueType::Vector128,
                "vector128 JSON return was not parsed");
}

bool testExternalPrototypeJsonMultiRegisterReturns() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-external-multi-return", context);
  // ABI：输入 RDI，返回 RAX + XMM0（XMM0 用 128 位寄存器 global 表示）。
  {
    notdec::bin2llvm::NativeAbiSpec abi;
    abi.PrototypeName = "__multi_return_test";
    abi.StackPointerRegister = "RSP";
    abi.StackPointerSpace = "register";
    notdec::bin2llvm::NativeAbiParamEntry input;
    input.MinSize = 1;
    input.MaxSize = 8;
    input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    input.Storage.Name = "RDI";
    abi.Inputs.push_back(input);
    auto addOutput = [&](llvm::StringRef name, uint32_t maxSize) {
      notdec::bin2llvm::NativeAbiParamEntry output;
      output.MinSize = 1;
      output.MaxSize = maxSize;
      output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
      output.Storage.Name = name.str();
      abi.Outputs.push_back(output);
      notdec::bin2llvm::NativeAbiEffect killed;
      killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
      killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
      killed.Storage.Name = name.str();
      abi.Effects.push_back(killed);
    };
    addOutput("RAX", 8);
    addOutput("XMM0", 16);
    for (llvm::StringRef name : {"RBX", "RBP"}) {
      notdec::bin2llvm::NativeAbiEffect unaffected;
      unaffected.Kind = notdec::bin2llvm::NativeAbiEffectKind::Unaffected;
      unaffected.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
      unaffected.Storage.Name = name.str();
      abi.Effects.push_back(unaffected);
    }
    notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
  }

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *xmm0 = createRegisterGlobal(
      module, "XMM0", llvm::IntegerType::get(context, 128), 0, 16);
  auto *sinkType = llvm::IntegerType::get(context, 128);
  auto *sink = new llvm::GlobalVariable(
      module, sinkType, false, llvm::GlobalValue::ExternalLinkage, nullptr,
      "multi_ret_sink");

  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "json_multi_ret", module);
  llvm::Function *caller =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "json_multi_ret_caller", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  builder.CreateCall(calleeType, callee);
  llvm::Value *low = loadRegister(builder, rax, "RAX", "low");
  llvm::Value *high = loadRegister(builder, xmm0, "XMM0", "high");
  llvm::Value *combined = builder.CreateOr(
      builder.CreateZExt(low, sinkType, "low.wide"), high, "combined");
  builder.CreateStore(combined, sink);
  builder.CreateRetVoid();

  llvm::SmallString<128> jsonPath;
  int jsonFd = -1;
  std::error_code error = llvm::sys::fs::createTemporaryFile(
      "notdec-external-multi-return", "json", jsonFd, jsonPath);
  if (error) {
    std::cerr << "failed to create multi-return prototype JSON: "
              << error.message() << '\n';
    return false;
  }
  {
    llvm::raw_fd_ostream json(jsonFd, true);
    json << R"({"json_multi_ret":{"params":["i64"],"returns":[)"
            R"({"register":"RAX","width":64},{"register":"XMM0","width":128}]}})";
  }

  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  options.ExternalPrototypeJsonPath = jsonPath.str().str();
  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module, options);
  (void)llvm::sys::fs::remove(jsonPath);

  llvm::Function *rewritten = module.getFunction("json_multi_ret");
  bool callHasTwoReturns = false;
  llvm::Function *rewrittenCaller =
      module.getFunction("json_multi_ret_caller");
  if (rewrittenCaller == nullptr) {
    return false;
  }
  for (llvm::Instruction &inst : llvm::instructions(*rewrittenCaller)) {
    if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      if (call->getCalledFunction() == rewritten) {
        auto *structType = llvm::dyn_cast<llvm::StructType>(call->getType());
        callHasTwoReturns = structType != nullptr &&
                            structType->getNumElements() == 2 &&
                            structType->getElementType(0)->isIntegerTy(64) &&
                            structType->getElementType(1)->isIntegerTy(128);
      }
    }
  }
  (void)summary;
  if (!callHasTwoReturns) {
    module.print(llvm::errs(), nullptr);
  }
  return expect(rewritten != nullptr, "multi-return callee missing") &&
         expect(rewritten->arg_size() == 1,
                "multi-return prototype lost its parameter") &&
         expect(callHasTwoReturns,
                "multi-return prototype did not produce a 2-value return") &&
         verifyOk(module, "module failed verifier after multi-return test");
}

bool testPinnedSignatureOverridesInference() {
  llvm::LLVMContext context;
  llvm::Module module("pinned-signature", context);
  attachTestAbiWithInputs(module, {"RDI"});

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      voidType, llvm::GlobalValue::InternalLinkage, "pinned_target", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *value = loadRegister(builder, rdi, "RDI", "in");
  storeRegister(builder, rax, value, "RAX");
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  notdec::bin2llvm::NativeSignaturePin pin;
  pin.Params.push_back(notdec::bin2llvm::NativeRegisterSlotSpec{"RDI", 32});
  pin.Returns.push_back(notdec::bin2llvm::NativeRegisterSlotSpec{"RAX", 32});
  options.PinnedSignatures.emplace("pinned_target", pin);
  (void)notdec::bin2llvm::runNativeRegisterElimination(module, options);

  llvm::Function *rewritten = module.getFunction("pinned_target");
  if (rewritten == nullptr || !rewritten->getReturnType()->isIntegerTy(32) ||
      rewritten->arg_size() != 1 ||
      !rewritten->getArg(0)->getType()->isIntegerTy(32)) {
    module.print(llvm::errs(), nullptr);
  }
  return expect(rewritten != nullptr, "pinned function missing") &&
         expect(rewritten->getReturnType()->isIntegerTy(32),
                "pinned return width was not applied") &&
         expect(rewritten->arg_size() == 1 &&
                    rewritten->getArg(0)->getType()->isIntegerTy(32),
                "pinned parameter width was not applied") &&
         verifyOk(module, "module failed verifier after pinned signature test");
}

bool testUnknownExternalArityStopsAtClobberArg() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unknown-external-clobber-arity", context);

  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_unknown_external_clobber_arity_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";
  for (llvm::StringRef name : {"RDI", "RSI", "RDX"}) {
    notdec::bin2llvm::NativeAbiParamEntry input;
    input.MinSize = 1;
    input.MaxSize = 8;
    input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    input.Storage.Name = name.str();
    abi.Inputs.push_back(input);
  }
  for (llvm::StringRef name : {"RAX", "RDX"}) {
    notdec::bin2llvm::NativeAbiEffect killed;
    killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
    killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    killed.Storage.Name = name.str();
    abi.Effects.push_back(killed);
  }
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  (void)rdx;

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *knownClobber = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "free", module);
  llvm::Function *unknown =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external_clobber_arity", module);
  llvm::Function *function =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external_clobber_arity_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);

  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 99),
                "RDI");
  builder.CreateCall(voidType, knownClobber);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  builder.CreateCall(voidType, unknown);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten =
      module.getFunction("unknown_external_clobber_arity");
  bool hasInferredArityWarning = false;
  for (const notdec::bin2llvm::NativeRegisterEliminationWarning &warning :
       summary.Warnings) {
    hasInferredArityWarning |=
        warning.CalleeName == "unknown_external_clobber_arity" &&
        warning.Reason == "inferred_unknown_external_arity";
  }

  return expect(rewritten != nullptr,
                "unknown external clobber arity callee missing") &&
         expect(rewritten->arg_size() == 2,
                "unknown external arity counted clobber as argument") &&
         expect(hasInferredArityWarning,
                "unknown external clobber arity warning missing") &&
         verifyOk(module, "module failed verifier after clobber arity test");
}

bool testUnknownExternalArityStopsAtPhiClobberArg() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unknown-external-phi-clobber-arity",
                      context);

  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_unknown_external_phi_clobber_arity_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";
  for (llvm::StringRef name : {"RDI", "RSI", "RDX"}) {
    notdec::bin2llvm::NativeAbiParamEntry input;
    input.MinSize = 1;
    input.MaxSize = 8;
    input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    input.Storage.Name = name.str();
    abi.Inputs.push_back(input);
  }
  for (llvm::StringRef name : {"RAX", "RDX"}) {
    notdec::bin2llvm::NativeAbiEffect killed;
    killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
    killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    killed.Storage.Name = name.str();
    abi.Effects.push_back(killed);
  }
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *knownClobber = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "free", module);
  llvm::Function *unknown =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external_phi_clobber_arity", module);
  llvm::Function *function = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage,
      "unknown_external_phi_clobber_arity_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *clobberPath =
      llvm::BasicBlock::Create(context, "clobber_path", function);
  llvm::BasicBlock *cleanPath =
      llvm::BasicBlock::Create(context, "clean_path", function);
  llvm::BasicBlock *join = llvm::BasicBlock::Create(context, "join", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), clobberPath,
                       cleanPath);

  builder.SetInsertPoint(clobberPath);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 99),
                "RDI");
  builder.CreateCall(voidType, knownClobber);
  builder.CreateBr(join);

  builder.SetInsertPoint(cleanPath);
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 0),
                "RDX");
  builder.CreateBr(join);

  builder.SetInsertPoint(join);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  builder.CreateCall(voidType, unknown);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten =
      module.getFunction("unknown_external_phi_clobber_arity");
  bool hasInferredArityWarning = false;
  for (const notdec::bin2llvm::NativeRegisterEliminationWarning &warning :
       summary.Warnings) {
    hasInferredArityWarning |=
        warning.CalleeName == "unknown_external_phi_clobber_arity" &&
        warning.Reason == "inferred_unknown_external_arity";
  }

  return expect(rewritten != nullptr,
                "unknown external phi clobber arity callee missing") &&
         expect(rewritten->arg_size() == 2,
                "unknown external arity counted phi clobber as argument") &&
         expect(hasInferredArityWarning,
                "unknown external phi clobber arity warning missing") &&
         verifyOk(module,
                  "module failed verifier after phi clobber arity test");
}

bool testUnknownExternalArityCountsExplicitBinaryArg() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unknown-external-binary-clobber-arity",
                      context);

  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName =
      "__summary_ssa_unknown_external_binary_clobber_arity_test";
  abi.StackPointerRegister = "RSP";
  abi.StackPointerSpace = "register";
  for (llvm::StringRef name : {"RDI", "RSI", "RDX"}) {
    notdec::bin2llvm::NativeAbiParamEntry input;
    input.MinSize = 1;
    input.MaxSize = 8;
    input.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    input.Storage.Name = name.str();
    abi.Inputs.push_back(input);
  }
  for (llvm::StringRef name : {"RAX", "RDX"}) {
    notdec::bin2llvm::NativeAbiEffect killed;
    killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
    killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
    killed.Storage.Name = name.str();
    abi.Effects.push_back(killed);
  }
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);

  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *knownClobber = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "free", module);
  llvm::Function *unknown =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external_binary_clobber_arity", module);
  llvm::Function *function = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage,
      "unknown_external_binary_clobber_arity_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);

  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 99),
                "RDI");
  builder.CreateCall(voidType, knownClobber);
  llvm::LoadInst *rdxAfter = loadRegister(builder, rdx, "RDX", "rdx.after");
  llvm::Value *combined = builder.CreateOr(
      rdxAfter, llvm::ConstantInt::get(rdx->getValueType(), 0x100),
      "rdx.combined");
  storeRegister(builder, rdx, combined, "RDX");
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 1),
                "RDI");
  storeRegister(builder, rsi, llvm::ConstantInt::get(rsi->getValueType(), 2),
                "RSI");
  builder.CreateCall(voidType, unknown);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten =
      module.getFunction("unknown_external_binary_clobber_arity");
  bool hasInferredArityWarning = false;
  for (const notdec::bin2llvm::NativeRegisterEliminationWarning &warning :
       summary.Warnings) {
    hasInferredArityWarning |=
        warning.CalleeName == "unknown_external_binary_clobber_arity" &&
        warning.Reason == "inferred_unknown_external_arity";
  }

  return expect(rewritten != nullptr,
                "unknown external binary clobber arity callee missing") &&
         expect(rewritten->arg_size() == 3,
                "unknown external arity missed explicit binary argument") &&
         expect(hasInferredArityWarning,
                "unknown external binary clobber arity warning missing") &&
         verifyOk(module,
                  "module failed verifier after binary clobber arity test");
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
  auto *type = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "call_arg_cleanup", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *path =
      builder.CreateAdd(function->getArg(0),
                        llvm::ConstantInt::get(rdi->getValueType(), 7), "path");
  llvm::Value *deadValue = builder.CreateXor(
      path, llvm::ConstantInt::get(rdi->getValueType(), 1), "dead.value");
  storeRegister(builder, rbx, deadValue, "RBX");
  storeRegister(builder, rdi, path, "RDI");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
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

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
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

bool testAddressTakenInternalFunctionUseFollowsRewrite() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-address-taken-function-rewrite", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage,
      "notdec_native_address_taken_callee", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(calleeEntry);
  llvm::LoadInst *input = loadRegister(builder, rdi, "RDI", "input");
  storeRegister(builder, rax, input, "RAX");
  builder.CreateRetVoid();

  llvm::Function *caller = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage,
      "notdec_native_address_taken_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 7),
                "RDI");
  builder.CreateCall(voidType, callee);
  llvm::LoadInst *result = loadRegister(builder, rax, "RAX", "result");
  (void)result;
  auto *slot = builder.CreateIntToPtr(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0x4000),
      llvm::PointerType::get(context, 0));
  builder.CreateStore(
      llvm::ConstantExpr::getPtrToInt(callee,
                                      llvm::Type::getInt64Ty(context)),
      slot);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten =
      module.getFunction("notdec_native_address_taken_callee");
  llvm::Function *rewrittenCaller =
      module.getFunction("notdec_native_address_taken_caller");
  bool addressUseRewritten = false;
  if (rewrittenCaller != nullptr && rewritten != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenCaller)) {
      for (llvm::Value *operand : inst.operands()) {
        auto *constant = llvm::dyn_cast<llvm::ConstantExpr>(operand);
        if (constant != nullptr &&
            constant->getOpcode() == llvm::Instruction::PtrToInt &&
            constant->getOperand(0)->stripPointerCasts() == rewritten) {
          addressUseRewritten = true;
        }
      }
    }
  }

  return expect(rewritten != nullptr, "rewritten address-taken callee missing") &&
         expect(rewrittenCaller != nullptr,
                "rewritten address-taken caller missing") &&
         expect(addressUseRewritten,
                "ptrtoint use still referenced the old function") &&
         expect(summary.FunctionsRewritten >= 1,
                "address-taken function was not rewritten") &&
         verifyOk(module,
                  "module failed verifier after address-taken rewrite");
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

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten =
      module.getFunction("notdec_native_non_abi_return");
  bool helperLeft = false;
  bool callReturnsRBX = false;
  for (llvm::Function &function : module) {
    if (function.getName().starts_with("notdec.register.summary_return")) {
      helperLeft |= !function.use_empty();
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

bool testInternalCalleeReturnsCallerSavedRegister() {
  auto build = [](llvm::LLVMContext &context, llvm::Module &module) {
    attachTestAbi(module);
    llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
    auto *i64Type = llvm::Type::getInt64Ty(context);
    auto *sink = new llvm::GlobalVariable(
        module, i64Type, false, llvm::GlobalValue::ExternalLinkage, nullptr,
        "sink");
    auto *flag = new llvm::GlobalVariable(
        module, llvm::Type::getInt1Ty(context), false,
        llvm::GlobalValue::ExternalLinkage, nullptr, "flag");

    auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
    llvm::Function *callee = llvm::Function::Create(
        voidType, llvm::GlobalValue::InternalLinkage, "rcx_mixed_callee",
        module);
    llvm::BasicBlock *calleeEntry =
        llvm::BasicBlock::Create(context, "entry", callee);
    llvm::BasicBlock *calleeWrite =
        llvm::BasicBlock::Create(context, "write", callee);
    llvm::BasicBlock *calleeDone =
        llvm::BasicBlock::Create(context, "done", callee);
    llvm::IRBuilder<> calleeBuilder(calleeEntry);
    llvm::Value *calleeCond =
        calleeBuilder.CreateLoad(llvm::Type::getInt1Ty(context), flag, "cond");
    calleeBuilder.CreateCondBr(calleeCond, calleeWrite, calleeDone);
    calleeBuilder.SetInsertPoint(calleeWrite);
    storeRegister(calleeBuilder, rcx, llvm::ConstantInt::get(i64Type, 7), "RCX");
    calleeBuilder.CreateBr(calleeDone);
    calleeBuilder.SetInsertPoint(calleeDone);
    calleeBuilder.CreateRetVoid();

    llvm::Function *caller = llvm::Function::Create(
        voidType, llvm::GlobalValue::InternalLinkage, "rcx_mixed_caller",
        module);
    llvm::BasicBlock *callerEntry =
        llvm::BasicBlock::Create(context, "entry", caller);
    llvm::IRBuilder<> builder(callerEntry);
    builder.CreateCall(voidType, callee);
    llvm::Value *after = loadRegister(builder, rcx, "RCX", "after");
    builder.CreateStore(after, sink);
    builder.CreateRetVoid();
    return rcx;
  };

  // 默认：接口可改 ⇒ callee 的签名带 RCX 返回槽，调用点不再产生 RCX 的 unknown。
  {
    llvm::LLVMContext context;
    llvm::Module module("summary-ssa-internal-caller-saved-return", context);
    llvm::GlobalVariable *rcx = build(context, module);
    auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
    llvm::Function *rewritten = module.getFunction("rcx_mixed_callee");
    bool callReturnsRCX = false;
    if (llvm::Function *rewrittenCaller =
            module.getFunction("rcx_mixed_caller")) {
      for (llvm::Instruction &inst : llvm::instructions(*rewrittenCaller)) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
        if (call != nullptr && call->getCalledFunction() == rewritten &&
            call->getType() == rcx->getValueType()) {
          callReturnsRCX = true;
        }
      }
    }
    std::map<std::string, uint64_t> counts =
        notdec::bin2llvm::countRemainingUnknownValues(module);
    bool internalMarker = false;
    if (rewritten != nullptr) {
      if (llvm::MDNode *node = rewritten->getMetadata("notdec.abi")) {
        for (const llvm::MDOperand &operand : node->operands()) {
          if (auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get())) {
            internalMarker |= text->getString() == "conventions=internal";
          }
        }
      }
    }
    if (!expect(rewritten != nullptr, "caller-saved return callee missing") ||
        !expect(rewritten->getReturnType() == rcx->getValueType(),
                "internal caller-saved return was not added to signature") ||
        !expect(callReturnsRCX,
                "internal caller-saved return callsite was not rewritten") ||
        !expect(internalMarker,
                "internal function was not marked as internal convention") ||
        !expect(counts.count("call_effect_mixed:rcx_mixed_callee:RCX") == 0,
                "internal caller-saved return still produced a mixed unknown") ||
        !expect(summary.CallsRewritten >= 1,
                "caller-saved return call rewrite was not counted") ||
        !verifyOk(module, "module failed verifier after caller-saved return")) {
      return false;
    }
  }

  // 开关打开：所有函数都遵守 ABI ⇒ 不出现 RCX 返回槽，回到"值丢了"的路径。
  {
    llvm::LLVMContext context;
    llvm::Module module("summary-ssa-abi-for-all-caller-saved", context);
    llvm::GlobalVariable *rcx = build(context, module);
    notdec::bin2llvm::NativeRegisterEliminationOptions options;
    options.AbiForAllFunctions = true;
    (void)notdec::bin2llvm::runNativeRegisterElimination(module, options);
    llvm::Function *rewritten = module.getFunction("rcx_mixed_callee");
    bool externalMarker = false;
    if (rewritten != nullptr) {
      if (llvm::MDNode *node = rewritten->getMetadata("notdec.abi")) {
        for (const llvm::MDOperand &operand : node->operands()) {
          if (auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get())) {
            externalMarker |= text->getString() == "conventions=external";
          }
        }
      }
    }
    if (!expect(rewritten != nullptr, "abi-for-all callee missing") ||
        !expect(rewritten->getReturnType() ==
                    llvm::Type::getVoidTy(context),
                "abi-for-all must not add caller-saved return slot") ||
        !expect(externalMarker,
                "abi-for-all function was not marked as external convention") ||
        !expect(rcx != nullptr, "register global missing") ||
        !verifyOk(module, "module failed verifier after abi-for-all test")) {
      return false;
    }
  }
  return true;
}

bool testInternalSignatureRewriteUsesReadEntryReturnRegisterArg() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-entry-return-register-arg", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  // 接口可改（本地链接 + 地址没被取）的内部函数才能用自定义约定，把入口 RAX 变成形参。
  llvm::Function *function =
      llvm::Function::Create(voidType, llvm::GlobalValue::InternalLinkage,
                             "notdec_native_read_entry_rax", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::AllocaInst *sink = builder.CreateAlloca(rax->getValueType());
  llvm::LoadInst *input = loadRegister(builder, rax, "RAX", "input");
  builder.CreateStore(input, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten =
      module.getFunction("notdec_native_read_entry_rax");
  unsigned raxLoads = 0;
  if (rewritten != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewritten)) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
      if (load != nullptr &&
          load->getPointerOperand()->stripPointerCasts() == rax) {
        ++raxLoads;
      }
    }
  }

  return expect(rewritten != nullptr, "RAX entry callee missing") &&
         expect(rewritten->arg_size() == 1,
                "read-entry return register was not added as argument") &&
         expect(rewritten->getArg(0)->getType() == rax->getValueType(),
                "RAX entry argument had wrong type") &&
         expect(raxLoads == 0, "RAX entry load remained") &&
         verifyOk(module,
                  "module failed verifier after RAX entry argument test");
}

bool testInternalSignatureShapeParamKeepsOverwrittenEntryRange() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-overwritten-entry-range-param", context);
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
                             "notdec_native_overwrites_rdi32", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *input = builder.CreateCall(
      partialRead,
      {rdi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "input32");
  builder.CreateStore(input, sink);
  storeRegister(builder, rdi, llvm::ConstantInt::get(rdi->getValueType(), 0),
                "RDI");
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten =
      module.getFunction("notdec_native_overwrites_rdi32");
  bool hasUnknownValue = false;
  if (rewritten != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewritten)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      hasUnknownValue |=
          call != nullptr && call->getCalledFunction() != nullptr &&
          call->getCalledFunction()->getName().starts_with("notdec.unknown.");
    }
  }

  return expect(rewritten != nullptr, "overwritten RDI entry callee missing") &&
         expect(rewritten->arg_size() == 1,
                "overwritten RDI entry range was not added as argument") &&
         expect(rewritten->getArg(0)->getType()->isIntegerTy(32),
                "overwritten RDI entry argument was not i32") &&
         expect(!hasPartialReadCall(*rewritten),
                "overwritten RDI entry left partial read helper") &&
         expect(!hasUnknownValue,
                "overwritten RDI entry was materialized as unknown") &&
         verifyOk(module,
                  "module failed verifier after overwritten entry range test");
}

bool testPostSignatureCleanupRewritesInternalEntryRawLoad() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-post-signature-entry-load", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_entry_raw_load", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right =
      llvm::BasicBlock::Create(context, "right", function);
  llvm::BasicBlock *merge =
      llvm::BasicBlock::Create(context, "merge", function);

  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *input = loadRegister(builder, rdi, "RDI", "input");
  llvm::Value *loadedPtr = builder.CreateAdd(
      input, llvm::ConstantInt::get(rdi->getValueType(), 16), "loaded_ptr");
  llvm::Value *loadedPtrAsPointer = builder.CreateIntToPtr(
      loadedPtr, llvm::PointerType::get(context, 0), "loaded_ptr_as_pointer");
  llvm::LoadInst *loaded =
      builder.CreateLoad(rdi->getValueType(), loadedPtrAsPointer, "loaded");
  llvm::Value *same = builder.CreateICmpEQ(
      loaded, llvm::ConstantInt::get(rdi->getValueType(), 0));
  builder.CreateCondBr(same, left, right);

  builder.SetInsertPoint(left);
  builder.CreateBr(merge);

  builder.SetInsertPoint(right);
  builder.CreateBr(merge);

  builder.SetInsertPoint(merge);
  llvm::PHINode *phi = builder.CreatePHI(rdi->getValueType(), 2, "merged");
  phi->addIncoming(input, left);
  phi->addIncoming(loaded, right);
  llvm::AllocaInst *sink = builder.CreateAlloca(rdi->getValueType());
  builder.CreateStore(phi, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten =
      module.getFunction("notdec_native_entry_raw_load");
  unsigned rdiLoads = 0;
  if (rewritten != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewritten)) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
      if (load != nullptr &&
          load->getPointerOperand()->stripPointerCasts() == rdi) {
        ++rdiLoads;
      }
    }
  }

  return expect(rewritten != nullptr, "post-signature callee missing") &&
         expect(rewritten->arg_size() == 1,
                "post-signature callee did not get RDI arg") &&
         expect(rdiLoads == 0, "post-signature cleanup left raw RDI load") &&
         verifyOk(
             module,
             "module failed verifier after post-signature entry load test");
}

bool testInternalSignatureRewriteUsesZmmArgAndReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-internal-zmm-signature", context);
  attachTestFloatAbi(module, 1);
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_zmm_callee", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(calleeEntry);
  llvm::LoadInst *input = loadRegister(builder, zmm0, "ZMM0", "input");
  llvm::Value *result =
      builder.CreateXor(input, llvm::ConstantInt::get(zmmType, 1), "result");
  storeRegister(builder, zmm0, result, "ZMM0");
  builder.CreateRetVoid();

  llvm::Function *caller =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_zmm_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  storeRegister(builder, zmm0, llvm::ConstantInt::get(zmmType, 7), "ZMM0");
  builder.CreateCall(voidType, callee);
  llvm::LoadInst *loaded = loadRegister(builder, zmm0, "ZMM0", "loaded");
  (void)loaded;
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("notdec_native_zmm_callee");
  llvm::Function *rewrittenCaller =
      module.getFunction("notdec_native_zmm_caller");
  bool callRewritten = false;
  unsigned zmmStores = 0;
  unsigned zmmLoads = 0;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenCaller)) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (call->getCalledFunction() == rewritten && call->arg_size() == 1 &&
            call->getArgOperand(0)->getType() == zmmType &&
            call->getType() == zmmType) {
          callRewritten = true;
        }
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        if (store->getPointerOperand()->stripPointerCasts() == zmm0) {
          ++zmmStores;
        }
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        if (load->getPointerOperand()->stripPointerCasts() == zmm0) {
          ++zmmLoads;
        }
      }
    }
  }

  return expect(rewritten != nullptr, "ZMM internal callee missing") &&
         expect(rewrittenCaller != nullptr, "ZMM internal caller missing") &&
         expect(rewritten->arg_size() == 1,
                "ZMM internal argument was not rewritten") &&
         expect(rewritten->getArg(0)->getType() == zmmType,
                "ZMM internal argument was not i512") &&
         expect(rewritten->getReturnType() == zmmType,
                "ZMM internal return was not i512") &&
         expect(callRewritten, "ZMM internal callsite was not rewritten") &&
         expect(zmmStores == 0, "ZMM caller argument store remained") &&
         expect(zmmLoads == 0, "ZMM caller result load remained") &&
         expect(summary.CallsRewritten >= 1,
                "ZMM internal call rewrite was not counted") &&
         verifyOk(module, "module failed verifier after ZMM signature rewrite");
}

bool testPreservedZmmEntryIsPassedAsInternalArgument() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-preserved-zmm-entry-arg", context);
  attachTestFloatAbi(module, 1);
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *leaf =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_zmm_leaf", module);
  llvm::BasicBlock *leafEntry =
      llvm::BasicBlock::Create(context, "entry", leaf);
  llvm::IRBuilder<> builder(leafEntry);
  llvm::AllocaInst *sink = builder.CreateAlloca(zmmType);
  llvm::LoadInst *input = loadRegister(builder, zmm0, "ZMM0", "input");
  builder.CreateStore(input, sink);
  builder.CreateRetVoid();

  llvm::Function *passthrough =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_zmm_passthrough", module);
  llvm::BasicBlock *passthroughEntry =
      llvm::BasicBlock::Create(context, "entry", passthrough);
  builder.SetInsertPoint(passthroughEntry);
  builder.CreateCall(voidType, leaf);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewrittenLeaf = module.getFunction("notdec_native_zmm_leaf");
  llvm::Function *rewrittenPassthrough =
      module.getFunction("notdec_native_zmm_passthrough");
  bool callUsesPassthroughArg = false;
  unsigned zmmLoads = 0;
  if (rewrittenPassthrough != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenPassthrough)) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        callUsesPassthroughArg =
            call->getCalledFunction() == rewrittenLeaf &&
            call->arg_size() == 1 && rewrittenPassthrough->arg_size() == 1 &&
            call->getArgOperand(0) == rewrittenPassthrough->getArg(0);
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        if (load->getPointerOperand()->stripPointerCasts() == zmm0) {
          ++zmmLoads;
        }
      }
    }
  }

  return expect(rewrittenLeaf != nullptr, "ZMM leaf missing") &&
         expect(rewrittenPassthrough != nullptr, "ZMM passthrough missing") &&
         expect(rewrittenLeaf->arg_size() == 1,
                "ZMM leaf argument was not rewritten") &&
         expect(rewrittenPassthrough->arg_size() == 1,
                "ZMM passthrough did not get preserved entry argument") &&
         expect(rewrittenPassthrough->getArg(0)->getType() == zmmType,
                "ZMM passthrough argument was not i512") &&
         expect(callUsesPassthroughArg,
                "ZMM passthrough call did not use its argument") &&
         expect(zmmLoads == 0, "ZMM passthrough entry load remained") &&
         verifyOk(
             module,
             "module failed verifier after preserved ZMM entry argument test");
}

bool testInternalSignatureRewriteUsesZmmLowLaneReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-internal-zmm-low-lane-return", context);
  attachTestFloatAbi(module, 0);
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_zmm_low_return", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(calleeEntry);
  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, zmm0->getType(), llvm::Type::getInt64Ty(context), 512, 64);
  builder.CreateCall(
      partialWrite,
      {zmm0,
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context),
                              0x3ff0000000000000ULL),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateRetVoid();

  llvm::Function *caller =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_zmm_low_caller", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  builder.CreateCall(voidType, callee);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, zmm0->getType(), llvm::Type::getInt64Ty(context), 512, 64);
  llvm::Value *loaded = builder.CreateCall(
      partialRead,
      {zmm0, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "loaded");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "zmm_low_sink");
  builder.CreateStore(loaded, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten =
      module.getFunction("notdec_native_zmm_low_return");
  llvm::Function *rewrittenCaller =
      module.getFunction("notdec_native_zmm_low_caller");
  bool callReturnsDouble = false;
  if (rewrittenCaller != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() == rewritten &&
          call->arg_empty() && call->getType()->isDoubleTy()) {
        callReturnsDouble = true;
      }
    }
  }

  return expect(rewritten != nullptr, "ZMM low-lane callee missing") &&
         expect(rewrittenCaller != nullptr, "ZMM low-lane caller missing") &&
         expect(rewritten->arg_empty(),
                "ZMM low-lane return added unexpected argument") &&
         expect(rewritten->getReturnType()->isDoubleTy(),
                "ZMM low-lane return was not rewritten to double") &&
         expect(callReturnsDouble,
                "ZMM low-lane callsite was not rewritten to double") &&
         expect(!hasPartialWriteCall(*rewritten),
                "ZMM low-lane partial write helper remained") &&
         expect(summary.CallsRewritten >= 1,
                "ZMM low-lane return call rewrite was not counted") &&
         verifyOk(module,
                  "module failed verifier after ZMM low-lane return rewrite");
}

bool testForeignArgumentInMovedBodyIsReplaced() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-foreign-argument", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *type = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *callee =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_foreign_arg", module);
  callee->getArg(0)->setName("R8.arg");
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rax, callee->getArg(0), "RAX");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("notdec_native_foreign_arg");
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

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
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

bool testInternalCallArgBindingsKeepLaterArgsAfterEntryInput() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-internal-entry-input-call-arg", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_child", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  llvm::LoadInst *childRdi = loadRegister(calleeBuilder, rdi, "RDI", "rdi.in");
  llvm::LoadInst *childRsi = loadRegister(calleeBuilder, rsi, "RSI", "rsi.in");
  llvm::Value *childSum = calleeBuilder.CreateAdd(childRdi, childRsi);
  llvm::AllocaInst *childSink = calleeBuilder.CreateAlloca(rdi->getValueType());
  calleeBuilder.CreateStore(childSum, childSink);
  calleeBuilder.CreateRetVoid();

  auto *parentType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *parent =
      llvm::Function::Create(parentType, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_parent", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", parent);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *rdiEntry = loadRegister(builder, rdi, "RDI", "rdi.entry");
  llvm::Value *rsiValue = builder.CreateAdd(
      rdiEntry, llvm::ConstantInt::get(rsi->getValueType(), 7), "rsi.value");
  storeRegister(builder, rsi, rsiValue, "RSI");
  builder.CreateCall(calleeType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("notdec_native_child");
  llvm::CallInst *call = nullptr;
  if (llvm::Function *rewrittenParent =
          module.getFunction("notdec_native_parent")) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenParent)) {
      auto *candidate = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (candidate != nullptr && candidate->getCalledFunction() == rewritten) {
        call = candidate;
      }
    }
  }

  return expect(rewritten != nullptr, "internal child callee missing") &&
         expect(call != nullptr, "internal child call missing") &&
         expect(
             call->arg_size() == 2,
             "internal child call did not keep later arg after entry input") &&
         expect(call->getArgOperand(1) != nullptr,
                "internal child later arg was dropped") &&
         verifyOk(
             module,
             "module failed verifier after internal entry-input call arg test");
}

bool testInternalSignatureParamsUseAbiOrder() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-internal-param-abi-order", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX", "R8", "R9"});
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rcx = createRegisterGlobal(module, "RCX");
  llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");
  llvm::GlobalVariable *r9 = createRegisterGlobal(module, "R9");
  llvm::GlobalVariable *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0), "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "notdec_native_param_order", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);

  llvm::Value *sum = loadRegister(builder, rcx, "RCX", "rcx.in");
  sum = builder.CreateAdd(sum, loadRegister(builder, rdx, "RDX", "rdx.in"));
  sum = builder.CreateAdd(sum, loadRegister(builder, rsi, "RSI", "rsi.in"));
  sum = builder.CreateAdd(sum, loadRegister(builder, rdi, "RDI", "rdi.in"));
  sum = builder.CreateAdd(sum, loadRegister(builder, r8, "R8", "r8.in"));
  sum = builder.CreateAdd(sum, loadRegister(builder, r9, "R9", "r9.in"));
  builder.CreateStore(sum, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("notdec_native_param_order");
  std::vector<llvm::StringRef> expected = {"RDI.arg", "RSI.arg", "RDX.arg",
                                           "RCX.arg", "R8.arg",  "R9.arg"};
  bool arityOk = rewritten != nullptr && rewritten->arg_size() == 6;
  bool orderOk =
      rewritten != nullptr && rewritten->arg_size() == expected.size();
  if (orderOk) {
    for (unsigned index = 0; index < expected.size(); ++index) {
      orderOk &= rewritten->getArg(index)->getName() == expected[index];
    }
  }

  return expect(rewritten != nullptr, "internal ABI order function missing") &&
         expect(arityOk, "internal ABI order function used wrong arity") &&
         expect(orderOk, "internal params were not ordered by ABI") &&
         expect(summary.FunctionsRewritten >= 1,
                "internal ABI order function was not rewritten") &&
         verifyOk(module,
                  "module failed verifier after internal ABI order param test");
}

bool testPostSignatureCleanupDropsAbiStoreBeforeUnrewrittenCall() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-post-cleanup-abi-store", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *calleeType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
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

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
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
  llvm::Function *stackChkFail =
      llvm::Function::Create(noreturnType, llvm::GlobalValue::ExternalLinkage,
                             "__stack_chk_fail", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "calls_noreturn", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(stackChkFail->getFunctionType(), stackChkFail, {});
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "unreachable_rax");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.CallReturnValues == 0,
                "noreturn external path created a summary return helper") &&
         verifyOk(module, "module failed verifier after noreturn cleanup test");
}

bool testExternalPrototypeJsonOverlaysDefaultNoReturn() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-external-json-overlay-noreturn", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *exitType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *exit = llvm::Function::Create(
      exitType, llvm::GlobalValue::ExternalLinkage, "exit", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "json_overlay_keeps_exit", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(
      exitType, exit,
      {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 1)});
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "unreachable_rax");
  builder.CreateRet(loaded);

  llvm::SmallString<128> jsonPath;
  int jsonFd = -1;
  std::error_code error = llvm::sys::fs::createTemporaryFile(
      "notdec-external-prototype-overlay", "json", jsonFd, jsonPath);
  if (error) {
    std::cerr << "failed to create external prototype JSON: " << error.message()
              << '\n';
    return false;
  }
  {
    llvm::raw_fd_ostream json(jsonFd, true);
    json << R"({"json_known_external":{"return":"void","fixed_args":0}})";
  }

  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  options.ExternalPrototypeJsonPath = jsonPath.str().str();
  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module, options);
  (void)llvm::sys::fs::remove(jsonPath);

  return expect(summary.CallReturnValues == 0,
                "custom prototype JSON replaced default exit noreturn") &&
         expect(blockEndsWithUnreachable(*function, "entry"),
                "default exit noreturn was not preserved under JSON overlay") &&
         verifyOk(module, "module failed verifier after JSON overlay noreturn");
}

bool testInternalNoReturnCallFallthroughIsTruncated() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-internal-noreturn-call", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  auto *exitType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *exit = llvm::Function::Create(
      exitType, llvm::GlobalValue::ExternalLinkage, "exit", module);
  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});

  llvm::Function *helper = llvm::Function::Create(
      voidType, llvm::GlobalValue::InternalLinkage, "local_fail", module);
  llvm::BasicBlock *helperEntry =
      llvm::BasicBlock::Create(context, "entry", helper);
  llvm::IRBuilder<> builder(helperEntry);
  builder.CreateCall(
      exitType, exit,
      {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 1)});
  builder.CreateRetVoid();

  auto *callerType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *caller =
      llvm::Function::Create(callerType, llvm::GlobalValue::ExternalLinkage,
                             "calls_local_fail", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  builder.CreateCall(voidType, helper, {});
  llvm::LoadInst *loaded = loadRegister(builder, rax, "RAX", "unreachable_rax");
  builder.CreateRet(loaded);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.CallReturnValues == 0,
                "internal noreturn call created a summary return helper") &&
         expect(helper->hasFnAttribute(llvm::Attribute::NoReturn),
                "internal helper was not marked noreturn") &&
         expect(blockEndsWithUnreachable(*caller, "entry"),
                "internal noreturn call kept fallthrough terminator") &&
         verifyOk(module,
                  "module failed verifier after internal noreturn test");
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
  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
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
      old,
      llvm::ConstantInt::get(zmmType, llvm::APInt::getBitsSet(512, 64, 512)));
  llvm::Value *low = llvm::ConstantInt::get(zmmType, 7);
  storeRegister(builder, zmm0, builder.CreateOr(keep, low), "ZMM0");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  unsigned zmmStores = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    if (store != nullptr &&
        store->getPointerOperand()->stripPointerCasts() == zmm0) {
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

bool testUnknownExternalFloatAbiClobberUsesWholeZmm() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unknown-float-clobber", context);
  attachTestFloatAbi(module, 1);

  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);
  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *external =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external_float_clobber", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "reads_zmm_after_unknown_external", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::AllocaInst *sink = builder.CreateAlloca(zmmType);
  builder.CreateCall(external);
  llvm::Value *loaded = loadRegister(builder, zmm0, "ZMM0", "after");
  builder.CreateStore(loaded, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.CallClobberValues >= 1,
                "float ABI clobber was not modeled") &&
         expect(!moduleHasFunctionNamed(module,
                                        "notdec.register.summary_clobber.i512"),
                "whole-ZMM summary clobber declaration remained") &&
         verifyOk(
             module,
             "module failed verifier after float clobber suppression test");
}

bool testUnknownExternalIntegerClobberUsesRangeCallValue() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-integer-clobber-range", context);
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__summary_ssa_clobber_test";
  notdec::bin2llvm::NativeAbiParamEntry output;
  output.MinSize = 1;
  output.MaxSize = 8;
  output.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  output.Storage.Name = "RAX";
  abi.Outputs.push_back(output);
  notdec::bin2llvm::NativeAbiEffect killed;
  killed.Kind = notdec::bin2llvm::NativeAbiEffectKind::KilledByCall;
  killed.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  killed.Storage.Name = "R10";
  abi.Effects.push_back(killed);
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);

  llvm::GlobalVariable *r10 = createRegisterGlobal(module, "R10");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *external =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_integer_clobber_range", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "reads_rdx_after_unknown_external", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(calleeType, external);
  llvm::Value *loaded = loadRegister(builder, r10, "R10", "after");
  builder.CreateStore(loaded, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  options.EnableResidueRemoval = false;
  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module, options);
  llvm::Function *rewritten =
      module.getFunction("reads_rdx_after_unknown_external");

  return expect(summary.CallClobberValues >= 1,
                "integer clobber range helper was not created") &&
         expect(rewritten != nullptr,
                "integer clobber test function was not preserved") &&
         expect(!hasLiveReplacedRegisterLoad(*rewritten),
                "integer clobber range left live raw R10 load") &&
         expect(moduleHasUsedFunctionNamed(
                    module, "notdec.register.summary_clobber.i1") ||
                    moduleHasUsedFunctionNamed(
                        module, "notdec.register.summary_clobber.i64"),
                "integer clobber range helper was not used") &&
         expect((functionHasUsedSummaryCallValueRange(*function, "clobber",
                                                      "R10", 0, 64) ||
                 (functionHasUsedSummaryCallValueRange(*function, "clobber",
                                                       "R10", 0, 1) &&
                  functionHasUsedSummaryCallValueRange(*function, "clobber",
                                                       "R10", 1, 63))),
                "integer clobber helper did not carry R10 range metadata") &&
         verifyOk(module,
                  "module failed verifier after integer clobber range test");
}

bool testKnownPowUsesFloatAbiSlots() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-pow-float-abi", context);
  attachTestFloatAbi(module, 2);

  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);
  llvm::GlobalVariable *zmm1 =
      createRegisterGlobal(module, "ZMM1", zmmType, 4672, 64);

  auto *oldPowType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *powFunction = llvm::Function::Create(
      oldPowType, llvm::GlobalValue::ExternalLinkage, "pow", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "pow_float_slots", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, zmm0, llvm::ConstantInt::get(zmmType, 1), "ZMM0");
  storeRegister(builder, zmm1, llvm::ConstantInt::get(zmmType, 2), "ZMM1");
  builder.CreateCall(oldPowType, powFunction);
  llvm::Value *powBits =
      builder.CreateTrunc(loadRegister(builder, zmm0, "ZMM0", "pow.after"),
                          llvm::Type::getInt64Ty(context));
  builder.CreateRet(powBits);

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::CallInst *powCall = nullptr;
  unsigned zmmStores = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      if (call->getCalledFunction() != nullptr &&
          call->getCalledFunction()->getName() == "pow") {
        powCall = call;
      }
    }
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      if (store->getPointerOperand()->stripPointerCasts() == zmm0 ||
          store->getPointerOperand()->stripPointerCasts() == zmm1) {
        ++zmmStores;
      }
    }
  }

  return expect(powCall != nullptr, "pow call missing after rewrite") &&
         expect(powCall->getType()->isDoubleTy(),
                "pow return type was not rewritten to double") &&
         expect(powCall->arg_size() == 2,
                "pow did not receive two float ABI arguments") &&
         expect(powCall->getArgOperand(0)->getType()->isDoubleTy() &&
                    powCall->getArgOperand(1)->getType()->isDoubleTy(),
                "pow arguments were not rewritten to double") &&
         expect(zmmStores == 0, "pow ABI ZMM argument stores remained") &&
         expect(summary.CallsRewritten >= 1,
                "pow call was not counted as rewritten") &&
         verifyOk(module, "module failed verifier after pow ABI rewrite test");
}

bool testKnownUnaryLibmUsesFloatAbiSlots() {
  bool ok = true;
  for (const std::string &calleeName :
       std::vector<std::string>{"sqrt", "sin", "cos", "log", "exp"}) {
    llvm::LLVMContext context;
    llvm::Module module("summary-ssa-libm-float-abi", context);
    attachTestFloatAbi(module, 1);

    llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
    llvm::GlobalVariable *zmm0 =
        createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

    auto *oldCalleeType =
        llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
    llvm::Function *callee = llvm::Function::Create(
        oldCalleeType, llvm::GlobalValue::ExternalLinkage, calleeName, module);
    auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
    llvm::Function *function =
        llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                               calleeName + "_float_slots", module);
    llvm::BasicBlock *entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::IRBuilder<> builder(entry);
    storeRegister(builder, zmm0, llvm::ConstantInt::get(zmmType, 1), "ZMM0");
    builder.CreateCall(oldCalleeType, callee);
    llvm::Value *bits =
        builder.CreateTrunc(loadRegister(builder, zmm0, "ZMM0", "libm.after"),
                            llvm::Type::getInt64Ty(context));
    builder.CreateRet(bits);

    auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
    llvm::CallInst *rewrittenCall = nullptr;
    unsigned zmmStores = 0;
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (call->getCalledFunction() != nullptr &&
            (call->getCalledFunction()->getName() == calleeName ||
             (calleeName == "log" &&
              call->getCalledFunction()->getIntrinsicID() ==
                  llvm::Intrinsic::log))) {
          rewrittenCall = call;
        }
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        if (store->getPointerOperand()->stripPointerCasts() == zmm0) {
          ++zmmStores;
        }
      }
    }

    std::string prefix = calleeName + ": ";
    ok &= expect(rewrittenCall != nullptr,
                 (prefix + "call missing after rewrite").c_str());
    if (rewrittenCall != nullptr) {
      ok &= expect(rewrittenCall->getType()->isDoubleTy(),
                   (prefix + "return type was not double").c_str());
      ok &= expect(rewrittenCall->arg_size() == 1,
                   (prefix + "did not receive one argument").c_str());
      ok &= expect(rewrittenCall->getArgOperand(0)->getType()->isDoubleTy(),
                   (prefix + "argument was not double").c_str());
    }
    ok &= expect(zmmStores == 0,
                 (prefix + "ABI ZMM argument store remained").c_str());
    ok &= expect(
        !moduleHasFunctionNamed(module, "notdec.register.summary_return.i64"),
        (prefix + "range return helper remained").c_str());
    ok &= expect(summary.CallsRewritten >= 1,
                 (prefix + "call was not counted as rewritten").c_str());
    ok &= verifyOk(module, (prefix + "module failed verifier").c_str());
  }
  return ok;
}

bool testPartialKeepHighStoreIsDemandRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-demand", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "partial_keep_high", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *old = loadRegister(builder, rdx, "RDX", "old");
  llvm::Value *keep = builder.CreateAnd(
      old, llvm::ConstantInt::get(rdx->getValueType(),
                                  llvm::APInt::getBitsSet(64, 8, 64)));
  llvm::Value *low = llvm::ConstantInt::get(rdx->getValueType(), 7);
  storeRegister(builder, rdx, builder.CreateOr(keep, low), "RDX");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  unsigned rdxLoads = 0;
  unsigned rdxStores = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
      if (load->getPointerOperand()->stripPointerCasts() == rdx) {
        ++rdxLoads;
      }
    }
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      if (store->getPointerOperand()->stripPointerCasts() == rdx) {
        ++rdxStores;
      }
    }
  }

  return expect(summary.PartialDemandCandidates >= 1,
                "partial keep-high store was not seen as a candidate") &&
         expect(summary.PartialDemandMatched >= 1,
                "partial keep-high store was not demand rewritten") &&
         expect(rdxLoads == 0, "partial keep-high old load was not removed") &&
         verifyOk(module,
                  "module failed verifier after partial demand rewrite test");
}

bool testPartialWriteHelperIsConsumedBySummarySSA() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-write-helper", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *sink = new llvm::GlobalVariable(module, rax->getValueType(), false,
                                        llvm::GlobalValue::ExternalLinkage,
                                        nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_write_readback", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 7),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *after = loadRegister(builder, rax, "RAX", "after_partial");
  builder.CreateStore(after, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  // runNativeRegisterElimination 会重写签名替换函数对象，旧指针会悬空，
  // 必须按名字重新取。
  llvm::Function *rewritten = module.getFunction("partial_write_readback");
  return expect(summary.LoadsReplaced == 1,
                "partial write readback load was not replaced") &&
         expect(rewritten == nullptr || !hasPartialWriteCall(*rewritten),
                "partial write helper call remained after summary SSA") &&
         expect(rewritten == nullptr || !hasRegisterLoad(*rewritten, "RAX"),
                "raw RAX load remained after partial write rewrite") &&
         verifyOk(module,
                  "module failed verifier after partial write helper test");
}

bool testPartialReadHelperIsConsumedBySummarySSA() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-read-helper", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_write_then_read", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 7),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *after = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "after_partial");
  builder.CreateStore(after, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  // runNativeRegisterElimination 会重写签名替换函数对象，旧指针会悬空，
  // 必须按名字重新取。
  llvm::Function *rewritten = module.getFunction("partial_write_then_read");
  return expect(summary.LoadsReplaced == 1,
                "partial read helper was not replaced") &&
         // 原来那次读（offset 0）必须被折成常量；返回槽里从没被写过的上半部分
         // （offset 32）现在保留成入口裸读——裸读优先于 notdec.unknown
         // （见寄存器值表示准则）。
         expect(rewritten == nullptr ||
                    countPartialReadCalls(*rewritten, rax, 0, 32) == 0,
                "partial read helper call remained after summary SSA") &&
         expect(rewritten == nullptr || !hasRegisterLoad(*rewritten, "RAX"),
                "raw RAX load remained after partial read rewrite") &&
         verifyOk(module,
                  "module failed verifier after partial read helper test");
}

bool testFullStoreFeedsPartialReadThroughRangeSSA() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-full-store-partial-read", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "full_store_partial_read", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);

  storeRegister(builder, rax,
                llvm::ConstantInt::get(rax->getValueType(), 0xb00000007ULL),
                "RAX");
  llvm::Value *low = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)}, "low");
  builder.CreateStore(low, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  // runNativeRegisterElimination 会重写签名替换函数对象，旧指针会悬空，
  // 必须按名字重新取。
  llvm::Function *rewritten = module.getFunction("full_store_partial_read");
  return expect(summary.LoadsReplaced >= 1,
                "full store partial read was not replaced") &&
         expect(rewritten == nullptr || !hasPartialReadCall(*rewritten),
                "full store partial read helper call remained") &&
         expect(rewritten == nullptr || !hasRegisterLoad(*rewritten, "RAX"),
                "full store partial read left raw RAX load") &&
         verifyOk(module,
                  "module failed verifier after full store partial read test");
}

bool testDeadPartialReadHelperIsRemovedBySummarySSA() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-dead-partial-read-helper", context);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *externalType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *unknown =
      llvm::Function::Create(externalType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "dead_partial_read", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rdi->getType(), llvm::Type::getInt32Ty(context), 64, 32);

  builder.CreateCall(unknown);
  builder.CreateCall(
      partialRead,
      {rdi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.DeadLoadsRemoved >= 1,
                "dead partial read helper was not counted") &&
         expect(!hasPartialReadCall(*function),
                "dead partial read helper remained") &&
         verifyOk(module,
                  "module failed verifier after dead partial read helper test");
}

bool testDeadSummaryCallValueHelperIsRemovedBySummarySSA() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-dead-summary-call-helper", context);
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *helperType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {}, false);
  llvm::Function *helper =
      llvm::Function::Create(helperType, llvm::GlobalValue::ExternalLinkage,
                             "notdec.register.summary_clobber.i64", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "dead_summary_call_helper", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::CallInst *dead = builder.CreateCall(helperType, helper, {}, "dead");
  llvm::CallInst *live = builder.CreateCall(helperType, helper, {}, "live");
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "name=RDX"),
      llvm::MDString::get(context, "kind=clobber"),
  };
  llvm::MDNode *metadata = llvm::MDNode::get(context, fields);
  dead->setMetadata("notdec.register.summary_ssa.call_value", metadata);
  live->setMetadata("notdec.register.summary_ssa.call_value", metadata);
  builder.CreateStore(live, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);

  unsigned helperCalls = 0;
  bool deadCallRemained = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call == nullptr || call->getCalledFunction() != helper) {
      continue;
    }
    ++helperCalls;
    deadCallRemained |= call->getName() == "dead";
  }

  return expect(helperCalls == 1,
                "dead summary call helper was not removed precisely") &&
         expect(!deadCallRemained, "dead summary call helper remained") &&
         expect(moduleHasFunctionNamed(module,
                                       "notdec.register.summary_clobber.i64"),
                "live summary helper declaration was removed") &&
         verifyOk(module,
                  "module failed verifier after dead summary helper test");
}

bool testConsecutivePartialReadHelpersAreConsumedBySummarySSA() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-consecutive-partial-read-helper", context);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "consecutive_partial_reads", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Value *first = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "first");
  llvm::Value *second = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "second");
  llvm::Value *value = builder.CreateXor(first, second, "same_xor");
  builder.CreateStore(value, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.LoadsReplaced >= 2,
                "consecutive partial reads were not both replaced") &&
         expect(!hasPartialReadCall(*function),
                "consecutive partial read helper call remained") &&
         verifyOk(module,
                  "module failed verifier after consecutive partial read test");
}

bool testConsecutivePartialReadXorIsZeroedAfterSummarySSA() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-consecutive-partial-read-xor", context);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "consecutive_partial_read_xor", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Value *first = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "first");
  llvm::Value *second = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "second");
  llvm::Value *value = builder.CreateXor(first, second, "same_xor");
  builder.CreateStore(value, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  bool hasNonZeroSinkStore = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    if (store == nullptr || store->getPointerOperand() != sink) {
      continue;
    }
    auto *constant =
        llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
    hasNonZeroSinkStore = constant == nullptr || !constant->isZero();
  }

  return expect(summary.LoadsReplaced >= 2,
                "consecutive partial read xor inputs were not replaced") &&
         expect(!hasPartialReadCall(*function),
                "consecutive partial read xor helper call remained") &&
         expect(!hasNonZeroSinkStore,
                "consecutive partial read xor was not folded to zero") &&
         verifyOk(
             module,
             "module failed verifier after consecutive partial read xor test");
}

bool testDuplicatePartialReadXorAfterUnknownCallIsZeroed() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unknown-call-partial-read-xor", context);
  attachTestAbi(module);
  llvm::GlobalVariable *r9 = createRegisterGlobal(module, "R9");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *externalType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *unknown =
      llvm::Function::Create(externalType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external", module);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "unknown_call_partial_read_xor", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, r9->getType(), llvm::Type::getInt32Ty(context), 64, 32);

  builder.CreateCall(unknown);
  llvm::Value *first = builder.CreateCall(
      partialRead,
      {r9, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "first");
  llvm::Value *second = builder.CreateCall(
      partialRead,
      {r9, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)},
      "second");
  llvm::Value *value = builder.CreateXor(first, second, "same_xor");
  builder.CreateStore(value, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  bool hasNonZeroSinkStore = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    if (store == nullptr || store->getPointerOperand() != sink) {
      continue;
    }
    auto *constant =
        llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
    hasNonZeroSinkStore = constant == nullptr || !constant->isZero();
  }

  return expect(summary.LoadsReplaced >= 1,
                "duplicate partial read xor was not folded") &&
         expect(!hasPartialReadCall(*function),
                "duplicate partial read xor helper calls remained") &&
         expect(!hasNonZeroSinkStore,
                "duplicate partial read xor after unknown call was not zero") &&
         verifyOk(
             module,
             "module failed verifier after unknown call partial read xor test");
}

bool testDeadPartialWriteUsesRangeLiveness() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-write-range-liveness", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "dead_partial_write_range_liveness", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);

  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rax->getType(), llvm::Type::getInt32Ty(context), 64, 32);

  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 7),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *high = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 32)},
      "high");
  builder.CreateStore(high, sink);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  // runNativeRegisterElimination 会重写签名替换函数对象，旧指针会悬空，
  // 必须按名字重新取。
  llvm::Function *rewritten =
      module.getFunction("dead_partial_write_range_liveness");
  return expect(summary.DeadStoresRemoved >= 1,
                "dead low partial write was not removed") &&
         expect(rewritten == nullptr || !hasPartialWriteCall(*rewritten),
                "dead low partial write helper remained") &&
         // 高半部分从没被写过，它的值就是入口值：保留成入口裸读，不是 unknown。
         // （可能有两份重复读：两遍改写各建了一次，后续可以再合并。）
         expect(rewritten == nullptr ||
                    countPartialReadCalls(*rewritten, rax, 32, 32) >= 1,
                "high partial read is missing after entry raw read fallback") &&
         verifyOk(module,
                  "module failed verifier after partial write liveness test");
}

bool testFloatCallEffectUsesRangeLiveness() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-float-call-effect-range-liveness", context);
  attachTestFloatAbi(module, 1);
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);
  llvm::Function *log = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getDoubleTy(context),
                              {llvm::Type::getDoubleTy(context)}, false),
      llvm::GlobalValue::ExternalLinkage, "log", module);

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(voidType, llvm::GlobalValue::ExternalLinkage,
                             "float_call_effect_range_liveness", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, zmm0, llvm::ConstantInt::get(zmmType, 1), "ZMM0");
  builder.CreateCall(
      log, {llvm::ConstantFP::get(llvm::Type::getDoubleTy(context), 1.0)});
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  return expect(summary.DeadStoresRemoved >= 1,
                "dead full ZMM store before float return was not removed") &&
         expect(!hasRegisterStore(*function, "ZMM0"),
                "dead full ZMM store remained before float return") &&
         verifyOk(module,
                  "module failed verifier after float call liveness test");
}

bool testCallArgSlotSpansPlannedRangeSegments() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-call-arg-spanning-segments", context);
  attachTestAbiWithInputs(module, {"RDI", "RDX"});
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  auto *i16 = llvm::Type::getInt16Ty(context);
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *sink = new llvm::GlobalVariable(
      module, i16, false, llvm::GlobalValue::ExternalLinkage, nullptr,
      "spanning_segments_sink");

  llvm::Function *partialRead32 =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rdx->getType(), i32, 64, 32);
  llvm::Function *partialRead16 =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rdx->getType(), i16, 64, 16);

  // callee 只读 RDX 低 32 位，所以它的形参槽位是 i32。
  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "spanning_segments_callee",
      module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(calleeEntry);
  llvm::Value *calleeLow = builder.CreateCall(
      partialRead32, {rdx, llvm::ConstantInt::get(i64, 0)}, "callee.rdx");
  builder.CreateStore(calleeLow, sink);
  builder.CreateRetVoid();

  llvm::Function *caller = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "spanning_segments_caller",
      module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  storeRegister(builder, rdx, llvm::ConstantInt::get(rdx->getValueType(), 16),
                "RDX");
  llvm::Value *low16 = builder.CreateCall(
      partialRead16, {rdx, llvm::ConstantInt::get(i64, 0)}, "caller.low16");
  builder.CreateStore(low16, sink);
  builder.CreateCall(voidType, callee);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  uint64_t callerSegments = 0;
  for (const auto &function : summary.Functions) {
    if (function.FunctionName == "spanning_segments_caller") {
      callerSegments = function.RangeSegmentsPlanned;
    }
  }
  // 拼接出来的范围 helper 由 final cleanup 降级成移位/或，再折叠成常量。
  notdec::bin2llvm::runNativeRegisterFinalCleanup(module);
  llvm::Function *rewrittenCallee =
      module.getFunction("spanning_segments_callee");
  llvm::CallInst *rewrittenCall = nullptr;
  if (llvm::Function *rewrittenCaller =
          module.getFunction("spanning_segments_caller")) {
    for (llvm::Instruction &inst : llvm::instructions(*rewrittenCaller)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call != nullptr && call->getCalledFunction() == rewrittenCallee) {
        rewrittenCall = call;
      }
    }
  }
  bool missingBinding = std::any_of(
      summary.Warnings.begin(), summary.Warnings.end(),
      [](const notdec::bin2llvm::NativeRegisterEliminationWarning &warning) {
        return warning.CalleeName == "spanning_segments_callee" &&
               llvm::StringRef(warning.Reason) ==
                   "call_arg_rewrite_missing_binding";
      });
  auto *bound = rewrittenCall == nullptr || rewrittenCall->arg_size() != 1
                    ? nullptr
                    : llvm::dyn_cast<llvm::ConstantInt>(
                          rewrittenCall->getArgOperand(0));
  return expect(callerSegments >= 3,
                "callee parameter slot boundary was not planned into the "
                "caller range partition") &&
         expect(rewrittenCall != nullptr && rewrittenCall->arg_size() == 1,
                "spanning segment call lost its RDX argument") &&
         expect(bound != nullptr && bound->getZExtValue() == 16,
                "spanning segment call argument was not composed from the "
                "planned range segments") &&
         expect(!missingBinding,
                "spanning segment call argument reported a missing binding") &&
         verifyOk(module, "module failed verifier after spanning segment test");
}

bool testPartialWriteHelperNameSurvivesSignatureRewrite() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-write-name", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rax->getType(), llvm::Type::getInt8Ty(context), 64, 8);

  auto *externalType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *external =
      llvm::Function::Create(externalType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_write_name_survives", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 11),
                "RAX");
  builder.CreateCall(externalType, external);
  builder.CreateCall(
      partialWrite,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 7),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *keptPartialWrite =
      module.getFunction("notdec.partial_write.i64.i8");

  return expect(keptPartialWrite == partialWrite,
                "partial write helper was renamed during signature rewrite") &&
         expect(module.getFunction("1") == nullptr,
                "numeric partial write helper declaration was created") &&
         verifyOk(module,
                  "module failed verifier after partial write name test");
}

bool testPartialReadHelperNameSurvivesSignatureRewrite() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-read-name", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");

  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rax->getType(), llvm::Type::getInt8Ty(context), 64, 8);

  auto *externalType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *external =
      llvm::Function::Create(externalType, llvm::GlobalValue::ExternalLinkage,
                             "unknown_external", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_read_name_survives", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  storeRegister(builder, rax, llvm::ConstantInt::get(rax->getValueType(), 11),
                "RAX");
  builder.CreateCall(externalType, external);
  llvm::Value *value = builder.CreateCall(
      partialRead,
      {rax, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *sinkPointer = builder.CreateIntToPtr(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 4096),
      llvm::PointerType::get(context, 0), "sink");
  builder.CreateStore(value, sinkPointer);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *keptPartialRead =
      module.getFunction("notdec.partial_read.i64.i8");

  return expect(keptPartialRead == partialRead,
                "partial read helper was renamed during signature rewrite") &&
         expect(module.getFunction("1") == nullptr,
                "numeric partial read helper declaration was created") &&
         verifyOk(module,
                  "module failed verifier after partial read name test");
}

bool testPartialDemandPoisonReplacementIsMarked() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-demand-poison-metadata", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_poison_metadata", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *old = loadRegister(builder, rdx, "RDX", "old");
  llvm::Value *keep = builder.CreateAnd(
      old,
      llvm::ConstantInt::get(rdx->getValueType(),
                             llvm::APInt::getBitsSet(64, 8, 64)),
      "keep_high");
  llvm::Value *low = llvm::ConstantInt::get(rdx->getValueType(), 7);
  storeRegister(builder, rdx, builder.CreateOr(keep, low, "merged"), "RDX");
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  options.EnablePostRewriteInstCombine = false;
  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module, options);

  return expect(summary.PartialDemandMatched >= 1,
                "partial poison replacement was not applied") &&
         expect(functionHasZeroDemandOperandMetadata(*function),
                "partial poison replacement was not marked") &&
         expect(functionHasPoisonZeroDemandOperand(*function),
                "zero-demand operand was not replaced with poison") &&
         verifyOk(module,
                  "module failed verifier after partial poison metadata test");
}

bool testPartialDemandKeepsDynamicShiftAmount() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-demand-dynamic-shift", context);
  // RDX 必须是 ABI 输入，入口读才对应真实参数；否则入口读会先被换成 unknown。
  attachTestAbiWithInputs(module, {"RDI", "RDX"});
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_dynamic_shift", module);
  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  llvm::Function *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::ExternalLinkage, "dynamic_shift_side_effect",
      module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *taken =
      llvm::BasicBlock::Create(context, "taken", function);
  llvm::BasicBlock *fallthrough =
      llvm::BasicBlock::Create(context, "fallthrough", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Type *valueType = rdx->getValueType();
  llvm::Value *value = loadRegister(builder, rdx, "RDX", "value");
  llvm::Value *amount = builder.CreateAnd(
      value, llvm::ConstantInt::get(valueType, 31), "amount");
  llvm::Value *shifted = builder.CreateLShr(value, amount, "shifted");
  llvm::Value *bit = builder.CreateAnd(
      shifted, llvm::ConstantInt::get(valueType, 1), "bit");
  llvm::Value *flag = builder.CreateICmpNE(
      bit, llvm::ConstantInt::get(valueType, 0), "flag");
  storeRegister(builder, rdx, shifted, "RDX");
  builder.CreateCondBr(flag, taken, fallthrough);

  // 两条分支必须不一样，否则 SimplifyCFG 会把分支连同条件一起折掉，
  // 测不到 partial demand 对移位量的处理。
  builder.SetInsertPoint(taken);
  builder.CreateCall(callee, {});
  builder.CreateRetVoid();
  builder.SetInsertPoint(fallthrough);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  // 签名重写会换掉 Function 对象，按名字重新取。
  function = module.getFunction("partial_dynamic_shift");
  unsigned shiftCount = 0;
  bool poisonedShiftOperand = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (inst.getOpcode() != llvm::Instruction::LShr &&
        inst.getOpcode() != llvm::Instruction::Shl) {
      continue;
    }
    ++shiftCount;
    for (llvm::Use &operand : inst.operands()) {
      poisonedShiftOperand |= llvm::isa<llvm::PoisonValue>(operand.get());
    }
  }
  // 移位量被替换成 poison 时整条 `shift -> and -> icmp -> br` 链都会被
  // 常量折叠掉，所以 shiftCount 也会掉到 0；两种表现都算失败。
  if (shiftCount == 0 || poisonedShiftOperand) {
    module.print(llvm::errs(), nullptr);
  }

  return expect(summary.PartialDemandCandidates >= 1,
                "dynamic shift store was not seen as a partial demand "
                "candidate") &&
         expect(shiftCount >= 1,
                "dynamic shift disappeared from the function") &&
         expect(!poisonedShiftOperand,
                "dynamic shift operand was replaced with poison") &&
         verifyOk(module,
                  "module failed verifier after dynamic shift demand test");
}

bool testPoisonAggregateBaseGetsFilledFields() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-poison-aggregate-base", context);
  attachTestAbi(module);

  llvm::Type *i64 = llvm::Type::getInt64Ty(context);
  llvm::StructType *pair = llvm::StructType::get(context, {i64, i64});
  auto *type = llvm::FunctionType::get(pair, {i64}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "poison_aggregate_helper",
      module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *base = llvm::PoisonValue::get(pair);
  llvm::Value *built = builder.CreateInsertValue(base, function->getArg(0), 0);
  builder.CreateRet(built);

  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  bool foundAttributedField = false;
  std::set<unsigned> writtenIndices;
  llvm::Value *returned = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(*function)) {
    if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
      returned = ret->getReturnValue();
    }
    auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
    if (call == nullptr) {
      continue;
    }
    if (llvm::MDNode *source = call->getMetadata("notdec.unknown.source")) {
      for (const llvm::MDOperand &operand : source->operands()) {
        auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
        if (text != nullptr &&
            text->getString() == "reason=poison_aggregate_field") {
          foundAttributedField = true;
        }
      }
    }
  }
  // 返回的聚合必须是完整写满的 insertvalue 链（基座可以是 poison，但每个字段
  // 都已被覆盖）。
  for (llvm::Value *value = returned; value != nullptr;) {
    auto *insert = llvm::dyn_cast<llvm::InsertValueInst>(value);
    if (insert == nullptr || insert->getNumIndices() != 1) {
      break;
    }
    writtenIndices.insert(insert->getIndices()[0]);
    value = insert->getAggregateOperand();
    if (llvm::isa<llvm::PoisonValue>(value)) {
      break;
    }
  }
  bool allFieldsWritten = writtenIndices.count(0) != 0 &&
                          writtenIndices.count(1) != 0;
  if (!foundAttributedField || !allFieldsWritten) {
    module.print(llvm::errs(), nullptr);
  }
  return expect(cleanup.PoisonAggregateBasesReplaced >= 1,
                "poison aggregate base was not counted") &&
         expect(foundAttributedField,
                "unset aggregate field was not filled with an attributed "
                "unknown") &&
         expect(allFieldsWritten,
                "returned aggregate still has an unwritten (poison) field") &&
         verifyOk(module,
                  "module failed verifier after poison aggregate base test");
}

bool testPoisonRangeExtractBecomesAttributedUnknown() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-poison-range-extract", context);
  attachTestAbi(module);

  llvm::Type *i64 = llvm::Type::getInt64Ty(context);
  llvm::Type *i32 = llvm::Type::getInt32Ty(context);
  auto *extractType = llvm::FunctionType::get(i32, {i64, i64}, false);
  llvm::FunctionCallee extract =
      module.getOrInsertFunction("notdec.reg.extract.i64.i32", extractType);
  auto *type = llvm::FunctionType::get(i32, {}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "poison_extract_helper",
      module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  // 位段读取的源是 poison：寄存器消除结束时必须换成带归因的 unknown，
  // 否则 post-rewrite peephole 会把它折成 poison 并流进 ret/br。
  llvm::Value *segment = builder.CreateCall(
      extract, {llvm::PoisonValue::get(i64), llvm::ConstantInt::get(i64, 0)},
      "range_segment");
  llvm::Value *result = builder.CreateAdd(
      segment, llvm::ConstantInt::get(i32, 1), "result");
  builder.CreateRet(result);

  notdec::bin2llvm::runNativeRegisterElimination(module);

  bool foundPoisonExtract = false;
  bool foundAttributedUnknown = false;
  for (llvm::Function &fn : module) {
    for (llvm::Instruction &inst : llvm::instructions(fn)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call == nullptr) {
        continue;
      }
      llvm::Function *callee = call->getCalledFunction();
      if (callee == nullptr) {
        continue;
      }
      if (callee->getName().starts_with("notdec.reg.extract.")) {
        for (llvm::Value *argument : call->args()) {
          if (llvm::isa<llvm::PoisonValue>(argument)) {
            foundPoisonExtract = true;
          }
        }
      }
      if (llvm::MDNode *source =
              call->getMetadata("notdec.unknown.source")) {
        for (const llvm::MDOperand &operand : source->operands()) {
          auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
          if (text != nullptr &&
              text->getString() == "reason=poison_range_extract_source") {
            foundAttributedUnknown = true;
          }
        }
      }
    }
  }
  if (foundPoisonExtract || !foundAttributedUnknown) {
    module.print(llvm::errs(), nullptr);
  }
  return expect(!foundPoisonExtract,
                "poison-sourced range extract remained after register "
                "elimination") &&
         expect(foundAttributedUnknown,
                "poison-sourced range extract was not replaced with an "
                "attributed unknown") &&
         verifyOk(module,
                  "module failed verifier after poison range extract test");
}

bool testPartialDemandKeepsSelectCondition() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-demand-select-condition", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX"});
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");

  llvm::Type *i64 = llvm::Type::getInt64Ty(context);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "select_condition_helper",
      module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *value = loadRegister(builder, rsi, "RSI", "value");
  llvm::Value *condition =
      builder.CreateICmpEQ(value, llvm::ConstantInt::get(i64, 0), "cond");
  // select 的 condition 必须先被 demand 到，否则 partial demand 会把 condition
  // 换成 poison（hexx64 FUN_153800 的地址选择 select 就这样毒化了整条地址链）。
  llvm::Value *selected =
      builder.CreateSelect(condition, llvm::ConstantInt::get(i64, 16),
                           llvm::ConstantInt::get(i64, 32), "selected");
  storeRegister(builder, rax, selected, "RAX");
  builder.CreateRetVoid();

  // 调用点观察 RAX 返回，select 才有真实 demand。
  llvm::Function *caller = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "use_select_condition", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  llvm::Value *sink = builder.CreateAlloca(i64);
  builder.CreateCall(type, callee);
  builder.CreateStore(loadRegister(builder, rax, "RAX", "returned"), sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  options.EnablePostRewriteInstCombine = false;
  notdec::bin2llvm::runNativeRegisterElimination(module, options);
  llvm::Function *rewritten = module.getFunction("select_condition_helper");
  llvm::SelectInst *select = nullptr;
  if (rewritten != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewritten)) {
      if (auto *candidate = llvm::dyn_cast<llvm::SelectInst>(&inst)) {
        select = candidate;
        break;
      }
    }
  }
  if (select == nullptr) {
    module.print(llvm::errs(), nullptr);
  }
  return expect(select != nullptr, "select was removed") &&
         expect(!llvm::isa<llvm::PoisonValue>(select->getCondition()),
                "partial demand replaced the select condition with poison") &&
         verifyOk(module, "module failed verifier after select condition test");
}

bool testPartialDemandKeepsSameWidthMulOperand() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-demand-same-width-mul", context);
  // RSI 必须是 ABI 输入，入口读才对应真实参数；否则入口读会先被换成 unknown。
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX"});
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  llvm::Type *i64 = llvm::Type::getInt64Ty(context);
  llvm::Type *i128 = llvm::Type::getInt128Ty(context);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "same_width_mul_helper", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", callee);
  llvm::BasicBlock *taken = llvm::BasicBlock::Create(context, "taken", callee);
  llvm::BasicBlock *fallthrough =
      llvm::BasicBlock::Create(context, "fallthrough", callee);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *value = loadRegister(builder, rsi, "RSI", "value");
  llvm::Value *scaled =
      builder.CreateShl(value, llvm::ConstantInt::get(i64, 2), "scaled");
  // 被后面的 RDX 写覆盖的 dead register store：它的 operand 本来没有 demand。
  storeRegister(builder, rdx, scaled, "RDX");
  llvm::Value *lhs = builder.CreateZExt(
      llvm::ConstantInt::get(i64, llvm::APInt(64, 0xAAAAAAAAAAAAAAABULL)), i128);
  llvm::Value *rhs = builder.CreateZExt(scaled, i128, "mul_rhs");
  llvm::Value *product = builder.CreateMul(lhs, rhs, "product");
  llvm::Value *high =
      builder.CreateLShr(product, llvm::ConstantInt::get(i128, 64), "high");
  llvm::Value *result = builder.CreateTrunc(high, i64, "result");
  storeRegister(builder, rdx, result, "RDX");
  storeRegister(builder, rax, result, "RAX");
  llvm::Value *flag =
      builder.CreateICmpEQ(result, llvm::ConstantInt::get(i64, 0), "flag");
  builder.CreateCondBr(flag, taken, fallthrough);

  // 两条分支必须不一样，否则 SimplifyCFG 会把分支连同条件一起折掉。
  builder.SetInsertPoint(taken);
  builder.CreateRetVoid();
  builder.SetInsertPoint(fallthrough);
  builder.CreateRetVoid();

  // 调用点观察 RAX 返回，乘法链才有真实 demand。
  llvm::Function *caller = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "use_same_width_mul", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  builder.CreateCall(type, callee);
  builder.CreateStore(loadRegister(builder, rax, "RAX", "returned"), sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  options.EnablePostRewriteInstCombine = false;
  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module, options);
  // 签名重写会换掉 Function 对象，按名字重新取。
  llvm::Function *rewritten = module.getFunction("same_width_mul_helper");
  bool sawMul = false;
  if (rewritten != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewritten)) {
      sawMul |= inst.getOpcode() == llvm::Instruction::Mul;
    }
  }
  // 旧 lane 规则下 `%scaled` 零 demand，会被换成 poison：先毒化
  // `zext i64 poison to i128` 的操作数，mul 结果随之变 poison，demand 链上
  // 的所有使用点（分支、返回）一起失效。这里断言这条函数里没有任何
  // zero-demand poison —— 同宽 mul 的操作数有真实 observer，不该被裁。
  bool poisonedOperand =
      rewritten != nullptr && functionHasPoisonZeroDemandOperand(*rewritten);
  if (!sawMul || poisonedOperand) {
    module.print(llvm::errs(), nullptr);
  }

  return expect(summary.PartialDemandCandidates >= 1,
                "same-width mul store was not seen as a partial demand "
                "candidate") &&
         expect(sawMul, "same-width mul disappeared from the function") &&
         expect(!poisonedOperand,
                "same-width mul operand was replaced with poison") &&
         verifyOk(module,
                  "module failed verifier after same-width mul demand test");
}

bool testPartialDemandKeepsDivRemOperand() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-demand-div-rem", context);
  attachTestAbiWithInputs(module, {"RDI", "RSI", "RDX", "RCX"});
  llvm::GlobalVariable *rax = createRegisterGlobal(module, "RAX");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  llvm::GlobalVariable *rsi = createRegisterGlobal(module, "RSI");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  llvm::Type *i64 = llvm::Type::getInt64Ty(context);
  llvm::Type *i128 = llvm::Type::getInt128Ty(context);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "div_rem_helper", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", callee);
  llvm::BasicBlock *taken = llvm::BasicBlock::Create(context, "taken", callee);
  llvm::BasicBlock *fallthrough =
      llvm::BasicBlock::Create(context, "fallthrough", callee);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *value = loadRegister(builder, rsi, "RSI", "value");
  // 128 位被除数：高半段来自 RSI，低半段是常数（模拟 idiv 的 RDX:RAX）。
  llvm::Value *high = builder.CreateZExt(value, i128, "high");
  llvm::Value *highShift =
      builder.CreateShl(high, llvm::ConstantInt::get(i128, 64), "high_shift");
  llvm::Value *low = builder.CreateZExt(
      llvm::ConstantInt::get(i64, 7), i128, "low");
  llvm::Value *dividend = builder.CreateOr(highShift, low, "dividend");
  llvm::Value *divisor = builder.CreateZExt(
      llvm::ConstantInt::get(i64, 3), i128, "divisor");
  llvm::Value *quotient = builder.CreateSDiv(dividend, divisor, "quotient");
  llvm::Value *remainder = builder.CreateSRem(dividend, divisor, "remainder");
  llvm::Value *quotient64 = builder.CreateTrunc(quotient, i64, "quotient64");
  llvm::Value *remainder64 = builder.CreateTrunc(remainder, i64, "remainder64");
  // 商写 RDX 之后不再被读：这条 register store 的 operand 本来没有 demand；
  // 但除法/取余共享同一份被除数，取余链是活的，被除数高位不能被裁。
  storeRegister(builder, rdx, quotient64, "RDX");
  storeRegister(builder, rax, remainder64, "RAX");
  llvm::Value *flag =
      builder.CreateICmpEQ(remainder64, llvm::ConstantInt::get(i64, 0), "flag");
  builder.CreateCondBr(flag, taken, fallthrough);

  // 两条分支必须不一样，否则 SimplifyCFG 会把分支连同条件一起折掉。
  builder.SetInsertPoint(taken);
  builder.CreateRetVoid();
  builder.SetInsertPoint(fallthrough);
  builder.CreateRetVoid();

  // 调用点观察 RAX 返回，取余链才有真实 demand。
  llvm::Function *caller = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "use_div_rem", module);
  llvm::BasicBlock *callerEntry =
      llvm::BasicBlock::Create(context, "entry", caller);
  builder.SetInsertPoint(callerEntry);
  builder.CreateCall(type, callee);
  builder.CreateStore(loadRegister(builder, rax, "RAX", "returned"), sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterEliminationOptions options;
  options.EnablePostRewriteInstCombine = false;
  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module, options);
  // 签名重写会换掉 Function 对象，按名字重新取。
  llvm::Function *rewritten = module.getFunction("div_rem_helper");
  bool sawSRem = false;
  bool poisonedDividend = false;
  if (rewritten != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(rewritten)) {
      if (inst.getOpcode() == llvm::Instruction::SRem) {
        sawSRem = true;
      }
      // 只查被除数的高半段（`shl i128`）和取余自身：商的 register store 是死值，
      // 那条 dead 链上的 poison 是 zero-demand 的正常结果，不算失败。旧 lane
      // 规则会把 `shl i128 poison, 64` 的高半段裁掉。
      const bool isDividendHigh = inst.getOpcode() == llvm::Instruction::Shl &&
                                  inst.getType()->isIntegerTy(128);
      if (!isDividendHigh && inst.getOpcode() != llvm::Instruction::SRem &&
          inst.getOpcode() != llvm::Instruction::SDiv) {
        continue;
      }
      for (llvm::Use &operand : inst.operands()) {
        poisonedDividend |= llvm::isa<llvm::PoisonValue>(operand.get());
      }
    }
  }
  if (!sawSRem || poisonedDividend) {
    module.print(llvm::errs(), nullptr);
  }

  return expect(summary.PartialDemandCandidates >= 1,
                "div/rem store was not seen as a partial demand candidate") &&
         expect(sawSRem, "srem disappeared from the function") &&
         expect(!poisonedDividend,
                "shared dividend operand was replaced with poison") &&
         verifyOk(module, "module failed verifier after div/rem demand test");
}

bool testPartialZmmKeepHighStoreIsDemandRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-zmm-demand", context);
  attachTestAbi(module);
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_zmm_keep_high", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *old = loadRegister(builder, zmm0, "ZMM0", "old");
  llvm::Value *keep = builder.CreateAnd(
      old,
      llvm::ConstantInt::get(zmmType, llvm::APInt::getBitsSet(512, 128, 512)));
  llvm::Value *low = llvm::ConstantInt::get(zmmType, 7);
  storeRegister(builder, zmm0, builder.CreateOr(keep, low), "ZMM0");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  unsigned zmmLoads = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
      if (load->getPointerOperand()->stripPointerCasts() == zmm0) {
        ++zmmLoads;
      }
    }
  }

  return expect(summary.PartialDemandCandidates >= 1,
                "partial zmm keep-high store was not seen as a candidate") &&
         expect(summary.PartialDemandMatched >= 1,
                "partial zmm keep-high store was not demand rewritten") &&
         expect(zmmLoads == 0,
                "partial zmm keep-high old load was not removed") &&
         verifyOk(module,
                  "module failed verifier after partial zmm demand test");
}

bool testPartialZmmNakedKeepHighStoreIsDemandRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-zmm-naked-keep-high", context);
  attachTestAbi(module);
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_zmm_naked_keep_high", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *old = loadRegister(builder, zmm0, "ZMM0", "old");
  llvm::Value *keep = builder.CreateAnd(
      old,
      llvm::ConstantInt::get(zmmType, llvm::APInt::getBitsSet(512, 128, 512)));
  storeRegister(builder, zmm0, keep, "ZMM0");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  unsigned zmmLoads = 0;
  unsigned zmmStores = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
      if (load->getPointerOperand()->stripPointerCasts() == zmm0) {
        ++zmmLoads;
      }
    }
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      if (store->getPointerOperand()->stripPointerCasts() == zmm0) {
        ++zmmStores;
      }
    }
  }

  return expect(
             summary.PartialDemandCandidates >= 1,
             "partial zmm naked keep-high store was not seen as a candidate") &&
         expect(summary.PartialDemandMatched >= 1,
                "partial zmm naked keep-high store was not demand rewritten") &&
         expect(zmmLoads == 0,
                "partial zmm naked keep-high old load was not removed") &&
         expect(zmmStores == 0,
                "partial zmm naked keep-high store was not removed") &&
         verifyOk(module,
                  "module failed verifier after partial zmm naked demand test");
}

bool testPartialZmmDisjointLaneChainIsDemandRewritten() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-zmm-lane-chain", context);
  attachTestAbi(module);
  llvm::Type *zmmType = llvm::IntegerType::get(context, 512);
  llvm::GlobalVariable *zmm0 =
      createRegisterGlobal(module, "ZMM0", zmmType, 4608, 64);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_zmm_lane_chain", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *old = loadRegister(builder, zmm0, "ZMM0", "old");
  llvm::Value *keep = builder.CreateAnd(
      old,
      llvm::ConstantInt::get(zmmType, llvm::APInt::getBitsSet(512, 128, 512)));
  llvm::Value *low = llvm::ConstantInt::get(zmmType, 7);
  llvm::Value *mid = builder.CreateOr(keep, low, "mid", /*IsDisjoint=*/true);
  llvm::Value *lane1 = builder.CreateShl(llvm::ConstantInt::get(zmmType, 11),
                                         llvm::ConstantInt::get(zmmType, 64));
  llvm::Value *combined =
      builder.CreateOr(mid, lane1, "combined", /*IsDisjoint=*/true);
  storeRegister(builder, zmm0, combined, "ZMM0");
  llvm::Value *used = builder.CreateTrunc(
      builder.CreateLShr(combined, llvm::ConstantInt::get(zmmType, 64)),
      llvm::Type::getInt64Ty(context));
  llvm::Value *slot =
      builder.CreateAlloca(llvm::Type::getInt64Ty(context), nullptr, "slot");
  builder.CreateStore(used, slot);
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);
  unsigned zmmLoads = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
      if (load->getPointerOperand()->stripPointerCasts() == zmm0) {
        ++zmmLoads;
      }
    }
  }

  return expect(summary.PartialDemandCandidates >= 1,
                "partial zmm lane chain store was not seen as a candidate") &&
         expect(summary.PartialDemandMatched >= 1,
                "partial zmm lane chain was not demand rewritten") &&
         expect(zmmLoads == 0,
                "partial zmm lane chain old load was not removed") &&
         verifyOk(module,
                  "module failed verifier after partial zmm lane chain test");
}

bool testPartialDemandKeepsMemoryPointerRegisterAddress() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-partial-memory-pointer-demand", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  llvm::GlobalVariable *rdx = createRegisterGlobal(module, "RDX");
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "ordinary_memory_sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "partial_memory_pointer_demand", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *base = loadRegister(builder, rdi, "RDI", "rdi_base");
  llvm::Value *address = builder.CreateAdd(
      base, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 40),
      "mem_addr");
  auto *pointerType = llvm::PointerType::get(context, 0);
  llvm::Value *pointer =
      builder.CreateIntToPtr(address, pointerType, "mem_ptr");
  llvm::Value *memoryValue =
      builder.CreateLoad(llvm::Type::getInt64Ty(context), pointer, "mem_value");
  builder.CreateStore(memoryValue, sink);

  llvm::Value *old = loadRegister(builder, rdx, "RDX", "old_rdx");
  llvm::Value *keep = builder.CreateAnd(
      old, llvm::ConstantInt::get(rdx->getValueType(),
                                  llvm::APInt::getBitsSet(64, 8, 64)));
  storeRegister(builder, rdx, builder.CreateOr(keep, memoryValue), "RDX");
  builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::runNativeRegisterElimination(module);

  llvm::IntToPtrInst *memoryPointer = nullptr;
  for (llvm::Function &rewrittenFunction : module) {
    if (rewrittenFunction.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
      if (auto *intToPtr = llvm::dyn_cast<llvm::IntToPtrInst>(&inst)) {
        if (intToPtr->getName() == "mem_ptr") {
          memoryPointer = intToPtr;
          break;
        }
      }
    }
    if (memoryPointer != nullptr) {
      break;
    }
  }
  if (memoryPointer == nullptr) {
    for (llvm::Function &rewrittenFunction : module) {
      if (rewrittenFunction.isDeclaration()) {
        continue;
      }
      for (llvm::Instruction &inst : llvm::instructions(rewrittenFunction)) {
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
        if (load == nullptr || load->getName() != "mem_value") {
          continue;
        }
        memoryPointer =
            llvm::dyn_cast<llvm::IntToPtrInst>(load->getPointerOperand());
        break;
      }
    }
  }
  bool addressWasZero = false;
  if (memoryPointer != nullptr) {
    if (auto *constant =
            llvm::dyn_cast<llvm::ConstantInt>(memoryPointer->getOperand(0))) {
      addressWasZero = constant->isZero();
    }
  }

  return expect(summary.PartialDemandCandidates >= 1,
                "partial memory pointer store was not seen as a candidate") &&
         expect(summary.PartialDemandMatched >= 1,
                "partial memory pointer store was not demand rewritten") &&
         expect(memoryPointer != nullptr,
                "ordinary memory inttoptr was removed") &&
         expect(!addressWasZero, "ordinary memory pointer register address was "
                                 "rewritten to zero") &&
         verifyOk(module,
                  "module failed verifier after partial memory pointer test");
}

bool testFinalCleanupDropsDeadRegisterGlobalsAndSummaryMetadata() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-clean", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "final_cleanup_clean", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *value = llvm::ConstantInt::get(rdi->getValueType(), 17);
  storeRegister(builder, rdi, value, "RDI");
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  bool hadMetadata = functionHasAnyRegisterSummaryMetadata(*function);
  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  return expect(hadMetadata,
                "summary SSA did not attach metadata before cleanup") &&
         expect(module.getGlobalVariable("RDI") == nullptr,
                "dead RDI global remained after final cleanup") &&
         expect(!functionHasAnyRegisterSummaryMetadata(*function),
                "register summary metadata remained on clean function") &&
         expect(cleanup.FunctionMetadataCleared >= 1,
                "final cleanup did not count cleared metadata") &&
         expect(cleanup.RemainingRegisterAccesses == 0,
                "final cleanup reported register residue in clean module") &&
         verifyOk(module, "module failed verifier after final cleanup test");
}

bool testFinalCleanupReplacesPoisonReturnValue() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-poison-return", context);
  attachTestAbi(module);

  auto *type = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(context), {}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "poison_return", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateRet(llvm::PoisonValue::get(llvm::Type::getInt32Ty(context)));

  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);
  auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&function->getEntryBlock().back());
  auto *unknown = ret == nullptr
                      ? nullptr
                      : llvm::dyn_cast_or_null<llvm::CallInst>(
                            ret->getReturnValue());
  bool hasSource = false;
  if (unknown != nullptr) {
    if (llvm::Function *callee = unknown->getCalledFunction()) {
      hasSource = callee->getName() == "notdec.unknown.i32";
    }
    hasSource = hasSource &&
                unknown->getMetadata("notdec.unknown.source") != nullptr;
  }
  if (!hasSource) {
    module.print(llvm::errs(), nullptr);
  }

  return expect(cleanup.PoisonReturnValuesReplaced >= 1,
                "poison return value was not counted") &&
         expect(unknown != nullptr,
                "poison return value was not replaced with a call") &&
         expect(hasSource,
                "poison return value has no attributed unknown helper") &&
         verifyOk(module, "module failed verifier after poison return test");
}

bool testFinalCleanupReplacesPoisonBranchCondition() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-poison-condition", context);
  attachTestAbi(module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "poison_condition", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *taken = llvm::BasicBlock::Create(context, "taken", function);
  llvm::BasicBlock *other = llvm::BasicBlock::Create(context, "other", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::PoisonValue::get(llvm::Type::getInt1Ty(context)),
                       taken, other);
  builder.SetInsertPoint(taken);
  builder.CreateRetVoid();
  builder.SetInsertPoint(other);
  builder.CreateRetVoid();

  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);
  auto *branch =
      llvm::dyn_cast<llvm::BranchInst>(&function->getEntryBlock().back());
  llvm::Value *condition =
      branch != nullptr && branch->isConditional() ? branch->getCondition()
                                                   : nullptr;
  auto *unknown = llvm::dyn_cast_or_null<llvm::CallInst>(condition);
  bool hasSource = false;
  if (unknown != nullptr) {
    if (llvm::Function *callee = unknown->getCalledFunction()) {
      hasSource = callee->getName().starts_with("notdec.unknown.");
    }
    hasSource = hasSource &&
                unknown->getMetadata("notdec.unknown.source") != nullptr;
  }
  if (!hasSource) {
    module.print(llvm::errs(), nullptr);
  }

  return expect(cleanup.PoisonControlFlowConditionsReplaced >= 1,
                "poison branch condition was not counted") &&
         expect(unknown != nullptr,
                "poison branch condition was not replaced with a call") &&
         expect(hasSource,
                "poison branch condition has no attributed unknown helper") &&
         verifyOk(module, "module failed verifier after poison condition test");
}

bool testFinalCleanupKeepsMetadataWhenRegisterAccessRemains() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-residue", context);
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "final_cleanup_residue", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *live = loadRegister(builder, rbx, "RBX", "rbx.live");
  llvm::AllocaInst *sink = builder.CreateAlloca(rbx->getValueType());
  builder.CreateStore(live, sink);
  builder.CreateRetVoid();

  llvm::LLVMContext &moduleContext = module.getContext();
  llvm::Metadata *field[] = {
      llvm::MDString::get(moduleContext, "loads_replaced=0"),
  };
  function->setMetadata("notdec.register.summary_ssa",
                        llvm::MDNode::get(moduleContext, field));

  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  return expect(module.getGlobalVariable("RBX") != nullptr,
                "live RBX global was removed") &&
         expect(function->getMetadata("notdec.register.summary_ssa") != nullptr,
                "metadata was cleared while register access remained") &&
         expect(cleanup.RemainingRegisterAccesses == 1,
                "final cleanup did not report remaining register access") &&
         verifyOk(module, "module failed verifier after residue cleanup test");
}

bool testFinalCleanupCountsPartialWriteResidue() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-partial-write", context);
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "final_cleanup_partial_write", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialWrite =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialWrite(
          module, rbx->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  builder.CreateCall(
      partialWrite,
      {rbx, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 7),
       llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateRetVoid();

  llvm::LLVMContext &moduleContext = module.getContext();
  llvm::Metadata *field[] = {
      llvm::MDString::get(moduleContext, "loads_replaced=0"),
  };
  function->setMetadata("notdec.register.summary_ssa",
                        llvm::MDNode::get(moduleContext, field));

  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  return expect(module.getGlobalVariable("RBX") != nullptr,
                "partial write RBX global was removed") &&
         expect(function->getMetadata("notdec.register.summary_ssa") != nullptr,
                "metadata was cleared while partial write remained") &&
         expect(cleanup.RemainingRegisterAccesses == 1,
                "final cleanup did not count partial write residue") &&
         verifyOk(module,
                  "module failed verifier after partial write cleanup test");
}

bool testFinalCleanupCountsPartialReadResidue() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-partial-read", context);
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "final_cleanup_partial_read", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rbx->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Value *value = builder.CreateCall(
      partialRead,
      {rbx, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *sinkPointer = builder.CreateIntToPtr(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 4096),
      llvm::PointerType::get(context, 0), "sink");
  builder.CreateStore(value, sinkPointer);
  builder.CreateRetVoid();

  llvm::LLVMContext &moduleContext = module.getContext();
  llvm::Metadata *field[] = {
      llvm::MDString::get(moduleContext, "loads_replaced=0"),
  };
  function->setMetadata("notdec.register.summary_ssa",
                        llvm::MDNode::get(moduleContext, field));

  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  return expect(module.getGlobalVariable("RBX") != nullptr,
                "partial read RBX global was removed") &&
         expect(function->getMetadata("notdec.register.summary_ssa") != nullptr,
                "metadata was cleared while partial read remained") &&
         expect(cleanup.RemainingRegisterAccesses == 1,
                "final cleanup did not count partial read residue") &&
         verifyOk(module,
                  "module failed verifier after partial read cleanup test");
}

bool testFinalCleanupDropsDeadPartialReadsAfterValueRangeLowering() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-dead-value-range", context);
  llvm::GlobalVariable *rbx = createRegisterGlobal(module, "RBX");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "final_cleanup_dead_value_range", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, rbx->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *insert =
      notdec::bin2llvm::getOrInsertNativeRegisterValueInsert(
          module, llvm::Type::getInt64Ty(context),
          llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Value *low = builder.CreateCall(
      partialRead,
      {rbx, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *high = builder.CreateCall(
      partialRead,
      {rbx, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 32)});
  llvm::Value *full = builder.CreateCall(
      insert, {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0), low,
               llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  full = builder.CreateCall(
      insert, {full, high,
               llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 32)});
  (void)full;
  builder.CreateRetVoid();

  llvm::LLVMContext &moduleContext = module.getContext();
  llvm::Metadata *field[] = {
      llvm::MDString::get(moduleContext, "loads_replaced=0"),
  };
  function->setMetadata("notdec.register.summary_ssa",
                        llvm::MDNode::get(moduleContext, field));

  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  return expect(cleanup.ValueRangeHelpersLowered == 2,
                "dead value range helpers were not lowered") &&
         expect(cleanup.DeadRegisterReadsRemoved == 2,
                "dead partial reads after value range lowering remained") &&
         expect(module.getGlobalVariable("RBX") == nullptr,
                "dead RBX global remained after value range cleanup") &&
         expect(!functionHasAnyRegisterSummaryMetadata(*function),
                "metadata remained after dead value range cleanup") &&
         expect(cleanup.RemainingRegisterAccesses == 0,
                "dead value range cleanup left register residue") &&
         verifyOk(module,
                  "module failed verifier after dead value range cleanup test");
}

bool testUnsignedAddCorrectionMagicFold() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unsigned-add-correction", context);
  // clang -O2 x86-64：x/7 → M=0x2492492492492493、shrq $2（前面还有 >>1）。
  //                     x/4095 → M=0x10010010010011、shrq $11。
  const MagicDivisionCase cases[] = {
      {7, 0x2492492492492493ULL, 2},
      {4095, 0x10010010010011ULL, 11},
  };
  for (const MagicDivisionCase &testCase : cases) {
    buildUnsignedAddFormFunction(
        module, "uadd_" + std::to_string(testCase.ExpectedDivisor), testCase,
        64);
  }

  auto peephole =
      notdec::bin2llvm::runNativeRegisterPostRewritePeephole(module);

  bool allFolded = true;
  for (const MagicDivisionCase &testCase : cases) {
    llvm::Function *function = module.getFunction(
        "uadd_" + std::to_string(testCase.ExpectedDivisor));
    allFolded &=
        expect(function != nullptr, "unsigned add-form function is missing") &&
        expect(functionHasUdivBy(*function, testCase.ExpectedDivisor),
               "unsigned add-form chain was not folded back to udiv") &&
        expect(!functionHasMul(*function),
               "unsigned add-form chain survived the udiv fold");
  }

  return expect(peephole.ConstantDivisionsRecognized == std::size(cases),
                "unexpected number of recognized add-form divisions") &&
         expect(peephole.SignedConstantDivisionsRecognized == 0,
                "add-form fold counted as signed") &&
         allFolded &&
         verifyOk(module, "module failed verifier after add-form fold");
}

bool testSignedNegativeMagicAddXFormFold() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-signed-negative-magic-addx", context);
  llvm::Type *i64 = llvm::Type::getInt64Ty(context);
  llvm::Type *i128 = llvm::Type::getInt128Ty(context);
  auto *type = llvm::FunctionType::get(i64, {i64}, false);
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "saddx_100", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *x = function->getArg(0);
  llvm::Value *wide = builder.CreateSExt(x, i128);
  // 机器里的常量是 64 位 0xA3D70A3D70A3D70B，按有符号解释是负数，lift 成
  // 符号扩展过的 i128 常量。
  llvm::Value *product = builder.CreateNSWMul(
      wide, llvm::ConstantInt::get(
                i128, llvm::APInt(64, 0xA3D70A3D70A3D70BULL).sext(128)));
  llvm::Value *high = builder.CreateLShr(product, llvm::ConstantInt::get(i128, 64));
  llvm::Value *h = builder.CreateTrunc(high, i64, "high_half");
  llvm::Value *sum = builder.CreateAdd(x, h);
  llvm::Value *shifted =
      builder.CreateAShr(sum, llvm::ConstantInt::get(i64, 6));
  llvm::Value *sign = builder.CreateLShr(sum, llvm::ConstantInt::get(i64, 63));
  builder.CreateRet(builder.CreateAdd(shifted, sign, "quotient"));

  auto peephole =
      notdec::bin2llvm::runNativeRegisterPostRewritePeephole(module);

  return expect(peephole.SignedConstantDivisionsRecognized == 1,
                "negative-magic add-x form was not recognized") &&
         expect(functionHasSdivBy(*function, 100),
                "negative-magic add-x form did not fold to sdiv 100") &&
         expect(!functionHasMul(*function),
                "negative-magic add-x form survived the sdiv fold") &&
         verifyOk(module, "module failed verifier after add-x fold");
}

bool testSignedDivisionSubFormFold() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-signed-sub-form", context);
  llvm::Type *i32 = llvm::Type::getInt32Ty(context);
  llvm::Type *i64 = llvm::Type::getInt64Ty(context);
  // (d, M, sarq 移位量) 取自 fortune 的实际机器码。
  const uint64_t divisor = 26;
  const uint64_t magic = 1321528399ULL;
  const unsigned shift = 35;
  llvm::Function *function = llvm::Function::Create(
      llvm::FunctionType::get(i32, {i32}, false),
      llvm::GlobalValue::ExternalLinkage, "sub_form_sdiv_26", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *x = function->getArg(0);
  llvm::Value *wrapped = wrapRegisterWidthRoundTrip(builder, x, 32);
  llvm::Value *wide = builder.CreateSExt(wrapped, i64);
  llvm::Value *product = builder.CreateNSWMul(
      wide, llvm::ConstantInt::get(i64, magic));
  llvm::Value *shifted = builder.CreateTrunc(
      builder.CreateAShr(product, llvm::ConstantInt::get(i64, shift)), i32);
  llvm::Value *sign = builder.CreateAShr(
      wrapped, llvm::ConstantInt::get(i32, 31));
  llvm::Value *quotient = builder.CreateSub(
      shifted, wrapRegisterWidthRoundTrip(builder, sign, 32), "quotient");
  builder.CreateRet(builder.CreateSub(
      x, builder.CreateMul(quotient, llvm::ConstantInt::get(i32, divisor)),
      "remainder"));

  auto peephole =
      notdec::bin2llvm::runNativeRegisterPostRewritePeephole(module);

  bool hasI64Mul = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (inst.getOpcode() == llvm::Instruction::Mul &&
        inst.getType()->isIntegerTy(64)) {
      hasI64Mul = true;
    }
  }
  return expect(peephole.SignedConstantDivisionsRecognized == 1,
                "sub-form signed quotient was not recognized") &&
         expect(functionHasSdivBy(*function, divisor),
                "sub-form signed quotient did not fold to sdiv 26") &&
         expect(!hasI64Mul,
                "sub-form magic multiply survived the sdiv fold") &&
         verifyOk(module, "module failed verifier after sub-form fold");
}

bool testX86_64Signed32BitAshrBeforeTruncFold() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-signed-32-ashr-before-trunc", context);
  llvm::Type *i32 = llvm::Type::getInt32Ty(context);
  llvm::Type *i64 = llvm::Type::getInt64Ty(context);
  struct Case {
    uint64_t Divisor;
    uint64_t Magic;
    unsigned AshrShift;
  };
  // (d, M, sarq 移位量) 取自 clang -m64 -O2 的真实输出。
  const Case cases[] = {
      {3, 0x55555556ULL, 32},    {9, 0x38E38E39ULL, 33},
      {11, 0x2E8BA2E9ULL, 33},   {13, 0x4EC4EC4FULL, 34},
      {1000, 0x10624DD3ULL, 38}, {1000000000, 0x44B82FA1ULL, 60},
  };
  for (const Case &testCase : cases) {
    llvm::Function *function = llvm::Function::Create(
        llvm::FunctionType::get(i32, {i32}, false),
        llvm::GlobalValue::ExternalLinkage,
        "x64_s32_ashr_" + std::to_string(testCase.Divisor), module);
    llvm::BasicBlock *entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::IRBuilder<> builder(entry);
    llvm::Value *wide = builder.CreateSExt(function->getArg(0), i64);
    llvm::Value *product = builder.CreateNSWMul(
        wide, llvm::ConstantInt::get(i64, testCase.Magic));
    llvm::Value *shifted = builder.CreateAShr(
        product, llvm::ConstantInt::get(i64, testCase.AshrShift));
    llvm::Value *quotient = builder.CreateTrunc(shifted, i32);
    llvm::Value *sign = builder.CreateTrunc(
        builder.CreateLShr(product, llvm::ConstantInt::get(i64, 63)), i32);
    builder.CreateRet(builder.CreateAdd(quotient, sign));
  }

  auto peephole =
      notdec::bin2llvm::runNativeRegisterPostRewritePeephole(module);

  bool allFolded = true;
  for (const Case &testCase : cases) {
    llvm::Function *function = module.getFunction(
        "x64_s32_ashr_" + std::to_string(testCase.Divisor));
    allFolded &=
        expect(function != nullptr, "x64 i32 ashr-form function is missing") &&
        expect(functionHasSdivBy(*function, testCase.Divisor),
               "x64 i32 ashr-form chain was not folded back to sdiv") &&
        expect(!functionHasMul(*function),
               "x64 i32 ashr-form chain survived the sdiv fold");
  }

  return expect(peephole.SignedConstantDivisionsRecognized == std::size(cases),
                "unexpected number of recognized x64 i32 ashr divisions") &&
         expect(peephole.ConstantDivisionsRecognized == 0,
                "x64 i32 ashr fold counted as unsigned") &&
         allFolded &&
         verifyOk(module, "module failed verifier after x64 i32 ashr fold");
}

bool testX86_64Signed32BitNegativeMagicAddXWithRoundTrips() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-signed-32-addx-round-trips", context);
  llvm::Type *i32 = llvm::Type::getInt32Ty(context);
  llvm::Type *i64 = llvm::Type::getInt64Ty(context);
  struct Case {
    uint64_t Divisor;
    uint64_t Magic32;
    unsigned PostShift;
  };
  // (d, 32 位负魔数的低 W 位, ashr 移位量) 取自 clang -m64 -O2 的真实输出。
  const Case cases[] = {
      {7, 0x92492493ULL, 2},
      {60, 0x88888889ULL, 5},
      {65535, 0x80008001ULL, 15},
  };
  for (const Case &testCase : cases) {
    llvm::Function *function = llvm::Function::Create(
        llvm::FunctionType::get(i32, {i32}, false),
        llvm::GlobalValue::ExternalLinkage,
        "x64_s32_addx_" + std::to_string(testCase.Divisor), module);
    llvm::BasicBlock *entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::IRBuilder<> builder(entry);
    llvm::Value *x = function->getArg(0);
    llvm::Value *wide = builder.CreateSExt(x, i64);
    llvm::Value *product = builder.CreateNSWMul(
        wide, llvm::ConstantInt::get(
                  i64, llvm::APInt(32, testCase.Magic32).sext(64)));
    llvm::Value *high = builder.CreateTrunc(
        builder.CreateLShr(product, llvm::ConstantInt::get(i64, 32)), i32);
    llvm::Value *wrappedX = builder.CreateTrunc(wide, i32);
    llvm::Value *sum = builder.CreateAdd(wrappedX, high);
    llvm::Value *wrappedSum = wrapRegisterWidthRoundTrip(builder, sum, 32);
    llvm::Value *shifted = builder.CreateAShr(
        wrappedSum, llvm::ConstantInt::get(i32, testCase.PostShift));
    llvm::Value *sign = builder.CreateLShr(
        wrappedSum, llvm::ConstantInt::get(i32, 31));
    builder.CreateRet(builder.CreateAdd(
        wrapRegisterWidthRoundTrip(builder, shifted, 32),
        wrapRegisterWidthRoundTrip(builder, sign, 32), "quotient"));
  }

  auto peephole =
      notdec::bin2llvm::runNativeRegisterPostRewritePeephole(module);

  bool allFolded = true;
  for (const Case &testCase : cases) {
    llvm::Function *function = module.getFunction(
        "x64_s32_addx_" + std::to_string(testCase.Divisor));
    allFolded &=
        expect(function != nullptr, "x64 i32 add-x function is missing") &&
        expect(functionHasSdivBy(*function, testCase.Divisor),
               "x64 i32 add-x chain was not folded back to sdiv") &&
        expect(!functionHasMul(*function),
               "x64 i32 add-x chain survived the sdiv fold");
  }

  return expect(peephole.SignedConstantDivisionsRecognized == std::size(cases),
                "unexpected number of recognized x64 i32 add-x divisions") &&
         allFolded &&
         verifyOk(module, "module failed verifier after x64 i32 add-x fold");
}

bool testUnsigned32BitAddCorrectionWithRoundTrips() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-unsigned-32-add-round-trips", context);
  llvm::Type *i32 = llvm::Type::getInt32Ty(context);
  llvm::Type *i64 = llvm::Type::getInt64Ty(context);
  struct Case {
    uint64_t Divisor;
    uint64_t Magic;
    unsigned PostShift;
  };
  // (d, M, 最后的 shrl 移位量) 取自 clang -m64 -O2 对 uint32_t x/7、x/4095 的输出。
  const Case cases[] = {
      {7, 613566757ULL, 2},
      {4095, 1048833ULL, 11},
  };
  for (const Case &testCase : cases) {
    llvm::Function *function = llvm::Function::Create(
        llvm::FunctionType::get(i32, {i32}, false),
        llvm::GlobalValue::ExternalLinkage,
        "x64_u32_add_" + std::to_string(testCase.Divisor), module);
    llvm::BasicBlock *entry =
        llvm::BasicBlock::Create(context, "entry", function);
    llvm::IRBuilder<> builder(entry);
    llvm::Value *x = function->getArg(0);
    llvm::Value *wide = builder.CreateZExt(x, i64);
    llvm::Value *product = builder.CreateNUWMul(
        wide, llvm::ConstantInt::get(i64, testCase.Magic));
    llvm::Value *high = builder.CreateTrunc(
        builder.CreateLShr(product, llvm::ConstantInt::get(i64, 32)), i32);
    llvm::Value *difference = builder.CreateSub(x, high);
    llvm::Value *halved = builder.CreateLShr(
        wrapRegisterWidthRoundTrip(builder, difference, 32),
        llvm::ConstantInt::get(i32, 1));
    llvm::Value *sum = builder.CreateAdd(
        high, wrapRegisterWidthRoundTrip(builder, halved, 32));
    builder.CreateRet(builder.CreateLShr(
        wrapRegisterWidthRoundTrip(builder, sum, 32),
        llvm::ConstantInt::get(i32, testCase.PostShift), "quotient"));
  }

  auto peephole =
      notdec::bin2llvm::runNativeRegisterPostRewritePeephole(module);

  bool allFolded = true;
  for (const Case &testCase : cases) {
    llvm::Function *function = module.getFunction(
        "x64_u32_add_" + std::to_string(testCase.Divisor));
    allFolded &=
        expect(function != nullptr, "x64 u32 add-form function is missing") &&
        expect(functionHasUdivBy(*function, testCase.Divisor),
               "x64 u32 add-form chain was not folded back to udiv") &&
        expect(!functionHasMul(*function),
               "x64 u32 add-form chain survived the udiv fold");
  }

  return expect(peephole.ConstantDivisionsRecognized == std::size(cases),
                "unexpected number of recognized x64 u32 add-form divisions") &&
         expect(peephole.SignedConstantDivisionsRecognized == 0,
                "x64 u32 add-form fold counted as signed") &&
         allFolded &&
         verifyOk(module, "module failed verifier after x64 u32 add-form fold");
}

bool testWrappedRoundTripUnknownMagicIsLeftAlone() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-wrapped-unknown-magic", context);
  llvm::Type *i32 = llvm::Type::getInt32Ty(context);
  llvm::Type *i64 = llvm::Type::getInt64Ty(context);
  llvm::Function *function = llvm::Function::Create(
      llvm::FunctionType::get(i32, {i32}, false),
      llvm::GlobalValue::ExternalLinkage, "wrapped_junk", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *x = function->getArg(0);
  llvm::Value *wide = builder.CreateSExt(x, i64);
  llvm::Value *product = builder.CreateNSWMul(
      wide, llvm::ConstantInt::get(
                i64, llvm::APInt(32, 0x12345678ULL).sext(64)));
  llvm::Value *high = builder.CreateTrunc(
      builder.CreateLShr(product, llvm::ConstantInt::get(i64, 32)), i32);
  llvm::Value *sum =
      builder.CreateAdd(builder.CreateTrunc(wide, i32), high);
  llvm::Value *wrappedSum = wrapRegisterWidthRoundTrip(builder, sum, 32);
  llvm::Value *shifted = builder.CreateAShr(
      wrappedSum, llvm::ConstantInt::get(i32, 2));
  llvm::Value *sign = builder.CreateLShr(
      wrappedSum, llvm::ConstantInt::get(i32, 31));
  builder.CreateRet(builder.CreateAdd(
      wrapRegisterWidthRoundTrip(builder, shifted, 32),
      wrapRegisterWidthRoundTrip(builder, sign, 32), "quotient"));

  auto peephole =
      notdec::bin2llvm::runNativeRegisterPostRewritePeephole(module);

  bool sawSdiv = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    sawSdiv |= inst.getOpcode() == llvm::Instruction::SDiv;
  }

  return expect(peephole.SignedConstantDivisionsRecognized == 0,
                "wrapped non-division magic must not be recognized") &&
         expect(functionHasMul(*function),
                "wrapped non-division magic lost its multiply") &&
         expect(!sawSdiv, "wrapped non-division magic was folded to an sdiv") &&
         verifyOk(module, "module failed verifier after wrapped junk skip");
}

bool testFinalCleanupRunsGlobalDCEForUnusedIntrinsicDeclarations() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-globaldce", context);

  llvm::Intrinsic::getOrInsertDeclaration(&module,
                                          llvm::Intrinsic::ssub_with_overflow,
                                          {llvm::Type::getInt8Ty(context)});
  auto *helperType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {}, false);
  llvm::Function::Create(helperType, llvm::GlobalValue::ExternalLinkage,
                         "notdec.register.summary_return.i64", module);

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "final_cleanup_globaldce", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  return expect(!moduleHasOverflowIntrinsicDeclaration(module),
                "unused overflow intrinsic declaration remained") &&
         expect(!moduleHasFunctionNamed(module,
                                        "notdec.register.summary_return.i64"),
                "unused register summary helper declaration remained") &&
         verifyOk(module,
                  "module failed verifier after globaldce cleanup test");
}

bool testFinalCleanupDropsDeadUnknownValueCalls() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-dead-unknown", context);

  auto *unknownType =
      llvm::FunctionType::get(llvm::Type::getInt32Ty(context), {}, false);
  llvm::Function *unknown =
      llvm::Function::Create(unknownType, llvm::GlobalValue::ExternalLinkage,
                             "notdec.unknown.i32", module);
  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt32Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "final_cleanup_dead_unknown", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCall(unknown, {}, "dead_unknown");
  llvm::CallInst *live = builder.CreateCall(unknown, {}, "live_unknown");
  builder.CreateStore(live, sink);
  builder.CreateRetVoid();

  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  unsigned unknownCalls = 0;
  bool storeUsesUnknown = false;
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    if (isUnknownValueCall(&inst)) {
      ++unknownCalls;
    }
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    storeUsesUnknown |= store != nullptr &&
                        store->getPointerOperand() == sink &&
                        isUnknownValueCall(store->getValueOperand());
  }

  return expect(unknownCalls == 1,
                "final cleanup kept wrong number of unknown value calls") &&
         expect(storeUsesUnknown, "live unknown value call was removed") &&
         expect(cleanup.DeadUnknownValuesRemoved == 1,
                "final cleanup did not count dead unknown value") &&
         expect(moduleHasUsedFunctionNamed(module, "notdec.unknown.i32"),
                "live unknown helper declaration was removed") &&
         verifyOk(module,
                  "module failed verifier after dead unknown cleanup test");
}

bool testFinalCleanupDropsUnusedUserOpPlaceholders() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-userop", context);

  auto *type = llvm::FunctionType::get(
      llvm::Type::getInt64Ty(context),
      {llvm::Type::getInt64Ty(context), llvm::Type::getInt8Ty(context)}, false);
  llvm::Function *used = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage,
      "notdec.register.userop.crc32.i64.i64.i8", module);
  llvm::Function *unused = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context),
                              {llvm::Type::getInt32Ty(context)}, false),
      llvm::GlobalValue::ExternalLinkage,
      "notdec.register.userop.cpuid.i64.i32", module);

  auto *callerType = llvm::FunctionType::get(llvm::Type::getInt64Ty(context),
                                             {llvm::Type::getInt64Ty(context)},
                                             false);
  llvm::Function *function = llvm::Function::Create(
      callerType, llvm::GlobalValue::ExternalLinkage, "userop_caller", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateRet(builder.CreateCall(
      used, {function->getArg(0),
             llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 0x56)},
      "crc"));

  // 关掉 GlobalDCE，确保是 helper 声明清理在做这件事（真实 pipeline 里
  // GlobalDCE 可能先把没人用的声明一起带走）。
  notdec::bin2llvm::NativeRegisterFinalCleanupOptions options;
  options.RunGlobalDCE = false;
  auto cleanup =
      notdec::bin2llvm::runNativeRegisterFinalCleanup(module, options);

  return expect(module.getFunction("notdec.register.userop.crc32.i64.i64.i8") !=
                    nullptr,
                "used userop placeholder was removed") &&
         expect(module.getFunction("notdec.register.userop.cpuid.i64.i32") ==
                    nullptr,
                "unused userop placeholder declaration was kept") &&
         expect(cleanup.HelperDeclarationsRemoved == 1,
                "unused userop declaration removal was not counted") &&
         verifyOk(module,
                  "module failed verifier after userop declaration cleanup");
}

bool testFinalCleanupLowersValueRangeHelpers() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-final-cleanup-value-range", context);

  auto *sink = new llvm::GlobalVariable(
      module, llvm::Type::getInt64Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "sink");
  auto *type = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {llvm::Type::getInt64Ty(context)}, false);
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "final_cleanup_value_range", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Argument *arg = function->getArg(0);
  llvm::Function *extract =
      notdec::bin2llvm::getOrInsertNativeRegisterValueExtract(
          module, llvm::Type::getInt64Ty(context),
          llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Function *insert =
      notdec::bin2llvm::getOrInsertNativeRegisterValueInsert(
          module, llvm::Type::getInt64Ty(context),
          llvm::Type::getInt32Ty(context), 64, 32);
  llvm::Value *low = builder.CreateCall(
      extract,
      {arg, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  llvm::Value *full = builder.CreateCall(
      insert,
      {arg, low, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  builder.CreateStore(full, sink);
  builder.CreateRetVoid();

  auto cleanup = notdec::bin2llvm::runNativeRegisterFinalCleanup(module);

  bool hasValueRangeCall = false;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    hasValueRangeCall |= call != nullptr &&
                         call->getCalledFunction() != nullptr &&
                         notdec::bin2llvm::isNativeRegisterValueRangeName(
                             call->getCalledFunction()->getName());
  }

  return expect(cleanup.ValueRangeHelpersLowered == 2,
                "value range helpers were not lowered") &&
         expect(!hasValueRangeCall, "value range helper call remained") &&
         expect(!moduleHasFunctionNamed(module, "notdec.reg.extract.i64.i32"),
                "value range extract declaration remained") &&
         expect(!moduleHasFunctionNamed(module, "notdec.reg.insert.i64.i32"),
                "value range insert declaration remained") &&
         verifyOk(module,
                  "module failed verifier after value range helper cleanup");
}

bool testEraseDeadUnknownValueCalls() {
  llvm::LLVMContext context;
  llvm::Module module("final-cleanup-dead-unknown", context);
  auto *i64Type = llvm::Type::getInt64Ty(context);
  auto *sink = new llvm::GlobalVariable(
      module, i64Type, false, llvm::GlobalValue::ExternalLinkage, nullptr,
      "sink");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "dead_unknown_sweep", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::FunctionCallee unknown = module.getOrInsertFunction(
      "notdec.unknown.i64", llvm::FunctionType::get(i64Type, {}, false));
  builder.CreateCall(unknown);
  llvm::Value *live = builder.CreateCall(unknown);
  builder.CreateStore(live, sink);
  builder.CreateRetVoid();

  uint64_t removed = notdec::bin2llvm::eraseDeadUnknownValueCalls(module);
  unsigned remaining = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call != nullptr && call->getCalledFunction() != nullptr &&
        call->getCalledFunction()->getName() == "notdec.unknown.i64") {
      ++remaining;
    }
  }

  return expect(removed == 1, "unused unknown call was not removed") &&
         expect(remaining == 1, "used unknown call was removed") &&
         verifyOk(module, "module failed verifier after dead unknown sweep");
}

bool testMixedCallBoundaryAttributesUnknownToCallee() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-mixed-call-unknown-attribution", context);
  attachTestAbi(module);
  llvm::GlobalVariable *rdi = createRegisterGlobal(module, "RDI");
  auto *i64Type = llvm::Type::getInt64Ty(context);
  auto *sink = new llvm::GlobalVariable(
      module, i64Type, false, llvm::GlobalValue::ExternalLinkage, nullptr,
      "sink");
  auto *flag = new llvm::GlobalVariable(
      module, llvm::Type::getInt1Ty(context), false,
      llvm::GlobalValue::ExternalLinkage, nullptr, "flag");

  auto *voidType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  // callee：true 路径把 RDI 写成常量（非入口值），false 路径不动 RDI
  // ⇒ RDI 的出口 provenance 是 Mixed，调用点只能用占位值。
  // 这里故意用外部链接：遵守 ABI 的函数不能把 RDI 当返回通道，调用点才会落到
  // "值丢了"的归因路径上（内部函数现在可以给 RDI 加返回槽，见
  // testInternalCalleeReturnsCallerSavedRegister）。
  llvm::Function *callee = llvm::Function::Create(
      voidType, llvm::GlobalValue::ExternalLinkage, "mixed_rdi_callee", module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::BasicBlock *calleeWrite =
      llvm::BasicBlock::Create(context, "write", callee);
  llvm::BasicBlock *calleeDone =
      llvm::BasicBlock::Create(context, "done", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  llvm::Value *calleeCond =
      calleeBuilder.CreateLoad(llvm::Type::getInt1Ty(context), flag, "cond");
  calleeBuilder.CreateCondBr(calleeCond, calleeWrite, calleeDone);
  calleeBuilder.SetInsertPoint(calleeWrite);
  storeRegister(calleeBuilder, rdi, llvm::ConstantInt::get(i64Type, 7), "RDI");
  calleeBuilder.CreateBr(calleeDone);
  calleeBuilder.SetInsertPoint(calleeDone);
  calleeBuilder.CreateRetVoid();

  // caller：entry 先读 RDI（ReadEntry=true ⇒ 签名里出现 RDI 参数），调用 Mixed
  // callee 之后，在没有本地定义的 merge 块再读 RDI：值只能是调用边界丢的。
  llvm::Function *caller = llvm::Function::Create(
      voidType, llvm::GlobalValue::InternalLinkage, "mixed_rdi_caller", module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", caller);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", caller);
  llvm::BasicBlock *right = llvm::BasicBlock::Create(context, "right", caller);
  llvm::BasicBlock *merge = llvm::BasicBlock::Create(context, "merge", caller);
  llvm::IRBuilder<> builder(entry);
  llvm::Value *input = loadRegister(builder, rdi, "RDI", "input");
  builder.CreateStore(input, sink);
  builder.CreateCall(voidType, callee);
  llvm::Value *cond =
      builder.CreateLoad(llvm::Type::getInt1Ty(context), flag, "cond2");
  builder.CreateCondBr(cond, left, right);
  // 两个分支各自留下不同的副作用，避免 SimplifyCFG 把 left/right 合掉、
  // 把 phi 折成一个非 phi 值（那样 unknown 会被 InstCombine 重建并丢 metadata）。
  builder.SetInsertPoint(left);
  builder.CreateStore(llvm::ConstantInt::get(i64Type, 1), sink);
  builder.CreateBr(merge);
  builder.SetInsertPoint(right);
  builder.CreateStore(llvm::ConstantInt::get(i64Type, 2), sink);
  builder.CreateBr(merge);
  builder.SetInsertPoint(merge);
  llvm::Value *after = loadRegister(builder, rdi, "RDI", "after");
  builder.CreateStore(after, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::runNativeRegisterElimination(module);
  llvm::Function *rewritten = module.getFunction("mixed_rdi_caller");
  std::map<std::string, uint64_t> counts =
      notdec::bin2llvm::countRemainingUnknownValues(module);

  bool reasonAttributed = false;
  bool calleeAttributed = false;
  if (rewritten != nullptr) {
    for (llvm::Instruction &inst : llvm::instructions(*rewritten)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr || call->getCalledFunction() == nullptr ||
          !call->getCalledFunction()->getName().starts_with("notdec.unknown.")) {
        continue;
      }
      if (llvm::MDNode *source = call->getMetadata("notdec.unknown.source")) {
        for (const llvm::MDOperand &operand : source->operands()) {
          auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
          if (text == nullptr) {
            continue;
          }
          reasonAttributed |= text->getString() == "reason=call_effect_mixed";
          calleeAttributed |= text->getString() == "callee=mixed_rdi_callee";
        }
      }
    }
  }

  return expect(rewritten != nullptr, "mixed call caller missing") &&
         expect(reasonAttributed,
                "mixed call boundary unknown was not attributed to the call") &&
         expect(calleeAttributed,
                "mixed call boundary unknown did not name the callee") &&
         expect(counts.count("call_effect_mixed:mixed_rdi_callee:RDI") != 0,
                "remaining unknown tally did not group by mixed call callee") &&
         verifyOk(module,
                  "module failed verifier after unknown attribution test");
}


// ABI 未定义位的记账口径：带 !notdec.abi_undefined 的值不能算进"建模缺口"，
// 否则修 ABI 事实会被当成修 bug，指标会朝错的方向优化。
bool testAbiUndefinedValuesAreAuditedSeparately() {
  llvm::LLVMContext context;
  llvm::Module module("summary-ssa-abi-undefined-audit", context);
  llvm::Type *i64Type = llvm::Type::getInt64Ty(context);
  auto *sink = new llvm::GlobalVariable(
      module, i64Type, false, llvm::GlobalValue::ExternalLinkage, nullptr,
      "sink");
  llvm::FunctionCallee unknown = module.getOrInsertFunction(
      "notdec.unknown.i64", llvm::FunctionType::get(i64Type, {}, false));

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function = llvm::Function::Create(
      type, llvm::GlobalValue::InternalLinkage, "audited_unknowns", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  auto *gap = builder.CreateCall(unknown, {}, "gap");
  gap->setMetadata(
      "notdec.unknown.source",
      llvm::MDNode::get(context,
                        {llvm::MDString::get(
                             context, "reason=callee_summary_missing"),
                         llvm::MDString::get(context, "function=audited_unknowns"),
                         llvm::MDString::get(context, "callee=some_callee")}));
  auto *abiUndefined = builder.CreateCall(unknown, {}, "abi.undefined");
  abiUndefined->setMetadata(
      "notdec.unknown.source",
      llvm::MDNode::get(context,
                        {llvm::MDString::get(context,
                                             "reason=abi_undefined_bits"),
                         llvm::MDString::get(context,
                                             "function=audited_unknowns"),
                         llvm::MDString::get(context, "callee=double_callee"),
                         llvm::MDString::get(context, "register=ZMM0"),
                         llvm::MDString::get(context, "kind=abi_undefined")}));
  abiUndefined->setMetadata(
      "notdec.abi_undefined",
      llvm::MDNode::get(context,
                        {llvm::MDString::get(context, "register=ZMM0"),
                         llvm::MDString::get(context, "callee=double_callee")}));
  builder.CreateStore(gap, sink);
  builder.CreateStore(abiUndefined, sink);
  builder.CreateRetVoid();

  notdec::bin2llvm::NativeRegisterValueSourceAudit audit =
      notdec::bin2llvm::auditNativeRegisterValueSources(module);
  std::map<std::string, uint64_t> remaining =
      notdec::bin2llvm::countRemainingUnknownValues(module);

  uint64_t countedUnknowns = 0;
  for (const auto &[reason, count] : audit.UnknownValuesByReason) {
    (void)reason;
    countedUnknowns += count;
  }
  uint64_t countedRemaining = 0;
  for (const auto &[key, count] : remaining) {
    (void)key;
    countedRemaining += count;
  }

  return expect(audit.AbiUndefinedValues == 1,
                "audit did not isolate the abi undefined value") &&
         expect(audit.AbiUndefinedByCallee.count("double_callee") == 1,
                "audit lost the abi undefined callee attribution") &&
         expect(countedUnknowns == 1,
                "audit counted the abi undefined value as a model unknown") &&
         expect(audit.UnknownValuesByReason.count("callee_summary_missing") == 1,
                "audit lost the real unknown reason") &&
         expect(remaining.count("callee_summary_missing:some_callee") == 1,
                "remaining unknown tally lost the real unknown") &&
         expect(countedRemaining == 1,
                "remaining unknown tally still counts abi undefined values") &&
         verifyOk(module, "module failed verifier after abi undefined audit test");
}

} // namespace


// 寄存器感知 DCE 的机制测试：死写的值换成 0、写本身保留、值链被删；
// 输入列表之外的写不动。
bool testRegisterValueDceClearsDeadWriteAndPrunesChain() {
  llvm::LLVMContext context;
  llvm::Module module("register-value-dce", context);
  llvm::GlobalVariable *r8 = createRegisterGlobal(module, "R8");

  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                             "value_dce_dead", module);
  llvm::BasicBlock *entry =
      llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::Function *partialRead =
      notdec::bin2llvm::getOrInsertNativeRegisterPartialRead(
          module, r8->getType(), llvm::Type::getInt32Ty(context), 64, 32);
  llvm::CallInst *read = builder.CreateCall(
      partialRead, {r8, builder.getInt64(0)}, "R8.read");
  llvm::Value *extended =
      builder.CreateZExt(read, llvm::Type::getInt64Ty(context), "R8.zext");
  llvm::StoreInst *store = storeRegister(builder, r8, extended, "R8");
  llvm::ReturnInst *ret = builder.CreateRetVoid();

  auto summary = notdec::bin2llvm::pruneDeadRegisterValues(module, {store});
  bool ok = expect(summary.DeadWrites == 1, "value dce did not see dead write");
  ok &= expect(summary.ValuesCleared == 1,
               "value dce did not clear the dead write value");
  ok &= expect(summary.InstructionsRemoved == 2,
               "value dce did not prune the dead read chain");
  ok &= expect(summary.RegisterReadsRemoved == 1,
               "value dce did not count the pruned register read");
  ok &= expect(store->getParent() == entry,
               "value dce must keep the dead write itself");
  auto *zero = llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
  ok &= expect(zero != nullptr && zero->isZero(),
               "value dce did not replace the dead write value with 0");
  ok &= expect(countPartialReadCalls(*function, r8, 0, 32) == 0,
               "value dce left the dead partial read in place");

  // 不在死写列表里的写保持原样（只按传入的观察结果动）。
  builder.SetInsertPoint(ret);
  llvm::CallInst *liveRead = builder.CreateCall(
      partialRead, {r8, builder.getInt64(0)}, "R8.live_read");
  storeRegister(builder, r8, liveRead, "R8");
  auto empty = notdec::bin2llvm::pruneDeadRegisterValues(module, {});
  ok &= expect(empty.ValuesCleared == 0 && empty.InstructionsRemoved == 0,
               "value dce changed the module with an empty dead list");
  ok &= expect(countPartialReadCalls(*function, r8, 0, 32) == 1,
               "value dce removed a read that was not in the dead list");
  ok &= expect(verifyOk(module, "value dce module failed verifier"),
               "value dce broke the verifier");
  return ok;
}

int main() {
  bool ok = true;
  ok &= testRegisterValueDceClearsDeadWriteAndPrunesChain();
  ok &= testDuplicatePredecessorEdgesKeepPhiComplete();
  ok &= testPartialReadLoopPassthroughUsesDominatorTree();
  ok &= testX87Fp80StoreStillFeedsRange();
  ok &= testX87WindowShiftLoadFollowsRangeState();
  ok &= testCalleeSavedRegisterSurvivesInternalCall();
  ok &= testRegisterPointerPhiLoadIsCanonicalized();
  ok &= testUnknownPhiIncomingUsesFrozenPoison();
  ok &= testSelfOnlyPhiBecomesOpaqueUnknown();
  ok &= testFsOffsetPreservedAcrossExternalCall();
  ok &= testPreservedCallKeepsPreviousValue();
  ok &= testRewrittenExternalCallPreservesUnaffectedRegister();
  ok &= testDemandedReturnCreatesCallValue();
  ok &= testExternalReturnUsesRangeCallValue();
  ok &= testIndirectCallReturnHelperIsRewritten();
  ok &= testIndirectConservativeReturnShapeExcludesRdx();
  ok &= testIndirectCallInputsUseLocalDefinitionEvidence();
  ok &= testIntrinsicDoesNotCreateCallValue();
  ok &= testOverwrittenStoreIsRemoved();
  ok &= testCrossBlockDeadStoreIsRemoved();
  ok &= testAbiInputStoreBeforeCallIsKept();
  ok &= testFinalCallArgClobberWarningIsKept();
  ok &= testKnownZeroArgExternalDropsAbiInputs();
  ok &= testKnownZeroArgExternalTypedReturnIsMaterialized();
  ok &= testKnownErrnoLocationReturnIsMaterialized();
  ok &= testKnownFixedArgExternalTruncatesAbiInputs();
  ok &= testFlagStoreBeforeVolatileCallIsRemoved();
  ok &= testKnownFiveArgExternalUsesFiveInputs();
  ok &= testKnownFixedExternalArities();
  ok &= testMismatchedDirectCallUseUsesReturnExtract();
  ok &= testKnownExternalUsesSingleIntegerReturn();
  ok &= testUnknownExternalTreatsRdxAsClobberNotReturn();
  ok &= testUnknownExternalFloatReturnPicksSingleXmmSlot();
  ok &= testAbiUndefinedValuesAreAuditedSeparately();
  ok &= testUnknownExternalClobberArgBecomesUnknown();
  ok &= testInternalReturnDoesNotExposeExternalClobber();
  ok &= testClobberReturnPhiPreservesMixedPath();
  ok &= testConsumedScratchRegisterIsNotAnExternalArgument();
  ok &= testFlagOnlyRegisterReadKeepsArgumentArity();
  ok &= testIndirectCallWithoutEvidenceHasNoArguments();
  ok &= testConsumedRegisterBeforeLaterArgumentKeepsArity();
  ok &= testExternalPrototypeJsonOverridesInferredArity();
  ok &= testExternalPrototypeJsonAcceptsLongDouble();
  ok &= testExternalPrototypeJsonAcceptsVector128();
  ok &= testExternalPrototypeJsonMultiRegisterReturns();
  ok &= testPinnedSignatureOverridesInference();
  ok &= testUnknownExternalArityStopsAtClobberArg();
  ok &= testUnknownExternalArityStopsAtPhiClobberArg();
  ok &= testUnknownExternalArityCountsExplicitBinaryArg();
  ok &= testRecordedCallArgValueSurvivesDeadStoreCleanup();
  ok &= testInternalSignatureRewriteUsesArgsAndReturn();
  ok &= testAddressTakenInternalFunctionUseFollowsRewrite();
  ok &= testInternalSignatureRewriteUsesNonAbiReturn();
  ok &= testInternalCalleeReturnsCallerSavedRegister();
  ok &= testInternalSignatureRewriteUsesReadEntryReturnRegisterArg();
  ok &= testInternalSignatureShapeParamKeepsOverwrittenEntryRange();
  ok &= testPostSignatureCleanupRewritesInternalEntryRawLoad();
  ok &= testInternalSignatureRewriteUsesZmmArgAndReturn();
  ok &= testPreservedZmmEntryIsPassedAsInternalArgument();
  ok &= testInternalSignatureRewriteUsesZmmLowLaneReturn();
  ok &= testForeignArgumentInMovedBodyIsReplaced();
  ok &= testForeignMappedCallArgumentIsLocalized();
  ok &= testInternalCallArgBindingsKeepLaterArgsAfterEntryInput();
  ok &= testInternalSignatureParamsUseAbiOrder();
  ok &= testPostSignatureCleanupDropsAbiStoreBeforeUnrewrittenCall();
  ok &= testNoReturnExternalDoesNotCreateSummaryReturn();
  ok &= testExternalPrototypeJsonOverlaysDefaultNoReturn();
  ok &= testInternalNoReturnCallFallthroughIsTruncated();
  ok &= testXmmAbiEffectUsesZmmBackingWithoutSignatureReturn();
  ok &= testUnknownExternalFloatAbiClobberUsesWholeZmm();
  ok &= testUnknownExternalIntegerClobberUsesRangeCallValue();
  ok &= testKnownPowUsesFloatAbiSlots();
  ok &= testKnownUnaryLibmUsesFloatAbiSlots();
  ok &= testPartialKeepHighStoreIsDemandRewritten();
  ok &= testPartialWriteHelperIsConsumedBySummarySSA();
  ok &= testPartialReadHelperIsConsumedBySummarySSA();
  ok &= testFullStoreFeedsPartialReadThroughRangeSSA();
  ok &= testDeadPartialReadHelperIsRemovedBySummarySSA();
  ok &= testDeadSummaryCallValueHelperIsRemovedBySummarySSA();
  ok &= testConsecutivePartialReadHelpersAreConsumedBySummarySSA();
  ok &= testConsecutivePartialReadXorIsZeroedAfterSummarySSA();
  ok &= testDuplicatePartialReadXorAfterUnknownCallIsZeroed();
  ok &= testDeadPartialWriteUsesRangeLiveness();
  ok &= testFloatCallEffectUsesRangeLiveness();
  ok &= testCallArgSlotSpansPlannedRangeSegments();
  ok &= testPartialWriteHelperNameSurvivesSignatureRewrite();
  ok &= testPartialReadHelperNameSurvivesSignatureRewrite();
  ok &= testPartialDemandPoisonReplacementIsMarked();
  ok &= testPartialDemandKeepsDynamicShiftAmount();
  ok &= testPartialDemandKeepsSameWidthMulOperand();
  ok &= testPartialDemandKeepsSelectCondition();
  ok &= testPoisonRangeExtractBecomesAttributedUnknown();
  ok &= testPoisonAggregateBaseGetsFilledFields();
  ok &= testPartialDemandKeepsDivRemOperand();
  ok &= testPartialZmmKeepHighStoreIsDemandRewritten();
  ok &= testPartialZmmNakedKeepHighStoreIsDemandRewritten();
  ok &= testPartialZmmDisjointLaneChainIsDemandRewritten();
  ok &= testPartialDemandKeepsMemoryPointerRegisterAddress();
  ok &= testFinalCleanupDropsDeadRegisterGlobalsAndSummaryMetadata();
  ok &= testFinalCleanupKeepsMetadataWhenRegisterAccessRemains();
  ok &= testFinalCleanupReplacesPoisonBranchCondition();
  ok &= testFinalCleanupReplacesPoisonReturnValue();
  ok &= testFinalCleanupCountsPartialWriteResidue();
  ok &= testFinalCleanupCountsPartialReadResidue();
  ok &= testFinalCleanupDropsDeadPartialReadsAfterValueRangeLowering();
  ok &= testUnsignedAddCorrectionMagicFold();
  ok &= testSignedNegativeMagicAddXFormFold();
  ok &= testSignedDivisionSubFormFold();
  ok &= testX86_64Signed32BitAshrBeforeTruncFold();
  ok &= testX86_64Signed32BitNegativeMagicAddXWithRoundTrips();
  ok &= testUnsigned32BitAddCorrectionWithRoundTrips();
  ok &= testWrappedRoundTripUnknownMagicIsLeftAlone();
  ok &= testFinalCleanupRunsGlobalDCEForUnusedIntrinsicDeclarations();
  ok &= testFinalCleanupDropsDeadUnknownValueCalls();
  ok &= testFinalCleanupDropsUnusedUserOpPlaceholders();
  ok &= testFinalCleanupLowersValueRangeHelpers();
  ok &= testEraseDeadUnknownValueCalls();
  ok &= testMixedCallBoundaryAttributesUnknownToCallee();
  return ok ? 0 : 1;
}
