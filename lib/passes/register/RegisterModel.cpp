#include "RegisterModel.h"

#include "core/NotDecMetadata.h"

#include "notdec-bin2llvm/abi/NativeAbi.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterPartialRead.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterPartialWrite.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterValueRange.h"
#include "notdec-bin2llvm/intrinsics/NativeX87Intrinsic.h"

#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <algorithm>
#include <utility>

namespace notdec::bin2llvm {

std::string unitName(const llvm::GlobalVariable &global) {
  if (auto name = mdField(global.getMetadata("notdec.register"), "name")) {
    if (!name->empty()) {
      return *name;
    }
  }
  return global.getName().str();
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

std::string unitSpace(const llvm::GlobalVariable &global) {
  if (auto space = mdField(global.getMetadata("notdec.register"), "space")) {
    return *space;
  }
  return "";
}

RegisterUnitMap collectRegisterUnits(llvm::Module &module) {
  RegisterUnitMap units;
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

std::vector<const RegisterUnit *> unitsInNameOrder(const RegisterUnitMap &units) {
  std::vector<const RegisterUnit *> ordered;
  ordered.reserve(units.size());
  for (const auto &[global, unit] : units) {
    (void)global;
    ordered.push_back(&unit);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const RegisterUnit *lhs, const RegisterUnit *rhs) {
              if (lhs->Name != rhs->Name) {
                return lhs->Name < rhs->Name;
              }
              if (lhs->Space != rhs->Space) {
                return lhs->Space < rhs->Space;
              }
              if (lhs->Offset != rhs->Offset) {
                return lhs->Offset < rhs->Offset;
              }
              return lhs->Size < rhs->Size;
            });
  return ordered;
}

std::vector<const RegisterUnit *>
unitsInCreationOrder(const llvm::Module &module, const RegisterUnitMap &units) {
  std::vector<const RegisterUnit *> ordered;
  ordered.reserve(units.size());
  for (const llvm::GlobalVariable &global : module.globals()) {
    auto it = units.find(const_cast<llvm::GlobalVariable *>(&global));
    if (it != units.end()) {
      ordered.push_back(&it->second);
    }
  }
  return ordered;
}

std::string storageUnitName(const NativeAbiStorage &storage,
                            const RegisterUnitMap &units) {
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

bool isSegmentBaseUnit(llvm::StringRef name) {
  return name == "FS_OFFSET" || name == "GS_OFFSET";
}

bool isX87WindowShiftLoad(llvm::LoadInst &load,
                          const RegisterUnitMap &units) {
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
  // Only loads whose every use is a window-internal move (store to a register
  // global / notdec.x87.* call) count.  fld %st(0) and fstpt may share one
  // load (cache reuse); the latter stores the value to memory, which is a real
  // consumption and must not be skipped.
  if (load.getNumUses() == 0) {
    return false;
  }
  for (llvm::User *user : load.users()) {
    llvm::Value *value = user;
    // Strip the bitcast/zext/trunc single-use chain to find the real consumer.
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
      if (callee != nullptr && isNativeX87IntrinsicName(callee->getName())) {
        internal = true;
      }
    }
    if (!internal) {
      return false;
    }
  }
  return true;
}

RegisterAccess registerLoad(llvm::LoadInst &load, const RegisterUnitMap &units,
                            RegisterReadOptions options) {
  // x87 window moves (push/pop move the old top into the adjacent slot) are
  // not real reads in the summary model, so they return an empty access by
  // default.  Range rewriting must see them: the lifter already writes the move
  // explicitly as `store old ST0 -> @ST1`, and the range state holds that value;
  // if the move load kept reading @ST1, it would get the stale value that the
  // summary no longer maintains (or whose write was deleted).
  if (!options.IncludeX87WindowShift &&
      (load.getMetadata("notdec.x87.window.shift") != nullptr ||
       isX87WindowShiftLoad(load, units))) {
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
  return RegisterAccess{&it->second, true,
                        load.getType() == global->getValueType()};
}

RegisterAccess registerStore(llvm::StoreInst &store,
                             const RegisterUnitMap &units) {
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
  return RegisterAccess{&it->second, true,
                        store.getValueOperand()->getType() ==
                            global->getValueType()};
}

bool isNotDecRegisterHelperCall(const llvm::CallBase &call) {
  llvm::Function *callee = call.getCalledFunction();
  if (callee == nullptr) {
    return false;
  }
  // notdec.register.* is the namespace of the register model intrinsics
  // (unknown placeholders, frame keep-alive, ...).  They are not real external
  // functions: their effect on registers is expressed explicitly by their
  // pointer argument and return value, and they take part in neither external
  // argument inference nor ABI clobber derivation.  New intrinsics use this
  // prefix, so the list below never needs extending.  The other prefixes are
  // historical but carry the same meaning.
  return callee->getName().starts_with("notdec.register.") ||
         callee->getName().starts_with("notdec.unknown.") ||
         isNativeX87IntrinsicName(callee->getName()) ||
         isNativeRegisterValueRangeName(callee->getName());
}

bool isAnalyzableCall(const llvm::CallBase &call) {
  if (isNotDecRegisterHelperCall(call) ||
      parseNativeRegisterPartialRead(call).has_value() ||
      parseNativeRegisterPartialWrite(call).has_value()) {
    return false;
  }
  llvm::Function *callee = call.getCalledFunction();
  return callee == nullptr || !callee->isIntrinsic();
}

bool isAnalyzableCall(const llvm::Instruction &inst) {
  auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
  return call != nullptr && isAnalyzableCall(*call);
}

llvm::APInt intersectBitMasks(std::initializer_list<llvm::APInt> masks) {
  unsigned width = 0;
  for (const llvm::APInt &mask : masks) {
    width = std::max(width, mask.getBitWidth());
  }
  if (width == 0) {
    return llvm::APInt();
  }
  llvm::APInt result = llvm::APInt::getAllOnes(width);
  for (const llvm::APInt &mask : masks) {
    result &= mask.zextOrTrunc(width);
  }
  return result;
}

} // namespace notdec::bin2llvm
