#pragma once

#include <cstdint>

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace notdec::bin2llvm {

// Runs after register rewrite/signature rewrite and before final cleanup.  It
// removes temporary range glue while helpers are still explicit, before final
// cleanup lowers helpers to ordinary LLVM instructions and deletes residue.
// It also folds the compiler's constant-division idiom `(x * magic) >> shift`
// back into `udiv/sdiv x, d` (see the .cpp for why the fold is only done when
// the divisor is provably recovered).
struct NativeRegisterPostRewritePeepholeSummary {
  uint64_t ValueRangeExtractsSimplified = 0;
  uint64_t ConstantDivisionsRecognized = 0;
  uint64_t SignedConstantDivisionsRecognized = 0;
};

NativeRegisterPostRewritePeepholeSummary
runNativeRegisterPostRewritePeephole(llvm::Module &module);

void printNativeRegisterPostRewritePeepholeSummary(
    const NativeRegisterPostRewritePeepholeSummary &summary,
    llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
