#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace notdec::bin2llvm {

struct NativeRegisterSummarySSAOptions {
  bool EnableRewrite = true;
  bool EnableResidueRemoval = true;
  // Signature rewrite can expose new dead register stores only after LLVM has
  // folded the now-local dataflow.  Keep this behind an option so
  // --no-instcombine-pass can still disable all InstCombine work.
  bool EnablePostRewriteInstCombine = true;
  bool AttachMetadata = true;
  bool PrintSummary = false;
  std::set<std::string> IgnoredRegisters;
};

struct NativeRegisterSummarySSAFunctionSummary {
  std::string FunctionName;
  uint64_t LoadsSeen = 0;
  uint64_t StoresSeen = 0;
  uint64_t LoadsReplaced = 0;
  uint64_t DeadLoadsRemoved = 0;
  uint64_t DeadStoresRemoved = 0;
  uint64_t PhisCreated = 0;
  uint64_t PhisSimplified = 0;
  uint64_t EntryInputs = 0;
  uint64_t CallReturnValues = 0;
  uint64_t CallClobberValues = 0;
  uint64_t CallArgStoresMarked = 0;
  uint64_t CallsRewritten = 0;
  uint64_t FunctionsRewritten = 0;
  uint64_t PreservedCalls = 0;
  uint64_t UnknownCallEffects = 0;
  uint64_t StackFrameAccessesRewritten = 0;
  uint64_t StackFramePointerLoadsReplaced = 0;
  uint64_t StackFrameRegisterLoadsRemoved = 0;
  uint64_t StackFrameRegisterStoresRemoved = 0;
  uint64_t StackFrameAllocaLoadsRemoved = 0;
  uint64_t StackFrameAllocaStoresRemoved = 0;
  uint64_t StackFrameAllocasRemoved = 0;
  uint64_t PartialDemandCandidates = 0;
  uint64_t PartialDemandMatched = 0;
  uint64_t PartialDemandRejected = 0;
};

struct NativeRegisterSummarySSASummary {
  uint64_t FunctionsSeen = 0;
  uint64_t LoadsSeen = 0;
  uint64_t StoresSeen = 0;
  uint64_t LoadsReplaced = 0;
  uint64_t DeadLoadsRemoved = 0;
  uint64_t DeadStoresRemoved = 0;
  uint64_t PhisCreated = 0;
  uint64_t PhisSimplified = 0;
  uint64_t EntryInputs = 0;
  uint64_t CallReturnValues = 0;
  uint64_t CallClobberValues = 0;
  uint64_t CallArgStoresMarked = 0;
  uint64_t CallsRewritten = 0;
  uint64_t FunctionsRewritten = 0;
  uint64_t PreservedCalls = 0;
  uint64_t UnknownCallEffects = 0;
  uint64_t StackFrameAccessesRewritten = 0;
  uint64_t StackFramePointerLoadsReplaced = 0;
  uint64_t StackFrameRegisterLoadsRemoved = 0;
  uint64_t StackFrameRegisterStoresRemoved = 0;
  uint64_t StackFrameAllocaLoadsRemoved = 0;
  uint64_t StackFrameAllocaStoresRemoved = 0;
  uint64_t StackFrameAllocasRemoved = 0;
  uint64_t StackCanaryChecksRemoved = 0;
  uint64_t StackCanaryFailBlocksRemoved = 0;
  uint64_t PartialDemandCandidates = 0;
  uint64_t PartialDemandMatched = 0;
  uint64_t PartialDemandRejected = 0;
  std::vector<NativeRegisterSummarySSAFunctionSummary> Functions;
};

NativeRegisterSummarySSASummary runNativeRegisterSummarySSA(
    llvm::Module &module, const NativeRegisterSummarySSAOptions &options = {});

void printNativeRegisterSummarySSASummary(
    const NativeRegisterSummarySSASummary &summary, llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
