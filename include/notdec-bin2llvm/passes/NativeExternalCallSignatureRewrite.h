#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace notdec::bin2llvm {

struct NativeExternalCallSignatureRewriteOptions {
  bool PrintSummary = false;
};

struct NativeExternalCallSignatureRewriteFunctionSummary {
  std::string FunctionName;
  uint64_t CallsSeen = 0;
  uint64_t CallsRewritten = 0;
  uint64_t StoresRemoved = 0;
  uint64_t SymbolsSkippedForConflict = 0;
};

struct NativeExternalCallSignatureRewriteSummary {
  uint64_t FunctionsSeen = 0;
  uint64_t CallsSeen = 0;
  uint64_t CallsRewritten = 0;
  uint64_t StoresRemoved = 0;
  uint64_t SymbolsSkippedForConflict = 0;
  uint64_t SymbolsResolvedWithKnownPrototype = 0;
  uint64_t SymbolsResolvedWithMinimumArgs = 0;
  uint64_t CallsSkippedForMissingKnownArgs = 0;
  std::vector<NativeExternalCallSignatureRewriteFunctionSummary> Functions;
};

NativeExternalCallSignatureRewriteSummary
runNativeExternalCallSignatureRewrite(
    llvm::Module &module,
    const NativeExternalCallSignatureRewriteOptions &options = {});

void printNativeExternalCallSignatureRewriteSummary(
    const NativeExternalCallSignatureRewriteSummary &summary,
    llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
