#pragma once

#include <cstdint>

namespace llvm {
class Module;
} // namespace llvm

namespace notdec::bin2llvm {

struct NativeFunctionPointerPromotionOptions {
  // Reserved for guarded multi-target promotion.  The first version only
  // promotes slots with a single known target.
  unsigned MaxGuardedTargets = 0;
};

struct NativeFunctionPointerPromotionSummary {
  uint64_t IndirectCallsSeen = 0;
  uint64_t IndirectCallsPromoted = 0;
  uint64_t SlotsWithKnownSingleTarget = 0;
  uint64_t SlotsWithUnknownWrites = 0;
  uint64_t SlotsWithMultipleTargets = 0;
};

// Promote indirect calls fed by a native relocation function-pointer slot to
// direct calls.  The initial version is deliberately conservative: it only
// promotes when the slot has a known initializer and no module-visible store
// with an unknown or different target.
NativeFunctionPointerPromotionSummary runNativeFunctionPointerPromotion(
    llvm::Module &module,
    const NativeFunctionPointerPromotionOptions &options = {});

} // namespace notdec::bin2llvm
