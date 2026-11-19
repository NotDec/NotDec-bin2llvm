#include "RegisterSummaryInternal.h"

namespace notdec::bin2llvm {

bool isNativeFrameSlotPointer(llvm::Value *value) {
  value = value->stripPointerCasts();
  if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(value)) {
    return alloca->hasName() &&
           alloca->getName().starts_with("notdec_stack.native");
  }
  if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value)) {
    return isNativeFrameSlotPointer(gep->getPointerOperand());
  }
  return false;
}

bool frameSlotPointerIsNeverLoaded(llvm::Value *pointer) {
  pointer = pointer->stripPointerCasts();
  for (llvm::User *user : pointer->users()) {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(user)) {
      if (load->getPointerOperand()->stripPointerCasts() == pointer) {
        return false;
      }
      continue;
    }
    if (llvm::isa<llvm::GetElementPtrInst>(user) ||
        llvm::isa<llvm::CastInst>(user)) {
      return false;
    }
  }
  return true;
}

bool partialReadFeedsOnlyDeadFrameSlots(llvm::CallBase &call) {
  if (call.use_empty()) {
    return true;
  }
  bool sawStore = false;
  for (llvm::User *user : call.users()) {
    llvm::Value *value = user;
    while (auto *cast = llvm::dyn_cast<llvm::CastInst>(value)) {
      if (!cast->hasOneUse()) {
        break;
      }
      value = *cast->user_begin();
    }
    auto *store = llvm::dyn_cast<llvm::StoreInst>(value);
    if (store == nullptr) {
      return false;
    }
    llvm::Value *pointer = store->getPointerOperand()->stripPointerCasts();
    if (!isNativeFrameSlotPointer(pointer) ||
        !frameSlotPointerIsNeverLoaded(pointer)) {
      return false;
    }
    sawStore = true;
  }
  return sawStore;
}

std::optional<std::pair<llvm::AllocaInst *, int64_t>>
nativeFrameOrigin(llvm::Function &function) {
  llvm::AllocaInst *alloca = nullptr;
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &inst : block) {
      auto *candidate = llvm::dyn_cast<llvm::AllocaInst>(&inst);
      if (candidate == nullptr || !candidate->hasName() ||
          !candidate->getName().starts_with("notdec_stack.native")) {
        continue;
      }
      if (alloca != nullptr) {
        return std::nullopt;
      }
      alloca = candidate;
    }
  }
  if (alloca == nullptr) {
    return std::nullopt;
  }
  auto *array = llvm::dyn_cast<llvm::ArrayType>(alloca->getAllocatedType());
  if (array == nullptr || !array->getElementType()->isIntegerTy(8)) {
    return std::nullopt;
  }
  uint64_t frameSize = array->getNumElements();
  if (frameSize > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return std::nullopt;
  }
  return std::make_pair(alloca, -static_cast<int64_t>(frameSize));
}

std::optional<StackSlotKey>
entrySlotToNativeFrameKey(llvm::Function &function,
                          const StackSlotKey &entryKey) {
  std::optional<std::pair<llvm::AllocaInst *, int64_t>> frame =
      nativeFrameOrigin(function);
  if (!frame || entryKey.Offset < frame->second) {
    return std::nullopt;
  }
  int64_t allocaOffset = entryKey.Offset - frame->second;
  if (allocaOffset < 0) {
    return std::nullopt;
  }
  return StackSlotKey{frame->first, allocaOffset};
}

Cell defaultCell(llvm::GlobalVariable *global) {
  Cell cell;
  unsigned width = registerBitWidth(global);
  if (width != 0) {
    cell.MayEntry = llvm::APInt::getAllOnes(width);
    cell.MayNonEntry = llvm::APInt(width, 0);
    cell.ReadEntry = llvm::APInt(width, 0);
  }
  return cell;
}

std::map<std::string, llvm::GlobalVariable *>
registersByName(const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  std::map<std::string, llvm::GlobalVariable *> byName;
  for (const auto &[global, unit] : units) {
    byName.emplace(unit.Name, global);
  }
  return byName;
}

unsigned registerBitWidth(const RegisterUnit &unit) {
  llvm::Type *type = unit.Global->getValueType();
  if (auto *integer = llvm::dyn_cast<llvm::IntegerType>(type)) {
    return integer->getBitWidth();
  }
  if (type != nullptr && type->isSized()) {
    return type->getScalarSizeInBits();
  }
  return static_cast<unsigned>(unit.Size * 8);
}

llvm::APInt fullMask(const RegisterUnit &unit) {
  unsigned width = registerBitWidth(unit);
  if (width == 0) {
    return llvm::APInt();
  }
  return llvm::APInt::getAllOnes(width);
}

llvm::APInt storageMaskForUnit(const NativeAbiStorage &storage,
                               const RegisterUnit &unit, uint64_t sizeBytes) {
  unsigned width = registerBitWidth(unit);
  if (width == 0) {
    return llvm::APInt();
  }
  if (sizeBytes == 0) {
    return llvm::APInt::getAllOnes(width);
  }
  uint64_t offsetBits = 0;
  if (storage.Space.empty()) {
    // ABI aliases like XMM0_Qa are mapped to the lifted ZMM0 unit by name.
    // Cspec metadata often omits the storage space for those aliases, so treat
    // them as low-lane accesses instead of falling back to whole-ZMM demand.
    offsetBits = 0;
  } else if (storage.Space == unit.Space && storage.Offset >= unit.Offset &&
             storage.Offset < unit.Offset + unit.Size) {
    offsetBits = (storage.Offset - unit.Offset) * 8;
  } else {
    return llvm::APInt::getAllOnes(width);
  }
  uint64_t sizeBits = sizeBytes * 8;
  if (offsetBits >= width || sizeBits == 0) {
    return llvm::APInt(width, 0);
  }
  uint64_t count = std::min<uint64_t>(sizeBits, width - offsetBits);
  return llvm::APInt::getBitsSet(width, offsetBits, offsetBits + count);
}

std::string maskToHex(const llvm::APInt &mask) {
  if (mask.getBitWidth() == 0 || mask.isZero()) {
    return "";
  }
  llvm::SmallVector<char, 64> text;
  mask.toStringUnsigned(text, 16);
  return std::string(text.begin(), text.end());
}

bool maskNonZero(const llvm::APInt &mask) {
  return mask.getBitWidth() != 0 && !mask.isZero();
}

unsigned globalBitWidth(llvm::GlobalVariable *global) {
  if (global == nullptr) {
    return 0;
  }
  llvm::Type *type = global->getValueType();
  if (auto *integer = llvm::dyn_cast<llvm::IntegerType>(type)) {
    return integer->getBitWidth();
  }
  if (type != nullptr && type->isSized()) {
    return type->getScalarSizeInBits();
  }
  return 0;
}

bool addDemand(RegisterDemand &demand, llvm::GlobalVariable *global,
               llvm::APInt mask) {
  unsigned width = globalBitWidth(global);
  if (width != 0) {
    mask = mask.zextOrTrunc(width);
  }
  if (global == nullptr || mask.getBitWidth() == 0 || mask.isZero()) {
    return false;
  }
  auto it = demand.find(global);
  if (it == demand.end()) {
    demand.emplace(global, std::move(mask));
    return true;
  }
  if (it->second.getBitWidth() == 0) {
    it->second = std::move(mask);
    return true;
  }
  llvm::APInt old = it->second;
  it->second |= mask.zextOrTrunc(it->second.getBitWidth());
  return it->second != old;
}

void eraseDemand(RegisterDemand &demand, llvm::GlobalVariable *global) {
  demand.erase(global);
}

void eraseDemand(RegisterDemand &demand, llvm::GlobalVariable *global,
                 llvm::APInt mask) {
  auto it = demand.find(global);
  if (it == demand.end() || mask.getBitWidth() == 0 || mask.isZero()) {
    return;
  }
  mask = mask.zextOrTrunc(it->second.getBitWidth());
  it->second &= ~mask;
  if (it->second.isZero()) {
    demand.erase(it);
  }
}

llvm::APInt demandFor(const RegisterDemand &demand,
                      llvm::GlobalVariable *global) {
  auto it = demand.find(global);
  if (it == demand.end()) {
    return llvm::APInt();
  }
  return it->second;
}

bool mergeDemand(RegisterDemand &target, const RegisterDemand &source) {
  bool changed = false;
  for (const auto &[global, mask] : source) {
    changed |= addDemand(target, global, mask);
  }
  return changed;
}

void mergeNamedMask(std::map<std::string, llvm::APInt> &masks,
                    const std::string &name, const llvm::APInt &mask) {
  if (mask.getBitWidth() == 0 || mask.isZero()) {
    return;
  }
  auto it = masks.find(name);
  if (it == masks.end() || it->second.getBitWidth() == 0) {
    masks[name] = mask;
    return;
  }
  it->second |= mask.zextOrTrunc(it->second.getBitWidth());
}

llvm::APInt namedMaskOrFull(const std::map<std::string, llvm::APInt> &masks,
                            const RegisterUnit &unit) {
  auto it = masks.find(unit.Name);
  if (it != masks.end() && it->second.getBitWidth() != 0) {
    return it->second.zextOrTrunc(registerBitWidth(unit));
  }
  return fullMask(unit);
}

unsigned valueBitWidth(llvm::Value *value) {
  if (value == nullptr || value->getType() == nullptr ||
      !value->getType()->isSized()) {
    return 0;
  }
  if (auto *integer = llvm::dyn_cast<llvm::IntegerType>(value->getType())) {
    return integer->getBitWidth();
  }
  return value->getType()->getScalarSizeInBits();
}

llvm::APInt fullMaskForValue(llvm::Value *value) {
  unsigned width = valueBitWidth(value);
  if (width == 0) {
    return llvm::APInt();
  }
  return llvm::APInt::getAllOnes(width);
}

llvm::APInt lowBitsMask(unsigned width, unsigned bits) {
  if (width == 0 || bits == 0) {
    return llvm::APInt(width, 0);
  }
  return llvm::APInt::getLowBitsSet(width, std::min(width, bits));
}

llvm::APInt trimmedMask(const llvm::APInt &mask, unsigned width) {
  if (width == 0) {
    return llvm::APInt();
  }
  return mask.zextOrTrunc(width);
}

namespace {
// 寄存器坐标掩码（寄存器位 0 起）-> 写入值坐标（结果位 0 起）。
// partial_write 的 BitOffset 是写入位段在寄存器里的起点；值本身从 bit 0 起。
llvm::APInt observedValueMask(const llvm::APInt &registerMask,
                              uint64_t offsetBits, unsigned valueWidth) {
  if (valueWidth == 0) {
    return llvm::APInt();
  }
  if (offsetBits >= registerMask.getBitWidth()) {
    return llvm::APInt(valueWidth, 0);
  }
  return trimmedMask(registerMask.lshr(offsetBits), valueWidth);
}
} // namespace

std::map<llvm::Value *, llvm::APInt> computeValueDemands(
    llvm::Function &function,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const std::map<llvm::Instruction *, llvm::APInt> *observedWrites) {
  std::map<llvm::Value *, llvm::APInt> demands;
  std::vector<llvm::Value *> worklist;

  // 上一轮寄存器走查的反馈：这个寄存器写之后还有哪些位会被读。
  // nullptr = 第一轮（或该写没被记录），按满宽保守处理。
  auto observedWrite = [&](llvm::Instruction &inst) -> const llvm::APInt * {
    if (observedWrites == nullptr) {
      return nullptr;
    }
    auto it = observedWrites->find(&inst);
    return it == observedWrites->end() ? nullptr : &it->second;
  };

  auto enqueue = [&](llvm::Value *value, llvm::APInt demand) {
    unsigned width = valueBitWidth(value);
    if (value == nullptr || width == 0 || demand.getBitWidth() == 0) {
      return;
    }
    demand = trimmedMask(demand, width);
    if (demand.isZero()) {
      return;
    }
    auto it = demands.find(value);
    if (it == demands.end()) {
      demands.emplace(value, demand);
      worklist.push_back(value);
      return;
    }
    llvm::APInt merged =
        it->second | demand.zextOrTrunc(it->second.getBitWidth());
    if (merged != it->second) {
      it->second = merged;
      worklist.push_back(value);
    }
  };

  auto seed = [&](llvm::Value *value) {
    enqueue(value, fullMaskForValue(value));
  };

  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
      if (llvm::Value *value = ret->getReturnValue()) {
        seed(value);
      }
      continue;
    }
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
      if (parseNativeRegisterValueExtract(*call) ||
          parseNativeRegisterValueInsert(*call)) {
        continue;
      }
      if (parseNativeRegisterPartialRead(*call)) {
        continue;
      }
      if (std::optional<NativeRegisterPartialWriteInfo> partial =
              parseNativeRegisterPartialWrite(*call)) {
        // 写进寄存器的值：只有该寄存器后续真被读的位才需要它。第一轮没有
        // 反馈（observedWrite == nullptr）时按满宽 seed，作为定点迭代的上界。
        if (const llvm::APInt *observed = observedWrite(inst)) {
          enqueue(partial->Value,
                  observedValueMask(*observed, partial->BitOffset,
                                    valueBitWidth(partial->Value)));
        } else {
          enqueue(partial->Value, fullMaskForValue(partial->Value));
        }
        continue;
      }
      for (llvm::Use &arg : call->args()) {
        seed(arg.get());
      }
      continue;
    }
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      RegisterAccess access = registerStore(*store, units);
      llvm::Value *stored = store->getValueOperand();
      if (access.Unit == nullptr) {
        seed(stored);
        continue;
      }
      // 整寄存器写同理：寄存器层告诉我们写完之后哪些位还会被读（写之前
      // 被覆盖的旧需求不算）。第一轮按满宽。
      if (const llvm::APInt *observed = observedWrite(inst)) {
        enqueue(stored, trimmedMask(*observed, valueBitWidth(stored)));
      } else {
        seed(stored);
      }
      continue;
    }
    if (auto *branch = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
      if (!branch->isUnconditional()) {
        seed(branch->getCondition());
      }
      continue;
    }
    if (auto *sw = llvm::dyn_cast<llvm::SwitchInst>(&inst)) {
      seed(sw->getCondition());
      continue;
    }
  }

  while (!worklist.empty()) {
    llvm::Value *value = worklist.back();
    worklist.pop_back();
    llvm::APInt demand = demands[value];
    auto *inst = llvm::dyn_cast<llvm::Instruction>(value);
    if (inst == nullptr) {
      continue;
    }

    switch (inst->getOpcode()) {
    case llvm::Instruction::Trunc:
      enqueue(inst->getOperand(0), demand);
      break;
    case llvm::Instruction::ZExt:
      enqueue(inst->getOperand(0),
              trimmedMask(demand, valueBitWidth(inst->getOperand(0))));
      break;
    case llvm::Instruction::SExt: {
      unsigned srcWidth = valueBitWidth(inst->getOperand(0));
      llvm::APInt srcDemand = trimmedMask(demand, srcWidth);
      llvm::APInt high = demand & ~lowBitsMask(valueBitWidth(inst), srcWidth);
      if (!high.isZero() && srcWidth != 0) {
        srcDemand |= llvm::APInt::getSignMask(srcWidth);
      }
      enqueue(inst->getOperand(0), srcDemand);
      break;
    }
    case llvm::Instruction::BitCast:
    case llvm::Instruction::Freeze:
      enqueue(inst->getOperand(0), demand);
      break;
    case llvm::Instruction::PHI:
      for (llvm::Value *incoming : inst->operand_values()) {
        enqueue(incoming, demand);
      }
      break;
    case llvm::Instruction::Select:
      // 条件本身也是被观察的值：漏掉它，select 条件上的比较链就收不到需求。
      enqueue(inst->getOperand(0), llvm::APInt::getAllOnes(1));
      enqueue(inst->getOperand(1), demand);
      enqueue(inst->getOperand(2), demand);
      break;
    case llvm::Instruction::ICmp:
    case llvm::Instruction::FCmp:
      // 比较结果被需要（1 位）就意味着两个操作数的所有位都被需要；结果没有
      // 需求时不能 seed——否则死标志位的计算链会把寄存器读重新拉成全宽
      // （FUN_1d01c0 的 R11 高 24 位就是这么来的）。
      enqueue(inst->getOperand(0), fullMaskForValue(inst->getOperand(0)));
      enqueue(inst->getOperand(1), fullMaskForValue(inst->getOperand(1)));
      break;
    case llvm::Instruction::And:
      if (auto *lhs = llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(0))) {
        enqueue(inst->getOperand(1), demand & lhs->getValue());
      } else if (auto *rhs =
                     llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1))) {
        enqueue(inst->getOperand(0), demand & rhs->getValue());
      } else {
        enqueue(inst->getOperand(0), demand);
        enqueue(inst->getOperand(1), demand);
      }
      break;
    case llvm::Instruction::LShr:
      if (auto *shift =
              llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1))) {
        unsigned srcWidth = valueBitWidth(inst->getOperand(0));
        unsigned amount = shift->getLimitedValue();
        if (amount < srcWidth) {
          enqueue(inst->getOperand(0),
                  demand.shl(amount).zextOrTrunc(srcWidth));
        }
      } else {
        enqueue(inst->getOperand(0), demand);
      }
      break;
    case llvm::Instruction::Xor:
      // `xor x, x` is a pure zeroing idiom.  Treating it like a normal binary
      // op would make the cleared register look like a real entry-register
      // input, especially for XMM/ZMM lanes.
      if (inst->getOperand(0) != inst->getOperand(1)) {
        enqueue(inst->getOperand(0), demand);
        enqueue(inst->getOperand(1), demand);
      }
      break;
    default:
      for (llvm::Value *operand : inst->operand_values()) {
        enqueue(operand, demand);
      }
      break;
    }
  }
  return demands;
}

std::optional<std::pair<std::string, llvm::APInt>>
storageUnitMask(const NativeAbiStorage &storage, uint64_t sizeBytes,
                const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  std::string name = storageUnitName(storage, units);
  for (const auto &[global, unit] : units) {
    (void)global;
    if (unit.Name == name) {
      return std::make_pair(name, storageMaskForUnit(storage, unit, sizeBytes));
    }
  }
  return std::nullopt;
}

SummaryAbiFacts
collectSummaryAbiFacts(const llvm::Module &module,
                const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  SummaryAbiFacts facts;
  std::optional<NativeAbiSpec> abi = readNativeAbiMetadata(module);
  if (!abi) {
    return facts;
  }
  for (const NativeAbiParamEntry &entry : abi->Inputs) {
    if (entry.Storage.Kind == NativeAbiStorageKind::Register &&
        !entry.Storage.Name.empty()) {
      if (auto slot = storageUnitMask(entry.Storage, entry.MaxSize, units)) {
        facts.Inputs.insert(slot->first);
        mergeNamedMask(facts.InputMasks, slot->first, slot->second);
        if (std::find(facts.InputOrder.begin(), facts.InputOrder.end(),
                      slot->first) == facts.InputOrder.end()) {
          facts.InputOrder.push_back(slot->first);
        }
      }
    }
  }
  for (const NativeAbiParamEntry &entry : abi->Outputs) {
    if (entry.Storage.Kind == NativeAbiStorageKind::Register &&
        !entry.Storage.Name.empty()) {
      if (auto slot = storageUnitMask(entry.Storage, entry.MaxSize, units)) {
        facts.Outputs.insert(slot->first);
        mergeNamedMask(facts.OutputMasks, slot->first, slot->second);
        facts.OutputOrder.push_back(slot->first);
        if (entry.MetaType != "float") {
          facts.IntegerOutputOrder.push_back(slot->first);
        }
      }
    }
  }
  for (const NativeAbiEffect &effect : abi->Effects) {
    if (effect.Storage.Kind != NativeAbiStorageKind::Register ||
        effect.Storage.Name.empty()) {
      continue;
    }
    if (effect.Kind == NativeAbiEffectKind::Unaffected) {
      facts.Unaffected.insert(storageUnitName(effect.Storage, units));
    } else if (effect.Kind == NativeAbiEffectKind::KilledByCall) {
      if (auto slot = storageUnitMask(effect.Storage, 0, units)) {
        facts.KilledByCall.insert(slot->first);
        mergeNamedMask(facts.KilledByCallMasks, slot->first, slot->second);
      }
    }
  }
  // SysV 的 cspec 不写 x87 寄存器，但 long double 返回约定把结果留在 ST0
  // （80 位）。手工补进输出集合，让根函数 seed ExitDemand、调用点把 ST0
  // 当作返回/破坏寄存器处理；ST1 只是窗口内部状态，不参与 ABI。
  for (const auto &[global, unit] : units) {
    (void)global;
    if (unit.Name != "ST0") {
      continue;
    }
    facts.Outputs.insert("ST0");
    mergeNamedMask(facts.OutputMasks, "ST0", fullMask(unit));
    facts.OutputOrder.push_back("ST0");
    break;
  }
  facts.StackPointer = abi->StackPointerRegister;
  facts.StackShift = abi->StackShift;
  return facts;
}

Cell &cellFor(State &state, llvm::GlobalVariable *global) {
  auto [it, inserted] = state.Cells.try_emplace(global);
  if (inserted) {
    it->second = defaultCell(global);
  }
  return it->second;
}

Cell cellIn(const State &state, llvm::GlobalVariable *global) {
  auto it = state.Cells.find(global);
  if (it == state.Cells.end()) {
    return defaultCell(global);
  }
  return it->second;
}

Cell cellIn(const FunctionEffect &effect, llvm::GlobalVariable *global) {
  auto it = effect.Registers.find(global);
  if (it == effect.Registers.end()) {
    return defaultCell(global);
  }
  return it->second;
}

bool joinCell(Cell &lhs, const Cell &rhs) {
  Cell old = lhs;
  lhs.MayEntry |= rhs.MayEntry;
  lhs.MayNonEntry |= rhs.MayNonEntry;
  lhs.ReadEntry |= rhs.ReadEntry;
  return !(lhs == old);
}

void joinReadEntryOnly(std::map<llvm::GlobalVariable *, Cell> &target,
                       const State &source) {
  for (const auto &[global, cell] : source.Cells) {
    if (cell.ReadEntry.getBitWidth() == 0 || cell.ReadEntry.isZero()) {
      continue;
    }
    auto [it, inserted] = target.try_emplace(global, defaultCell(global));
    if (inserted) {
      it->second.ReadEntry = cell.ReadEntry;
    } else {
      it->second.ReadEntry |= cell.ReadEntry;
    }
  }
}

unsigned registerBitWidth(llvm::GlobalVariable *global) {
  if (global == nullptr) {
    return 0;
  }
  llvm::Type *type = global->getValueType();
  if (auto *integer = llvm::dyn_cast<llvm::IntegerType>(type)) {
    return integer->getBitWidth();
  }
  return type->isSized() ? type->getScalarSizeInBits() : 0;
}

RegisterOriginBits defaultOrigin(llvm::GlobalVariable *global) {
  unsigned width = registerBitWidth(global);
  if (width == 0) {
    return {llvm::APInt(), llvm::APInt(), llvm::APInt(), llvm::APInt()};
  }
  return {llvm::APInt::getAllOnes(width), llvm::APInt(width, 0),
          llvm::APInt(width, 0), llvm::APInt(width, 0)};
}

RegisterOriginBits originIn(const State &state, llvm::GlobalVariable *global) {
  auto it = state.Origins.find(global);
  return it == state.Origins.end() ? defaultOrigin(global) : it->second;
}

llvm::APInt registerRangeMask(llvm::GlobalVariable *global, unsigned offsetBits,
                              unsigned sizeBits) {
  unsigned width = registerBitWidth(global);
  if (width == 0 || offsetBits >= width) {
    return llvm::APInt();
  }
  unsigned boundedSize = sizeBits == 0 ? width - offsetBits
                                       : std::min(sizeBits, width - offsetBits);
  return llvm::APInt::getLowBitsSet(width, boundedSize).shl(offsetBits);
}

llvm::APInt fullRegisterMask(llvm::GlobalVariable *global) {
  unsigned width = registerBitWidth(global);
  return width == 0 ? llvm::APInt() : llvm::APInt::getAllOnes(width);
}

void setRegisterOrigin(State &state, llvm::GlobalVariable *global,
                       const llvm::APInt &mask, RegisterOriginKind kind) {
  unsigned width = registerBitWidth(global);
  if (width == 0 || mask.getBitWidth() == 0 || mask.isZero()) {
    return;
  }
  llvm::APInt boundedMask = mask.zextOrTrunc(width);
  RegisterOriginBits bits = originIn(state, global);
  bits.Entry &= ~boundedMask;
  bits.Local &= ~boundedMask;
  bits.CallClobber &= ~boundedMask;
  if (kind == RegisterOriginKind::Entry) {
    bits.Entry |= boundedMask;
  } else if (kind == RegisterOriginKind::Local) {
    bits.Local |= boundedMask;
  } else if (kind == RegisterOriginKind::CallClobber) {
    bits.CallClobber |= boundedMask;
  }
  state.Origins[global] = std::move(bits);
}

void setFullRegisterOrigin(State &state, llvm::GlobalVariable *global,
                           RegisterOriginKind kind) {
  unsigned width = registerBitWidth(global);
  if (width != 0) {
    setRegisterOrigin(state, global, llvm::APInt::getAllOnes(width), kind);
  }
}

bool hasFloatWriteMetadata(const llvm::Instruction &inst) {
  return inst.getMetadata("notdec.register.float_write") != nullptr;
}

void recordRegisterFloatWrite(State &state, llvm::GlobalVariable *global,
                              const llvm::APInt &mask, bool isFloatWrite) {
  unsigned width = registerBitWidth(global);
  if (width == 0 || mask.getBitWidth() == 0 || mask.isZero()) {
    return;
  }
  llvm::APInt boundedMask = mask.zextOrTrunc(width);
  RegisterOriginBits bits = originIn(state, global);
  if (isFloatWrite) {
    bits.FloatLocal |= boundedMask;
  } else {
    bits.FloatLocal &= ~boundedMask;
  }
  state.Origins[global] = std::move(bits);
}

bool joinState(State &target, const State &source) {
  if (!source.Reachable) {
    return false;
  }
  if (!target.Reachable) {
    target = source;
    return true;
  }
  bool changed = false;
  for (const auto &[global, cell] : source.Cells) {
    changed |= joinCell(cellFor(target, global), cell);
  }
  // Sparse maps treat missing cells as untouched. If target already contains a
  // register and source omits it, the join still has to include that default
  // untouched path; otherwise predecessor order would change the result.
  for (auto &[global, cell] : target.Cells) {
    if (source.Cells.count(global) == 0) {
      changed |= joinCell(cell, defaultCell(global));
    }
  }
  std::set<llvm::GlobalVariable *> originGlobals;
  for (const auto &[global, bits] : target.Origins) {
    (void)bits;
    originGlobals.insert(global);
  }
  for (const auto &[global, bits] : source.Origins) {
    (void)bits;
    originGlobals.insert(global);
  }
  for (llvm::GlobalVariable *global : originGlobals) {
    RegisterOriginBits before = originIn(target, global);
    RegisterOriginBits incoming = originIn(source, global);
    RegisterOriginBits joined = {
        before.Entry & incoming.Entry,
        before.Local & incoming.Local,
        before.FloatLocal & incoming.FloatLocal,
        before.CallClobber & incoming.CallClobber,
    };
    if (!(joined == before)) {
      target.Origins[global] = std::move(joined);
      changed = true;
    }
  }
  for (auto it = target.StackSlots.begin(); it != target.StackSlots.end();) {
    auto sourceIt = source.StackSlots.find(it->first);
    if (sourceIt == source.StackSlots.end() || sourceIt->second != it->second) {
      it = target.StackSlots.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  for (auto it = target.StackSlotOrigins.begin();
       it != target.StackSlotOrigins.end();) {
    auto sourceIt = source.StackSlotOrigins.find(it->first);
    if (sourceIt == source.StackSlotOrigins.end() ||
        sourceIt->second != it->second) {
      it = target.StackSlotOrigins.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  for (auto it = target.StackSlotStores.begin();
       it != target.StackSlotStores.end();) {
    auto sourceIt = source.StackSlotStores.find(it->first);
    if (sourceIt == source.StackSlotStores.end() ||
        !(sourceIt->second == it->second)) {
      it = target.StackSlotStores.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  for (auto it = target.ValueOrigins.begin();
       it != target.ValueOrigins.end();) {
    auto sourceIt = source.ValueOrigins.find(it->first);
    if (sourceIt == source.ValueOrigins.end() ||
        sourceIt->second != it->second) {
      it = target.ValueOrigins.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  return changed;
}

void readRegister(State &state, llvm::GlobalVariable *global,
                  const llvm::APInt &readMask) {
  if (readMask.getBitWidth() == 0 || readMask.isZero()) {
    return;
  }
  Cell &cell = cellFor(state, global);
  llvm::APInt bounded = readMask.zextOrTrunc(cell.MayEntry.getBitWidth());
  cell.ReadEntry |= (cell.MayEntry & bounded);
}

void writeRegister(State &state, llvm::GlobalVariable *global,
                   const llvm::APInt &writeMask) {
  if (writeMask.getBitWidth() == 0 || writeMask.isZero()) {
    return;
  }
  Cell &cell = cellFor(state, global);
  llvm::APInt bounded = writeMask.zextOrTrunc(cell.MayEntry.getBitWidth());
  cell.MayEntry &= ~bounded;
  cell.MayNonEntry |= bounded;
}

bool isX64GprName(llvm::StringRef name) {
  return name == "RAX" || name == "RBX" || name == "RCX" || name == "RDX" ||
         name == "RSI" || name == "RDI" || name == "RBP" || name == "RSP" ||
         name == "R8" || name == "R9" || name == "R10" || name == "R11" ||
         name == "R12" || name == "R13" || name == "R14" || name == "R15";
}

bool isX64Low32GprWrite(const NativeRegisterPartialWriteInfo &partial,
                        const RegisterUnit &unit) {
  // In x86-64, writing an E* register clears the high 32 bits of the matching
  // R* register.  Treat this as a full non-entry definition for the coarse
  // whole-register summary; otherwise zeroing idioms keep a fake entry high
  // half alive and can turn callee-saved registers into function parameters.
  return partial.FullWidth == 64 && partial.WriteWidth == 32 &&
         partial.BitOffset == 0 && registerBitWidth(unit) == 64 &&
         isX64GprName(unit.Name);
}

void restoreRegister(State &state, llvm::GlobalVariable *global,
                     const llvm::APInt &restoreMask) {
  if (restoreMask.getBitWidth() == 0 || restoreMask.isZero()) {
    return;
  }
  Cell &cell = cellFor(state, global);
  cell.MayEntry |= restoreMask;
  cell.MayNonEntry &= ~restoreMask;
}

bool isKeepHighPartialStoreValue(llvm::Value *value,
                                 llvm::GlobalVariable *global) {
  // RegisterStorage lowers a partial write as:
  //   old = load @REG
  //   keep = and old, KEEP_MASK
  //   insert = and (shl new, shift), WRITE_MASK
  //   store (or keep, insert), @REG
  // The old load is not a semantic function input if it only preserves lanes
  // outside the partial write.
  auto *orOp = llvm::dyn_cast<llvm::Operator>(value);
  if (orOp == nullptr || orOp->getOpcode() != llvm::Instruction::Or) {
    return false;
  }
  for (llvm::Value *operand : orOp->operands()) {
    auto *andOp = llvm::dyn_cast<llvm::Operator>(operand);
    if (andOp == nullptr || andOp->getOpcode() != llvm::Instruction::And) {
      continue;
    }
    for (llvm::Value *andOperand : andOp->operands()) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(andOperand);
      if (load != nullptr &&
          load->getPointerOperand()->stripPointerCasts() == global) {
        return true;
      }
    }
  }
  return false;
}

bool isKeepHighPartialLoadUse(llvm::LoadInst &load,
                              llvm::GlobalVariable *global) {
  if (load.getPointerOperand()->stripPointerCasts() != global ||
      load.user_empty()) {
    return false;
  }
  for (llvm::User *loadUser : load.users()) {
    auto *andOp = llvm::dyn_cast<llvm::Operator>(loadUser);
    if (andOp == nullptr || andOp->getOpcode() != llvm::Instruction::And) {
      return false;
    }
    bool hasStore = false;
    for (llvm::User *andUser : andOp->users()) {
      auto *orOp = llvm::dyn_cast<llvm::Operator>(andUser);
      if (orOp == nullptr || orOp->getOpcode() != llvm::Instruction::Or) {
        return false;
      }
      for (llvm::User *orUser : orOp->users()) {
        auto *store = llvm::dyn_cast<llvm::StoreInst>(orUser);
        if (store == nullptr ||
            store->getPointerOperand()->stripPointerCasts() != global ||
            store->getValueOperand() != orOp) {
          return false;
        }
        hasStore = true;
      }
    }
    if (!hasStore) {
      return false;
    }
  }
  return true;
}

bool isIgnored(const RegisterUnit &unit,
               const NativeRegisterSummaryOptions &options) {
  return options.IgnoredRegisters.count(unit.Name) != 0;
}

} // namespace notdec::bin2llvm
