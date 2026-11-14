#include "notdec-bin2llvm/passes/register/NativeRegisterSummary.h"

#include "core/NotDecMetadata.h"
#include "passes/register/RegisterModel.h"

#include "notdec-bin2llvm/abi/NativeAbi.h"
#include "notdec-bin2llvm/abi/NativeExternalPrototype.h"
#include "notdec-bin2llvm/intrinsics/NativeX87Intrinsic.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterPartialRead.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterPartialWrite.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterValueRange.h"
#include "notdec-bin2llvm/passes/stack/NativeStackAddress.h"
#include "notdec-bin2llvm/passes/vararg/NativeVarArgRewrite.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>


namespace notdec::bin2llvm {

struct StackSlotKey {
  llvm::Value *Base = nullptr;
  int64_t Offset = 0;

  bool operator<(const StackSlotKey &other) const {
    return std::tie(Base, Offset) < std::tie(other.Base, other.Offset);
  }

  bool operator==(const StackSlotKey &other) const {
    return Base == other.Base && Offset == other.Offset;
  }
};

struct ValueOrigin {
  llvm::GlobalVariable *Base = nullptr;
  int64_t Offset = 0;
  // 该值实际来自 Base 寄存器中的哪些位：整寄存器读是全宽，窄 partial
  // read 只带读到的位区间。入口读（ReadEntry）按这个范围记录，避免窄值
  // 沿整寄存器扩散成全宽参数。
  llvm::APInt Mask;

  bool operator==(const ValueOrigin &other) const {
    return Base == other.Base && Offset == other.Offset && Mask == other.Mask;
  }

  bool operator!=(const ValueOrigin &other) const { return !(*this == other); }
};

struct Cell {
  llvm::APInt MayEntry;
  llvm::APInt MayNonEntry;
  llvm::APInt ReadEntry;

  bool operator==(const Cell &other) const {
    return MayEntry == other.MayEntry && MayNonEntry == other.MayNonEntry &&
           ReadEntry == other.ReadEntry;
  }
};

struct RegisterOriginBits {
  llvm::APInt Entry;
  llvm::APInt Local;
  // Local 中"最后一次写是浮点指令"的子集。XMM 槽被 movq/punpcklqdq 这类
  // 整数打包写也会变 Local，但不是浮点参数；浮点参数证据只认这个子集。
  llvm::APInt FloatLocal;
  llvm::APInt CallClobber;

  bool operator==(const RegisterOriginBits &other) const {
    return Entry == other.Entry && Local == other.Local &&
           FloatLocal == other.FloatLocal &&
           CallClobber == other.CallClobber;
  }
};

struct State {
  bool Reachable = false;
  bool EndsInNoReturn = false;
  std::map<llvm::GlobalVariable *, Cell> Cells;
  std::map<llvm::GlobalVariable *, RegisterOriginBits> Origins;
  // Narrow frame-local model for saved-register recognition.  It only tracks
  // fixed entry-SP offsets or notdec_stack.native alloca offsets, not arbitrary
  // memory.
  std::map<StackSlotKey, llvm::GlobalVariable *> StackSlots;
  // Caller stack-argument evidence is separate from saved-register tracking.
  // Values are intentionally coarse: the summary only needs to know whether a
  // cspec stack slot was explicitly written before a call.
  std::map<StackSlotKey, NativeRegisterCallsiteValueOrigin> StackSlotOrigins;
  // The last store that wrote each evidence slot, kept beside
  // StackSlotOrigins so callsite evidence can widen a stack vararg slot when
  // the ABI grid (4 bytes on i386) is narrower than the actual store (an
  // 8-byte fstpl double).
  struct StackSlotStoreInfo {
    uint32_t SizeBytes = 0;
    bool IsFloat = false;

    bool operator==(const StackSlotStoreInfo &other) const {
      return SizeBytes == other.SizeBytes && IsFloat == other.IsFloat;
    }
  };
  std::map<StackSlotKey, StackSlotStoreInfo> StackSlotStores;
  // SSA values known to be a function-entry register plus a constant offset.
  // Offset 0 is also used for saved register values loaded back from stack.
  std::map<llvm::Value *, ValueOrigin> ValueOrigins;
  bool operator==(const State &other) const {
    return Reachable == other.Reachable &&
           EndsInNoReturn == other.EndsInNoReturn && Cells == other.Cells &&
           Origins == other.Origins && StackSlots == other.StackSlots &&
           StackSlotOrigins == other.StackSlotOrigins &&
           StackSlotStores == other.StackSlotStores &&
           ValueOrigins == other.ValueOrigins;
  }

  bool sameSummaryLattice(const State &other) const {
    return Reachable == other.Reachable &&
           EndsInNoReturn == other.EndsInNoReturn && Cells == other.Cells;
  }
};

struct FunctionEffect {
  std::map<llvm::GlobalVariable *, Cell> Registers;
  bool NoReturn = false;

  bool operator==(const FunctionEffect &other) const {
    return Registers == other.Registers && NoReturn == other.NoReturn;
  }
};

struct FunctionFlow {
  std::map<llvm::BasicBlock *, State> In;
  std::map<llvm::BasicBlock *, State> Out;
  FunctionEffect Effect;
};

struct FunctionDemand {
  std::map<llvm::GlobalVariable *, llvm::APInt> ExitDemand;
  std::map<llvm::GlobalVariable *, llvm::APInt> EntryDemand;

  bool operator==(const FunctionDemand &other) const {
    return ExitDemand == other.ExitDemand && EntryDemand == other.EntryDemand;
  }
};

struct SummaryAbiFacts {
  std::set<std::string> Inputs;
  std::map<std::string, llvm::APInt> InputMasks;
  std::vector<std::string> InputOrder;
  std::set<std::string> Outputs;
  std::map<std::string, llvm::APInt> OutputMasks;
  std::vector<std::string> OutputOrder;
  std::vector<std::string> IntegerOutputOrder;
  std::set<std::string> Unaffected;
  std::set<std::string> KilledByCall;
  std::map<std::string, llvm::APInt> KilledByCallMasks;
  std::string StackPointer;
  uint64_t StackShift = 0;
};

using RegisterDemand = std::map<llvm::GlobalVariable *, llvm::APInt>;

enum class RegisterOriginKind {
  Entry,
  Local,
  CallClobber,
  Mixed,
};

bool isNativeFrameSlotPointer(llvm::Value *value);

bool frameSlotPointerIsNeverLoaded(llvm::Value *pointer);

bool partialReadFeedsOnlyDeadFrameSlots(llvm::CallBase &call);

std::optional<std::pair<llvm::AllocaInst *, int64_t>>
nativeFrameOrigin(llvm::Function &function);

std::optional<StackSlotKey>
entrySlotToNativeFrameKey(llvm::Function &function,
                          const StackSlotKey &entryKey);

Cell defaultCell(llvm::GlobalVariable *global);

std::map<std::string, llvm::GlobalVariable *>
registersByName(const std::map<llvm::GlobalVariable *, RegisterUnit> &units);

unsigned registerBitWidth(const RegisterUnit &unit);

llvm::APInt fullMask(const RegisterUnit &unit);

llvm::APInt storageMaskForUnit(const NativeAbiStorage &storage,
                               const RegisterUnit &unit, uint64_t sizeBytes);

std::string maskToHex(const llvm::APInt &mask);

bool maskNonZero(const llvm::APInt &mask);

unsigned globalBitWidth(llvm::GlobalVariable *global);

bool addDemand(RegisterDemand &demand, llvm::GlobalVariable *global,
               llvm::APInt mask);

void eraseDemand(RegisterDemand &demand, llvm::GlobalVariable *global);

void eraseDemand(RegisterDemand &demand, llvm::GlobalVariable *global,
                 llvm::APInt mask);

llvm::APInt demandFor(const RegisterDemand &demand,
                      llvm::GlobalVariable *global);

bool mergeDemand(RegisterDemand &target, const RegisterDemand &source);

void mergeNamedMask(std::map<std::string, llvm::APInt> &masks,
                    const std::string &name, const llvm::APInt &mask);

llvm::APInt namedMaskOrFull(const std::map<std::string, llvm::APInt> &masks,
                            const RegisterUnit &unit);

unsigned valueBitWidth(llvm::Value *value);

llvm::APInt fullMaskForValue(llvm::Value *value);

llvm::APInt lowBitsMask(unsigned width, unsigned bits);

llvm::APInt trimmedMask(const llvm::APInt &mask, unsigned width);

std::map<llvm::Value *, llvm::APInt> computeValueDemands(
    llvm::Function &function,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units);

std::optional<std::pair<std::string, llvm::APInt>>
storageUnitMask(const NativeAbiStorage &storage, uint64_t sizeBytes,
                const std::map<llvm::GlobalVariable *, RegisterUnit> &units);

SummaryAbiFacts
collectSummaryAbiFacts(const llvm::Module &module,
                const std::map<llvm::GlobalVariable *, RegisterUnit> &units);

Cell &cellFor(State &state, llvm::GlobalVariable *global);

Cell cellIn(const State &state, llvm::GlobalVariable *global);

Cell cellIn(const FunctionEffect &effect, llvm::GlobalVariable *global);

bool joinCell(Cell &lhs, const Cell &rhs);

void joinReadEntryOnly(std::map<llvm::GlobalVariable *, Cell> &target,
                       const State &source);

unsigned registerBitWidth(llvm::GlobalVariable *global);

RegisterOriginBits defaultOrigin(llvm::GlobalVariable *global);

RegisterOriginBits originIn(const State &state, llvm::GlobalVariable *global);

llvm::APInt registerRangeMask(llvm::GlobalVariable *global, unsigned offsetBits,
                              unsigned sizeBits);

llvm::APInt fullRegisterMask(llvm::GlobalVariable *global);

void setRegisterOrigin(State &state, llvm::GlobalVariable *global,
                       const llvm::APInt &mask, RegisterOriginKind kind);

void setFullRegisterOrigin(State &state, llvm::GlobalVariable *global,
                           RegisterOriginKind kind);

bool hasFloatWriteMetadata(const llvm::Instruction &inst);

void recordRegisterFloatWrite(State &state, llvm::GlobalVariable *global,
                              const llvm::APInt &mask, bool isFloatWrite);

bool joinState(State &target, const State &source);

void readRegister(State &state, llvm::GlobalVariable *global,
                  const llvm::APInt &readMask);

void writeRegister(State &state, llvm::GlobalVariable *global,
                   const llvm::APInt &writeMask);

bool isX64GprName(llvm::StringRef name);

bool isX64Low32GprWrite(const NativeRegisterPartialWriteInfo &partial,
                        const RegisterUnit &unit);

void restoreRegister(State &state, llvm::GlobalVariable *global,
                     const llvm::APInt &restoreMask);

bool isKeepHighPartialStoreValue(llvm::Value *value,
                                 llvm::GlobalVariable *global);

bool isKeepHighPartialLoadUse(llvm::LoadInst &load,
                              llvm::GlobalVariable *global);

bool isIgnored(const RegisterUnit &unit,
               const NativeRegisterSummaryOptions &options);

class Analyzer {
public:
  Analyzer(llvm::Module &module, NativeRegisterSummaryOptions options);
  NativeRegisterSummary run();

  void buildCallGraph();

  void collectCrossBlockValues();

  std::vector<std::vector<llvm::Function *>> sccs() const;

  void runBottomUp();

  FunctionFlow solveFunction(llvm::Function &function);

  const NativeExternalCallShape *
  externalCallShape(const llvm::CallBase &call) const;

  bool isUnknownExternalCall(const llvm::CallBase &call) const;

  const NativeExternalCallShape *
  knownVarArgExternalShape(const llvm::CallBase &call) const;

  bool loadValueIsMovedAway(const llvm::LoadInst &load) const;

  bool registerValuePendingAtCall(const llvm::GlobalVariable &global,
                                  const llvm::CallBase &call) const;

  NativeRegisterCallsiteValueOrigin
  callsiteOrigin(const State &state, llvm::CallBase &call,
                 const NativeRegisterCallInputSlot &slot,
                 StackSlotKey *resolvedKey = nullptr) const;

  std::vector<NativeRegisterCallsiteSlotEvidence>
  callsiteEvidence(const State &state, llvm::CallBase &call,
                   llvm::ArrayRef<NativeRegisterCallInputSlot> slots) const;

  void recordExternalCallsite(llvm::Function &caller, llvm::CallBase &call,
                              const State &state);

  void collectExternalCallsiteEvidence();

  State transferBlock(llvm::BasicBlock &block, State state);

  void pruneBlockLocalValueFacts(llvm::BasicBlock &block, State &state) const;

  void transferInstruction(llvm::Instruction &inst, State &state);

  llvm::GlobalVariable *stackPointerGlobal() const;

  bool canTrackSavedEntryRegister(llvm::GlobalVariable *global) const;

  bool instructionReadsRegister(const llvm::Instruction &inst,
                                const llvm::GlobalVariable &global) const;

  bool instructionWritesRegister(const llvm::Instruction &inst,
                                 const llvm::GlobalVariable &global) const;

  bool callMayReadRegister(const llvm::CallBase &call,
                           const llvm::GlobalVariable &global) const;

  bool registerValueDiesAfter(llvm::StoreInst &store,
                              const llvm::GlobalVariable &global,
                              bool ignoreCallReads = false) const;

  const std::set<StackSlotKey> &
  stackAdjustmentSlots(llvm::Function &function) const;

  std::optional<StackSlotKey>
  comparableSlotKey(llvm::Function &function,
                    const NativeStackAddress &address) const;

  bool isStackAdjustmentAccess(llvm::Instruction &inst, llvm::Value *pointer,
                               uint64_t sizeBytes, bool push) const;

  static uint64_t valueSizeInBytes(llvm::Value *value);

  void collectStackAdjustmentSlots(llvm::Function &function,
                                   std::set<StackSlotKey> &result) const;

  std::optional<ValueOrigin> entryValueOrigin(llvm::Value *value,
                                              const State &state) const;

  RegisterOriginKind
  registerOriginKindForStoredValue(llvm::Value *value,
                                   const State &state) const;

  void markEntryValueRead(llvm::Value *value, State &state) const;

  void markInstructionEntryValueReads(llvm::Instruction &inst,
                                      State &state) const;

  NativeStackAddressAnalysis &
  stackAddressAnalysis(llvm::Function &function) const;

  std::optional<StackSlotKey>
  stackSlotKey(const NativeStackAddress &address) const;

  std::optional<StackSlotKey>
  callsiteStackSlot(llvm::CallBase &call,
                    const NativeRegisterCallInputSlot &slot) const;

  std::optional<StackSlotKey>
  fixedStackSlot(llvm::Value *pointer, llvm::Instruction &instruction) const;

  bool hasSavedEntryRegister(const State &state,
                             llvm::GlobalVariable *global) const;

  State exitStateWithSavedRegisters(const State &state) const;

  void transferCall(llvm::CallBase &call, State &state);

  void consumeCallerStackArgEvidence(State &state) const;

  void markNoReturnExit(State &state) const;

  void applyFunctionEffect(const FunctionEffect &effect, State &state);

  void applyCallInputs(State &state,
                       const std::vector<NativeRegisterCallInputSlot> &inputs);

  void applyAbiInputs(State &state);

  void applyAbiCallClobbers(State &state);

  void applyExternalCallEffect(State &state,
                               const NativeExternalCallShape &shape);

  void applyUnknownExternalCallEffect(State &state);

  void runTopDownDemand();

  void seedAbiReturns(FunctionDemand &demand) const;

  llvm::APInt
  valueDemand(llvm::Value *value,
              const std::map<llvm::Value *, llvm::APInt> &valueDemands) const;

  std::map<llvm::Function *, RegisterDemand>
  analyzeCallerDemand(llvm::Function &function);

  RegisterDemand
  liveBeforeBlock(llvm::BasicBlock &block, RegisterDemand live,
                  std::map<llvm::Function *, RegisterDemand> &additions,
                  const std::map<llvm::Value *, llvm::APInt> &valueDemands);

  void applyBackwardCallDemand(
      llvm::CallBase &call, RegisterDemand &live,
      std::map<llvm::Function *, RegisterDemand> &additions);

  llvm::MDNode *registerNode(llvm::LLVMContext &context,
                             const RegisterUnit &unit) const;

  llvm::MDNode *registerSummaryNode(llvm::LLVMContext &context,
                                    llvm::GlobalVariable *global,
                                    const Cell &cell,
                                    const FunctionDemand &demand) const;

  void attachMetadata();

  NativeRegisterSummary buildPublicSummary() const;

private:
  llvm::Module &Module;
  NativeRegisterSummaryOptions Options;
  std::map<llvm::GlobalVariable *, RegisterUnit> Units;
  std::map<std::string, llvm::GlobalVariable *> UnitsByName;
  SummaryAbiFacts Abi;
  std::vector<llvm::Function *> Functions;
  std::map<llvm::Function *, std::set<llvm::Function *>> Calls;
  std::map<llvm::Function *, std::set<llvm::Function *>> Callers;
  std::map<llvm::Function *, FunctionEffect> Effects;
  std::map<llvm::Function *, FunctionDemand> Demands;
  std::set<llvm::Value *> CrossBlockValues;
  std::map<llvm::Function *, FunctionFlow> StableFlows;
  std::vector<NativeRegisterExternalCallsite> ExternalCallsites;
  mutable std::map<llvm::Function *,
                   std::unique_ptr<NativeStackAddressAnalysis>>
      StackAddressAnalyses;
  mutable std::map<llvm::Function *, std::set<StackSlotKey>> StackAdjustmentSlots;
  static const FunctionDemand EmptyDemand;
};

} // namespace notdec::bin2llvm
