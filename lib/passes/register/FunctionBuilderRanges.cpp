#include "RegisterEliminationInternal.h"

namespace notdec::bin2llvm {

unsigned FunctionBuilder::registerBitWidth(const RegisterUnit &unit) const {
    auto *type = llvm::dyn_cast<llvm::IntegerType>(unit.Global->getValueType());
    return type == nullptr ? 0 : type->getBitWidth();
  }

void FunctionBuilder::addRangeBoundary(llvm::GlobalVariable *global, uint64_t offset,
                        uint64_t width) {
    auto unitIt = Units.find(global);
    if (unitIt == Units.end() || width == 0) {
      return;
    }
    unsigned fullWidth = registerBitWidth(unitIt->second);
    if (fullWidth == 0 || offset > fullWidth || offset + width > fullWidth) {
      return;
    }
    std::set<uint64_t> &boundaries = RangeBoundaries[global];
    boundaries.insert(0);
    boundaries.insert(fullWidth);
    boundaries.insert(offset);
    boundaries.insert(offset + width);
  }

const RegisterUnit *FunctionBuilder::unitForName(llvm::StringRef name) const {
    for (const auto &[global, unit] : Units) {
      (void)global;
      if (unit.Name == name) {
        return &unit;
      }
    }
    return nullptr;
  }

void FunctionBuilder::addRangeBoundaryForSlot(const AbiFacts::RegisterSlot &slot) {
    const RegisterUnit *unit = unitForName(slot.UnitName);
    if (unit == nullptr) {
      return;
    }
    addRangeBoundary(unit->Global, slot.OffsetBits, slot.SizeBits);
  }

void FunctionBuilder::addRangeBoundariesForMask(llvm::GlobalVariable *global,
                                 const llvm::APInt &mask) {
    if (mask.getBitWidth() == 0 || mask.isZero()) {
      return;
    }
    auto unitIt = Units.find(global);
    if (unitIt == Units.end()) {
      return;
    }
    unsigned fullWidth = registerBitWidth(unitIt->second);
    if (fullWidth == 0) {
      return;
    }
    llvm::APInt trimmed = mask.zextOrTrunc(fullWidth);
    unsigned bit = 0;
    while (bit < fullWidth) {
      while (bit < fullWidth && !trimmed[bit]) {
        ++bit;
      }
      unsigned start = bit;
      while (bit < fullWidth && trimmed[bit]) {
        ++bit;
      }
      if (start < bit) {
        addRangeBoundary(global, start, bit - start);
      }
    }
  }

void FunctionBuilder::recordRangeEvent(llvm::GlobalVariable *global, uint64_t offset,
                        uint32_t width, RangeEventKind kind) {
    std::vector<RegisterRangeKey> ranges =
        plannedRangesCovering(global, offset, width);
    uint64_t count = ranges.empty() ? 1 : ranges.size();
    switch (kind) {
    case RangeEventKind::Read:
      Summary.RangeReadEvents += count;
      break;
    case RangeEventKind::Write:
      Summary.RangeWriteEvents += count;
      break;
    case RangeEventKind::Clobber:
      Summary.RangeClobberEvents += count;
      break;
    }
  }

void FunctionBuilder::addAbiRangeBoundaries() {
    for (const AbiFacts::RegisterSlot &slot : Abi.IntegerInputsInOrder) {
      addRangeBoundaryForSlot(slot);
    }
    for (const AbiFacts::RegisterSlot &slot : Abi.FloatInputsInOrder) {
      addRangeBoundaryForSlot(slot);
    }
    for (const AbiFacts::RegisterSlot &slot : Abi.IntegerOutputsInOrder) {
      addRangeBoundaryForSlot(slot);
    }
    for (const AbiFacts::RegisterSlot &slot : Abi.FloatOutputsInOrder) {
      addRangeBoundaryForSlot(slot);
    }
    for (const std::string &name : Abi.KilledByCall) {
      if (const RegisterUnit *unit = unitForName(name)) {
        addRangeBoundary(unit->Global, 0, registerBitWidth(*unit));
      }
    }
    for (const std::string &name : Abi.Unaffected) {
      if (const RegisterUnit *unit = unitForName(name)) {
        addRangeBoundary(unit->Global, 0, registerBitWidth(*unit));
      }
    }
  }

void FunctionBuilder::addSummaryDemandRangeBoundaries() {
    auto factsIt = SummaryFacts.find(&Function);
    if (factsIt == SummaryFacts.end()) {
      return;
    }
    for (const auto &[global, unit] : Units) {
      auto regIt = factsIt->second.Registers.find(unit.Name);
      if (regIt == factsIt->second.Registers.end()) {
        continue;
      }
      addRangeBoundariesForMask(global, regIt->second.ReadEntryMask);
      addRangeBoundariesForMask(global, regIt->second.ExitDemandMask);
    }
  }

void FunctionBuilder::addCallParamRangeBoundaries() {
    for (llvm::Instruction &inst : llvm::instructions(Function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr || !isAnalyzableCall(*call)) {
        continue;
      }
      const SignatureShape *shape = shapeForCall(SignatureState, *call);
      if (shape == nullptr) {
        continue;
      }
      for (const NativeSignatureSlot &slot : callParamSlots(*call, *shape)) {
        if (slot.Unit == nullptr) {
          continue;
        }
        addRangeBoundary(slot.Unit->Global, slot.OffsetBits, slot.SizeBits);
      }
    }
  }

void FunctionBuilder::planRegisterRanges() {
    RangeBoundaries.clear();
    PlannedRanges.clear();
    for (const auto &[global, unit] : Units) {
      unsigned fullWidth = registerBitWidth(unit);
      if (fullWidth == 0) {
        continue;
      }
      RangeBoundaries[global].insert(0);
      RangeBoundaries[global].insert(fullWidth);
    }

    for (llvm::Instruction &inst : llvm::instructions(Function)) {
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        RegisterAccess access = registerLoad(*load, Units);
        if (access.Unit != nullptr && access.IsStorageValue) {
          addRangeBoundary(access.Unit->Global, 0,
                           registerBitWidth(*access.Unit));
        }
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && access.IsStorageValue) {
          addRangeBoundary(access.Unit->Global, 0,
                           registerBitWidth(*access.Unit));
        }
        continue;
      }
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr) {
        continue;
      }
      if (std::optional<NativeRegisterPartialReadInfo> partial =
              parseNativeRegisterPartialRead(*call)) {
        addRangeBoundary(partial->Global, partial->BitOffset,
                         partial->ReadWidth);
        continue;
      }
      if (std::optional<NativeRegisterPartialWriteInfo> partial =
              parseNativeRegisterPartialWrite(*call)) {
        addRangeBoundary(partial->Global, partial->BitOffset,
                         partial->WriteWidth);
      }
    }

    addAbiRangeBoundaries();
    addSummaryDemandRangeBoundaries();
    addCallParamRangeBoundaries();

    // Iterate registers in name order, not std::map<GlobalVariable*> order:
    // PlannedRanges is a vector, and the order in which its segments are later
    // materialized determines every generated value name.
    for (const RegisterUnit *unitPtr : unitsInNameOrder(Units)) {
      llvm::GlobalVariable *global = unitPtr->Global;
      auto boundariesIt = RangeBoundaries.find(global);
      if (boundariesIt == RangeBoundaries.end()) {
        continue;
      }
      const std::set<uint64_t> &boundaries = boundariesIt->second;
      if (boundaries.size() < 2) {
        continue;
      }
      std::vector<RegisterRangeKey> &ranges = PlannedRanges[global];
      auto previous = boundaries.begin();
      for (auto current = std::next(previous); current != boundaries.end();
           previous = current, ++current) {
        if (*current > *previous) {
          ranges.push_back(RegisterRangeKey{
              global, *previous, static_cast<uint32_t>(*current - *previous)});
        }
      }
    }

    for (const auto &[global, ranges] : PlannedRanges) {
      if (!ranges.empty()) {
        ++Summary.RangeRegistersPlanned;
        Summary.RangeSegmentsPlanned += ranges.size();
      }
    }

    for (llvm::Instruction &inst : llvm::instructions(Function)) {
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        RegisterAccess access = registerLoad(*load, Units);
        if (access.Unit != nullptr && access.IsStorageValue) {
          recordRangeEvent(access.Unit->Global, 0,
                           registerBitWidth(*access.Unit),
                           RangeEventKind::Read);
        }
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && access.IsStorageValue) {
          recordRangeEvent(access.Unit->Global, 0,
                           registerBitWidth(*access.Unit),
                           RangeEventKind::Write);
        }
        continue;
      }
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr) {
        continue;
      }
      if (std::optional<NativeRegisterPartialReadInfo> partial =
              parseNativeRegisterPartialRead(*call)) {
        recordRangeEvent(partial->Global, partial->BitOffset,
                         partial->ReadWidth, RangeEventKind::Read);
        continue;
      }
      if (std::optional<NativeRegisterPartialWriteInfo> partial =
              parseNativeRegisterPartialWrite(*call)) {
        recordRangeEvent(partial->Global, partial->BitOffset,
                         partial->WriteWidth, RangeEventKind::Write);
        continue;
      }
      if (!isAnalyzableCall(*call)) {
        continue;
      }
      for (const auto &[global, unit] : Units) {
        if (callReadsRegister(*call, unit)) {
          recordRangeEvent(global, 0, registerBitWidth(unit),
                           RangeEventKind::Read);
        }
        CallRegisterEffect effect = callEffect(*call, unit);
        if (effect == CallRegisterEffect::ReturnValue) {
          recordRangeEvent(global, 0, registerBitWidth(unit),
                           RangeEventKind::Write);
        } else if (effect == CallRegisterEffect::Clobber) {
          recordRangeEvent(global, 0, registerBitWidth(unit),
                           RangeEventKind::Clobber);
        }
      }
    }
  }

std::vector<RegisterRangeKey>
  FunctionBuilder::plannedRangesCovering(llvm::GlobalVariable *global, uint64_t offset,
                        uint32_t width) const {
    std::vector<RegisterRangeKey> result;
    if (width == 0) {
      return result;
    }
    auto rangesIt = PlannedRanges.find(global);
    if (rangesIt == PlannedRanges.end()) {
      return result;
    }

    uint64_t cursor = offset;
    uint64_t end = offset + width;
    for (const RegisterRangeKey &range : rangesIt->second) {
      if (range.BitOffset + range.BitWidth <= cursor) {
        continue;
      }
      if (range.BitOffset > cursor || range.BitOffset >= end) {
        break;
      }
      if (range.BitOffset + range.BitWidth > end) {
        break;
      }
      result.push_back(range);
      cursor = range.BitOffset + range.BitWidth;
      if (cursor == end) {
        return result;
      }
    }
    result.clear();
    return result;
  }

bool FunctionBuilder::rangeContains(const RegisterRangeKey &outer,
                            const RegisterRangeKey &inner) {
    return outer.Global == inner.Global && outer.BitOffset <= inner.BitOffset &&
           inner.BitOffset + inner.BitWidth <= outer.BitOffset + outer.BitWidth;
  }

bool FunctionBuilder::rangeContainsAccess(const RegisterRangeKey &range,
                                  llvm::GlobalVariable *global, uint64_t offset,
                                  uint32_t width) {
    return range.Global == global && range.BitOffset <= offset &&
           offset + width <= range.BitOffset + range.BitWidth;
  }

llvm::Value *FunctionBuilder::materializeCoveredAccess(const RangedSSAValue &def,
                                        uint64_t readOffset, uint32_t readWidth,
                                        llvm::Instruction *before,
                                        llvm::Twine name) {
    llvm::Value *value = resolve(def.Value);
    if (value == nullptr || before == nullptr ||
        valueBitWidth(value) != def.CoveredRange.BitWidth ||
        !rangeContainsAccess(def.CoveredRange, def.CoveredRange.Global,
                             readOffset, readWidth)) {
      return nullptr;
    }
    if (def.CoveredRange.BitOffset == readOffset &&
        def.CoveredRange.BitWidth == readWidth) {
      return value;
    }
    return extractBitsFromIntegerValue(value, readWidth,
                                       readOffset - def.CoveredRange.BitOffset,
                                       before, name);
  }

llvm::Value *FunctionBuilder::tryCurrentCoveredAccess(llvm::GlobalVariable *global,
                                       uint64_t readOffset, uint32_t readWidth,
                                       llvm::Instruction *before,
                                       llvm::Twine name) {
    if (global == nullptr || before == nullptr || readWidth == 0 ||
        before->getParent() == nullptr) {
      return nullptr;
    }
    std::vector<RegisterRangeKey> ranges =
        plannedRangesCovering(global, readOffset, readWidth);
    if (ranges.empty() ||
        !transferRangeBlockUntil(*before->getParent(), before)) {
      return nullptr;
    }

    const RangedSSAValue *first = nullptr;
    llvm::Value *firstValue = nullptr;
    for (const RegisterRangeKey &range : ranges) {
      auto it = CurrentDef.find({before->getParent(), range});
      if (it == CurrentDef.end()) {
        return nullptr;
      }
      llvm::Value *value = resolve(it->second.Value);
      if (value == nullptr ||
          !rangeContainsAccess(it->second.CoveredRange, global, readOffset,
                               readWidth)) {
        return nullptr;
      }
      if (first == nullptr) {
        first = &it->second;
        firstValue = value;
        continue;
      }
      if (firstValue != value ||
          !(first->CoveredRange == it->second.CoveredRange)) {
        return nullptr;
      }
    }
    return first == nullptr ? nullptr
                            : materializeCoveredAccess(*first, readOffset,
                                                       readWidth, before, name);
  }

llvm::Value *
  FunctionBuilder::tryCommonExtractSourceAccess(llvm::ArrayRef<RegisterRangeKey> ranges,
                               llvm::ArrayRef<llvm::Value *> segments,
                               uint64_t readOffset, uint32_t readWidth,
                               llvm::Instruction *before, llvm::Twine name) {
    if (ranges.empty() || ranges.size() != segments.size() ||
        before == nullptr || readWidth == 0) {
      return nullptr;
    }
    llvm::Value *source = nullptr;
    uint32_t sourceWidth = 0;
    for (auto it : llvm::enumerate(ranges)) {
      llvm::Value *segment = resolve(segments[it.index()]);
      auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(segment);
      if (call == nullptr) {
        return nullptr;
      }
      std::optional<NativeRegisterValueExtractInfo> extract =
          parseNativeRegisterValueExtract(*call);
      if (!extract || extract->BitOffset != it.value().BitOffset ||
          extract->ReadWidth != it.value().BitWidth) {
        return nullptr;
      }
      llvm::Value *candidate = resolve(extract->FullValue);
      if (candidate == nullptr) {
        return nullptr;
      }
      if (source == nullptr) {
        source = candidate;
        sourceWidth = extract->FullWidth;
        continue;
      }
      if (source != candidate || sourceWidth != extract->FullWidth) {
        return nullptr;
      }
    }
    if (source == nullptr || readOffset + readWidth > sourceWidth) {
      return nullptr;
    }
    if (readOffset == 0 && readWidth == sourceWidth) {
      return source;
    }
    return extractBitsFromIntegerValue(source, readWidth, readOffset, before,
                                       name);
  }

bool FunctionBuilder::valueDominatesUse(llvm::Value *value, llvm::Instruction *use,
                         llvm::DominatorTree &domTree) const {
    value = resolve(value);
    if (value == nullptr || use == nullptr) {
      return false;
    }
    auto *inst = llvm::dyn_cast<llvm::Instruction>(value);
    if (inst == nullptr) {
      return true;
    }
    if (inst->getParent() == nullptr || use->getParent() == nullptr) {
      return false;
    }
    if (inst->getFunction() != use->getFunction()) {
      return false;
    }
    if (inst->getParent() == use->getParent()) {
      return inst->comesBefore(use);
    }
    return domTree.dominates(inst->getParent(), use->getParent());
  }

llvm::Value *FunctionBuilder::assembleRangeReadIfDominating(
      const std::vector<RegisterRangeKey> &ranges, uint64_t readOffset,
      uint32_t readWidth, llvm::Instruction *before, llvm::Twine name,
      llvm::DominatorTree &domTree, bool allowUnknownSegments ,
      bool entryFallback ) {
    if (ranges.empty() || before == nullptr || readWidth == 0) {
      return nullptr;
    }
    std::vector<llvm::Value *> segments;
    segments.reserve(ranges.size());
    for (const RegisterRangeKey &range : ranges) {
      llvm::Value *segment = resolve(
          readRangeBefore(*before->getParent(), range, before, domTree));
      if (entryFallback &&
          (segment == nullptr || segment->getType() != rangeType(range) ||
           !valueDominatesUse(segment, before, domTree))) {
        // 新准则（Arch2.md §13）：range 状态答不上来时退到函数入口值（裸读），
        // 不要直接落 unknown。入口闸门仍然生效（判为填充/junk 的位拿不到值）。
        segment = resolve(entryRangeInput(*before->getParent(), range));
      }
      if (segment == nullptr || segment->getType() != rangeType(range) ||
          !valueDominatesUse(segment, before, domTree)) {
        if (!allowUnknownSegments ||
            UnknownCurrentDef.count({before->getParent(), range}) != 0) {
          return nullptr;
        }
        segment = attributedRangeUnknown(*before, range, ".range_unknown");
        if (segment == nullptr || segment->getType() != rangeType(range) ||
            !valueDominatesUse(segment, before, domTree)) {
          return nullptr;
        }
      }
      segments.push_back(segment);
    }

    if (ranges.size() == 1 && ranges.front().BitOffset == readOffset &&
        ranges.front().BitWidth == readWidth) {
      return segments.front();
    }

    if (llvm::Value *common = tryCommonExtractSourceAccess(
            ranges, segments, readOffset, readWidth, before,
            name + ".common_extract")) {
      return common;
    }

    auto *resultType = llvm::IntegerType::get(Function.getContext(), readWidth);
    llvm::Value *result = llvm::ConstantInt::get(resultType, 0);
    for (auto it : llvm::enumerate(ranges)) {
      const RegisterRangeKey &range = it.value();
      result = insertBitsIntoInteger(
          result, segments[it.index()], range.BitWidth,
          range.BitOffset - readOffset, before, name + ".part_insert");
      if (result == nullptr) {
        return nullptr;
      }
    }
    return result;
  }

llvm::Value *
  FunctionBuilder::readAccessRangeIfDominating(llvm::GlobalVariable *global, uint64_t readOffset,
                              uint32_t readWidth, llvm::Instruction *before,
                              llvm::Twine name, llvm::DominatorTree &domTree,
                              bool allowUnknownSegments ) {
    if (llvm::Value *covered = tryCurrentCoveredAccess(
            global, readOffset, readWidth, before, name + ".covered")) {
      covered = resolve(covered);
      return covered != nullptr && valueDominatesUse(covered, before, domTree)
                 ? covered
                 : nullptr;
    }
    std::vector<RegisterRangeKey> ranges =
        plannedRangesCovering(global, readOffset, readWidth);
    return assembleRangeReadIfDominating(ranges, readOffset, readWidth, before,
                                         name, domTree, allowUnknownSegments);
  }

llvm::Value *FunctionBuilder::readFullRangeValueBefore(llvm::BasicBlock &block,
                                        const RegisterUnit &unit,
                                        llvm::Instruction *before,
                                        llvm::DominatorTree &domTree,
                                        bool allowUnknownSegments ) {
    (void)block;
    unsigned fullWidth = registerBitWidth(unit);
    llvm::Value *value = readAccessRangeIfDominating(
        unit.Global, 0, fullWidth, before,
        llvm::Twine(unit.Name) + ".full_range", domTree, allowUnknownSegments);
    value = resolve(value);
    if (value == nullptr || value->getType() != unit.Global->getValueType()) {
      return nullptr;
    }
    return value;
  }

bool FunctionBuilder::hasReplacementMetadata(const llvm::Instruction &inst) const {
    return inst.getMetadata("notdec.register.summary_ssa.replaced") != nullptr;
  }

void FunctionBuilder::removeReplacementSource(llvm::Value *target, llvm::Value *key) {
    auto sourcesIt = ReplacementSources.find(target);
    if (sourcesIt == ReplacementSources.end()) {
      return;
    }
    auto &keys = sourcesIt->second;
    keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
    if (keys.empty()) {
      ReplacementSources.erase(sourcesIt);
    }
  }

void FunctionBuilder::recordReplacement(llvm::Value *key, llvm::Value *target) {
    auto it = Replacement.find(key);
    if (it != Replacement.end()) {
      if (it->second == target) {
        return;
      }
      removeReplacementSource(it->second, key);
      it->second = target;
    } else {
      Replacement.emplace(key, target);
    }
    ReplacementSources[target].push_back(key);
  }

void FunctionBuilder::forgetReplacementValue(llvm::Value *value) {
    if (auto it = Replacement.find(value); it != Replacement.end()) {
      removeReplacementSource(it->second, value);
      Replacement.erase(it);
    }
    auto sourcesIt = ReplacementSources.find(value);
    if (sourcesIt == ReplacementSources.end()) {
      return;
    }
    std::vector<llvm::Value *> keys = std::move(sourcesIt->second);
    ReplacementSources.erase(sourcesIt);
    for (llvm::Value *key : keys) {
      Replacement.erase(key);
    }
  }

void FunctionBuilder::eraseInstructionFromParent(llvm::Instruction &inst) {
    forgetReplacementValue(&inst);
    inst.eraseFromParent();
  }

void FunctionBuilder::rewritePartialReads() {
    llvm::DominatorTree domTree(Function);
    for (llvm::WeakVH &handle : PartialReads) {
      auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(handle);
      if (call == nullptr) {
        continue;
      }
      if (call->getParent() == nullptr) {
        continue;
      }
      std::optional<NativeRegisterPartialReadInfo> partial =
          parseNativeRegisterPartialRead(*call);
      if (!partial) {
        continue;
      }
      auto unitIt = Units.find(partial->Global);
      if (unitIt == Units.end()) {
        continue;
      }
      llvm::Value *value = nullptr;
      value = readAccessRangeIfDominating(
          partial->Global, partial->BitOffset, partial->ReadWidth, call,
          unitIt->second.Name + ".partial_range", domTree,
          /*allowUnknownSegments=*/true);
      if (value == nullptr || value->getType() != call->getType()) {
        continue;
      }
      recordReplacement(call, value);
      call->replaceAllUsesWith(value);
      call->setMetadata("notdec.register.summary_ssa.replaced",
                        markerNode("true"));
      ReplacedPartialReads.push_back(call);
      ++Summary.LoadsReplaced;
    }
  }

void FunctionBuilder::rewriteLoads() {
    for (llvm::LoadInst *load : Loads) {
      RegisterAccess access = registerLoad(*load, Units);
      bool x87WindowShift = false;
      if (access.Unit == nullptr) {
        access = registerLoad(*load, Units,
                             RegisterReadOptions{/*IncludeX87WindowShift=*/true});
        x87WindowShift = access.Unit != nullptr;
      }
      if (access.Unit == nullptr || !access.IsStorageValue) {
        continue;
      }
      llvm::Value *value = nullptr;
      if (x87WindowShift) {
        // 搬移读先只看本块（不触发 entry/phi 解析、不造 unknown）；本块没有定义时
        // 再退回完整 range 解析，让跨块的已知值（phi）也能替换掉读 global 的形态。
        value = readBlockLocalValueBefore(*load, *access.Unit);
      }
      if (value == nullptr) {
        value = readValueBefore(*load->getParent(), *access.Unit, load);
      }
      value = resolve(value);
      if (value == nullptr || value == load ||
          value->getType() != load->getType()) {
        continue;
      }
      recordReplacement(load, value);
      load->replaceAllUsesWith(value);
      load->setMetadata("notdec.register.summary_ssa.replaced",
                        markerNode("true"));
      ReplacedLoads.push_back(load);
      ++Summary.LoadsReplaced;
    }
  }

void FunctionBuilder::removeDeadPartialReads() {
    llvm::SmallPtrSet<llvm::CallBase *, 16> candidates;
    for (llvm::WeakVH &handle : ReplacedPartialReads) {
      if (auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(handle)) {
        candidates.insert(call);
      }
    }
    for (llvm::WeakVH &handle : PartialReads) {
      if (auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(handle)) {
        candidates.insert(call);
      }
    }
    for (llvm::CallBase *call : candidates) {
      if (call->getParent() == nullptr || !call->use_empty()) {
        continue;
      }
      if (isRecordedCallArgValue(call) || isRecordedFunctionReturnValue(call)) {
        continue;
      }
      eraseInstructionFromParent(*call);
      ++Summary.DeadLoadsRemoved;
    }
  }

void FunctionBuilder::removeDeadFoldedZeroXors() {
    for (llvm::WeakVH &handle : FoldedZeroXors) {
      auto *op = llvm::dyn_cast_or_null<llvm::BinaryOperator>(handle);
      if (op == nullptr) {
        continue;
      }
      if (op->getParent() == nullptr || !op->use_empty()) {
        continue;
      }
      eraseInstructionFromParent(*op);
    }
    FoldedZeroXors.clear();
  }

void FunctionBuilder::removeDeadEntryReads() {
    std::vector<llvm::Instruction *> deadReads;
    for (llvm::Instruction &inst : llvm::instructions(Function)) {
      if (!inst.use_empty()) {
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        if (load->getMetadata("notdec.register.summary_ssa.entry") != nullptr) {
          deadReads.push_back(load);
        }
        continue;
      }
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr ||
          call->getMetadata("notdec.register.summary_ssa.range_entry") ==
              nullptr ||
          !parseNativeRegisterPartialRead(*call)) {
        continue;
      }
      deadReads.push_back(call);
    }

    for (llvm::Instruction *inst : deadReads) {
      if (inst->getParent() == nullptr || !inst->use_empty()) {
        continue;
      }
      if (isRecordedCallArgValue(inst) || isRecordedFunctionReturnValue(inst)) {
        continue;
      }
      eraseInstructionFromParent(*inst);
      ++Summary.DeadLoadsRemoved;
    }
  }

void FunctionBuilder::foldDuplicatePartialReadXors() {
    std::vector<llvm::BinaryOperator *> zeroXors;

    for (llvm::Instruction &inst : llvm::instructions(Function)) {
      auto *op = llvm::dyn_cast<llvm::BinaryOperator>(&inst);
      if (op == nullptr || op->getOpcode() != llvm::Instruction::Xor) {
        continue;
      }

      auto *lhs = llvm::dyn_cast<llvm::CallBase>(op->getOperand(0));
      auto *rhs = llvm::dyn_cast<llvm::CallBase>(op->getOperand(1));
      if (lhs == nullptr || rhs == nullptr || lhs == rhs ||
          lhs->getParent() != rhs->getParent() ||
          lhs->getParent() != op->getParent()) {
        continue;
      }

      std::optional<NativeRegisterPartialReadInfo> lhsRead =
          parseNativeRegisterPartialRead(*lhs);
      std::optional<NativeRegisterPartialReadInfo> rhsRead =
          parseNativeRegisterPartialRead(*rhs);
      if (!samePartialReadRange(lhsRead, rhsRead) ||
          hasInterveningWriteToPartialReadRange(*lhs, *rhs, *lhsRead)) {
        continue;
      }

      zeroXors.push_back(op);
    }

    for (llvm::BinaryOperator *op : zeroXors) {
      if (op->getParent() == nullptr) {
        continue;
      }
      auto *zero = llvm::ConstantInt::get(op->getType(), 0);
      recordReplacement(op, zero);
      op->replaceAllUsesWith(zero);
      FoldedZeroXors.push_back(op);
      ++Summary.LoadsReplaced;
    }
  }

bool FunctionBuilder::samePartialReadRange(
      const std::optional<NativeRegisterPartialReadInfo> &lhs,
      const std::optional<NativeRegisterPartialReadInfo> &rhs) {
    return lhs && rhs && lhs->Global == rhs->Global &&
           lhs->FullWidth == rhs->FullWidth &&
           lhs->ReadWidth == rhs->ReadWidth && lhs->BitOffset == rhs->BitOffset;
  }

bool FunctionBuilder::hasInterveningWriteToPartialReadRange(
      llvm::CallBase &first, llvm::CallBase &second,
      const NativeRegisterPartialReadInfo &read) const {
    bool afterFirst = false;
    uint64_t readEnd = read.BitOffset + read.ReadWidth;
    for (llvm::Instruction &inst : *first.getParent()) {
      if (&inst == &first) {
        afterFirst = true;
        continue;
      }
      if (&inst == &second) {
        return false;
      }
      if (!afterFirst) {
        continue;
      }

      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && access.Unit->Global == read.Global &&
            access.IsStorageValue) {
          return true;
        }
        continue;
      }

      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr) {
        continue;
      }
      if (parseNativeRegisterPartialRead(*call)) {
        continue;
      }
      if (std::optional<NativeRegisterPartialWriteInfo> write =
              parseNativeRegisterPartialWrite(*call)) {
        uint64_t writeEnd = write->BitOffset + write->WriteWidth;
        if (write->Global == read.Global && read.BitOffset < writeEnd &&
            write->BitOffset < readEnd) {
          return true;
        }
        continue;
      }
      if (isAnalyzableCall(*call)) {
        return true;
      }
    }
    return true;
  }

void FunctionBuilder::removeDeadReplacedLoads() {
    for (llvm::WeakVH &handle : ReplacedLoads) {
      auto *load = llvm::dyn_cast_or_null<llvm::LoadInst>(handle);
      if (load == nullptr) {
        continue;
      }
      if (load->getParent() == nullptr || !load->use_empty()) {
        continue;
      }
      if (isRecordedCallArgValue(load) || isRecordedFunctionReturnValue(load)) {
        continue;
      }
      eraseInstructionFromParent(*load);
      ++Summary.DeadLoadsRemoved;
    }
  }

void FunctionBuilder::invalidateRecordedValueCache() const { FunctionReturnCacheDirty = true; }


void FunctionBuilder::ensureFunctionReturnValueCache() const {
    if (!FunctionReturnCacheDirty) {
      return;
    }
    FunctionReturnCacheDirty = false;
    RecordedFunctionReturnValues.clear();
    auto returnsIt = SignatureState.FunctionReturns.find(&Function);
    if (returnsIt == SignatureState.FunctionReturns.end()) {
      return;
    }
    for (const auto &[ret, values] : returnsIt->second) {
      (void)ret;
      for (llvm::Value *retValue : values) {
        llvm::Value *value = retValue;
        llvm::SmallPtrSet<llvm::Value *, 8> seen;
        while (value != nullptr && seen.insert(value).second) {
          RecordedFunctionReturnValues.insert(value);
          auto it = Replacement.find(value);
          if (it == Replacement.end() || it->second == value) {
            break;
          }
          value = it->second;
        }
      }
    }
  }

bool FunctionBuilder::isRecordedCallArgStore(llvm::StoreInst *store) const {
    if (SignatureState.StoresToErase.count(store) != 0) {
      return true;
    }
    return SignatureState.CallArgRecordedStores.count(store) != 0;
  }

bool FunctionBuilder::isRecordedCallArgValue(llvm::Value *value) const {
    llvm::Value *resolved = resolve(value);
    // 模块级集合存的是原始值。resolve 的终点一定不再被改写，所以"跨函数的记录值
    // （resolve 恒等）"和"本函数自己记录、没被改写的值"都能直接命中。
    if (SignatureState.CallArgRecordedValues.count(resolved) != 0) {
      return true;
    }
    // 本函数记录、且被 Replacement 改写过的值：按解析结果再比一次。
    if (!LocalRecordedValuesLooked) {
      LocalRecordedValuesLooked = true;
      auto it = SignatureState.CallArgRecordedValuesByFunction.find(
          Function.getName().str());
      if (it != SignatureState.CallArgRecordedValuesByFunction.end()) {
        LocalRecordedValues = &it->second;
      }
    }
    if (LocalRecordedValues != nullptr) {
      for (llvm::Value *recorded : *LocalRecordedValues) {
        if (recorded != nullptr && resolve(recorded) == resolved) {
          return true;
        }
      }
    }
    return false;
  }

bool FunctionBuilder::isRecordedFunctionReturnValue(llvm::Value *value) const {
    auto shapeIt = SignatureState.Shapes.find(&Function);
    if (shapeIt == SignatureState.Shapes.end() ||
        shapeIt->second.Returns.empty() ||
        Function.getFunctionType() ==
            functionTypeForShape(Function.getContext(), shapeIt->second)) {
      return false;
    }
    ensureFunctionReturnValueCache();
    return RecordedFunctionReturnValues.count(resolve(value)) != 0;
  }

void FunctionBuilder::removeDeadStoresByLiveness() {
    std::map<llvm::BasicBlock *, LiveRegisterRanges> liveIn;
    std::map<llvm::BasicBlock *, LiveRegisterRanges> liveOut;
    std::vector<llvm::BasicBlock *> blocks;
    for (llvm::BasicBlock &block : Function) {
      blocks.push_back(&block);
    }

    bool changed = true;
    while (changed) {
      changed = false;
      for (auto blockIt = blocks.rbegin(); blockIt != blocks.rend();
           ++blockIt) {
        llvm::BasicBlock &block = **blockIt;
        LiveRegisterRanges out;
        for (llvm::BasicBlock *succ : llvm::successors(&block)) {
          auto succLive = liveIn.find(succ);
          if (succLive != liveIn.end()) {
            out.insert(succLive->second.begin(), succLive->second.end());
          }
        }
        // After signature rewrite, explicit function returns carry these
        // values.  Register globals should no longer stay live just because a
        // summary return register exists.
        if (!PostSignatureCleanup && llvm::succ_empty(&block)) {
          addExitLiveRegisters(out);
        }

        LiveRegisterRanges in = transferBlockLiveness(block, out);
        changed |= liveOut[&block] != out || liveIn[&block] != in;
        liveOut[&block] = std::move(out);
        liveIn[&block] = std::move(in);
      }
    }

    for (llvm::BasicBlock &block : Function) {
      auto outIt = liveOut.find(&block);
      LiveRegisterRanges live =
          outIt == liveOut.end() ? LiveRegisterRanges{} : outIt->second;
      eraseDeadStoresInBlock(block, live);
    }
  }

LiveRegisterRanges FunctionBuilder::transferBlockLiveness(llvm::BasicBlock &block,
                                           LiveRegisterRanges live) {
    for (auto it = block.rbegin(); it != block.rend(); ++it) {
      llvm::Instruction &inst = *it;
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        transferStoreLiveness(*store, live);
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        transferLoadLiveness(*load, live);
        continue;
      }
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (transferPartialReadLiveness(*call, live)) {
          continue;
        }
        if (transferPartialWriteLiveness(*call, live)) {
          continue;
        }
        transferCallLiveness(*call, live);
        continue;
      }
    }
    return live;
  }

void FunctionBuilder::eraseDeadStoresInBlock(llvm::BasicBlock &block,
                              LiveRegisterRanges live) {
    std::vector<llvm::StoreInst *> deadStores;
    std::vector<llvm::CallBase *> deadPartialWrites;
    for (auto it = block.rbegin(); it != block.rend(); ++it) {
      llvm::Instruction &inst = *it;
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && access.IsStorageValue &&
            !hasLiveWriteRange(live, *access.Unit) &&
            !isRecordedCallArgStore(store)) {
          deadStores.push_back(store);
        }
        transferStoreLiveness(*store, live);
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        transferLoadLiveness(*load, live);
        continue;
      }
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (transferPartialReadLiveness(*call, live)) {
          continue;
        }
        if (std::optional<NativeRegisterPartialWriteInfo> partial =
                parseNativeRegisterPartialWrite(*call)) {
          if (Units.count(partial->Global) != 0 &&
              !hasLivePartialRange(live, partial->Global, partial->BitOffset,
                                   partial->WriteWidth)) {
            deadPartialWrites.push_back(call);
          }
          transferPartialWriteLiveness(*call, live);
          continue;
        }
        transferCallLiveness(*call, live);
        continue;
      }
    }
    for (llvm::StoreInst *store : deadStores) {
      eraseInstructionFromParent(*store);
      ++Summary.DeadStoresRemoved;
    }
    for (llvm::CallBase *call : deadPartialWrites) {
      eraseDeadPartialWriteCall(call);
    }
  }

void FunctionBuilder::transferStoreLiveness(llvm::StoreInst &store,
                             LiveRegisterRanges &live) const {
    RegisterAccess access = registerStore(store, Units);
    if (access.Unit != nullptr && access.IsStorageValue) {
      eraseGlobalRanges(live, access.Unit->Global);
    }
  }

void FunctionBuilder::transferLoadLiveness(llvm::LoadInst &load,
                            LiveRegisterRanges &live) const {
    RegisterAccess access = registerLoad(load, Units);
    if (access.Unit != nullptr && access.IsStorageValue) {
      // Entry/replaced loads are SummarySSA scaffolding.  Only still-raw
      // register loads should keep earlier register stores live.
      if (load.getMetadata("notdec.register.summary_ssa.entry") != nullptr ||
          hasReplacementMetadata(load)) {
        return;
      }
      insertGlobalRanges(live, access.Unit->Global);
    }
  }

bool FunctionBuilder::transferPartialReadLiveness(llvm::CallBase &call,
                                   LiveRegisterRanges &live) const {
    std::optional<NativeRegisterPartialReadInfo> partial =
        parseNativeRegisterPartialRead(call);
    if (!partial || Units.count(partial->Global) == 0) {
      return false;
    }
    if (hasReplacementMetadata(call) ||
        call.getMetadata("notdec.register.summary_ssa.range_entry") !=
            nullptr) {
      return true;
    }
    insertPartialRanges(live, partial->Global, partial->BitOffset,
                        partial->ReadWidth);
    return true;
  }

bool FunctionBuilder::transferPartialWriteLiveness(llvm::CallBase &call,
                                    LiveRegisterRanges &live) const {
    std::optional<NativeRegisterPartialWriteInfo> partial =
        parseNativeRegisterPartialWrite(call);
    if (!partial || Units.count(partial->Global) == 0) {
      return false;
    }
    // The helper defines only the written bit range.  Untouched ranges remain
    // live, but the written range itself no longer needs an earlier definition.
    erasePartialRanges(live, partial->Global, partial->BitOffset,
                       partial->WriteWidth);
    return true;
  }

void FunctionBuilder::transferCallLiveness(llvm::CallBase &call,
                            LiveRegisterRanges &live) const {
    if (!isAnalyzableCall(call)) {
      return;
    }
    for (const auto &[global, unit] : Units) {
      CallRegisterEffect effect = callEffect(call, unit);
      if (effect == CallRegisterEffect::ReturnValue ||
          effect == CallRegisterEffect::Clobber) {
        eraseRegisterEffectRanges(live, unit);
      }
      if (callReadsRegister(call, unit)) {
        insertGlobalRanges(live, global);
      }
    }
  }

void FunctionBuilder::addExitLiveRegisters(LiveRegisterRanges &live) const {
    auto functionFacts = SummaryFacts.find(&Function);
    if (functionFacts == SummaryFacts.end()) {
      return;
    }
    for (const auto &[global, unit] : Units) {
      auto regIt = functionFacts->second.Registers.find(unit.Name);
      if (regIt == functionFacts->second.Registers.end()) {
        continue;
      }
      const SummaryRegisterFact &fact = regIt->second;
      if (fact.ExitDemand && fact.MayNonEntry) {
        // 返回需求只取"被本函数改过"（MayNonEntry）的位：ExitDemandMask
        // 可能包含写保留位，那些位不是返回值。两个 mask 来自 hex 文本，
        // 前导零丢失会让宽度不一致，统一按寄存器宽度对齐再求交集。
        unsigned width = registerBitWidth(unit);
        if (width == 0) {
          continue;
        }
        llvm::APInt returnedMask =
            fact.ExitDemandMask.zextOrTrunc(width) &
            fact.MayNonEntryMask.zextOrTrunc(width);
        if (returnedMask.getBitWidth() != 0 && !returnedMask.isZero()) {
          insertMaskRanges(live, global, returnedMask);
        }
      }
    }
  }

} // namespace notdec::bin2llvm
