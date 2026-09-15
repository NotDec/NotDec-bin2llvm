#pragma once

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <string>

namespace llvm {
class Module;
} // namespace llvm

namespace notdec::bin2llvm {

// One non-executable loadable range of the input image.  Bytes only covers the
// file-backed part; a range may be larger than Bytes (NOBITS/.bss tail), and
// the missing bytes are modeled as zero.
struct NativeDataImageSegment {
  uint64_t Address = 0;
  uint64_t Size = 0;
  bool Executable = false;
  llvm::ArrayRef<uint8_t> Bytes;
};

// A pointer-sized relocation slot whose computed target is a local function.
// The data image keeps the numeric bytes for every other slot; these slots get
// a symbolic ptrtoint(@function) initializer so the function body stays
// referenced and later promotion can see the target.
struct NativeDataImageFunctionSlot {
  uint64_t Address = 0;
  uint64_t Width = 0;
  std::string FunctionName;
};

struct NativeDataImageOptions {
  // 0 keeps every referenced segment.  A non-zero value skips segments whose
  // virtual size exceeds the limit, which bounds IR growth on huge binaries.
  uint64_t MaxSegmentSize = 0;
};

struct NativeDataImageSummary {
  uint64_t SegmentsCreated = 0;
  uint64_t SegmentsSkipped = 0;
  uint64_t FunctionSlots = 0;
  uint64_t StaticAccesses = 0;
  uint64_t BaseOffsetAccesses = 0;
  uint64_t UnresolvedIntToPtrs = 0;
  uint64_t FallbackSlots = 0;
};

// Data image modeling: turn referenced non-executable loadable ranges into real
// LLVM globals holding the segment bytes, then rewrite inttoptr address chains
// that provably point into one of those globals.
//
// The global is a packed struct of byte arrays plus one integer field per
// function-pointer slot, so every byte offset stays exactly at its virtual
// address and symbolic function initializers survive.  Accesses are rewritten
// as i8 GEPs, which keeps constant-address and base+offset accesses of the same
// segment inside one LLVM object.
//
// Addresses outside the modeled segments (stack, heap, parameters, values
// loaded from memory) are intentionally left as raw inttoptr; connecting those
// needs interprocedural value propagation and is a later stage.
//
// knownAddressBases lists constants the frontend already knows are data
// addresses (relocation slot addresses and relocation values).  A dynamic
// base+offset access is only rewritten when its constant anchor is one of
// them, or a constant that appears as a direct inttoptr target in the module.
// Without that filter a small integer such as 8 or 65535 can fall inside a
// segment by accident and would redirect unrelated memory accesses.
NativeDataImageSummary materializeNativeDataImage(
    llvm::Module &module,
    llvm::ArrayRef<NativeDataImageSegment> segments,
    llvm::ArrayRef<NativeDataImageFunctionSlot> slots,
    llvm::ArrayRef<uint64_t> knownAddressBases,
    const NativeDataImageOptions &options = {});

} // namespace notdec::bin2llvm
