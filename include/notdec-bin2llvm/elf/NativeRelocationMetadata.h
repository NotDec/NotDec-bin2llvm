#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <cstdint>
#include <string>

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace notdec::bin2llvm {

// A relocation slot whose computed value is a known local function entry.
// This is the only relocation fact the native pipeline records on the IR
// itself; the matcher in NativeFunctionPointerPromotion consumes it.
struct NativeFunctionPointerSlot {
  uint64_t Address = 0;
  uint64_t Width = 0;
  std::string FunctionName;
};

// Metadata schema (module named metadata):
//
//   !notdec.relocation.function_pointer = !{!0, !1, ...}
//   !0 = !{i64 <address>, i64 <width>, !"<function name>"}
//
// Attaching metadata does not rewrite any instruction: the lifted IR keeps its
// absolute-address inttoptr form.  Passes that want to reason about relocated
// function addresses read the metadata back.
void attachNativeFunctionPointerMetadata(
    llvm::Module &module, llvm::ArrayRef<NativeFunctionPointerSlot> slots);

struct NativeFunctionPointerSlotInfo {
  uint64_t Address = 0;
  uint64_t Width = 0;
  // Null when the recorded function is missing from the module (for example
  // when the slot was optimized away).
  llvm::Function *Target = nullptr;
};

llvm::SmallVector<NativeFunctionPointerSlotInfo, 16>
readNativeFunctionPointerMetadata(llvm::Module &module);

} // namespace notdec::bin2llvm
