#include "RegisterEliminationInternal.h"

namespace notdec::bin2llvm {

llvm::Value *FunctionBuilder::entryArgument(const RegisterUnit &unit) const {
    return entryArgumentSlot(unit).first;
  }

llvm::Value *FunctionBuilder::entryRangeArgument(const RegisterRangeKey &range,
                                  llvm::Instruction *insertBefore) {
    const RegisterUnit *unit = unitForRange(range);
    if (unit == nullptr || insertBefore == nullptr) {
      return nullptr;
    }
    auto [arg, slot] = entryArgumentSlot(*unit);
    if (arg == nullptr || slot == nullptr || slot->SizeBits == 0 ||
        range.BitOffset < slot->OffsetBits ||
        range.BitOffset + range.BitWidth > slot->OffsetBits + slot->SizeBits) {
      return nullptr;
    }

    if (slot->Kind == NativeSignatureSlotKind::FloatRegister) {
      if (range.BitOffset != slot->OffsetBits ||
          range.BitWidth != slot->SizeBits ||
          !arg->getType()->isFloatingPointTy()) {
        return nullptr;
      }
      llvm::IRBuilder<> builder(insertBefore);
      llvm::Type *bitsType =
          llvm::IntegerType::get(Function.getContext(), range.BitWidth);
      return builder.CreateBitCast(arg, bitsType,
                                   unit->Name + ".range_entry_arg");
    }

    if (!arg->getType()->isIntegerTy()) {
      return nullptr;
    }
    return extractRangeValue(range, arg, slot->OffsetBits, insertBefore,
                             unit->Name + ".range_entry_arg");
  }

llvm::MDNode *FunctionBuilder::entryRangeNode(const RegisterRangeKey &range) const {
    const RegisterUnit *unit = unitForRange(range);
    llvm::Metadata *fields[] = {
        llvm::MDString::get(
            Function.getContext(),
            "name=" + (unit == nullptr ? std::string("") : unit->Name)),
        llvm::MDString::get(Function.getContext(),
                            "bit_offset=" + std::to_string(range.BitOffset)),
        llvm::MDString::get(Function.getContext(),
                            "bit_width=" + std::to_string(range.BitWidth)),
    };
    return llvm::MDNode::get(Function.getContext(), fields);
  }

llvm::LoadInst *FunctionBuilder::entryRegisterLoad(const RegisterUnit &unit,
                                    llvm::Instruction &insertBefore) {
    if (auto cached = EntryRegisterLoads.find(unit.Global);
        cached != EntryRegisterLoads.end()) {
      return cached->second;
    }

    llvm::IRBuilder<> builder(&insertBefore);
    llvm::LoadInst *load = builder.CreateLoad(
        unit.Global->getValueType(), unit.Global, unit.Name + ".entry");
    load->setMetadata("notdec.register.access",
                      registerAccessNode(Function.getContext(), unit));
    load->setMetadata("notdec.register.summary_ssa.entry", markerNode("true"));
    EntryRegisterLoads.emplace(unit.Global, load);
    return load;
  }

llvm::Value *FunctionBuilder::entryPartialRead(const RegisterUnit &unit,
                                const RegisterRangeKey &range,
                                llvm::Instruction &insertBefore) {
    unsigned fullWidth = registerBitWidth(unit);
    if (fullWidth == 0 || range.BitOffset + range.BitWidth > fullWidth) {
      return nullptr;
    }

    llvm::IRBuilder<> builder(&insertBefore);
    llvm::Type *ptrType = unit.Global->getType();
    llvm::Type *resultType = rangeType(range);
    llvm::Function *callee = getOrInsertNativeRegisterPartialRead(
        *Function.getParent(), ptrType, resultType, fullWidth, range.BitWidth);
    llvm::Value *offset = llvm::ConstantInt::get(
        llvm::Type::getInt64Ty(Function.getContext()), range.BitOffset);
    llvm::CallInst *call = builder.CreateCall(callee, {unit.Global, offset},
                                              unit.Name + ".range_entry");
    call->setMetadata("notdec.register.access",
                      registerAccessNode(Function.getContext(), unit));
    call->setMetadata("notdec.register.summary_ssa.range_entry",
                      entryRangeNode(range));
    return call;
  }

bool FunctionBuilder::shapeParamCoversRange(const RegisterRangeKey &range) const {
    auto shapeIt = SignatureState.Shapes.find(&Function);
    if (shapeIt == SignatureState.Shapes.end()) {
      return false;
    }
    for (const NativeSignatureSlot &slot : shapeIt->second.Params) {
      if (slot.Unit == nullptr || slot.Unit->Global != range.Global) {
        continue;
      }
      if (range.BitOffset >= slot.OffsetBits &&
          range.BitOffset + range.BitWidth <= slot.OffsetBits + slot.SizeBits) {
        return true;
      }
    }
    return false;
  }

bool FunctionBuilder::rangeMayComeFromEntry(const RegisterRangeKey &range) const {
    if (shapeParamCoversRange(range)) {
      return true;
    }
    const RegisterUnit *unit = unitForRange(range);
    if (unit == nullptr) {
      return false;
    }
    auto fnIt = SummaryFacts.find(&Function);
    if (fnIt == SummaryFacts.end()) {
      return true;
    }
    auto regIt = fnIt->second.Registers.find(unit->Name);
    if (regIt == fnIt->second.Registers.end()) {
      return true;
    }
    // 按 bit 判断：range 覆盖的位里，只要还有位可能仍是入口值，该 range
    // 就可能来自入口。被本函数覆盖过的位（MayEntry=0）不再误当入口。
    const llvm::APInt &mayEntry = regIt->second.MayEntryMask;
    if (mayEntry.getBitWidth() == 0) {
      return regIt->second.MayEntry;
    }
    if (range.BitOffset >= mayEntry.getBitWidth()) {
      return false;
    }
    uint64_t bits = std::min<uint64_t>(
        range.BitWidth, mayEntry.getBitWidth() - range.BitOffset);
    return bits != 0 &&
           !mayEntry.extractBits(bits, range.BitOffset).isZero();
  }

llvm::Value *FunctionBuilder::entryRangeInput(llvm::BasicBlock &block,
                               const RegisterRangeKey &range) {
    if (auto cached = EntryRangeInputs.find(range);
        cached != EntryRangeInputs.end()) {
      return resolve(cached->second);
    }
    const RegisterUnit *unit = unitForRange(range);
    if (unit == nullptr) {
      return nullptr;
    }
    if (isAbiBoundaryZeroRegister(unit->Name)) {
      // DF 在函数入口按 ABI 就是 0（见 isAbiBoundaryZeroRegister 注释）：走到
      // 入口解析说明这条路径上没有本函数写过的 DF 定义（cld/std 会写），按 ABI
      // 直接给常量 0，不要退化成 unknown。
      llvm::Value *zero = llvm::ConstantInt::get(rangeType(range), 0);
      EntryRangeInputs.emplace(range, zero);
      EntryInputRanges.emplace(zero, range);
      ++Summary.RangeEntryInputs;
      return zero;
    }
    if (PostSignatureCleanup) {
      llvm::Instruction *insertBefore =
          &*Function.getEntryBlock().getFirstInsertionPt();
      if (llvm::Value *value = entryRangeArgument(range, insertBefore)) {
        EntryRangeInputs.emplace(range, value);
        EntryInputRanges.emplace(value, range);
        return value;
      }
    }
    if (isX87StackRegisterName(unit->Name) && isLikelyX86_64SysVAbi(Abi)) {
      // x86-64 的入口 x87 栈为空（见 isX87StackRegisterName 注释）：这里是
      // 窗口模型对"空槽"的搬移读，值不属于源程序语义，给 poison，不要
      // 退化成 notdec.unknown。
      llvm::Value *empty = llvm::PoisonValue::get(rangeType(range));
      EntryRangeInputs.emplace(range, empty);
      EntryInputRanges.emplace(empty, range);
      ++Summary.RangeEntryInputs;
      return empty;
    }
    if (!rangeMayComeFromEntry(range)) {
      // 闸门拒绝：summary 说这些位不可能还来自入口（例如编译器为对齐压栈的
      // 那个值，汇总侧特意没把它记成入口读）。拒绝只表示"不向调用点要这几位、
      // 不建形参"，值本身仍然要给出——退回裸读（partial_read/load），保留
      // 寄存器/位/位置，比 notdec.unknown 好（寄存器值表示准则：载体 > 裸读 >
      // unknown）。所以这里只记数，不返回 nullptr。
      ++Summary.EntryRangeRejectedReads;
    }
    llvm::Instruction *insertBefore =
        &*Function.getEntryBlock().getFirstInsertionPt();
    if (insertBefore == nullptr) {
      RangeLossCause cause;
      cause.Reason = "entry_value_unavailable";
      RangeLossCauses[{&block, range}] = cause;
      return nullptr;
    }
    llvm::Value *value = nullptr;
    unsigned fullWidth = registerBitWidth(*unit);
    if (range.BitOffset == 0 && range.BitWidth == fullWidth) {
      value = entryRegisterLoad(*unit, *insertBefore);
    } else {
      value = entryPartialRead(*unit, range, *insertBefore);
    }
    if (value == nullptr) {
      RangeLossCause cause;
      cause.Reason = "entry_value_unavailable";
      RangeLossCauses[{&block, range}] = cause;
      return nullptr;
    }
    EntryRangeInputs.emplace(range, value);
    EntryInputRanges.emplace(value, range);
    ++Summary.RangeEntryInputs;
    return value;
  }

RangeLossCause FunctionBuilder::unknownCallEffectCause(const llvm::CallBase &call) const {
    RangeLossCause cause;
    llvm::Function *callee = call.getCalledFunction();
    cause.Callee =
        callee != nullptr ? callee->getName().str() : std::string("<indirect>");
    cause.Reason = callee != nullptr && !callee->isDeclaration() &&
                           SummaryFacts.find(callee) == SummaryFacts.end()
                       ? "callee_summary_missing"
                       : "call_effect_mixed";
    return cause;
  }

CallRegisterEffect FunctionBuilder::callEffect(const llvm::CallBase &call,
                                const RegisterUnit &unit) const {
    llvm::Function *callee = call.getCalledFunction();
    if (PostSignatureCleanup &&
        SignatureState.RewrittenCalls.count(&call) != 0) {
      if (signatureReturnUsesUnit(const_cast<llvm::CallBase &>(call), unit)) {
        return CallRegisterEffect::ReturnValue;
      }
      if (callPreservesRegisterByAbi(unit)) {
        return CallRegisterEffect::Preserve;
      }
      return CallRegisterEffect::Clobber;
    }
    // ABI unaffected（callee-saved）寄存器由 ABI 保证原样保留，优先于 callee 的
    // summary 事实：callee 的 may_non_entry 只说明它内部改过（例如把 RBP 当帧
    // 指针用、再从事先存好的槽里恢复），不能当成调用后的新值，否则调用方会为
    // 一次普通调用凭空造出 RBP/RBX 的 unknown（memcached FUN_16bb0 就是这种）。
    if (callPreservesRegisterByAbi(unit)) {
      return CallRegisterEffect::Preserve;
    }
    // DF 在调用后按 ABI 是常量 0（见 isAbiBoundaryZeroRegister 注释）：走 Clobber
    // 路径，让 callRangeValue 直接给 0；不能因为 callee 内部写过 DF 就退化成
    // Unknown 占位。
    if (isAbiBoundaryZeroRegister(unit.Name)) {
      return CallRegisterEffect::Clobber;
    }
    if (callee != nullptr && !callee->isDeclaration()) {
      auto fnIt = SummaryFacts.find(callee);
      if (fnIt == SummaryFacts.end()) {
        return CallRegisterEffect::Unknown;
      }
      auto regIt = fnIt->second.Registers.find(unit.Name);
      SummaryRegisterFact fact;
      if (regIt != fnIt->second.Registers.end()) {
        fact = regIt->second;
      }
      if (!fact.MayNonEntry) {
        return CallRegisterEffect::Preserve;
      }
      // 第一遍不再区分 return：ExitDemand 是"函数确实返回这个寄存器"的
      // summary 事实，但调用点只需要知道"被改动"，该寄存器是否返回由
      // 签名（shape.Returns）在重写后按需解析。见
      // logs/20260805-12-native-unify-call-return-plan.md。
      if (!fact.MayEntry) {
        return CallRegisterEffect::Clobber;
      }
      return CallRegisterEffect::Unknown;
    }

    if (callPreservesRegisterByAbi(unit)) {
      return CallRegisterEffect::Preserve;
    }
    return CallRegisterEffect::Clobber;
  }

bool FunctionBuilder::callReadsRegister(const llvm::CallBase &call,
                         const RegisterUnit &unit) const {
    llvm::Function *callee = call.getCalledFunction();
    if (PostSignatureCleanup) {
      return false;
    }
    if (callee != nullptr && !callee->isDeclaration()) {
      auto fnIt = SummaryFacts.find(callee);
      if (fnIt == SummaryFacts.end()) {
        return Abi.Inputs.count(unit.Name) != 0;
      }
      auto regIt = fnIt->second.Registers.find(unit.Name);
      return regIt != fnIt->second.Registers.end() && regIt->second.ReadEntry;
    }
    if (callee == nullptr) {
      return Abi.Inputs.count(unit.Name) != 0;
    }
    const SignatureShape *shape = shapeForCall(SignatureState, call);
    if (shape == nullptr) {
      return false;
    }
    if (const NativeExternalCallShape *callsiteShape =
            externalCallsiteShapeForCall(SignatureState, call)) {
      return llvm::any_of(
          callsiteShape->Inputs, [&](const NativeRegisterCallInputSlot &input) {
            return input.Kind == NativeRegisterCallInputSlotKind::Register &&
                   input.UnitName == unit.Name;
          });
    }
    for (const NativeSignatureSlot &slot : shape->Params) {
      if (slot.Unit != nullptr && slot.Unit->Name == unit.Name) {
        return true;
      }
    }
    return false;
  }

void FunctionBuilder::collectSignatureCallArgs(bool stackSlotsOnly) {
    // Build fresh address facts in both collection phases rather than carrying
    // raw Instruction* through register and phi rewriting.
    StackAddresses = std::make_unique<NativeStackAddressAnalysis>(
        Function, stackPointerGlobal(), Abi.StackPointer);
    std::vector<llvm::CallBase *> calls;
    for (llvm::Instruction &inst : llvm::instructions(Function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr || !isAnalyzableCall(*call)) {
        continue;
      }
      if (shapeForCall(SignatureState, *call) == nullptr) {
        continue;
      }
      calls.push_back(call);
    }

    for (llvm::CallBase *call : calls) {
      if (call->getParent() == nullptr) {
        continue;
      }
      llvm::Function *callee = call->getCalledFunction();
      const SignatureShape *shape = shapeForCall(SignatureState, *call);
      if (shape == nullptr) {
        continue;
      }
      std::vector<CallArgStoreBinding> bindings =
          callArgStoreBindings(*call, *shape, stackSlotsOnly);
      if (bindings.empty() && shape->Params.empty() && shape->Returns.empty()) {
        continue;
      }

      std::vector<CallArgStoreBinding> &recorded =
          SignatureState.CallArgs[call];
      if (!stackSlotsOnly) {
        recorded = bindings;
      } else {
        for (CallArgStoreBinding &binding : bindings) {
          auto existing = std::find_if(
              recorded.begin(), recorded.end(), [&](const auto &candidate) {
                return candidate.Index == binding.Index;
              });
          if (existing == recorded.end()) {
            recorded.push_back(binding);
          } else {
            *existing = binding;
          }
        }
      }
      std::vector<llvm::Value *> *localRecorded = nullptr;
      if (llvm::Function *caller = call->getFunction()) {
        localRecorded = &SignatureState.CallArgRecordedValuesByFunction
                             [caller->getName().str()];
      }
      // 逐槽记录"调用前那一刻"的 range 值（见 SignatureRewriteState::
      // CallArgRangeValues）。绑定收集遇到第一个读不出值的槽就停，这里不因
      // 前一个槽失败而放弃后面的槽；已由绑定覆盖的槽不再重复读（readSlotValueBefore
      // 每次都要重建 DominatorTree，成本不能翻倍）。栈槽沿用绑定收集的两遍结构：
      // 栈地址要等循环携带的 RSP/RBP phi 定型，所以只在第二遍记录。
      {
        std::map<unsigned, llvm::Value *> &rangeValues =
            SignatureState.CallArgRangeValues[call];
        for (auto [index, slot] :
             llvm::enumerate(callParamSlots(*call, *shape))) {
          bool isStackSlot =
              slot.Kind == NativeSignatureSlotKind::IntegerStack ||
              slot.Kind == NativeSignatureSlotKind::FloatStack;
          if (isStackSlot != stackSlotsOnly) {
            continue;
          }
          if (bindingForIndex(bindings, index) != nullptr) {
            continue;
          }
          if (slot.Unit == nullptr && slot.LlvmType == nullptr) {
            continue;
          }
          llvm::Value *value = resolve(readSlotValueBefore(*call, slot));
          if (value == nullptr || value->getType() != slotType(slot)) {
            // 新准则（Arch2.md 第 13 节）：range 状态答不上来时退到函数入口值
            // （裸读），不要直接落 unknown。裸读保留了"哪个寄存器、哪几位"，
            // 后续还能提成形参/实参；入口闸门仍生效（填充/junk 位不在此列）。
            value =
                resolve(readSlotValueBefore(*call, slot, /*entryFallback=*/true));
          }
          if (value == nullptr || value->getType() != slotType(slot)) {
            continue;
          }
          rangeValues[index] = value;
          SignatureState.CallArgRecordedValues.insert(value);
          if (localRecorded != nullptr) {
            localRecorded->push_back(value);
          }
        }
      }
      for (const CallArgStoreBinding &binding : bindings) {
        if (binding.Store != nullptr) {
          SignatureState.CallArgRecordedStores.insert(binding.Store);
        }
        if (binding.Value != nullptr) {
          SignatureState.CallArgRecordedValues.insert(binding.Value);
          if (localRecorded != nullptr) {
            localRecorded->push_back(binding.Value);
          }
        }
        if (binding.RegisterValue != nullptr) {
          SignatureState.CallArgRecordedValues.insert(binding.RegisterValue);
          if (localRecorded != nullptr) {
            localRecorded->push_back(binding.RegisterValue);
          }
        }
      }
      if (callee == nullptr ||
          !isUnknownExternalFunction(
              *callee, externalPrototypesForState(SignatureState))) {
        for (const CallArgStoreBinding &binding : bindings) {
          if (binding.Store != nullptr &&
              SignatureState.StoresToErase.insert(binding.Store).second) {
            ++Summary.CallArgStoresMarked;
          }
        }
      }
    }
  }

const RegisterUnit *FunctionBuilder::unitByName(llvm::StringRef name) const {
    for (const auto &[global, unit] : Units) {
      if (unit.Name == name) {
        return &unit;
      }
    }
    return nullptr;
  }

std::optional<NativeSignatureSlot>
  FunctionBuilder::signatureSlotForCallInput(const NativeRegisterCallInputSlot &input) const {
    return makeSignatureSlotForCallInput(input, Units,
                                         Function.getContext());
  }

void FunctionBuilder::appendAbiVarArgSlots(std::vector<NativeSignatureSlot> &slots) const {
    auto covered = [&](const RegisterUnit &unit) {
      return std::any_of(
          slots.begin(), slots.end(), [&](const NativeSignatureSlot &slot) {
            return slot.Unit != nullptr && slot.Unit->Global == unit.Global;
          });
    };
    for (const AbiFacts::RegisterSlot &regSlot : Abi.IntegerInputsInOrder) {
      const RegisterUnit *unit =
          notdec::bin2llvm::unitByName(Units, regSlot.UnitName);
      if (unit == nullptr || covered(*unit)) {
        continue;
      }
      slots.push_back(integerSignatureSlot(*unit));
    }
    for (const AbiFacts::RegisterSlot &regSlot : Abi.FloatInputsInOrder) {
      const RegisterUnit *unit =
          notdec::bin2llvm::unitByName(Units, regSlot.UnitName);
      if (unit == nullptr || covered(*unit)) {
        continue;
      }
      unsigned bits = unit->Global->getValueType()->getScalarSizeInBits();
      std::optional<NativeSignatureSlot> slot = floatSlotForDemand(
          Function.getContext(), regSlot, Units,
          bits == 0 ? llvm::APInt() : llvm::APInt::getAllOnes(bits));
      if (slot.has_value()) {
        slots.push_back(*slot);
      }
    }
  }

void FunctionBuilder::appendAbiStackVarArgSlots(llvm::CallBase &call,
                                 std::vector<NativeSignatureSlot> &slots) {
    if (Abi.StackInputsInOrder.empty()) {
      return;
    }
    const AbiFacts::StackSlot &abiSlot = Abi.StackInputsInOrder.front();
    uint64_t step = stackGridStep(abiSlot.Align);
    if (step == 0) {
      return;
    }
    uint64_t nextOffset = abiSlot.Offset;
    for (const NativeSignatureSlot &slot : slots) {
      if (!isStackSignatureSlot(slot) || slot.StackSpace != abiSlot.Space) {
        continue;
      }
      nextOffset = std::max(nextOffset, slot.StackOffset + slot.StackSize);
    }
    if (nextOffset < abiSlot.Offset) {
      return;
    }
    uint64_t relative = nextOffset - abiSlot.Offset;
    if (relative % step != 0) {
      relative += step - relative % step;
    }
    auto *i64Type = llvm::Type::getInt64Ty(call.getContext());
    for (uint64_t index = relative / step;; ++index) {
      std::optional<NativeSignatureSlot> probe = signatureStackSlotFromAbi(
          abiSlot, index, i64Type, NativeSignatureSlotKind::IntegerStack);
      if (!probe.has_value()) {
        return;
      }
      llvm::StoreInst *store = findNearestStackStoreBeforeCall(call, *probe);
      if (store == nullptr) {
        return;
      }
      llvm::Type *valueType = store->getValueOperand()->getType();
      bool isFloat = valueType->isFloatingPointTy();
      std::optional<NativeSignatureSlot> slot = signatureStackSlotFromAbi(
          abiSlot, index, isFloat ? valueType : static_cast<llvm::Type *>(i64Type),
          isFloat ? NativeSignatureSlotKind::FloatStack
                  : NativeSignatureSlotKind::IntegerStack);
      if (!slot.has_value()) {
        return;
      }
      slots.push_back(*slot);
    }
  }

std::vector<NativeSignatureSlot> FunctionBuilder::callParamSlots(llvm::CallBase &call,
                                                  const SignatureShape &shape) {
    std::vector<NativeSignatureSlot> slots = shape.Params;
    if (!shape.VarArg) {
      return slots;
    }
    const NativeExternalCallShape *callsiteShape =
        externalCallsiteShapeForCall(SignatureState, call);
    if (callsiteShape == nullptr ||
        callsiteShape->Inputs.size() <= shape.Params.size()) {
      llvm::Function *callee = call.getCalledFunction();
      if (callee != nullptr && !callee->isDeclaration() &&
          isNativeVarArgFunction(*callee)) {
        appendAbiVarArgSlots(slots);
        appendAbiStackVarArgSlots(call, slots);
      }
      return slots;
    }
    for (unsigned index = shape.Params.size();
         index < callsiteShape->Inputs.size(); ++index) {
      const NativeRegisterCallInputSlot &input = callsiteShape->Inputs[index];
      std::optional<NativeSignatureSlot> slot =
          signatureSlotForCallInput(input);
      if (!slot) {
        break;
      }
      slots.push_back(*slot);
    }
    return slots;
  }

std::vector<CallArgStoreBinding>
  FunctionBuilder::callArgStoreBindings(llvm::CallBase &call, const SignatureShape &shape,
                       bool stackSlotsOnly) {
    std::vector<CallArgStoreBinding> bindings;
    std::vector<NativeSignatureSlot> slots = callParamSlots(call, shape);
    // This collection still runs against the initial signature.  Missing or
    // clobber-derived values are evidence used to trim an internal stack
    // parameter prefix, so reporting them here leaves stale warnings after
    // refineInternalStackParamShapes().  rewriteSignatureShapes() diagnoses
    // only the slots retained by the final shape.
    for (auto [index, slot] : llvm::enumerate(slots)) {
      const RegisterUnit *unit = slot.Unit;
      bool isStackSlot = slot.Kind == NativeSignatureSlotKind::IntegerStack ||
                         slot.Kind == NativeSignatureSlotKind::FloatStack;
      if (isStackSlot != stackSlotsOnly) {
        continue;
      }
      llvm::Value *value = resolve(readSlotValueBefore(call, slot));
      if (value == nullptr) {
        break;
      }
      if (value->getType() != slotType(slot)) {
        break;
      }
      llvm::StoreInst *store = isStackSlot
                                   ? findNearestStackStoreBeforeCall(call, slot)
                                   : findNearestStoreBeforeCall(call, *unit);
      bindings.push_back(CallArgStoreBinding{store, unit, value, value,
                                             slotType(slot),
                                             static_cast<unsigned>(index)});
    }
    return bindings;
  }

llvm::Value *FunctionBuilder::readSlotValueBefore(llvm::CallBase &call,
                                   const NativeSignatureSlot &slot,
                                   bool entryFallback ) {
    return readSlotValueBefore(
        static_cast<llvm::Instruction &>(call), slot,
        slot.Unit == nullptr ? llvm::Twine("stack.arg")
                             : llvm::Twine(slot.Unit->Name) + ".arg_range",
        /*allowUnknownSegments=*/false, entryFallback);
  }

llvm::Value *FunctionBuilder::readSlotValueBefore(llvm::Instruction &before,
                                   const NativeSignatureSlot &slot,
                                   llvm::Twine rangeName,
                                   bool allowUnknownSegments ,
                                   bool entryFallback ) {
    if (slot.Kind == NativeSignatureSlotKind::IntegerStack ||
        slot.Kind == NativeSignatureSlotKind::FloatStack) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&before);
      return call == nullptr ? nullptr : readStackSlotValueBefore(*call, slot);
    }
    if (slot.Unit == nullptr || before.getParent() == nullptr) {
      return nullptr;
    }
    llvm::Value *rangeValue = readSlotRangeBefore(
        before, slot, rangeName, allowUnknownSegments, entryFallback);
    if (rangeValue != nullptr && rangeValue->getType() == slotType(slot)) {
      return rangeValue;
    }
    return nullptr;
  }

llvm::Value *FunctionBuilder::composePlannedRangeRead(llvm::GlobalVariable *global,
                                       uint64_t readOffset,
                                       uint32_t readWidth,
                                       llvm::Instruction *before,
                                       llvm::Twine name,
                                       llvm::DominatorTree &domTree,
                                       bool entryFallback ) {
    if (global == nullptr || before == nullptr || before->getParent() == nullptr ||
        readWidth == 0) {
      return nullptr;
    }
    auto plannedIt = PlannedRanges.find(global);
    if (plannedIt == PlannedRanges.end()) {
      return nullptr;
    }
    uint64_t readEnd = readOffset + readWidth;
    auto *resultType =
        llvm::IntegerType::get(Function.getContext(), readWidth);
    llvm::Value *result = llvm::ConstantInt::get(resultType, 0);
    uint64_t cursor = readOffset;
    for (const RegisterRangeKey &range : plannedIt->second) {
      uint64_t rangeEnd = range.BitOffset + range.BitWidth;
      if (rangeEnd <= cursor) {
        continue;
      }
      if (range.BitOffset > cursor) {
        // 分区在请求范围内出现空洞，无法完整覆盖。
        return nullptr;
      }
      uint64_t pieceEnd = std::min(rangeEnd, readEnd);
      if (pieceEnd <= cursor) {
        continue;
      }
      llvm::Value *full = resolve(
          readRangeBefore(*before->getParent(), range, before, domTree));
      if (entryFallback &&
          (full == nullptr || full->getType() != rangeType(range) ||
           !valueDominatesUse(full, before, domTree))) {
        full = resolve(entryRangeInput(*before->getParent(), range));
      }
      if (full == nullptr || full->getType() != rangeType(range) ||
          !valueDominatesUse(full, before, domTree)) {
        return nullptr;
      }
      auto pieceWidth = static_cast<unsigned>(pieceEnd - cursor);
      llvm::Value *piece = extractBitsFromIntegerValue(
          full, pieceWidth, cursor - range.BitOffset, before, name);
      if (piece == nullptr) {
        return nullptr;
      }
      if (cursor == readOffset && pieceEnd == readEnd) {
        return piece;
      }
      result = insertBitsIntoInteger(result, piece, pieceWidth,
                                     cursor - readOffset, before, name);
      if (result == nullptr) {
        return nullptr;
      }
      cursor = pieceEnd;
      if (cursor == readEnd) {
        return result;
      }
    }
    return nullptr;
  }

llvm::Value *FunctionBuilder::readSlotRangeBefore(llvm::Instruction &before,
                                   const NativeSignatureSlot &slot,
                                   llvm::Twine name,
                                   bool allowUnknownSegments ,
                                   bool entryFallback ) {
    if (slot.Unit == nullptr || before.getParent() == nullptr ||
        slot.SizeBits == 0) {
      return nullptr;
    }
    std::vector<RegisterRangeKey> ranges = plannedRangesCovering(
        slot.Unit->Global, slot.OffsetBits, slot.SizeBits);
    llvm::DominatorTree domTree(Function);
    llvm::Value *bits = nullptr;
    if (ranges.empty()) {
      // 槽位比唯一的分段窄（例如 32 位 ESI 读一个 64 位 RSI 存）时只找包含
      // 段是不够的：分区可能把槽位切成多段（RDX = [0,16)+[16,64) 而槽位是
      // i32 [0,32)）。按重叠分段拼接。
      bits = composePlannedRangeRead(slot.Unit->Global, slot.OffsetBits,
                                     slot.SizeBits, &before, name, domTree,
                                     entryFallback);
      bits = resolve(bits);
    } else {
      bits = assembleRangeReadIfDominating(
          ranges, slot.OffsetBits, slot.SizeBits, &before, name, domTree,
          allowUnknownSegments, entryFallback);
      bits = resolve(bits);
    }
    if (bits == nullptr) {
      return nullptr;
    }
    if (!valueDominatesUse(bits, &before, domTree)) {
      return nullptr;
    }

    llvm::Instruction *castBefore = &before;
    if (auto *bitsInst = llvm::dyn_cast<llvm::Instruction>(bits);
        bitsInst != nullptr && bitsInst->getParent() == before.getParent() &&
        bitsInst->comesBefore(&before)) {
      if (llvm::Instruction *next = bitsInst->getNextNode()) {
        castBefore = next;
      }
    }

    llvm::Type *targetType = slotType(slot);
    if (slot.Kind == NativeSignatureSlotKind::IntegerRegister) {
      if (!targetType->isIntegerTy() || !bits->getType()->isIntegerTy()) {
        return nullptr;
      }
      llvm::IRBuilder<> builder(castBefore);
      return builder.CreateZExtOrTrunc(bits, targetType,
                                       slot.Unit->Name + ".arg_range_cast");
    }

    if (!targetType->isFloatingPointTy() || !bits->getType()->isIntegerTy() ||
        bits->getType()->getIntegerBitWidth() !=
            targetType->getScalarSizeInBits()) {
      return nullptr;
    }
    llvm::IRBuilder<> builder(castBefore);
    return builder.CreateBitCast(bits, targetType,
                                 slot.Unit->Name + ".arg_range_float");
  }

llvm::Value *FunctionBuilder::readStackSlotValueBefore(llvm::CallBase &call,
                                        const NativeSignatureSlot &slot) {
    std::vector<RelativeStoreGroup> relativeStores;
    llvm::StoreInst *store =
        scanStackStoresBeforeCall(call, slot, relativeStores);
    if (store == nullptr) {
      return readCrossBlockStackSlotBeforeCall(call, slot, relativeStores);
    }
    llvm::Value *value = resolve(store->getValueOperand());
    llvm::Type *targetType = slotType(slot);
    if (value == nullptr || targetType == nullptr) {
      return nullptr;
    }
    if (value->getType() == targetType) {
      return value;
    }
    llvm::IRBuilder<> builder(&call);
    if (value->getType()->isIntegerTy() && targetType->isIntegerTy()) {
      return builder.CreateZExtOrTrunc(value, targetType, "stack.arg.cast");
    }
    bool integerFloatPair =
        (value->getType()->isIntegerTy() &&
         targetType->isFloatingPointTy()) ||
        (value->getType()->isFloatingPointTy() && targetType->isIntegerTy());
    if (integerFloatPair && value->getType()->getScalarSizeInBits() ==
                                targetType->getScalarSizeInBits()) {
      // Lifted x87 stack stores carry the raw 80 bits as i80.  The trusted
      // external declaration consumes the same bits as LLVM x86_fp80.
      return builder.CreateBitCast(value, targetType, "stack.arg.float_cast");
    }
    return nullptr;
  }

uint64_t FunctionBuilder::stackGridStep(uint32_t alignBytes) const {
    const llvm::DataLayout &layout = Function.getParent()->getDataLayout();
    uint64_t gridBytes = layout.getPointerSize();
    if (gridBytes == 0) {
      return 0;
    }
    return stackSlotStep(static_cast<uint32_t>(gridBytes), alignBytes);
  }

std::optional<uint64_t>
  FunctionBuilder::stackSlotIndex(const NativeSignatureSlot &slot) const {
    for (const AbiFacts::StackSlot &abiSlot : Abi.StackInputsInOrder) {
      if (slot.StackSpace != abiSlot.Space ||
          slot.StackOffset < abiSlot.Offset) {
        continue;
      }
      uint64_t relative = slot.StackOffset - abiSlot.Offset;
      if (abiSlot.Align != 0 && relative % abiSlot.Align != 0) {
        continue;
      }
      uint64_t step = stackGridStep(abiSlot.Align);
      if (step == 0 || relative % step != 0) {
        continue;
      }
      return relative / step;
    }
    return std::nullopt;
  }

NativeStackAddressAnalysis &FunctionBuilder::stackAddressAnalysis() {
    if (!StackAddresses) {
      StackAddresses = std::make_unique<NativeStackAddressAnalysis>(
          Function, stackPointerGlobal(), Abi.StackPointer);
    }
    return *StackAddresses;
  }

std::optional<NativeStackAddress>
  FunctionBuilder::callsiteStackAddress(llvm::CallBase &call, const NativeSignatureSlot &slot) {
    const auto *callInst = llvm::dyn_cast<llvm::CallInst>(&call);
    const bool isTailCall = callInst != nullptr && callInst->isTailCall();
    if (slot.StackOffset >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        (!isTailCall && Abi.StackShift >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))) {
      return std::nullopt;
    }
    int64_t slotOffset = static_cast<int64_t>(slot.StackOffset);
    if (isTailCall) {
      // Tail-call stack arguments remain caller-owned entry-frame slots.  Do
      // not let a relative/aligned SP value in the epilogue change that base.
      return NativeStackAddress{NativeStackAddressKind::EntryStackPointer,
                                nullptr, slotOffset};
    }
    std::optional<NativeStackAddress> current =
        stackAddressAnalysis().stackPointerBefore(call);
    if (!current) {
      return std::nullopt;
    }
    int64_t stackShift = static_cast<int64_t>(Abi.StackShift);
    int64_t delta = slotOffset >= stackShift ? slotOffset - stackShift
                                             : -(stackShift - slotOffset);
    return nativeStackAddressWithOffset(*current, delta);
  }

llvm::StoreInst *
  FunctionBuilder::scanStackStoresBeforeCall(llvm::CallBase &call,
                            const NativeSignatureSlot &slot,
                            std::vector<RelativeStoreGroup> &relativeStores) {
    if (call.getParent() == nullptr) {
      return nullptr;
    }
    std::optional<NativeStackAddress> expected =
        callsiteStackAddress(call, slot);
    std::optional<uint64_t> stackIndex = stackSlotIndex(slot);
    uint64_t stackStep =
        stackIndex ? stackGridStep(slot.StackAlign) : 0;
    auto recordRelativeStore = [&](const NativeStackAddress &address,
                                   llvm::StoreInst *store) {
      for (RelativeStoreGroup &group : relativeStores) {
        if (group.Kind == address.Kind && group.Base == address.Base) {
          group.Stores.try_emplace(address.Offset, store);
          return;
        }
      }
      RelativeStoreGroup group;
      group.Kind = address.Kind;
      group.Base = address.Base;
      group.Stores.try_emplace(address.Offset, store);
      relativeStores.push_back(std::move(group));
    };
    for (auto it = call.getIterator(); it != call.getParent()->begin();) {
      --it;
      llvm::Instruction &inst = *it;
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        std::optional<NativeStackAddress> address =
            stackAddressAnalysis().addressForPointer(
                store->getPointerOperand());
        if (address) {
          if (expected && *address == *expected) {
            return store;
          }
          // A native alloca or aligned SP has no entry-SP meaning, but it still
          // preserves the ordering of outgoing slots. Group it by its concrete
          // base and use the cspec slot index only for this caller-side match.
          if (stackIndex && stackStep != 0 &&
              address->Kind != NativeStackAddressKind::EntryStackPointer) {
            recordRelativeStore(*address, store);
          }
          continue;
        }
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr) {
          continue;
        }
        // Same convention as the first-pass dataflow: only concrete stack
        // slots are evidence for outgoing args. A store to an unclassified
        // address (data/heap pointer) does not invalidate the slots we are
        // matching, so keep scanning instead of stopping. Calls and other
        // memory-writing instructions still bound the scan.
        continue;
      }
      if (auto *otherCall = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (isAnalyzableCall(*otherCall)) {
          break;
        }
        continue;
      }
      if (inst.mayWriteToMemory()) {
        break;
      }
    }
    auto storeForIndexedOffset =
        [&](const std::map<int64_t, llvm::StoreInst *> &stores)
        -> llvm::StoreInst * {
      if (!stackIndex || stores.empty() || stackStep == 0 ||
          *stackIndex >
              static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) /
                  stackStep) {
        return nullptr;
      }
      int64_t delta = static_cast<int64_t>(*stackIndex * stackStep);
      int64_t lowestOffset = stores.begin()->first;
      if (lowestOffset > std::numeric_limits<int64_t>::max() - delta) {
        return nullptr;
      }
      auto found = stores.find(lowestOffset + delta);
      return found == stores.end() ? nullptr : found->second;
    };
    for (const RelativeStoreGroup &group : relativeStores) {
      if (llvm::StoreInst *store = storeForIndexedOffset(group.Stores)) {
        return store;
      }
    }
    return nullptr;
  }

llvm::StoreInst *
  FunctionBuilder::findNearestStackStoreBeforeCall(llvm::CallBase &call,
                                  const NativeSignatureSlot &slot) {
    std::vector<RelativeStoreGroup> relativeStores;
    return scanStackStoresBeforeCall(call, slot, relativeStores);
  }

llvm::StoreInst *
  FunctionBuilder::findNearestStackStoreInBlock(llvm::BasicBlock &block,
                               const NativeStackAddress &expected) {
    for (auto it = block.end(); it != block.begin();) {
      --it;
      llvm::Instruction &inst = *it;
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        std::optional<NativeStackAddress> address =
            stackAddressAnalysis().addressForPointer(
                store->getPointerOperand());
        if (address && *address == expected) {
          return store;
        }
        continue;
      }
      if (auto *otherCall = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (isAnalyzableCall(*otherCall)) {
          return nullptr;
        }
        continue;
      }
      if (inst.mayWriteToMemory()) {
        return nullptr;
      }
    }
    return nullptr;
  }

llvm::Value *
  FunctionBuilder::readCrossBlockStackSlotBeforeCall(
      llvm::CallBase &call, const NativeSignatureSlot &slot,
      const std::vector<RelativeStoreGroup> &relativeStores) {
    if (call.getParent() == nullptr) {
      return nullptr;
    }
    llvm::Type *targetType = slotType(slot);
    if (targetType == nullptr ||
        (!targetType->isIntegerTy() && !targetType->isFloatingPointTy())) {
      return nullptr;
    }
    std::optional<uint64_t> stackIndex = stackSlotIndex(slot);
    uint64_t stackStep =
        stackIndex ? stackGridStep(slot.StackAlign) : 0;
    if (!stackIndex || stackStep == 0 ||
        *stackIndex >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) /
                stackStep) {
      return nullptr;
    }
    int64_t delta = static_cast<int64_t>(*stackIndex * stackStep);
    for (const RelativeStoreGroup &group : relativeStores) {
      if (group.Kind == NativeStackAddressKind::EntryStackPointer ||
          group.Stores.empty()) {
        continue;
      }
      int64_t lowestOffset = group.Stores.begin()->first;
      if (lowestOffset > std::numeric_limits<int64_t>::max() - delta) {
        continue;
      }
      NativeStackAddress expected{group.Kind, group.Base,
                                  lowestOffset + delta};
      llvm::Value *pointer = nullptr;
      bool allPredecessorsWriteSlot = true;
      for (llvm::BasicBlock *predecessor :
           llvm::predecessors(call.getParent())) {
        llvm::StoreInst *store =
            findNearestStackStoreInBlock(*predecessor, expected);
        if (store == nullptr) {
          allPredecessorsWriteSlot = false;
          break;
        }
        if (pointer == nullptr) {
          pointer = store->getPointerOperand();
        }
      }
      if (!allPredecessorsWriteSlot || pointer == nullptr) {
        continue;
      }
      llvm::IRBuilder<> builder(&call);
      return resolve(builder.CreateLoad(targetType, pointer));
    }
    return nullptr;
  }

llvm::StoreInst *FunctionBuilder::findNearestStoreBeforeCall(llvm::CallBase &call,
                                              const RegisterUnit &unit) {
    for (auto it = call.getIterator(); it != call.getParent()->begin();) {
      --it;
      llvm::Instruction &inst = *it;
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && access.Unit->Global == unit.Global &&
            access.IsStorageValue) {
          return store;
        }
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        RegisterAccess access = registerLoad(*load, Units);
        if (access.Unit != nullptr && access.Unit->Global == unit.Global &&
            access.IsStorageValue &&
            load->getMetadata("notdec.register.summary_ssa.replaced") ==
                nullptr) {
          return nullptr;
        }
        continue;
      }
      if (auto *otherCall = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (std::optional<NativeRegisterPartialReadInfo> partial =
                parseNativeRegisterPartialRead(*otherCall)) {
          if (partial->Global == unit.Global) {
            return nullptr;
          }
          continue;
        }
        if (std::optional<NativeRegisterPartialWriteInfo> partial =
                parseNativeRegisterPartialWrite(*otherCall)) {
          if (partial->Global == unit.Global) {
            return nullptr;
          }
          continue;
        }
        llvm::Function *callee = otherCall->getCalledFunction();
        if (isAnalyzableCall(*otherCall) &&
            (callee == nullptr || !callee->isIntrinsic())) {
          return nullptr;
        }
        continue;
      }
      if (inst.mayWriteToMemory()) {
        return nullptr;
      }
    }
    return nullptr;
  }

bool FunctionBuilder::isEntryInputValue(llvm::Value *value) {
    value = resolve(value);
    for (const auto &[entryValue, range] : EntryInputRanges) {
      (void)range;
      if (resolve(entryValue) == value) {
        return true;
      }
    }
    return false;
  }

llvm::Value *FunctionBuilder::callRangeValue(llvm::CallBase &call,
                              const RegisterRangeKey &range) {
    const RegisterUnit *unit = unitForRange(range);
    if (unit == nullptr || range.BitWidth == 0) {
      return nullptr;
    }
    // 占位统一：第一遍与清理遍共用同一个键（不再按 return/clobber 分）。
    // "是不是返回值"唯一来自签名（shape.Returns）；第一遍只负责生成
    // "调用后状态"占位并记录 ABI 输出寄存器读证据，见
    // logs/20260805-12-native-unify-call-return-plan.md。
    CallRangeValueKey key{&call, range, "clobber"};
    if (auto cached = CallRangeValues.find(key);
        cached != CallRangeValues.end()) {
      return cached->second;
    }

    llvm::Instruction *insertBefore = call.getNextNode();
    if (insertBefore == nullptr) {
      insertBefore = call.getParent()->getTerminator();
    }
    if (insertBefore == nullptr) {
      return llvm::UndefValue::get(rangeType(range));
    }
    if (isAbiBoundaryZeroRegister(unit->Name)) {
      // DF 在调用点（外部 clobber 或内部函数返回）都是 ABI 保证的常量 0，
      // 见 isAbiBoundaryZeroRegister 注释。直接生成 0，避免 cmpsb 展开循环
      // 的方向被 summary_clobber / range_return_unknown 占位污染。
      llvm::Value *value = llvm::ConstantInt::get(rangeType(range), 0);
      CallRangeValues.emplace(key, value);
      return value;
    }
    llvm::IRBuilder<> builder(insertBefore);
    if (PostSignatureCleanup &&
        SignatureState.RewrittenCalls.count(&call) != 0 &&
        signatureReturnUsesUnit(call, *unit)) {
      const SignatureShape *shape = shapeForCall(SignatureState, call);
      if (shape != nullptr) {
        llvm::Value *value = extractReturnRange(builder, *shape, call, range);
        if (value != nullptr && value->getType() == rangeType(range)) {
          CallRangeValues.emplace(key, value);
          return value;
        }
      }
      llvm::Value *value = unknownValueAt(builder, rangeType(range),
                                          unit->Name + ".range_return_unknown");
      attachUnknownValueMetadata(value, "range_return_rewrite_missing_value",
                                 Function.getName(), calleeNameForWarning(call),
                                 unit->Name, "return");
      CallRangeValues.emplace(key, value);
      return value;
    }

    llvm::CallInst *value = builder.CreateCall(
        callRangeValueHelper(range), {}, unit->Name + ".clobber");
    value->setMetadata("notdec.register.summary_ssa.call_value",
                       callRangeValueNode(range, &call));
    CallRangeValues.emplace(key, value);
    ++Summary.CallClobberValues;
    // 调用点后 ABI 输出寄存器被读的证据：返回槽唯一由签名决定，
    // addDemandedExternalReturns 用这份证据给未知外部补返回槽，
    // 重写时按 shape 解析成真实返回值或 unknown。
    if (isIntegerAbiOutput(Abi, unit->Name) ||
        isFloatAbiOutputUnit(Abi, unit->Name)) {
      SignatureState.ReturnHelpers[&call];
      SignatureState.RangeReturnHelpers[&call].push_back(
          RangeReturnHelper{range, value});
    }
    return value;
  }

bool FunctionBuilder::signatureReturnUsesUnit(llvm::CallBase &call,
                               const RegisterUnit &unit) const {
    llvm::Function *callee = call.getCalledFunction();
    if (callee == nullptr) {
      const SignatureShape *shape = shapeForCall(SignatureState, call);
      if (shape == nullptr) {
        return false;
      }
      for (const NativeSignatureSlot &slot : shape->Returns) {
        if (slot.Unit == &unit || slot.Unit->Name == unit.Name) {
          return true;
        }
      }
      return false;
    }
    auto shapeIt = SignatureState.Shapes.find(callee);
    if (shapeIt == SignatureState.Shapes.end()) {
      return false;
    }
    for (const NativeSignatureSlot &slot : shapeIt->second.Returns) {
      if (slot.Unit == &unit || slot.Unit->Name == unit.Name) {
        return true;
      }
    }
    return false;
  }

bool FunctionBuilder::calleeNeverWritesRange(const llvm::CallBase &call,
                              const RegisterRangeKey &range) const {
    llvm::Function *callee = call.getCalledFunction();
    if (callee == nullptr || callee->isDeclaration()) {
      return false;
    }
    auto fnIt = SummaryFacts.find(callee);
    if (fnIt == SummaryFacts.end()) {
      return false;
    }
    const RegisterUnit *unit = unitForRange(range);
    if (unit == nullptr) {
      return false;
    }
    auto regIt = fnIt->second.Registers.find(unit->Name);
    if (regIt == fnIt->second.Registers.end()) {
      return false;
    }
    const llvm::APInt &mask = regIt->second.MayNonEntryMask;
    if (mask.getBitWidth() == 0 ||
        range.BitOffset + range.BitWidth > mask.getBitWidth()) {
      return false;
    }
    return mask.extractBits(range.BitWidth, range.BitOffset).isZero();
  }

llvm::FunctionCallee FunctionBuilder::callRangeValueHelper(const RegisterRangeKey &range) {
    llvm::Module *module = Function.getParent();
    llvm::Type *valueType = rangeType(range);
    llvm::FunctionType *functionType =
        llvm::FunctionType::get(valueType, {}, false);
    return module->getOrInsertFunction("notdec.register.summary_clobber." +
                                           typeSuffix(*valueType),
                                       functionType);
  }

llvm::MDNode *FunctionBuilder::registerNode(const RegisterUnit &unit) const {
    llvm::Metadata *fields[] = {
        llvm::MDString::get(Function.getContext(), "name=" + unit.Name),
    };
    return llvm::MDNode::get(Function.getContext(), fields);
  }

llvm::MDNode *FunctionBuilder::callRangeValueNode(const RegisterRangeKey &range,
                                   llvm::Instruction *call) const {
    const RegisterUnit *unit = unitForRange(range);
    uint64_t index = 0;
    for (const llvm::Instruction &inst : *call->getParent()) {
      if (&inst == call) {
        break;
      }
      ++index;
    }
    llvm::Metadata *fields[] = {
        llvm::MDString::get(
            Function.getContext(),
            "name=" + (unit == nullptr ? std::string("") : unit->Name)),
        llvm::MDString::get(Function.getContext(), "kind=clobber"),
        llvm::MDString::get(Function.getContext(),
                            "bit_offset=" + std::to_string(range.BitOffset)),
        llvm::MDString::get(Function.getContext(),
                            "bit_width=" + std::to_string(range.BitWidth)),
        llvm::MDString::get(Function.getContext(),
                            "call_index=" + std::to_string(index)),
    };
    return llvm::MDNode::get(Function.getContext(), fields);
  }

void FunctionBuilder::collectFunctionReturnValues(bool recordBinding ) {
    auto shapeIt = SignatureState.Shapes.find(&Function);
    if (shapeIt == SignatureState.Shapes.end() ||
        shapeIt->second.Returns.empty()) {
      return;
    }
    for (llvm::BasicBlock &block : Function) {
      auto *ret =
          llvm::dyn_cast_or_null<llvm::ReturnInst>(block.getTerminator());
      if (ret == nullptr) {
        continue;
      }
      std::vector<llvm::Value *> values;
      values.reserve(shapeIt->second.Returns.size());
      for (const NativeSignatureSlot &slot : shapeIt->second.Returns) {
        const RegisterUnit *unit = slot.Unit;
        llvm::Value *value = resolve(
            readSlotValueBefore(*ret, slot, unit->Name + ".return_range",
                                /*allowUnknownSegments=*/true));
        llvm::Value *observedValue = value;
        llvm::StringRef reason;
        if (value == nullptr) {
          reason = "return_binding_missing";
        } else if (value->getType() != slotType(slot)) {
          reason = "return_binding_type_mismatch";
        } else if (PostSignatureCleanup) {
          // A clobber is unknown for this path, not proof that every path is
          // unknown.  Preserve a mixed value graph so the concrete path stays
          // visible; only collapse a graph made solely of unknown leaves.
          const unsigned sources = classifyValueSources(value);
          if ((sources & SourceClobber) != 0) {
            if ((sources & SourceReal) != 0) {
              SignatureState.Warnings.push_back(returnSlotWarning(
                  Function, slot, "return_binding_preserved_mixed_clobber"));
            } else {
              value = unknownValueBefore(*ret, slotType(slot),
                                         unit->Name + ".return_unknown");
              attachUnknownValueMetadata(
                  value, "return_binding_uses_clobber_value",
                  Function.getName(), "<return>", unit->Name,
                  "return_binding");
            }
          }
        }
        // 返回绑定只在缺失/类型不符/纯 clobber/unknown 图时降级成 unknown；
        // 混合路径保留原值图，由重写后清理遍按签名解析成真实返回值。
        if (!reason.empty()) {
          SignatureState.Warnings.push_back(
              returnSlotWarning(Function, slot, reason));
          value = unknownValueBefore(*ret, slotType(slot),
                                     unit->Name + ".return_unknown");
          attachUnknownValueMetadata(value, reason, Function.getName(),
                                     "<return>", unit->Name, "return_binding");
        }
        values.push_back(value);
        if (observedValue != nullptr &&
            observedValue->getType() == slotType(slot)) {
          ReturnObservationValues.emplace_back(observedValue);
        }
      }
      if (PostSignatureCleanup && recordBinding &&
          ret->getReturnValue() != nullptr) {
        // 重写时 ret 先建 unknown，这里按最终链值更新。
        llvm::IRBuilder<> builder(ret);
        llvm::Value *retValue = buildReturnValue(builder, shapeIt->second, values);
        if (retValue != nullptr && retValue != ret->getReturnValue()) {
          ret->setOperand(0, retValue);
        }
      }
      if (recordBinding) {
        SignatureState.FunctionReturns[&Function][ret] = std::move(values);
        invalidateRecordedValueCache();
      }
    }
  }

} // namespace notdec::bin2llvm
