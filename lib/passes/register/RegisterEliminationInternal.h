#pragma once
#include "notdec-bin2llvm/passes/register/NativeRegisterElimination.h"

#include "core/NotDecIntrinsics.h"
#include "core/NotDecMetadata.h"
#include "passes/register/RegisterModel.h"

#include "notdec-bin2llvm/abi/NativeAbi.h"
#include "notdec-bin2llvm/abi/NativeExternalPrototype.h"
#include "notdec-bin2llvm/intrinsics/NativeX87Intrinsic.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterPartialRead.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterPartialWrite.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterValueRange.h"
#include "notdec-bin2llvm/passes/register/NativeRegisterPreSummaryPeephole.h"
#include "notdec-bin2llvm/passes/register/NativeRegisterSummary.h"
#include "notdec-bin2llvm/passes/stack/NativeStackAddress.h"
#include "notdec-bin2llvm/passes/stack/NativeStackCanaryCleanup.h"
#include "notdec-bin2llvm/passes/stack/NativeStackFrame.h"
#include "notdec-bin2llvm/passes/vararg/NativeVarArgRewrite.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Local.h"

#include <algorithm>
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

using KnownExternalPrototype = NativeExternalPrototype;

struct SummaryRegisterFact {
  // bool 字段是 mask 的整寄存器投影；精确判断用 mask 字段。
  bool ReadEntry = false;
  bool MayEntry = true;
  bool MayNonEntry = false;
  bool ExitDemand = false;
  llvm::APInt ReadEntryMask;
  llvm::APInt MayEntryMask;
  llvm::APInt MayNonEntryMask;
  llvm::APInt EntryDemandMask;
  llvm::APInt ExitDemandMask;
  // 返回槽里"可能还是入口值"的位（ExitDemand ∩ MayEntry ∩ MayNonEntry）。出口
  // 要交回这些位，入口就必须有形参承载（Arch2.md 第 13 节的"双向位"）。
  llvm::APInt ReturnEntryMask;
};

struct FunctionSummaryFacts {
  bool NoReturn = false;
  std::map<std::string, SummaryRegisterFact> Registers;
};

struct AbiFacts {
  std::set<std::string> Inputs;
  std::vector<std::string> InputsInOrder;
  // ABI slots keep the cspec register name and the backing lifted register
  // separate.  For x86-64 this lets XMM0_Qa be consumed as the low lane of
  // the lifted ZMM0 global without changing the register model.
  struct RegisterSlot {
    std::string UnitName;
    std::string AbiName;
    std::string MetaType;
    unsigned OffsetBits = 0;
    unsigned SizeBits = 0;
  };
  // Stack pentry copied from cspec metadata.  A single Ghidra stack pentry
  // normally describes a repeating argument area; concrete argument slots below
  // are expanded from this range, never from an architecture hardcode.
  struct StackSlot {
    std::string Space;
    std::string MetaType;
    uint64_t Offset = 0;
    uint32_t MinSize = 0;
    uint32_t MaxSize = 0;
    uint32_t Align = 0;
    size_t AbiIndex = 0;
  };
  std::vector<RegisterSlot> IntegerInputsInOrder;
  std::vector<RegisterSlot> FloatInputsInOrder;
  std::vector<StackSlot> StackInputsInOrder;
  std::set<std::string> Outputs;
  std::vector<std::string> OutputsInOrder;
  std::vector<RegisterSlot> IntegerOutputsInOrder;
  std::vector<RegisterSlot> FloatOutputsInOrder;
  std::set<std::string> InternalParamRegisters;
  std::set<std::string> InternalReturnRegisters;
  std::set<std::string> Unaffected;
  std::set<std::string> KilledByCall;
  std::string StackPointer;
  uint64_t StackShift = 0;
};

enum class CallRegisterEffect {
  Preserve,
  ReturnValue,
  Clobber,
  Unknown,
};

struct RegisterRangeKey {
  llvm::GlobalVariable *Global = nullptr;
  uint64_t BitOffset = 0;
  uint32_t BitWidth = 0;

  bool operator<(const RegisterRangeKey &other) const {
    return std::tie(Global, BitOffset, BitWidth) <
           std::tie(other.Global, other.BitOffset, other.BitWidth);
  }

  bool operator==(const RegisterRangeKey &other) const {
    return Global == other.Global && BitOffset == other.BitOffset &&
           BitWidth == other.BitWidth;
  }
};

struct RangedSSAValue {
  llvm::Value *Value = nullptr;
  RegisterRangeKey CoveredRange;
};

using BlockRangeKey = std::pair<llvm::BasicBlock *, RegisterRangeKey>;

using InstRangeKey = std::pair<llvm::Instruction *, RegisterRangeKey>;

using LiveRegisterRanges = std::set<RegisterRangeKey>;

enum class RangeEventKind { Read, Write, Clobber };

using CallRangeValueKey =
    std::tuple<llvm::Instruction *, RegisterRangeKey, std::string>;

struct CallArgStoreBinding {
  llvm::StoreInst *Store = nullptr;
  const RegisterUnit *Unit = nullptr;
  llvm::Value *RegisterValue = nullptr;
  llvm::Value *Value = nullptr;
  llvm::Type *ValueType = nullptr;
  unsigned Index = 0;
};

struct RelativeStoreGroup {
  NativeStackAddressKind Kind =
      NativeStackAddressKind::RelativeStackPointer;
  llvm::Value *Base = nullptr;
  std::map<int64_t, llvm::StoreInst *> Stores;
};

struct PartialDemandState {
  std::map<llvm::Value *, llvm::APInt> Demands;
  std::vector<llvm::Value *> Worklist;

  static llvm::APInt fullMask(unsigned bitWidth) {
    return llvm::APInt::getAllOnes(bitWidth);
  }

  static llvm::APInt trimmedMask(const llvm::APInt &mask, unsigned bitWidth) {
    if (bitWidth == 0) {
      return llvm::APInt();
    }
    if (mask.getBitWidth() == bitWidth) {
      return mask;
    }
    if (mask.getBitWidth() < bitWidth) {
      return mask.zext(bitWidth);
    }
    return mask.trunc(bitWidth);
  }

  static unsigned bitWidthOf(const llvm::Value &value) {
    llvm::Type *type = value.getType();
    if (type == nullptr || !type->isSized()) {
      return 0;
    }
    return type->getScalarSizeInBits();
  }

  void addDemand(llvm::Value *value, llvm::APInt demand) {
    if (value == nullptr || demand.isZero()) {
      return;
    }
    unsigned bitWidth = bitWidthOf(*value);
    if (bitWidth == 0) {
      return;
    }
    demand = trimmedMask(demand, bitWidth);
    if (demand.isZero()) {
      return;
    }
    auto it = Demands.find(value);
    if (it == Demands.end()) {
      Demands.emplace(value, demand);
      Worklist.push_back(value);
      return;
    }
    llvm::APInt combined = it->second | demand;
    if (combined != it->second) {
      it->second = combined;
      Worklist.push_back(value);
    }
  }

  llvm::APInt demandOf(llvm::Value *value) const {
    if (value == nullptr) {
      return llvm::APInt();
    }
    auto it = Demands.find(value);
    if (it == Demands.end()) {
      return llvm::APInt(bitWidthOf(*value), 0);
    }
    return it->second;
  }
};

enum class NativeSignatureSlotKind {
  IntegerRegister,
  FloatRegister,
  IntegerStack,
  FloatStack,
};

struct NativeSignatureSlot {
  NativeSignatureSlotKind Kind = NativeSignatureSlotKind::IntegerRegister;
  const RegisterUnit *Unit = nullptr;
  std::string AbiName;
  std::string MetaType;
  std::string StackSpace;
  uint64_t StackOffset = 0;
  uint32_t StackSize = 0;
  uint32_t StackAlign = 0;
  unsigned OffsetBits = 0;
  unsigned SizeBits = 0;
  llvm::Type *LlvmType = nullptr;
  // True when this stack parameter has direct incoming-load evidence or was
  // propagated from a tail-call callee.  Such parameters must not be trimmed
  // only because one callsite binding prefix is short.
  bool PreserveParam = false;
};

struct SignatureShape {
  std::vector<NativeSignatureSlot> Params;
  std::vector<NativeSignatureSlot> Returns;
  bool VarArg = false;
  // Bounded varargs, such as open(path, flags[, mode]), should keep the LLVM
  // vararg type but must not pull in every ABI input register as an argument.
  // Zero means the vararg tail is unbounded.
  unsigned MaxArgs = 0;
};

struct RangeReturnHelper {
  RegisterRangeKey Range;
  llvm::CallInst *Helper = nullptr;
};

struct SignatureRewriteState {
  std::map<llvm::Function *, SignatureShape> Shapes;
  // Indirect calls do not have a callee Function to hang a rewritten type on.
  // Keep their inferred type on the callsite, then rebuild the call with the
  // typed function pointer during signature rewrite.
  std::map<llvm::CallBase *, SignatureShape> IndirectCallShapes;
  // Per-callsite parameter evidence for indirect calls.  The evidence is
  // collected by NativeRegisterSummary from LocalDefinition / ForwardedEntry /
  // CallClobber provenance before SSA rewriting; this avoids treating every
  // ABI input register as a parameter just because it has some current SSA
  // value at the call.
  std::map<const llvm::CallBase *, std::vector<NativeRegisterCallInputSlot>>
      IndirectCallInputs;
  std::map<llvm::CallBase *, std::vector<CallArgStoreBinding>> CallArgs;
  // "这个值/这条 store 是不是被调用点记录过"的模块级集合，在记录绑定时增量插入
  // （见 collectSignatureCallArgs）。原来每个函数重建一次要扫全模块 CallArgs，
  // wolfssl 上占 33% 采样；这里查询 O(1) 且不用重建。存的是原始值：本函数自己的
  // 值可能被 Replacement 改写，由 FunctionBuilder 用 LocalRecordedValues 再解析一次。
  llvm::DenseSet<llvm::Value *> CallArgRecordedValues;
  llvm::DenseSet<llvm::StoreInst *> CallArgRecordedStores;
  // 按函数名分组的绑定值：解析（resolve）用的是"当前正在构建的函数"的 Replacement 表，
  // 所以查询时要单独按解析结果比一次。用函数名分组而不是 Function*，是因为签名重写会
  // 换成新的 Function 对象（名字保持不变），清理遍按名字仍能找到同一批记录值。
  std::map<std::string, std::vector<llvm::Value *>>
      CallArgRecordedValuesByFunction;
  // 调用点每个参数槽在"调用前那一刻"的按位 SSA 值：改写阶段 bindingForIndex
  // 找不到绑定时按 (call, index) 查它，而不是造 unknown。绑定收集为了保持
  // "前缀"语义，遇到第一个读不出值的槽就停；这张表逐槽记录、不因前一个槽失败
  // 而放弃后面的槽，所以能补上断档处的值。栈槽在第二遍（栈地址稳定后）记录。
  std::map<llvm::CallBase *, std::map<unsigned, llvm::Value *>>
      CallArgRangeValues;
  std::map<llvm::CallBase *, std::map<std::string, llvm::CallInst *>>
      ReturnHelpers;
  std::map<llvm::CallBase *, std::vector<RangeReturnHelper>> RangeReturnHelpers;
  std::map<llvm::Function *,
           std::map<llvm::ReturnInst *, std::vector<llvm::Value *>>>
      FunctionReturns;
  std::map<llvm::Value *, llvm::Value *> ValueMap;
  std::set<llvm::StoreInst *> StoresToErase;
  std::vector<NativeRegisterEliminationWarning> Warnings;
  // External prototypes come from the built-in libc table plus an optional JSON
  // overlay.  Keep one shared map so every signature decision classifies known
  // and unknown declarations the same way.
  const NativeExternalPrototypeMap *ExternalPrototypes = nullptr;
  // Known vararg tails are inferred per callsite between the two register
  // summary runs.  The declaration shape still contains fixed parameters only.
  const NativeExternalCallsiteShapeMap *ExternalCallsiteShapes = nullptr;
  // Calls rebuilt by SummarySSA already carry their ABI inputs as LLVM
  // operands, so the later register-store liveness pass must not treat them as
  // users of @RDI/@RSI/... globals.
  std::set<const llvm::CallBase *> RewrittenCalls;
};

enum class AbiConvention { External, Internal };

enum : unsigned {
  SourceReal = 1u << 0,
  SourceClobber = 1u << 1,
  SourceUnknown = 1u << 2,
};

const CallArgStoreBinding *
bindingForIndex(const std::vector<CallArgStoreBinding> &bindings,
                unsigned index);

bool sameStackSignatureSlot(const NativeSignatureSlot &lhs,
                            const NativeSignatureSlot &rhs);

const NativeExternalPrototypeMap &
externalPrototypesForState(const SignatureRewriteState &state);

const NativeExternalCallShape *
externalCallsiteShapeForCall(const SignatureRewriteState &state,
                             const llvm::CallBase &call);

void attachUnknownValueMetadata(llvm::Value *value, llvm::StringRef reason,
                                llvm::StringRef functionName = "",
                                llvm::StringRef calleeName = "",
                                llvm::StringRef registerName = "",
                                llvm::StringRef kind = "",
                                llvm::StringRef detail = "");

llvm::Value *unknownValueBefore(llvm::Instruction &insertBefore,
                                llvm::Type *type, llvm::Twine name);

llvm::Function *getOrInsertVarArgUnknownHelper(llvm::Module &module,
                                               llvm::Type *type);

void attachZeroDemandOperandMetadata(llvm::Instruction &instruction,
                                     unsigned operandIndex,
                                     llvm::Value &original);

const KnownExternalPrototype *
knownExternalPrototype(const NativeExternalPrototypeMap &prototypes,
                       llvm::StringRef name);

bool isNotDecOpaqueUnknownName(llvm::StringRef name);

bool isKnownNoReturnExternal(const llvm::Function &function,
                             const NativeExternalPrototypeMap &prototypes);

bool isKnownExternalFunction(const llvm::Function &function,
                             const NativeExternalPrototypeMap &prototypes);

bool isUnknownExternalFunction(const llvm::Function &function,
                               const NativeExternalPrototypeMap &prototypes);

bool isKnownNoReturnCall(
    const llvm::Function &callee, const NativeExternalPrototypeMap &prototypes,
    const std::map<llvm::Function *, FunctionSummaryFacts> *facts);

void truncateKnownNoReturnCalls(
    llvm::Module &module, const NativeExternalPrototypeMap &prototypes,
    const std::map<llvm::Function *, FunctionSummaryFacts> *facts = nullptr);

void eraseUnusedSummaryHelperDeclarations(llvm::Module &module);

uint64_t eraseDeadSummarySSAEntryReads(llvm::Module &module);

uint64_t eraseDeadSummaryCallValueHelpers(llvm::Module &module);

bool registerValueSourceIsControlled(const llvm::Function &function);

AbiConvention abiConventionFor(
    const llvm::Function &function, bool abiForAllFunctions,
    const std::set<std::string> *forcedExternal = nullptr,
    const std::set<std::string> *forcedInternal = nullptr);

bool pointerIsRegisterGlobal(const llvm::Value *value);

NativeRegisterValueSourceAudit auditRegisterValueSourcesImpl(
    llvm::Module &module, bool abiForAllFunctions,
    const std::set<std::string> *forcedExternal = nullptr,
    const std::set<std::string> *forcedInternal = nullptr);

llvm::MDNode *registerAccessNode(llvm::LLVMContext &context,
                                 const RegisterUnit &unit);

llvm::GlobalVariable *registerGlobalFromValue(
    llvm::Value *value,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units);

bool valueTypeMatchesRegisterStorage(llvm::Type *valueType,
                                     const llvm::GlobalVariable &global);

uint64_t canonicalizeRegisterPointerPhiLoads(
    llvm::Module &module,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units);

uint64_t canonicalizeX87WindowAccesses(
    llvm::Module &module,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units);

bool isAbiBoundaryZeroRegister(llvm::StringRef name);

bool isX87StackRegisterName(llvm::StringRef name);

bool isValueCarryingRegisterName(llvm::StringRef name);

void pushUnique(std::vector<std::string> &items, const std::string &item);

const RegisterUnit *
unitByName(const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
           llvm::StringRef name);

AbiFacts::RegisterSlot
abiRegisterSlot(const NativeAbiParamEntry &entry, const std::string &unitName,
                const std::map<llvm::GlobalVariable *, RegisterUnit> &units);

AbiFacts::StackSlot abiStackSlot(const NativeAbiParamEntry &entry,
                                 size_t abiIndex);

AbiFacts
collectAbiFacts(const llvm::Module &module,
                const std::map<llvm::GlobalVariable *, RegisterUnit> &units);

std::map<llvm::Function *, FunctionSummaryFacts>
summaryFactsByFunction(const NativeRegisterSummary &summary,
                       llvm::Module &module);

std::string typeSuffix(llvm::Type &type);

const RegisterUnit *
unitByName(const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
           llvm::StringRef name);

llvm::Type *llvmTypeForKnownValue(llvm::Module &module,
                                  NativeExternalPrototype::ValueType type);

bool isFloatKnownValue(NativeExternalPrototype::ValueType type);

NativeSignatureSlot integerSignatureSlot(const RegisterUnit &unit);

std::optional<NativeSignatureSlot>
integerSlotForSingleDemandRange(const RegisterUnit &unit,
                                const llvm::APInt &demand);

std::optional<NativeSignatureSlot> signatureSlotFromAbi(
    const AbiFacts::RegisterSlot &abiSlot,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    llvm::Type *llvmType, NativeSignatureSlotKind kind);

uint64_t stackSlotStep(uint32_t sizeBytes, uint32_t alignBytes);

std::optional<NativeSignatureSlot>
signatureStackSlotFromAbi(const AbiFacts::StackSlot &abiSlot,
                          unsigned stackIndex, llvm::Type *llvmType,
                          NativeSignatureSlotKind kind);

llvm::Type *slotType(const NativeSignatureSlot &slot);

llvm::Type *singleReturnType(const SignatureShape &shape);

llvm::Type *returnTypeForShape(llvm::LLVMContext &context,
                               const SignatureShape &shape);

llvm::FunctionType *functionTypeForShape(llvm::LLVMContext &context,
                                         const SignatureShape &shape);

bool isFloatAbiOutputUnit(const AbiFacts &abi, llvm::StringRef name);

const AbiFacts::RegisterSlot *floatAbiInputSlotForUnit(const AbiFacts &abi,
                                                       llvm::StringRef name);

const AbiFacts::RegisterSlot *floatAbiOutputSlotForUnit(const AbiFacts &abi,
                                                        llvm::StringRef name);

llvm::APInt fullMaskForSlot(unsigned registerBits,
                            const AbiFacts::RegisterSlot &slot);

bool demandFitsSlot(const llvm::APInt &demand,
                    const AbiFacts::RegisterSlot &slot, unsigned registerBits);

llvm::Type *floatTypeForSizeBits(llvm::LLVMContext &context,
                                 unsigned sizeBits);

llvm::Type *floatTypeForSlot(llvm::LLVMContext &context,
                             const AbiFacts::RegisterSlot &slot);

std::optional<NativeSignatureSlot>
floatSlotForDemand(llvm::LLVMContext &context,
                   const AbiFacts::RegisterSlot &abiSlot,
                   const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
                   const llvm::APInt &demand);

const RegisterUnit *
stackPointerUnit(const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
                 const AbiFacts &abi);

std::optional<NativeSignatureSlot>
stackInputSlotForLoad(llvm::LoadInst &load,
                      const NativeStackAddressAnalysis &stackAddresses,
                      const AbiFacts &abi);

void appendInternalStackParams(
    SignatureShape &shape, llvm::Function &function,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi);

SignatureShape shapeForInternalFunction(
    llvm::Function &function,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const std::map<llvm::Function *, FunctionSummaryFacts> &summaryFacts,
    const AbiFacts &abi, AbiConvention own);

std::optional<NativeSignatureSlot>
typedParamSlot(NativeExternalPrototype::ValueType type, unsigned &integerIndex,
               unsigned &floatIndex, unsigned &stackIndex,
               const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
               const AbiFacts &abi, llvm::Module &module);

std::optional<NativeSignatureSlot> untypedIntegerParamSlot(
    unsigned index, const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi, llvm::Module &module);

std::optional<NativeSignatureSlot>
typedReturnSlot(NativeExternalPrototype::ValueType type,
                const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
                const AbiFacts &abi, llvm::Module &module);

bool isIntegerAbiOutput(const AbiFacts &abi, llvm::StringRef name);

bool isLikelyNonReturnIntegerAbiOutput(const AbiFacts &abi,
                                       llvm::StringRef name);

std::optional<NativeSignatureSlot>
integerReturnSlotForUnit(const RegisterUnit &unit, const AbiFacts &abi);

std::optional<NativeSignatureSlot>
floatReturnSlotForUnit(const RegisterUnit &unit,
                       const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
                       const AbiFacts &abi);

SignatureShape shapeForKnownExternal(
    llvm::Function &function,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi, const NativeExternalPrototypeMap &prototypes);

NativeRegisterCallInputSlot callInputSlot(const NativeSignatureSlot &slot);

NativeRegisterCallInputSlot callInputSlot(const AbiFacts::RegisterSlot &slot);

NativeRegisterCallInputSlot callInputSlot(const AbiFacts::StackSlot &slot,
                                          unsigned stackIndex,
                                          llvm::Type *llvmType);

bool sameCallInputSlot(const NativeRegisterCallInputSlot &lhs,
                       const NativeRegisterCallInputSlot &rhs);

void addStackEvidenceSlots(const AbiFacts &abi, llvm::Module &module,
                           std::vector<NativeRegisterCallInputSlot> &slots,
                           unsigned startIndex, unsigned count,
                           llvm::ArrayRef<NativeRegisterCallInputSlot> fixed);

NativeExternalCallShapeMap buildExternalCallShapes(
    llvm::Module &module,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi, const NativeExternalPrototypeMap &prototypes);

std::vector<NativeRegisterCallInputSlot>
unknownExternalEvidenceSlots(const AbiFacts &abi, llvm::Module &module);

bool isLikelyX86_64SysVAbi(const AbiFacts &abi);

unsigned localDefinitionPrefix(
    llvm::ArrayRef<NativeRegisterCallsiteSlotEvidence> slots);

std::vector<NativeRegisterCallsiteSlotEvidence>
renumberedSlotsByKind(const NativeRegisterExternalCallsite &callsite,
                      bool wantFloat);

bool hasLocalDefinitionAfter(
    llvm::ArrayRef<NativeRegisterCallsiteSlotEvidence> slots, unsigned prefix);

NativeRegisterCallInputSlot
callInputSlot(const NativeRegisterCallsiteSlotEvidence &slot);

std::optional<NativeSignatureSlot>
makeSignatureSlotForCallInput(
    const NativeRegisterCallInputSlot &input,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    llvm::LLVMContext &context);

std::optional<unsigned> constantLowByteBeforeCall(
    const NativeRegisterExternalCallsite &callsite,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    llvm::StringRef registerName);

NativeRegisterEliminationWarning externalInferenceWarning(llvm::StringRef callee,
                                                         llvm::StringRef detail,
                                                         llvm::StringRef reason,
                                                         unsigned uses);

NativeRegisterEliminationWarning
varArgInferenceWarning(const NativeRegisterExternalCallsite &callsite,
                       llvm::StringRef detail, llvm::StringRef reason);

llvm::StringRef originName(NativeRegisterCallsiteValueOrigin origin);

const NativeRegisterCallsiteSlotEvidence *
slotAtPrefix(llvm::ArrayRef<NativeRegisterCallsiteSlotEvidence> slots,
             unsigned prefix);

std::string slotDetail(const NativeRegisterCallsiteSlotEvidence &slot);

void warnIfStoppedAtCallClobber(
    std::vector<NativeRegisterEliminationWarning> &warnings,
    const NativeRegisterExternalCallsite &callsite,
    llvm::ArrayRef<NativeRegisterCallsiteSlotEvidence> slots, unsigned prefix,
    llvm::StringRef reason);

std::string calleeNameForWarning(const llvm::CallBase &call);

std::string slotRegisterName(const NativeSignatureSlot &slot);

NativeRegisterEliminationWarning callSlotWarning(const llvm::CallBase &call,
                                                const NativeSignatureSlot &slot,
                                                llvm::StringRef kind,
                                                llvm::StringRef reason);

NativeRegisterEliminationWarning
callRegisterWarning(const llvm::CallBase &call, llvm::StringRef registerName,
                    llvm::StringRef kind, llvm::StringRef reason);

std::string registerNameForRange(const RegisterRangeKey &range);

NativeRegisterEliminationWarning callRangeWarning(const llvm::CallBase &call,
                                                 const RegisterRangeKey &range,
                                                 llvm::StringRef kind,
                                                 llvm::StringRef reason);

NativeRegisterEliminationWarning
returnSlotWarning(const llvm::Function &function,
                  const NativeSignatureSlot &slot, llvm::StringRef reason);

NativeExternalCallsiteShapeMap inferExternalCallShapes(
    NativeExternalPrototypeMap &prototypes,
    const NativeRegisterSummary &baseSummary,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    std::vector<NativeRegisterEliminationWarning> &warnings);

void inferIndirectCallInputs(
    const NativeRegisterSummary &baseSummary,
    std::map<const llvm::CallBase *,
             std::vector<NativeRegisterCallInputSlot>> &out);

bool signatureHasReturnForRegister(const SignatureShape &shape,
                                   const RegisterUnit &unit);

void addConservativeUnknownReturnSlots(
    SignatureShape &shape, llvm::Type *oldReturnType,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi,
    const std::function<bool(llvm::StringRef)> &hasLiveReadEvidence);

void addIndirectCallsiteShapes(
    SignatureRewriteState &state,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi);

void addDemandedExternalReturns(
    llvm::Module &module, SignatureRewriteState &state,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi, const NativeExternalPrototypeMap &prototypes);

bool isDirectSummaryClobberValue(const llvm::Value *value);

unsigned classifyValueSources(
    const llvm::Value *value,
    llvm::SmallPtrSetImpl<const llvm::Value *> &visiting);

unsigned classifyValueSources(const llvm::Value *value);

bool mayDependOnSummaryClobberValue(const llvm::Value *value);

unsigned
callsiteBoundArgPrefix(const std::vector<CallArgStoreBinding> &bindings);

void refineIndirectCallsiteParamShapes(SignatureRewriteState &state);

void refineInternalStackParamShapes(SignatureRewriteState &state);

void markSignatureCallArgStores(SignatureRewriteState &state,
                                NativeRegisterEliminationSummary &summary);

// 用户显式声明的寄存器槽（`returns: [{"register":...,"width":...}]` 派生的
// NativeSignatureSlot）。寄存器不存在时返回 nullopt。
std::optional<NativeSignatureSlot> explicitRegisterSlot(
    const NativeRegisterSlotSpec &spec,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    llvm::Module &module);

std::map<llvm::Function *, SignatureShape> buildInitialSignatureShapes(
    llvm::Module &module,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const std::map<llvm::Function *, FunctionSummaryFacts> &summaryFacts,
    const AbiFacts &abi, const NativeExternalPrototypeMap &prototypes,
    bool abiForAllFunctions,
    const std::set<std::string> *forcedExternal = nullptr,
    const std::set<std::string> *forcedInternal = nullptr,
    const std::map<std::string, NativeSignaturePin> *pinnedSignatures =
        nullptr);

bool isStackSignatureSlot(const NativeSignatureSlot &slot);

bool sameStackSignatureSlot(const NativeSignatureSlot &lhs,
                            const NativeSignatureSlot &rhs);

void propagateTailCallStackParams(llvm::Module &module,
                                  SignatureRewriteState &state);

const SignatureShape *shapeForCall(const SignatureRewriteState &state,
                                   const llvm::CallBase &call);

llvm::Value *extractReturnRange(llvm::IRBuilder<> &builder,
                                const SignatureShape &shape, llvm::Value &call,
                                const RegisterRangeKey &range);

llvm::Value *buildReturnValue(llvm::IRBuilder<> &builder,
                              const SignatureShape &shape,
                              const std::vector<llvm::Value *> &values);

bool runPostRewriteInstCombine(llvm::Module &module);

void addFunctionSummary(NativeRegisterEliminationSummary &total,
                        const NativeRegisterEliminationFunctionSummary &fn);

llvm::AttributeList attributesForNewFunction(llvm::Function &oldFunction,
                                             llvm::FunctionType &newType);

void copyFunctionMetadata(llvm::Function &from, llvm::Function &to);

llvm::Function *createReplacementFunction(llvm::Function &oldFunction,
                                          llvm::FunctionType &newType);

llvm::Value *castSlotValueToRegister(llvm::IRBuilder<> &builder,
                                     llvm::Value *value,
                                     const NativeSignatureSlot &slot);

llvm::Value *buildReturnValue(llvm::IRBuilder<> &builder,
                              const SignatureShape &shape,
                              const std::vector<llvm::Value *> &values);

llvm::Value *extractReturnRegister(llvm::IRBuilder<> &builder,
                                   const SignatureShape &shape,
                                   llvm::Value &call,
                                   llvm::StringRef registerName);

llvm::Module *moduleForBuilder(llvm::IRBuilder<> &builder);

llvm::Value *createValueRangeExtract(llvm::IRBuilder<> &builder,
                                     llvm::Value *source, unsigned readWidth,
                                     uint64_t bitOffset, llvm::Twine name);

llvm::Value *createValueRangeInsert(llvm::IRBuilder<> &builder,
                                    llvm::Value *base, llvm::Value *value,
                                    unsigned writeWidth, uint64_t bitOffset,
                                    llvm::Twine name);

llvm::Value *extractReturnRange(llvm::IRBuilder<> &builder,
                                const SignatureShape &shape, llvm::Value &call,
                                const RegisterRangeKey &range);

llvm::Value *foreignArgumentReplacement(
    llvm::Function &function, llvm::Argument &argument,
    std::map<llvm::Type *, llvm::Value *> &unknownByType);

void replaceForeignArgumentsInBody(llvm::Function &function);

llvm::Value *localizeReturnValue(llvm::Function &function,
                                 llvm::ReturnInst &insertBefore,
                                 llvm::Value *value);

llvm::Value *localizeCallArgument(llvm::Function &function,
                                  llvm::Instruction &insertBefore,
                                  llvm::Value *value);

llvm::Value *castSlotValueToRegister(llvm::IRBuilder<> &builder,
                                     llvm::Value *value,
                                     const NativeSignatureSlot &slot);

llvm::CallInst *rewriteCallInst(llvm::CallBase &oldCall, llvm::Value &callee,
                                llvm::FunctionType &newType,
                                const std::vector<llvm::Value *> &args);

llvm::Value *replacementForOldCallUses(llvm::IRBuilder<> &builder,
                                       llvm::CallBase &oldCall,
                                       llvm::CallInst &newCall,
                                       const SignatureShape &shape);

llvm::Value *integerEntrySlotReplacement(llvm::Instruction &inst,
                                         llvm::LoadInst &entryLoad,
                                         const NativeSignatureSlot &slot,
                                         llvm::Argument &arg,
                                         llvm::IRBuilder<> &builder);

llvm::Value *rangeEntrySlotReplacement(llvm::Instruction &inst,
                                       const NativeSignatureSlot &slot,
                                       llvm::Argument &arg,
                                       llvm::IRBuilder<> &builder);

void rewriteInternalFunctionBody(
    llvm::Function &oldFunction, llvm::Function &newFunction,
    const SignatureShape &shape, SignatureRewriteState &state,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi);

unsigned integerBitWidthOf(llvm::Value *value);

llvm::Value *resolveValueMap(llvm::Value *value,
                             std::map<llvm::Value *, llvm::Value *> &valueMap);

bool valueMayDependOnUnknownPlaceholder(
    llvm::Value *value, llvm::SmallPtrSetImpl<llvm::Value *> &seen);

bool valueMayDependOnUnknownPlaceholder(llvm::Value *value);

bool shouldPoisonUnprovenVarArg(const CallArgStoreBinding &binding,
                                llvm::Value *value,
                                const llvm::DataLayout &layout,
                                const llvm::Instruction *context);

bool collectInsertedExtractPieces(
    llvm::Value *value, unsigned fullWidth,
    std::map<llvm::Value *, llvm::Value *> &valueMap, llvm::Value *&source,
    llvm::APInt &covered);

llvm::Value *
foldFullInsertOfExtracts(llvm::Value *value,
                         std::map<llvm::Value *, llvm::Value *> &valueMap);

void redirectAddressTakenUses(llvm::Function &oldFunction,
                              llvm::Function &newFunction);

llvm::Value *callerEntryArgumentForSlot(
    llvm::Function &caller, const SignatureShape &callerShape,
    const NativeSignatureSlot &slot, llvm::IRBuilder<> &builder);

void rewriteSignatureShapes(
    llvm::Module &module, SignatureRewriteState &state,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi, NativeRegisterEliminationSummary &summary);

std::string metadataField(const llvm::MDNode &node, llvm::StringRef key);

uint64_t metadataUInt64Field(const llvm::MDNode &node, llvm::StringRef key);

std::string warningReasonForHelper(const llvm::CallInst &helper,
                                   llvm::StringRef kind,
                                   llvm::StringRef regName,
                                   const llvm::Function *callee);

std::vector<NativeRegisterEliminationWarning>
collectRemainingCallValueWarnings(const llvm::Module &module);

  struct RangeLossCause {
    std::string Reason;
    std::string Callee;
  };

class FunctionBuilder {
public:
  FunctionBuilder( llvm::Function &function, const std::map<llvm::GlobalVariable *, RegisterUnit> &units, const std::map<llvm::Function *, FunctionSummaryFacts> &summaryFacts, const AbiFacts &abiFacts, const NativeRegisterEliminationOptions &options, NativeRegisterEliminationFunctionSummary &summary, SignatureRewriteState &signatureState);

  void run();

  void removeDeadStoresAfterSignatureRewrite(bool collectReturns = true);

private:
  llvm::Function &Function;
  const std::map<llvm::GlobalVariable *, RegisterUnit> &Units;
  const std::map<llvm::Function *, FunctionSummaryFacts> &SummaryFacts;
  const AbiFacts &Abi;
  const NativeRegisterEliminationOptions &Options;
  NativeRegisterEliminationFunctionSummary &Summary;
  SignatureRewriteState &SignatureState;
  std::vector<llvm::LoadInst *> Loads;
  std::vector<llvm::WeakVH> PartialReads;
  // 本遍 rewritePartialReads()/rewriteLoads() 没能替换掉的寄存器读（demand 链会
  // 断在这些读上）。rewritePartialWrites() 用它对写侧暂缓下毒。
  std::set<llvm::GlobalVariable *> UnresolvedReadGlobals;
  std::vector<llvm::WeakVH> ReplacedLoads;
  std::vector<llvm::WeakVH> ReplacedPartialReads;
  std::vector<llvm::WeakVH> FoldedZeroXors;
  std::vector<llvm::WeakVH> ReturnObservationValues;
  std::map<BlockRangeKey, llvm::Value *> EntryRangeValue;
  std::map<BlockRangeKey, llvm::Value *> ExitRangeValue;
  std::map<BlockRangeKey, llvm::PHINode *> PendingRangePhi;
  std::set<BlockRangeKey> ResolvingRangeEntry;
  std::map<BlockRangeKey, RangedSSAValue> CurrentDef;
  std::set<BlockRangeKey> UnknownCurrentDef;
  std::map<BlockRangeKey, RangeLossCause> RangeLossCauses;
  std::map<llvm::BasicBlock *, llvm::Instruction *> CurrentDefPosition;
  std::map<llvm::Value *, llvm::Value *> Replacement;
  std::map<llvm::Value *, std::vector<llvm::Value *>> ReplacementSources;
  llvm::DenseSet<llvm::Value *> UndemandedWalkClean;
  mutable const std::vector<llvm::Value *> *LocalRecordedValues = nullptr;
  mutable bool LocalRecordedValuesLooked = false;
  mutable bool FunctionReturnCacheDirty = true;
  mutable llvm::DenseSet<llvm::Value *> RecordedFunctionReturnValues;
  std::set<llvm::PHINode *> DeadPhis;
  std::map<RegisterRangeKey, llvm::Value *> EntryRangeInputs;
  std::map<llvm::Value *, RegisterRangeKey> EntryInputRanges;
  std::map<llvm::GlobalVariable *, llvm::LoadInst *> EntryRegisterLoads;
  std::map<llvm::GlobalVariable *, std::set<uint64_t>> RangeBoundaries;
  std::map<llvm::GlobalVariable *, std::vector<RegisterRangeKey>> PlannedRanges;
  std::map<CallRangeValueKey, llvm::Value *> CallRangeValues;
  std::unique_ptr<NativeStackAddressAnalysis> StackAddresses;
  bool PostSignatureCleanup = false;
  static unsigned valueBitWidth(llvm::Value *value);

  static llvm::APInt fullMaskFor(llvm::Value *value);

  bool callPreservesRegisterByAbi(const RegisterUnit &unit) const;

  static llvm::APInt maskForLowBits(unsigned bitWidth, unsigned lowBits);

  static llvm::APInt shiftedMask(const llvm::APInt &mask, unsigned shift,
                                 unsigned width);

  static llvm::APInt lshrSourceDemand(const llvm::APInt &resultDemand,
                                      unsigned shift, unsigned sourceWidth);

  static llvm::APInt shlSourceDemand(const llvm::APInt &resultDemand,
                                     unsigned shift, unsigned sourceWidth);

  static llvm::APInt partialWriteMask(unsigned fullWidth, unsigned writeWidth,
                                      uint64_t bitOffset);

  static bool isDisjointOr(const llvm::Instruction &inst);

  llvm::APInt
  demandedBits(llvm::Value *value,
               const std::map<llvm::Value *, llvm::APInt> &demands) const;

  llvm::Value *undemandedStoreOperandPoison(llvm::Type *type) const;

  llvm::Value *
  replacePartialRegisterValue(const RegisterUnit &unit, llvm::Value *oldValue,
                              llvm::Value *partialValue, unsigned writeWidth,
                              uint64_t bitOffset, llvm::Instruction *before);

  llvm::Value *insertBitsIntoInteger(llvm::Value *base, llvm::Value *value,
                                     unsigned writeWidth, uint64_t bitOffset,
                                     llvm::Instruction *before,
                                     llvm::Twine name);

  llvm::Value *extractBitsFromIntegerValue(llvm::Value *value,
                                           unsigned readWidth,
                                           uint64_t bitOffset,
                                           llvm::Instruction *before,
                                           llvm::Twine name);

  llvm::Value *extractPartialRegisterValue(const RegisterUnit &unit,
                                           llvm::Value *fullValue,
                                           unsigned readWidth,
                                           uint64_t bitOffset,
                                           llvm::Instruction *before);

  llvm::Value *extractBitsFromInteger(llvm::Value *value, unsigned readWidth,
                                      uint64_t bitOffset,
                                      llvm::Instruction *before,
                                      llvm::Twine name);

  void eraseTriviallyDeadNonPhiTree(llvm::Instruction *root);

  bool rewriteUndemandedRegisterStoreOperands(
      llvm::Value *value, const std::map<llvm::Value *, llvm::APInt> &demands,
      llvm::SmallPtrSetImpl<llvm::Value *> &visiting);

  std::map<llvm::Value *, llvm::APInt> computePartialDemands();
  // 本函数里"喂给调用实参"的值的依赖闭包：这些指令的操作数不能按零 demand
  // 下毒（调用实参是最高优先级载体，链上任何一段变成 poison 都会让整个实参
  // 退化成 unknown；hexx64 里 memmove/memset 的长度参数就是这样丢的）。
  void collectCallArgDependencies();
  llvm::DenseSet<llvm::Value *> CallArgDependencyValues;
  bool CallArgDependenciesCollected = false;

  void rewritePartialWrites();

  void eraseDeadPartialWriteCall(llvm::CallBase *call);

  void collectAccesses();

  unsigned registerBitWidth(const RegisterUnit &unit) const;

  void addRangeBoundary(llvm::GlobalVariable *global, uint64_t offset,
                        uint64_t width);

  const RegisterUnit *unitForName(llvm::StringRef name) const;

  void addRangeBoundaryForSlot(const AbiFacts::RegisterSlot &slot);

  void addRangeBoundariesForMask(llvm::GlobalVariable *global,
                                 const llvm::APInt &mask);

  void recordRangeEvent(llvm::GlobalVariable *global, uint64_t offset,
                        uint32_t width, RangeEventKind kind);

  void addAbiRangeBoundaries();

  void addSummaryDemandRangeBoundaries();

  void addCallParamRangeBoundaries();

  void planRegisterRanges();

  std::vector<RegisterRangeKey>
  plannedRangesCovering(llvm::GlobalVariable *global, uint64_t offset,
                        uint32_t width) const;

  static bool rangeContains(const RegisterRangeKey &outer,
                            const RegisterRangeKey &inner);

  static bool rangeContainsAccess(const RegisterRangeKey &range,
                                  llvm::GlobalVariable *global, uint64_t offset,
                                  uint32_t width);

  llvm::Value *materializeCoveredAccess(const RangedSSAValue &def,
                                        uint64_t readOffset, uint32_t readWidth,
                                        llvm::Instruction *before,
                                        llvm::Twine name);

  llvm::Value *tryCurrentCoveredAccess(llvm::GlobalVariable *global,
                                       uint64_t readOffset, uint32_t readWidth,
                                       llvm::Instruction *before,
                                       llvm::Twine name);

  llvm::Value *
  tryCommonExtractSourceAccess(llvm::ArrayRef<RegisterRangeKey> ranges,
                               llvm::ArrayRef<llvm::Value *> segments,
                               uint64_t readOffset, uint32_t readWidth,
                               llvm::Instruction *before, llvm::Twine name);

  bool valueDominatesUse(llvm::Value *value, llvm::Instruction *use,
                         llvm::DominatorTree &domTree) const;

  llvm::Value *assembleRangeReadIfDominating(
      const std::vector<RegisterRangeKey> &ranges, uint64_t readOffset,
      uint32_t readWidth, llvm::Instruction *before, llvm::Twine name,
      llvm::DominatorTree &domTree, bool allowUnknownSegments = false,
      bool entryFallback = false);

  llvm::Value *
  readAccessRangeIfDominating(llvm::GlobalVariable *global, uint64_t readOffset,
                              uint32_t readWidth, llvm::Instruction *before,
                              llvm::Twine name, llvm::DominatorTree &domTree,
                              bool allowUnknownSegments = false);

  llvm::Value *readFullRangeValueBefore(llvm::BasicBlock &block,
                                        const RegisterUnit &unit,
                                        llvm::Instruction *before,
                                        llvm::DominatorTree &domTree,
                                        bool allowUnknownSegments = false);

  bool hasReplacementMetadata(const llvm::Instruction &inst) const;

  void removeReplacementSource(llvm::Value *target, llvm::Value *key);

  void recordReplacement(llvm::Value *key, llvm::Value *target);

  void forgetReplacementValue(llvm::Value *value);

  void eraseInstructionFromParent(llvm::Instruction &inst);

  void rewritePartialReads();

  void rewriteLoads();

  void removeDeadPartialReads();

  void removeDeadFoldedZeroXors();

  void removeDeadEntryReads();

  void foldDuplicatePartialReadXors();

  static bool samePartialReadRange(
      const std::optional<NativeRegisterPartialReadInfo> &lhs,
      const std::optional<NativeRegisterPartialReadInfo> &rhs);

  bool hasInterveningWriteToPartialReadRange(
      llvm::CallBase &first, llvm::CallBase &second,
      const NativeRegisterPartialReadInfo &read) const;

  void removeDeadReplacedLoads();

  void invalidateRecordedValueCache() const;

  void ensureFunctionReturnValueCache() const;

  bool isRecordedCallArgStore(llvm::StoreInst *store) const;

  bool isRecordedCallArgValue(llvm::Value *value) const;

  bool isRecordedFunctionReturnValue(llvm::Value *value) const;

  void removeDeadStoresByLiveness();

  LiveRegisterRanges transferBlockLiveness(llvm::BasicBlock &block,
                                           LiveRegisterRanges live);

  void eraseDeadStoresInBlock(llvm::BasicBlock &block,
                              LiveRegisterRanges live);

  void transferStoreLiveness(llvm::StoreInst &store,
                             LiveRegisterRanges &live) const;

  void transferLoadLiveness(llvm::LoadInst &load,
                            LiveRegisterRanges &live) const;

  bool transferPartialReadLiveness(llvm::CallBase &call,
                                   LiveRegisterRanges &live) const;

  bool transferPartialWriteLiveness(llvm::CallBase &call,
                                    LiveRegisterRanges &live) const;

  void transferCallLiveness(llvm::CallBase &call,
                            LiveRegisterRanges &live) const;

  void addExitLiveRegisters(LiveRegisterRanges &live) const;

  bool rangesOverlap(const RegisterRangeKey &lhs,
                     const RegisterRangeKey &rhs) const;

  void insertMaskRanges(LiveRegisterRanges &live, llvm::GlobalVariable *global,
                        const llvm::APInt &mask) const;

  void eraseRegisterEffectRanges(LiveRegisterRanges &live,
                                 const RegisterUnit &unit) const;

  void insertGlobalRanges(LiveRegisterRanges &live,
                          llvm::GlobalVariable *global) const;

  void eraseGlobalRanges(LiveRegisterRanges &live,
                         llvm::GlobalVariable *global) const;

  void insertPartialRanges(LiveRegisterRanges &live,
                           llvm::GlobalVariable *global, uint64_t offset,
                           uint32_t width) const;

  void erasePartialRanges(LiveRegisterRanges &live,
                          llvm::GlobalVariable *global, uint64_t offset,
                          uint32_t width) const;

  bool hasLiveGlobalRange(const LiveRegisterRanges &live,
                          llvm::GlobalVariable *global) const;

  bool hasLivePartialRange(const LiveRegisterRanges &live,
                           llvm::GlobalVariable *global, uint64_t offset,
                           uint32_t width) const;

  bool hasLiveWriteRange(const LiveRegisterRanges &live,
                         const RegisterUnit &unit) const;

  const RegisterUnit *unitForRange(const RegisterRangeKey &range) const;

  llvm::IntegerType *rangeType(const RegisterRangeKey &range) const;

  llvm::Value *unknownRangeBefore(llvm::Instruction &insertBefore,
                                  const RegisterRangeKey &range,
                                  llvm::Twine suffix,
                                  llvm::StringRef callee = "");

  llvm::Value *attributedRangeUnknown(llvm::Instruction &insertBefore,
                                      const RegisterRangeKey &range,
                                      llvm::StringRef fallbackReason);

  llvm::Value *rangeTypedValueOrUnknown(llvm::Value *value,
                                        const RegisterRangeKey &range,
                                        llvm::Instruction *insertBefore,
                                        llvm::Twine suffix);

  llvm::Value *extractRangeValue(const RegisterRangeKey &range,
                                 llvm::Value *source, uint64_t sourceBitOffset,
                                 llvm::Instruction *before, llvm::Twine name);

  llvm::Instruction *
  rangeWriteInsertPoint(llvm::Instruction &writeInst,
                        llvm::Instruction *readBefore) const;

  llvm::Value *coerceStoreValueToRangeWidth(llvm::Value *value,
                                            const RegisterUnit &unit,
                                            llvm::Instruction *insertBefore);

  llvm::Value *writeSegment(llvm::Instruction &writeInst,
                            const RegisterRangeKey &range, llvm::Value *value);

  bool writeCoveredValue(llvm::Instruction &writeInst,
                         const RegisterRangeKey &coveredRange,
                         llvm::Value *value);

  bool writeAccessRange(llvm::Instruction &writeInst,
                        llvm::GlobalVariable *global, uint64_t writeOffset,
                        uint32_t writeWidth, llvm::Value *source,
                        uint64_t sourceBitOffset,
                        llvm::Instruction *insertBefore, llvm::Twine name);

  llvm::Value *currentSegment(llvm::BasicBlock &block,
                              const RegisterRangeKey &range,
                              llvm::Instruction *before);

  void clearBlockRangeDefs(llvm::BasicBlock &block,
                           llvm::GlobalVariable *global);

  void clearAllBlockRangeDefs(llvm::BasicBlock &block);

  void markBlockRangeUnknown(llvm::BasicBlock &block,
                             const RegisterRangeKey &range,
                             const RangeLossCause &cause = {});

  void inheritRangeLossCause(const BlockRangeKey &dst,
                             const BlockRangeKey &src);

  const RangeLossCause *rangeLossCause(const BlockRangeKey &key) const;

  bool transferRangeInstruction(llvm::Instruction &inst,
                                llvm::Instruction *readBefore);

  bool transferRangeBlockUntil(llvm::BasicBlock &block,
                               llvm::Instruction *before);

  llvm::Value *readRangeBefore(llvm::BasicBlock &block,
                               const RegisterRangeKey &range,
                               llvm::Instruction *before,
                               llvm::DominatorTree &domTree);

  llvm::Value *readRangeEntry(llvm::BasicBlock &block,
                              const RegisterRangeKey &range,
                              llvm::DominatorTree &domTree);

  llvm::Value *readRangeExit(llvm::BasicBlock &block,
                             const RegisterRangeKey &range,
                             llvm::DominatorTree &domTree);

  llvm::PHINode *ensureRangePhi(llvm::BasicBlock &block,
                                const RegisterRangeKey &range);

  llvm::Value *completeRangePhi(llvm::BasicBlock &block,
                                const RegisterRangeKey &range,
                                llvm::DominatorTree &domTree);

  llvm::Value *simplifyRangePhi(llvm::PHINode &phi,
                                const RegisterRangeKey &range);

  llvm::Value *readValueBefore(llvm::BasicBlock &block,
                               const RegisterUnit &unit,
                               llvm::Instruction *before);

  llvm::Value *readBlockLocalValueBefore(llvm::LoadInst &load,
                                         const RegisterUnit &unit);

  bool isCompletePhi(const llvm::PHINode &phi) const;

  void finalizePendingPhis();

  void eraseDeadPhis();

  llvm::Value *resolve(llvm::Value *value) const;

  llvm::GlobalVariable *stackPointerGlobal() const;

  std::pair<llvm::Argument *, const NativeSignatureSlot *>
  entryArgumentSlot(const RegisterUnit &unit) const;

  llvm::Value *entryArgument(const RegisterUnit &unit) const;

  llvm::Value *entryRangeArgument(const RegisterRangeKey &range,
                                  llvm::Instruction *insertBefore);

  llvm::MDNode *entryRangeNode(const RegisterRangeKey &range) const;

  llvm::LoadInst *entryRegisterLoad(const RegisterUnit &unit,
                                    llvm::Instruction &insertBefore);

  llvm::Value *entryPartialRead(const RegisterUnit &unit,
                                const RegisterRangeKey &range,
                                llvm::Instruction &insertBefore);

  bool shapeParamCoversRange(const RegisterRangeKey &range) const;

  bool rangeMayComeFromEntry(const RegisterRangeKey &range) const;

  llvm::Value *entryRangeInput(llvm::BasicBlock &block,
                               const RegisterRangeKey &range);

  RangeLossCause unknownCallEffectCause(const llvm::CallBase &call) const;

  CallRegisterEffect callEffect(const llvm::CallBase &call,
                                const RegisterUnit &unit) const;

  bool callReadsRegister(const llvm::CallBase &call,
                         const RegisterUnit &unit) const;

  void collectSignatureCallArgs(bool stackSlotsOnly);

  const RegisterUnit *unitByName(llvm::StringRef name) const;

  std::optional<NativeSignatureSlot>
  signatureSlotForCallInput(const NativeRegisterCallInputSlot &input) const;

  void appendAbiVarArgSlots(std::vector<NativeSignatureSlot> &slots) const;

  void appendAbiStackVarArgSlots(llvm::CallBase &call,
                                 std::vector<NativeSignatureSlot> &slots);

  std::vector<NativeSignatureSlot> callParamSlots(llvm::CallBase &call,
                                                  const SignatureShape &shape);

  std::vector<CallArgStoreBinding>
  callArgStoreBindings(llvm::CallBase &call, const SignatureShape &shape,
                       bool stackSlotsOnly);

  llvm::Value *readSlotValueBefore(llvm::CallBase &call,
                                   const NativeSignatureSlot &slot,
                                   bool entryFallback = false);

  llvm::Value *readSlotValueBefore(llvm::Instruction &before,
                                   const NativeSignatureSlot &slot,
                                   llvm::Twine rangeName,
                                   bool allowUnknownSegments = false,
                                   bool entryFallback = false);

  llvm::Value *composePlannedRangeRead(llvm::GlobalVariable *global,
                                       uint64_t readOffset,
                                       uint32_t readWidth,
                                       llvm::Instruction *before,
                                       llvm::Twine name,
                                       llvm::DominatorTree &domTree,
                                       bool entryFallback = false);

  llvm::Value *readSlotRangeBefore(llvm::Instruction &before,
                                   const NativeSignatureSlot &slot,
                                   llvm::Twine name,
                                   bool allowUnknownSegments = false,
                                   bool entryFallback = false);

  llvm::Value *readStackSlotValueBefore(llvm::CallBase &call,
                                        const NativeSignatureSlot &slot);

  uint64_t stackGridStep(uint32_t alignBytes) const;

  std::optional<uint64_t>
  stackSlotIndex(const NativeSignatureSlot &slot) const;

  NativeStackAddressAnalysis &stackAddressAnalysis();

  std::optional<NativeStackAddress>
  callsiteStackAddress(llvm::CallBase &call, const NativeSignatureSlot &slot);

  llvm::StoreInst *
  scanStackStoresBeforeCall(llvm::CallBase &call,
                            const NativeSignatureSlot &slot,
                            std::vector<RelativeStoreGroup> &relativeStores);

  llvm::StoreInst *
  findNearestStackStoreBeforeCall(llvm::CallBase &call,
                                  const NativeSignatureSlot &slot);

  llvm::StoreInst *
  findNearestStackStoreInBlock(llvm::BasicBlock &block,
                               const NativeStackAddress &expected);

  llvm::Value *
  readCrossBlockStackSlotBeforeCall(
      llvm::CallBase &call, const NativeSignatureSlot &slot,
      const std::vector<RelativeStoreGroup> &relativeStores);

  llvm::StoreInst *findNearestStoreBeforeCall(llvm::CallBase &call,
                                              const RegisterUnit &unit);

  bool isEntryInputValue(llvm::Value *value);

  llvm::Value *callRangeValue(llvm::CallBase &call,
                              const RegisterRangeKey &range);

  bool signatureReturnUsesUnit(llvm::CallBase &call,
                               const RegisterUnit &unit) const;

  bool calleeNeverWritesRange(const llvm::CallBase &call,
                              const RegisterRangeKey &range) const;

  llvm::FunctionCallee callRangeValueHelper(const RegisterRangeKey &range);

  llvm::MDNode *registerNode(const RegisterUnit &unit) const;

  llvm::MDNode *callRangeValueNode(const RegisterRangeKey &range,
                                   llvm::Instruction *call) const;

  void collectFunctionReturnValues(bool recordBinding = true);

  llvm::MDNode *markerNode(llvm::StringRef value) const;

  void attachMetadata();

};

} // namespace notdec::bin2llvm
