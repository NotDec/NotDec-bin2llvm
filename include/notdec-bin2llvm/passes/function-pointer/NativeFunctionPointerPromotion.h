#pragma once

#include <cstdint>

namespace llvm {
class Module;
} // namespace llvm

namespace notdec::bin2llvm {

struct NativeFunctionPointerPromotionOptions {
  // Reserved for guarded multi-target promotion.  The current version only
  // promotes slots with a single known target.
  unsigned MaxGuardedTargets = 0;
};

struct NativeFunctionPointerPromotionSummary {
  uint64_t IndirectCallsSeen = 0;
  uint64_t IndirectCallsPromoted = 0;
  uint64_t SlotsWithKnownSingleTarget = 0;
  uint64_t SlotsWithUnknownWrites = 0;
  uint64_t SlotsWithEscapedAddress = 0;
  uint64_t SlotsWithMultipleTargets = 0;
};

// Promote indirect calls fed by a relocation function-pointer slot to direct
// calls.  Slot facts come from !notdec.relocation.function_pointer metadata
// (see NativeRelocationMetadata.h); the lifted IR itself is not rewritten.
//
// The matcher is deliberately small and only handles the common shape
//
//   load i64, ptr inttoptr (<slot address>) -> inttoptr/bitcast -> call %fn
//
// and refuses to promote when the slot is written with an unknown or different
// value, when the address escapes, or when the chain cannot be proven.
NativeFunctionPointerPromotionSummary runNativeFunctionPointerPromotion(
    llvm::Module &module,
    const NativeFunctionPointerPromotionOptions &options = {});

} // namespace notdec::bin2llvm
