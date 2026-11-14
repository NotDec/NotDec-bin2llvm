#include "RegisterSummaryInternal.h"

namespace notdec::bin2llvm {

NativeRegisterCallsiteValueOrigin
  Analyzer::callsiteOrigin(const State &state, llvm::CallBase &call,
                 const NativeRegisterCallInputSlot &slot,
                 StackSlotKey *resolvedKey ) const {
    if (slot.Kind == NativeRegisterCallInputSlotKind::Stack) {
      std::optional<StackSlotKey> key = callsiteStackSlot(call, slot);
      if (!key) {
        return NativeRegisterCallsiteValueOrigin::Unknown;
      }
      auto originFor = [&](const StackSlotKey &candidate)
          -> std::optional<NativeRegisterCallsiteValueOrigin> {
        auto originIt = state.StackSlotOrigins.find(candidate);
        if (originIt != state.StackSlotOrigins.end()) {
          return originIt->second;
        }
        if (state.StackSlots.count(candidate) != 0) {
          return NativeRegisterCallsiteValueOrigin::ForwardedEntry;
        }
        return std::nullopt;
      };
      if (std::optional<NativeRegisterCallsiteValueOrigin> origin =
              originFor(*key)) {
        if (resolvedKey != nullptr) {
          *resolvedKey = *key;
        }
        return *origin;
      }
      // After the stack frame rewrite, outgoing argument stores live in
      // notdec_stack.native under alloca-relative offsets.  Try that coordinate
      // space as a fallback so cspec stack slots still see their stores.
      if (std::optional<StackSlotKey> frameKey =
              entrySlotToNativeFrameKey(*call.getFunction(), *key)) {
        if (std::optional<NativeRegisterCallsiteValueOrigin> origin =
                originFor(*frameKey)) {
          if (resolvedKey != nullptr) {
            *resolvedKey = *frameKey;
          }
          return *origin;
        }
      }
      return NativeRegisterCallsiteValueOrigin::Unknown;
    }
    auto unitIt = UnitsByName.find(slot.UnitName);
    if (unitIt == UnitsByName.end()) {
      return NativeRegisterCallsiteValueOrigin::Unknown;
    }
    llvm::GlobalVariable *global = unitIt->second;
    llvm::APInt mask =
        registerRangeMask(global, slot.OffsetBits, slot.SizeBits);
    if (mask.getBitWidth() == 0 || mask.isZero()) {
      return NativeRegisterCallsiteValueOrigin::Unknown;
    }
    RegisterOriginBits bits = originIn(state, global);
    if ((bits.Local & mask) == mask) {
      // 浮点参数证据只认最后一次写是浮点指令的 Local 槽；movq/punpcklqdq
      // 整数打包写的 XMM 槽会退化成 Mixed，不参与参数推断。
      if (slot.Float && (bits.FloatLocal & mask) != mask) {
        return NativeRegisterCallsiteValueOrigin::Mixed;
      }
      return NativeRegisterCallsiteValueOrigin::LocalDefinition;
    }
    if ((bits.Entry & mask) == mask) {
      return NativeRegisterCallsiteValueOrigin::ForwardedEntry;
    }
    if ((bits.CallClobber & mask) == mask) {
      return NativeRegisterCallsiteValueOrigin::CallClobber;
    }
    return NativeRegisterCallsiteValueOrigin::Mixed;
  }

std::vector<NativeRegisterCallsiteSlotEvidence>
  Analyzer::callsiteEvidence(const State &state, llvm::CallBase &call,
                   llvm::ArrayRef<NativeRegisterCallInputSlot> slots) const {
    std::vector<NativeRegisterCallsiteSlotEvidence> result;
    result.reserve(slots.size());
    for (auto [index, slot] : llvm::enumerate(slots)) {
      NativeRegisterCallsiteSlotEvidence evidence;
      evidence.Index = index;
      evidence.Kind = slot.Kind;
      evidence.UnitName = slot.UnitName;
      evidence.StackSpace = slot.StackSpace;
      evidence.StackOffset = slot.StackOffset;
      evidence.StackSize = slot.StackSize;
      evidence.StackAlign = slot.StackAlign;
      evidence.OffsetBits = slot.OffsetBits;
      evidence.SizeBits = slot.SizeBits;
      evidence.Float = slot.Float;
      StackSlotKey resolvedKey;
      evidence.Origin =
          callsiteOrigin(state, call, slot, &resolvedKey);
      if (evidence.Origin ==
              NativeRegisterCallsiteValueOrigin::LocalDefinition &&
          slot.Kind != NativeRegisterCallInputSlotKind::Stack) {
        auto unitIt = UnitsByName.find(slot.UnitName);
        evidence.ConsumedLocal =
            unitIt != UnitsByName.end() &&
            !registerValuePendingAtCall(*unitIt->second, call);
      }
      if (slot.Kind == NativeRegisterCallInputSlotKind::Stack &&
          evidence.Origin == NativeRegisterCallsiteValueOrigin::LocalDefinition) {
        auto storeIt = state.StackSlotStores.find(resolvedKey);
        if (storeIt != state.StackSlotStores.end()) {
          evidence.StoreSizeBytes = storeIt->second.SizeBytes;
          evidence.StoreIsFloat = storeIt->second.IsFloat;
        }
      }
      result.push_back(std::move(evidence));
    }
    return result;
  }

void Analyzer::recordExternalCallsite(llvm::Function &caller, llvm::CallBase &call,
                              const State &state) {
    NativeRegisterExternalCallsite evidence;
    evidence.Call = &call;
    evidence.CallerName = caller.getName().str();
    llvm::Function *callee = call.getCalledFunction();
    evidence.Indirect = callee == nullptr;
    evidence.CalleeName =
        callee == nullptr ? "<indirect>" : callee->getName().str();

    if (const NativeExternalCallShape *shape = knownVarArgExternalShape(call)) {
      evidence.Kind = NativeRegisterExternalCallsiteKind::KnownVarArg;
      evidence.FixedInputs = shape->Inputs;
      evidence.FixedArgs = shape->FixedArgs;
      evidence.MaxArgs = shape->MaxArgs;
      evidence.FixedInputsComplete = shape->FixedInputsComplete;
      evidence.Slots = callsiteEvidence(state, call, shape->VarArgInputs);
    } else {
      evidence.Kind = NativeRegisterExternalCallsiteKind::UnknownExternal;
      evidence.Slots =
          callsiteEvidence(state, call, Options.ExternalEvidenceSlots);
    }
    ExternalCallsites.push_back(std::move(evidence));
  }

void Analyzer::collectExternalCallsiteEvidence() {
    ExternalCallsites.clear();
    for (llvm::Function *function : Functions) {
      auto flowIt = StableFlows.find(function);
      if (flowIt == StableFlows.end()) {
        continue;
      }
      const FunctionFlow &flow = flowIt->second;
      for (llvm::BasicBlock &block : *function) {
        auto stateIt = flow.In.find(&block);
        if (stateIt == flow.In.end()) {
          continue;
        }
        State state = stateIt->second;
        if (!state.Reachable) {
          continue;
        }
        for (llvm::Instruction &inst : block) {
          if (!state.Reachable) {
            break;
          }
          if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
              call != nullptr && isAnalyzableCall(inst) &&
              (isUnknownExternalCall(*call) ||
               knownVarArgExternalShape(*call) != nullptr)) {
            recordExternalCallsite(*function, *call, state);
          }
          transferInstruction(inst, state);
        }
      }
    }
  }

State Analyzer::transferBlock(llvm::BasicBlock &block, State state) {
    if (!state.Reachable) {
      return state;
    }
    for (llvm::Instruction &inst : block) {
      if (!state.Reachable) {
        break;
      }
      transferInstruction(inst, state);
    }
    pruneBlockLocalValueFacts(block, state);
    return state;
  }

void Analyzer::pruneBlockLocalValueFacts(llvm::BasicBlock &block, State &state) const {
    if (state.ValueOrigins.empty()) {
      return;
    }
    for (llvm::Instruction &inst : block) {
      auto it = state.ValueOrigins.find(&inst);
      if (it != state.ValueOrigins.end() &&
          CrossBlockValues.count(&inst) == 0) {
        state.ValueOrigins.erase(it);
      }
    }
  }

void Analyzer::transferInstruction(llvm::Instruction &inst, State &state) {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
      RegisterAccess access = registerLoad(*load, Units);
      if (access.Unit != nullptr) {
        Cell before = cellIn(state, access.Unit->Global);
        bool keepHighUse = isKeepHighPartialLoadUse(*load, access.Unit->Global);
        // 整寄存器 load 只有在所有位都可能仍是入口值时，load 结果才是纯
        // 入口值（ValueOrigins 只按整寄存器追踪）。
        bool pureEntryValue =
            before.MayEntry.getBitWidth() != 0 &&
            before.MayEntry.isAllOnes() && before.MayNonEntry.isZero();
        if (!isIgnored(*access.Unit, Options) && !keepHighUse &&
            !pureEntryValue) {
          readRegister(state, access.Unit->Global,
                       fullRegisterMask(access.Unit->Global));
        }
        if (pureEntryValue) {
          if (access.Unit->Global != stackPointerGlobal()) {
            state.ValueOrigins[load] =
                ValueOrigin{access.Unit->Global, 0,
                            fullRegisterMask(access.Unit->Global)};
          }
        } else {
          state.ValueOrigins.erase(load);
        }
        return;
      }
      if (std::optional<StackSlotKey> slot =
              fixedStackSlot(load->getPointerOperand(), *load)) {
        auto saved = state.StackSlots.find(*slot);
        if (saved != state.StackSlots.end() &&
            stackAdjustmentSlots(*load->getFunction()).count(*slot) == 0) {
          state.ValueOrigins[load] =
              ValueOrigin{saved->second, 0, fullRegisterMask(saved->second)};
        } else {
          state.ValueOrigins.erase(load);
        }
      }
      return;
    }
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      RegisterAccess access = registerStore(*store, Units);
      if (access.Unit != nullptr) {
        if (isIgnored(*access.Unit, Options)) {
          state.ValueOrigins.erase(store);
          return;
        }
        if (isKeepHighPartialStoreValue(store->getValueOperand(),
                                        access.Unit->Global)) {
          writeRegister(state, access.Unit->Global,
                        fullRegisterMask(access.Unit->Global));
          setFullRegisterOrigin(state, access.Unit->Global,
                                RegisterOriginKind::Mixed);
          state.ValueOrigins.erase(store);
          return;
        }
        std::optional<ValueOrigin> origin =
            entryValueOrigin(store->getValueOperand(), state);
        if (origin && origin->Base == access.Unit->Global &&
            origin->Offset == 0) {
          // 入口值写回：只恢复该值实际覆盖的位（整寄存器值全宽，窄值只
          // 恢复窄位）。
          llvm::APInt restoreMask =
              origin->Mask.getBitWidth() == 0
                  ? fullRegisterMask(access.Unit->Global)
                  : origin->Mask;
          restoreRegister(state, access.Unit->Global, restoreMask);
          setFullRegisterOrigin(state, access.Unit->Global,
                                RegisterOriginKind::Entry);
        } else {
          markEntryValueRead(store->getValueOperand(), state);
          writeRegister(state, access.Unit->Global,
                        fullRegisterMask(access.Unit->Global));
          RegisterOriginKind storedKind =
              registerOriginKindForStoredValue(store->getValueOperand(),
                                               state);
          setFullRegisterOrigin(state, access.Unit->Global, storedKind);
          if (storedKind == RegisterOriginKind::Local) {
            recordRegisterFloatWrite(
                state, access.Unit->Global,
                llvm::APInt::getAllOnes(
                    registerBitWidth(access.Unit->Global)),
                hasFloatWriteMetadata(*store));
          }
        }
        state.ValueOrigins.erase(store);
        return;
      }
      if (std::optional<StackSlotKey> slot =
              fixedStackSlot(store->getPointerOperand(), *store)) {
        State::StackSlotStoreInfo storeInfo;
        if (llvm::Type *valueType = store->getValueOperand()->getType();
            valueType != nullptr && valueType->isSized() &&
            (valueType->isIntegerTy() || valueType->isFloatingPointTy())) {
          storeInfo.SizeBytes =
              static_cast<uint32_t>(valueType->getScalarSizeInBits() / 8);
          storeInfo.IsFloat = valueType->isFloatingPointTy();
        }
        std::optional<ValueOrigin> origin =
            entryValueOrigin(store->getValueOperand(), state);
        if (stackAdjustmentSlots(*store->getFunction()).count(*slot) != 0) {
          // 栈对齐 push：push 的值是 undef（LLVM 源码里就是 getUndefRegState），
          // 不是"保存入口值"，所以不计入口读；也不登记保存槽，load 侧自然不挂
          // 保存值来源。
          state.StackSlots.erase(*slot);
          state.StackSlotOrigins.erase(*slot);
        } else if (origin && origin->Offset == 0 &&
                   canTrackSavedEntryRegister(origin->Base)) {
          state.StackSlots[*slot] = origin->Base;
          state.StackSlotOrigins[*slot] =
              NativeRegisterCallsiteValueOrigin::ForwardedEntry;
        } else {
          markEntryValueRead(store->getValueOperand(), state);
          state.StackSlots.erase(*slot);
          state.StackSlotOrigins[*slot] =
              origin ? NativeRegisterCallsiteValueOrigin::ForwardedEntry
                     : NativeRegisterCallsiteValueOrigin::LocalDefinition;
        }
        state.StackSlotStores[*slot] = storeInfo;
      } else {
        markInstructionEntryValueReads(inst, state);
      }
      return;
    }
    if (isAnalyzableCall(inst)) {
      transferCall(llvm::cast<llvm::CallBase>(inst), state);
      return;
    }
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
      std::optional<NativeRegisterPartialReadInfo> partialRead =
          parseNativeRegisterPartialRead(*call);
      if (partialRead) {
        auto unitIt = Units.find(partialRead->Global);
        if (unitIt != Units.end() && !isIgnored(unitIt->second, Options) &&
            !partialReadFeedsOnlyDeadFrameSlots(*call)) {
          Cell before = cellIn(state, partialRead->Global);
          bool pureEntryValue =
              before.MayEntry.getBitWidth() != 0 &&
              before.MayEntry.isAllOnes() &&
              before.MayNonEntry.isZero();
          llvm::APInt readMask =
              registerRangeMask(partialRead->Global, partialRead->BitOffset,
                                partialRead->ReadWidth);
          // 无论是否纯入口值，都按读到的位记录入口读（被覆盖的位不会
          // 置位）；纯入口时才把值标记成入口派生，供后续 store 转发。
          readRegister(state, partialRead->Global, readMask);
          if (pureEntryValue) {
            state.ValueOrigins[&inst] =
                ValueOrigin{partialRead->Global, 0, readMask};
          } else {
            state.ValueOrigins.erase(&inst);
          }
        } else {
          state.ValueOrigins.erase(&inst);
        }
        return;
      }
      std::optional<NativeRegisterPartialWriteInfo> partial =
          parseNativeRegisterPartialWrite(*call);
      if (partial) {
        auto unitIt = Units.find(partial->Global);
        if (unitIt != Units.end() && !isIgnored(unitIt->second, Options)) {
          markEntryValueRead(partial->Value, state);
          RegisterOriginKind partialKind =
              registerOriginKindForStoredValue(partial->Value, state);
          bool floatWrite = hasFloatWriteMetadata(*call);
          llvm::APInt writeMask = registerRangeMask(
              partial->Global, partial->BitOffset, partial->WriteWidth);
          if (isX64Low32GprWrite(*partial, unitIt->second)) {
            // 写 EAX 会清零 RAX 高 32 位，整寄存器都变成非入口定义。
            writeRegister(state, partial->Global,
                          fullRegisterMask(partial->Global));
            setFullRegisterOrigin(state, partial->Global, partialKind);
            if (partialKind == RegisterOriginKind::Local) {
              recordRegisterFloatWrite(
                  state, partial->Global,
                  llvm::APInt::getAllOnes(
                      registerBitWidth(partial->Global)),
                  floatWrite);
            }
          } else {
            // 部分写只覆盖写到的位：未写位保留入口值，只有写到的位变成
            // 非入口定义。整寄存器粒度的保守 MayEntry 保留不再需要。
            writeRegister(state, partial->Global, writeMask);
            setRegisterOrigin(state, partial->Global, writeMask, partialKind);
            if (partialKind == RegisterOriginKind::Local) {
              recordRegisterFloatWrite(state, partial->Global, writeMask,
                                       floatWrite);
            }
          }
        }
        state.ValueOrigins.erase(&inst);
        return;
      }
    }
    markInstructionEntryValueReads(inst, state);
  }

} // namespace notdec::bin2llvm
