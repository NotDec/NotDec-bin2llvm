#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace llvm {
class CallBase;
class Module;
class raw_ostream;
} // namespace llvm

namespace notdec::bin2llvm {

struct NativeRegisterCallInputSlot {
  std::string UnitName;
  unsigned OffsetBits = 0;
  unsigned SizeBits = 0;
  // True for ABI floating-point slots backed by a lifted integer register.
  bool Float = false;
};

struct NativeExternalCallShape {
  std::vector<NativeRegisterCallInputSlot> Inputs;
  // Known vararg calls read only Inputs during the preliminary summary.
  // Candidate tails are inspected after the bottom-up state has stabilized.
  std::vector<NativeRegisterCallInputSlot> VarArgInputs;
  unsigned FixedArgs = 0;
  bool VarArg = false;
  bool NoReturn = false;
  unsigned MaxArgs = 0;
  bool FixedInputsComplete = true;
};

using NativeExternalCallShapeMap =
    std::map<std::string, NativeExternalCallShape, std::less<>>;
using NativeExternalCallsiteShapeMap =
    std::map<const llvm::CallBase *, NativeExternalCallShape>;

enum class NativeRegisterUnknownExternalInputPolicy {
  AbiInputs,
  NoInputs,
};

enum class NativeRegisterCallsiteValueOrigin {
  LocalDefinition,
  ForwardedEntry,
  Mixed,
  CallClobber,
  Unknown,
};

struct NativeRegisterCallsiteSlotEvidence {
  unsigned Index = 0;
  std::string UnitName;
  unsigned OffsetBits = 0;
  unsigned SizeBits = 0;
  bool Float = false;
  NativeRegisterCallsiteValueOrigin Origin =
      NativeRegisterCallsiteValueOrigin::Unknown;
};

enum class NativeRegisterExternalCallsiteKind {
  UnknownExternal,
  KnownVarArg,
};

// Evidence is tied to the original call instruction and is only valid until
// SummarySSA starts rewriting calls.
struct NativeRegisterExternalCallsite {
  const llvm::CallBase *Call = nullptr;
  NativeRegisterExternalCallsiteKind Kind =
      NativeRegisterExternalCallsiteKind::UnknownExternal;
  std::string CallerName;
  std::string CalleeName;
  bool Indirect = false;
  std::vector<NativeRegisterCallInputSlot> FixedInputs;
  unsigned FixedArgs = 0;
  unsigned MaxArgs = 0;
  bool FixedInputsComplete = true;
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
  NativeExternalCallsiteShapeMap ExternalCallsiteShapes;
  NativeRegisterUnknownExternalInputPolicy UnknownExternalInputPolicy =
      NativeRegisterUnknownExternalInputPolicy::AbiInputs;
  // Callsite evidence only needs the stable bottom-up states.  The preliminary
  // external-call pass can skip return-demand propagation.
  bool RunTopDownDemand = true;
  bool CollectExternalCallsiteEvidence = false;
  std::vector<NativeRegisterCallInputSlot> ExternalEvidenceSlots;
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
  bool NoReturn = false;
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
  uint64_t NoReturnFunctions = 0;
  uint64_t LoadsSeen = 0;
  uint64_t StoresSeen = 0;
  uint64_t CallsSeen = 0;
  uint64_t ReadEntryRegisters = 0;
  uint64_t ModifiedRegisters = 0;
  uint64_t PreservedRegisters = 0;
  uint64_t DemandedReturns = 0;
  std::vector<NativeRegisterSummaryFunction> Functions;
  std::vector<NativeRegisterExternalCallsite> ExternalCallsites;
};

NativeRegisterSummary
runNativeRegisterSummary(llvm::Module &module,
                         const NativeRegisterSummaryOptions &options = {});

void printNativeRegisterSummary(const NativeRegisterSummary &summary,
                                llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
