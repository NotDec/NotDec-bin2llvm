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
  uint64_t DeadUnknownValuesRemoved = 0;
  uint64_t RegisterGlobalsRemoved = 0;
  uint64_t HelperDeclarationsRemoved = 0;
  uint64_t ValueRangeHelpersLowered = 0;
  uint64_t VarArgUnknownHelpersLowered = 0;
  uint64_t FunctionMetadataCleared = 0;
  uint64_t InstructionMetadataCleared = 0;
  uint64_t RemainingRegisterAccesses = 0;
};

NativeRegisterFinalCleanupSummary runNativeRegisterFinalCleanup(
    llvm::Module &module,
    const NativeRegisterFinalCleanupOptions &options = {});

// 清掉没人用的 notdec.unknown.* 占位调用。runNativeRegisterFinalCleanup 内部会
// 调一次；tool 在最后一次 InstCombine 之后还要再调一次——那次 InstCombine 可能
// 删掉某个 unknown 的最后一个使用者，而调用指令本身有副作用语义、不会被当死代码
// 删掉。
uint64_t eraseDeadUnknownValueCalls(llvm::Module &module);

void printNativeRegisterFinalCleanupSummary(
    const NativeRegisterFinalCleanupSummary &summary, llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
