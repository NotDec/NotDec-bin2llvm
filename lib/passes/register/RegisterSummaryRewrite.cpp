#include "RegisterSummaryInternal.h"

namespace notdec::bin2llvm {

std::optional<ValueOrigin> Analyzer::entryValueOrigin(llvm::Value *value,
                                              const State &state) const {
    value = value->stripPointerCasts();
    auto it = state.ValueOrigins.find(value);
    if (it != state.ValueOrigins.end()) {
      return it->second;
    }
    auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(value);
    if (binary == nullptr || (binary->getOpcode() != llvm::Instruction::Add &&
                              binary->getOpcode() != llvm::Instruction::Sub)) {
      return std::nullopt;
    }

    auto parseConstantOffset = [](llvm::Value *constant,
                                  bool negate) -> std::optional<int64_t> {
      auto *intConstant = llvm::dyn_cast<llvm::ConstantInt>(constant);
      if (intConstant == nullptr || intConstant->getBitWidth() > 64) {
        return std::nullopt;
      }
      int64_t value = intConstant->getSExtValue();
      return negate ? -value : value;
    };

    if (auto origin = entryValueOrigin(binary->getOperand(0), state)) {
      if (auto offset = parseConstantOffset(binary->getOperand(1),
                                            binary->getOpcode() ==
                                                llvm::Instruction::Sub)) {
        origin->Offset += *offset;
        return origin;
      }
    }
    if (binary->getOpcode() == llvm::Instruction::Add) {
      if (auto origin = entryValueOrigin(binary->getOperand(1), state)) {
        if (auto offset = parseConstantOffset(binary->getOperand(0), false)) {
          origin->Offset += *offset;
          return origin;
        }
      }
    }
    return std::nullopt;
  }

RegisterOriginKind
  Analyzer::registerOriginKindForStoredValue(llvm::Value *value,
                                   const State &state) const {
    if (entryValueOrigin(value, state)) {
      return RegisterOriginKind::Entry;
    }
    return RegisterOriginKind::Local;
  }

void Analyzer::markEntryValueRead(llvm::Value *value, State &state) const {
    std::optional<ValueOrigin> origin = entryValueOrigin(value, state);
    if (!origin || origin->Offset != 0) {
      return;
    }
    // 按该值实际用到的位记录入口读：整寄存器值是全宽，窄 partial read
    // 值只带读到的位，避免入口读沿整寄存器扩散。
    llvm::APInt mask = origin->Mask.getBitWidth() == 0
                           ? fullRegisterMask(origin->Base)
                           : origin->Mask;
    readRegister(state, origin->Base, mask);
  }

void Analyzer::markInstructionEntryValueReads(llvm::Instruction &inst,
                                      State &state) const {
    for (llvm::Value *operand : inst.operands()) {
      markEntryValueRead(operand, state);
    }
  }

NativeStackAddressAnalysis &
  Analyzer::stackAddressAnalysis(llvm::Function &function) const {
    std::unique_ptr<NativeStackAddressAnalysis> &analysis =
        StackAddressAnalyses[&function];
    if (!analysis) {
      analysis = std::make_unique<NativeStackAddressAnalysis>(
          function, stackPointerGlobal(), Abi.StackPointer);
    }
    return *analysis;
  }

std::optional<StackSlotKey>
  Analyzer::stackSlotKey(const NativeStackAddress &address) const {
    llvm::Value *base = address.Base;
    if (address.Kind == NativeStackAddressKind::EntryStackPointer) {
      base = stackPointerGlobal();
    }
    if (base == nullptr) {
      return std::nullopt;
    }
    return StackSlotKey{base, address.Offset};
  }

std::optional<StackSlotKey>
  Analyzer::callsiteStackSlot(llvm::CallBase &call,
                    const NativeRegisterCallInputSlot &slot) const {
    llvm::Function *function = call.getFunction();
    const auto *callInst = llvm::dyn_cast<llvm::CallInst>(&call);
    const bool isTailCall = callInst != nullptr && callInst->isTailCall();
    if (function == nullptr ||
        slot.StackOffset >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        (!isTailCall && Abi.StackShift >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))) {
      return std::nullopt;
    }
    int64_t stackOffset = static_cast<int64_t>(slot.StackOffset);
    if (isTailCall) {
      // A tail jump reuses the caller's return address.  Its callee-entry stack
      // slots therefore stay in the caller's entry-SP coordinate even when a
      // relative/aligned SP value remains in the lifted epilogue.
      return stackSlotKey(NativeStackAddress{
          NativeStackAddressKind::EntryStackPointer, nullptr, stackOffset});
    }
    std::optional<NativeStackAddress> current =
        stackAddressAnalysis(*function).stackPointerBefore(call);
    if (!current) {
      return std::nullopt;
    }
    // A normal CALL pushes a return address, so cspec callee-entry offsets are
    // shifted back to caller-side stores.
    int64_t stackShift = static_cast<int64_t>(Abi.StackShift);
    int64_t delta = stackOffset >= stackShift ? stackOffset - stackShift
                                              : -(stackShift - stackOffset);
    std::optional<NativeStackAddress> address =
        nativeStackAddressWithOffset(*current, delta);
    return address ? stackSlotKey(*address) : std::nullopt;
  }

std::optional<StackSlotKey>
  Analyzer::fixedStackSlot(llvm::Value *pointer, llvm::Instruction &instruction) const {
    llvm::Function *function = instruction.getFunction();
    if (function == nullptr) {
      return std::nullopt;
    }
    std::optional<NativeStackAddress> address =
        stackAddressAnalysis(*function).addressForPointer(pointer);
    return address ? stackSlotKey(*address) : std::nullopt;
  }

bool Analyzer::hasSavedEntryRegister(const State &state,
                             llvm::GlobalVariable *global) const {
    for (const auto &[slot, saved] : state.StackSlots) {
      (void)slot;
      if (saved == global) {
        return true;
      }
    }
    return false;
  }

State Analyzer::exitStateWithSavedRegisters(const State &state) const {
    State adjusted = state;
    for (const auto &[global, unit] : Units) {
      if (isIgnored(unit, Options) || unit.Name == Abi.StackPointer ||
          Abi.Unaffected.count(unit.Name) == 0) {
        continue;
      }
      Cell cell = cellIn(adjusted, global);
      if (!cell.MayNonEntry.isZero() && hasSavedEntryRegister(state, global)) {
        restoreRegister(adjusted, global, fullRegisterMask(global));
      }
    }
    return adjusted;
  }

void Analyzer::transferCall(llvm::CallBase &call, State &state) {
    llvm::Function *callee = call.getCalledFunction();
    // A trusted vararg prototype (for example the in-binary aprintf helper)
    // overrides body-derived fixed arguments.  Without this, the variadic
    // register save area makes every ABI input look like a fixed parameter and
    // the fake reads propagate back into all callers.
    if (const NativeExternalCallShape *shape = externalCallShape(call)) {
      applyExternalCallEffect(state, *shape);
      consumeCallerStackArgEvidence(state);
      if (shape->NoReturn) {
        markNoReturnExit(state);
      }
      return;
    }
    if (callee != nullptr && !callee->isDeclaration() &&
        Effects.count(callee) != 0) {
      const FunctionEffect &effect = Effects[callee];
      applyFunctionEffect(effect, state);
      consumeCallerStackArgEvidence(state);
      if (effect.NoReturn) {
        markNoReturnExit(state);
      }
      return;
    }
    applyUnknownExternalCallEffect(state);
    consumeCallerStackArgEvidence(state);
    if (callee != nullptr && callee->isDeclaration() &&
        callee->hasFnAttribute(llvm::Attribute::NoReturn)) {
      markNoReturnExit(state);
    }
  }

void Analyzer::consumeCallerStackArgEvidence(State &state) const {
    // A caller stack slot is argument evidence only for the next real call.
    // Keep StackSlots for saved-register tracking, but discard this separate
    // evidence so a reused outgoing area cannot widen a later call signature.
    state.StackSlotOrigins.clear();
    state.StackSlotStores.clear();
  }

void Analyzer::markNoReturnExit(State &state) const {
    state.Reachable = false;
    state.EndsInNoReturn = true;
  }

void Analyzer::applyFunctionEffect(const FunctionEffect &effect, State &state) {
    for (const auto &[global, unit] : Units) {
      if (isIgnored(unit, Options)) {
        continue;
      }
      // callee-saved（ABI unaffected）寄存器由 ABI 保证 callee 原样保留：callee
      // summary 里的 may_non_entry 只说明它内部“存了又读回来”证不出相等，不能
      // 当成调用后的新值。否则调用方会为一次普通调用凭空造出 RBP/RBX 的
      // unknown（memcached FUN_16bb0 调 FUN_15820 就是这种）。对外部调用，
      // applyAbiCallClobbers 已经做同样的跳过。
      if (Abi.Unaffected.count(unit.Name) != 0) {
        continue;
      }
      Cell pre = cellIn(state, global);
      Cell callee = cellIn(effect, global);
      Cell post;
      // 三个事实都按 bit 组合：callee 只改写的位影响调用者对应位，未写
      // 位保持调用者状态。
      post.ReadEntry = pre.ReadEntry | (callee.ReadEntry & pre.MayEntry);
      post.MayEntry = callee.MayEntry & pre.MayEntry;
      post.MayNonEntry =
          (callee.MayEntry & pre.MayNonEntry) | callee.MayNonEntry;
      state.Cells[global] = post;
      bool calleeWroteAll = callee.MayEntry.isZero() && !callee.MayNonEntry.isZero();
      bool calleeWroteSome =
          !callee.MayEntry.isZero() && !callee.MayNonEntry.isZero();
      if (calleeWroteAll) {
        setFullRegisterOrigin(state, global, RegisterOriginKind::CallClobber);
      } else if (calleeWroteSome) {
        setFullRegisterOrigin(state, global, RegisterOriginKind::Mixed);
      }
    }
  }

void Analyzer::applyCallInputs(State &state,
                       const std::vector<NativeRegisterCallInputSlot> &inputs) {
    for (const NativeRegisterCallInputSlot &input : inputs) {
      if (input.Kind != NativeRegisterCallInputSlotKind::Register) {
        continue;
      }
      auto unitIt = UnitsByName.find(input.UnitName);
      if (unitIt == UnitsByName.end()) {
        continue;
      }
      const RegisterUnit &unit = Units.at(unitIt->second);
      if (!isIgnored(unit, Options)) {
        // 只把 slot 覆盖的位当作"入口值被读过"：调用点传参只准备了自己
        // 的位，其余位是调用者自己的状态，不归 callee 读。
        readRegister(state, unit.Global,
                     registerRangeMask(unit.Global, input.OffsetBits,
                                       input.SizeBits));
      }
    }
  }

void Analyzer::applyAbiInputs(State &state) {
    for (const auto &[global, unit] : Units) {
      if (isIgnored(unit, Options)) {
        continue;
      }
      if (Abi.Inputs.count(unit.Name) != 0) {
        readRegister(state, global, fullRegisterMask(global));
      }
    }
  }

void Analyzer::applyAbiCallClobbers(State &state) {
    for (const auto &[global, unit] : Units) {
      if (isIgnored(unit, Options) || Abi.Unaffected.count(unit.Name) != 0 ||
          isSegmentBaseUnit(unit.Name)) {
        continue;
      }
      llvm::APInt clobberMask = fullMask(unit);
      if (clobberMask.getBitWidth() == 0 || clobberMask.isZero()) {
        continue;
      }
      writeRegister(state, global, clobberMask);
      setRegisterOrigin(state, global, clobberMask,
                        RegisterOriginKind::CallClobber);
    }
  }

void Analyzer::applyExternalCallEffect(State &state,
                               const NativeExternalCallShape &shape) {
    applyCallInputs(state, shape.Inputs);
    applyAbiCallClobbers(state);
  }

void Analyzer::applyUnknownExternalCallEffect(State &state) {
    if (Options.UnknownExternalInputPolicy ==
        NativeRegisterUnknownExternalInputPolicy::AbiInputs) {
      applyAbiInputs(state);
    }
    applyAbiCallClobbers(state);
  }

void Analyzer::runTopDownDemand() {
    // 需求是双向的，所以这里是一个普通的单调 worklist 定点，而不是"自顶向下扫描"：
    //   caller -> callee：调用点用到的位进 callee 的 ExitDemand（callee 要返回它）；
    //   callee -> caller：调用点规则读 callee 的 ExitDemand（透传需求 = ExitDemand ∩
    //                     MayEntry ∩ MayNonEntry ∩ 调用点的 live），所以 callee 的
    //                     ExitDemand 变了要重算它的所有 caller。
    // 两个方向都只置位（mergeDemand / addDemand 只 OR），有限格 → 一定收敛；递归、
    // 互递归也一样，每轮只会多置几位。用 inQueue 去重，避免同一函数被反复入队。
    std::set<llvm::Function *> inQueue;
    std::deque<llvm::Function *> worklist;
    auto enqueue = [&](llvm::Function *function) {
      if (function != nullptr && inQueue.insert(function).second) {
        worklist.push_back(function);
      }
    };
    for (llvm::Function *function : Functions) {
      if (Callers[function].empty()) {
        seedAbiReturns(Demands[function]);
      }
      // 每个函数至少要分析一次：EntryDemand 是 caller 侧需求的输入。
      enqueue(function);
    }

    while (!worklist.empty()) {
      llvm::Function *function = worklist.front();
      worklist.pop_front();
      inQueue.erase(function);
      std::map<llvm::Function *, RegisterDemand> additions =
          analyzeCallerDemand(*function);
      for (auto &[callee, demand] : additions) {
        if (mergeDemand(Demands[callee].ExitDemand, demand)) {
          enqueue(callee);
          // 调用点规则也读 callee 的 ExitDemand（取它和 MayEntry ∩ MayNonEntry 的
          // 交集当透传需求），所以 callee 的 ExitDemand 变了，它的所有 caller 也要
          // 重算：这正是 callee → caller 那条反向需求边。
          auto callersIt = Callers.find(callee);
          if (callersIt != Callers.end()) {
            for (llvm::Function *caller : callersIt->second) {
              enqueue(caller);
            }
          }
        }
      }
    }
  }

void Analyzer::seedAbiReturns(FunctionDemand &demand) const {
    auto seed = [&](const std::string &name) {
      auto it = UnitsByName.find(name);
      if (it == UnitsByName.end()) {
        return;
      }
      const RegisterUnit &unit = Units.at(it->second);
      addDemand(demand.ExitDemand, it->second,
                namedMaskOrFull(Abi.OutputMasks, unit));
    };
    if (!Abi.IntegerOutputOrder.empty()) {
      seed(Abi.IntegerOutputOrder.front());
    }
    // ST0（SysV long double 返回）不 seed 根函数：与 RAX 不同，根入口点
    // 几乎不会真的用 ST0 返回，seed 会让 ST0 需求沿调用图反向级联，给
    // 一堆只在返回路径上被调用点 clobber 的函数误加 x86_fp80 返回槽。
    // ST0 的 ExitDemand 只来自真正读 @ST0 的调用点（调用方把 long double
    // 返回值接走），见 collectSummaryAbiFacts 的 ST0 输出槽补充。
  }

llvm::APInt
  Analyzer::valueDemand(llvm::Value *value,
              const std::map<llvm::Value *, llvm::APInt> &valueDemands) const {
    unsigned width = valueBitWidth(value);
    if (width == 0) {
      return llvm::APInt();
    }
    auto it = valueDemands.find(value);
    if (it == valueDemands.end()) {
      return llvm::APInt::getAllOnes(width);
    }
    return it->second.zextOrTrunc(width);
  }

std::map<llvm::Function *, RegisterDemand>
  Analyzer::analyzeCallerDemand(llvm::Function &function) {
    std::map<llvm::Value *, llvm::APInt> valueDemands =
        computeValueDemands(function, Units);
    std::map<llvm::BasicBlock *, RegisterDemand> in;
    std::map<llvm::BasicBlock *, RegisterDemand> out;
    std::map<llvm::Function *, RegisterDemand> additions;

    bool changed = true;
    while (changed) {
      changed = false;
      for (llvm::BasicBlock &block : llvm::reverse(function)) {
        RegisterDemand liveOut;
        for (llvm::BasicBlock *succ : llvm::successors(&block)) {
          mergeDemand(liveOut, in[succ]);
        }
        if (llvm::isa<llvm::ReturnInst>(block.getTerminator())) {
          mergeDemand(liveOut, Demands[&function].ExitDemand);
        }
        RegisterDemand live =
            liveBeforeBlock(block, liveOut, additions, valueDemands);
        if (out[&block] != liveOut) {
          out[&block] = std::move(liveOut);
          changed = true;
        }
        if (in[&block] != live) {
          in[&block] = std::move(live);
          changed = true;
        }
      }
    }
    Demands[&function].EntryDemand = in[&function.getEntryBlock()];
    return additions;
  }

} // namespace notdec::bin2llvm
