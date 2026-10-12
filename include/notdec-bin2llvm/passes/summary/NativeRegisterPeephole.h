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
  // GCC 的字节 switch 跳转表：下标 c - lo 在 32 位里算、只有低字节被比较和
  // 重扩展（见 .cpp 的 narrowByteSwitchIndexRead）。这类 32 位读的高位没有
  // 观察者，收窄成 8 位读后 SummarySSA 才不会把它记成入口依赖。
  uint64_t ByteIndexCandidates = 0;
  uint64_t ByteIndexRewrites = 0;
  uint64_t ByteIndexSkipped = 0;
  // 提升器自己发的标志位物化（OF/SF/ZF/PF/CF）：一个 flag store 到下一次同 flag
  // 的写/调用/函数出口之前没有任何读，就是死的，连同它的计算链一起删掉
  // （见 .cpp 的 dead flag value 一段）。标志位不是编译器模式，只认这一条逻辑。
  uint64_t FlagStoresSeen = 0;
  uint64_t DeadFlagStoresRemoved = 0;
  uint64_t DeadFlagValuesRemoved = 0;
};

// Runs after register rewrite/signature rewrite and before final cleanup.  It
// removes temporary range glue while helpers are still explicit, before final
// cleanup lowers helpers to ordinary LLVM instructions and deletes residue.
// It also folds the compiler's unsigned-constant-division idiom
// `(x * magic) >> shift` back into `udiv x, d` (see the .cpp for why the
// fold is only done when the divisor is provably recovered).
struct NativeRegisterPostRewritePeepholeSummary {
  uint64_t ValueRangeExtractsSimplified = 0;
  uint64_t ConstantDivisionsRecognized = 0;
  uint64_t SignedConstantDivisionsRecognized = 0;
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
