#pragma once

#include <cstdint>

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace notdec::bin2llvm {

// Runs before register SummarySSA.  It rewrites raw full-register loads whose
// only live use is a smaller bit range, so SummarySSA sees the real demanded
// range instead of inventing a full-width entry value.
struct NativeRegisterPreSummaryPeepholeSummary {
  uint64_t FullRegisterLoadsSeen = 0;
  uint64_t Rewrites = 0;
  uint64_t DirectTruncRewrites = 0;
  uint64_t ShiftTruncRewrites = 0;
  uint64_t MultiUseLoadsSkipped = 0;
};

// Runs after register rewrite/signature rewrite and before final cleanup.  It
// removes temporary range glue while helpers are still explicit, before final
// cleanup lowers helpers to ordinary LLVM instructions and deletes residue.
struct NativeRegisterPostRewritePeepholeSummary {
  uint64_t ValueRangeExtractsSimplified = 0;
};

NativeRegisterPreSummaryPeepholeSummary
runNativeRegisterPreSummaryPeephole(llvm::Module &module);

NativeRegisterPostRewritePeepholeSummary
runNativeRegisterPostRewritePeephole(llvm::Module &module);

void printNativeRegisterPreSummaryPeepholeSummary(
    const NativeRegisterPreSummaryPeepholeSummary &summary,
    llvm::raw_ostream &os);

void printNativeRegisterPostRewritePeepholeSummary(
    const NativeRegisterPostRewritePeepholeSummary &summary,
    llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
