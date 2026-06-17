#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace notdec::bin2llvm {

struct NativeHeritageSSAOptions {
  bool EnableRewrite = true;
  bool PrintSummary = false;
};

struct NativeHeritageSSAFunctionSummary {
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
  uint64_t ActiveCallInputTrials = 0;
  uint64_t InactiveCallInputTrials = 0;
  uint64_t NoUseCallInputTrials = 0;
  uint64_t BlockedCallInputTrials = 0;
  uint64_t DefinitelyNotUsedCallInputTrials = 0;
  uint64_t KilledByCallInputTrials = 0;
  uint64_t ConditionalEffectCallInputTrials = 0;
  uint64_t ConditionalFinalCheckCallInputTrials = 0;
  uint64_t PathRealisticCallInputTrials = 0;
  uint64_t PathConditionalCallInputTrials = 0;
  uint64_t PathBlockedCallInputTrials = 0;
  uint64_t LocalDefCallInputTrials = 0;
  uint64_t LocalConstCallInputTrials = 0;
  uint64_t LocalArithCallInputTrials = 0;
  uint64_t LocalCastCallInputTrials = 0;
  uint64_t LocalLoadCallInputTrials = 0;
  uint64_t LocalUnknownCallInputTrials = 0;
  uint64_t LocalSharedUseCallInputTrials = 0;
  uint64_t LocalDoubleCallUseCallInputTrials = 0;
  uint64_t PhiCallInputTrials = 0;
  uint64_t EntryInputCallInputTrials = 0;
  uint64_t CallEffectCallInputTrials = 0;
  uint64_t ReturnForwardCallInputTrials = 0;
};

struct NativeHeritageSSASummary {
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
  uint64_t ActiveCallInputTrials = 0;
  uint64_t InactiveCallInputTrials = 0;
  uint64_t NoUseCallInputTrials = 0;
  uint64_t BlockedCallInputTrials = 0;
  uint64_t DefinitelyNotUsedCallInputTrials = 0;
  uint64_t KilledByCallInputTrials = 0;
  uint64_t ConditionalEffectCallInputTrials = 0;
  uint64_t ConditionalFinalCheckCallInputTrials = 0;
  uint64_t PathRealisticCallInputTrials = 0;
  uint64_t PathConditionalCallInputTrials = 0;
  uint64_t PathBlockedCallInputTrials = 0;
  uint64_t LocalDefCallInputTrials = 0;
  uint64_t LocalConstCallInputTrials = 0;
  uint64_t LocalArithCallInputTrials = 0;
  uint64_t LocalCastCallInputTrials = 0;
  uint64_t LocalLoadCallInputTrials = 0;
  uint64_t LocalUnknownCallInputTrials = 0;
  uint64_t LocalSharedUseCallInputTrials = 0;
  uint64_t LocalDoubleCallUseCallInputTrials = 0;
  uint64_t PhiCallInputTrials = 0;
  uint64_t EntryInputCallInputTrials = 0;
  uint64_t CallEffectCallInputTrials = 0;
  uint64_t ReturnForwardCallInputTrials = 0;
  std::vector<NativeHeritageSSAFunctionSummary> Functions;
};

// Heritage SSA is the old Ghidra-style register elimination path.  It is kept
// as an explicit fallback while the summary-based SSA pass is the default path.
NativeHeritageSSASummary
runNativeHeritageSSA(llvm::Module &module,
                     const NativeHeritageSSAOptions &options = {});

void printNativeHeritageSSASummary(const NativeHeritageSSASummary &summary,
                                   llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
