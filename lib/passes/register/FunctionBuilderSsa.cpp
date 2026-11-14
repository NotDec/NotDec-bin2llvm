#include "RegisterEliminationInternal.h"

namespace notdec::bin2llvm {

bool FunctionBuilder::rangesOverlap(const RegisterRangeKey &lhs,
                     const RegisterRangeKey &rhs) const {
    if (lhs.Global != rhs.Global) {
      return false;
    }
    uint64_t lhsEnd = lhs.BitOffset + lhs.BitWidth;
    uint64_t rhsEnd = rhs.BitOffset + rhs.BitWidth;
    return lhs.BitOffset < rhsEnd && rhs.BitOffset < lhsEnd;
  }

void FunctionBuilder::insertMaskRanges(LiveRegisterRanges &live, llvm::GlobalVariable *global,
                        const llvm::APInt &mask) const {
    auto unitIt = Units.find(global);
    if (unitIt == Units.end()) {
      return;
    }
    unsigned width = registerBitWidth(unitIt->second);
    if (width == 0) {
      return;
    }
    if (mask.getBitWidth() == 0 || mask.isZero()) {
      insertGlobalRanges(live, global);
      return;
    }
    llvm::APInt trimmed = mask.zextOrTrunc(width);
    unsigned bit = 0;
    while (bit < width) {
      while (bit < width && !trimmed[bit]) {
        ++bit;
      }
      unsigned start = bit;
      while (bit < width && trimmed[bit]) {
        ++bit;
      }
      if (start < bit) {
        insertPartialRanges(live, global, start, bit - start);
      }
    }
  }

void FunctionBuilder::eraseRegisterEffectRanges(LiveRegisterRanges &live,
                                 const RegisterUnit &unit) const {
    if (isFloatAbiOutputUnit(Abi, unit.Name)) {
      if (const AbiFacts::RegisterSlot *slot =
              floatAbiOutputSlotForUnit(Abi, unit.Name)) {
        erasePartialRanges(live, unit.Global, slot->OffsetBits, slot->SizeBits);
        return;
      }
    }
    eraseGlobalRanges(live, unit.Global);
  }

void FunctionBuilder::insertGlobalRanges(LiveRegisterRanges &live,
                          llvm::GlobalVariable *global) const {
    auto rangesIt = PlannedRanges.find(global);
    if (rangesIt == PlannedRanges.end() || rangesIt->second.empty()) {
      auto unitIt = Units.find(global);
      if (unitIt == Units.end()) {
        return;
      }
      unsigned width = registerBitWidth(unitIt->second);
      if (width != 0) {
        live.insert(RegisterRangeKey{global, 0, width});
      }
      return;
    }
    live.insert(rangesIt->second.begin(), rangesIt->second.end());
  }

void FunctionBuilder::eraseGlobalRanges(LiveRegisterRanges &live,
                         llvm::GlobalVariable *global) const {
    for (auto it = live.begin(); it != live.end();) {
      if (it->Global == global) {
        it = live.erase(it);
      } else {
        ++it;
      }
    }
  }

void FunctionBuilder::insertPartialRanges(LiveRegisterRanges &live,
                           llvm::GlobalVariable *global, uint64_t offset,
                           uint32_t width) const {
    std::vector<RegisterRangeKey> ranges =
        plannedRangesCovering(global, offset, width);
    if (ranges.empty()) {
      insertGlobalRanges(live, global);
      return;
    }
    live.insert(ranges.begin(), ranges.end());
  }

void FunctionBuilder::erasePartialRanges(LiveRegisterRanges &live,
                          llvm::GlobalVariable *global, uint64_t offset,
                          uint32_t width) const {
    std::vector<RegisterRangeKey> ranges =
        plannedRangesCovering(global, offset, width);
    if (ranges.empty()) {
      return;
    }
    for (const RegisterRangeKey &range : ranges) {
      live.erase(range);
    }
  }

bool FunctionBuilder::hasLiveGlobalRange(const LiveRegisterRanges &live,
                          llvm::GlobalVariable *global) const {
    for (const RegisterRangeKey &range : live) {
      if (range.Global == global) {
        return true;
      }
    }
    return false;
  }

bool FunctionBuilder::hasLivePartialRange(const LiveRegisterRanges &live,
                           llvm::GlobalVariable *global, uint64_t offset,
                           uint32_t width) const {
    std::vector<RegisterRangeKey> ranges =
        plannedRangesCovering(global, offset, width);
    if (ranges.empty()) {
      return hasLiveGlobalRange(live, global);
    }
    for (const RegisterRangeKey &range : ranges) {
      if (live.count(range) != 0) {
        return true;
      }
    }
    return false;
  }

bool FunctionBuilder::hasLiveWriteRange(const LiveRegisterRanges &live,
                         const RegisterUnit &unit) const {
    unsigned width = registerBitWidth(unit);
    if (width == 0) {
      return false;
    }
    std::vector<RegisterRangeKey> written =
        plannedRangesCovering(unit.Global, 0, width);
    if (written.empty()) {
      return hasLiveGlobalRange(live, unit.Global);
    }
    for (const RegisterRangeKey &writeRange : written) {
      for (const RegisterRangeKey &liveRange : live) {
        if (rangesOverlap(writeRange, liveRange)) {
          return true;
        }
      }
    }
    return false;
  }

const RegisterUnit *FunctionBuilder::unitForRange(const RegisterRangeKey &range) const {
    auto unitIt = Units.find(range.Global);
    return unitIt == Units.end() ? nullptr : &unitIt->second;
  }

llvm::IntegerType *FunctionBuilder::rangeType(const RegisterRangeKey &range) const {
    return llvm::IntegerType::get(Function.getContext(), range.BitWidth);
  }

llvm::Value *FunctionBuilder::unknownRangeBefore(llvm::Instruction &insertBefore,
                                  const RegisterRangeKey &range,
                                  llvm::Twine suffix,
                                  llvm::StringRef callee ) {
    const RegisterUnit *unit = unitForRange(range);
    if (unit == nullptr || range.BitWidth == 0) {
      return nullptr;
    }
    llvm::Value *value =
        unknownValueBefore(insertBefore, rangeType(range), unit->Name + suffix);
    std::string reason = suffix.str();
    if (!reason.empty() && reason.front() == '.') {
      reason.erase(reason.begin());
    }
    if (reason.empty()) {
      reason = "range_unknown";
    }
    std::string detail = "offset=" + std::to_string(range.BitOffset) +
                         ",width=" + std::to_string(range.BitWidth);
    attachUnknownValueMetadata(value, reason, Function.getName(), callee,
                               unit->Name, "range_ssa", detail);
    return value;
  }

llvm::Value *FunctionBuilder::attributedRangeUnknown(llvm::Instruction &insertBefore,
                                      const RegisterRangeKey &range,
                                      llvm::StringRef fallbackReason) {
    const RangeLossCause *cause =
        rangeLossCause({insertBefore.getParent(), range});
    if (cause == nullptr || cause->Reason.empty()) {
      return unknownRangeBefore(insertBefore, range, fallbackReason);
    }
    return unknownRangeBefore(insertBefore, range, cause->Reason, cause->Callee);
  }

llvm::Value *FunctionBuilder::rangeTypedValueOrUnknown(llvm::Value *value,
                                        const RegisterRangeKey &range,
                                        llvm::Instruction *insertBefore,
                                        llvm::Twine suffix) {
    value = resolve(value);
    if (value == nullptr) {
      return nullptr;
    }
    if (value->getType() == rangeType(range)) {
      return value;
    }
    if (insertBefore != nullptr) {
      return unknownRangeBefore(*insertBefore, range, suffix);
    }
    return llvm::UndefValue::get(rangeType(range));
  }

llvm::Value *FunctionBuilder::extractRangeValue(const RegisterRangeKey &range,
                                 llvm::Value *source, uint64_t sourceBitOffset,
                                 llvm::Instruction *before, llvm::Twine name) {
    if (source == nullptr || before == nullptr || range.BitWidth == 0) {
      return nullptr;
    }
    if (range.BitOffset < sourceBitOffset) {
      return nullptr;
    }
    return extractBitsFromInteger(source, range.BitWidth,
                                  range.BitOffset - sourceBitOffset, before,
                                  name);
  }

llvm::Instruction *
  FunctionBuilder::rangeWriteInsertPoint(llvm::Instruction &writeInst,
                        llvm::Instruction *readBefore) const {
    if (llvm::Instruction *next = writeInst.getNextNode()) {
      return next;
    }
    return readBefore;
  }

llvm::Value *FunctionBuilder::coerceStoreValueToRangeWidth(llvm::Value *value,
                                            const RegisterUnit &unit,
                                            llvm::Instruction *insertBefore) {
    value = resolve(value);
    if (value == nullptr || insertBefore == nullptr) {
      return nullptr;
    }
    llvm::Type *type = value->getType();
    if (type == nullptr ||
        (!type->isIntegerTy() && !type->isFloatingPointTy())) {
      return nullptr;
    }
    unsigned width = registerBitWidth(unit);
    if (width == 0 || valueBitWidth(value) != width) {
      return nullptr;
    }
    auto *integerType = llvm::IntegerType::get(Function.getContext(), width);
    if (type == integerType) {
      return value;
    }
    llvm::IRBuilder<> builder(insertBefore);
    return builder.CreateBitCast(value, integerType,
                                 unit.Name + ".range_store.bits");
  }

llvm::Value *FunctionBuilder::writeSegment(llvm::Instruction &writeInst,
                            const RegisterRangeKey &range, llvm::Value *value) {
    value = resolve(value);
    if (value == nullptr || value->getType() != rangeType(range)) {
      return nullptr;
    }
    CurrentDef[{writeInst.getParent(), range}] = RangedSSAValue{value, range};
    UnknownCurrentDef.erase({writeInst.getParent(), range});
    RangeLossCauses.erase({writeInst.getParent(), range});
    return value;
  }

bool FunctionBuilder::writeCoveredValue(llvm::Instruction &writeInst,
                         const RegisterRangeKey &coveredRange,
                         llvm::Value *value) {
    value = resolve(value);
    if (value == nullptr || valueBitWidth(value) != coveredRange.BitWidth) {
      return false;
    }
    std::vector<RegisterRangeKey> ranges = plannedRangesCovering(
        coveredRange.Global, coveredRange.BitOffset, coveredRange.BitWidth);
    if (ranges.empty()) {
      return false;
    }
    for (const RegisterRangeKey &range : ranges) {
      CurrentDef[{writeInst.getParent(), range}] =
          RangedSSAValue{value, coveredRange};
      UnknownCurrentDef.erase({writeInst.getParent(), range});
      RangeLossCauses.erase({writeInst.getParent(), range});
    }
    return true;
  }

bool FunctionBuilder::writeAccessRange(llvm::Instruction &writeInst,
                        llvm::GlobalVariable *global, uint64_t writeOffset,
                        uint32_t writeWidth, llvm::Value *source,
                        uint64_t sourceBitOffset,
                        llvm::Instruction *insertBefore, llvm::Twine name) {
    if (source == nullptr || insertBefore == nullptr || writeWidth == 0) {
      return false;
    }
    std::vector<RegisterRangeKey> ranges =
        plannedRangesCovering(global, writeOffset, writeWidth);
    if (ranges.empty()) {
      return false;
    }
    if (valueBitWidth(source) == writeWidth && sourceBitOffset == writeOffset) {
      RegisterRangeKey coveredRange{global, writeOffset, writeWidth};
      if (writeCoveredValue(writeInst, coveredRange, source)) {
        return true;
      }
    }
    for (const RegisterRangeKey &range : ranges) {
      llvm::Value *segment =
          extractRangeValue(range, source, sourceBitOffset, insertBefore, name);
      if (writeSegment(writeInst, range, segment) == nullptr) {
        return false;
      }
    }
    return true;
  }

llvm::Value *FunctionBuilder::currentSegment(llvm::BasicBlock &block,
                              const RegisterRangeKey &range,
                              llvm::Instruction *before) {
    auto cached = CurrentDef.find({&block, range});
    if (cached == CurrentDef.end()) {
      return nullptr;
    }
    const RangedSSAValue &def = cached->second;
    llvm::Value *value = resolve(def.Value);
    if (value == nullptr) {
      return nullptr;
    }
    if (def.CoveredRange == range) {
      return value->getType() == rangeType(range) ? value : nullptr;
    }
    if (!rangeContains(def.CoveredRange, range)) {
      return nullptr;
    }
    return materializeCoveredAccess(def, range.BitOffset, range.BitWidth,
                                    before, "range_segment");
  }

void FunctionBuilder::clearBlockRangeDefs(llvm::BasicBlock &block,
                           llvm::GlobalVariable *global) {
    for (auto it = CurrentDef.begin(); it != CurrentDef.end();) {
      if (it->first.first == &block && it->first.second.Global == global) {
        it = CurrentDef.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = UnknownCurrentDef.begin(); it != UnknownCurrentDef.end();) {
      if (it->first == &block && it->second.Global == global) {
        it = UnknownCurrentDef.erase(it);
      } else {
        ++it;
      }
    }
  }

void FunctionBuilder::clearAllBlockRangeDefs(llvm::BasicBlock &block) {
    for (auto it = CurrentDef.begin(); it != CurrentDef.end();) {
      if (it->first.first == &block) {
        it = CurrentDef.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = UnknownCurrentDef.begin(); it != UnknownCurrentDef.end();) {
      if (it->first == &block) {
        it = UnknownCurrentDef.erase(it);
      } else {
        ++it;
      }
    }
    CurrentDefPosition.erase(&block);
  }

void FunctionBuilder::markBlockRangeUnknown(llvm::BasicBlock &block,
                             const RegisterRangeKey &range,
                             const RangeLossCause &cause ) {
    CurrentDef.erase({&block, range});
    UnknownCurrentDef.insert({&block, range});
    if (!cause.Reason.empty()) {
      RangeLossCauses[{&block, range}] = cause;
    }
  }

void FunctionBuilder::inheritRangeLossCause(const BlockRangeKey &dst,
                             const BlockRangeKey &src) {
    if (RangeLossCauses.count(dst) != 0) {
      return;
    }
    auto it = RangeLossCauses.find(src);
    if (it != RangeLossCauses.end()) {
      RangeLossCauses[dst] = it->second;
    }
  }

const RangeLossCause *FunctionBuilder::rangeLossCause(const BlockRangeKey &key) const {
    auto it = RangeLossCauses.find(key);
    return it == RangeLossCauses.end() ? nullptr : &it->second;
  }

bool FunctionBuilder::transferRangeInstruction(llvm::Instruction &inst,
                                llvm::Instruction *readBefore) {
    llvm::BasicBlock &block = *inst.getParent();
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      RegisterAccess access = registerStore(*store, Units);
      if (access.Unit != nullptr) {
        llvm::Instruction *insertBefore =
            rangeWriteInsertPoint(*store, readBefore);
        llvm::Value *source =
            access.IsStorageValue
                ? resolve(store->getValueOperand())
                : coerceStoreValueToRangeWidth(store->getValueOperand(),
                                               *access.Unit, insertBefore);
        if (source != nullptr) {
          (void)writeAccessRange(
              *store, access.Unit->Global, 0, registerBitWidth(*access.Unit),
              source, 0, insertBefore, access.Unit->Name + ".range_store");
        }
      }
      return true;
    }

    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call == nullptr || parseNativeRegisterPartialRead(*call)) {
      return true;
    }
    if (std::optional<NativeRegisterPartialWriteInfo> partial =
            parseNativeRegisterPartialWrite(*call)) {
      auto unitIt = Units.find(partial->Global);
      if (unitIt != Units.end()) {
        (void)writeAccessRange(*call, partial->Global, partial->BitOffset,
                               partial->WriteWidth, partial->Value,
                               partial->BitOffset,
                               rangeWriteInsertPoint(*call, readBefore),
                               unitIt->second.Name + ".range_partial_write");
      }
      return true;
    }
    if (!isAnalyzableCall(*call)) {
      return true;
    }

    bool countedUnknownCall = false;
    // Register order decides the order of the post-call placeholders created
    // below (and therefore every generated value name); keep it name-stable
    // instead of relying on std::map<GlobalVariable*> pointer order.
    for (const RegisterUnit *unitPtr : unitsInCreationOrder(*Function.getParent(), Units)) {
      llvm::GlobalVariable *global = unitPtr->Global;
      const RegisterUnit &unit = *unitPtr;
      CallRegisterEffect effect = callEffect(*call, unit);
      if (effect == CallRegisterEffect::Preserve) {
        continue;
      }
      bool mixed = effect != CallRegisterEffect::ReturnValue &&
                   effect != CallRegisterEffect::Clobber;
      if (mixed && !countedUnknownCall) {
        ++Summary.UnknownCallEffects;
        countedUnknownCall = true;
      }
      std::vector<RegisterRangeKey> ranges =
          plannedRangesCovering(global, 0, registerBitWidth(unit));
      if (ranges.empty()) {
        clearBlockRangeDefs(block, global);
        continue;
      }
      // Mixed（出口既可能保留入口值、也可能是新值）。按"第一遍统一生成
      // 调用后状态占位、是不是返回由签名决定"的原则，这不该直接判丢：只要签名
      // 说这个寄存器在 callee 的返回槽里，就照 Clobber 一样生成可替换占位，
      // 重写 / 清理遍再按签名解析成真实返回值（就是调用后寄存器真实持有的值，
      // 包含"原样保留入口值"的路径）。注意这里只动分派、不动 callEffect 的
      // 分类：活跃性里"入口值可能活过这次调用"那条仍然保留。
      // 不在返回槽里才保持原来的"判丢 + 归因"路径，省掉一次无意义的占位。
      bool carried = signatureReturnUsesUnit(*call, unit);
      RangeLossCause cause =
          mixed ? unknownCallEffectCause(*call) : RangeLossCause();
      for (const RegisterRangeKey &range : ranges) {
        // 按位细化：callee 从不写这些位，出口值就等于入口值，调用者自己的调用
        // 前值就是答案（Arch2.md 第 13 节第二/三行）。整寄存器的 Mixed 布尔在
        // 这里被拆到每一位，这是"返回槽只覆盖真写过的位"能成立的前提。
        if (calleeNeverWritesRange(*call, range)) {
          continue;
        }
        if (mixed && !carried) {
          markBlockRangeUnknown(block, range, cause);
          continue;
        }
        llvm::Value *value = callRangeValue(*call, range);
        value = resolve(value);
        if (value != nullptr && value->getType() == rangeType(range)) {
          CurrentDef[{&block, range}] = RangedSSAValue{value, range};
          UnknownCurrentDef.erase({&block, range});
          RangeLossCauses.erase({&block, range});
        }
      }
    }
    return true;
  }

bool FunctionBuilder::transferRangeBlockUntil(llvm::BasicBlock &block,
                               llvm::Instruction *before) {
    if (before == nullptr || before->getParent() != &block) {
      return false;
    }

    llvm::Instruction *position = nullptr;
    if (auto posIt = CurrentDefPosition.find(&block);
        posIt != CurrentDefPosition.end()) {
      position = posIt->second;
    }
    if (position != nullptr && position->getParent() != &block) {
      clearAllBlockRangeDefs(block);
      position = nullptr;
    }
    if (position != nullptr && !position->comesBefore(before)) {
      clearAllBlockRangeDefs(block);
      position = nullptr;
    }
    auto it = position == nullptr ? block.begin()
                                  : std::next(position->getIterator());
    for (; it != block.end() && &*it != before; ++it) {
      (void)transferRangeInstruction(*it, before);
      CurrentDefPosition[&block] = &*it;
    }
    return true;
  }

llvm::Value *FunctionBuilder::readRangeBefore(llvm::BasicBlock &block,
                               const RegisterRangeKey &range,
                               llvm::Instruction *before,
                               llvm::DominatorTree &domTree) {
    const RegisterUnit *unit = unitForRange(range);
    if (unit == nullptr || before == nullptr || range.BitWidth == 0) {
      return nullptr;
    }

    if (!transferRangeBlockUntil(block, before)) {
      return nullptr;
    }
    if (llvm::Value *value = currentSegment(block, range, before)) {
      return value;
    }
    if (UnknownCurrentDef.count({&block, range}) != 0) {
      return nullptr;
    }
    return readRangeEntry(block, range, domTree);
  }

llvm::Value *FunctionBuilder::readRangeEntry(llvm::BasicBlock &block,
                              const RegisterRangeKey &range,
                              llvm::DominatorTree &domTree) {
    BlockRangeKey key{&block, range};
    if (auto cached = EntryRangeValue.find(key);
        cached != EntryRangeValue.end()) {
      return resolve(cached->second);
    }
    if (ResolvingRangeEntry.count(key) != 0) {
      return ensureRangePhi(block, range);
    }

    ResolvingRangeEntry.insert(key);
    std::vector<llvm::BasicBlock *> preds(llvm::pred_begin(&block),
                                          llvm::pred_end(&block));
    llvm::Value *value = nullptr;
    if (preds.empty()) {
      value = entryRangeInput(block, range);
    } else if (preds.size() == 1) {
      value = readRangeExit(*preds.front(), range, domTree);
      if (PendingRangePhi.count(key) != 0) {
        value = completeRangePhi(block, range, domTree);
      }
    } else {
      value = completeRangePhi(block, range, domTree);
    }
    ResolvingRangeEntry.erase(key);
    EntryRangeValue[key] = resolve(value);
    if (EntryRangeValue[key] == nullptr) {
      // 值丢失时把成因沿 pred 传上来，phi 兜底才能指名到具体调用点。
      for (llvm::BasicBlock *pred : preds) {
        inheritRangeLossCause(key, {pred, range});
      }
    }
    return EntryRangeValue[key];
  }

llvm::Value *FunctionBuilder::readRangeExit(llvm::BasicBlock &block,
                             const RegisterRangeKey &range,
                             llvm::DominatorTree &domTree) {
    BlockRangeKey key{&block, range};
    if (auto cached = ExitRangeValue.find(key);
        cached != ExitRangeValue.end()) {
      return resolve(cached->second);
    }
    llvm::Instruction *terminator = block.getTerminator();
    llvm::Value *value =
        terminator == nullptr
            ? readRangeEntry(block, range, domTree)
            : readRangeBefore(block, range, terminator, domTree);
    value = resolve(value);
    if (terminator == nullptr ||
        valueDominatesUse(value, terminator, domTree)) {
      ExitRangeValue[key] = value;
    }
    return value;
  }

llvm::PHINode *FunctionBuilder::ensureRangePhi(llvm::BasicBlock &block,
                                const RegisterRangeKey &range) {
    const RegisterUnit *unit = unitForRange(range);
    if (unit == nullptr) {
      return nullptr;
    }
    BlockRangeKey key{&block, range};
    if (auto existing = PendingRangePhi.find(key);
        existing != PendingRangePhi.end()) {
      return existing->second;
    }
    llvm::IRBuilder<> builder(&block, block.getFirstNonPHIIt());
    llvm::PHINode *phi = builder.CreatePHI(rangeType(range), 0,
                                           unit->Name + ".range_summary_ssa");
    phi->setMetadata("notdec.register.summary_ssa.phi", registerNode(*unit));
    PendingRangePhi.emplace(key, phi);
    EntryRangeValue[key] = phi;
    ++Summary.PhisCreated;
    return phi;
  }

llvm::Value *FunctionBuilder::completeRangePhi(llvm::BasicBlock &block,
                                const RegisterRangeKey &range,
                                llvm::DominatorTree &domTree) {
    llvm::PHINode *phi = ensureRangePhi(block, range);
    if (phi == nullptr) {
      return nullptr;
    }
    std::map<llvm::BasicBlock *, unsigned> existingIncoming;
    for (unsigned index = 0; index < phi->getNumIncomingValues(); ++index) {
      ++existingIncoming[phi->getIncomingBlock(index)];
    }
    std::map<llvm::BasicBlock *, unsigned> requiredIncoming;
    // 同一个前驱在 CFG 里可能出现多次（重复边：switch 多个 case 指向同一块、
    // 条件分支两边同目标等）。LLVM 要求同一前驱的多个 incoming **值相同**，所以本轮
    // 为同一个 pred 补的多个槽必须复用同一个值——否则每条边各造一个 unknown 会得到
    // 非法 phi（实测 tmux 上偶发 verifier 报 "multiple entries for the same basic
    // block with different incoming values"，是随堆布局变化的偶发失败）。
    std::map<llvm::BasicBlock *, llvm::Value *> appendedIncoming;
    for (llvm::BasicBlock *pred : llvm::predecessors(&block)) {
      unsigned requiredCount = ++requiredIncoming[pred];
      if (existingIncoming[pred] >= requiredCount) {
        continue;
      }
      if (auto cached = appendedIncoming.find(pred);
          cached != appendedIncoming.end()) {
        phi->addIncoming(cached->second, pred);
        continue;
      }
      llvm::Instruction *terminator = pred->getTerminator();
      llvm::Value *incoming =
          rangeTypedValueOrUnknown(readRangeExit(*pred, range, domTree), range,
                                   terminator, ".range_type_mismatch");
      if (incoming != nullptr &&
          !valueDominatesUse(incoming, terminator, domTree)) {
        incoming = nullptr;
      }
      if (incoming == nullptr) {
        incoming =
            terminator != nullptr
                ? attributedRangeUnknown(*terminator, range, ".range_unknown")
                : llvm::UndefValue::get(rangeType(range));
      }
      appendedIncoming.emplace(pred, incoming);
      phi->addIncoming(incoming, pred);
    }
    return simplifyRangePhi(*phi, range);
  }

llvm::Value *FunctionBuilder::simplifyRangePhi(llvm::PHINode &phi,
                                const RegisterRangeKey &range) {
    if (!isCompletePhi(phi)) {
      return &phi;
    }
    llvm::Value *same = nullptr;
    for (llvm::Value *incoming : phi.incoming_values()) {
      incoming = resolve(incoming);
      if (incoming == &phi) {
        continue;
      }
      if (same == nullptr) {
        same = incoming;
        continue;
      }
      if (same != incoming) {
        return &phi;
      }
    }
    if (same == nullptr) {
      auto insertIt = phi.getParent()->getFirstInsertionPt();
      if (insertIt == phi.getParent()->end()) {
        return &phi;
      }
      same = unknownRangeBefore(*insertIt, range, ".range_phi_unknown");
      if (same == nullptr) {
        return &phi;
      }
    }
    recordReplacement(&phi, same);
    phi.replaceAllUsesWith(same);
    DeadPhis.insert(&phi);
    ++Summary.PhisSimplified;
    return same;
  }

llvm::Value *FunctionBuilder::readValueBefore(llvm::BasicBlock &block,
                               const RegisterUnit &unit,
                               llvm::Instruction *before) {
    llvm::DominatorTree domTree(Function);
    llvm::Value *rangeValue = readFullRangeValueBefore(
        block, unit, before, domTree, /*allowUnknownSegments=*/true);
    rangeValue = resolve(rangeValue);
    if (rangeValue != nullptr &&
        rangeValue->getType() == unit.Global->getValueType()) {
      return rangeValue;
    }
    return nullptr;
  }

llvm::Value *FunctionBuilder::readBlockLocalValueBefore(llvm::LoadInst &load,
                                         const RegisterUnit &unit) {
    llvm::BasicBlock &block = *load.getParent();
    if (!transferRangeBlockUntil(block, &load)) {
      return nullptr;
    }
    std::vector<RegisterRangeKey> ranges =
        plannedRangesCovering(unit.Global, 0, registerBitWidth(unit));
    if (ranges.size() != 1) {
      return nullptr;
    }
    llvm::Value *rangeValue =
        resolve(currentSegment(block, ranges.front(), &load));
    if (rangeValue == nullptr) {
      return nullptr;
    }
    if (rangeValue->getType() != load.getType()) {
      return nullptr;
    }
    return rangeValue;
  }

bool FunctionBuilder::isCompletePhi(const llvm::PHINode &phi) const {
    const llvm::BasicBlock *block = phi.getParent();
    return block != nullptr &&
           phi.getNumIncomingValues() == llvm::pred_size(block);
  }

void FunctionBuilder::finalizePendingPhis() {
    bool changed = true;
    while (changed) {
      changed = false;
      std::vector<std::pair<llvm::BasicBlock *, RegisterRangeKey>> rangeWork;
      for (const auto &[key, phi] : PendingRangePhi) {
        if (DeadPhis.count(phi) == 0 && !isCompletePhi(*phi)) {
          rangeWork.push_back({key.first, key.second});
        }
      }
      for (const auto &[block, range] : rangeWork) {
        llvm::PHINode *phi = PendingRangePhi[{block, range}];
        unsigned before = phi->getNumIncomingValues();
        llvm::DominatorTree domTree(Function);
        (void)completeRangePhi(*block, range, domTree);
        changed |= phi->getNumIncomingValues() != before;
      }
    }
  }

void FunctionBuilder::eraseDeadPhis() {
    for (llvm::PHINode *phi : DeadPhis) {
      if (phi->use_empty()) {
        phi->eraseFromParent();
      }
    }
  }

llvm::Value *FunctionBuilder::resolve(llvm::Value *value) const {
    while (value != nullptr) {
      auto it = Replacement.find(value);
      if (it == Replacement.end() || it->second == value) {
        return value;
      }
      value = it->second;
    }
    return nullptr;
  }

llvm::GlobalVariable *FunctionBuilder::stackPointerGlobal() const {
    if (Abi.StackPointer.empty()) {
      return nullptr;
    }
    for (const auto &[global, unit] : Units) {
      if (unit.Name == Abi.StackPointer) {
        return global;
      }
    }
    return nullptr;
  }

std::pair<llvm::Argument *, const NativeSignatureSlot *>
  FunctionBuilder::entryArgumentSlot(const RegisterUnit &unit) const {
    auto shapeIt = SignatureState.Shapes.find(&Function);
    if (shapeIt == SignatureState.Shapes.end()) {
      return {nullptr, nullptr};
    }
    llvm::Argument *arg = Function.arg_begin();
    for (const NativeSignatureSlot &slot : shapeIt->second.Params) {
      if (arg == Function.arg_end()) {
        return {nullptr, nullptr};
      }
      if (slot.Unit != nullptr &&
          (slot.Unit == &unit || slot.Unit->Global == unit.Global)) {
        return {arg, &slot};
      }
      ++arg;
    }
    return {nullptr, nullptr};
  }

} // namespace notdec::bin2llvm
