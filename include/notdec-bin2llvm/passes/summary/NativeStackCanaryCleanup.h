#pragma once

#include <cstdint>

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace notdec::bin2llvm {

struct NativeStackCanaryCleanupOptions {
  bool PrintSummary = false;
};

// Summary-chain stack canary cleanup.  This pass only removes compiler-inserted
// stack-protector epilogues that compare a saved stack slot with a known TLS
// canary slot and branch to __stack_chk_fail on mismatch.  It does not model
// general TLS or segment-base semantics.
struct NativeStackCanaryCleanupSummary {
  uint64_t FunctionsSeen = 0;
  uint64_t CanaryChecksRemoved = 0;
  uint64_t FailBlocksRemoved = 0;
};

NativeStackCanaryCleanupSummary runNativeStackCanaryCleanup(
    llvm::Module &module, const NativeStackCanaryCleanupOptions &options = {});

void printNativeStackCanaryCleanupSummary(
    const NativeStackCanaryCleanupSummary &summary, llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
