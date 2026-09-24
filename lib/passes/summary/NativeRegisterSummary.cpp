#include "notdec-bin2llvm/passes/summary/NativeRegisterSummary.h"

#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/NativeExternalPrototype.h"
#include "notdec-bin2llvm/NativeX87Intrinsic.h"
#include "notdec-bin2llvm/NativeRegisterPartialRead.h"
#include "notdec-bin2llvm/NativeRegisterPartialWrite.h"
#include "notdec-bin2llvm/NativeRegisterValueRange.h"
#include "notdec-bin2llvm/passes/summary/NativeStackAddress.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
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

// native frame 槽位指针：notdec_stack.native 的 alloca（或它的 GEP/bitcast）。
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

// 该槽位指针上没有 load 能读到它。判据和帧清理的 stackAllocaStoreIsLoaded 保持
// 一致：只认指针相等的 load；对子偏移/位转换保守地当成"会被读到"。
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

// partial read 的结果只被存进"没人读"的 native frame 槽位（或者是死值）时，这个
// 读不构成对寄存器入口值/调用点值的需求：值存下来之后再没有 load 能观察到它，
// 帧清理本来就会把这种寄存器溢出 store 当死值删掉。memcached 里几个向量函数的
// 序言把 ZMM0..ZMM7 存进栈帧后再没读过，就是这种形态；不排掉的话签名会多出没人
// 用的向量参数，每个调用点还要造 unknown 去填。
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

// 识别 ST0/ST1 的窗口内部搬移读（见下方定义）。
bool isX87WindowShiftLoad(
    llvm::LoadInst &load,
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
  // 该值实际来自 Base 寄存器中的哪些位：整寄存器读是全宽，窄 partial
  // read 只带读到的位区间。入口读（ReadEntry）按这个范围记录，避免窄值
  // 沿整寄存器扩散成全宽参数。
  llvm::APInt Mask;

  bool operator==(const ValueOrigin &other) const {
    return Base == other.Base && Offset == other.Offset && Mask == other.Mask;
  }

  bool operator!=(const ValueOrigin &other) const { return !(*this == other); }
};

// Forward abstract state for one backing register, kept per bit range so a
// partial write only kills the written bits and a read only marks the read
// bits.  The three facts are bit masks (bit i set means fact i holds):
//   MayEntry    : exit may still hold the entry value on bit i
//   MayNonEntry : bit i may have been rewritten inside the function
//   ReadEntry   : the entry value on bit i was read before being killed
// Missing map entries use this default value, but only inside a reachable
// block state.
struct Cell {
  llvm::APInt MayEntry;
  llvm::APInt MayNonEntry;
  llvm::APInt ReadEntry;

  bool operator==(const Cell &other) const {
    return MayEntry == other.MayEntry && MayNonEntry == other.MayNonEntry &&
           ReadEntry == other.ReadEntry;
  }
};

unsigned registerBitWidth(llvm::GlobalVariable *global);

// Default cell for a register that was never touched: the whole register may
// still be the entry value.
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

// Callsite inference needs to distinguish explicit argument definitions from
// entry forwarding and previous-call clobbers.  Keep that information beside
// the existing Cell domain so it cannot change the summary lattice itself.
struct RegisterOriginBits {
  llvm::APInt Entry;
  llvm::APInt Local;
  // Local 中"最后一次写是浮点指令"的子集。XMM 槽被 movq/punpcklqdq 这类
  // 整数打包写也会变 Local，但不是浮点参数；浮点参数证据只认这个子集。
  llvm::APInt FloatLocal;
  llvm::APInt CallClobber;

  bool operator==(const RegisterOriginBits &other) const {
    return Entry == other.Entry && Local == other.Local &&
           FloatLocal == other.FloatLocal &&
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
  if (mask.getBitWidth() == 0 || mask.isZero()) {
    return "";
  }
  llvm::SmallVector<char, 64> text;
  mask.toStringUnsigned(text, 16);
  return std::string(text.begin(), text.end());
}

// mask 非零的 bool 投影：宽度 0（无信息）也按 false 处理。
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
  // x87 窗口移位读（push/pop/fxch 的槽位搬移）只转移窗口内部状态，不产生
  // 对寄存器值的真实需求。跳过它们，避免把 ST0 的需求通过调用点误传成
  // callee 的 ExitDemand（见 PcodeToLLVM 的 readX87ST windowShift）。
  if (load.getMetadata("notdec.x87.window.shift") != nullptr ||
      isX87WindowShiftLoad(load, units)) {
    return {};
  }
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

// x87 窗口移位读：push/pop/fxch 的槽位搬移（值经 bitcast 链后 store 到另一个
// ST 寄存器，或传给 notdec.x87.* 库），只转移窗口内部状态，不是对寄存器值
// 的真实消费。lifting 会给这类 load 打 notdec.x87.window.shift 标记，但
// InstCombine 可能重写 load 丢掉标记，这里按 use 形态兜底识别，避免把 ST0
// 的需求通过调用点误传成 callee 的 ExitDemand。
bool isX87WindowShiftLoad(
    llvm::LoadInst &load,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      load.getPointerOperand()->stripPointerCasts());
  if (global == nullptr) {
    return false;
  }
  auto it = units.find(global);
  if (it == units.end()) {
    return false;
  }
  const RegisterUnit &unit = it->second;
  if (unit.Name != "ST0" && unit.Name != "ST1") {
    return false;
  }
  // 只有所有 use 都是窗口内部搬移（store 到寄存器全局 / 传给 x87 库）才算
  // 纯移位读。fld %st(0) 和 fstpt 可能共享同一个 load（缓存复用）：后者
  // 把值存到内存，是真实消费，不能按移位跳过。
  if (load.getNumUses() == 0) {
    return false;
  }
  for (llvm::User *user : load.users()) {
    llvm::Value *value = user;
    // 剥离 bitcast/zext/trunc 单链，找到真正的消费指令。
    while (auto *cast = llvm::dyn_cast<llvm::CastInst>(value)) {
      if (!cast->hasOneUse()) {
        break;
      }
      value = *cast->user_begin();
    }
    bool internal = false;
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(value)) {
      auto *target = llvm::dyn_cast<llvm::GlobalVariable>(
          store->getPointerOperand()->stripPointerCasts());
      if (target != nullptr && units.count(target) != 0) {
        internal = true;
      }
    }
    if (auto *call = llvm::dyn_cast<llvm::CallInst>(value)) {
      llvm::Function *callee = call->getCalledFunction();
      if (callee != nullptr &&
          isNativeX87IntrinsicName(callee->getName())) {
        internal = true;
      }
    }
    if (!internal) {
      return false;
    }
  }
  return true;
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
  if (callee == nullptr) {
    return false;
  }
  // notdec.register.* 是寄存器模型内置 intrinsic 的命名空间：unknown 占位、
  // frame keep-alive 等等。它们不是真实外部函数，调用点不参与外部参数推断或
  // ABI clobber 推导；对寄存器的影响一律由它们自己的语义（指针参数、返回值）
  // 显式表达。以后新增 intrinsic 统一用这个前缀，不需要再往名单里补。
  // 下面几个是历史前缀，语义相同，保留兼容。
  return callee->getName().starts_with("notdec.register.") ||
         callee->getName().starts_with("notdec.unknown.") ||
         isNativeX87IntrinsicName(callee->getName()) ||
         isNativeRegisterValueRangeName(callee->getName());
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

// 整寄存器读写的全宽 mask。
llvm::APInt fullRegisterMask(llvm::GlobalVariable *global) {
  unsigned width = registerBitWidth(global);
  return width == 0 ? llvm::APInt() : llvm::APInt::getAllOnes(width);
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

bool hasFloatWriteMetadata(const llvm::Instruction &inst) {
  return inst.getMetadata("notdec.register.float_write") != nullptr;
}

// 浮点写记录：只对 Local 定义的写调用，维护 FloatLocal 子集。
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

// readMask 置位的位，如果该位此刻仍可能是入口值，就记为"入口值被读过"。
// 按 bit 处理：被覆盖过的位（MayEntry=0）即使被读也不算入口读。
void readRegister(State &state, llvm::GlobalVariable *global,
                  const llvm::APInt &readMask) {
  if (readMask.getBitWidth() == 0 || readMask.isZero()) {
    return;
  }
  Cell &cell = cellFor(state, global);
  llvm::APInt bounded = readMask.zextOrTrunc(cell.MayEntry.getBitWidth());
  cell.ReadEntry |= (cell.MayEntry & bounded);
}

// writeMask 置位的位：入口值被覆盖，变成非入口定义。整寄存器写就是全宽
// mask，部分写只传写到的位区间，不再需要在整寄存器粒度下保守保留
// MayEntry。
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

// 保存的入口值写回寄存器：只有恢复的位重新成为入口值。
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
  // 栈对齐 push 的槽位集合，按函数缓存（见 stackAdjustmentSlots 的注释）。
  mutable std::map<llvm::Function *, std::set<StackSlotKey>> StackAdjustmentSlots;

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
        if (cell.ReadEntry.getBitWidth() == 0 || cell.ReadEntry.isZero()) {
          continue;
        }
        auto [it, inserted] =
            flow.Effect.Registers.try_emplace(global, defaultCell(global));
        if (inserted) {
          it->second.ReadEntry = cell.ReadEntry;
        } else {
          it->second.ReadEntry |= cell.ReadEntry;
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
    if (callee == nullptr || callee->isIntrinsic()) {
      return nullptr;
    }
    auto shapeIt = Options.ExternalCallShapes.find(callee->getName().str());
    if (shapeIt == Options.ExternalCallShapes.end() ||
        !shapeIt->second.VarArg) {
      return nullptr;
    }
    return &shapeIt->second;
  }

  // 读出来的值是否被"原样搬到别处"（直接存进另一个寄存器或内存，或只经过一层
  // cast 再存）。只有这种读才说明寄存器里的值已经被物化到别处、本身可能只是残留
  // （例如 lea name,%rcx; mov %rcx,%rdx; call f 里的 RCX）。
  // 比较（判空 test %rdi,%rdi）、算术/寻址（mov 0x10(%rdi),%rax）都是"还需要
  // 这个值"，寄存器仍要带着它进调用，不能算消费。
  bool loadValueIsMovedAway(const llvm::LoadInst &load) const {
    llvm::SmallVector<const llvm::Value *, 8> worklist{&load};
    llvm::SmallPtrSet<const llvm::Value *, 8> seen;
    unsigned budget = 16;
    while (!worklist.empty() && budget-- != 0) {
      const llvm::Value *value = worklist.pop_back_val();
      if (!seen.insert(value).second) {
        continue;
      }
      for (const llvm::User *user : value->users()) {
        if (llvm::isa<llvm::StoreInst>(user)) {
          return true;
        }
        if (auto *cast = llvm::dyn_cast<llvm::CastInst>(user)) {
          worklist.push_back(cast);
        }
      }
    }
    return false;
  }

  // An argument register holds a value that is still waiting for the call.  A
  // local value that was read between its last write and the call was consumed
  // by that read, so it may only be a scratch copy.  Only the current block is
  // scanned: a value written in an earlier block has nothing in between here
  // that could have consumed it.  A read that only sets flags (compare/test)
  // does not consume the value; the register still has to carry it into the
  // call.
  bool registerValuePendingAtCall(const llvm::GlobalVariable &global,
                                  const llvm::CallBase &call) const {
    const llvm::BasicBlock *block = call.getParent();
    if (block == nullptr) {
      return true;
    }
    for (auto it = call.getIterator(); it != block->begin();) {
      --it;
      const llvm::Instruction &instruction = *it;
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        if (load->getPointerOperand()->stripPointerCasts() == &global) {
          if (!loadValueIsMovedAway(*load)) {
            continue;
          }
          return false;
        }
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        if (store->getPointerOperand()->stripPointerCasts() == &global) {
          return true;
        }
      }
    }
    return true;
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

  bool instructionReadsRegister(const llvm::Instruction &inst,
                                const llvm::GlobalVariable &global) const {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
      return load->getPointerOperand()->stripPointerCasts() == &global;
    }
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call == nullptr) {
      return false;
    }
    std::optional<NativeRegisterPartialReadInfo> partial =
        parseNativeRegisterPartialRead(*call);
    return partial && partial->Global == &global;
  }

  bool instructionWritesRegister(const llvm::Instruction &inst,
                                 const llvm::GlobalVariable &global) const {
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      return store->getPointerOperand()->stripPointerCasts() == &global;
    }
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call == nullptr) {
      return false;
    }
    std::optional<NativeRegisterPartialWriteInfo> partial =
        parseNativeRegisterPartialWrite(*call);
    return partial && partial->Global == &global;
  }

  // 该调用会不会消费 @global 的值。外部调用看 shape 的输入槽；内部调用看
  // bottom-up effect 的 ReadEntry；未知外部、间接调用、effect 还没算出的 callee
  // 一律保守当成会读。
  bool callMayReadRegister(const llvm::CallBase &call,
                           const llvm::GlobalVariable &global) const {
    auto unitIt = Units.find(const_cast<llvm::GlobalVariable *>(&global));
    if (unitIt == Units.end()) {
      return true;
    }
    llvm::StringRef name = unitIt->second.Name;
    auto mentions = [&](llvm::ArrayRef<NativeRegisterCallInputSlot> slots) {
      for (const NativeRegisterCallInputSlot &input : slots) {
        if (input.Kind == NativeRegisterCallInputSlotKind::Register &&
            input.UnitName == name) {
          return true;
        }
      }
      return false;
    };
    if (const NativeExternalCallShape *shape = externalCallShape(call)) {
      return mentions(shape->Inputs) || mentions(shape->VarArgInputs);
    }
    llvm::Function *callee = call.getCalledFunction();
    if (callee == nullptr || callee->isDeclaration()) {
      return true;
    }
    auto effectIt = Effects.find(callee);
    if (effectIt == Effects.end()) {
      return true;
    }
    auto regIt = effectIt->second.Registers.find(
        const_cast<llvm::GlobalVariable *>(&global));
    if (regIt == effectIt->second.Registers.end()) {
      return false;
    }
    const llvm::APInt &readEntry = regIt->second.ReadEntry;
    return readEntry.getBitWidth() != 0 && !readEntry.isZero();
  }

  // store 写进 @global 的值，在被下一次写覆盖之前有没有被显式读过。块内先看，
  // 没被覆盖就沿后继块看（上限 64 个块，超了按"活"处理，保守）。遇到会消费该
  // 寄存器的调用按"读"处理（lift 的 IR 里实参是显式 store 进寄存器 + call）；
  // callee 不碰这个寄存器就继续往后看。ignoreCallReads 只给确认过的栈对齐
  // push/pop 对用（见 collectStackAdjustmentSlots 判据 4）：那种槽的目的寄存器
  // 由编译器挑成死的，调用对它的消费不算真实使用。
  bool registerValueDiesAfter(llvm::StoreInst &store,
                              const llvm::GlobalVariable &global,
                              bool ignoreCallReads = false) const {
    llvm::BasicBlock *block = store.getParent();
    if (block == nullptr) {
      return false;
    }
    constexpr unsigned maxBlocks = 64;
    llvm::SmallVector<llvm::BasicBlock *, 8> worklist;
    llvm::SmallPtrSet<llvm::BasicBlock *, 32> visited;
    visited.insert(block);
    for (llvm::BasicBlock *successor : llvm::successors(block)) {
      worklist.push_back(successor);
    }
    for (auto it = std::next(store.getIterator()); it != block->end(); ++it) {
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&*it);
          call != nullptr && !ignoreCallReads && isAnalyzableCall(*call) &&
          callMayReadRegister(*call, global)) {
        return false;
      }
      if (instructionReadsRegister(*it, global)) {
        return false;
      }
      if (instructionWritesRegister(*it, global)) {
        return true;
      }
    }
    while (!worklist.empty()) {
      if (visited.size() >= maxBlocks) {
        return false;
      }
      llvm::BasicBlock *current = worklist.pop_back_val();
      if (!visited.insert(current).second) {
        continue;
      }
      bool killed = false;
      for (const llvm::Instruction &inst : *current) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
        if ((call != nullptr && !ignoreCallReads && isAnalyzableCall(*call) &&
             callMayReadRegister(*call, global)) ||
            instructionReadsRegister(inst, global)) {
          return false;
        }
        if (instructionWritesRegister(inst, global)) {
          killed = true;
          break;
        }
      }
      if (killed) {
        continue;
      }
      for (llvm::BasicBlock *successor : llvm::successors(current)) {
        worklist.push_back(successor);
      }
    }
    return true;
  }

  // 栈对齐 push（GCC 的 push %rcx + pop %rax；LLVM 的 pushq %rax + pop 死寄存器）
  // 在 lift 之后只是"存入口值 → 取出来写进另一个寄存器"，前向 summary 会把入口读
  // 记成真实使用，把 scratch 寄存器变成形参。编译器源码里这两种 push/pop 的语义
  // 是明确的：LLVM X86FrameLowering::BuildStackAdjustment 用
  // getUndefRegState(isSub) 标 push（值不存在），pop 用 findDeadCallerSavedReg 挑
  // 一个死寄存器。
  //
  // 识别判据（只对非 callee-saved 寄存器，尽量保守）：
  //   1. store 进固定栈槽的值是某个 caller-saved 寄存器的入口 load；
  //   2. 该槽的 load 没有写回同一个寄存器（真 save/restore 对不走这里）；
  //   3. 每个这样的 load 都只被搬进"搬完就死"的寄存器（下一次写之前没有显式读）；
  //   4. 或者这是一条栈调整访问（push 写在栈顶、pop 读栈顶，且 SP 调整量正好是
  //      访问宽度，见 isStackAdjustmentAccess）。编译器挑 pop 目的寄存器时已经保证它是死的
  //      （LLVM findDeadCallerSavedReg；GCC 同形），所以判据 4 里调用对目的寄存器
  //      的消费不算读——间接/未知调用算不出实参寄存器，判据 3 会因此拒绝真 push。
  // 成立才认定为对齐填充：push 侧不计入口读，load 侧不挂保存值来源。
  // 判据 4 的边界：`push %rcx` 与 `sub $8,%rsp; mov %rcx,(%rsp)` 的 p-code 相同，
  // 无帧指针函数里后者那种 8 字节临时槽也会命中；这种槽若真把值传给未知调用就会
  // 被误判。实测 7 个目标只有 lighttpd 的两个函数命中（机器码都是真 push），
  // 见 logs/20260924-01-native-stack-align-push-plan.md。
  const std::set<StackSlotKey> &
  stackAdjustmentSlots(llvm::Function &function) const {
    auto cached = StackAdjustmentSlots.find(&function);
    if (cached != StackAdjustmentSlots.end()) {
      return cached->second;
    }
    std::set<StackSlotKey> slots;
    collectStackAdjustmentSlots(function, slots);
    return StackAdjustmentSlots.emplace(&function, std::move(slots))
        .first->second;
  }

  // 把地址换算到可以互相比较的坐标：NativeFrame / RelativeStackPointer 原样
  // 返回；入口 SP 坐标在有重写帧的函数里换算成帧 alloca 坐标（换算不了就原样，
  // 那种情况两边都换算不了，仍可比）。
  std::optional<StackSlotKey>
  comparableSlotKey(llvm::Function &function,
                    const NativeStackAddress &address) const {
    std::optional<StackSlotKey> key = stackSlotKey(address);
    if (!key) {
      return std::nullopt;
    }
    if (address.Kind != NativeStackAddressKind::EntryStackPointer) {
      return key;
    }
    if (std::optional<StackSlotKey> converted =
            entrySlotToNativeFrameKey(function, *key)) {
      return converted;
    }
    return key;
  }

  // 这条访问是不是栈调整本身（push 写进去 / pop 读出来的那个栈顶槽）。
  // 三个条件：
  //   1. 访问地址等于该指令处的栈指针（正好落在栈顶）；
  //   2. 另一侧的栈指针与它相差恰好一个访问宽度（push 看调整前，pop 看调整后）：
  //      `push %reg` 是 SP-=8 后立刻写 [SP]，而"在预留区里做临时溢出"是
  //      `sub $N,%rsp` 之后写 [rsp]，N 是整块预留大小，不等于单次访问宽度；
  //   3. 两边的地址换算到同一坐标后再比（栈帧重写把帧访问换成 notdec_stack.native
  //      的 GEP，而 @RSP 的值仍是入口 SP 坐标，见 entrySlotToNativeFrameKey）。
  bool isStackAdjustmentAccess(llvm::Instruction &inst, llvm::Value *pointer,
                               uint64_t sizeBytes, bool push) const {
    llvm::Function *function = inst.getFunction();
    llvm::BasicBlock *block = inst.getParent();
    llvm::GlobalVariable *stackPointer = stackPointerGlobal();
    if (function == nullptr || block == nullptr || stackPointer == nullptr ||
        sizeBytes == 0) {
      return false;
    }
    auto isStackPointerStore = [stackPointer](llvm::Instruction &candidate) {
      auto *store = llvm::dyn_cast<llvm::StoreInst>(&candidate);
      return store != nullptr &&
             store->getPointerOperand()->stripPointerCasts() == stackPointer;
    };
    NativeStackAddressAnalysis &analysis = stackAddressAnalysis(*function);
    std::optional<NativeStackAddress> slot =
        analysis.addressForPointer(pointer);
    std::optional<NativeStackAddress> current =
        analysis.stackPointerBefore(inst);
    if (!slot || !current) {
      return false;
    }
    std::optional<StackSlotKey> slotKey = comparableSlotKey(*function, *slot);
    std::optional<StackSlotKey> currentKey =
        comparableSlotKey(*function, *current);
    if (!slotKey || !currentKey || !(*slotKey == *currentKey)) {
      return false;
    }
    std::optional<NativeStackAddress> other;
    if (push) {
      for (auto it = inst.getIterator(); it != block->begin();) {
        --it;
        if (isStackPointerStore(*it)) {
          other = analysis.stackPointerBefore(*it);
          break;
        }
      }
    } else {
      for (auto it = std::next(inst.getIterator()); it != block->end(); ++it) {
        if (isStackPointerStore(*it)) {
          other = analysis.addressForIntegerValue(
              llvm::cast<llvm::StoreInst>(&*it)->getValueOperand());
          break;
        }
      }
    }
    if (!other) {
      return false;
    }
    std::optional<NativeStackAddress> expected = nativeStackAddressWithOffset(
        *slot, static_cast<int64_t>(sizeBytes));
    if (!expected) {
      return false;
    }
    std::optional<StackSlotKey> otherKey = comparableSlotKey(*function, *other);
    std::optional<StackSlotKey> expectedKey =
        comparableSlotKey(*function, *expected);
    return otherKey && expectedKey && *otherKey == *expectedKey;
  }

  // llvm::Value 的字节宽度，取不到（不可计算/非整数）时返回 0（判据直接不成立）。
  static uint64_t valueSizeInBytes(llvm::Value *value) {
    llvm::Type *type = value != nullptr ? value->getType() : nullptr;
    if (type == nullptr || !type->isSized() ||
        !(type->isIntegerTy() || type->isFloatingPointTy() ||
          type->isPointerTy())) {
      return 0;
    }
    return type->getPrimitiveSizeInBits() / 8;
  }

  void collectStackAdjustmentSlots(llvm::Function &function,
                                   std::set<StackSlotKey> &result) const {
    struct Candidate {
      StackSlotKey Slot;
      llvm::GlobalVariable *Register = nullptr;
      // store 正好写在当前栈顶（push 那一侧，判据 4）。
      bool StackTop = false;
    };
    std::vector<Candidate> candidates;
    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &inst : block) {
        auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
        if (store == nullptr) {
          continue;
        }
        auto *loaded = llvm::dyn_cast<llvm::LoadInst>(store->getValueOperand());
        if (loaded == nullptr) {
          continue;
        }
        RegisterAccess access = registerLoad(*loaded, Units);
        if (access.Unit == nullptr || isIgnored(*access.Unit, Options) ||
            access.Unit->Name == Abi.StackPointer ||
            Abi.Unaffected.count(access.Unit->Name) != 0) {
          continue;
        }
        std::optional<StackSlotKey> slot =
            fixedStackSlot(store->getPointerOperand(), *store);
        if (!slot) {
          continue;
        }
        const bool stackTop = isStackAdjustmentAccess(
            inst, store->getPointerOperand(),
            valueSizeInBytes(store->getValueOperand()), /*push=*/true);
        candidates.push_back({*slot, access.Unit->Global, stackTop});
      }
    }
    for (const Candidate &candidate : candidates) {
      if (result.count(candidate.Slot) != 0) {
        continue;
      }
      bool padding = true;
      bool sawLoad = false;
      for (llvm::BasicBlock &block : function) {
        for (llvm::Instruction &inst : block) {
          auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
          if (load == nullptr) {
            continue;
          }
          std::optional<StackSlotKey> slot =
              fixedStackSlot(load->getPointerOperand(), *load);
          if (!slot || !(*slot == candidate.Slot)) {
            continue;
          }
          sawLoad = true;
          const bool stackTopLoad = isStackAdjustmentAccess(
              *load, load->getPointerOperand(), valueSizeInBytes(load),
              /*push=*/false);
          for (llvm::User *user : load->users()) {
            auto *store = llvm::dyn_cast<llvm::StoreInst>(user);
            if (store == nullptr) {
              padding = false;
              break;
            }
            RegisterAccess access = registerStore(*store, Units);
            if (access.Unit == nullptr ||
                access.Unit->Global == candidate.Register) {
              padding = false;
              break;
            }
            // 判据 3：普通保存槽，目的寄存器在下次写之前不能被显式读、也不能被
            // 调用消费。
            if (registerValueDiesAfter(*store, *access.Unit->Global)) {
              continue;
            }
            // 判据 4：push/pop 对（store 写在栈顶、load 也从栈顶读）里目的寄存器
            // 由编译器保证是死的，调用对它的消费不算真实使用。
            if (candidate.StackTop && stackTopLoad &&
                registerValueDiesAfter(*store, *access.Unit->Global,
                                       /*ignoreCallReads=*/true)) {
              continue;
            }
            padding = false;
            break;
          }
          if (!padding) {
            break;
          }
        }
        if (!padding) {
          break;
        }
      }
      if (padding && sawLoad) {
        result.insert(candidate.Slot);
      }
    }
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

  void markEntryValueRead(llvm::Value *value, State &state) const {
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
      if (!cell.MayNonEntry.isZero() && hasSavedEntryRegister(state, global)) {
        restoreRegister(adjusted, global, fullRegisterMask(global));
      }
    }
    return adjusted;
  }

  void transferCall(llvm::CallBase &call, State &state) {
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
        // 只把 slot 覆盖的位当作"入口值被读过"：调用点传参只准备了自己
        // 的位，其余位是调用者自己的状态，不归 callee 读。
        readRegister(state, unit.Global,
                     registerRangeMask(unit.Global, input.OffsetBits,
                                       input.SizeBits));
      }
    }
  }

  void applyAbiInputs(State &state) {
    for (const auto &[global, unit] : Units) {
      if (isIgnored(unit, Options)) {
        continue;
      }
      if (Abi.Inputs.count(unit.Name) != 0) {
        readRegister(state, global, fullRegisterMask(global));
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
      writeRegister(state, global, clobberMask);
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
    // 返回值接走），见 collectAbiFacts 的 ST0 输出槽补充。
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
          // 只喂给"没人读"的帧槽的死读不产生需求（见 helper 注释）。
          if (!partialReadFeedsOnlyDeadFrameSlots(*call)) {
            addDemand(live, partial->Global, valueDemand(call, valueDemands));
          }
          continue;
        }
        if (std::optional<NativeRegisterPartialWriteInfo> partial =
                parseNativeRegisterPartialWrite(*call)) {
          auto unitIt = Units.find(partial->Global);
          if (unitIt != Units.end() &&
              isX64Low32GprWrite(*partial, unitIt->second)) {
            eraseDemand(live, partial->Global);
          } else {
            // partial write 定义写入位：擦除这些位的 live demand（写覆盖旧
            // 需求），未写位保留（读改写需要旧值）。写入值自己的位需求由
            // value 链上的显式 load 追踪，不能反过来加到 global 上——否则
            // 调用点之后的传参写（如 lua_pushnumber 的 double 参数写 ZMM0）
            // 会把 ZMM0 需求误传给更早的 callee，给不返回 double 的函数
            // 误加 ZMM0 返回槽。
            llvm::APInt writeMask = registerRangeMask(
                partial->Global, partial->BitOffset, partial->WriteWidth);
            eraseDemand(live, partial->Global, writeMask);
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
        // 只把 callee 实际改写（MayNonEntry）的位加进 callee 的退出需求；
        // 调用后仍可能是入口值（MayEntry）的位，live 需求保留。
        llvm::APInt nonEntryBits = calleeCell.MayNonEntry & mask;
        if (maskNonZero(nonEntryBits)) {
          addDemand(additions[callee], global, nonEntryBits);
        }
        llvm::APInt killedBits = ~calleeCell.MayEntry & mask;
        if (maskNonZero(killedBits)) {
          eraseDemand(live, global, killedBits);
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
    // bool 字段是 mask 的投影（mask 非零），mask 字段给出按位信息。
    std::vector<llvm::Metadata *> fields{
        llvm::MDString::get(context, "name=" + unit.Name),
        llvm::MDString::get(context, std::string("read_entry=") +
                                         (maskNonZero(cell.ReadEntry)
                                              ? "true"
                                              : "false")),
        llvm::MDString::get(context, "read_entry_mask=0x" +
                                         maskToHex(cell.ReadEntry)),
        llvm::MDString::get(context, std::string("may_entry=") +
                                         (maskNonZero(cell.MayEntry) ? "true"
                                                                     : "false")),
        llvm::MDString::get(context,
                            "may_entry_mask=0x" + maskToHex(cell.MayEntry)),
        llvm::MDString::get(context,
                            std::string("may_non_entry=") +
                                (maskNonZero(cell.MayNonEntry) ? "true"
                                                               : "false")),
        llvm::MDString::get(context, "may_non_entry_mask=0x" +
                                         maskToHex(cell.MayNonEntry)),
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
        if (maskNonZero(cell.ReadEntry)) {
          reads.push_back(registerNode(context, unit));
        }
        if (maskNonZero(cell.MayEntry) && cell.MayNonEntry.isZero()) {
          preserves.push_back(registerNode(context, unit));
        }
        if (maskNonZero(cell.MayNonEntry)) {
          modifies.push_back(registerNode(context, unit));
        }
        if (exitDemand && maskNonZero(cell.MayNonEntry) &&
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
        // bool 字段保留为 mask 非零的投影，测试和计数器继续用它们；
        // 精确的按位信息在新增的 *MaskHex 字段里。
        reg.ReadEntry = maskNonZero(cell.ReadEntry);
        reg.MayEntry = maskNonZero(cell.MayEntry);
        reg.MayNonEntry = maskNonZero(cell.MayNonEntry);
        reg.ExitDemand = exitDemand;
        reg.ReadEntryMaskHex = maskToHex(cell.ReadEntry);
        reg.MayEntryMaskHex = maskToHex(cell.MayEntry);
        reg.MayNonEntryMaskHex = maskToHex(cell.MayNonEntry);
        reg.EntryDemandMaskHex =
            maskToHex(demandFor(demand.EntryDemand, global));
        reg.ExitDemandMaskHex = maskToHex(demandFor(demand.ExitDemand, global));
        fn.Registers.push_back(reg);
        if (reg.ReadEntry) {
          ++fn.ReadEntryRegisters;
        }
        if (reg.MayNonEntry) {
          ++fn.ModifiedRegisters;
        }
        if (reg.MayEntry && !reg.MayNonEntry) {
          ++fn.PreservedRegisters;
        }
        if (exitDemand && reg.MayNonEntry && Abi.Outputs.count(unit.Name)) {
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
