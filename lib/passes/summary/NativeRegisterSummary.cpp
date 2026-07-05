#include "notdec-bin2llvm/passes/summary/NativeRegisterSummary.h"

#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/NativeRegisterPartialRead.h"
#include "notdec-bin2llvm/NativeRegisterPartialWrite.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
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
#include <map>
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

struct State {
  bool Reachable = false;
  std::map<llvm::GlobalVariable *, Cell> Cells;
  // Narrow frame-local model for saved-register recognition.  It only tracks
  // fixed entry-SP offsets or notdec_stack.native alloca offsets, not arbitrary
  // memory.
  std::map<StackSlotKey, llvm::GlobalVariable *> StackSlots;
  // SSA values known to be exactly one function-entry register value.
  std::map<llvm::Value *, llvm::GlobalVariable *> ValueOrigins;

  bool operator==(const State &other) const {
    return Reachable == other.Reachable && Cells == other.Cells &&
           StackSlots == other.StackSlots && ValueOrigins == other.ValueOrigins;
  }
};

// Bottom-up summary: the CFG-level effect visible at normal function exits.
struct FunctionEffect {
  std::map<llvm::GlobalVariable *, Cell> Registers;

  bool operator==(const FunctionEffect &other) const {
    return Registers == other.Registers;
  }
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
  std::set<std::string> Outputs;
  std::map<std::string, llvm::APInt> OutputMasks;
  std::vector<std::string> OutputOrder;
  std::vector<std::string> IntegerOutputOrder;
  std::set<std::string> Unaffected;
  std::set<std::string> KilledByCall;
  std::map<std::string, llvm::APInt> KilledByCallMasks;
  std::string StackPointer;
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
         callee->getName().starts_with("notdec.register.");
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

bool isKnownVarArgExternalFunction(const llvm::Function &function) {
  if (!function.isDeclaration()) {
    return false;
  }
  static const std::set<std::string> names = {
      "__asprintf_chk", "__fprintf_chk", "__isoc23_sscanf",
      "__isoc99_sscanf", "__printf_chk", "__snprintf_chk",
      "__sprintf_chk",  "__syslog_chk",  "__vasprintf_chk",
      "fcntl",          "fcntl64",       "fprintf",
      "fscanf",         "ioctl",         "open",
      "open64",         "printf",        "prctl",
      "snprintf",       "sscanf",        "syscall",
  };
  return names.count(function.getName().str()) != 0;
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
  for (auto it = target.StackSlots.begin(); it != target.StackSlots.end();) {
    auto sourceIt = source.StackSlots.find(it->first);
    if (sourceIt == source.StackSlots.end() || sourceIt->second != it->second) {
      it = target.StackSlots.erase(it);
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
  }

  NativeRegisterSummary run() {
    runBottomUp();
    runTopDownDemand();
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
          FunctionEffect next = analyzeFunction(*function);
          if (!(Effects[function] == next)) {
            Effects[function] = std::move(next);
            changed = true;
          }
        }
      }
      done.insert(id);
    };

    for (unsigned id = 0; id < components.size(); ++id) {
      process(id);
    }
  }

  FunctionEffect analyzeFunction(llvm::Function &function) {
    std::map<llvm::BasicBlock *, State> in;
    std::map<llvm::BasicBlock *, State> out;
    in[&function.getEntryBlock()].Reachable = true;

    bool changed = true;
    while (changed) {
      changed = false;
      for (llvm::BasicBlock &block : function) {
        if (&block != &function.getEntryBlock()) {
          State joined;
          for (llvm::BasicBlock *pred : llvm::predecessors(&block)) {
            joinState(joined, out[pred]);
          }
          if (!(in[&block] == joined)) {
            in[&block] = std::move(joined);
            changed = true;
          }
        }
        State next = transferBlock(block, in[&block]);
        if (!(out[&block] == next)) {
          out[&block] = std::move(next);
          changed = true;
        }
      }
    }

    State exits;
    for (llvm::BasicBlock &block : function) {
      if (llvm::isa<llvm::ReturnInst>(block.getTerminator())) {
        joinState(exits, exitStateWithSavedRegisters(out[&block]));
      }
    }
    FunctionEffect effect;
    if (exits.Reachable) {
      effect.Registers = std::move(exits.Cells);
    }
    return effect;
  }

  State transferBlock(llvm::BasicBlock &block, State state) {
    if (!state.Reachable) {
      return state;
    }
    for (llvm::Instruction &inst : block) {
      transferInstruction(inst, state);
    }
    return state;
  }

  void transferInstruction(llvm::Instruction &inst, State &state) {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
      RegisterAccess access = registerLoad(*load, Units);
      if (access.Unit != nullptr && !isIgnored(*access.Unit, Options)) {
        Cell before = cellIn(state, access.Unit->Global);
        bool keepHighUse = isKeepHighPartialLoadUse(*load, access.Unit->Global);
        if (!keepHighUse) {
          readRegister(state, access.Unit->Global);
        }
        if (before.MayEntry && !before.MayNonEntry) {
          state.ValueOrigins[load] = access.Unit->Global;
        } else {
          state.ValueOrigins.erase(load);
        }
        return;
      }
      if (std::optional<StackSlotKey> slot =
              fixedEntryStackSlot(load->getPointerOperand(), state)) {
        auto saved = state.StackSlots.find(*slot);
        if (saved != state.StackSlots.end()) {
          state.ValueOrigins[load] = saved->second;
        } else {
          state.ValueOrigins.erase(load);
        }
      }
      return;
    }
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      RegisterAccess access = registerStore(*store, Units);
      if (access.Unit != nullptr && !isIgnored(*access.Unit, Options)) {
        if (isKeepHighPartialStoreValue(store->getValueOperand(),
                                        access.Unit->Global)) {
          writeRegister(state, access.Unit->Global);
          state.ValueOrigins.erase(store);
          return;
        }
        llvm::GlobalVariable *origin =
            entryRegisterOrigin(store->getValueOperand(), state);
        if (origin == access.Unit->Global) {
          restoreRegister(state, access.Unit->Global);
        } else {
          writeRegister(state, access.Unit->Global);
        }
        state.ValueOrigins.erase(store);
        return;
      }
      if (std::optional<StackSlotKey> slot =
              fixedEntryStackSlot(store->getPointerOperand(), state)) {
        llvm::GlobalVariable *origin =
            entryRegisterOrigin(store->getValueOperand(), state);
        if (origin != nullptr) {
          state.StackSlots[*slot] = origin;
        } else {
          state.StackSlots.erase(*slot);
        }
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
          readRegister(state, partialRead->Global);
        }
        state.ValueOrigins.erase(&inst);
        return;
      }
      std::optional<NativeRegisterPartialWriteInfo> partial =
          parseNativeRegisterPartialWrite(*call);
      if (partial) {
        auto unitIt = Units.find(partial->Global);
        if (unitIt != Units.end() && !isIgnored(unitIt->second, Options)) {
          partialWriteRegister(state, partial->Global);
        }
        state.ValueOrigins.erase(&inst);
        return;
      }
    }
  }

  llvm::GlobalVariable *stackPointerGlobal() const {
    if (Abi.StackPointer.empty()) {
      return nullptr;
    }
    auto it = UnitsByName.find(Abi.StackPointer);
    return it == UnitsByName.end() ? nullptr : it->second;
  }

  llvm::GlobalVariable *entryRegisterOrigin(llvm::Value *value,
                                            const State &state) const {
    value = value->stripPointerCasts();
    auto it = state.ValueOrigins.find(value);
    if (it != state.ValueOrigins.end()) {
      return it->second;
    }
    return nullptr;
  }

  std::optional<std::pair<llvm::GlobalVariable *, int64_t>>
  entryAddressOrigin(llvm::Value *value, const State &state) const {
    value = value->stripPointerCasts();
    if (llvm::GlobalVariable *origin = entryRegisterOrigin(value, state)) {
      return std::make_pair(origin, 0);
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

    if (auto base = entryAddressOrigin(binary->getOperand(0), state)) {
      if (auto offset = parseConstantOffset(binary->getOperand(1),
                                            binary->getOpcode() ==
                                                llvm::Instruction::Sub)) {
        base->second += *offset;
        return base;
      }
    }
    if (binary->getOpcode() == llvm::Instruction::Add) {
      if (auto base = entryAddressOrigin(binary->getOperand(1), state)) {
        if (auto offset = parseConstantOffset(binary->getOperand(0), false)) {
          base->second += *offset;
          return base;
        }
      }
    }
    return std::nullopt;
  }

  std::optional<StackSlotKey> fixedEntryStackSlot(llvm::Value *pointer,
                                                  const State &state) const {
    if (auto slot = nativeStackAllocaSlot(pointer)) {
      return slot;
    }
    auto *intToPtr = llvm::dyn_cast<llvm::IntToPtrInst>(pointer);
    if (intToPtr == nullptr) {
      return std::nullopt;
    }
    auto address = entryAddressOrigin(intToPtr->getOperand(0), state);
    llvm::GlobalVariable *stackPointer = stackPointerGlobal();
    if (!address || stackPointer == nullptr || address->first != stackPointer) {
      return std::nullopt;
    }
    return StackSlotKey{stackPointer, address->second};
  }

  std::optional<StackSlotKey>
  nativeStackAllocaSlot(llvm::Value *pointer) const {
    auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(pointer);
    if (gep == nullptr || gep->getNumIndices() != 1) {
      return std::nullopt;
    }
    auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(
        gep->getPointerOperand()->stripPointerCasts());
    if (alloca == nullptr || !alloca->hasName() ||
        !alloca->getName().starts_with("notdec_stack.native")) {
      return std::nullopt;
    }
    auto *offset = llvm::dyn_cast<llvm::ConstantInt>(gep->idx_begin()->get());
    if (offset == nullptr || offset->getBitWidth() > 64) {
      return std::nullopt;
    }
    return StackSlotKey{alloca, offset->getSExtValue()};
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
      applyFunctionEffect(Effects[callee], state);
      return;
    }
    applyAbiCallEffect(state);
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
    }
  }

  void applyAbiCallEffect(State &state) {
    for (const auto &[global, unit] : Units) {
      if (isIgnored(unit, Options)) {
        continue;
      }
      if (Abi.Inputs.count(unit.Name) != 0) {
        readRegister(state, global);
      }
      if (Abi.Unaffected.count(unit.Name) != 0) {
        continue;
      }
      if (Abi.Outputs.count(unit.Name) != 0 ||
          Abi.KilledByCall.count(unit.Name) != 0) {
        writeRegister(state, global);
      }
    }
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
          addDemand(live, partial->Global,
                    valueDemand(partial->Value, valueDemands));
          continue;
        }
        if (!isNotDecRegisterHelperCall(*call)) {
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

    bool knownVarArgExternal =
        callee != nullptr && isKnownVarArgExternalFunction(*callee);
    for (const auto &[global, unit] : Units) {
      llvm::APInt mask = demandFor(live, global);
      if (isIgnored(unit, Options) || mask.getBitWidth() == 0 ||
          mask.isZero()) {
        continue;
      }
      if (Abi.Outputs.count(unit.Name) != 0 ||
          Abi.KilledByCall.count(unit.Name) != 0) {
        eraseDemand(live, global);
      }
    }
    if (!knownVarArgExternal) {
      return;
    }
    // For known vararg declarations, the fixed prototype does not tell us
    // which later ABI input registers are consumed.  Keep the complete ABI
    // input values live before the call so partial writes do not incorrectly
    // shrink the caller entry demand.
    for (const auto &[global, unit] : Units) {
      if (isIgnored(unit, Options) || Abi.Inputs.count(unit.Name) == 0) {
        continue;
      }
      addDemand(live, global, namedMaskOrFull(Abi.InputMasks, unit));
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
    }
  }

  NativeRegisterSummary buildPublicSummary() const {
    NativeRegisterSummary summary;
    summary.FunctionsSeen = Functions.size();
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
     << " loads=" << summary.LoadsSeen << " stores=" << summary.StoresSeen
     << " calls=" << summary.CallsSeen
     << " read_entry=" << summary.ReadEntryRegisters
     << " modified=" << summary.ModifiedRegisters
     << " preserved=" << summary.PreservedRegisters
     << " demanded_returns=" << summary.DemandedReturns << "\n";
  for (const NativeRegisterSummaryFunction &function : summary.Functions) {
    os << "  " << function.FunctionName << ": loads=" << function.LoadsSeen
       << " stores=" << function.StoresSeen << " calls=" << function.CallsSeen
       << " read_entry=" << function.ReadEntryRegisters
       << " modified=" << function.ModifiedRegisters
       << " preserved=" << function.PreservedRegisters
       << " demanded_returns=" << function.DemandedReturns << "\n";
  }
}

} // namespace notdec::bin2llvm
