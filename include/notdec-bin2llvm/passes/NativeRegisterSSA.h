#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace notdec::bin2llvm {

struct NativeRegisterSSAOptions {
  bool EnableRewrite = true;
  bool PrintSummary = false;
};

struct NativeRegisterSSAFunctionSummary {
  std::string FunctionName;
  uint64_t LoadsSeen = 0;
  uint64_t StoresSeen = 0;
  uint64_t LoadsReplaced = 0;
  uint64_t DeadStoresRemoved = 0;
  uint64_t UnreadFlagStoresRemoved = 0;
  uint64_t UnreadRipStoresRemoved = 0;
  uint64_t PhisCreated = 0;
  uint64_t PhisSimplified = 0;
  uint64_t ExternalInputs = 0;
  uint64_t CallsSeen = 0;
  uint64_t PreservedRegisters = 0;
  uint64_t ClobberedRegisters = 0;
  uint64_t CallInputHelpers = 0;
  uint64_t CallReturnHelpers = 0;
  uint64_t CallEffectHelpers = 0;
  uint64_t StrongCallInputs = 0;
  uint64_t WeakCallInputs = 0;
  uint64_t BlockedCallInputs = 0;
};

struct NativeRegisterSSASummary {
  uint64_t FunctionsSeen = 0;
  uint64_t LoadsSeen = 0;
  uint64_t StoresSeen = 0;
  uint64_t LoadsReplaced = 0;
  uint64_t DeadStoresRemoved = 0;
  uint64_t UnreadFlagStoresRemoved = 0;
  uint64_t UnreadRipStoresRemoved = 0;
  uint64_t PhisCreated = 0;
  uint64_t PhisSimplified = 0;
  uint64_t ExternalInputs = 0;
  uint64_t CallsSeen = 0;
  uint64_t PreservedRegisters = 0;
  uint64_t ClobberedRegisters = 0;
  uint64_t CallInputHelpers = 0;
  uint64_t CallReturnHelpers = 0;
  uint64_t CallEffectHelpers = 0;
  uint64_t StrongCallInputs = 0;
  uint64_t WeakCallInputs = 0;
  uint64_t BlockedCallInputs = 0;
  std::vector<NativeRegisterSSAFunctionSummary> Functions;
};

// This pass promotes RegisterStorage-backed globals into SSA values. Integer
// partial accesses are rewritten through the full backing value, while flags
// and vector lanes stay conservative.
NativeRegisterSSASummary
runNativeRegisterSSA(llvm::Module &module,
                     const NativeRegisterSSAOptions &options = {});

void printNativeRegisterSSASummary(const NativeRegisterSSASummary &summary,
                                   llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
