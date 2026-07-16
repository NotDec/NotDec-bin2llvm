#pragma once

#include <cstdint>

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace notdec::bin2llvm {

struct NativeRegisterFinalCleanupOptions {
  bool RunGlobalDCE = true;
  bool PrintSummary = false;
};

// Final native-IR cleanup after register SSA and signature rewrite have already
// consumed register summary metadata.  It only removes facts that are no longer
// referenced by the LLVM IR, so earlier analysis passes can keep using the
// richer metadata during the pipeline.
struct NativeRegisterFinalCleanupSummary {
  uint64_t FunctionsSeen = 0;
  uint64_t FunctionsWithoutRegisterResidue = 0;
  uint64_t DeadRegisterReadsRemoved = 0;
  uint64_t RegisterGlobalsRemoved = 0;
  uint64_t HelperDeclarationsRemoved = 0;
  uint64_t ValueRangeExtractsSimplified = 0;
  uint64_t ValueRangeHelpersLowered = 0;
  uint64_t FunctionMetadataCleared = 0;
  uint64_t InstructionMetadataCleared = 0;
  uint64_t RemainingRegisterAccesses = 0;
};

NativeRegisterFinalCleanupSummary runNativeRegisterFinalCleanup(
    llvm::Module &module,
    const NativeRegisterFinalCleanupOptions &options = {});

void printNativeRegisterFinalCleanupSummary(
    const NativeRegisterFinalCleanupSummary &summary, llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
