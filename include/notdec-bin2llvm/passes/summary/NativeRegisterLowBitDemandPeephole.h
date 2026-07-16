#pragma once

#include <cstdint>

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace notdec::bin2llvm {

// Rewrites raw full-register loads whose only real use is a narrow low-bit
// extraction.  SLEIGH sometimes emits a full-width register read for x86
// address calculation and then truncates the result.  Keeping that as a full
// load makes register-summary analysis believe unused high bits are real entry
// inputs.
struct NativeRegisterLowBitDemandPeepholeSummary {
  uint64_t FullRegisterLoadsSeen = 0;
  uint64_t Rewrites = 0;
  uint64_t DirectTruncRewrites = 0;
  uint64_t ShiftTruncRewrites = 0;
  uint64_t MultiUseLoadsSkipped = 0;
};

NativeRegisterLowBitDemandPeepholeSummary
runNativeRegisterLowBitDemandPeephole(llvm::Module &module);

void printNativeRegisterLowBitDemandPeepholeSummary(
    const NativeRegisterLowBitDemandPeepholeSummary &summary,
    llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
