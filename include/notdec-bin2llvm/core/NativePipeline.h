#pragma once

#include "notdec-bin2llvm/passes/register/NativeRegisterElimination.h"
#include "notdec-bin2llvm/passes/register/NativeRegisterFinalCleanup.h"
#include "notdec-bin2llvm/passes/register/NativeRegisterPostRewritePeephole.h"

#include <set>
#include <string>

namespace llvm {
class Module;
} // namespace llvm

namespace notdec::bin2llvm {

// Knobs of the LLVM pass pipeline that `notdec-native-llvm` runs after lifting.
// CLI parsing stays in the tool; the stages below are reusable from tests.
struct NativePipelineOptions {
  bool DisableRegisterElimination = false;
  bool DisableResidueRemoval = false;
  bool DisableInstCombine = false;
  bool DisableVarArgRecovery = false;
  bool AbiForAllFunctions = false;
  bool PrintRegisterSummary = false;
  std::string ExternalPrototypeJsonPath;
};

struct NativeRegisterEliminationStageResult {
  bool Ok = true;
  NativeRegisterEliminationSummary Summary;
};

NativeRegisterEliminationStageResult
runNativeRegisterEliminationStage(llvm::Module &module,
                                  const NativePipelineOptions &options);

// EarlyCSE + InstCombine + SimplifyCFG.  EarlyCSE must run after register
// rewriting: the model rebuilds the same lane insert/extract chain per use, and
// LLVM cannot see the duplicates without it.
bool runNativeInstCombineStage(llvm::Module &module,
                               const NativePipelineOptions &options,
                               bool runEarlyCSEFirst = false);

bool runNativePostRewritePeepholeStage(llvm::Module &module,
                                       const NativePipelineOptions &options);

bool runNativeFinalCleanupStage(
    llvm::Module &module,
    const std::set<std::string> &preservedFunctions = {});

} // namespace notdec::bin2llvm
