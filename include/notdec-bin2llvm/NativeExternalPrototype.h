#pragma once

#include "llvm/ADT/StringRef.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace notdec::bin2llvm {

// External library prototypes used by native register SSA.  These are not
// recovered program prototypes; they are trusted declarations for known imported
// functions such as libc calls.  The storage-independent type is lowered through
// the active ABI when SummarySSA rewrites call signatures.
struct NativeExternalPrototype {
  enum class ValueType {
    I32,
    I64,
    Float,
    Double,
  };

  unsigned FixedArgs = 0;
  bool VarArg = false;
  bool NoReturn = false;
  unsigned MaxReturnRegisters = 1;
  std::vector<ValueType> TypedParams;
  std::optional<ValueType> TypedReturn;
};

using NativeExternalPrototypeMap =
    std::map<std::string, NativeExternalPrototype, std::less<>>;

const NativeExternalPrototypeMap &defaultNativeExternalPrototypes();

const NativeExternalPrototype *
lookupNativeExternalPrototype(const NativeExternalPrototypeMap &prototypes,
                              llvm::StringRef name);

std::optional<NativeExternalPrototypeMap>
loadNativeExternalPrototypesJson(llvm::StringRef path,
                                 std::string &errorMessage);

} // namespace notdec::bin2llvm
