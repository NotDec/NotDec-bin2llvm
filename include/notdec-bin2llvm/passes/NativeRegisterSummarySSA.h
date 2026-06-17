#pragma once

#include <cstdint>
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
  bool AttachMetadata = true;
  bool PrintSummary = false;
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
  uint64_t PreservedCalls = 0;
  uint64_t UnknownCallEffects = 0;
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
  uint64_t PreservedCalls = 0;
  uint64_t UnknownCallEffects = 0;
  std::vector<NativeRegisterSummarySSAFunctionSummary> Functions;
};

NativeRegisterSummarySSASummary runNativeRegisterSummarySSA(
    llvm::Module &module, const NativeRegisterSummarySSAOptions &options = {});

void printNativeRegisterSummarySSASummary(
    const NativeRegisterSummarySSASummary &summary, llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
