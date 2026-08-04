#include "notdec-bin2llvm/passes/summary/NativeRegisterSummary.h"

#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/NativeExternalPrototype.h"
#include "notdec-bin2llvm/NativeX87Intrinsic.h"
#include "notdec-bin2llvm/NativeRegisterPartialRead.h"
#include "notdec-bin2llvm/NativeRegisterPartialWrite.h"
#include "notdec-bin2llvm/NativeRegisterValueRange.h"
#include "notdec-bin2llvm/passes/summary/NativeStackAddress.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace notdec::bin2llvm {
namespace {

struct RegisterUnit {
  llvm::GlobalVariable *Global = nullptr;
  std::string Space;
  std::string Name;
  uint64_t Offset = 0;
  uint64_t Size = 0;
};

struct RegisterAccess {
  const RegisterUnit *Unit = nullptr;
};

RegisterAccess
registerStore(llvm::StoreInst &store,
              const std::map<llvm::GlobalVariable *, RegisterUnit> &units);

struct StackSlotKey {
  llvm::Value *Base = nullptr;
  int64_t Offset = 0;

  bool operator<(const StackSlotKey &other) const {
    return std::tie(Base, Offset) < std::tie(other.Base, other.Offset);
  }

  bool operator==(const StackSlotKey &other) const {
    return Base == other.Base && Offset == other.Offset;
  }
};

// The stack frame rewrite folds outgoing call arguments into the same
// notdec_stack.native alloca as locals.  Stores before a call are therefore
// recorded under alloca-relative offsets, while cspec slot math starts from the
// entry-SP offset.  This helper translates between the two coordinate spaces;
// the alloca size is exactly -frameLow by construction, so no extra metadata is
// needed.
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

// Convert an entry-SP relative slot into the equivalent alloca-relative slot,
// or return nullopt when the function has no rewritten native frame.
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

struct ValueOrigin {
  llvm::GlobalVariable *Base = nullptr;
  int64_t Offset = 0;

  bool operator==(const ValueOrigin &other) const {
    return Base == other.Base && Offset == other.Offset;
  }

  bool operator!=(const ValueOrigin &other) const { return !(*this == other); }
};

// Forward abstract state for one backing register. Missing map entries use this
// default value, but only inside a reachable block state.
struct Cell {
  bool MayEntry = true;
  bool MayNonEntry = false;
  bool ReadEntry = false;

  bool operator==(const Cell &other) const {
    return MayEntry == other.MayEntry && MayNonEntry == other.MayNonEntry &&
           ReadEntry == other.ReadEntry;
  }
};

// Callsite inference needs to distinguish explicit argument definitions from
// entry forwarding and previous-call clobbers.  Keep that information beside
// the existing Cell domain so it cannot change the summary lattice itself.
struct RegisterOriginBits {
  llvm::APInt Entry;
  llvm::APInt Local;
  llvm::APInt CallClobber;

  bool operator==(const RegisterOriginBits &other) const {
    return Entry == other.Entry && Local == other.Local &&
           CallClobber == other.CallClobber;
  }
};

struct State {
  bool Reachable = false;
  bool EndsInNoReturn = false;
  std::map<llvm::GlobalVariable *, Cell> Cells;
  std::map<llvm::GlobalVariable *, RegisterOriginBits> Origins;
  // Narrow frame-local model for saved-register recognition.  It only tracks
  // fixed entry-SP offsets or notdec_stack.native alloca offsets, not arbitrary
  // memory.
  std::map<StackSlotKey, llvm::GlobalVariable *> StackSlots;
  // Caller stack-argument evidence is separate from saved-register tracking.
  // Values are intentionally coarse: the summary only needs to know whether a
  // cspec stack slot was explicitly written before a call.
  std::map<StackSlotKey, NativeRegisterCallsiteValueOrigin> StackSlotOrigins;
  // The last store that wrote each evidence slot, kept beside
  // StackSlotOrigins so callsite evidence can widen a stack vararg slot when
  // the ABI grid (4 bytes on i386) is narrower than the actual store (an
  // 8-byte fstpl double).
  struct StackSlotStoreInfo {
    uint32_t SizeBytes = 0;
    bool IsFloat = false;

    bool operator==(const StackSlotStoreInfo &other) const {
      return SizeBytes == other.SizeBytes && IsFloat == other.IsFloat;
    }
  };
  std::map<StackSlotKey, StackSlotStoreInfo> StackSlotStores;
  // SSA values known to be a function-entry register plus a constant offset.
  // Offset 0 is also used for saved register values loaded back from stack.
  std::map<llvm::Value *, ValueOrigin> ValueOrigins;
  bool operator==(const State &other) const {
    return Reachable == other.Reachable &&
           EndsInNoReturn == other.EndsInNoReturn && Cells == other.Cells &&
           Origins == other.Origins && StackSlots == other.StackSlots &&
           StackSlotOrigins == other.StackSlotOrigins &&
           StackSlotStores == other.StackSlotStores &&
           ValueOrigins == other.ValueOrigins;
  }

  bool sameSummaryLattice(const State &other) const {
    return Reachable == other.Reachable &&
           EndsInNoReturn == other.EndsInNoReturn && Cells == other.Cells;
  }
};

// Bottom-up summary: the CFG-level effect visible at normal function exits.
struct FunctionEffect {
  std::map<llvm::GlobalVariable *, Cell> Registers;
  bool NoReturn = false;

  bool operator==(const FunctionEffect &other) const {
    return Registers == other.Registers && NoReturn == other.NoReturn;
  }
};

struct FunctionFlow {
  std::map<llvm::BasicBlock *, State> In;
  std::map<llvm::BasicBlock *, State> Out;
  FunctionEffect Effect;
};

// Top-down summary: which changed exit registers are actually observed by
// callers, plus an audit value for entry registers that feed those
// observations.
struct FunctionDemand {
  std::map<llvm::GlobalVariable *, llvm::APInt> ExitDemand;
  std::map<llvm::GlobalVariable *, llvm::APInt> EntryDemand;

  bool operator==(const FunctionDemand &other) const {
    return ExitDemand == other.ExitDemand && EntryDemand == other.EntryDemand;
  }
};

// ABI fallback used for external and indirect calls. OutputOrder keeps the
// prototype order so root demand can seed only the regular first return value.
struct AbiFacts {
  std::set<std::string> Inputs;
  std::map<std::string, llvm::APInt> InputMasks;
  std::vector<std::string> InputOrder;
  std::set<std::string> Outputs;
  std::map<std::string, llvm::APInt> OutputMasks;
  std::vector<std::string> OutputOrder;
  std::vector<std::string> IntegerOutputOrder;
  std::set<std::string> Unaffected;
  std::set<std::string> KilledByCall;
  std::map<std::string, llvm::APInt> KilledByCallMasks;
  std::string StackPointer;
  uint64_t StackShift = 0;
};

using RegisterDemand = std::map<llvm::GlobalVariable *, llvm::APInt>;

std::optional<std::string> mdField(const llvm::MDNode *node,
                                   llvm::StringRef key) {
  if (node == nullptr) {
    return std::nullopt;
  }
  std::string prefix = (key + "=").str();
  for (const llvm::MDOperand &operand : node->operands()) {
    auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
    if (text == nullptr) {
      continue;
    }
    llvm::StringRef value = text->getString();
    if (value.starts_with(prefix)) {
      return value.drop_front(prefix.size()).str();
    }
  }
  return std::nullopt;
}

std::string unitName(const llvm::GlobalVariable &global) {
  if (auto name = mdField(global.getMetadata("notdec.register"), "name")) {
    if (!name->empty()) {
      return *name;
    }
  }
  return global.getName().str();
}

std::string unitSpace(const llvm::GlobalVariable &global) {
  if (auto space = mdField(global.getMetadata("notdec.register"), "space")) {
    return *space;
  }
  return "";
}

uint64_t unitOffset(const llvm::GlobalVariable &global) {
  if (auto offset = mdField(global.getMetadata("notdec.register"), "offset")) {
    uint64_t value = 0;
    if (!llvm::StringRef(*offset).getAsInteger(10, value)) {
      return value;
    }
  }
  return 0;
}

uint64_t unitSize(const llvm::GlobalVariable &global) {
  if (auto size = mdField(global.getMetadata("notdec.register"), "size")) {
    uint64_t value = 0;
    if (!llvm::StringRef(*size).getAsInteger(10, value)) {
      return value;
    }
  }
  return 0;
}

std::map<llvm::GlobalVariable *, RegisterUnit>
collectRegisterUnits(llvm::Module &module) {
  std::map<llvm::GlobalVariable *, RegisterUnit> units;
  for (llvm::GlobalVariable &global : module.globals()) {
    if (global.getMetadata("notdec.register") == nullptr) {
      continue;
    }
    RegisterUnit unit;
    unit.Global = &global;
    unit.Space = unitSpace(global);
    unit.Name = unitName(global);
    unit.Offset = unitOffset(global);
    unit.Size = unitSize(global);
    units.emplace(&global, std::move(unit));
  }
  return units;
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
  if (mask.getBitWidth() == 0) {
    return "";
  }
  llvm::SmallVector<char, 64> text;
  mask.toStringUnsigned(text, 16);
  return std::string(text.begin(), text.end());
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

bool isSegmentBaseUnit(llvm::StringRef name) {
  return name == "FS_OFFSET" || name == "GS_OFFSET";
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

std::map<llvm::Value *, llvm::APInt> computeValueDemands(
    llvm::Function &function,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  std::map<llvm::Value *, llvm::APInt> demands;
  std::vector<llvm::Value *> worklist;

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
        enqueue(partial->Value, fullMaskForValue(partial->Value));
        continue;
      }
      for (llvm::Use &arg : call->args()) {
        seed(arg.get());
      }
      continue;
    }
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      RegisterAccess access = registerStore(*store, units);
      if (access.Unit == nullptr) {
        seed(store->getValueOperand());
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
    if (auto *icmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
      seed(icmp->getOperand(0));
      seed(icmp->getOperand(1));
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
      enqueue(inst->getOperand(1), demand);
      enqueue(inst->getOperand(2), demand);
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

std::string
storageUnitName(const NativeAbiStorage &storage,
                const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  // ABI records may mention partial names such as XMM0_Qa while lifting keeps
  // only the largest overlapping register global such as ZMM0.
  for (const auto &[global, unit] : units) {
    (void)global;
    if (unit.Name == storage.Name) {
      return unit.Name;
    }
  }
  for (llvm::StringRef prefix :
       {llvm::StringRef("XMM"), llvm::StringRef("YMM")}) {
    llvm::StringRef name(storage.Name);
    if (!name.starts_with(prefix)) {
      continue;
    }
    llvm::StringRef rest = name.drop_front(prefix.size());
    size_t digits = 0;
    while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9') {
      ++digits;
    }
    if (digits == 0) {
      continue;
    }
    std::string candidate = ("ZMM" + rest.take_front(digits)).str();
    for (const auto &[global, unit] : units) {
      (void)global;
      if (unit.Name == candidate) {
        return unit.Name;
      }
    }
  }
  for (const auto &[global, unit] : units) {
    (void)global;
    if (unit.Space == storage.Space && unit.Offset <= storage.Offset &&
        storage.Offset < unit.Offset + unit.Size) {
      return unit.Name;
    }
  }
  return storage.Name;
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

RegisterAccess
registerLoad(llvm::LoadInst &load,
             const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      load.getPointerOperand()->stripPointerCasts());
  if (global == nullptr) {
    return {};
  }
  auto it = units.find(global);
  if (it == units.end()) {
    return {};
  }
  if (load.getMetadata("notdec.register.access") == nullptr &&
      global->getMetadata("notdec.register") == nullptr) {
    return {};
  }
  return RegisterAccess{&it->second};
}

RegisterAccess
registerStore(llvm::StoreInst &store,
              const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      store.getPointerOperand()->stripPointerCasts());
  if (global == nullptr) {
    return {};
  }
  auto it = units.find(global);
  if (it == units.end()) {
    return {};
  }
  if (store.getMetadata("notdec.register.access") == nullptr &&
      global->getMetadata("notdec.register") == nullptr) {
    return {};
  }
  return RegisterAccess{&it->second};
}

bool isNotDecRegisterHelperCall(const llvm::CallBase &call) {
  llvm::Function *callee = call.getCalledFunction();
  return callee != nullptr &&
         (callee->getName().starts_with("notdec.register.") ||
          // x87 instructions are folded into library-style calls: they own the
          // FPU stack and touch no general register, so no ABI clobber/input
          // should be derived for them.
          isNativeX87IntrinsicName(callee->getName()) ||
          isNativeRegisterValueRangeName(callee->getName()));
}

bool isAnalyzableCall(const llvm::Instruction &inst) {
  auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
  if (call == nullptr || isNotDecRegisterHelperCall(*call) ||
      parseNativeRegisterPartialRead(*call).has_value() ||
      parseNativeRegisterPartialWrite(*call).has_value()) {
    return false;
  }
  llvm::Function *callee = call->getCalledFunction();
  return callee == nullptr || !callee->isIntrinsic();
}

AbiFacts
collectAbiFacts(const llvm::Module &module,
                const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  AbiFacts facts;
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
  facts.StackPointer = abi->StackPointerRegister;
  facts.StackShift = abi->StackShift;
  return facts;
}

Cell &cellFor(State &state, llvm::GlobalVariable *global) {
  return state.Cells[global];
}

Cell cellIn(const State &state, llvm::GlobalVariable *global) {
  auto it = state.Cells.find(global);
  if (it == state.Cells.end()) {
    return Cell{};
  }
  return it->second;
}

Cell cellIn(const FunctionEffect &effect, llvm::GlobalVariable *global) {
  auto it = effect.Registers.find(global);
  if (it == effect.Registers.end()) {
    return Cell{};
  }
  return it->second;
}

bool joinCell(Cell &lhs, const Cell &rhs) {
  Cell old = lhs;
  lhs.MayEntry = lhs.MayEntry || rhs.MayEntry;
  lhs.MayNonEntry = lhs.MayNonEntry || rhs.MayNonEntry;
  lhs.ReadEntry = lhs.ReadEntry || rhs.ReadEntry;
  return !(lhs == old);
}

void joinReadEntryOnly(std::map<llvm::GlobalVariable *, Cell> &target,
                       const State &source) {
  for (const auto &[global, cell] : source.Cells) {
    if (cell.ReadEntry) {
      target[global].ReadEntry = true;
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
    return {llvm::APInt(), llvm::APInt(), llvm::APInt()};
  }
  return {llvm::APInt::getAllOnes(width), llvm::APInt(width, 0),
          llvm::APInt(width, 0)};
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

enum class RegisterOriginKind {
  Entry,
  Local,
  CallClobber,
  Mixed,
};

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
    changed |= joinCell(target.Cells[global], cell);
  }
  // Sparse maps treat missing cells as untouched. If target already contains a
  // register and source omits it, the join still has to include that default
  // untouched path; otherwise predecessor order would change the result.
  for (auto &[global, cell] : target.Cells) {
    if (source.Cells.count(global) == 0) {
      changed |= joinCell(cell, Cell{});
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

void readRegister(State &state, llvm::GlobalVariable *global) {
  Cell &cell = cellFor(state, global);
  cell.ReadEntry = cell.ReadEntry || cell.MayEntry;
}

void writeRegister(State &state, llvm::GlobalVariable *global) {
  Cell &cell = cellFor(state, global);
  cell.MayEntry = false;
  cell.MayNonEntry = true;
}

void partialWriteRegister(State &state, llvm::GlobalVariable *global) {
  // A partial write replaces only some bits.  At the current whole-register
  // summary granularity, the untouched bits may still be the entry value, so do
  // not kill MayEntry.  The call itself is still a non-entry definition.
  Cell &cell = cellFor(state, global);
  cell.MayNonEntry = true;
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

void restoreRegister(State &state, llvm::GlobalVariable *global) {
  Cell &cell = cellFor(state, global);
  cell.MayEntry = true;
  cell.MayNonEntry = false;
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

class Analyzer {
public:
  Analyzer(llvm::Module &module, NativeRegisterSummaryOptions options)
      : Module(module), Options(std::move(options)),
        Units(collectRegisterUnits(module)),
        UnitsByName(registersByName(Units)),
        Abi(collectAbiFacts(module, Units)) {
    for (llvm::Function &function : Module) {
      if (!function.isDeclaration()) {
        Functions.push_back(&function);
      }
    }
    buildCallGraph();
    collectCrossBlockValues();
  }

  NativeRegisterSummary run() {
    runBottomUp();
    if (Options.CollectExternalCallsiteEvidence) {
      collectExternalCallsiteEvidence();
    }
    if (Options.RunTopDownDemand) {
      runTopDownDemand();
    }
    if (Options.AttachMetadata) {
      attachMetadata();
    }
    return buildPublicSummary();
  }

private:
  llvm::Module &Module;
  NativeRegisterSummaryOptions Options;
  std::map<llvm::GlobalVariable *, RegisterUnit> Units;
  std::map<std::string, llvm::GlobalVariable *> UnitsByName;
  AbiFacts Abi;
  std::vector<llvm::Function *> Functions;
  std::map<llvm::Function *, std::set<llvm::Function *>> Calls;
  std::map<llvm::Function *, std::set<llvm::Function *>> Callers;
  std::map<llvm::Function *, FunctionEffect> Effects;
  std::map<llvm::Function *, FunctionDemand> Demands;
  // ValueOrigins are forward facts for LLVM SSA values. A value defined and
  // fully used inside one basic block is already consumed during
  // transferBlock(); carrying it around loops only slows convergence.
  std::set<llvm::Value *> CrossBlockValues;
  // The last SCC iteration is already solved against stable callee effects.
  // Keep those block states only for the preliminary callsite-evidence pass.
  std::map<llvm::Function *, FunctionFlow> StableFlows;
  std::vector<NativeRegisterExternalCallsite> ExternalCallsites;
  // Stack coordinates are independent from register-value flow.  Cache one
  // immutable analysis per function so the preliminary summary and its
  // callsite evidence use the same ABI-facing address model as SummarySSA.
  mutable std::map<llvm::Function *,
                   std::unique_ptr<NativeStackAddressAnalysis>>
      StackAddressAnalyses;

  void buildCallGraph() {
    std::set<llvm::Function *> defined(Functions.begin(), Functions.end());
    for (llvm::Function *function : Functions) {
      for (llvm::BasicBlock &block : *function) {
        for (llvm::Instruction &inst : block) {
          auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
          if (call == nullptr || isNotDecRegisterHelperCall(*call)) {
            continue;
          }
          llvm::Function *callee = call->getCalledFunction();
          if (callee == nullptr || defined.count(callee) == 0) {
            continue;
          }
          Calls[function].insert(callee);
          Callers[callee].insert(function);
        }
      }
    }
  }

  void collectCrossBlockValues() {
    for (llvm::Function *function : Functions) {
      for (llvm::Instruction &inst : llvm::instructions(function)) {
        llvm::BasicBlock *parent = inst.getParent();
        for (llvm::User *user : inst.users()) {
          auto *userInst = llvm::dyn_cast<llvm::Instruction>(user);
          if (userInst != nullptr && userInst->getParent() != parent) {
            CrossBlockValues.insert(&inst);
            break;
          }
        }
      }
    }
  }

  std::vector<std::vector<llvm::Function *>> sccs() const {
    std::vector<std::vector<llvm::Function *>> result;
    std::map<llvm::Function *, unsigned> index;
    std::map<llvm::Function *, unsigned> lowlink;
    std::vector<llvm::Function *> stack;
    std::set<llvm::Function *> onStack;
    unsigned nextIndex = 0;

    std::function<void(llvm::Function *)> visit = [&](llvm::Function *fn) {
      index[fn] = nextIndex;
      lowlink[fn] = nextIndex;
      ++nextIndex;
      stack.push_back(fn);
      onStack.insert(fn);

      auto edgeIt = Calls.find(fn);
      if (edgeIt != Calls.end()) {
        for (llvm::Function *callee : edgeIt->second) {
          if (index.count(callee) == 0) {
            visit(callee);
            lowlink[fn] = std::min(lowlink[fn], lowlink[callee]);
          } else if (onStack.count(callee) != 0) {
            lowlink[fn] = std::min(lowlink[fn], index[callee]);
          }
        }
      }

      if (lowlink[fn] != index[fn]) {
        return;
      }
      std::vector<llvm::Function *> component;
      while (!stack.empty()) {
        llvm::Function *member = stack.back();
        stack.pop_back();
        onStack.erase(member);
        component.push_back(member);
        if (member == fn) {
          break;
        }
      }
      result.push_back(std::move(component));
    };

    for (llvm::Function *function : Functions) {
      if (index.count(function) == 0) {
        visit(function);
      }
    }
    return result;
  }

  void runBottomUp() {
    std::vector<std::vector<llvm::Function *>> components = sccs();
    std::map<llvm::Function *, unsigned> componentOf;
    for (unsigned id = 0; id < components.size(); ++id) {
      for (llvm::Function *function : components[id]) {
        componentOf[function] = id;
      }
    }

    std::set<unsigned> done;
    std::function<void(unsigned)> process = [&](unsigned id) {
      if (done.count(id) != 0) {
        return;
      }
      for (llvm::Function *function : components[id]) {
        auto it = Calls.find(function);
        if (it == Calls.end()) {
          continue;
        }
        for (llvm::Function *callee : it->second) {
          unsigned calleeId = componentOf[callee];
          if (calleeId != id) {
            process(calleeId);
          }
        }
      }

      bool changed = true;
      while (changed) {
        changed = false;
        for (llvm::Function *function : components[id]) {
          FunctionFlow flow = solveFunction(*function);
          FunctionEffect next = std::move(flow.Effect);
          if (!(Effects[function] == next)) {
            Effects[function] = std::move(next);
            changed = true;
          }
          if (Options.CollectExternalCallsiteEvidence) {
            StableFlows[function] = std::move(flow);
          }
        }
      }
      done.insert(id);
    };

    for (unsigned id = 0; id < components.size(); ++id) {
      process(id);
    }
  }

  FunctionFlow solveFunction(llvm::Function &function) {
    FunctionFlow flow;
    flow.In[&function.getEntryBlock()].Reachable = true;

    bool changed = true;
    while (changed) {
      changed = false;
      for (llvm::BasicBlock &block : function) {
        if (&block != &function.getEntryBlock()) {
          State joined;
          for (llvm::BasicBlock *pred : llvm::predecessors(&block)) {
            joinState(joined, flow.Out[pred]);
          }
          bool inputChanged = Options.CollectExternalCallsiteEvidence
                                  ? !(flow.In[&block] == joined)
                                  : !flow.In[&block].sameSummaryLattice(joined);
          if (inputChanged) {
            flow.In[&block] = std::move(joined);
            changed = true;
          } else {
            flow.In[&block] = std::move(joined);
          }
        }
        State next = transferBlock(block, flow.In[&block]);
        bool outputChanged = Options.CollectExternalCallsiteEvidence
                                 ? !(flow.Out[&block] == next)
                                 : !flow.Out[&block].sameSummaryLattice(next);
        if (outputChanged) {
          flow.Out[&block] = std::move(next);
          changed = true;
        } else {
          flow.Out[&block] = std::move(next);
        }
      }
    }

    State exits;
    bool sawNoReturnExit = false;
    std::map<llvm::GlobalVariable *, Cell> noReturnReads;
    for (llvm::BasicBlock &block : function) {
      if (llvm::isa<llvm::ReturnInst>(block.getTerminator())) {
        joinState(exits, exitStateWithSavedRegisters(flow.Out[&block]));
      }
      const State &out = flow.Out[&block];
      if (out.EndsInNoReturn) {
        sawNoReturnExit = true;
        joinReadEntryOnly(noReturnReads, out);
      }
    }
    if (exits.Reachable) {
      flow.Effect.Registers = std::move(exits.Cells);
      for (const auto &[global, cell] : noReturnReads) {
        if (cell.ReadEntry) {
          flow.Effect.Registers[global].ReadEntry = true;
        }
      }
    } else if (sawNoReturnExit) {
      flow.Effect.Registers = std::move(noReturnReads);
      flow.Effect.NoReturn = true;
    }
    return flow;
  }

  const NativeExternalCallShape *
  externalCallShape(const llvm::CallBase &call) const {
    auto callsiteIt = Options.ExternalCallsiteShapes.find(&call);
    if (callsiteIt != Options.ExternalCallsiteShapes.end()) {
      return &callsiteIt->second;
    }
    llvm::Function *callee = call.getCalledFunction();
    if (callee == nullptr) {
      return nullptr;
    }
    auto shapeIt = Options.ExternalCallShapes.find(callee->getName().str());
    return shapeIt == Options.ExternalCallShapes.end() ? nullptr
                                                       : &shapeIt->second;
  }

  bool isUnknownExternalCall(const llvm::CallBase &call) const {
    llvm::Function *callee = call.getCalledFunction();
    if (callee != nullptr &&
        (!callee->isDeclaration() || callee->isIntrinsic())) {
      return false;
    }
    if (isNotDecRegisterHelperCall(call)) {
      return false;
    }
    return externalCallShape(call) == nullptr;
  }

  const NativeExternalCallShape *
  knownVarArgExternalShape(const llvm::CallBase &call) const {
    llvm::Function *callee = call.getCalledFunction();
    if (callee == nullptr || !callee->isDeclaration() ||
        callee->isIntrinsic()) {
      return nullptr;
    }
    auto shapeIt = Options.ExternalCallShapes.find(callee->getName().str());
    if (shapeIt == Options.ExternalCallShapes.end() ||
        !shapeIt->second.VarArg) {
      return nullptr;
    }
    return &shapeIt->second;
  }

  NativeRegisterCallsiteValueOrigin
  callsiteOrigin(const State &state, llvm::CallBase &call,
                 const NativeRegisterCallInputSlot &slot,
                 StackSlotKey *resolvedKey = nullptr) const {
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
  callsiteEvidence(const State &state, llvm::CallBase &call,
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

  void recordExternalCallsite(llvm::Function &caller, llvm::CallBase &call,
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

  void collectExternalCallsiteEvidence() {
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

  State transferBlock(llvm::BasicBlock &block, State state) {
    if (!state.Reachable) {
      return state;
    }
    for (llvm::Instruction &inst : block) {
      if (!state.Reachable) {
        break;
      }
      transferInstruction(inst, state);
    }
    pruneBlockLocalValueFacts(state);
    return state;
  }

  void pruneBlockLocalValueFacts(State &state) const {
    for (auto it = state.ValueOrigins.begin();
         it != state.ValueOrigins.end();) {
      if (CrossBlockValues.count(it->first) == 0) {
        it = state.ValueOrigins.erase(it);
      } else {
        ++it;
      }
    }
  }

  void transferInstruction(llvm::Instruction &inst, State &state) {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
      RegisterAccess access = registerLoad(*load, Units);
      if (access.Unit != nullptr) {
        Cell before = cellIn(state, access.Unit->Global);
        bool keepHighUse = isKeepHighPartialLoadUse(*load, access.Unit->Global);
        bool pureEntryValue = before.MayEntry && !before.MayNonEntry;
        if (!isIgnored(*access.Unit, Options) && !keepHighUse &&
            !pureEntryValue) {
          readRegister(state, access.Unit->Global);
        }
        if (pureEntryValue) {
          if (access.Unit->Global != stackPointerGlobal()) {
            state.ValueOrigins[load] = ValueOrigin{access.Unit->Global, 0};
          }
        } else {
          state.ValueOrigins.erase(load);
        }
        return;
      }
      if (std::optional<StackSlotKey> slot =
              fixedStackSlot(load->getPointerOperand(), *load)) {
        auto saved = state.StackSlots.find(*slot);
        if (saved != state.StackSlots.end()) {
          state.ValueOrigins[load] = ValueOrigin{saved->second, 0};
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
          writeRegister(state, access.Unit->Global);
          setFullRegisterOrigin(state, access.Unit->Global,
                                RegisterOriginKind::Mixed);
          state.ValueOrigins.erase(store);
          return;
        }
        std::optional<ValueOrigin> origin =
            entryValueOrigin(store->getValueOperand(), state);
        if (origin && origin->Base == access.Unit->Global &&
            origin->Offset == 0) {
          restoreRegister(state, access.Unit->Global);
          setFullRegisterOrigin(state, access.Unit->Global,
                                RegisterOriginKind::Entry);
        } else {
          markEntryValueRead(store->getValueOperand(), state);
          writeRegister(state, access.Unit->Global);
          setFullRegisterOrigin(state, access.Unit->Global,
                                registerOriginKindForStoredValue(
                                    store->getValueOperand(), state));
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
        if (origin && origin->Offset == 0 &&
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
        if (unitIt != Units.end() && !isIgnored(unitIt->second, Options)) {
          Cell before = cellIn(state, partialRead->Global);
          if (before.MayEntry && !before.MayNonEntry) {
            state.ValueOrigins[&inst] = ValueOrigin{partialRead->Global, 0};
          } else {
            readRegister(state, partialRead->Global);
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
          if (isX64Low32GprWrite(*partial, unitIt->second)) {
            writeRegister(state, partial->Global);
            setFullRegisterOrigin(
                state, partial->Global,
                registerOriginKindForStoredValue(partial->Value, state));
          } else {
            partialWriteRegister(state, partial->Global);
            llvm::APInt mask = registerRangeMask(
                partial->Global, partial->BitOffset, partial->WriteWidth);
            setRegisterOrigin(
                state, partial->Global, mask,
                registerOriginKindForStoredValue(partial->Value, state));
          }
        }
        state.ValueOrigins.erase(&inst);
        return;
      }
    }
    markInstructionEntryValueReads(inst, state);
  }

  llvm::GlobalVariable *stackPointerGlobal() const {
    if (Abi.StackPointer.empty()) {
      return nullptr;
    }
    auto it = UnitsByName.find(Abi.StackPointer);
    return it == UnitsByName.end() ? nullptr : it->second;
  }

  bool canTrackSavedEntryRegister(llvm::GlobalVariable *global) const {
    auto unitIt = Units.find(global);
    if (unitIt == Units.end()) {
      return false;
    }
    const RegisterUnit &unit = unitIt->second;
    return unit.Name != Abi.StackPointer &&
           Abi.Unaffected.count(unit.Name) != 0;
  }

  std::optional<ValueOrigin> entryValueOrigin(llvm::Value *value,
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
  registerOriginKindForStoredValue(llvm::Value *value,
                                   const State &state) const {
    if (entryValueOrigin(value, state)) {
      return RegisterOriginKind::Entry;
    }
    return RegisterOriginKind::Local;
  }

  llvm::GlobalVariable *entryRegisterOrigin(llvm::Value *value,
                                            const State &state) const {
    std::optional<ValueOrigin> origin = entryValueOrigin(value, state);
    if (!origin || origin->Offset != 0) {
      return nullptr;
    }
    return origin->Base;
  }

  void markEntryValueRead(llvm::Value *value, State &state) const {
    if (llvm::GlobalVariable *origin = entryRegisterOrigin(value, state)) {
      readRegister(state, origin);
    }
  }

  void markInstructionEntryValueReads(llvm::Instruction &inst,
                                      State &state) const {
    for (llvm::Value *operand : inst.operands()) {
      markEntryValueRead(operand, state);
    }
  }

  NativeStackAddressAnalysis &
  stackAddressAnalysis(llvm::Function &function) const {
    std::unique_ptr<NativeStackAddressAnalysis> &analysis =
        StackAddressAnalyses[&function];
    if (!analysis) {
      analysis = std::make_unique<NativeStackAddressAnalysis>(
          function, stackPointerGlobal(), Abi.StackPointer);
    }
    return *analysis;
  }

  std::optional<StackSlotKey>
  stackSlotKey(const NativeStackAddress &address) const {
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
  callsiteStackSlot(llvm::CallBase &call,
                    const NativeRegisterCallInputSlot &slot) const {
    llvm::Function *function = call.getFunction();
    if (function == nullptr ||
        slot.StackOffset >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        Abi.StackShift >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return std::nullopt;
    }
    std::optional<NativeStackAddress> current =
        stackAddressAnalysis(*function).stackPointerBefore(call);
    if (!current) {
      return std::nullopt;
    }
    int64_t stackOffset = static_cast<int64_t>(slot.StackOffset);
    int64_t stackShift = static_cast<int64_t>(Abi.StackShift);
    int64_t delta = stackOffset >= stackShift ? stackOffset - stackShift
                                              : -(stackShift - stackOffset);
    std::optional<NativeStackAddress> address =
        nativeStackAddressWithOffset(*current, delta);
    return address ? stackSlotKey(*address) : std::nullopt;
  }

  std::optional<StackSlotKey>
  fixedStackSlot(llvm::Value *pointer, llvm::Instruction &instruction) const {
    llvm::Function *function = instruction.getFunction();
    if (function == nullptr) {
      return std::nullopt;
    }
    std::optional<NativeStackAddress> address =
        stackAddressAnalysis(*function).addressForPointer(pointer);
    return address ? stackSlotKey(*address) : std::nullopt;
  }

  bool hasSavedEntryRegister(const State &state,
                             llvm::GlobalVariable *global) const {
    for (const auto &[slot, saved] : state.StackSlots) {
      (void)slot;
      if (saved == global) {
        return true;
      }
    }
    return false;
  }

  State exitStateWithSavedRegisters(const State &state) const {
    State adjusted = state;
    for (const auto &[global, unit] : Units) {
      if (isIgnored(unit, Options) || unit.Name == Abi.StackPointer ||
          Abi.Unaffected.count(unit.Name) == 0) {
        continue;
      }
      Cell cell = cellIn(adjusted, global);
      if (cell.MayNonEntry && hasSavedEntryRegister(state, global)) {
        restoreRegister(adjusted, global);
      }
    }
    return adjusted;
  }

  void transferCall(llvm::CallBase &call, State &state) {
    llvm::Function *callee = call.getCalledFunction();
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
    if (const NativeExternalCallShape *shape = externalCallShape(call)) {
      applyExternalCallEffect(state, *shape);
      consumeCallerStackArgEvidence(state);
      if (shape->NoReturn) {
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

  void consumeCallerStackArgEvidence(State &state) const {
    // A caller stack slot is argument evidence only for the next real call.
    // Keep StackSlots for saved-register tracking, but discard this separate
    // evidence so a reused outgoing area cannot widen a later call signature.
    state.StackSlotOrigins.clear();
    state.StackSlotStores.clear();
  }

  void markNoReturnExit(State &state) const {
    state.Reachable = false;
    state.EndsInNoReturn = true;
  }

  void applyFunctionEffect(const FunctionEffect &effect, State &state) {
    for (const auto &[global, unit] : Units) {
      if (isIgnored(unit, Options)) {
        continue;
      }
      Cell pre = cellIn(state, global);
      Cell callee = cellIn(effect, global);
      Cell post;
      post.ReadEntry = pre.ReadEntry || (callee.ReadEntry && pre.MayEntry);
      post.MayEntry = callee.MayEntry && pre.MayEntry;
      post.MayNonEntry =
          (callee.MayEntry && pre.MayNonEntry) || callee.MayNonEntry;
      state.Cells[global] = post;
      if (!callee.MayEntry && callee.MayNonEntry) {
        setFullRegisterOrigin(state, global, RegisterOriginKind::CallClobber);
      } else if (callee.MayEntry && callee.MayNonEntry) {
        setFullRegisterOrigin(state, global, RegisterOriginKind::Mixed);
      }
    }
  }

  void applyCallInputs(State &state,
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
        readRegister(state, unit.Global);
      }
    }
  }

  void applyAbiInputs(State &state) {
    for (const auto &[global, unit] : Units) {
      if (isIgnored(unit, Options)) {
        continue;
      }
      if (Abi.Inputs.count(unit.Name) != 0) {
        readRegister(state, global);
      }
    }
  }

  void applyAbiCallClobbers(State &state) {
    for (const auto &[global, unit] : Units) {
      if (isIgnored(unit, Options) || Abi.Unaffected.count(unit.Name) != 0 ||
          isSegmentBaseUnit(unit.Name)) {
        continue;
      }
      llvm::APInt clobberMask = fullMask(unit);
      if (clobberMask.getBitWidth() == 0 || clobberMask.isZero()) {
        continue;
      }
      writeRegister(state, global);
      setRegisterOrigin(state, global, clobberMask,
                        RegisterOriginKind::CallClobber);
    }
  }

  void applyExternalCallEffect(State &state,
                               const NativeExternalCallShape &shape) {
    applyCallInputs(state, shape.Inputs);
    applyAbiCallClobbers(state);
  }

  void applyUnknownExternalCallEffect(State &state) {
    if (Options.UnknownExternalInputPolicy ==
        NativeRegisterUnknownExternalInputPolicy::AbiInputs) {
      applyAbiInputs(state);
    }
    applyAbiCallClobbers(state);
  }

  void runTopDownDemand() {
    for (llvm::Function *function : Functions) {
      if (Callers[function].empty()) {
        seedAbiReturns(Demands[function]);
      }
    }

    bool changed = true;
    while (changed) {
      changed = false;
      for (llvm::Function *function : Functions) {
        std::map<llvm::Function *, RegisterDemand> additions =
            analyzeCallerDemand(*function);
        for (auto &[callee, demand] : additions) {
          changed |= mergeDemand(Demands[callee].ExitDemand, demand);
        }
      }
    }
  }

  void seedAbiReturns(FunctionDemand &demand) const {
    if (Abi.IntegerOutputOrder.empty()) {
      return;
    }
    auto it = UnitsByName.find(Abi.IntegerOutputOrder.front());
    if (it == UnitsByName.end()) {
      return;
    }
    const RegisterUnit &unit = Units.at(it->second);
    addDemand(demand.ExitDemand, it->second,
              namedMaskOrFull(Abi.OutputMasks, unit));
  }

  llvm::APInt
  valueDemand(llvm::Value *value,
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
  analyzeCallerDemand(llvm::Function &function) {
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

  RegisterDemand
  liveBeforeBlock(llvm::BasicBlock &block, RegisterDemand live,
                  std::map<llvm::Function *, RegisterDemand> &additions,
                  const std::map<llvm::Value *, llvm::APInt> &valueDemands) {
    for (llvm::Instruction &inst : llvm::reverse(block)) {
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (std::optional<NativeRegisterPartialReadInfo> partial =
                parseNativeRegisterPartialRead(*call)) {
          addDemand(live, partial->Global, valueDemand(call, valueDemands));
          continue;
        }
        if (std::optional<NativeRegisterPartialWriteInfo> partial =
                parseNativeRegisterPartialWrite(*call)) {
          auto unitIt = Units.find(partial->Global);
          if (unitIt != Units.end() &&
              isX64Low32GprWrite(*partial, unitIt->second)) {
            eraseDemand(live, partial->Global);
          } else {
            addDemand(live, partial->Global,
                      valueDemand(partial->Value, valueDemands));
          }
          continue;
        }
        llvm::Function *calleeF = call->getCalledFunction();
        if (!isNotDecRegisterHelperCall(*call) &&
            (calleeF == nullptr || !calleeF->isIntrinsic())) {
          applyBackwardCallDemand(*call, live, additions);
        }
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && !isIgnored(*access.Unit, Options)) {
          eraseDemand(live, access.Unit->Global);
        }
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        RegisterAccess access = registerLoad(*load, Units);
        if (access.Unit != nullptr && !isIgnored(*access.Unit, Options)) {
          addDemand(live, access.Unit->Global, valueDemand(load, valueDemands));
        }
      }
    }
    return live;
  }

  void applyBackwardCallDemand(
      llvm::CallBase &call, RegisterDemand &live,
      std::map<llvm::Function *, RegisterDemand> &additions) {
    llvm::Function *callee = call.getCalledFunction();
    if (callee != nullptr && !callee->isDeclaration() &&
        Effects.count(callee) != 0) {
      const FunctionEffect &effect = Effects[callee];
      if (effect.NoReturn) {
        live.clear();
        return;
      }
      for (const auto &[global, unit] : Units) {
        llvm::APInt mask = demandFor(live, global);
        if (isIgnored(unit, Options) || mask.getBitWidth() == 0 ||
            mask.isZero()) {
          continue;
        }
        Cell calleeCell = cellIn(effect, global);
        if (calleeCell.MayNonEntry) {
          addDemand(additions[callee], global, mask);
        }
        if (!calleeCell.MayEntry) {
          eraseDemand(live, global);
        }
      }
      return;
    }

    for (const auto &[global, unit] : Units) {
      if (isIgnored(unit, Options) || Abi.Unaffected.count(unit.Name) != 0 ||
          isSegmentBaseUnit(unit.Name)) {
        continue;
      }
      llvm::APInt clobberMask = fullMask(unit);
      if (clobberMask.getBitWidth() != 0 && !clobberMask.isZero()) {
        eraseDemand(live, global, clobberMask);
      }
    }

    auto addInputDemand = [&](const NativeRegisterCallInputSlot &input) {
      if (input.Kind != NativeRegisterCallInputSlotKind::Register) {
        return;
      }
      auto unitIt = UnitsByName.find(input.UnitName);
      if (unitIt == UnitsByName.end()) {
        return;
      }
      llvm::GlobalVariable *global = unitIt->second;
      const RegisterUnit &unit = Units.at(global);
      if (isIgnored(unit, Options)) {
        return;
      }
      llvm::APInt mask =
          registerRangeMask(global, input.OffsetBits, input.SizeBits);
      addDemand(live, global, mask);
    };

    if (const NativeExternalCallShape *shape = externalCallShape(call)) {
      if (shape->NoReturn) {
        live.clear();
      }
      for (const NativeRegisterCallInputSlot &input : shape->Inputs) {
        addInputDemand(input);
      }
      return;
    }
    if (callee != nullptr && callee->isDeclaration() &&
        callee->hasFnAttribute(llvm::Attribute::NoReturn)) {
      live.clear();
    }
    if (Options.UnknownExternalInputPolicy !=
        NativeRegisterUnknownExternalInputPolicy::AbiInputs) {
      return;
    }
    for (const std::string &name : Abi.InputOrder) {
      auto unitIt = UnitsByName.find(name);
      if (unitIt == UnitsByName.end()) {
        continue;
      }
      const RegisterUnit &unit = Units.at(unitIt->second);
      if (!isIgnored(unit, Options)) {
        addDemand(live, unit.Global, namedMaskOrFull(Abi.InputMasks, unit));
      }
    }
  }

  llvm::MDNode *registerNode(llvm::LLVMContext &context,
                             const RegisterUnit &unit) const {
    llvm::Metadata *fields[] = {
        llvm::MDString::get(context, "name=" + unit.Name),
    };
    return llvm::MDNode::get(context, fields);
  }

  llvm::MDNode *registerSummaryNode(llvm::LLVMContext &context,
                                    llvm::GlobalVariable *global,
                                    const Cell &cell,
                                    const FunctionDemand &demand) const {
    const RegisterUnit &unit = Units.at(global);
    bool exitDemand = demand.ExitDemand.count(global) != 0;
    std::vector<llvm::Metadata *> fields{
        llvm::MDString::get(context, "name=" + unit.Name),
        llvm::MDString::get(context, std::string("read_entry=") +
                                         (cell.ReadEntry ? "true" : "false")),
        llvm::MDString::get(context, std::string("may_entry=") +
                                         (cell.MayEntry ? "true" : "false")),
        llvm::MDString::get(context, std::string("may_non_entry=") +
                                         (cell.MayNonEntry ? "true" : "false")),
        llvm::MDString::get(context, std::string("exit_demand=") +
                                         (exitDemand ? "true" : "false")),
    };
    llvm::APInt entryMask = demandFor(demand.EntryDemand, global);
    if (entryMask.getBitWidth() != 0 && !entryMask.isZero()) {
      fields.push_back(llvm::MDString::get(context, "entry_demand_mask=0x" +
                                                        maskToHex(entryMask)));
    }
    llvm::APInt exitMask = demandFor(demand.ExitDemand, global);
    if (exitMask.getBitWidth() != 0 && !exitMask.isZero()) {
      fields.push_back(llvm::MDString::get(context, "exit_demand_mask=0x" +
                                                        maskToHex(exitMask)));
    }
    return llvm::MDNode::get(context, fields);
  }

  void attachMetadata() {
    llvm::LLVMContext &context = Module.getContext();
    for (llvm::Function *function : Functions) {
      std::vector<llvm::Metadata *> all;
      std::vector<llvm::Metadata *> reads;
      std::vector<llvm::Metadata *> preserves;
      std::vector<llvm::Metadata *> modifies;
      std::vector<llvm::Metadata *> demandedReturns;
      FunctionEffect &effect = Effects[function];
      FunctionDemand &demand = Demands[function];
      for (const auto &[global, unit] : Units) {
        if (isIgnored(unit, Options)) {
          continue;
        }
        Cell cell = cellIn(effect, global);
        bool exitDemand = demand.ExitDemand.count(global) != 0;
        all.push_back(registerSummaryNode(context, global, cell, demand));
        if (cell.ReadEntry) {
          reads.push_back(registerNode(context, unit));
        }
        if (cell.MayEntry && !cell.MayNonEntry) {
          preserves.push_back(registerNode(context, unit));
        }
        if (cell.MayNonEntry) {
          modifies.push_back(registerNode(context, unit));
        }
        if (exitDemand && cell.MayNonEntry &&
            Abi.Outputs.count(unit.Name) != 0) {
          demandedReturns.push_back(registerNode(context, unit));
        }
      }
      function->setMetadata("notdec.register.summary",
                            llvm::MDNode::get(context, all));
      function->setMetadata("notdec.register.summary.read_entry",
                            llvm::MDNode::get(context, reads));
      function->setMetadata("notdec.register.summary.preserves",
                            llvm::MDNode::get(context, preserves));
      function->setMetadata("notdec.register.summary.modifies",
                            llvm::MDNode::get(context, modifies));
      function->setMetadata("notdec.register.summary.demanded_returns",
                            llvm::MDNode::get(context, demandedReturns));
      if (effect.NoReturn) {
        function->addFnAttr(llvm::Attribute::NoReturn);
        function->setMetadata("notdec.register.summary.noreturn",
                              llvm::MDNode::get(context, {}));
      }
    }
  }

  NativeRegisterSummary buildPublicSummary() const {
    NativeRegisterSummary summary;
    summary.FunctionsSeen = Functions.size();
    summary.ExternalCallsites = ExternalCallsites;
    for (llvm::Function *function : Functions) {
      NativeRegisterSummaryFunction fn;
      fn.FunctionName = function->getName().str();
      for (llvm::BasicBlock &block : *function) {
        for (llvm::Instruction &inst : block) {
          if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
            RegisterAccess access = registerLoad(*load, Units);
            if (access.Unit != nullptr && !isIgnored(*access.Unit, Options)) {
              ++fn.LoadsSeen;
            }
          } else if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
            RegisterAccess access = registerStore(*store, Units);
            if (access.Unit != nullptr && !isIgnored(*access.Unit, Options)) {
              ++fn.StoresSeen;
            }
          } else if (isAnalyzableCall(inst)) {
            ++fn.CallsSeen;
          }
        }
      }

      const FunctionEffect &effect = Effects.at(function);
      fn.NoReturn = effect.NoReturn;
      const FunctionDemand &demand =
          Demands.count(function) == 0 ? EmptyDemand : Demands.at(function);
      for (const auto &[global, unit] : Units) {
        if (isIgnored(unit, Options)) {
          continue;
        }
        Cell cell = cellIn(effect, global);
        bool exitDemand = demand.ExitDemand.count(global) != 0;
        NativeRegisterSummaryRegister reg;
        reg.Name = unit.Name;
        reg.ReadEntry = cell.ReadEntry;
        reg.MayEntry = cell.MayEntry;
        reg.MayNonEntry = cell.MayNonEntry;
        reg.ExitDemand = exitDemand;
        reg.EntryDemandMaskHex =
            maskToHex(demandFor(demand.EntryDemand, global));
        reg.ExitDemandMaskHex = maskToHex(demandFor(demand.ExitDemand, global));
        fn.Registers.push_back(reg);
        if (cell.ReadEntry) {
          ++fn.ReadEntryRegisters;
        }
        if (cell.MayNonEntry) {
          ++fn.ModifiedRegisters;
        }
        if (cell.MayEntry && !cell.MayNonEntry) {
          ++fn.PreservedRegisters;
        }
        if (exitDemand && cell.MayNonEntry && Abi.Outputs.count(unit.Name)) {
          ++fn.DemandedReturns;
        }
      }
      summary.LoadsSeen += fn.LoadsSeen;
      summary.StoresSeen += fn.StoresSeen;
      summary.CallsSeen += fn.CallsSeen;
      if (fn.NoReturn) {
        ++summary.NoReturnFunctions;
      }
      summary.ReadEntryRegisters += fn.ReadEntryRegisters;
      summary.ModifiedRegisters += fn.ModifiedRegisters;
      summary.PreservedRegisters += fn.PreservedRegisters;
      summary.DemandedReturns += fn.DemandedReturns;
      summary.Functions.push_back(std::move(fn));
    }
    return summary;
  }

  static const FunctionDemand EmptyDemand;
};

const FunctionDemand Analyzer::EmptyDemand = {};

} // namespace

NativeRegisterSummary
runNativeRegisterSummary(llvm::Module &module,
                         const NativeRegisterSummaryOptions &options) {
  Analyzer analyzer(module, options);
  NativeRegisterSummary summary = analyzer.run();
  if (options.PrintSummary) {
    printNativeRegisterSummary(summary, llvm::errs());
  }
  return summary;
}

void printNativeRegisterSummary(const NativeRegisterSummary &summary,
                                llvm::raw_ostream &os) {
  os << "Native register summary: functions=" << summary.FunctionsSeen
     << " noreturn=" << summary.NoReturnFunctions
     << " loads=" << summary.LoadsSeen << " stores=" << summary.StoresSeen
     << " calls=" << summary.CallsSeen
     << " read_entry=" << summary.ReadEntryRegisters
     << " modified=" << summary.ModifiedRegisters
     << " preserved=" << summary.PreservedRegisters
     << " demanded_returns=" << summary.DemandedReturns << "\n";
  for (const NativeRegisterSummaryFunction &function : summary.Functions) {
    os << "  " << function.FunctionName << ": loads=" << function.LoadsSeen
       << " stores=" << function.StoresSeen << " calls=" << function.CallsSeen
       << " noreturn=" << (function.NoReturn ? "true" : "false")
       << " read_entry=" << function.ReadEntryRegisters
       << " modified=" << function.ModifiedRegisters
       << " preserved=" << function.PreservedRegisters
       << " demanded_returns=" << function.DemandedReturns << "\n";
  }
}

} // namespace notdec::bin2llvm
