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

struct NativeRegisterSummaryOptions {
  bool AttachMetadata = true;
  bool PrintSummary = false;
  // Registers handled by a dedicated pass, such as stack/frame pointers, can be
  // excluded from this summary without changing the abstract domain.
  std::set<std::string> IgnoredRegisters;
};

// Per-register result for one function. MayEntry/MayNonEntry describe the value
// visible at normal exits; ReadEntry records whether the function entry value
// may have been read before being killed.
struct NativeRegisterSummaryRegister {
  std::string Name;
  bool ReadEntry = false;
  bool MayEntry = true;
  bool MayNonEntry = false;
  bool ExitDemand = false;
};

// Function-level public summary. The counters are derived from Registers and
// are kept here so tests, logs, and later pipeline wiring do not need to repeat
// the same scans.
struct NativeRegisterSummaryFunction {
  std::string FunctionName;
  uint64_t LoadsSeen = 0;
  uint64_t StoresSeen = 0;
  uint64_t CallsSeen = 0;
  uint64_t ReadEntryRegisters = 0;
  uint64_t ModifiedRegisters = 0;
  uint64_t PreservedRegisters = 0;
  uint64_t DemandedReturns = 0;
  std::vector<NativeRegisterSummaryRegister> Registers;
};

// Module-level summary for the new SCC/fixpoint register analysis chain.
struct NativeRegisterSummary {
  uint64_t FunctionsSeen = 0;
  uint64_t LoadsSeen = 0;
  uint64_t StoresSeen = 0;
  uint64_t CallsSeen = 0;
  uint64_t ReadEntryRegisters = 0;
  uint64_t ModifiedRegisters = 0;
  uint64_t PreservedRegisters = 0;
  uint64_t DemandedReturns = 0;
  std::vector<NativeRegisterSummaryFunction> Functions;
};

NativeRegisterSummary
runNativeRegisterSummary(llvm::Module &module,
                         const NativeRegisterSummaryOptions &options = {});

void printNativeRegisterSummary(const NativeRegisterSummary &summary,
                                llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
