#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace notdec::bin2llvm {

struct NativeRegisterCallInputSlot {
  std::string UnitName;
  unsigned OffsetBits = 0;
  unsigned SizeBits = 0;
};

struct NativeExternalCallShape {
  std::vector<NativeRegisterCallInputSlot> Inputs;
  bool VarArg = false;
  unsigned MaxArgs = 0;
};

using NativeExternalCallShapeMap =
    std::map<std::string, NativeExternalCallShape, std::less<>>;

enum class NativeRegisterUnknownExternalInputPolicy {
  AbiInputs,
  NoInputs,
};

enum class NativeRegisterCallsiteValueOrigin {
  LocalDefinition,
  ForwardedEntry,
  Mixed,
  CallProduced,
  Unknown,
};

struct NativeRegisterCallsiteSlotEvidence {
  unsigned Index = 0;
  std::string UnitName;
  unsigned OffsetBits = 0;
  unsigned SizeBits = 0;
  NativeRegisterCallsiteValueOrigin Origin =
      NativeRegisterCallsiteValueOrigin::Unknown;
};

struct NativeRegisterUnknownExternalCallsite {
  std::string CallerName;
  std::string CalleeName;
  bool Indirect = false;
  std::vector<NativeRegisterCallsiteSlotEvidence> Slots;
};

struct NativeRegisterSummaryOptions {
  bool AttachMetadata = true;
  bool PrintSummary = false;
  // Registers handled by a dedicated pass, such as stack/frame pointers, can be
  // excluded from this summary without changing the abstract domain.
  std::set<std::string> IgnoredRegisters;
  // Prototype lowering stays in SummarySSA.  RegisterSummary only consumes
  // already-mapped register ranges so both passes use one ABI mapping.
  NativeExternalCallShapeMap ExternalCallShapes;
  NativeRegisterUnknownExternalInputPolicy UnknownExternalInputPolicy =
      NativeRegisterUnknownExternalInputPolicy::AbiInputs;
  // Callsite evidence only needs the stable bottom-up states.  The preliminary
  // unknown-external pass can skip return-demand propagation.
  bool RunTopDownDemand = true;
  bool CollectUnknownExternalCallsiteEvidence = false;
  std::vector<NativeRegisterCallInputSlot> UnknownExternalEvidenceSlots;
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
  // Bit-level demand refines ReadEntry/ExitDemand for wide backing registers.
  // Empty masks mean the old whole-register boolean is the only known fact.
  std::string EntryDemandMaskHex;
  std::string ExitDemandMaskHex;
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
  std::vector<NativeRegisterUnknownExternalCallsite> UnknownExternalCallsites;
};

NativeRegisterSummary
runNativeRegisterSummary(llvm::Module &module,
                         const NativeRegisterSummaryOptions &options = {});

void printNativeRegisterSummary(const NativeRegisterSummary &summary,
                                llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
