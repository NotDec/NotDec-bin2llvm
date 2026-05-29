#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace notdec::bin2llvm {

struct NativePrototypeRecoveryOptions {
  bool PrintSummary = false;
};

// Native copy of Ghidra ParamTrial for the first input-recovery step.  It only
// models register inputs for now, but keeps ABI slot and active state so later
// work can add no-use, joining, and return candidates without changing the
// metadata shape.
struct NativeParamTrial {
  std::string RegisterName;
  uint64_t Slot = 0;
  // Optional simple value identity used by return recovery.  It is intentionally
  // small: unknown values are left unset so this first pass stays conservative.
  std::optional<std::string> ValueKey;
  bool Active = false;
};

// Small container matching Ghidra ParamActive's role: keep the trials currently
// being considered for one function.
struct NativeParamActive {
  std::vector<NativeParamTrial> Trials;
};

struct NativePrototypeRecoveryFunctionSummary {
  std::string FunctionName;
  uint64_t ExternalInputsSeen = 0;
  uint64_t InputCandidates = 0;
  uint64_t ReturnCandidates = 0;
};

struct NativePrototypeRecoverySummary {
  uint64_t FunctionsSeen = 0;
  uint64_t ExternalInputsSeen = 0;
  uint64_t InputCandidates = 0;
  uint64_t ReturnCandidates = 0;
  std::vector<NativePrototypeRecoveryFunctionSummary> Functions;
};

NativePrototypeRecoverySummary
runNativePrototypeRecovery(llvm::Module &module,
                           const NativePrototypeRecoveryOptions &options = {});

void printNativePrototypeRecoverySummary(
    const NativePrototypeRecoverySummary &summary, llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
