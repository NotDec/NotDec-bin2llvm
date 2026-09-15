#pragma once

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <string>

namespace llvm {
class Module;
} // namespace llvm

namespace notdec::bin2llvm {

// A relocated pointer slot in the native data image.  The slot stores a
// pointer-sized value and, when FunctionName is non-empty, the relocation
// target is a known local function.
struct NativeFunctionPointerSlot {
  uint64_t Address = 0;
  uint64_t Width = 0;
  std::string FunctionName;
};

struct NativeRelocationDataSummary {
  uint64_t SlotsCreated = 0;
  uint64_t SlotsReused = 0;
  uint64_t AccessesRewritten = 0;
};

// First-stage relocation modeling: represent known function-pointer slots as
// real LLVM globals whose initializer is ptrtoint(@function).  This preserves
// the slot value and the referenced function body, but intentionally only
// rewrites memory accesses whose pointer is the slot address itself.  Pointer
// arithmetic through a larger data image is a later stage.
NativeRelocationDataSummary
materializeNativeFunctionPointerSlots(
    llvm::Module &module,
    llvm::ArrayRef<NativeFunctionPointerSlot> slots);

} // namespace notdec::bin2llvm
