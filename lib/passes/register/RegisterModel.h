#pragma once

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace llvm {
class CallBase;
class GlobalVariable;
class Instruction;
class LoadInst;
class Module;
class Module;
class StoreInst;
} // namespace llvm

namespace notdec::bin2llvm {

struct NativeAbiStorage;

// One lifted register global (@RAX, @ZMM0, @FS_OFFSET ...) with the metadata
// that names its space/offset/size.  This is the read-only base shared by the
// register effect analysis and the elimination pass; the two passes keep their
// own analysis state on top of it.
struct RegisterUnit {
  llvm::GlobalVariable *Global = nullptr;
  std::string Space;
  std::string Name;
  uint64_t Offset = 0;
  uint64_t Size = 0;
};

using RegisterUnitMap = std::map<llvm::GlobalVariable *, RegisterUnit>;

RegisterUnitMap collectRegisterUnits(llvm::Module &module);

// Deterministic (name, space, offset) order.  RegisterUnitMap is keyed by the
// global pointer, so iterating it directly makes emitted metadata depend on
// heap layout; anything that writes IR or JSON must use this instead.
std::vector<const RegisterUnit *> unitsInNameOrder(const RegisterUnitMap &units);

// Module global creation order.  The lifter creates register globals in a fixed
// order, and the original pointer-keyed maps happened to iterate in that order;
// IR-generation code uses this order so value names stay reproducible.
std::vector<const RegisterUnit *>
unitsInCreationOrder(const llvm::Module &module, const RegisterUnitMap &units);

std::string unitName(const llvm::GlobalVariable &global);
uint64_t unitOffset(const llvm::GlobalVariable &global);
uint64_t unitSize(const llvm::GlobalVariable &global);
std::string unitSpace(const llvm::GlobalVariable &global);

// Maps a cspec storage name (e.g. XMM0_Qa) onto the lifted register global that
// backs it (e.g. ZMM0).  Falls back to the storage name when nothing overlaps.
std::string storageUnitName(const NativeAbiStorage &storage,
                            const RegisterUnitMap &units);

bool isSegmentBaseUnit(llvm::StringRef name);

struct RegisterAccess {
  const RegisterUnit *Unit = nullptr;
  bool IsRegisterAccess = false;
  bool IsStorageValue = false;
};

struct RegisterReadOptions {
  // x87 window moves (push/pop/fxch) are not real reads of the register value
  // in the summary model, but range rewriting needs to see them; see the note
  // in RegisterModel.cpp.
  bool IncludeX87WindowShift = false;
};

RegisterAccess registerLoad(llvm::LoadInst &load, const RegisterUnitMap &units,
                            RegisterReadOptions options = {});
RegisterAccess registerStore(llvm::StoreInst &store,
                             const RegisterUnitMap &units);

// True when every use of the load is an x87 window move (store to another
// register global or a notdec.x87.* call).  Used as the fallback when
// InstCombine dropped the notdec.x87.window.shift marker.
bool isX87WindowShiftLoad(llvm::LoadInst &load, const RegisterUnitMap &units);

// notdec.register.* / notdec.unknown.* / notdec.reg.extract|insert.* / x87
// library calls are part of the lifting model, not real external functions.
bool isNotDecRegisterHelperCall(const llvm::CallBase &call);
bool isAnalyzableCall(const llvm::CallBase &call);
// Convenience overload for instruction walks: non-call instructions are not
// analyzable calls.
bool isAnalyzableCall(const llvm::Instruction &inst);

llvm::APInt intersectBitMasks(std::initializer_list<llvm::APInt> masks);

} // namespace notdec::bin2llvm
