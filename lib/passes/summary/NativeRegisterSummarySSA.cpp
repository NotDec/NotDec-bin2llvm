#include "notdec-bin2llvm/passes/summary/NativeRegisterSummarySSA.h"

#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/NativeExternalPrototype.h"
#include "notdec-bin2llvm/NativeRegisterPartialRead.h"
#include "notdec-bin2llvm/NativeRegisterPartialWrite.h"
#include "notdec-bin2llvm/NativeRegisterValueRange.h"
#include "notdec-bin2llvm/passes/summary/NativeRegisterPeephole.h"
#include "notdec-bin2llvm/passes/summary/NativeRegisterSummary.h"
#include "notdec-bin2llvm/passes/summary/NativeStackCanaryCleanup.h"
#include "notdec-bin2llvm/passes/summary/NativeStackFrame.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
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
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace notdec::bin2llvm {
namespace {

using KnownExternalPrototype = NativeExternalPrototype;

struct RegisterUnit {
  llvm::GlobalVariable *Global = nullptr;
  std::string Space;
  std::string Name;
  uint64_t Offset = 0;
  uint64_t Size = 0;
};

struct RegisterAccess {
  const RegisterUnit *Unit = nullptr;
  bool IsRegisterAccess = false;
  bool IsStorageValue = false;
};

struct SummaryRegisterFact {
  bool ReadEntry = false;
  bool MayEntry = true;
  bool MayNonEntry = false;
  bool ExitDemand = false;
  llvm::APInt EntryDemandMask;
  llvm::APInt ExitDemandMask;
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
  std::vector<RegisterSlot> IntegerInputsInOrder;
  std::vector<RegisterSlot> FloatInputsInOrder;
  std::set<std::string> Outputs;
  std::vector<std::string> OutputsInOrder;
  std::vector<RegisterSlot> IntegerOutputsInOrder;
  std::vector<RegisterSlot> FloatOutputsInOrder;
  std::set<std::string> InternalParamRegisters;
  std::set<std::string> InternalReturnRegisters;
  std::set<std::string> Unaffected;
  std::set<std::string> KilledByCall;
};

enum class CallRegisterEffect {
  Preserve,
  ReturnValue,
  Clobber,
  Unknown,
};

// SummarySSA variables are concrete register bit ranges, not whole lifted
// register globals.
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

// A range definition can come from a value that covers exactly this segment
// today, and later from a wider value after full-register writes are fully
// folded into range SSA.  Keeping the covered range explicit avoids treating a
// bare LLVM value as if it always represented the whole register.
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

const CallArgStoreBinding *
bindingForIndex(const std::vector<CallArgStoreBinding> &bindings,
                unsigned index) {
  auto it = std::find_if(bindings.begin(), bindings.end(),
                         [&](const CallArgStoreBinding &binding) {
                           return binding.Index == index;
                         });
  return it == bindings.end() ? nullptr : &*it;
}

// Backward bit demand for values produced while lowering partial register
// writes.  A set bit means some later real observer needs that bit.  Register
// stores are not observers by themselves; later loads, calls, returns,
// branches, and ordinary memory stores seed the demand.
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
};

// A signature slot is the bridge between the ABI slot and the lifted register
// global.  Integer slots keep the old whole-register behavior; float slots
// carry the LLVM scalar type that should be read from or written to the low
// lane of the backing register.
struct NativeSignatureSlot {
  NativeSignatureSlotKind Kind = NativeSignatureSlotKind::IntegerRegister;
  const RegisterUnit *Unit = nullptr;
  std::string AbiName;
  std::string MetaType;
  unsigned OffsetBits = 0;
  unsigned SizeBits = 0;
  llvm::Type *LlvmType = nullptr;
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

// A range return helper records a narrow register segment produced by a direct
// call.  Signature rewrite can then replace it from the rewritten call's real
// return value instead of leaving a summary_return.iN helper in the IR.
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
  std::map<llvm::CallBase *, std::vector<CallArgStoreBinding>> CallArgs;
  std::map<llvm::CallBase *, std::map<std::string, llvm::CallInst *>>
      ReturnHelpers;
  std::map<llvm::CallBase *, std::vector<RangeReturnHelper>> RangeReturnHelpers;
  std::map<llvm::Function *,
           std::map<llvm::ReturnInst *, std::vector<llvm::Value *>>>
      FunctionReturns;
  std::map<llvm::Value *, llvm::Value *> ValueMap;
  std::set<llvm::StoreInst *> StoresToErase;
  std::vector<NativeRegisterSummarySSAWarning> Warnings;
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

const NativeExternalPrototypeMap &
externalPrototypesForState(const SignatureRewriteState &state) {
  if (state.ExternalPrototypes != nullptr) {
    return *state.ExternalPrototypes;
  }
  return defaultNativeExternalPrototypes();
}

const NativeExternalCallShape *
externalCallsiteShapeForCall(const SignatureRewriteState &state,
                             const llvm::CallBase &call) {
  if (state.ExternalCallsiteShapes == nullptr) {
    return nullptr;
  }
  auto it = state.ExternalCallsiteShapes->find(&call);
  return it == state.ExternalCallsiteShapes->end() ? nullptr : &it->second;
}

std::string llvmTypeName(llvm::Type *type) {
  std::string result;
  llvm::raw_string_ostream os(result);
  if (type != nullptr) {
    type->print(os);
  } else {
    os << "<null>";
  }
  return os.str();
}

llvm::Function *getOrInsertUnknownValueHelper(llvm::Module &module,
                                              llvm::Type *type) {
  std::string name = "notdec.unknown." + llvmTypeName(type);
  llvm::FunctionType *functionType =
      llvm::FunctionType::get(type, {}, false);
  if (auto *function = module.getFunction(name)) {
    if (function->getFunctionType() == functionType) {
      return function;
    }
  }
  std::string prefix = name + ".";
  for (llvm::Function &function : module.functions()) {
    if (function.getName().starts_with(prefix) &&
        function.getFunctionType() == functionType) {
      return &function;
    }
  }
  if (module.getNamedValue(name) != nullptr) {
    name += ".typed";
  }
  return llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage,
                                name, module);
}

llvm::Value *unknownValueAt(llvm::IRBuilder<> &builder, llvm::Type *type,
                            llvm::Twine name) {
  llvm::BasicBlock *block = builder.GetInsertBlock();
  if (block == nullptr || block->getModule() == nullptr) {
    return llvm::PoisonValue::get(type);
  }
  return builder.CreateCall(getOrInsertUnknownValueHelper(*block->getModule(),
                                                          type),
                            {}, name);
}

void attachUnknownValueMetadata(llvm::Value *value, llvm::StringRef reason,
                                llvm::StringRef functionName = "",
                                llvm::StringRef calleeName = "",
                                llvm::StringRef registerName = "",
                                llvm::StringRef kind = "",
                                llvm::StringRef detail = "") {
  auto *inst = llvm::dyn_cast_or_null<llvm::Instruction>(value);
  if (inst == nullptr) {
    return;
  }
  llvm::LLVMContext &context = inst->getContext();
  std::vector<llvm::Metadata *> fields;
  fields.push_back(llvm::MDString::get(context, "reason=" + reason.str()));
  if (!functionName.empty()) {
    fields.push_back(llvm::MDString::get(context,
                                         "function=" + functionName.str()));
  }
  if (!calleeName.empty()) {
    fields.push_back(llvm::MDString::get(context, "callee=" + calleeName.str()));
  }
  if (!registerName.empty()) {
    fields.push_back(
        llvm::MDString::get(context, "register=" + registerName.str()));
  }
  if (!kind.empty()) {
    fields.push_back(llvm::MDString::get(context, "kind=" + kind.str()));
  }
  if (!detail.empty()) {
    fields.push_back(llvm::MDString::get(context, "detail=" + detail.str()));
  }
  inst->setMetadata("notdec.unknown.source", llvm::MDNode::get(context, fields));
}

llvm::Value *unknownValueBefore(llvm::Instruction &insertBefore,
                                llvm::Type *type, llvm::Twine name) {
  llvm::IRBuilder<> builder(&insertBefore);
  return unknownValueAt(builder, type, name);
}

llvm::Function *getOrInsertVarArgUnknownHelper(llvm::Module &module,
                                               llvm::Type *type) {
  std::string name = "notdec.register.vararg_unknown." + llvmTypeName(type);
  llvm::FunctionType *functionType =
      llvm::FunctionType::get(type, {}, false);
  return llvm::cast<llvm::Function>(
      module.getOrInsertFunction(name, functionType).getCallee());
}

void attachZeroDemandOperandMetadata(llvm::Instruction &instruction,
                                     unsigned operandIndex,
                                     llvm::Value &original) {
  llvm::LLVMContext &context = instruction.getContext();
  std::vector<llvm::Metadata *> entries;
  if (llvm::MDNode *existing = instruction.getMetadata(
          "notdec.register.summary_ssa.zero_demand_operand")) {
    for (const llvm::MDOperand &operand : existing->operands()) {
      if (llvm::Metadata *metadata = operand.get()) {
        entries.push_back(metadata);
      }
    }
  }

  // The zero constant cannot carry metadata.  Mark the user instruction instead
  // so leaked synthetic zeros can be traced without changing the optimized IR.
  std::vector<llvm::Metadata *> fields = {
      llvm::MDString::get(context, "operand=" + std::to_string(operandIndex)),
      llvm::MDString::get(context,
                          "original_type=" + llvmTypeName(original.getType())),
  };
  if (original.hasName()) {
    fields.push_back(llvm::MDString::get(
        context, "original_name=" + original.getName().str()));
  }
  if (auto *originalInst = llvm::dyn_cast<llvm::Instruction>(&original)) {
    fields.push_back(
        llvm::MDString::get(context, std::string("original_opcode=") +
                                         originalInst->getOpcodeName()));
  }

  entries.push_back(llvm::MDNode::get(context, fields));
  instruction.setMetadata("notdec.register.summary_ssa.zero_demand_operand",
                          llvm::MDNode::get(context, entries));
}

const KnownExternalPrototype *
knownExternalPrototype(const NativeExternalPrototypeMap &prototypes,
                       llvm::StringRef name) {
  return lookupNativeExternalPrototype(prototypes, name);
}

bool isNotDecOpaqueUnknownName(llvm::StringRef name) {
  return name.starts_with("notdec.unknown.");
}

bool isKnownNoReturnExternal(const llvm::Function &function,
                             const NativeExternalPrototypeMap &prototypes) {
  if (!function.isDeclaration()) {
    return false;
  }
  if (function.hasFnAttribute(llvm::Attribute::NoReturn)) {
    return true;
  }
  const KnownExternalPrototype *known =
      knownExternalPrototype(prototypes, function.getName());
  return known != nullptr && known->NoReturn;
}

bool isKnownExternalFunction(const llvm::Function &function,
                             const NativeExternalPrototypeMap &prototypes) {
  return knownExternalPrototype(prototypes, function.getName()) != nullptr;
}

bool isUnknownExternalFunction(const llvm::Function &function,
                               const NativeExternalPrototypeMap &prototypes) {
  return function.isDeclaration() && !function.isIntrinsic() &&
         !function.getName().starts_with("notdec.register.") &&
         !isNotDecOpaqueUnknownName(function.getName()) &&
         !isNativeRegisterValueRangeName(function.getName()) &&
         !isNativeRegisterPartialReadName(function.getName()) &&
         !isNativeRegisterPartialWriteName(function.getName()) &&
         !isKnownExternalFunction(function, prototypes);
}

bool isKnownNoReturnCall(
    const llvm::Function &callee, const NativeExternalPrototypeMap &prototypes,
    const std::map<llvm::Function *, FunctionSummaryFacts> *facts) {
  if (isKnownNoReturnExternal(callee, prototypes)) {
    return true;
  }
  if (facts == nullptr || callee.isDeclaration()) {
    return false;
  }
  auto factIt = facts->find(const_cast<llvm::Function *>(&callee));
  return factIt != facts->end() && factIt->second.NoReturn;
}

void truncateKnownNoReturnCalls(
    llvm::Module &module, const NativeExternalPrototypeMap &prototypes,
    const std::map<llvm::Function *, FunctionSummaryFacts> *facts = nullptr) {
  std::vector<std::pair<llvm::Instruction *, llvm::Function *>> work;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &inst : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
        if (call == nullptr || call->getNextNode() == nullptr) {
          continue;
        }
        llvm::Function *callee = call->getCalledFunction();
        if (callee == nullptr ||
            !isKnownNoReturnCall(*callee, prototypes, facts)) {
          continue;
        }
        work.push_back({call->getNextNode(), &function});
        break;
      }
    }
  }

  std::set<llvm::Function *> changedFunctions;
  for (auto [truncatePoint, function] : work) {
    llvm::changeToUnreachable(truncatePoint);
    changedFunctions.insert(function);
  }
  for (llvm::Function *function : changedFunctions) {
    llvm::removeUnreachableBlocks(*function);
  }
}

void eraseUnusedSummaryHelperDeclarations(llvm::Module &module) {
  std::vector<llvm::Function *> deadHelpers;
  for (llvm::Function &function : module) {
    if (function.isDeclaration() && function.use_empty() &&
        function.getName().starts_with("notdec.register.summary_")) {
      deadHelpers.push_back(&function);
    }
  }
  for (llvm::Function *function : deadHelpers) {
    function->eraseFromParent();
  }
}

uint64_t eraseDeadSummarySSAEntryReads(llvm::Module &module) {
  std::vector<llvm::Instruction *> deadReads;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      if (!inst.use_empty()) {
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        if (load->getMetadata("notdec.register.summary_ssa.entry") != nullptr) {
          deadReads.push_back(load);
        }
        continue;
      }
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr ||
          call->getMetadata("notdec.register.summary_ssa.range_entry") ==
              nullptr ||
          !parseNativeRegisterPartialRead(*call)) {
        continue;
      }
      deadReads.push_back(call);
    }
  }

  for (llvm::Instruction *inst : deadReads) {
    if (inst->getParent() == nullptr || !inst->use_empty()) {
      continue;
    }
    inst->eraseFromParent();
  }
  return deadReads.size();
}

uint64_t eraseDeadSummaryCallValueHelpers(llvm::Module &module) {
  std::vector<llvm::CallBase *> deadHelpers;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr || !call->use_empty()) {
        continue;
      }
      llvm::Function *callee = call->getCalledFunction();
      if (callee == nullptr ||
          !callee->getName().starts_with("notdec.register.summary_")) {
        continue;
      }
      if (call->getMetadata("notdec.register.summary_ssa.call_value") ==
          nullptr) {
        continue;
      }
      deadHelpers.push_back(call);
    }
  }
  for (llvm::CallBase *call : deadHelpers) {
    call->eraseFromParent();
  }
  return deadHelpers.size();
}

std::optional<std::string> mdField(const llvm::MDNode *node,
                                   llvm::StringRef key) {
  if (node == nullptr) {
    return std::nullopt;
  }
  std::string prefix = (key + "=").str();
  for (const llvm::MDOperand &operand : node->operands()) {
    auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
    if (text == nullptr) {
      continue;
    }
    llvm::StringRef value = text->getString();
    if (value.starts_with(prefix)) {
      return value.drop_front(prefix.size()).str();
    }
  }
  return std::nullopt;
}

std::string unitName(const llvm::GlobalVariable &global) {
  if (auto name = mdField(global.getMetadata("notdec.register"), "name")) {
    if (!name->empty()) {
      return *name;
    }
  }
  return global.getName().str();
}

uint64_t unitOffset(const llvm::GlobalVariable &global) {
  if (auto offset = mdField(global.getMetadata("notdec.register"), "offset")) {
    uint64_t value = 0;
    if (!llvm::StringRef(*offset).getAsInteger(10, value)) {
      return value;
    }
  }
  return 0;
}

uint64_t unitSize(const llvm::GlobalVariable &global) {
  if (auto size = mdField(global.getMetadata("notdec.register"), "size")) {
    uint64_t value = 0;
    if (!llvm::StringRef(*size).getAsInteger(10, value)) {
      return value;
    }
  }
  return 0;
}

std::string unitSpace(const llvm::GlobalVariable &global) {
  if (auto space = mdField(global.getMetadata("notdec.register"), "space")) {
    return *space;
  }
  return "";
}

std::map<llvm::GlobalVariable *, RegisterUnit>
collectRegisterUnits(llvm::Module &module) {
  std::map<llvm::GlobalVariable *, RegisterUnit> units;
  for (llvm::GlobalVariable &global : module.globals()) {
    if (global.getMetadata("notdec.register") == nullptr) {
      continue;
    }
    RegisterUnit unit;
    unit.Global = &global;
    unit.Space = unitSpace(global);
    unit.Name = unitName(global);
    unit.Offset = unitOffset(global);
    unit.Size = unitSize(global);
    units.emplace(&global, std::move(unit));
  }
  return units;
}

llvm::MDNode *registerAccessNode(llvm::LLVMContext &context,
                                 const RegisterUnit &unit) {
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "base=" + unit.Name),
      llvm::MDString::get(context, "space=" + unit.Space),
      llvm::MDString::get(context, "offset=" + std::to_string(unit.Offset)),
      llvm::MDString::get(context, "size=" + std::to_string(unit.Size)),
      llvm::MDString::get(context, "name=" + unit.Name),
  };
  return llvm::MDNode::get(context, fields);
}

RegisterAccess
registerLoad(llvm::LoadInst &load,
             const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      load.getPointerOperand()->stripPointerCasts());
  if (global == nullptr) {
    return {};
  }
  auto it = units.find(global);
  if (it == units.end()) {
    return {};
  }
  if (load.getMetadata("notdec.register.access") == nullptr &&
      global->getMetadata("notdec.register") == nullptr) {
    return {};
  }
  return RegisterAccess{&it->second, true,
                        load.getType() == global->getValueType()};
}

RegisterAccess
registerStore(llvm::StoreInst &store,
              const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      store.getPointerOperand()->stripPointerCasts());
  if (global == nullptr) {
    return {};
  }
  auto it = units.find(global);
  if (it == units.end()) {
    return {};
  }
  if (store.getMetadata("notdec.register.access") == nullptr &&
      global->getMetadata("notdec.register") == nullptr) {
    return {};
  }
  return RegisterAccess{&it->second, true,
                        store.getValueOperand()->getType() ==
                            global->getValueType()};
}

llvm::GlobalVariable *registerGlobalFromValue(
    llvm::Value *value,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  auto *global =
      llvm::dyn_cast<llvm::GlobalVariable>(value->stripPointerCasts());
  if (global == nullptr || units.find(global) == units.end()) {
    return nullptr;
  }
  return global;
}

bool valueTypeMatchesRegisterStorage(llvm::Type *valueType,
                                     const llvm::GlobalVariable &global) {
  return valueType == global.getValueType();
}

uint64_t canonicalizeRegisterPointerPhiLoads(
    llvm::Module &module,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  // InstCombine may turn a value PHI over register loads into
  // load(phi ptr @REG...).  SummarySSA only understands direct register global
  // accesses, so expose those loads again before building the register summary.
  std::vector<llvm::LoadInst *> loads;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
      if (load == nullptr || load->isVolatile() || load->isAtomic()) {
        continue;
      }
      if (llvm::isa<llvm::PHINode>(load->getPointerOperand())) {
        loads.push_back(load);
      }
    }
  }

  uint64_t rewritten = 0;
  for (llvm::LoadInst *load : loads) {
    auto *pointerPhi = llvm::dyn_cast<llvm::PHINode>(load->getPointerOperand());
    if (pointerPhi == nullptr || pointerPhi->getParent() != load->getParent()) {
      continue;
    }

    std::vector<llvm::GlobalVariable *> incomingGlobals;
    incomingGlobals.reserve(pointerPhi->getNumIncomingValues());
    bool allRegisterGlobals = true;
    for (llvm::Value *incoming : pointerPhi->incoming_values()) {
      llvm::GlobalVariable *global = registerGlobalFromValue(incoming, units);
      if (global == nullptr ||
          !valueTypeMatchesRegisterStorage(load->getType(), *global)) {
        allRegisterGlobals = false;
        break;
      }
      incomingGlobals.push_back(global);
    }
    if (!allRegisterGlobals || incomingGlobals.empty()) {
      continue;
    }

    auto *valuePhi = llvm::PHINode::Create(
        load->getType(), pointerPhi->getNumIncomingValues(),
        load->getName().empty() ? "" : (load->getName() + ".regphi"),
        pointerPhi->getParent()->getFirstNonPHIIt());
    for (unsigned index = 0; index < pointerPhi->getNumIncomingValues();
         ++index) {
      llvm::BasicBlock *pred = pointerPhi->getIncomingBlock(index);
      llvm::IRBuilder<> builder(pred->getTerminator());
      const RegisterUnit &unit = units.find(incomingGlobals[index])->second;
      auto *incomingLoad = builder.CreateLoad(
          load->getType(), incomingGlobals[index],
          load->getName().empty() ? "" : (load->getName() + ".regload"));
      incomingLoad->setAlignment(load->getAlign());
      incomingLoad->copyMetadata(*load);
      incomingLoad->setMetadata("notdec.register.access",
                                registerAccessNode(module.getContext(), unit));
      valuePhi->addIncoming(incomingLoad, pred);
    }

    load->replaceAllUsesWith(valuePhi);
    load->eraseFromParent();
    if (pointerPhi->use_empty()) {
      pointerPhi->eraseFromParent();
    }
    ++rewritten;
  }
  return rewritten;
}

bool isNotDecRegisterHelperCall(const llvm::CallBase &call) {
  llvm::Function *callee = call.getCalledFunction();
  return callee != nullptr &&
         (callee->getName().starts_with("notdec.register.") ||
          isNotDecOpaqueUnknownName(callee->getName()));
}

bool isAnalyzableCall(const llvm::CallBase &call) {
  if (isNotDecRegisterHelperCall(call) ||
      parseNativeRegisterValueExtract(call).has_value() ||
      parseNativeRegisterValueInsert(call).has_value() ||
      parseNativeRegisterPartialRead(call).has_value() ||
      parseNativeRegisterPartialWrite(call).has_value()) {
    return false;
  }
  llvm::Function *callee = call.getCalledFunction();
  return callee == nullptr || !callee->isIntrinsic();
}

bool isSegmentBaseUnit(llvm::StringRef name) {
  return name == "FS_OFFSET" || name == "GS_OFFSET";
}

std::string
storageUnitName(const NativeAbiStorage &storage,
                const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  // ABI records may mention partial names such as XMM0_Qa while lifting keeps
  // only the largest overlapping register global such as ZMM0.
  for (const auto &[global, unit] : units) {
    (void)global;
    if (unit.Name == storage.Name) {
      return unit.Name;
    }
  }
  for (llvm::StringRef prefix :
       {llvm::StringRef("XMM"), llvm::StringRef("YMM")}) {
    llvm::StringRef name(storage.Name);
    if (!name.starts_with(prefix)) {
      continue;
    }
    llvm::StringRef rest = name.drop_front(prefix.size());
    size_t digits = 0;
    while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9') {
      ++digits;
    }
    if (digits == 0) {
      continue;
    }
    std::string candidate = ("ZMM" + rest.take_front(digits)).str();
    for (const auto &[global, unit] : units) {
      (void)global;
      if (unit.Name == candidate) {
        return unit.Name;
      }
    }
  }
  for (const auto &[global, unit] : units) {
    (void)global;
    if (unit.Space == storage.Space && unit.Offset <= storage.Offset &&
        storage.Offset < unit.Offset + unit.Size) {
      return unit.Name;
    }
  }
  return storage.Name;
}

void pushUnique(std::vector<std::string> &items, const std::string &item) {
  if (std::find(items.begin(), items.end(), item) == items.end()) {
    items.push_back(item);
  }
}

const RegisterUnit *
unitByName(const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
           llvm::StringRef name);

AbiFacts::RegisterSlot
abiRegisterSlot(const NativeAbiParamEntry &entry, const std::string &unitName,
                const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  AbiFacts::RegisterSlot slot;
  slot.UnitName = unitName;
  slot.AbiName = entry.Storage.Name;
  slot.MetaType = entry.MetaType;
  slot.SizeBits = entry.MaxSize * 8;
  const RegisterUnit *unit = unitByName(units, unitName);
  if (unit != nullptr && entry.Storage.Space == unit->Space &&
      entry.Storage.Offset >= unit->Offset &&
      entry.Storage.Offset < unit->Offset + unit->Size) {
    slot.OffsetBits = (entry.Storage.Offset - unit->Offset) * 8;
  }
  return slot;
}

AbiFacts
collectAbiFacts(const llvm::Module &module,
                const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  AbiFacts facts;
  std::optional<NativeAbiSpec> abi = readNativeAbiMetadata(module);
  if (!abi) {
    return facts;
  }
  for (const NativeAbiParamEntry &entry : abi->Inputs) {
    if (entry.Storage.Kind == NativeAbiStorageKind::Register &&
        !entry.Storage.Name.empty()) {
      std::string name = storageUnitName(entry.Storage, units);
      facts.Inputs.insert(name);
      if (entry.MetaType == "float") {
        facts.FloatInputsInOrder.push_back(abiRegisterSlot(entry, name, units));
        facts.InternalParamRegisters.insert(name);
        continue;
      }
      facts.InternalParamRegisters.insert(name);
      pushUnique(facts.InputsInOrder, name);
      facts.IntegerInputsInOrder.push_back(abiRegisterSlot(entry, name, units));
    }
  }
  for (const NativeAbiParamEntry &entry : abi->Outputs) {
    if (entry.Storage.Kind == NativeAbiStorageKind::Register &&
        !entry.Storage.Name.empty()) {
      std::string name = storageUnitName(entry.Storage, units);
      facts.Outputs.insert(name);
      if (entry.MetaType == "float") {
        facts.FloatOutputsInOrder.push_back(
            abiRegisterSlot(entry, name, units));
        facts.InternalReturnRegisters.insert(name);
        continue;
      }
      facts.InternalReturnRegisters.insert(name);
      pushUnique(facts.OutputsInOrder, name);
      facts.IntegerOutputsInOrder.push_back(
          abiRegisterSlot(entry, name, units));
    }
  }
  for (const NativeAbiEffect &effect : abi->Effects) {
    if (effect.Storage.Kind != NativeAbiStorageKind::Register ||
        effect.Storage.Name.empty()) {
      continue;
    }
    if (effect.Kind == NativeAbiEffectKind::Unaffected) {
      std::string name = storageUnitName(effect.Storage, units);
      facts.Unaffected.insert(name);
      facts.InternalParamRegisters.insert(name);
      facts.InternalReturnRegisters.insert(name);
    } else if (effect.Kind == NativeAbiEffectKind::KilledByCall) {
      facts.KilledByCall.insert(storageUnitName(effect.Storage, units));
    }
  }
  return facts;
}

std::map<llvm::Function *, FunctionSummaryFacts>
summaryFactsByFunction(const NativeRegisterSummary &summary,
                       llvm::Module &module) {
  std::map<llvm::Function *, FunctionSummaryFacts> result;
  for (const NativeRegisterSummaryFunction &functionSummary :
       summary.Functions) {
    llvm::Function *function = module.getFunction(functionSummary.FunctionName);
    if (function == nullptr) {
      continue;
    }
    FunctionSummaryFacts facts;
    facts.NoReturn = functionSummary.NoReturn;
    for (const NativeRegisterSummaryRegister &reg : functionSummary.Registers) {
      auto parseMask = [](llvm::StringRef text) -> llvm::APInt {
        if (text.empty()) {
          return llvm::APInt();
        }
        if (text.consume_front("0x") || text.consume_front("0X")) {
          return llvm::APInt(std::max<unsigned>(1, text.size() * 4), text, 16);
        }
        return llvm::APInt(std::max<unsigned>(1, text.size() * 4), text, 16);
      };
      SummaryRegisterFact fact;
      fact.ReadEntry = reg.ReadEntry;
      fact.MayEntry = reg.MayEntry;
      fact.MayNonEntry = reg.MayNonEntry;
      fact.ExitDemand = reg.ExitDemand;
      fact.EntryDemandMask = parseMask(reg.EntryDemandMaskHex);
      fact.ExitDemandMask = parseMask(reg.ExitDemandMaskHex);
      facts.Registers.emplace(reg.Name, std::move(fact));
    }
    result.emplace(function, std::move(facts));
  }
  return result;
}

std::string typeSuffix(llvm::Type &type) {
  if (auto *integerType = llvm::dyn_cast<llvm::IntegerType>(&type)) {
    return "i" + std::to_string(integerType->getBitWidth());
  }
  return "value";
}

const RegisterUnit *
unitByName(const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
           llvm::StringRef name) {
  for (const auto &[global, unit] : units) {
    (void)global;
    if (unit.Name == name) {
      return &unit;
    }
  }
  return nullptr;
}

llvm::Type *llvmTypeForKnownValue(llvm::LLVMContext &context,
                                  NativeExternalPrototype::ValueType type) {
  switch (type) {
  case NativeExternalPrototype::ValueType::I32:
    return llvm::Type::getInt32Ty(context);
  case NativeExternalPrototype::ValueType::I64:
    return llvm::Type::getInt64Ty(context);
  case NativeExternalPrototype::ValueType::Float:
    return llvm::Type::getFloatTy(context);
  case NativeExternalPrototype::ValueType::Double:
    return llvm::Type::getDoubleTy(context);
  }
  return llvm::Type::getInt64Ty(context);
}

bool isFloatKnownValue(NativeExternalPrototype::ValueType type) {
  return type == NativeExternalPrototype::ValueType::Float ||
         type == NativeExternalPrototype::ValueType::Double;
}

NativeSignatureSlot integerSignatureSlot(const RegisterUnit &unit) {
  NativeSignatureSlot slot;
  slot.Kind = NativeSignatureSlotKind::IntegerRegister;
  slot.Unit = &unit;
  slot.AbiName = unit.Name;
  slot.SizeBits = unit.Global->getValueType()->getScalarSizeInBits();
  slot.LlvmType = unit.Global->getValueType();
  return slot;
}

std::optional<NativeSignatureSlot>
integerSlotForSingleDemandRange(const RegisterUnit &unit,
                                const llvm::APInt &demand) {
  if (demand.getBitWidth() == 0 || demand.isZero()) {
    return std::nullopt;
  }
  auto *registerType =
      llvm::dyn_cast<llvm::IntegerType>(unit.Global->getValueType());
  if (registerType == nullptr) {
    return std::nullopt;
  }
  unsigned registerBits = registerType->getBitWidth();
  llvm::APInt trimmed = demand.zextOrTrunc(registerBits);
  unsigned start = 0;
  while (start < registerBits && !trimmed[start]) {
    ++start;
  }
  unsigned end = start;
  while (end < registerBits && trimmed[end]) {
    ++end;
  }
  if (start == end) {
    return std::nullopt;
  }
  for (unsigned bit = end; bit < registerBits; ++bit) {
    if (trimmed[bit]) {
      return std::nullopt;
    }
  }

  NativeSignatureSlot slot;
  slot.Kind = NativeSignatureSlotKind::IntegerRegister;
  slot.Unit = &unit;
  slot.AbiName = unit.Name;
  slot.OffsetBits = start;
  slot.SizeBits = end - start;
  slot.LlvmType =
      llvm::IntegerType::get(unit.Global->getContext(), slot.SizeBits);
  return slot;
}

std::optional<NativeSignatureSlot> signatureSlotFromAbi(
    const AbiFacts::RegisterSlot &abiSlot,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    llvm::Type *llvmType, NativeSignatureSlotKind kind) {
  const RegisterUnit *unit = unitByName(units, abiSlot.UnitName);
  if (unit == nullptr) {
    return std::nullopt;
  }
  NativeSignatureSlot slot;
  slot.Kind = kind;
  slot.Unit = unit;
  slot.AbiName = abiSlot.AbiName;
  slot.MetaType = abiSlot.MetaType;
  slot.OffsetBits = abiSlot.OffsetBits;
  slot.SizeBits = abiSlot.SizeBits;
  slot.LlvmType = llvmType;
  if (slot.LlvmType == nullptr) {
    slot.LlvmType = unit->Global->getValueType();
  }
  if (kind == NativeSignatureSlotKind::FloatRegister) {
    slot.SizeBits = slot.LlvmType->getScalarSizeInBits();
  }
  return slot;
}

llvm::Type *slotType(const NativeSignatureSlot &slot) {
  return slot.LlvmType != nullptr ? slot.LlvmType
                                  : slot.Unit->Global->getValueType();
}

llvm::Type *singleReturnType(const SignatureShape &shape) {
  if (shape.Returns.empty()) {
    return nullptr;
  }
  return slotType(shape.Returns.front());
}

llvm::Type *returnTypeForShape(llvm::LLVMContext &context,
                               const SignatureShape &shape) {
  if (shape.Returns.empty()) {
    return llvm::Type::getVoidTy(context);
  }
  if (shape.Returns.size() == 1) {
    return singleReturnType(shape);
  }
  std::vector<llvm::Type *> fields;
  fields.reserve(shape.Returns.size());
  for (const NativeSignatureSlot &slot : shape.Returns) {
    fields.push_back(slotType(slot));
  }
  return llvm::StructType::get(context, fields);
}

llvm::FunctionType *functionTypeForShape(llvm::LLVMContext &context,
                                         const SignatureShape &shape) {
  std::vector<llvm::Type *> params;
  params.reserve(shape.Params.size());
  for (const NativeSignatureSlot &slot : shape.Params) {
    params.push_back(slotType(slot));
  }
  return llvm::FunctionType::get(returnTypeForShape(context, shape), params,
                                 shape.VarArg);
}

bool isFloatAbiOutputUnit(const AbiFacts &abi, llvm::StringRef name) {
  return std::any_of(abi.FloatOutputsInOrder.begin(),
                     abi.FloatOutputsInOrder.end(),
                     [&](const AbiFacts::RegisterSlot &slot) {
                       return slot.UnitName == name;
                     });
}

const AbiFacts::RegisterSlot *floatAbiInputSlotForUnit(const AbiFacts &abi,
                                                       llvm::StringRef name) {
  auto it =
      std::find_if(abi.FloatInputsInOrder.begin(), abi.FloatInputsInOrder.end(),
                   [&](const AbiFacts::RegisterSlot &slot) {
                     return slot.UnitName == name;
                   });
  return it == abi.FloatInputsInOrder.end() ? nullptr : &*it;
}

const AbiFacts::RegisterSlot *floatAbiOutputSlotForUnit(const AbiFacts &abi,
                                                        llvm::StringRef name) {
  auto it = std::find_if(abi.FloatOutputsInOrder.begin(),
                         abi.FloatOutputsInOrder.end(),
                         [&](const AbiFacts::RegisterSlot &slot) {
                           return slot.UnitName == name;
                         });
  return it == abi.FloatOutputsInOrder.end() ? nullptr : &*it;
}

llvm::APInt fullMaskForSlot(unsigned registerBits,
                            const AbiFacts::RegisterSlot &slot) {
  if (registerBits == 0 || slot.SizeBits == 0 ||
      slot.OffsetBits >= registerBits) {
    return llvm::APInt(registerBits, 0);
  }
  unsigned end = std::min(registerBits, slot.OffsetBits + slot.SizeBits);
  return llvm::APInt::getBitsSet(registerBits, slot.OffsetBits, end);
}

bool demandFitsSlot(const llvm::APInt &demand,
                    const AbiFacts::RegisterSlot &slot, unsigned registerBits) {
  if (demand.getBitWidth() == 0 || demand.isZero()) {
    return false;
  }
  llvm::APInt slotMask = fullMaskForSlot(registerBits, slot);
  llvm::APInt trimmed = demand.zextOrTrunc(registerBits);
  return !(trimmed & ~slotMask).getBoolValue();
}

llvm::Type *floatTypeForSizeBits(llvm::LLVMContext &context,
                                 unsigned sizeBits) {
  if (sizeBits <= 32) {
    return llvm::Type::getFloatTy(context);
  }
  if (sizeBits <= 64) {
    return llvm::Type::getDoubleTy(context);
  }
  return nullptr;
}

llvm::Type *floatTypeForSlot(llvm::LLVMContext &context,
                             const AbiFacts::RegisterSlot &slot) {
  return floatTypeForSizeBits(context, slot.SizeBits);
}

std::optional<NativeSignatureSlot>
floatSlotForDemand(llvm::LLVMContext &context,
                   const AbiFacts::RegisterSlot &abiSlot,
                   const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
                   const llvm::APInt &demand) {
  const RegisterUnit *unit = unitByName(units, abiSlot.UnitName);
  if (unit == nullptr) {
    return std::nullopt;
  }
  unsigned registerBits = unit->Global->getValueType()->getScalarSizeInBits();
  if (!demandFitsSlot(demand, abiSlot, registerBits)) {
    return std::nullopt;
  }
  llvm::Type *type = floatTypeForSlot(context, abiSlot);
  if (type == nullptr) {
    return std::nullopt;
  }
  return signatureSlotFromAbi(abiSlot, units, type,
                              NativeSignatureSlotKind::FloatRegister);
}

SignatureShape shapeForInternalFunction(
    llvm::Function &function,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const std::map<llvm::Function *, FunctionSummaryFacts> &summaryFacts,
    const AbiFacts &abi) {
  SignatureShape shape;
  auto factsIt = summaryFacts.find(&function);
  if (factsIt == summaryFacts.end()) {
    return shape;
  }

  std::vector<const RegisterUnit *> orderedUnits;
  orderedUnits.reserve(units.size());
  for (const auto &[global, unit] : units) {
    (void)global;
    orderedUnits.push_back(&unit);
  }
  std::sort(orderedUnits.begin(), orderedUnits.end(),
            [](const RegisterUnit *lhs, const RegisterUnit *rhs) {
              if (lhs->Offset != rhs->Offset) {
                return lhs->Offset < rhs->Offset;
              }
              return lhs->Name < rhs->Name;
            });

  const FunctionSummaryFacts &facts = factsIt->second;
  std::set<std::string> addedParamUnits;

  // Internal native functions can be compiled with interprocedural register
  // allocation, so their real interface is not limited to the external ABI.
  // The register summary decides which registers are real inputs; ABI order
  // decides where those inputs appear in the rewritten LLVM function type.
  auto addParamForUnit = [&](const RegisterUnit &unit,
                             const AbiFacts::RegisterSlot *floatSlot) {
    if (addedParamUnits.count(unit.Name) != 0) {
      return;
    }
    auto regIt = facts.Registers.find(unit.Name);
    if (regIt == facts.Registers.end() || !regIt->second.ReadEntry) {
      return;
    }
    if (abi.InternalParamRegisters.count(unit.Name) == 0 &&
        abi.InternalReturnRegisters.count(unit.Name) == 0) {
      return;
    }
    if (floatSlot != nullptr) {
      if (std::optional<NativeSignatureSlot> slot =
              floatSlotForDemand(function.getContext(), *floatSlot, units,
                                 regIt->second.EntryDemandMask)) {
        shape.Params.push_back(*slot);
      } else if (regIt->second.EntryDemandMask.getBitWidth() != 0 &&
                 !regIt->second.EntryDemandMask.isZero()) {
        if (std::optional<NativeSignatureSlot> rangeSlot =
                integerSlotForSingleDemandRange(
                    unit, regIt->second.EntryDemandMask)) {
          shape.Params.push_back(*rangeSlot);
        } else {
          shape.Params.push_back(integerSignatureSlot(unit));
        }
      } else {
        // ReadEntry already says the internal function needs an incoming
        // value.  If the demand walker did not recover a float lane mask, use
        // the backing register type rather than leaving an entry global load
        // in the IR.
        shape.Params.push_back(integerSignatureSlot(unit));
      }
    } else if (std::optional<NativeSignatureSlot> rangeSlot =
                   integerSlotForSingleDemandRange(
                       unit, regIt->second.EntryDemandMask)) {
      shape.Params.push_back(*rangeSlot);
    } else {
      shape.Params.push_back(integerSignatureSlot(unit));
    }
    addedParamUnits.insert(unit.Name);
  };

  for (const AbiFacts::RegisterSlot &slot : abi.IntegerInputsInOrder) {
    if (const RegisterUnit *unit = unitByName(units, slot.UnitName)) {
      addParamForUnit(*unit, nullptr);
    }
  }
  for (const AbiFacts::RegisterSlot &slot : abi.FloatInputsInOrder) {
    if (const RegisterUnit *unit = unitByName(units, slot.UnitName)) {
      addParamForUnit(*unit, &slot);
    }
  }
  for (const RegisterUnit *unit : orderedUnits) {
    addParamForUnit(*unit, floatAbiInputSlotForUnit(abi, unit->Name));
  }

  for (const RegisterUnit *unit : orderedUnits) {
    if (abi.InternalReturnRegisters.count(unit->Name) == 0) {
      continue;
    }
    // Whole-ZMM returns are only safe for lifted void helpers.  If a function
    // already has an LLVM return value, widening it to i512 would overwrite the
    // existing public return shape instead of refining register passing.
    if (!function.getReturnType()->isVoidTy() &&
        isFloatAbiOutputUnit(abi, unit->Name)) {
      continue;
    }
    auto regIt = factsIt->second.Registers.find(unit->Name);
    if (regIt != factsIt->second.Registers.end() && regIt->second.MayNonEntry &&
        regIt->second.ExitDemand) {
      if (const AbiFacts::RegisterSlot *slot =
              floatAbiOutputSlotForUnit(abi, unit->Name)) {
        if (std::optional<NativeSignatureSlot> floatSlot =
                floatSlotForDemand(function.getContext(), *slot, units,
                                   regIt->second.ExitDemandMask)) {
          shape.Returns.push_back(*floatSlot);
        } else if (function.getReturnType()->isVoidTy() &&
                   regIt->second.ExitDemandMask.getBitWidth() != 0 &&
                   !regIt->second.ExitDemandMask.isZero()) {
          if (std::optional<NativeSignatureSlot> rangeSlot =
                  integerSlotForSingleDemandRange(
                      *unit, regIt->second.ExitDemandMask)) {
            shape.Returns.push_back(*rangeSlot);
          } else {
            shape.Returns.push_back(integerSignatureSlot(*unit));
          }
        }
        continue;
      }
      if (std::optional<NativeSignatureSlot> rangeSlot =
              integerSlotForSingleDemandRange(*unit,
                                              regIt->second.ExitDemandMask)) {
        shape.Returns.push_back(*rangeSlot);
      } else {
        shape.Returns.push_back(integerSignatureSlot(*unit));
      }
    }
  }
  return shape;
}

std::optional<NativeSignatureSlot>
typedParamSlot(NativeExternalPrototype::ValueType type, unsigned &integerIndex,
               unsigned &floatIndex,
               const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
               const AbiFacts &abi, llvm::LLVMContext &context) {
  llvm::Type *llvmType = llvmTypeForKnownValue(context, type);
  if (isFloatKnownValue(type)) {
    if (floatIndex >= abi.FloatInputsInOrder.size()) {
      return std::nullopt;
    }
    return signatureSlotFromAbi(abi.FloatInputsInOrder[floatIndex++], units,
                                llvmType,
                                NativeSignatureSlotKind::FloatRegister);
  }
  if (integerIndex >= abi.IntegerInputsInOrder.size()) {
    return std::nullopt;
  }
  return signatureSlotFromAbi(abi.IntegerInputsInOrder[integerIndex++], units,
                              llvmType,
                              NativeSignatureSlotKind::IntegerRegister);
}

std::optional<NativeSignatureSlot>
typedReturnSlot(NativeExternalPrototype::ValueType type,
                const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
                const AbiFacts &abi, llvm::LLVMContext &context) {
  llvm::Type *llvmType = llvmTypeForKnownValue(context, type);
  if (isFloatKnownValue(type)) {
    if (abi.FloatOutputsInOrder.empty()) {
      return std::nullopt;
    }
    return signatureSlotFromAbi(abi.FloatOutputsInOrder.front(), units,
                                llvmType,
                                NativeSignatureSlotKind::FloatRegister);
  }
  if (abi.IntegerOutputsInOrder.empty()) {
    return std::nullopt;
  }
  return signatureSlotFromAbi(abi.IntegerOutputsInOrder.front(), units,
                              llvmType,
                              NativeSignatureSlotKind::IntegerRegister);
}

bool isIntegerAbiOutput(const AbiFacts &abi, llvm::StringRef name) {
  return std::any_of(abi.IntegerOutputsInOrder.begin(),
                     abi.IntegerOutputsInOrder.end(),
                     [&](const AbiFacts::RegisterSlot &slot) {
                       return slot.UnitName == name;
                     });
}

bool isLikelyNonReturnIntegerAbiOutput(const AbiFacts &abi,
                                       llvm::StringRef name) {
  // On x86-64 SysV, RDX is listed as call-clobbered and sometimes appears in
  // ABI output metadata, but ordinary libc calls rarely return a second integer
  // value there.  Without a stronger prototype, keep it as clobber evidence.
  return abi.IntegerOutputsInOrder.size() > 1 &&
         abi.IntegerOutputsInOrder.front().UnitName != name && name == "RDX";
}

std::optional<NativeSignatureSlot>
integerReturnSlotForUnit(const RegisterUnit &unit, const AbiFacts &abi) {
  for (const AbiFacts::RegisterSlot &slot : abi.IntegerOutputsInOrder) {
    if (slot.UnitName == unit.Name) {
      NativeSignatureSlot result;
      result.Kind = NativeSignatureSlotKind::IntegerRegister;
      result.Unit = &unit;
      result.AbiName = slot.AbiName;
      result.MetaType = slot.MetaType;
      result.OffsetBits = slot.OffsetBits;
      result.SizeBits = slot.SizeBits;
      result.LlvmType = unit.Global->getValueType();
      return result;
    }
  }
  return integerSignatureSlot(unit);
}

SignatureShape shapeForKnownExternal(
    llvm::Function &function,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi, const NativeExternalPrototypeMap &prototypes) {
  SignatureShape shape;
  const KnownExternalPrototype *known =
      knownExternalPrototype(prototypes, function.getName());
  if (known == nullptr) {
    return shape;
  }
  if (!known->TypedParams.empty() || known->TypedReturn) {
    unsigned integerIndex = 0;
    unsigned floatIndex = 0;
    for (NativeExternalPrototype::ValueType type : known->TypedParams) {
      std::optional<NativeSignatureSlot> slot = typedParamSlot(
          type, integerIndex, floatIndex, units, abi, function.getContext());
      if (!slot) {
        return shape;
      }
      shape.Params.push_back(*slot);
    }
    if (known->TypedReturn) {
      std::optional<NativeSignatureSlot> slot = typedReturnSlot(
          *known->TypedReturn, units, abi, function.getContext());
      if (slot) {
        shape.Returns.push_back(*slot);
      }
    }
    shape.VarArg = known->VarArg;
    shape.MaxArgs = known->MaxArgs;
    return shape;
  }
  unsigned count =
      std::min<unsigned>(known->FixedArgs, abi.InputsInOrder.size());
  shape.VarArg = known->VarArg;
  shape.MaxArgs = known->MaxArgs;
  for (unsigned index = 0; index < count; ++index) {
    const RegisterUnit *unit = unitByName(units, abi.InputsInOrder[index]);
    if (unit != nullptr) {
      shape.Params.push_back(integerSignatureSlot(*unit));
    }
  }
  return shape;
}

NativeRegisterCallInputSlot callInputSlot(const NativeSignatureSlot &slot) {
  NativeRegisterCallInputSlot input;
  input.UnitName = slot.Unit->Name;
  input.OffsetBits = slot.OffsetBits;
  input.SizeBits = slot.SizeBits;
  input.Float = slot.Kind == NativeSignatureSlotKind::FloatRegister;
  return input;
}

NativeRegisterCallInputSlot callInputSlot(const AbiFacts::RegisterSlot &slot) {
  NativeRegisterCallInputSlot input;
  input.UnitName = slot.UnitName;
  input.OffsetBits = slot.OffsetBits;
  input.SizeBits = slot.SizeBits;
  input.Float = slot.MetaType == "float";
  return input;
}

bool sameCallInputSlot(const NativeRegisterCallInputSlot &lhs,
                       const NativeRegisterCallInputSlot &rhs) {
  return lhs.UnitName == rhs.UnitName && lhs.OffsetBits == rhs.OffsetBits &&
         lhs.SizeBits == rhs.SizeBits && lhs.Float == rhs.Float;
}

NativeExternalCallShapeMap buildExternalCallShapes(
    llvm::Module &module,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi, const NativeExternalPrototypeMap &prototypes) {
  NativeExternalCallShapeMap result;
  for (llvm::Function &function : module) {
    if (!function.isDeclaration() || function.isIntrinsic() ||
        knownExternalPrototype(prototypes, function.getName()) == nullptr) {
      continue;
    }
    SignatureShape signature =
        shapeForKnownExternal(function, units, abi, prototypes);
    const KnownExternalPrototype *known =
        knownExternalPrototype(prototypes, function.getName());
    NativeExternalCallShape callShape;
    callShape.FixedArgs = known->FixedArgs;
    callShape.VarArg = known->VarArg;
    callShape.NoReturn = known->NoReturn;
    callShape.MaxArgs = known->MaxArgs;
    callShape.FixedInputsComplete = signature.Params.size() == known->FixedArgs;
    for (const NativeSignatureSlot &slot : signature.Params) {
      if (slot.Unit != nullptr) {
        callShape.Inputs.push_back(callInputSlot(slot));
      }
    }

    // The preliminary summary reads only fixed inputs.  Keep the remaining ABI
    // slots as evidence candidates for the per-callsite inference step.
    if (callShape.VarArg) {
      auto addRemaining = [&](const AbiFacts::RegisterSlot &slot,
                              std::vector<NativeRegisterCallInputSlot> &tail) {
        NativeRegisterCallInputSlot input = callInputSlot(slot);
        if (std::none_of(callShape.Inputs.begin(), callShape.Inputs.end(),
                         [&](const NativeRegisterCallInputSlot &existing) {
                           return sameCallInputSlot(existing, input);
                         })) {
          tail.push_back(std::move(input));
        }
      };
      for (const AbiFacts::RegisterSlot &slot : abi.IntegerInputsInOrder) {
        addRemaining(slot, callShape.VarArgInputs);
      }
      for (const AbiFacts::RegisterSlot &slot : abi.FloatInputsInOrder) {
        addRemaining(slot, callShape.VarArgInputs);
      }
    }
    result.emplace(function.getName().str(), std::move(callShape));
  }
  return result;
}

std::vector<NativeRegisterCallInputSlot>
unknownExternalEvidenceSlots(const AbiFacts &abi) {
  std::vector<NativeRegisterCallInputSlot> result;
  result.reserve(abi.IntegerInputsInOrder.size());
  for (const AbiFacts::RegisterSlot &slot : abi.IntegerInputsInOrder) {
    result.push_back(callInputSlot(slot));
  }
  return result;
}

bool isLikelyX86_64SysVAbi(const AbiFacts &abi) {
  static constexpr const char *expected[] = {"RDI", "RSI", "RDX",
                                             "RCX", "R8",  "R9"};
  constexpr unsigned expectedCount = sizeof(expected) / sizeof(expected[0]);
  if (abi.IntegerInputsInOrder.size() < expectedCount) {
    return false;
  }
  for (unsigned index = 0; index < expectedCount; ++index) {
    if (abi.IntegerInputsInOrder[index].UnitName != expected[index]) {
      return false;
    }
  }
  return true;
}

unsigned localDefinitionPrefix(
    llvm::ArrayRef<NativeRegisterCallsiteSlotEvidence> slots) {
  unsigned prefix = 0;
  for (const NativeRegisterCallsiteSlotEvidence &slot : slots) {
    if (slot.Index != prefix ||
        slot.Origin != NativeRegisterCallsiteValueOrigin::LocalDefinition) {
      break;
    }
    ++prefix;
  }
  return prefix;
}

unsigned localDefinitionPrefix(const NativeRegisterExternalCallsite &callsite) {
  return localDefinitionPrefix(callsite.Slots);
}

std::vector<NativeRegisterCallsiteSlotEvidence>
renumberedSlotsByKind(const NativeRegisterExternalCallsite &callsite,
                      bool wantFloat) {
  std::vector<NativeRegisterCallsiteSlotEvidence> result;
  for (NativeRegisterCallsiteSlotEvidence slot : callsite.Slots) {
    if (slot.Float != wantFloat) {
      continue;
    }
    slot.Index = result.size();
    result.push_back(std::move(slot));
  }
  return result;
}

bool hasLocalDefinitionAfter(
    llvm::ArrayRef<NativeRegisterCallsiteSlotEvidence> slots, unsigned prefix) {
  return llvm::any_of(
      slots.drop_front(std::min<size_t>(prefix, slots.size())),
      [](const NativeRegisterCallsiteSlotEvidence &slot) {
        return slot.Origin ==
               NativeRegisterCallsiteValueOrigin::LocalDefinition;
      });
}

NativeRegisterCallInputSlot
callInputSlot(const NativeRegisterCallsiteSlotEvidence &slot) {
  NativeRegisterCallInputSlot input;
  input.UnitName = slot.UnitName;
  input.OffsetBits = slot.OffsetBits;
  input.SizeBits = slot.SizeBits;
  input.Float = slot.Float;
  return input;
}

std::optional<unsigned> constantLowByteBeforeCall(
    const NativeRegisterExternalCallsite &callsite,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    llvm::StringRef registerName) {
  auto unitIt = std::find_if(units.begin(), units.end(), [&](const auto &entry) {
    return entry.second.Name == registerName;
  });
  if (unitIt == units.end() || callsite.Call == nullptr ||
      callsite.Call->getParent() == nullptr) {
    return std::nullopt;
  }
  llvm::GlobalVariable *global = unitIt->first;
  for (auto it = callsite.Call->getIterator();
       it != callsite.Call->getParent()->begin();) {
    --it;
    llvm::Instruction &inst = const_cast<llvm::Instruction &>(*it);
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      RegisterAccess access = registerStore(*store, units);
      if (access.Unit == nullptr || access.Unit->Global != global) {
        continue;
      }
      auto *constant =
          llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
      if (constant == nullptr) {
        return std::nullopt;
      }
      return static_cast<unsigned>(constant->getZExtValue() & 0xffu);
    }
    if (auto *otherCall = llvm::dyn_cast<llvm::CallBase>(&inst)) {
      if (std::optional<NativeRegisterPartialWriteInfo> partial =
              parseNativeRegisterPartialWrite(*otherCall)) {
        if (partial->Global != global) {
          continue;
        }
        if (partial->BitOffset != 0 || partial->WriteWidth < 8) {
          return std::nullopt;
        }
        auto *constant = llvm::dyn_cast<llvm::ConstantInt>(partial->Value);
        if (constant == nullptr) {
          return std::nullopt;
        }
        return static_cast<unsigned>(constant->getZExtValue() & 0xffu);
      }
      if (isAnalyzableCall(*otherCall)) {
        return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

NativeRegisterSummarySSAWarning externalInferenceWarning(llvm::StringRef callee,
                                                         llvm::StringRef detail,
                                                         llvm::StringRef reason,
                                                         unsigned uses) {
  NativeRegisterSummarySSAWarning warning;
  warning.FunctionName = "<external-signature>";
  warning.CalleeName = callee.str();
  warning.RegisterName = detail.str();
  warning.Kind = "signature";
  warning.Reason = reason.str();
  warning.Uses = uses;
  return warning;
}

NativeRegisterSummarySSAWarning
varArgInferenceWarning(const NativeRegisterExternalCallsite &callsite,
                       llvm::StringRef detail, llvm::StringRef reason) {
  NativeRegisterSummarySSAWarning warning;
  warning.FunctionName = callsite.CallerName;
  warning.CalleeName = callsite.CalleeName;
  warning.RegisterName = detail.str();
  warning.Kind = "signature";
  warning.Reason = reason.str();
  warning.Uses = 1;
  return warning;
}

llvm::StringRef originName(NativeRegisterCallsiteValueOrigin origin) {
  switch (origin) {
  case NativeRegisterCallsiteValueOrigin::LocalDefinition:
    return "local";
  case NativeRegisterCallsiteValueOrigin::ForwardedEntry:
    return "entry";
  case NativeRegisterCallsiteValueOrigin::CallClobber:
    return "call_clobber";
  case NativeRegisterCallsiteValueOrigin::Mixed:
    return "mixed";
  case NativeRegisterCallsiteValueOrigin::Unknown:
    return "unknown";
  }
  return "unknown";
}

const NativeRegisterCallsiteSlotEvidence *
slotAtPrefix(llvm::ArrayRef<NativeRegisterCallsiteSlotEvidence> slots,
             unsigned prefix) {
  if (prefix >= slots.size()) {
    return nullptr;
  }
  return &slots[prefix];
}

std::string slotDetail(const NativeRegisterCallsiteSlotEvidence &slot) {
  return slot.UnitName + "[" + std::to_string(slot.OffsetBits) + ":" +
         std::to_string(slot.SizeBits) + "]=" +
         originName(slot.Origin).str();
}

void warnIfStoppedAtCallClobber(
    std::vector<NativeRegisterSummarySSAWarning> &warnings,
    const NativeRegisterExternalCallsite &callsite,
    llvm::ArrayRef<NativeRegisterCallsiteSlotEvidence> slots, unsigned prefix,
    llvm::StringRef reason) {
  const NativeRegisterCallsiteSlotEvidence *slot = slotAtPrefix(slots, prefix);
  if (slot == nullptr ||
      slot->Origin != NativeRegisterCallsiteValueOrigin::CallClobber) {
    return;
  }
  warnings.push_back(varArgInferenceWarning(callsite, slotDetail(*slot),
                                            reason));
}

std::string calleeNameForWarning(const llvm::CallBase &call) {
  llvm::Function *callee = call.getCalledFunction();
  return callee == nullptr ? std::string("<indirect>") : callee->getName().str();
}

std::string slotRegisterName(const NativeSignatureSlot &slot) {
  if (slot.Unit != nullptr) {
    return slot.Unit->Name;
  }
  return slot.AbiName;
}

NativeRegisterSummarySSAWarning
callSlotWarning(const llvm::CallBase &call, const NativeSignatureSlot &slot,
                llvm::StringRef kind, llvm::StringRef reason) {
  NativeRegisterSummarySSAWarning warning;
  const llvm::Function *function = call.getFunction();
  warning.FunctionName =
      function == nullptr ? std::string("") : function->getName().str();
  warning.CalleeName = calleeNameForWarning(call);
  warning.RegisterName = slotRegisterName(slot);
  warning.Kind = kind.str();
  warning.Reason = reason.str();
  warning.Uses = 1;
  return warning;
}

NativeRegisterSummarySSAWarning
callRegisterWarning(const llvm::CallBase &call, llvm::StringRef registerName,
                    llvm::StringRef kind, llvm::StringRef reason) {
  NativeRegisterSummarySSAWarning warning;
  const llvm::Function *function = call.getFunction();
  warning.FunctionName =
      function == nullptr ? std::string("") : function->getName().str();
  warning.CalleeName = calleeNameForWarning(call);
  warning.RegisterName = registerName.str();
  warning.Kind = kind.str();
  warning.Reason = reason.str();
  warning.Uses = 1;
  return warning;
}

NativeRegisterSummarySSAWarning
returnSlotWarning(const llvm::Function &function, const NativeSignatureSlot &slot,
                  llvm::StringRef reason) {
  NativeRegisterSummarySSAWarning warning;
  warning.FunctionName = function.getName().str();
  warning.CalleeName = "<return>";
  warning.RegisterName = slotRegisterName(slot);
  warning.Kind = "return_binding";
  warning.Reason = reason.str();
  warning.Uses = 1;
  return warning;
}

NativeExternalCallsiteShapeMap inferExternalCallShapes(
    NativeExternalPrototypeMap &prototypes,
    const NativeRegisterSummary &baseSummary,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    std::vector<NativeRegisterSummarySSAWarning> &warnings) {
  NativeExternalCallsiteShapeMap callsiteShapes;
  std::map<std::string, std::vector<unsigned>> aritiesByCallee;
  for (const NativeRegisterExternalCallsite &callsite :
       baseSummary.ExternalCallsites) {
    if (callsite.Kind == NativeRegisterExternalCallsiteKind::UnknownExternal) {
      if (!callsite.Indirect) {
        unsigned prefix = localDefinitionPrefix(callsite);
        aritiesByCallee[callsite.CalleeName].push_back(prefix);
        warnIfStoppedAtCallClobber(
            warnings, callsite, callsite.Slots, prefix,
            "unknown_external_arity_stopped_at_call_clobber");
      }
      continue;
    }

    if (callsite.Call == nullptr) {
      continue;
    }
    NativeExternalCallShape shape;
    shape.Inputs = callsite.FixedInputs;
    shape.FixedArgs = callsite.FixedArgs;
    shape.VarArg = true;
    shape.MaxArgs = callsite.MaxArgs;
    shape.FixedInputsComplete = callsite.FixedInputsComplete;

    if (!callsite.FixedInputsComplete) {
      warnings.push_back(varArgInferenceWarning(
          callsite, "fixed=" + std::to_string(callsite.FixedArgs),
          "incomplete_fixed_vararg_mapping"));
      callsiteShapes.emplace(callsite.Call, std::move(shape));
      continue;
    }
    std::vector<NativeRegisterCallsiteSlotEvidence> integerSlots =
        renumberedSlotsByKind(callsite, false);
    std::vector<NativeRegisterCallsiteSlotEvidence> floatSlots =
        renumberedSlotsByKind(callsite, true);
    std::optional<unsigned> sseVarArgs =
        constantLowByteBeforeCall(callsite, units, "RAX");
    if (sseVarArgs && *sseVarArgs != 0) {
      unsigned wanted = std::min<unsigned>(*sseVarArgs, floatSlots.size());
      unsigned added = 0;
      for (const NativeRegisterCallsiteSlotEvidence &slot : floatSlots) {
        if (added >= wanted ||
            slot.Origin != NativeRegisterCallsiteValueOrigin::LocalDefinition) {
          break;
        }
        shape.Inputs.push_back(callInputSlot(slot));
        ++added;
      }
      if (added < *sseVarArgs) {
        warnings.push_back(varArgInferenceWarning(
            callsite,
            "float_tail=" + std::to_string(added) + "/" +
                std::to_string(*sseVarArgs),
            "incomplete_float_vararg_mapping"));
        warnIfStoppedAtCallClobber(
            warnings, callsite, floatSlots, added,
            "float_vararg_evidence_stopped_at_call_clobber");
      }
      callsiteShapes.emplace(callsite.Call, std::move(shape));
      continue;
    }

    unsigned rawPrefix = localDefinitionPrefix(integerSlots);
    unsigned allowed = integerSlots.size();
    if (callsite.MaxArgs != 0) {
      unsigned maxTail = callsite.MaxArgs > callsite.FixedArgs
                             ? callsite.MaxArgs - callsite.FixedArgs
                             : 0;
      allowed = std::min(allowed, maxTail);
    }
    unsigned inferredTail = std::min(rawPrefix, allowed);
    for (unsigned index = 0; index < inferredTail; ++index) {
      shape.Inputs.push_back(callInputSlot(integerSlots[index]));
    }
    if (rawPrefix > allowed) {
      warnings.push_back(
          varArgInferenceWarning(callsite,
                                 "tail=" + std::to_string(rawPrefix) +
                                     "/max=" + std::to_string(allowed),
                                 "vararg_evidence_truncated_by_max_args"));
    }
    if (hasLocalDefinitionAfter(integerSlots, rawPrefix + 1)) {
      warnings.push_back(varArgInferenceWarning(
          callsite, "tail=" + std::to_string(inferredTail),
          "non_contiguous_vararg_evidence"));
    }
    warnIfStoppedAtCallClobber(
        warnings, callsite, integerSlots, rawPrefix,
        "vararg_evidence_stopped_at_call_clobber");
    callsiteShapes.emplace(callsite.Call, std::move(shape));
  }

  for (const auto &[callee, arities] : aritiesByCallee) {
    unsigned minArity = *std::min_element(arities.begin(), arities.end());
    unsigned maxArity = *std::max_element(arities.begin(), arities.end());
    std::string detail = "arity=" + std::to_string(minArity) + ".." +
                         std::to_string(maxArity) +
                         "/final=" + std::to_string(maxArity);
    if (minArity != maxArity) {
      warnings.push_back(externalInferenceWarning(
          callee, detail, "inconsistent_unknown_external_arity",
          arities.size()));
    }
    if (maxArity == 0) {
      warnings.push_back(externalInferenceWarning(
          callee, detail, "unresolved_unknown_external_signature",
          arities.size()));
      continue;
    }
    NativeExternalPrototype prototype;
    prototype.FixedArgs = maxArity;
    // The first pass only infers input arity.  Do not cap unknown external
    // returns to the built-in one-register default; later demand decides them.
    prototype.MaxReturnRegisters = std::numeric_limits<unsigned>::max();
    prototypes[callee] = std::move(prototype);
    warnings.push_back(externalInferenceWarning(
        callee, detail, "inferred_unknown_external_arity", arities.size()));
  }
  return callsiteShapes;
}

bool rangeReturnHelpersUseRegister(
    llvm::ArrayRef<RangeReturnHelper> helpers,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    llvm::StringRef name) {
  for (const RangeReturnHelper &helper : helpers) {
    auto unitIt = units.find(helper.Range.Global);
    if (unitIt != units.end() && unitIt->second.Name == name) {
      return true;
    }
  }
  return false;
}

bool signatureHasReturnForRegister(const SignatureShape &shape,
                                   const RegisterUnit &unit) {
  for (const NativeSignatureSlot &slot : shape.Returns) {
    if (slot.Unit == &unit || slot.Unit->Name == unit.Name) {
      return true;
    }
  }
  return false;
}

void addReturnSlotForRange(
    SignatureShape &shape, const RegisterRangeKey &range,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  auto unitIt = units.find(range.Global);
  if (unitIt == units.end()) {
    return;
  }
  const RegisterUnit &unit = unitIt->second;
  if (signatureHasReturnForRegister(shape, unit)) {
    return;
  }
  // Indirect calls often have several demanded ranges from the same ABI return
  // register, e.g. RAX[0:32] and RAX[32:64].  Use the whole register return and
  // let extractReturnRange() split out the exact bits for each helper.
  shape.Returns.push_back(integerSignatureSlot(unit));
}

void addAbiInputParamSlots(
    SignatureShape &shape,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi) {
  if (!shape.Params.empty()) {
    return;
  }
  for (const std::string &name : abi.InputsInOrder) {
    const RegisterUnit *unit = unitByName(units, name);
    if (unit != nullptr) {
      shape.Params.push_back(integerSignatureSlot(*unit));
    }
  }
}

void addIndirectCallsiteShapes(
    SignatureRewriteState &state,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi) {
  for (const auto &[call, helpers] : state.ReturnHelpers) {
    if (call == nullptr || call->getParent() == nullptr ||
        call->getCalledFunction() != nullptr) {
      continue;
    }
    SignatureShape &shape = state.IndirectCallShapes[call];
    addAbiInputParamSlots(shape, units, abi);
    for (const auto &[name, helper] : helpers) {
      (void)helper;
      const RegisterUnit *unit = unitByName(units, name);
      if (unit != nullptr && !signatureHasReturnForRegister(shape, *unit)) {
        shape.Returns.push_back(integerSignatureSlot(*unit));
      }
    }
  }

  for (const auto &[call, helpers] : state.RangeReturnHelpers) {
    if (call == nullptr || call->getParent() == nullptr ||
        call->getCalledFunction() != nullptr) {
      continue;
    }
    SignatureShape &shape = state.IndirectCallShapes[call];
    addAbiInputParamSlots(shape, units, abi);
    for (const RangeReturnHelper &helper : helpers) {
      addReturnSlotForRange(shape, helper.Range, units);
    }
  }
}

void addDemandedExternalReturns(
    SignatureRewriteState &state,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi, const NativeExternalPrototypeMap &prototypes) {
  std::set<llvm::CallBase *> calls;
  for (const auto &[call, helpers] : state.ReturnHelpers) {
    (void)helpers;
    calls.insert(call);
  }
  for (const auto &[call, helpers] : state.RangeReturnHelpers) {
    (void)helpers;
    calls.insert(call);
  }

  for (llvm::CallBase *call : calls) {
    llvm::Function *callee = call->getCalledFunction();
    if (callee == nullptr || !callee->isDeclaration()) {
      continue;
    }
    auto shapeIt = state.Shapes.try_emplace(callee).first;
    auto helpersIt = state.ReturnHelpers.find(call);
    auto rangeHelpersIt = state.RangeReturnHelpers.find(call);
    auto hasDemandedReturn = [&](llvm::StringRef name) {
      if (helpersIt != state.ReturnHelpers.end() &&
          helpersIt->second.count(name.str()) != 0) {
        return true;
      }
      return rangeHelpersIt != state.RangeReturnHelpers.end() &&
             rangeReturnHelpersUseRegister(rangeHelpersIt->second, units, name);
    };
    std::optional<unsigned> maxReturnRegisters;
    const KnownExternalPrototype *known =
        knownExternalPrototype(prototypes, callee->getName());
    if (known != nullptr) {
      maxReturnRegisters = known->NoReturn ? 0 : known->MaxReturnRegisters;
    }
    unsigned returnIndex = 0;
    for (const std::string &name : abi.OutputsInOrder) {
      if (maxReturnRegisters.has_value() &&
          returnIndex >= *maxReturnRegisters) {
        break;
      }
      // Without a strong external prototype on x86-64, RDX is usually
      // caller-clobbered, not a second return value.
      if (!maxReturnRegisters.has_value() &&
          isLikelyNonReturnIntegerAbiOutput(abi, name)) {
        break;
      }
      if (!hasDemandedReturn(name)) {
        ++returnIndex;
        continue;
      }
      bool alreadyPresent = false;
      for (const NativeSignatureSlot &slot : shapeIt->second.Returns) {
        alreadyPresent |= slot.Unit->Name == name;
      }
      if (!alreadyPresent) {
        const RegisterUnit *unit = unitByName(units, name);
        if (unit != nullptr) {
          if (std::optional<NativeSignatureSlot> slot =
                  integerReturnSlotForUnit(*unit, abi)) {
            shapeIt->second.Returns.push_back(*slot);
          }
        }
      }
      ++returnIndex;
    }
  }
}

bool isDirectSummaryClobberValue(const llvm::Value *value) {
  auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(value);
  if (call == nullptr) {
    return false;
  }
  llvm::Function *callee = call->getCalledFunction();
  return callee != nullptr &&
         callee->getName().starts_with("notdec.register.summary_clobber");
}

bool mayDependOnSummaryClobberValue(
    const llvm::Value *value,
    llvm::SmallPtrSetImpl<const llvm::Value *> &visiting) {
  if (value == nullptr || !visiting.insert(value).second) {
    return false;
  }
  if (isDirectSummaryClobberValue(value)) {
    return true;
  }
  if (auto *phi = llvm::dyn_cast<llvm::PHINode>(value)) {
    return llvm::any_of(
        phi->incoming_values(), [&](const llvm::Value *incoming) {
          return mayDependOnSummaryClobberValue(incoming, visiting);
        });
  }
  if (auto *select = llvm::dyn_cast<llvm::SelectInst>(value)) {
    return mayDependOnSummaryClobberValue(select->getTrueValue(), visiting) ||
           mayDependOnSummaryClobberValue(select->getFalseValue(), visiting);
  }
  if (auto *cast = llvm::dyn_cast<llvm::CastInst>(value)) {
    return mayDependOnSummaryClobberValue(cast->getOperand(0), visiting);
  }
  if (auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(value)) {
    return mayDependOnSummaryClobberValue(binary->getOperand(0), visiting) ||
           mayDependOnSummaryClobberValue(binary->getOperand(1), visiting);
  }
  return false;
}

bool mayDependOnSummaryClobberValue(const llvm::Value *value) {
  llvm::SmallPtrSet<const llvm::Value *, 8> visiting;
  return mayDependOnSummaryClobberValue(value, visiting);
}

unsigned
callsiteBoundArgPrefix(const std::vector<CallArgStoreBinding> &bindings) {
  unsigned prefix = 0;
  for (const CallArgStoreBinding &binding : bindings) {
    if (binding.Index != prefix) {
      break;
    }
    // A clobber helper means the register only came from a previous call's
    // caller-clobbered state.  It is not strong evidence that this call really
    // has that ABI argument.
    if (mayDependOnSummaryClobberValue(binding.RegisterValue)) {
      break;
    }
    ++prefix;
  }
  return prefix;
}

void refineIndirectCallsiteParamShapes(SignatureRewriteState &state) {
  for (auto &[call, shape] : state.IndirectCallShapes) {
    if (call == nullptr || call->getParent() == nullptr || shape.VarArg) {
      continue;
    }
    auto bindingsIt = state.CallArgs.find(call);
    unsigned finalArity = bindingsIt == state.CallArgs.end()
                              ? 0
                              : callsiteBoundArgPrefix(bindingsIt->second);
    if (finalArity < shape.Params.size()) {
      shape.Params.resize(finalArity);
    }
    if (bindingsIt == state.CallArgs.end()) {
      continue;
    }
    bindingsIt->second.erase(
        std::remove_if(bindingsIt->second.begin(), bindingsIt->second.end(),
                       [&](const CallArgStoreBinding &binding) {
                         return binding.Index >= finalArity;
                       }),
        bindingsIt->second.end());
  }
}

void markSignatureCallArgStores(SignatureRewriteState &state,
                                NativeRegisterSummarySSASummary &summary) {
  for (const auto &[call, bindings] : state.CallArgs) {
    (void)call;
    for (const CallArgStoreBinding &binding : bindings) {
      if (binding.Store != nullptr &&
          state.StoresToErase.insert(binding.Store).second) {
        ++summary.CallArgStoresMarked;
      }
    }
  }
}

std::map<llvm::Function *, SignatureShape> buildInitialSignatureShapes(
    llvm::Module &module,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const std::map<llvm::Function *, FunctionSummaryFacts> &summaryFacts,
    const AbiFacts &abi, const NativeExternalPrototypeMap &prototypes) {
  std::map<llvm::Function *, SignatureShape> shapes;
  for (llvm::Function &function : module) {
    if (function.isIntrinsic() ||
        function.getName().starts_with("notdec.register.") ||
        isNotDecOpaqueUnknownName(function.getName()) ||
        isNativeRegisterValueRangeName(function.getName()) ||
        isNativeRegisterPartialReadName(function.getName()) ||
        isNativeRegisterPartialWriteName(function.getName())) {
      continue;
    }
    SignatureShape shape =
        function.isDeclaration()
            ? shapeForKnownExternal(function, units, abi, prototypes)
            : shapeForInternalFunction(function, units, summaryFacts, abi);
    if (!shape.Params.empty() || !shape.Returns.empty() || shape.VarArg) {
      shapes.emplace(&function, std::move(shape));
    }
  }
  return shapes;
}

const SignatureShape *shapeForCall(const SignatureRewriteState &state,
                                   const llvm::CallBase &call) {
  if (llvm::Function *callee = call.getCalledFunction()) {
    auto shapeIt = state.Shapes.find(callee);
    return shapeIt == state.Shapes.end() ? nullptr : &shapeIt->second;
  }
  auto shapeIt =
      state.IndirectCallShapes.find(const_cast<llvm::CallBase *>(&call));
  return shapeIt == state.IndirectCallShapes.end() ? nullptr : &shapeIt->second;
}

llvm::Value *extractReturnRange(llvm::IRBuilder<> &builder,
                                const SignatureShape &shape, llvm::Value &call,
                                const RegisterRangeKey &range);

// Local canonicalization used between signature rewrite and final residue
// cleanup.  It is intentionally kept at the SummarySSA top level: the per
// function builder caches instruction pointers while rewriting, and running
// LLVM cleanup inside that phase would make those caches unsafe.
void runPostRewriteInstCombine(llvm::Module &module) {
  llvm::LoopAnalysisManager loopAnalysis;
  llvm::FunctionAnalysisManager functionAnalysis;
  llvm::CGSCCAnalysisManager cgsccAnalysis;
  llvm::ModuleAnalysisManager moduleAnalysis;

  llvm::PassBuilder builder;
  builder.registerModuleAnalyses(moduleAnalysis);
  builder.registerCGSCCAnalyses(cgsccAnalysis);
  builder.registerFunctionAnalyses(functionAnalysis);
  builder.registerLoopAnalyses(loopAnalysis);
  builder.crossRegisterProxies(loopAnalysis, functionAnalysis, cgsccAnalysis,
                               moduleAnalysis);

  llvm::FunctionPassManager functionPasses;
  functionPasses.addPass(llvm::InstCombinePass());
  functionPasses.addPass(llvm::SimplifyCFGPass());
  for (llvm::Function &function : module) {
    if (!function.isDeclaration()) {
      functionPasses.run(function, functionAnalysis);
    }
  }
}

class FunctionBuilder {
public:
  FunctionBuilder(
      llvm::Function &function,
      const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
      const std::map<llvm::Function *, FunctionSummaryFacts> &summaryFacts,
      const AbiFacts &abiFacts, const NativeRegisterSummarySSAOptions &options,
      NativeRegisterSummarySSAFunctionSummary &summary,
      SignatureRewriteState &signatureState)
      : Function(function), Units(units), SummaryFacts(summaryFacts),
        Abi(abiFacts), Options(options), Summary(summary),
        SignatureState(signatureState) {}

  void run() {
    Summary.FunctionName = Function.getName().str();
    collectAccesses();
    planRegisterRanges();
    if (Options.EnableRewrite) {
      rewritePartialReads();
      foldDuplicatePartialReadXors();
      rewriteLoads();
      addIndirectCallsiteShapes(SignatureState, Units, Abi);
      collectSignatureCallArgs();
      collectFunctionReturnValues();
      rewritePartialWrites();
      finalizePendingPhis();
      if (Options.EnableResidueRemoval) {
        removeDeadStoresByLiveness();
        // Keep replaced reads/xors/loads physically alive until every range
        // read and call binding is done.  They stay as Replacement keys while
        // the builder is active; deleting them earlier can leave stale keys if
        // LLVM later reuses the same address for a new helper.
        removeDeadPartialReads();
        removeDeadFoldedZeroXors();
        removeDeadReplacedLoads();
        removeDeadEntryReads();
      }
      eraseDeadPhis();
    }
    if (Options.AttachMetadata) {
      attachMetadata();
    }
  }

  void removeDeadStoresAfterSignatureRewrite() {
    Summary.FunctionName = Function.getName().str();
    PostSignatureCleanup = true;
    collectAccesses();
    planRegisterRanges();
    rewritePartialReads();
    foldDuplicatePartialReadXors();
    rewriteLoads();
    rewritePartialWrites();
    // This cleanup run still creates new range helper calls while rewriting
    // loads/writes, so physical deletion stays after liveness cleanup.
    removeDeadStoresByLiveness();
    removeDeadPartialReads();
    removeDeadFoldedZeroXors();
    removeDeadReplacedLoads();
    removeDeadEntryReads();
  }

private:
  llvm::Function &Function;
  const std::map<llvm::GlobalVariable *, RegisterUnit> &Units;
  const std::map<llvm::Function *, FunctionSummaryFacts> &SummaryFacts;
  const AbiFacts &Abi;
  const NativeRegisterSummarySSAOptions &Options;
  NativeRegisterSummarySSAFunctionSummary &Summary;
  SignatureRewriteState &SignatureState;
  std::vector<llvm::LoadInst *> Loads;
  std::vector<llvm::WeakVH> PartialReads;
  std::vector<llvm::WeakVH> ReplacedLoads;
  std::vector<llvm::WeakVH> ReplacedPartialReads;
  std::vector<llvm::WeakVH> FoldedZeroXors;
  std::map<BlockRangeKey, llvm::Value *> EntryRangeValue;
  std::map<BlockRangeKey, llvm::Value *> ExitRangeValue;
  std::map<BlockRangeKey, llvm::PHINode *> PendingRangePhi;
  std::set<BlockRangeKey> ResolvingRangeEntry;
  // Block-local range definitions.  Each key stores the current SSA value for
  // one planned register segment while this pass walks a block forward.
  std::map<BlockRangeKey, RangedSSAValue> CurrentDef;
  // A call with unknown register effect blocks reads from falling back to the
  // block entry value.  PHI completion can then materialize an explicit unknown
  // incoming instead of silently reusing an entry register.
  std::set<BlockRangeKey> UnknownCurrentDef;
  // The last instruction whose effects have been applied to CurrentDef for a
  // block.  This lets multiple reads in the same block continue from the
  // previous transfer point instead of rebuilding the block state each time.
  std::map<llvm::BasicBlock *, llvm::Instruction *> CurrentDefPosition;
  std::map<llvm::Value *, llvm::Value *> Replacement;
  std::set<llvm::PHINode *> DeadPhis;
  std::map<RegisterRangeKey, llvm::Value *> EntryRangeInputs;
  std::map<llvm::Value *, RegisterRangeKey> EntryInputRanges;
  // Canonical entry loads represent real register values at function entry.
  // Range entry reads are built from these loads or from partial_read helpers
  // instead of using poison, because stack and pointer registers must keep a
  // stable symbolic base when stack recovery cannot immediately fold to alloca.
  std::map<llvm::GlobalVariable *, llvm::LoadInst *> EntryRegisterLoads;
  std::map<llvm::GlobalVariable *, std::set<uint64_t>> RangeBoundaries;
  std::map<llvm::GlobalVariable *, std::vector<RegisterRangeKey>> PlannedRanges;
  std::map<CallRangeValueKey, llvm::Value *> CallRangeValues;
  bool PostSignatureCleanup = false;

  static unsigned valueBitWidth(llvm::Value *value) {
    if (value == nullptr) {
      return 0;
    }
    llvm::Type *type = value->getType();
    if (type == nullptr || !type->isSized()) {
      return 0;
    }
    if (auto *integerType = llvm::dyn_cast<llvm::IntegerType>(type)) {
      return integerType->getBitWidth();
    }
    return type->getScalarSizeInBits();
  }

  static llvm::APInt fullMaskFor(llvm::Value *value) {
    unsigned bitWidth = valueBitWidth(value);
    if (bitWidth == 0) {
      return llvm::APInt();
    }
    return llvm::APInt::getAllOnes(bitWidth);
  }

  bool callPreservesRegisterByAbi(const RegisterUnit &unit) const {
    return isSegmentBaseUnit(unit.Name) ||
           Abi.Unaffected.count(unit.Name) != 0;
  }

  static llvm::APInt maskForLowBits(unsigned bitWidth, unsigned lowBits) {
    if (bitWidth == 0 || lowBits == 0) {
      return llvm::APInt(bitWidth, 0);
    }
    unsigned count = std::min(bitWidth, lowBits);
    return llvm::APInt::getLowBitsSet(bitWidth, count);
  }

  static llvm::APInt shiftedMask(const llvm::APInt &mask, unsigned shift,
                                 unsigned width) {
    if (width == 0) {
      return llvm::APInt();
    }
    llvm::APInt wide = PartialDemandState::trimmedMask(mask, width);
    if (wide.isZero()) {
      return wide;
    }
    if (shift >= width) {
      return llvm::APInt(width, 0);
    }
    wide <<= shift;
    return PartialDemandState::trimmedMask(wide, width);
  }

  static llvm::APInt lshrSourceDemand(const llvm::APInt &resultDemand,
                                      unsigned shift, unsigned sourceWidth) {
    if (sourceWidth == 0 || shift >= sourceWidth) {
      return llvm::APInt(sourceWidth, 0);
    }
    return shiftedMask(resultDemand, shift, sourceWidth);
  }

  static llvm::APInt shlSourceDemand(const llvm::APInt &resultDemand,
                                     unsigned shift, unsigned sourceWidth) {
    if (sourceWidth == 0 || shift >= sourceWidth) {
      return llvm::APInt(sourceWidth, 0);
    }
    return PartialDemandState::trimmedMask(resultDemand.lshr(shift),
                                           sourceWidth);
  }

  static llvm::APInt partialWriteMask(unsigned fullWidth, unsigned writeWidth,
                                      uint64_t bitOffset) {
    if (fullWidth == 0 || writeWidth == 0 || bitOffset >= fullWidth ||
        bitOffset + writeWidth > fullWidth) {
      return llvm::APInt(fullWidth, 0);
    }
    return llvm::APInt::getLowBitsSet(fullWidth, writeWidth).shl(bitOffset);
  }

  static bool isDisjointOr(const llvm::Instruction &inst) {
    auto *possiblyDisjoint = llvm::dyn_cast<llvm::PossiblyDisjointInst>(&inst);
    return possiblyDisjoint != nullptr && possiblyDisjoint->isDisjoint();
  }

  llvm::APInt
  demandedBits(llvm::Value *value,
               const std::map<llvm::Value *, llvm::APInt> &demands) const {
    unsigned width = valueBitWidth(value);
    if (width == 0) {
      return llvm::APInt();
    }
    auto it = demands.find(value);
    if (it == demands.end()) {
      return llvm::APInt(width, 0);
    }
    return PartialDemandState::trimmedMask(it->second, width);
  }

  llvm::Constant *undemandedStoreOperandZero(llvm::Type *type) const {
    if (type == nullptr || !type->isIntegerTy()) {
      return nullptr;
    }
    return llvm::ConstantInt::get(type, 0);
  }

  llvm::Value *
  replacePartialRegisterValue(const RegisterUnit &unit, llvm::Value *oldValue,
                              llvm::Value *partialValue, unsigned writeWidth,
                              uint64_t bitOffset, llvm::Instruction *before) {
    if (oldValue == nullptr || partialValue == nullptr || before == nullptr) {
      return nullptr;
    }
    auto *baseType =
        llvm::dyn_cast<llvm::IntegerType>(unit.Global->getValueType());
    if (baseType == nullptr || oldValue->getType() != baseType) {
      return nullptr;
    }
    unsigned fullWidth = baseType->getBitWidth();
    llvm::APInt writeMask = partialWriteMask(fullWidth, writeWidth, bitOffset);
    if (writeMask.isZero()) {
      return nullptr;
    }

    (void)writeMask;
    return insertBitsIntoInteger(oldValue, partialValue, writeWidth, bitOffset,
                                 before, unit.Name + ".partial_write");
  }

  llvm::Value *insertBitsIntoInteger(llvm::Value *base, llvm::Value *value,
                                     unsigned writeWidth, uint64_t bitOffset,
                                     llvm::Instruction *before,
                                     llvm::Twine name) {
    if (base == nullptr || value == nullptr || before == nullptr ||
        writeWidth == 0) {
      return nullptr;
    }
    auto *baseType = llvm::dyn_cast<llvm::IntegerType>(base->getType());
    if (baseType == nullptr || value->getType() == nullptr ||
        !value->getType()->isIntegerTy() ||
        value->getType()->getIntegerBitWidth() != writeWidth ||
        bitOffset + writeWidth > baseType->getBitWidth()) {
      return nullptr;
    }
    if (bitOffset == 0 && writeWidth == baseType->getBitWidth() &&
        value->getType() == baseType) {
      return value;
    }

    llvm::IRBuilder<> builder(before);
    llvm::Function *callee = getOrInsertNativeRegisterValueInsert(
        *Function.getParent(), baseType, value->getType(),
        baseType->getBitWidth(), writeWidth);
    llvm::Value *offset = llvm::ConstantInt::get(
        llvm::Type::getInt64Ty(Function.getContext()), bitOffset);
    return builder.CreateCall(callee, {base, value, offset}, name);
  }

  llvm::Value *extractBitsFromIntegerValue(llvm::Value *value,
                                           unsigned readWidth,
                                           uint64_t bitOffset,
                                           llvm::Instruction *before,
                                           llvm::Twine name) {
    if (value == nullptr || before == nullptr) {
      return nullptr;
    }
    auto *sourceType = llvm::dyn_cast<llvm::IntegerType>(value->getType());
    if (sourceType == nullptr || readWidth == 0 ||
        bitOffset + readWidth > sourceType->getBitWidth()) {
      return nullptr;
    }
    auto *resultType = llvm::IntegerType::get(Function.getContext(), readWidth);
    if (bitOffset == 0 && resultType == sourceType) {
      return value;
    }

    llvm::IRBuilder<> builder(before);
    llvm::Function *callee = getOrInsertNativeRegisterValueExtract(
        *Function.getParent(), sourceType, resultType,
        sourceType->getBitWidth(), readWidth);
    llvm::Value *offset = llvm::ConstantInt::get(
        llvm::Type::getInt64Ty(Function.getContext()), bitOffset);
    return builder.CreateCall(callee, {value, offset}, name);
  }

  llvm::Value *extractPartialRegisterValue(const RegisterUnit &unit,
                                           llvm::Value *fullValue,
                                           unsigned readWidth,
                                           uint64_t bitOffset,
                                           llvm::Instruction *before) {
    if (fullValue == nullptr || before == nullptr) {
      return nullptr;
    }
    auto *baseType =
        llvm::dyn_cast<llvm::IntegerType>(unit.Global->getValueType());
    if (baseType == nullptr || fullValue->getType() != baseType ||
        readWidth == 0 || bitOffset + readWidth > baseType->getBitWidth()) {
      return nullptr;
    }
    return extractBitsFromIntegerValue(fullValue, readWidth, bitOffset, before,
                                       unit.Name + ".partial_read");
  }

  llvm::Value *extractBitsFromInteger(llvm::Value *value, unsigned readWidth,
                                      uint64_t bitOffset,
                                      llvm::Instruction *before,
                                      llvm::Twine name) {
    if (value == nullptr || before == nullptr) {
      return nullptr;
    }
    auto *sourceType = llvm::dyn_cast<llvm::IntegerType>(value->getType());
    if (sourceType == nullptr || readWidth == 0 ||
        bitOffset + readWidth > sourceType->getBitWidth()) {
      return nullptr;
    }
    return extractBitsFromIntegerValue(value, readWidth, bitOffset, before,
                                       name);
  }

  void eraseTriviallyDeadNonPhiTree(llvm::Instruction *root) {
    std::vector<llvm::Instruction *> worklist;
    worklist.push_back(root);
    while (!worklist.empty()) {
      llvm::Instruction *inst = worklist.back();
      worklist.pop_back();
      if (inst == nullptr || inst->getParent() == nullptr ||
          llvm::isa<llvm::PHINode>(inst) ||
          isRecordedFunctionReturnValue(inst) ||
          !llvm::isInstructionTriviallyDead(inst)) {
        continue;
      }

      std::vector<llvm::Instruction *> operands;
      for (llvm::Value *operand : inst->operand_values()) {
        if (auto *operandInst = llvm::dyn_cast<llvm::Instruction>(operand)) {
          operands.push_back(operandInst);
        }
      }
      inst->eraseFromParent();
      for (llvm::Instruction *operand : operands) {
        if (operand->use_empty() && !isRecordedFunctionReturnValue(operand)) {
          worklist.push_back(operand);
        }
      }
    }
  }

  bool rewriteUndemandedRegisterStoreOperands(
      llvm::Value *value, const std::map<llvm::Value *, llvm::APInt> &demands,
      llvm::SmallPtrSetImpl<llvm::Value *> &visiting) {
    auto *inst = llvm::dyn_cast_or_null<llvm::Instruction>(value);
    if (inst == nullptr || !visiting.insert(inst).second) {
      return false;
    }
    if (llvm::isa<llvm::CallBase>(inst) || inst->mayHaveSideEffects()) {
      return false;
    }

    bool changed = false;
    unsigned operandIndex = 0;
    for (llvm::Use &operandUse : inst->operands()) {
      unsigned currentOperandIndex = operandIndex++;
      llvm::Value *operand = operandUse.get();
      if (llvm::isa<llvm::Constant>(operand)) {
        continue;
      }

      llvm::APInt demand = demandedBits(operand, demands);
      if (!demand.isZero()) {
        changed |=
            rewriteUndemandedRegisterStoreOperands(operand, demands, visiting);
        continue;
      }

      // This only rewrites register-store dataflow.  Replacing an undemanded
      // integer operand with zero preserves every bit that has a real observer
      // and lets normal DCE remove stale register loads.
      llvm::Constant *zero = undemandedStoreOperandZero(operand->getType());
      if (zero == nullptr) {
        changed |=
            rewriteUndemandedRegisterStoreOperands(operand, demands, visiting);
        continue;
      }

      attachZeroDemandOperandMetadata(*inst, currentOperandIndex, *operand);
      operandUse.set(zero);
      if (auto *operandInst = llvm::dyn_cast<llvm::Instruction>(operand)) {
        eraseTriviallyDeadNonPhiTree(operandInst);
      }
      changed = true;
    }
    return changed;
  }

  std::map<llvm::Value *, llvm::APInt> computePartialDemands() {
    std::map<llvm::Value *, llvm::APInt> demands;
    std::map<llvm::Value *, llvm::APInt> knownMasks;
    std::vector<llvm::Value *> worklist;
    std::function<llvm::APInt(llvm::Value *)> knownValueMask =
        [&](llvm::Value *value) -> llvm::APInt {
      if (value == nullptr) {
        return llvm::APInt();
      }
      auto cached = knownMasks.find(value);
      if (cached != knownMasks.end()) {
        return cached->second;
      }
      unsigned width = valueBitWidth(value);
      if (width == 0) {
        return llvm::APInt();
      }
      llvm::APInt result(width, 0);
      if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value)) {
        result = PartialDemandState::trimmedMask(constant->getValue(), width);
        knownMasks.emplace(value, result);
        return result;
      }
      auto *inst = llvm::dyn_cast<llvm::Instruction>(value);
      if (inst == nullptr) {
        result = fullMaskFor(value);
        knownMasks.emplace(value, result);
        return result;
      }
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(inst)) {
        if (std::optional<NativeRegisterValueExtractInfo> extract =
                parseNativeRegisterValueExtract(*call)) {
          result = fullMaskFor(value);
          knownMasks.emplace(value, result);
          return result;
        }
        if (std::optional<NativeRegisterValueInsertInfo> insert =
                parseNativeRegisterValueInsert(*call)) {
          llvm::APInt baseMask = knownValueMask(insert->Base);
          llvm::APInt valueMask = knownValueMask(insert->Value);
          llvm::APInt writeMask = partialWriteMask(
              insert->FullWidth, insert->WriteWidth, insert->BitOffset);
          result = (baseMask & ~writeMask) |
                   shiftedMask(valueMask, insert->BitOffset, insert->FullWidth);
          knownMasks.emplace(value, result);
          return result;
        }
      }
      auto computeUnaryMask = [&](unsigned operandIndex) -> llvm::APInt {
        return knownValueMask(inst->getOperand(operandIndex));
      };
      switch (inst->getOpcode()) {
      case llvm::Instruction::And:
        if (auto *lhs =
                llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(0))) {
          result = PartialDemandState::trimmedMask(lhs->getValue(), width);
          break;
        }
        if (auto *rhs =
                llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1))) {
          result = PartialDemandState::trimmedMask(rhs->getValue(), width);
          break;
        }
        result = computeUnaryMask(0) & computeUnaryMask(1);
        break;
      case llvm::Instruction::Or:
      case llvm::Instruction::Xor:
        if (inst->getOpcode() == llvm::Instruction::Xor &&
            inst->getOperand(0) == inst->getOperand(1)) {
          result = llvm::APInt(width, 0);
        } else {
          result = computeUnaryMask(0) | computeUnaryMask(1);
        }
        break;
      case llvm::Instruction::Trunc:
        result = llvm::APInt::getAllOnes(width);
        break;
      case llvm::Instruction::ZExt: {
        unsigned srcWidth = valueBitWidth(inst->getOperand(0));
        result = maskForLowBits(width, srcWidth);
        break;
      }
      case llvm::Instruction::SExt: {
        unsigned srcWidth = valueBitWidth(inst->getOperand(0));
        result = maskForLowBits(width, srcWidth);
        break;
      }
      case llvm::Instruction::Shl:
        if (auto *shift =
                llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1))) {
          unsigned amount = shift->getLimitedValue();
          if (amount < width) {
            result = shiftedMask(computeUnaryMask(0), amount, width);
          }
          break;
        }
        result = fullMaskFor(value);
        break;
      case llvm::Instruction::LShr:
        if (auto *shift =
                llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1))) {
          unsigned amount = shift->getLimitedValue();
          if (amount < width) {
            result = computeUnaryMask(0).lshr(amount).zextOrTrunc(width);
          }
          break;
        }
        result = fullMaskFor(value);
        break;
      case llvm::Instruction::Select:
        result = computeUnaryMask(1) | computeUnaryMask(2);
        break;
      case llvm::Instruction::ExtractValue:
      case llvm::Instruction::InsertValue:
        // Partial demand tracks scalar register bits, not aggregate layout.
        // Conservatively keep the scalar result instead of merging masks from
        // aggregate operands with unrelated or zero bit widths.
        result = fullMaskFor(value);
        break;
      default:
        result = fullMaskFor(value);
        break;
      }
      knownMasks.emplace(value, result);
      return result;
    };
    auto enqueue = [&](llvm::Value *value, llvm::APInt demand) {
      if (value == nullptr || demand.isZero()) {
        return;
      }
      unsigned bitWidth = valueBitWidth(value);
      if (bitWidth == 0) {
        return;
      }
      demand = PartialDemandState::trimmedMask(demand, bitWidth);
      if (demand.isZero()) {
        return;
      }
      auto it = demands.find(value);
      if (it == demands.end()) {
        demands.emplace(value, demand);
        worklist.push_back(value);
        return;
      }
      llvm::APInt merged = it->second | demand;
      if (merged != it->second) {
        it->second = merged;
        worklist.push_back(value);
      }
    };
    auto seedOperand = [&](llvm::Value *value) {
      enqueue(value, fullMaskFor(value));
    };
    auto seedMemoryPointer = [&](llvm::Value *pointer) {
      seedOperand(pointer);
      auto *op = llvm::dyn_cast_or_null<llvm::Operator>(pointer);
      if (op != nullptr && op->getOpcode() == llvm::Instruction::IntToPtr &&
          op->getNumOperands() != 0) {
        seedOperand(op->getOperand(0));
      }
    };

    if (auto returnsIt = SignatureState.FunctionReturns.find(&Function);
        returnsIt != SignatureState.FunctionReturns.end()) {
      for (const auto &[ret, values] : returnsIt->second) {
        (void)ret;
        for (llvm::Value *value : values) {
          if (isRecordedFunctionReturnValue(value)) {
            seedOperand(resolve(value));
          }
        }
      }
    }

    for (llvm::Instruction &inst : llvm::instructions(Function)) {
      if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
        if (llvm::Value *value = ret->getReturnValue()) {
          seedOperand(value);
        }
        continue;
      }
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (parseNativeRegisterValueExtract(*call) ||
            parseNativeRegisterValueInsert(*call)) {
          continue;
        }
        for (llvm::Use &operand : call->args()) {
          seedOperand(operand.get());
        }
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        RegisterAccess access = registerLoad(*load, Units);
        if (access.Unit == nullptr) {
          seedMemoryPointer(load->getPointerOperand());
        }
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit == nullptr) {
          seedMemoryPointer(store->getPointerOperand());
          seedOperand(store->getValueOperand());
        }
        continue;
      }
      if (auto *br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
        if (!br->isUnconditional()) {
          seedOperand(br->getCondition());
        }
        continue;
      }
      if (auto *sw = llvm::dyn_cast<llvm::SwitchInst>(&inst)) {
        seedOperand(sw->getCondition());
        continue;
      }
      if (auto *icmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
        seedOperand(icmp->getOperand(0));
        seedOperand(icmp->getOperand(1));
        continue;
      }
      if (auto *fcmp = llvm::dyn_cast<llvm::FCmpInst>(&inst)) {
        seedOperand(fcmp->getOperand(0));
        seedOperand(fcmp->getOperand(1));
        continue;
      }
    }

    while (!worklist.empty()) {
      llvm::Value *value = worklist.back();
      worklist.pop_back();
      llvm::APInt demand = demands[value];
      auto *inst = llvm::dyn_cast<llvm::Instruction>(value);
      if (inst == nullptr) {
        continue;
      }
      llvm::APInt inputDemand = demand;
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(inst)) {
        if (std::optional<NativeRegisterValueExtractInfo> extract =
                parseNativeRegisterValueExtract(*call)) {
          enqueue(
              extract->FullValue,
              shiftedMask(inputDemand, extract->BitOffset, extract->FullWidth));
          continue;
        }
        if (std::optional<NativeRegisterValueInsertInfo> insert =
                parseNativeRegisterValueInsert(*call)) {
          llvm::APInt writeMask = partialWriteMask(
              insert->FullWidth, insert->WriteWidth, insert->BitOffset);
          enqueue(insert->Base, inputDemand & ~writeMask);
          enqueue(insert->Value,
                  shlSourceDemand(inputDemand & writeMask, insert->BitOffset,
                                  insert->WriteWidth));
          continue;
        }
      }
      switch (inst->getOpcode()) {
      case llvm::Instruction::Trunc:
        enqueue(inst->getOperand(0), inputDemand);
        break;
      case llvm::Instruction::ZExt:
        enqueue(inst->getOperand(0),
                PartialDemandState::trimmedMask(
                    inputDemand, valueBitWidth(inst->getOperand(0))));
        break;
      case llvm::Instruction::SExt:
        if (auto *srcType = llvm::dyn_cast<llvm::IntegerType>(
                inst->getOperand(0)->getType())) {
          unsigned srcWidth = srcType->getBitWidth();
          llvm::APInt sourceDemand =
              PartialDemandState::trimmedMask(inputDemand, srcWidth);
          llvm::APInt highMask =
              inputDemand & ~maskForLowBits(valueBitWidth(inst), srcWidth);
          if (!highMask.isZero() && srcWidth > 0) {
            sourceDemand |= llvm::APInt::getSignMask(srcWidth);
          }
          enqueue(inst->getOperand(0), sourceDemand);
        } else {
          enqueue(inst->getOperand(0), inputDemand);
        }
        break;
      case llvm::Instruction::BitCast:
      case llvm::Instruction::Freeze:
        enqueue(inst->getOperand(0), inputDemand);
        break;
      case llvm::Instruction::PHI:
        for (llvm::Value *incoming : inst->operand_values()) {
          enqueue(incoming, inputDemand);
        }
        break;
      case llvm::Instruction::Select:
        enqueue(inst->getOperand(1), inputDemand);
        enqueue(inst->getOperand(2), inputDemand);
        break;
      case llvm::Instruction::And: {
        llvm::ConstantInt *constOp = nullptr;
        if (auto *lhs =
                llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(0))) {
          constOp = lhs;
          enqueue(inst->getOperand(1), inputDemand & lhs->getValue());
        } else if (auto *rhs =
                       llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1))) {
          constOp = rhs;
          enqueue(inst->getOperand(0), inputDemand & rhs->getValue());
        } else {
          enqueue(inst->getOperand(0), inputDemand);
          enqueue(inst->getOperand(1), inputDemand);
        }
        (void)constOp;
        break;
      }
      case llvm::Instruction::Or:
      case llvm::Instruction::Xor: {
        if (inst->getOpcode() == llvm::Instruction::Xor &&
            inst->getOperand(0) == inst->getOperand(1)) {
          break;
        }
        llvm::APInt lhsMask = knownValueMask(inst->getOperand(0));
        llvm::APInt rhsMask = knownValueMask(inst->getOperand(1));
        bool canSplit = inst->getOpcode() == llvm::Instruction::Or &&
                        isDisjointOr(*inst) &&
                        lhsMask.getBitWidth() == rhsMask.getBitWidth() &&
                        !lhsMask.isZero() && !rhsMask.isZero();
        if (canSplit) {
          enqueue(inst->getOperand(0), inputDemand & lhsMask);
          enqueue(inst->getOperand(1), inputDemand & rhsMask);
        } else {
          enqueue(inst->getOperand(0), inputDemand);
          enqueue(inst->getOperand(1), inputDemand);
        }
        break;
      }
      case llvm::Instruction::Add:
      case llvm::Instruction::Sub:
      case llvm::Instruction::Mul: {
        unsigned srcWidth = valueBitWidth(inst->getOperand(0));
        llvm::APInt lowDemand =
            PartialDemandState::trimmedMask(inputDemand, srcWidth);
        llvm::APInt highDemand =
            inputDemand & ~maskForLowBits(valueBitWidth(inst), srcWidth);
        if (!highDemand.isZero()) {
          enqueue(inst->getOperand(0), fullMaskFor(inst->getOperand(0)));
          enqueue(inst->getOperand(1), fullMaskFor(inst->getOperand(1)));
        } else {
          enqueue(inst->getOperand(0), lowDemand);
          enqueue(inst->getOperand(1), lowDemand);
        }
        break;
      }
      case llvm::Instruction::Shl:
        if (auto *shift =
                llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1))) {
          unsigned amount = shift->getLimitedValue();
          enqueue(inst->getOperand(0),
                  shlSourceDemand(inputDemand, amount,
                                  valueBitWidth(inst->getOperand(0))));
        } else {
          enqueue(inst->getOperand(0), inputDemand);
        }
        break;
      case llvm::Instruction::LShr:
        if (auto *shift =
                llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1))) {
          unsigned amount = shift->getLimitedValue();
          enqueue(inst->getOperand(0),
                  lshrSourceDemand(inputDemand, amount,
                                   valueBitWidth(inst->getOperand(0))));
        } else {
          enqueue(inst->getOperand(0), inputDemand);
        }
        break;
      case llvm::Instruction::AShr:
        enqueue(inst->getOperand(0), inputDemand);
        break;
      case llvm::Instruction::PtrToInt:
      case llvm::Instruction::IntToPtr:
      case llvm::Instruction::AddrSpaceCast:
        enqueue(inst->getOperand(0), inputDemand);
        break;
      case llvm::Instruction::Load:
        seedMemoryPointer(inst->getOperand(0));
        break;
      case llvm::Instruction::InsertValue:
      case llvm::Instruction::ExtractValue:
        for (llvm::Value *operand : inst->operand_values()) {
          enqueue(operand, inputDemand);
        }
        break;
      default:
        for (llvm::Value *operand : inst->operand_values()) {
          enqueue(operand, inputDemand);
        }
        break;
      }
    }
    return demands;
  }

  void rewritePartialWrites() {
    std::map<llvm::Value *, llvm::APInt> demands = computePartialDemands();
    std::vector<llvm::StoreInst *> stores;
    std::vector<llvm::CallBase *> partialWrites;
    for (llvm::Instruction &inst : llvm::instructions(Function)) {
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        stores.push_back(store);
        continue;
      }
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (parseNativeRegisterPartialWrite(*call)) {
          partialWrites.push_back(call);
        }
      }
    }
    for (llvm::StoreInst *store : stores) {
      if (store->getParent() == nullptr) {
        continue;
      }
      RegisterAccess access = registerStore(*store, Units);
      if (access.Unit == nullptr || !access.IsStorageValue) {
        continue;
      }
      ++Summary.PartialDemandCandidates;
      if (!PostSignatureCleanup &&
          (isRecordedCallArgStore(store) ||
           isRecordedCallArgValue(store->getValueOperand()))) {
        ++Summary.PartialDemandRejected;
        continue;
      }
      llvm::SmallPtrSet<llvm::Value *, 16> visiting;
      if (rewriteUndemandedRegisterStoreOperands(store->getValueOperand(),
                                                 demands, visiting)) {
        ++Summary.PartialDemandMatched;
      } else {
        ++Summary.PartialDemandRejected;
      }
    }
    for (llvm::CallBase *call : partialWrites) {
      if (call->getParent() == nullptr) {
        continue;
      }
      std::optional<NativeRegisterPartialWriteInfo> partial =
          parseNativeRegisterPartialWrite(*call);
      if (!partial || Units.count(partial->Global) == 0) {
        continue;
      }
      ++Summary.PartialDemandCandidates;
      llvm::SmallPtrSet<llvm::Value *, 16> visiting;
      if (rewriteUndemandedRegisterStoreOperands(partial->Value, demands,
                                                 visiting)) {
        ++Summary.PartialDemandMatched;
      } else {
        ++Summary.PartialDemandRejected;
      }
    }
  }

  void eraseDeadPartialWriteCall(llvm::CallBase *call) {
    eraseInstructionFromParent(*call);
    ++Summary.DeadStoresRemoved;
  }

  void collectAccesses() {
    for (llvm::BasicBlock &block : Function) {
      for (llvm::Instruction &inst : block) {
        if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
          RegisterAccess access = registerLoad(*load, Units);
          if (access.Unit != nullptr) {
            ++Summary.LoadsSeen;
            if (access.IsStorageValue &&
                load->getMetadata("notdec.register.summary_ssa.entry") ==
                    nullptr) {
              Loads.push_back(load);
            }
          }
          continue;
        }
        if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
          RegisterAccess access = registerStore(*store, Units);
          if (access.Unit != nullptr) {
            ++Summary.StoresSeen;
          }
          continue;
        }
        if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
          std::optional<NativeRegisterPartialReadInfo> partialRead =
              parseNativeRegisterPartialRead(*call);
          if (partialRead && Units.count(partialRead->Global) != 0) {
            ++Summary.LoadsSeen;
            if (call->getMetadata("notdec.register.summary_ssa.range_entry") !=
                nullptr) {
              continue;
            }
            PartialReads.push_back(call);
            continue;
          }
          std::optional<NativeRegisterPartialWriteInfo> partial =
              parseNativeRegisterPartialWrite(*call);
          if (partial && Units.count(partial->Global) != 0) {
            ++Summary.StoresSeen;
          }
        }
      }
    }
  }

  unsigned registerBitWidth(const RegisterUnit &unit) const {
    auto *type = llvm::dyn_cast<llvm::IntegerType>(unit.Global->getValueType());
    return type == nullptr ? 0 : type->getBitWidth();
  }

  void addRangeBoundary(llvm::GlobalVariable *global, uint64_t offset,
                        uint64_t width) {
    auto unitIt = Units.find(global);
    if (unitIt == Units.end() || width == 0) {
      return;
    }
    unsigned fullWidth = registerBitWidth(unitIt->second);
    if (fullWidth == 0 || offset > fullWidth || offset + width > fullWidth) {
      return;
    }
    std::set<uint64_t> &boundaries = RangeBoundaries[global];
    boundaries.insert(0);
    boundaries.insert(fullWidth);
    boundaries.insert(offset);
    boundaries.insert(offset + width);
  }

  const RegisterUnit *unitForName(llvm::StringRef name) const {
    for (const auto &[global, unit] : Units) {
      (void)global;
      if (unit.Name == name) {
        return &unit;
      }
    }
    return nullptr;
  }

  void addRangeBoundaryForSlot(const AbiFacts::RegisterSlot &slot) {
    const RegisterUnit *unit = unitForName(slot.UnitName);
    if (unit == nullptr) {
      return;
    }
    addRangeBoundary(unit->Global, slot.OffsetBits, slot.SizeBits);
  }

  void addRangeBoundariesForMask(llvm::GlobalVariable *global,
                                 const llvm::APInt &mask) {
    if (mask.getBitWidth() == 0 || mask.isZero()) {
      return;
    }
    auto unitIt = Units.find(global);
    if (unitIt == Units.end()) {
      return;
    }
    unsigned fullWidth = registerBitWidth(unitIt->second);
    if (fullWidth == 0) {
      return;
    }
    llvm::APInt trimmed = mask.zextOrTrunc(fullWidth);
    unsigned bit = 0;
    while (bit < fullWidth) {
      while (bit < fullWidth && !trimmed[bit]) {
        ++bit;
      }
      unsigned start = bit;
      while (bit < fullWidth && trimmed[bit]) {
        ++bit;
      }
      if (start < bit) {
        addRangeBoundary(global, start, bit - start);
      }
    }
  }

  void recordRangeEvent(llvm::GlobalVariable *global, uint64_t offset,
                        uint32_t width, RangeEventKind kind) {
    std::vector<RegisterRangeKey> ranges =
        plannedRangesCovering(global, offset, width);
    uint64_t count = ranges.empty() ? 1 : ranges.size();
    switch (kind) {
    case RangeEventKind::Read:
      Summary.RangeReadEvents += count;
      break;
    case RangeEventKind::Write:
      Summary.RangeWriteEvents += count;
      break;
    case RangeEventKind::Clobber:
      Summary.RangeClobberEvents += count;
      break;
    }
  }

  void addAbiRangeBoundaries() {
    for (const AbiFacts::RegisterSlot &slot : Abi.IntegerInputsInOrder) {
      addRangeBoundaryForSlot(slot);
    }
    for (const AbiFacts::RegisterSlot &slot : Abi.FloatInputsInOrder) {
      addRangeBoundaryForSlot(slot);
    }
    for (const AbiFacts::RegisterSlot &slot : Abi.IntegerOutputsInOrder) {
      addRangeBoundaryForSlot(slot);
    }
    for (const AbiFacts::RegisterSlot &slot : Abi.FloatOutputsInOrder) {
      addRangeBoundaryForSlot(slot);
    }
    for (const std::string &name : Abi.KilledByCall) {
      if (const RegisterUnit *unit = unitForName(name)) {
        addRangeBoundary(unit->Global, 0, registerBitWidth(*unit));
      }
    }
    for (const std::string &name : Abi.Unaffected) {
      if (const RegisterUnit *unit = unitForName(name)) {
        addRangeBoundary(unit->Global, 0, registerBitWidth(*unit));
      }
    }
  }

  void addSummaryDemandRangeBoundaries() {
    auto factsIt = SummaryFacts.find(&Function);
    if (factsIt == SummaryFacts.end()) {
      return;
    }
    for (const auto &[global, unit] : Units) {
      auto regIt = factsIt->second.Registers.find(unit.Name);
      if (regIt == factsIt->second.Registers.end()) {
        continue;
      }
      addRangeBoundariesForMask(global, regIt->second.EntryDemandMask);
      addRangeBoundariesForMask(global, regIt->second.ExitDemandMask);
    }
  }

  void planRegisterRanges() {
    RangeBoundaries.clear();
    PlannedRanges.clear();
    for (const auto &[global, unit] : Units) {
      unsigned fullWidth = registerBitWidth(unit);
      if (fullWidth == 0) {
        continue;
      }
      RangeBoundaries[global].insert(0);
      RangeBoundaries[global].insert(fullWidth);
    }

    for (llvm::Instruction &inst : llvm::instructions(Function)) {
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        RegisterAccess access = registerLoad(*load, Units);
        if (access.Unit != nullptr && access.IsStorageValue) {
          addRangeBoundary(access.Unit->Global, 0,
                           registerBitWidth(*access.Unit));
        }
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && access.IsStorageValue) {
          addRangeBoundary(access.Unit->Global, 0,
                           registerBitWidth(*access.Unit));
        }
        continue;
      }
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr) {
        continue;
      }
      if (std::optional<NativeRegisterPartialReadInfo> partial =
              parseNativeRegisterPartialRead(*call)) {
        addRangeBoundary(partial->Global, partial->BitOffset,
                         partial->ReadWidth);
        continue;
      }
      if (std::optional<NativeRegisterPartialWriteInfo> partial =
              parseNativeRegisterPartialWrite(*call)) {
        addRangeBoundary(partial->Global, partial->BitOffset,
                         partial->WriteWidth);
      }
    }

    addAbiRangeBoundaries();
    addSummaryDemandRangeBoundaries();

    for (const auto &[global, boundaries] : RangeBoundaries) {
      if (boundaries.size() < 2) {
        continue;
      }
      std::vector<RegisterRangeKey> &ranges = PlannedRanges[global];
      auto previous = boundaries.begin();
      for (auto current = std::next(previous); current != boundaries.end();
           previous = current, ++current) {
        if (*current > *previous) {
          ranges.push_back(RegisterRangeKey{
              global, *previous, static_cast<uint32_t>(*current - *previous)});
        }
      }
    }

    for (const auto &[global, ranges] : PlannedRanges) {
      if (!ranges.empty()) {
        ++Summary.RangeRegistersPlanned;
        Summary.RangeSegmentsPlanned += ranges.size();
      }
    }

    for (llvm::Instruction &inst : llvm::instructions(Function)) {
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        RegisterAccess access = registerLoad(*load, Units);
        if (access.Unit != nullptr && access.IsStorageValue) {
          recordRangeEvent(access.Unit->Global, 0,
                           registerBitWidth(*access.Unit),
                           RangeEventKind::Read);
        }
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && access.IsStorageValue) {
          recordRangeEvent(access.Unit->Global, 0,
                           registerBitWidth(*access.Unit),
                           RangeEventKind::Write);
        }
        continue;
      }
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr) {
        continue;
      }
      if (std::optional<NativeRegisterPartialReadInfo> partial =
              parseNativeRegisterPartialRead(*call)) {
        recordRangeEvent(partial->Global, partial->BitOffset,
                         partial->ReadWidth, RangeEventKind::Read);
        continue;
      }
      if (std::optional<NativeRegisterPartialWriteInfo> partial =
              parseNativeRegisterPartialWrite(*call)) {
        recordRangeEvent(partial->Global, partial->BitOffset,
                         partial->WriteWidth, RangeEventKind::Write);
        continue;
      }
      if (!isAnalyzableCall(*call)) {
        continue;
      }
      for (const auto &[global, unit] : Units) {
        if (callReadsRegister(*call, unit)) {
          recordRangeEvent(global, 0, registerBitWidth(unit),
                           RangeEventKind::Read);
        }
        CallRegisterEffect effect = callEffect(*call, unit);
        if (effect == CallRegisterEffect::ReturnValue) {
          recordRangeEvent(global, 0, registerBitWidth(unit),
                           RangeEventKind::Write);
        } else if (effect == CallRegisterEffect::Clobber) {
          recordRangeEvent(global, 0, registerBitWidth(unit),
                           RangeEventKind::Clobber);
        }
      }
    }
  }

  std::vector<RegisterRangeKey>
  plannedRangesCovering(llvm::GlobalVariable *global, uint64_t offset,
                        uint32_t width) const {
    std::vector<RegisterRangeKey> result;
    if (width == 0) {
      return result;
    }
    auto rangesIt = PlannedRanges.find(global);
    if (rangesIt == PlannedRanges.end()) {
      return result;
    }

    uint64_t cursor = offset;
    uint64_t end = offset + width;
    for (const RegisterRangeKey &range : rangesIt->second) {
      if (range.BitOffset + range.BitWidth <= cursor) {
        continue;
      }
      if (range.BitOffset > cursor || range.BitOffset >= end) {
        break;
      }
      if (range.BitOffset + range.BitWidth > end) {
        break;
      }
      result.push_back(range);
      cursor = range.BitOffset + range.BitWidth;
      if (cursor == end) {
        return result;
      }
    }
    result.clear();
    return result;
  }

  static bool rangeContains(const RegisterRangeKey &outer,
                            const RegisterRangeKey &inner) {
    return outer.Global == inner.Global && outer.BitOffset <= inner.BitOffset &&
           inner.BitOffset + inner.BitWidth <= outer.BitOffset + outer.BitWidth;
  }

  static bool rangeContainsAccess(const RegisterRangeKey &range,
                                  llvm::GlobalVariable *global, uint64_t offset,
                                  uint32_t width) {
    return range.Global == global && range.BitOffset <= offset &&
           offset + width <= range.BitOffset + range.BitWidth;
  }

  llvm::Value *materializeCoveredAccess(const RangedSSAValue &def,
                                        uint64_t readOffset, uint32_t readWidth,
                                        llvm::Instruction *before,
                                        llvm::Twine name) {
    llvm::Value *value = resolve(def.Value);
    if (value == nullptr || before == nullptr ||
        valueBitWidth(value) != def.CoveredRange.BitWidth ||
        !rangeContainsAccess(def.CoveredRange, def.CoveredRange.Global,
                             readOffset, readWidth)) {
      return nullptr;
    }
    if (def.CoveredRange.BitOffset == readOffset &&
        def.CoveredRange.BitWidth == readWidth) {
      return value;
    }
    return extractBitsFromIntegerValue(value, readWidth,
                                       readOffset - def.CoveredRange.BitOffset,
                                       before, name);
  }

  llvm::Value *tryCurrentCoveredAccess(llvm::GlobalVariable *global,
                                       uint64_t readOffset, uint32_t readWidth,
                                       llvm::Instruction *before,
                                       llvm::Twine name) {
    if (global == nullptr || before == nullptr || readWidth == 0 ||
        before->getParent() == nullptr) {
      return nullptr;
    }
    std::vector<RegisterRangeKey> ranges =
        plannedRangesCovering(global, readOffset, readWidth);
    if (ranges.empty() ||
        !transferRangeBlockUntil(*before->getParent(), before)) {
      return nullptr;
    }

    const RangedSSAValue *first = nullptr;
    llvm::Value *firstValue = nullptr;
    for (const RegisterRangeKey &range : ranges) {
      auto it = CurrentDef.find({before->getParent(), range});
      if (it == CurrentDef.end()) {
        return nullptr;
      }
      llvm::Value *value = resolve(it->second.Value);
      if (value == nullptr ||
          !rangeContainsAccess(it->second.CoveredRange, global, readOffset,
                               readWidth)) {
        return nullptr;
      }
      if (first == nullptr) {
        first = &it->second;
        firstValue = value;
        continue;
      }
      if (firstValue != value ||
          !(first->CoveredRange == it->second.CoveredRange)) {
        return nullptr;
      }
    }
    return first == nullptr ? nullptr
                            : materializeCoveredAccess(*first, readOffset,
                                                       readWidth, before, name);
  }

  llvm::Value *
  tryCommonExtractSourceAccess(llvm::ArrayRef<RegisterRangeKey> ranges,
                               llvm::ArrayRef<llvm::Value *> segments,
                               uint64_t readOffset, uint32_t readWidth,
                               llvm::Instruction *before, llvm::Twine name) {
    if (ranges.empty() || ranges.size() != segments.size() ||
        before == nullptr || readWidth == 0) {
      return nullptr;
    }
    llvm::Value *source = nullptr;
    uint32_t sourceWidth = 0;
    for (auto it : llvm::enumerate(ranges)) {
      llvm::Value *segment = resolve(segments[it.index()]);
      auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(segment);
      if (call == nullptr) {
        return nullptr;
      }
      std::optional<NativeRegisterValueExtractInfo> extract =
          parseNativeRegisterValueExtract(*call);
      if (!extract || extract->BitOffset != it.value().BitOffset ||
          extract->ReadWidth != it.value().BitWidth) {
        return nullptr;
      }
      llvm::Value *candidate = resolve(extract->FullValue);
      if (candidate == nullptr) {
        return nullptr;
      }
      if (source == nullptr) {
        source = candidate;
        sourceWidth = extract->FullWidth;
        continue;
      }
      if (source != candidate || sourceWidth != extract->FullWidth) {
        return nullptr;
      }
    }
    if (source == nullptr || readOffset + readWidth > sourceWidth) {
      return nullptr;
    }
    if (readOffset == 0 && readWidth == sourceWidth) {
      return source;
    }
    return extractBitsFromIntegerValue(source, readWidth, readOffset, before,
                                       name);
  }

  bool valueDominatesUse(llvm::Value *value, llvm::Instruction *use,
                         llvm::DominatorTree &domTree) const {
    value = resolve(value);
    if (value == nullptr || use == nullptr) {
      return false;
    }
    auto *inst = llvm::dyn_cast<llvm::Instruction>(value);
    if (inst == nullptr) {
      return true;
    }
    if (inst->getParent() == nullptr || use->getParent() == nullptr) {
      return false;
    }
    if (inst->getFunction() != use->getFunction()) {
      return false;
    }
    if (inst->getParent() == use->getParent()) {
      return inst->comesBefore(use);
    }
    return domTree.dominates(inst->getParent(), use->getParent());
  }

  llvm::Value *
  assembleRangeReadIfDominating(const std::vector<RegisterRangeKey> &ranges,
                                uint64_t readOffset, uint32_t readWidth,
                                llvm::Instruction *before, llvm::Twine name,
                                llvm::DominatorTree &domTree,
                                bool allowUnknownSegments = false) {
    if (ranges.empty() || before == nullptr || readWidth == 0) {
      return nullptr;
    }
    std::vector<llvm::Value *> segments;
    segments.reserve(ranges.size());
    for (const RegisterRangeKey &range : ranges) {
      llvm::Value *segment = resolve(
          readRangeBefore(*before->getParent(), range, before, domTree));
      if (segment == nullptr || segment->getType() != rangeType(range) ||
          !valueDominatesUse(segment, before, domTree)) {
        if (!allowUnknownSegments ||
            UnknownCurrentDef.count({before->getParent(), range}) != 0) {
          return nullptr;
        }
        segment = unknownRangeBefore(*before, range, ".range_unknown");
        if (segment == nullptr || segment->getType() != rangeType(range) ||
            !valueDominatesUse(segment, before, domTree)) {
          return nullptr;
        }
      }
      segments.push_back(segment);
    }

    if (ranges.size() == 1 && ranges.front().BitOffset == readOffset &&
        ranges.front().BitWidth == readWidth) {
      return segments.front();
    }

    if (llvm::Value *common = tryCommonExtractSourceAccess(
            ranges, segments, readOffset, readWidth, before,
            name + ".common_extract")) {
      return common;
    }

    auto *resultType = llvm::IntegerType::get(Function.getContext(), readWidth);
    llvm::Value *result = llvm::ConstantInt::get(resultType, 0);
    for (auto it : llvm::enumerate(ranges)) {
      const RegisterRangeKey &range = it.value();
      result = insertBitsIntoInteger(
          result, segments[it.index()], range.BitWidth,
          range.BitOffset - readOffset, before, name + ".part_insert");
      if (result == nullptr) {
        return nullptr;
      }
    }
    return result;
  }

  llvm::Value *
  readAccessRangeIfDominating(llvm::GlobalVariable *global, uint64_t readOffset,
                              uint32_t readWidth, llvm::Instruction *before,
                              llvm::Twine name, llvm::DominatorTree &domTree,
                              bool allowUnknownSegments = false) {
    if (llvm::Value *covered = tryCurrentCoveredAccess(
            global, readOffset, readWidth, before, name + ".covered")) {
      covered = resolve(covered);
      return covered != nullptr && valueDominatesUse(covered, before, domTree)
                 ? covered
                 : nullptr;
    }
    std::vector<RegisterRangeKey> ranges =
        plannedRangesCovering(global, readOffset, readWidth);
    return assembleRangeReadIfDominating(ranges, readOffset, readWidth, before,
                                         name, domTree, allowUnknownSegments);
  }

  llvm::Value *readFullRangeValueBefore(llvm::BasicBlock &block,
                                        const RegisterUnit &unit,
                                        llvm::Instruction *before,
                                        llvm::DominatorTree &domTree,
                                        bool allowUnknownSegments = false) {
    (void)block;
    unsigned fullWidth = registerBitWidth(unit);
    llvm::Value *value = readAccessRangeIfDominating(
        unit.Global, 0, fullWidth, before,
        llvm::Twine(unit.Name) + ".full_range", domTree,
        allowUnknownSegments);
    value = resolve(value);
    if (value == nullptr || value->getType() != unit.Global->getValueType()) {
      return nullptr;
    }
    return value;
  }

  bool hasReplacementMetadata(const llvm::Instruction &inst) const {
    return inst.getMetadata("notdec.register.summary_ssa.replaced") != nullptr;
  }

  void forgetReplacementValue(llvm::Value *value) {
    for (auto it = Replacement.begin(); it != Replacement.end();) {
      if (it->first == value || it->second == value) {
        it = Replacement.erase(it);
      } else {
        ++it;
      }
    }
  }

  void eraseInstructionFromParent(llvm::Instruction &inst) {
    forgetReplacementValue(&inst);
    inst.eraseFromParent();
  }

  void rewritePartialReads() {
    llvm::DominatorTree domTree(Function);
    for (llvm::WeakVH &handle : PartialReads) {
      auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(handle);
      if (call == nullptr) {
        continue;
      }
      if (call->getParent() == nullptr) {
        continue;
      }
      std::optional<NativeRegisterPartialReadInfo> partial =
          parseNativeRegisterPartialRead(*call);
      if (!partial) {
        continue;
      }
      auto unitIt = Units.find(partial->Global);
      if (unitIt == Units.end()) {
        continue;
      }
      llvm::Value *value = nullptr;
      value = readAccessRangeIfDominating(
          partial->Global, partial->BitOffset, partial->ReadWidth, call,
          unitIt->second.Name + ".partial_range", domTree);
      if (value == nullptr || value->getType() != call->getType()) {
        continue;
      }
      Replacement[call] = value;
      call->replaceAllUsesWith(value);
      call->setMetadata("notdec.register.summary_ssa.replaced",
                        markerNode("true"));
      ReplacedPartialReads.push_back(call);
      ++Summary.LoadsReplaced;
    }
  }

  void rewriteLoads() {
    for (llvm::LoadInst *load : Loads) {
      RegisterAccess access = registerLoad(*load, Units);
      if (access.Unit == nullptr || !access.IsStorageValue) {
        continue;
      }
      llvm::Value *value =
          readValueBefore(*load->getParent(), *access.Unit, load);
      value = resolve(value);
      if (value == nullptr || value == load ||
          value->getType() != load->getType()) {
        continue;
      }
      Replacement[load] = value;
      load->replaceAllUsesWith(value);
      load->setMetadata("notdec.register.summary_ssa.replaced",
                        markerNode("true"));
      ReplacedLoads.push_back(load);
      ++Summary.LoadsReplaced;
    }
  }

  void removeDeadPartialReads() {
    llvm::SmallPtrSet<llvm::CallBase *, 16> candidates;
    for (llvm::WeakVH &handle : ReplacedPartialReads) {
      if (auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(handle)) {
        candidates.insert(call);
      }
    }
    for (llvm::WeakVH &handle : PartialReads) {
      if (auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(handle)) {
        candidates.insert(call);
      }
    }
    for (llvm::CallBase *call : candidates) {
      if (call->getParent() == nullptr || !call->use_empty()) {
        continue;
      }
      if (isRecordedCallArgValue(call) || isRecordedFunctionReturnValue(call)) {
        continue;
      }
      eraseInstructionFromParent(*call);
      ++Summary.DeadLoadsRemoved;
    }
  }

  void removeDeadFoldedZeroXors() {
    for (llvm::WeakVH &handle : FoldedZeroXors) {
      auto *op = llvm::dyn_cast_or_null<llvm::BinaryOperator>(handle);
      if (op == nullptr) {
        continue;
      }
      if (op->getParent() == nullptr || !op->use_empty()) {
        continue;
      }
      eraseInstructionFromParent(*op);
    }
    FoldedZeroXors.clear();
  }

  void removeDeadEntryReads() {
    std::vector<llvm::Instruction *> deadReads;
    for (llvm::Instruction &inst : llvm::instructions(Function)) {
      if (!inst.use_empty()) {
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        if (load->getMetadata("notdec.register.summary_ssa.entry") != nullptr) {
          deadReads.push_back(load);
        }
        continue;
      }
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr ||
          call->getMetadata("notdec.register.summary_ssa.range_entry") ==
              nullptr ||
          !parseNativeRegisterPartialRead(*call)) {
        continue;
      }
      deadReads.push_back(call);
    }

    for (llvm::Instruction *inst : deadReads) {
      if (inst->getParent() == nullptr || !inst->use_empty()) {
        continue;
      }
      if (isRecordedCallArgValue(inst) || isRecordedFunctionReturnValue(inst)) {
        continue;
      }
      eraseInstructionFromParent(*inst);
      ++Summary.DeadLoadsRemoved;
    }
  }

  void foldDuplicatePartialReadXors() {
    std::vector<llvm::BinaryOperator *> zeroXors;

    for (llvm::Instruction &inst : llvm::instructions(Function)) {
      auto *op = llvm::dyn_cast<llvm::BinaryOperator>(&inst);
      if (op == nullptr || op->getOpcode() != llvm::Instruction::Xor) {
        continue;
      }

      auto *lhs = llvm::dyn_cast<llvm::CallBase>(op->getOperand(0));
      auto *rhs = llvm::dyn_cast<llvm::CallBase>(op->getOperand(1));
      if (lhs == nullptr || rhs == nullptr || lhs == rhs ||
          lhs->getParent() != rhs->getParent() ||
          lhs->getParent() != op->getParent()) {
        continue;
      }

      std::optional<NativeRegisterPartialReadInfo> lhsRead =
          parseNativeRegisterPartialRead(*lhs);
      std::optional<NativeRegisterPartialReadInfo> rhsRead =
          parseNativeRegisterPartialRead(*rhs);
      if (!samePartialReadRange(lhsRead, rhsRead) ||
          hasInterveningWriteToPartialReadRange(*lhs, *rhs, *lhsRead)) {
        continue;
      }

      zeroXors.push_back(op);
    }

    for (llvm::BinaryOperator *op : zeroXors) {
      if (op->getParent() == nullptr) {
        continue;
      }
      auto *zero = llvm::ConstantInt::get(op->getType(), 0);
      Replacement[op] = zero;
      op->replaceAllUsesWith(zero);
      FoldedZeroXors.push_back(op);
      ++Summary.LoadsReplaced;
    }
  }

  static bool samePartialReadRange(
      const std::optional<NativeRegisterPartialReadInfo> &lhs,
      const std::optional<NativeRegisterPartialReadInfo> &rhs) {
    return lhs && rhs && lhs->Global == rhs->Global &&
           lhs->FullWidth == rhs->FullWidth &&
           lhs->ReadWidth == rhs->ReadWidth && lhs->BitOffset == rhs->BitOffset;
  }

  bool hasInterveningWriteToPartialReadRange(
      llvm::CallBase &first, llvm::CallBase &second,
      const NativeRegisterPartialReadInfo &read) const {
    bool afterFirst = false;
    uint64_t readEnd = read.BitOffset + read.ReadWidth;
    for (llvm::Instruction &inst : *first.getParent()) {
      if (&inst == &first) {
        afterFirst = true;
        continue;
      }
      if (&inst == &second) {
        return false;
      }
      if (!afterFirst) {
        continue;
      }

      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && access.Unit->Global == read.Global &&
            access.IsStorageValue) {
          return true;
        }
        continue;
      }

      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr) {
        continue;
      }
      if (parseNativeRegisterPartialRead(*call)) {
        continue;
      }
      if (std::optional<NativeRegisterPartialWriteInfo> write =
              parseNativeRegisterPartialWrite(*call)) {
        uint64_t writeEnd = write->BitOffset + write->WriteWidth;
        if (write->Global == read.Global && read.BitOffset < writeEnd &&
            write->BitOffset < readEnd) {
          return true;
        }
        continue;
      }
      if (isAnalyzableCall(*call)) {
        return true;
      }
    }
    return true;
  }

  void removeDeadReplacedLoads() {
    for (llvm::WeakVH &handle : ReplacedLoads) {
      auto *load = llvm::dyn_cast_or_null<llvm::LoadInst>(handle);
      if (load == nullptr) {
        continue;
      }
      if (load->getParent() == nullptr || !load->use_empty()) {
        continue;
      }
      if (isRecordedCallArgValue(load) || isRecordedFunctionReturnValue(load)) {
        continue;
      }
      eraseInstructionFromParent(*load);
      ++Summary.DeadLoadsRemoved;
    }
  }

  bool isRecordedCallArgStore(llvm::StoreInst *store) const {
    if (SignatureState.StoresToErase.count(store) != 0) {
      return true;
    }
    for (const auto &[call, bindings] : SignatureState.CallArgs) {
      (void)call;
      for (const CallArgStoreBinding &binding : bindings) {
        if (binding.Store == store) {
          return true;
        }
      }
    }
    return false;
  }

  bool isRecordedCallArgValue(llvm::Value *value) const {
    value = resolve(value);
    for (const auto &[call, bindings] : SignatureState.CallArgs) {
      (void)call;
      for (const CallArgStoreBinding &binding : bindings) {
        if (resolve(binding.Value) == value ||
            resolve(binding.RegisterValue) == value) {
          return true;
        }
      }
    }
    return false;
  }

  bool isRecordedFunctionReturnValue(llvm::Value *value) const {
    auto shapeIt = SignatureState.Shapes.find(&Function);
    if (shapeIt == SignatureState.Shapes.end() ||
        shapeIt->second.Returns.empty() ||
        Function.getFunctionType() ==
            functionTypeForShape(Function.getContext(), shapeIt->second)) {
      return false;
    }
    auto returnsIt = SignatureState.FunctionReturns.find(&Function);
    if (returnsIt == SignatureState.FunctionReturns.end()) {
      return false;
    }

    value = resolve(value);
    for (const auto &[ret, values] : returnsIt->second) {
      (void)ret;
      for (llvm::Value *retValue : values) {
        if (resolve(retValue) == value) {
          return true;
        }
      }
    }
    return false;
  }

  void removeDeadStoresByLiveness() {
    std::map<llvm::BasicBlock *, LiveRegisterRanges> liveIn;
    std::map<llvm::BasicBlock *, LiveRegisterRanges> liveOut;
    std::vector<llvm::BasicBlock *> blocks;
    for (llvm::BasicBlock &block : Function) {
      blocks.push_back(&block);
    }

    bool changed = true;
    while (changed) {
      changed = false;
      for (auto blockIt = blocks.rbegin(); blockIt != blocks.rend();
           ++blockIt) {
        llvm::BasicBlock &block = **blockIt;
        LiveRegisterRanges out;
        for (llvm::BasicBlock *succ : llvm::successors(&block)) {
          auto succLive = liveIn.find(succ);
          if (succLive != liveIn.end()) {
            out.insert(succLive->second.begin(), succLive->second.end());
          }
        }
        // After signature rewrite, explicit function returns carry these
        // values.  Register globals should no longer stay live just because a
        // summary return register exists.
        if (!PostSignatureCleanup && llvm::succ_empty(&block)) {
          addExitLiveRegisters(out);
        }

        LiveRegisterRanges in = transferBlockLiveness(block, out);
        changed |= liveOut[&block] != out || liveIn[&block] != in;
        liveOut[&block] = std::move(out);
        liveIn[&block] = std::move(in);
      }
    }

    for (llvm::BasicBlock &block : Function) {
      auto outIt = liveOut.find(&block);
      LiveRegisterRanges live =
          outIt == liveOut.end() ? LiveRegisterRanges{} : outIt->second;
      eraseDeadStoresInBlock(block, live);
    }
  }

  LiveRegisterRanges transferBlockLiveness(llvm::BasicBlock &block,
                                           LiveRegisterRanges live) {
    for (auto it = block.rbegin(); it != block.rend(); ++it) {
      llvm::Instruction &inst = *it;
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        transferStoreLiveness(*store, live);
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        transferLoadLiveness(*load, live);
        continue;
      }
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (transferPartialReadLiveness(*call, live)) {
          continue;
        }
        if (transferPartialWriteLiveness(*call, live)) {
          continue;
        }
        transferCallLiveness(*call, live);
        continue;
      }
    }
    return live;
  }

  void eraseDeadStoresInBlock(llvm::BasicBlock &block,
                              LiveRegisterRanges live) {
    std::vector<llvm::StoreInst *> deadStores;
    std::vector<llvm::CallBase *> deadPartialWrites;
    for (auto it = block.rbegin(); it != block.rend(); ++it) {
      llvm::Instruction &inst = *it;
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && access.IsStorageValue &&
            !hasLiveWriteRange(live, *access.Unit) &&
            !isRecordedCallArgStore(store)) {
          deadStores.push_back(store);
        }
        transferStoreLiveness(*store, live);
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        transferLoadLiveness(*load, live);
        continue;
      }
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (transferPartialReadLiveness(*call, live)) {
          continue;
        }
        if (std::optional<NativeRegisterPartialWriteInfo> partial =
                parseNativeRegisterPartialWrite(*call)) {
          if (Units.count(partial->Global) != 0 &&
              !hasLivePartialRange(live, partial->Global, partial->BitOffset,
                                   partial->WriteWidth)) {
            deadPartialWrites.push_back(call);
          }
          transferPartialWriteLiveness(*call, live);
          continue;
        }
        transferCallLiveness(*call, live);
        continue;
      }
    }
    for (llvm::StoreInst *store : deadStores) {
      eraseInstructionFromParent(*store);
      ++Summary.DeadStoresRemoved;
    }
    for (llvm::CallBase *call : deadPartialWrites) {
      eraseDeadPartialWriteCall(call);
    }
  }

  void transferStoreLiveness(llvm::StoreInst &store,
                             LiveRegisterRanges &live) const {
    RegisterAccess access = registerStore(store, Units);
    if (access.Unit != nullptr && access.IsStorageValue) {
      eraseGlobalRanges(live, access.Unit->Global);
    }
  }

  void transferLoadLiveness(llvm::LoadInst &load,
                            LiveRegisterRanges &live) const {
    RegisterAccess access = registerLoad(load, Units);
    if (access.Unit != nullptr && access.IsStorageValue) {
      // Entry/replaced loads are SummarySSA scaffolding.  Only still-raw
      // register loads should keep earlier register stores live.
      if (load.getMetadata("notdec.register.summary_ssa.entry") != nullptr ||
          hasReplacementMetadata(load)) {
        return;
      }
      insertGlobalRanges(live, access.Unit->Global);
    }
  }

  bool transferPartialReadLiveness(llvm::CallBase &call,
                                   LiveRegisterRanges &live) const {
    std::optional<NativeRegisterPartialReadInfo> partial =
        parseNativeRegisterPartialRead(call);
    if (!partial || Units.count(partial->Global) == 0) {
      return false;
    }
    if (hasReplacementMetadata(call) ||
        call.getMetadata("notdec.register.summary_ssa.range_entry") !=
            nullptr) {
      return true;
    }
    insertPartialRanges(live, partial->Global, partial->BitOffset,
                        partial->ReadWidth);
    return true;
  }

  bool transferPartialWriteLiveness(llvm::CallBase &call,
                                    LiveRegisterRanges &live) const {
    std::optional<NativeRegisterPartialWriteInfo> partial =
        parseNativeRegisterPartialWrite(call);
    if (!partial || Units.count(partial->Global) == 0) {
      return false;
    }
    // The helper defines only the written bit range.  Untouched ranges remain
    // live, but the written range itself no longer needs an earlier definition.
    erasePartialRanges(live, partial->Global, partial->BitOffset,
                       partial->WriteWidth);
    return true;
  }

  void transferCallLiveness(llvm::CallBase &call,
                            LiveRegisterRanges &live) const {
    if (!isAnalyzableCall(call)) {
      return;
    }
    for (const auto &[global, unit] : Units) {
      CallRegisterEffect effect = callEffect(call, unit);
      if (effect == CallRegisterEffect::ReturnValue ||
          effect == CallRegisterEffect::Clobber) {
        eraseRegisterEffectRanges(live, unit);
      }
      if (callReadsRegister(call, unit)) {
        insertGlobalRanges(live, global);
      }
    }
  }

  void addExitLiveRegisters(LiveRegisterRanges &live) const {
    auto functionFacts = SummaryFacts.find(&Function);
    if (functionFacts == SummaryFacts.end()) {
      return;
    }
    for (const auto &[global, unit] : Units) {
      auto regIt = functionFacts->second.Registers.find(unit.Name);
      if (regIt == functionFacts->second.Registers.end()) {
        continue;
      }
      const SummaryRegisterFact &fact = regIt->second;
      if (fact.ExitDemand && fact.MayNonEntry) {
        insertMaskRanges(live, global, fact.ExitDemandMask);
      }
    }
  }

  bool rangesOverlap(const RegisterRangeKey &lhs,
                     const RegisterRangeKey &rhs) const {
    if (lhs.Global != rhs.Global) {
      return false;
    }
    uint64_t lhsEnd = lhs.BitOffset + lhs.BitWidth;
    uint64_t rhsEnd = rhs.BitOffset + rhs.BitWidth;
    return lhs.BitOffset < rhsEnd && rhs.BitOffset < lhsEnd;
  }

  void insertMaskRanges(LiveRegisterRanges &live, llvm::GlobalVariable *global,
                        const llvm::APInt &mask) const {
    auto unitIt = Units.find(global);
    if (unitIt == Units.end()) {
      return;
    }
    unsigned width = registerBitWidth(unitIt->second);
    if (width == 0) {
      return;
    }
    if (mask.getBitWidth() == 0 || mask.isZero()) {
      insertGlobalRanges(live, global);
      return;
    }
    llvm::APInt trimmed = mask.zextOrTrunc(width);
    unsigned bit = 0;
    while (bit < width) {
      while (bit < width && !trimmed[bit]) {
        ++bit;
      }
      unsigned start = bit;
      while (bit < width && trimmed[bit]) {
        ++bit;
      }
      if (start < bit) {
        insertPartialRanges(live, global, start, bit - start);
      }
    }
  }

  void eraseRegisterEffectRanges(LiveRegisterRanges &live,
                                 const RegisterUnit &unit) const {
    if (isFloatAbiOutputUnit(Abi, unit.Name)) {
      if (const AbiFacts::RegisterSlot *slot =
              floatAbiOutputSlotForUnit(Abi, unit.Name)) {
        erasePartialRanges(live, unit.Global, slot->OffsetBits, slot->SizeBits);
        return;
      }
    }
    eraseGlobalRanges(live, unit.Global);
  }

  void insertGlobalRanges(LiveRegisterRanges &live,
                          llvm::GlobalVariable *global) const {
    auto rangesIt = PlannedRanges.find(global);
    if (rangesIt == PlannedRanges.end() || rangesIt->second.empty()) {
      auto unitIt = Units.find(global);
      if (unitIt == Units.end()) {
        return;
      }
      unsigned width = registerBitWidth(unitIt->second);
      if (width != 0) {
        live.insert(RegisterRangeKey{global, 0, width});
      }
      return;
    }
    live.insert(rangesIt->second.begin(), rangesIt->second.end());
  }

  void eraseGlobalRanges(LiveRegisterRanges &live,
                         llvm::GlobalVariable *global) const {
    for (auto it = live.begin(); it != live.end();) {
      if (it->Global == global) {
        it = live.erase(it);
      } else {
        ++it;
      }
    }
  }

  void insertPartialRanges(LiveRegisterRanges &live,
                           llvm::GlobalVariable *global, uint64_t offset,
                           uint32_t width) const {
    std::vector<RegisterRangeKey> ranges =
        plannedRangesCovering(global, offset, width);
    if (ranges.empty()) {
      insertGlobalRanges(live, global);
      return;
    }
    live.insert(ranges.begin(), ranges.end());
  }

  void erasePartialRanges(LiveRegisterRanges &live,
                          llvm::GlobalVariable *global, uint64_t offset,
                          uint32_t width) const {
    std::vector<RegisterRangeKey> ranges =
        plannedRangesCovering(global, offset, width);
    if (ranges.empty()) {
      return;
    }
    for (const RegisterRangeKey &range : ranges) {
      live.erase(range);
    }
  }

  bool hasLiveGlobalRange(const LiveRegisterRanges &live,
                          llvm::GlobalVariable *global) const {
    for (const RegisterRangeKey &range : live) {
      if (range.Global == global) {
        return true;
      }
    }
    return false;
  }

  bool hasLivePartialRange(const LiveRegisterRanges &live,
                           llvm::GlobalVariable *global, uint64_t offset,
                           uint32_t width) const {
    std::vector<RegisterRangeKey> ranges =
        plannedRangesCovering(global, offset, width);
    if (ranges.empty()) {
      return hasLiveGlobalRange(live, global);
    }
    for (const RegisterRangeKey &range : ranges) {
      if (live.count(range) != 0) {
        return true;
      }
    }
    return false;
  }

  bool hasLiveWriteRange(const LiveRegisterRanges &live,
                         const RegisterUnit &unit) const {
    unsigned width = registerBitWidth(unit);
    if (width == 0) {
      return false;
    }
    std::vector<RegisterRangeKey> written =
        plannedRangesCovering(unit.Global, 0, width);
    if (written.empty()) {
      return hasLiveGlobalRange(live, unit.Global);
    }
    for (const RegisterRangeKey &writeRange : written) {
      for (const RegisterRangeKey &liveRange : live) {
        if (rangesOverlap(writeRange, liveRange)) {
          return true;
        }
      }
    }
    return false;
  }

  const RegisterUnit *unitForRange(const RegisterRangeKey &range) const {
    auto unitIt = Units.find(range.Global);
    return unitIt == Units.end() ? nullptr : &unitIt->second;
  }

  llvm::IntegerType *rangeType(const RegisterRangeKey &range) const {
    return llvm::IntegerType::get(Function.getContext(), range.BitWidth);
  }

  llvm::Value *unknownRangeBefore(llvm::Instruction &insertBefore,
                                  const RegisterRangeKey &range,
                                  llvm::Twine suffix) {
    const RegisterUnit *unit = unitForRange(range);
    if (unit == nullptr || range.BitWidth == 0) {
      return nullptr;
    }
    llvm::Value *value =
        unknownValueBefore(insertBefore, rangeType(range), unit->Name + suffix);
    std::string reason = suffix.str();
    if (!reason.empty() && reason.front() == '.') {
      reason.erase(reason.begin());
    }
    if (reason.empty()) {
      reason = "range_unknown";
    }
    std::string detail = "offset=" + std::to_string(range.BitOffset) +
                         ",width=" + std::to_string(range.BitWidth);
    attachUnknownValueMetadata(value, reason, Function.getName(), "",
                               unit->Name, "range_ssa", detail);
    return value;
  }

  llvm::Value *rangeTypedValueOrUnknown(llvm::Value *value,
                                        const RegisterRangeKey &range,
                                        llvm::Instruction *insertBefore,
                                        llvm::Twine suffix) {
    value = resolve(value);
    if (value == nullptr) {
      return nullptr;
    }
    if (value->getType() == rangeType(range)) {
      return value;
    }
    if (insertBefore != nullptr) {
      return unknownRangeBefore(*insertBefore, range, suffix);
    }
    return llvm::UndefValue::get(rangeType(range));
  }

  llvm::Value *extractRangeValue(const RegisterRangeKey &range,
                                 llvm::Value *source, uint64_t sourceBitOffset,
                                 llvm::Instruction *before, llvm::Twine name) {
    if (source == nullptr || before == nullptr || range.BitWidth == 0) {
      return nullptr;
    }
    if (range.BitOffset < sourceBitOffset) {
      return nullptr;
    }
    return extractBitsFromInteger(source, range.BitWidth,
                                  range.BitOffset - sourceBitOffset, before,
                                  name);
  }

  llvm::Instruction *
  rangeWriteInsertPoint(llvm::Instruction &writeInst,
                        llvm::Instruction *readBefore) const {
    if (llvm::Instruction *next = writeInst.getNextNode()) {
      return next;
    }
    return readBefore;
  }

  llvm::Value *writeSegment(llvm::Instruction &writeInst,
                            const RegisterRangeKey &range, llvm::Value *value) {
    value = resolve(value);
    if (value == nullptr || value->getType() != rangeType(range)) {
      return nullptr;
    }
    CurrentDef[{writeInst.getParent(), range}] = RangedSSAValue{value, range};
    UnknownCurrentDef.erase({writeInst.getParent(), range});
    return value;
  }

  bool writeCoveredValue(llvm::Instruction &writeInst,
                         const RegisterRangeKey &coveredRange,
                         llvm::Value *value) {
    value = resolve(value);
    if (value == nullptr || valueBitWidth(value) != coveredRange.BitWidth) {
      return false;
    }
    std::vector<RegisterRangeKey> ranges = plannedRangesCovering(
        coveredRange.Global, coveredRange.BitOffset, coveredRange.BitWidth);
    if (ranges.empty()) {
      return false;
    }
    for (const RegisterRangeKey &range : ranges) {
      CurrentDef[{writeInst.getParent(), range}] =
          RangedSSAValue{value, coveredRange};
      UnknownCurrentDef.erase({writeInst.getParent(), range});
    }
    return true;
  }

  bool writeAccessRange(llvm::Instruction &writeInst,
                        llvm::GlobalVariable *global, uint64_t writeOffset,
                        uint32_t writeWidth, llvm::Value *source,
                        uint64_t sourceBitOffset,
                        llvm::Instruction *insertBefore, llvm::Twine name) {
    if (source == nullptr || insertBefore == nullptr || writeWidth == 0) {
      return false;
    }
    std::vector<RegisterRangeKey> ranges =
        plannedRangesCovering(global, writeOffset, writeWidth);
    if (ranges.empty()) {
      return false;
    }
    if (valueBitWidth(source) == writeWidth && sourceBitOffset == writeOffset) {
      RegisterRangeKey coveredRange{global, writeOffset, writeWidth};
      if (writeCoveredValue(writeInst, coveredRange, source)) {
        return true;
      }
    }
    for (const RegisterRangeKey &range : ranges) {
      llvm::Value *segment =
          extractRangeValue(range, source, sourceBitOffset, insertBefore, name);
      if (writeSegment(writeInst, range, segment) == nullptr) {
        return false;
      }
    }
    return true;
  }

  llvm::Value *currentSegment(llvm::BasicBlock &block,
                              const RegisterRangeKey &range,
                              llvm::Instruction *before) {
    auto cached = CurrentDef.find({&block, range});
    if (cached == CurrentDef.end()) {
      return nullptr;
    }
    const RangedSSAValue &def = cached->second;
    llvm::Value *value = resolve(def.Value);
    if (value == nullptr) {
      return nullptr;
    }
    if (def.CoveredRange == range) {
      return value->getType() == rangeType(range) ? value : nullptr;
    }
    if (!rangeContains(def.CoveredRange, range)) {
      return nullptr;
    }
    return materializeCoveredAccess(def, range.BitOffset, range.BitWidth,
                                    before, "range_segment");
  }

  void clearBlockRangeDefs(llvm::BasicBlock &block,
                           llvm::GlobalVariable *global) {
    for (auto it = CurrentDef.begin(); it != CurrentDef.end();) {
      if (it->first.first == &block && it->first.second.Global == global) {
        it = CurrentDef.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = UnknownCurrentDef.begin(); it != UnknownCurrentDef.end();) {
      if (it->first == &block && it->second.Global == global) {
        it = UnknownCurrentDef.erase(it);
      } else {
        ++it;
      }
    }
  }

  void clearAllBlockRangeDefs(llvm::BasicBlock &block) {
    for (auto it = CurrentDef.begin(); it != CurrentDef.end();) {
      if (it->first.first == &block) {
        it = CurrentDef.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = UnknownCurrentDef.begin(); it != UnknownCurrentDef.end();) {
      if (it->first == &block) {
        it = UnknownCurrentDef.erase(it);
      } else {
        ++it;
      }
    }
    CurrentDefPosition.erase(&block);
  }

  void markBlockRangeUnknown(llvm::BasicBlock &block,
                             const RegisterRangeKey &range) {
    CurrentDef.erase({&block, range});
    UnknownCurrentDef.insert({&block, range});
  }

  bool transferRangeInstruction(llvm::Instruction &inst,
                                llvm::Instruction *readBefore) {
    llvm::BasicBlock &block = *inst.getParent();
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      RegisterAccess access = registerStore(*store, Units);
      if (access.Unit != nullptr && access.IsStorageValue) {
        (void)writeAccessRange(*store, access.Unit->Global, 0,
                               registerBitWidth(*access.Unit),
                               resolve(store->getValueOperand()), 0,
                               rangeWriteInsertPoint(*store, readBefore),
                               access.Unit->Name + ".range_store");
      }
      return true;
    }

    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    if (call == nullptr || parseNativeRegisterPartialRead(*call)) {
      return true;
    }
    if (std::optional<NativeRegisterPartialWriteInfo> partial =
            parseNativeRegisterPartialWrite(*call)) {
      auto unitIt = Units.find(partial->Global);
      if (unitIt != Units.end()) {
        (void)writeAccessRange(*call, partial->Global, partial->BitOffset,
                               partial->WriteWidth, partial->Value,
                               partial->BitOffset,
                               rangeWriteInsertPoint(*call, readBefore),
                               unitIt->second.Name + ".range_partial_write");
      }
      return true;
    }
    if (!isAnalyzableCall(*call)) {
      return true;
    }

    bool countedUnknownCall = false;
    for (const auto &[global, unit] : Units) {
      CallRegisterEffect effect = callEffect(*call, unit);
      if (effect == CallRegisterEffect::Preserve) {
        continue;
      }
      if (effect != CallRegisterEffect::ReturnValue &&
          effect != CallRegisterEffect::Clobber) {
        if (!countedUnknownCall) {
          ++Summary.UnknownCallEffects;
          countedUnknownCall = true;
        }
        std::vector<RegisterRangeKey> ranges =
            plannedRangesCovering(global, 0, registerBitWidth(unit));
        if (ranges.empty()) {
          clearBlockRangeDefs(block, global);
          continue;
        }
        for (const RegisterRangeKey &range : ranges) {
          markBlockRangeUnknown(block, range);
        }
        continue;
      }

      llvm::StringRef kind =
          effect == CallRegisterEffect::ReturnValue ? "return" : "clobber";
      std::vector<RegisterRangeKey> ranges =
          plannedRangesCovering(global, 0, registerBitWidth(unit));
      if (ranges.empty()) {
        clearBlockRangeDefs(block, global);
        continue;
      }
      for (const RegisterRangeKey &range : ranges) {
        llvm::Value *value = callRangeValue(*call, range, kind);
        value = resolve(value);
        if (value != nullptr && value->getType() == rangeType(range)) {
          CurrentDef[{&block, range}] = RangedSSAValue{value, range};
          UnknownCurrentDef.erase({&block, range});
        }
      }
    }
    return true;
  }

  bool transferRangeBlockUntil(llvm::BasicBlock &block,
                               llvm::Instruction *before) {
    if (before == nullptr || before->getParent() != &block) {
      return false;
    }

    llvm::Instruction *position = nullptr;
    if (auto posIt = CurrentDefPosition.find(&block);
        posIt != CurrentDefPosition.end()) {
      position = posIt->second;
    }
    if (position != nullptr && position->getParent() != &block) {
      clearAllBlockRangeDefs(block);
      position = nullptr;
    }
    if (position != nullptr && !position->comesBefore(before)) {
      clearAllBlockRangeDefs(block);
      position = nullptr;
    }
    auto it = position == nullptr ? block.begin()
                                  : std::next(position->getIterator());
    for (; it != block.end() && &*it != before; ++it) {
      (void)transferRangeInstruction(*it, before);
      CurrentDefPosition[&block] = &*it;
    }
    return true;
  }

  llvm::Value *readRangeBefore(llvm::BasicBlock &block,
                               const RegisterRangeKey &range,
                               llvm::Instruction *before,
                               llvm::DominatorTree &domTree) {
    const RegisterUnit *unit = unitForRange(range);
    if (unit == nullptr || before == nullptr || range.BitWidth == 0) {
      return nullptr;
    }

    if (!transferRangeBlockUntil(block, before)) {
      return nullptr;
    }
    if (llvm::Value *value = currentSegment(block, range, before)) {
      return value;
    }
    if (UnknownCurrentDef.count({&block, range}) != 0) {
      return nullptr;
    }
    return readRangeEntry(block, range, domTree);
  }

  llvm::Value *readRangeEntry(llvm::BasicBlock &block,
                              const RegisterRangeKey &range,
                              llvm::DominatorTree &domTree) {
    BlockRangeKey key{&block, range};
    if (auto cached = EntryRangeValue.find(key);
        cached != EntryRangeValue.end()) {
      return resolve(cached->second);
    }
    if (ResolvingRangeEntry.count(key) != 0) {
      return ensureRangePhi(block, range);
    }

    ResolvingRangeEntry.insert(key);
    std::vector<llvm::BasicBlock *> preds(llvm::pred_begin(&block),
                                          llvm::pred_end(&block));
    llvm::Value *value = nullptr;
    if (preds.empty()) {
      value = entryRangeInput(range);
    } else if (preds.size() == 1) {
      value = readRangeExit(*preds.front(), range, domTree);
      if (PendingRangePhi.count(key) != 0) {
        value = completeRangePhi(block, range, domTree);
      }
    } else {
      value = completeRangePhi(block, range, domTree);
    }
    ResolvingRangeEntry.erase(key);
    EntryRangeValue[key] = resolve(value);
    return EntryRangeValue[key];
  }

  llvm::Value *readRangeExit(llvm::BasicBlock &block,
                             const RegisterRangeKey &range,
                             llvm::DominatorTree &domTree) {
    BlockRangeKey key{&block, range};
    if (auto cached = ExitRangeValue.find(key);
        cached != ExitRangeValue.end()) {
      return resolve(cached->second);
    }
    llvm::Instruction *terminator = block.getTerminator();
    llvm::Value *value =
        terminator == nullptr
            ? readRangeEntry(block, range, domTree)
            : readRangeBefore(block, range, terminator, domTree);
    value = resolve(value);
    if (terminator == nullptr ||
        valueDominatesUse(value, terminator, domTree)) {
      ExitRangeValue[key] = value;
    }
    return value;
  }

  llvm::PHINode *ensureRangePhi(llvm::BasicBlock &block,
                                const RegisterRangeKey &range) {
    const RegisterUnit *unit = unitForRange(range);
    if (unit == nullptr) {
      return nullptr;
    }
    BlockRangeKey key{&block, range};
    if (auto existing = PendingRangePhi.find(key);
        existing != PendingRangePhi.end()) {
      return existing->second;
    }
    llvm::IRBuilder<> builder(&block, block.getFirstNonPHIIt());
    llvm::PHINode *phi = builder.CreatePHI(rangeType(range), 0,
                                           unit->Name + ".range_summary_ssa");
    phi->setMetadata("notdec.register.summary_ssa.phi", registerNode(*unit));
    PendingRangePhi.emplace(key, phi);
    EntryRangeValue[key] = phi;
    ++Summary.PhisCreated;
    return phi;
  }

  llvm::Value *completeRangePhi(llvm::BasicBlock &block,
                                const RegisterRangeKey &range,
                                llvm::DominatorTree &domTree) {
    llvm::PHINode *phi = ensureRangePhi(block, range);
    if (phi == nullptr) {
      return nullptr;
    }
    std::map<llvm::BasicBlock *, unsigned> existingIncoming;
    for (unsigned index = 0; index < phi->getNumIncomingValues(); ++index) {
      ++existingIncoming[phi->getIncomingBlock(index)];
    }
    std::map<llvm::BasicBlock *, unsigned> requiredIncoming;
    for (llvm::BasicBlock *pred : llvm::predecessors(&block)) {
      unsigned requiredCount = ++requiredIncoming[pred];
      if (existingIncoming[pred] >= requiredCount) {
        continue;
      }
      llvm::Instruction *terminator = pred->getTerminator();
      llvm::Value *incoming =
          rangeTypedValueOrUnknown(readRangeExit(*pred, range, domTree), range,
                                   terminator, ".range_type_mismatch");
      if (incoming != nullptr &&
          !valueDominatesUse(incoming, terminator, domTree)) {
        incoming = nullptr;
      }
      if (incoming == nullptr) {
        incoming =
            terminator != nullptr
                ? unknownRangeBefore(*terminator, range, ".range_unknown")
                : llvm::UndefValue::get(rangeType(range));
      }
      phi->addIncoming(incoming, pred);
    }
    return simplifyRangePhi(*phi, range);
  }

  llvm::Value *simplifyRangePhi(llvm::PHINode &phi,
                                const RegisterRangeKey &range) {
    if (!isCompletePhi(phi)) {
      return &phi;
    }
    llvm::Value *same = nullptr;
    for (llvm::Value *incoming : phi.incoming_values()) {
      incoming = resolve(incoming);
      if (incoming == &phi) {
        continue;
      }
      if (same == nullptr) {
        same = incoming;
        continue;
      }
      if (same != incoming) {
        return &phi;
      }
    }
    if (same == nullptr) {
      auto insertIt = phi.getParent()->getFirstInsertionPt();
      if (insertIt == phi.getParent()->end()) {
        return &phi;
      }
      same = unknownRangeBefore(*insertIt, range, ".range_phi_unknown");
      if (same == nullptr) {
        return &phi;
      }
    }
    Replacement[&phi] = same;
    phi.replaceAllUsesWith(same);
    DeadPhis.insert(&phi);
    ++Summary.PhisSimplified;
    return same;
  }

  llvm::Value *readValueBefore(llvm::BasicBlock &block,
                               const RegisterUnit &unit,
                               llvm::Instruction *before) {
    llvm::DominatorTree domTree(Function);
    llvm::Value *rangeValue = readFullRangeValueBefore(
        block, unit, before, domTree, /*allowUnknownSegments=*/true);
    rangeValue = resolve(rangeValue);
    if (rangeValue != nullptr &&
        rangeValue->getType() == unit.Global->getValueType()) {
      return rangeValue;
    }
    return nullptr;
  }

  bool isCompletePhi(const llvm::PHINode &phi) const {
    const llvm::BasicBlock *block = phi.getParent();
    return block != nullptr &&
           phi.getNumIncomingValues() == llvm::pred_size(block);
  }

  void finalizePendingPhis() {
    bool changed = true;
    while (changed) {
      changed = false;
      std::vector<std::pair<llvm::BasicBlock *, RegisterRangeKey>> rangeWork;
      for (const auto &[key, phi] : PendingRangePhi) {
        if (DeadPhis.count(phi) == 0 && !isCompletePhi(*phi)) {
          rangeWork.push_back({key.first, key.second});
        }
      }
      for (const auto &[block, range] : rangeWork) {
        llvm::PHINode *phi = PendingRangePhi[{block, range}];
        unsigned before = phi->getNumIncomingValues();
        llvm::DominatorTree domTree(Function);
        (void)completeRangePhi(*block, range, domTree);
        changed |= phi->getNumIncomingValues() != before;
      }
    }
  }

  void eraseDeadPhis() {
    for (llvm::PHINode *phi : DeadPhis) {
      if (phi->use_empty()) {
        phi->eraseFromParent();
      }
    }
  }

  llvm::Value *resolve(llvm::Value *value) const {
    while (value != nullptr) {
      auto it = Replacement.find(value);
      if (it == Replacement.end() || it->second == value) {
        return value;
      }
      value = it->second;
    }
    return nullptr;
  }

  std::pair<llvm::Argument *, const NativeSignatureSlot *>
  entryArgumentSlot(const RegisterUnit &unit) const {
    auto shapeIt = SignatureState.Shapes.find(&Function);
    if (shapeIt == SignatureState.Shapes.end()) {
      return {nullptr, nullptr};
    }
    llvm::Argument *arg = Function.arg_begin();
    for (const NativeSignatureSlot &slot : shapeIt->second.Params) {
      if (arg == Function.arg_end()) {
        return {nullptr, nullptr};
      }
      if (slot.Unit == &unit || slot.Unit->Global == unit.Global) {
        return {arg, &slot};
      }
      ++arg;
    }
    return {nullptr, nullptr};
  }

  llvm::Value *entryArgument(const RegisterUnit &unit) const {
    return entryArgumentSlot(unit).first;
  }

  llvm::Value *entryRangeArgument(const RegisterRangeKey &range,
                                  llvm::Instruction *insertBefore) {
    const RegisterUnit *unit = unitForRange(range);
    if (unit == nullptr || insertBefore == nullptr) {
      return nullptr;
    }
    auto [arg, slot] = entryArgumentSlot(*unit);
    if (arg == nullptr || slot == nullptr || slot->SizeBits == 0 ||
        range.BitOffset < slot->OffsetBits ||
        range.BitOffset + range.BitWidth > slot->OffsetBits + slot->SizeBits) {
      return nullptr;
    }

    if (slot->Kind == NativeSignatureSlotKind::FloatRegister) {
      if (range.BitOffset != slot->OffsetBits ||
          range.BitWidth != slot->SizeBits ||
          !arg->getType()->isFloatingPointTy()) {
        return nullptr;
      }
      llvm::IRBuilder<> builder(insertBefore);
      llvm::Type *bitsType =
          llvm::IntegerType::get(Function.getContext(), range.BitWidth);
      return builder.CreateBitCast(arg, bitsType,
                                   unit->Name + ".range_entry_arg");
    }

    if (!arg->getType()->isIntegerTy()) {
      return nullptr;
    }
    return extractRangeValue(range, arg, slot->OffsetBits, insertBefore,
                             unit->Name + ".range_entry_arg");
  }

  llvm::MDNode *entryRangeNode(const RegisterRangeKey &range) const {
    const RegisterUnit *unit = unitForRange(range);
    llvm::Metadata *fields[] = {
        llvm::MDString::get(
            Function.getContext(),
            "name=" + (unit == nullptr ? std::string("") : unit->Name)),
        llvm::MDString::get(Function.getContext(),
                            "bit_offset=" + std::to_string(range.BitOffset)),
        llvm::MDString::get(Function.getContext(),
                            "bit_width=" + std::to_string(range.BitWidth)),
    };
    return llvm::MDNode::get(Function.getContext(), fields);
  }

  llvm::LoadInst *entryRegisterLoad(const RegisterUnit &unit,
                                    llvm::Instruction &insertBefore) {
    if (auto cached = EntryRegisterLoads.find(unit.Global);
        cached != EntryRegisterLoads.end()) {
      return cached->second;
    }

    llvm::IRBuilder<> builder(&insertBefore);
    llvm::LoadInst *load = builder.CreateLoad(
        unit.Global->getValueType(), unit.Global, unit.Name + ".entry");
    load->setMetadata("notdec.register.access",
                      registerAccessNode(Function.getContext(), unit));
    load->setMetadata("notdec.register.summary_ssa.entry", markerNode("true"));
    EntryRegisterLoads.emplace(unit.Global, load);
    return load;
  }

  llvm::Value *entryPartialRead(const RegisterUnit &unit,
                                const RegisterRangeKey &range,
                                llvm::Instruction &insertBefore) {
    unsigned fullWidth = registerBitWidth(unit);
    if (fullWidth == 0 || range.BitOffset + range.BitWidth > fullWidth) {
      return nullptr;
    }

    llvm::IRBuilder<> builder(&insertBefore);
    llvm::Type *ptrType = unit.Global->getType();
    llvm::Type *resultType = rangeType(range);
    llvm::Function *callee = getOrInsertNativeRegisterPartialRead(
        *Function.getParent(), ptrType, resultType, fullWidth, range.BitWidth);
    llvm::Value *offset = llvm::ConstantInt::get(
        llvm::Type::getInt64Ty(Function.getContext()), range.BitOffset);
    llvm::CallInst *call = builder.CreateCall(callee, {unit.Global, offset},
                                              unit.Name + ".range_entry");
    call->setMetadata("notdec.register.access",
                      registerAccessNode(Function.getContext(), unit));
    call->setMetadata("notdec.register.summary_ssa.range_entry",
                      entryRangeNode(range));
    return call;
  }

  bool shapeParamCoversRange(const RegisterRangeKey &range) const {
    auto shapeIt = SignatureState.Shapes.find(&Function);
    if (shapeIt == SignatureState.Shapes.end()) {
      return false;
    }
    for (const NativeSignatureSlot &slot : shapeIt->second.Params) {
      if (slot.Unit == nullptr || slot.Unit->Global != range.Global) {
        continue;
      }
      if (range.BitOffset >= slot.OffsetBits &&
          range.BitOffset + range.BitWidth <= slot.OffsetBits + slot.SizeBits) {
        return true;
      }
    }
    return false;
  }

  bool rangeMayComeFromEntry(const RegisterRangeKey &range) const {
    if (shapeParamCoversRange(range)) {
      return true;
    }
    const RegisterUnit *unit = unitForRange(range);
    if (unit == nullptr) {
      return false;
    }
    auto fnIt = SummaryFacts.find(&Function);
    if (fnIt == SummaryFacts.end()) {
      return true;
    }
    auto regIt = fnIt->second.Registers.find(unit->Name);
    if (regIt == fnIt->second.Registers.end()) {
      return true;
    }
    return regIt->second.MayEntry;
  }

  llvm::Value *entryRangeInput(const RegisterRangeKey &range) {
    if (auto cached = EntryRangeInputs.find(range);
        cached != EntryRangeInputs.end()) {
      return resolve(cached->second);
    }
    const RegisterUnit *unit = unitForRange(range);
    if (unit == nullptr) {
      return nullptr;
    }
    if (PostSignatureCleanup) {
      llvm::Instruction *insertBefore =
          &*Function.getEntryBlock().getFirstInsertionPt();
      if (llvm::Value *value = entryRangeArgument(range, insertBefore)) {
        EntryRangeInputs.emplace(range, value);
        EntryInputRanges.emplace(value, range);
        return value;
      }
    }
    if (!rangeMayComeFromEntry(range)) {
      return nullptr;
    }
    llvm::Instruction *insertBefore =
        &*Function.getEntryBlock().getFirstInsertionPt();
    if (insertBefore == nullptr) {
      return nullptr;
    }
    llvm::Value *value = nullptr;
    unsigned fullWidth = registerBitWidth(*unit);
    if (range.BitOffset == 0 && range.BitWidth == fullWidth) {
      value = entryRegisterLoad(*unit, *insertBefore);
    } else {
      value = entryPartialRead(*unit, range, *insertBefore);
    }
    if (value == nullptr) {
      return nullptr;
    }
    EntryRangeInputs.emplace(range, value);
    EntryInputRanges.emplace(value, range);
    ++Summary.RangeEntryInputs;
    return value;
  }

  CallRegisterEffect callEffect(const llvm::CallBase &call,
                                const RegisterUnit &unit) const {
    llvm::Function *callee = call.getCalledFunction();
    if (PostSignatureCleanup &&
        SignatureState.RewrittenCalls.count(&call) != 0) {
      if (signatureReturnUsesUnit(const_cast<llvm::CallBase &>(call), unit)) {
        return CallRegisterEffect::ReturnValue;
      }
      if (callPreservesRegisterByAbi(unit)) {
        return CallRegisterEffect::Preserve;
      }
      return CallRegisterEffect::Clobber;
    }
    if (callee != nullptr && !callee->isDeclaration()) {
      auto fnIt = SummaryFacts.find(callee);
      if (fnIt == SummaryFacts.end()) {
        return CallRegisterEffect::Unknown;
      }
      auto regIt = fnIt->second.Registers.find(unit.Name);
      SummaryRegisterFact fact;
      if (regIt != fnIt->second.Registers.end()) {
        fact = regIt->second;
      }
      if (!fact.MayNonEntry) {
        return CallRegisterEffect::Preserve;
      }
      if (fact.ExitDemand) {
        return CallRegisterEffect::ReturnValue;
      }
      if (!fact.MayEntry) {
        return CallRegisterEffect::Clobber;
      }
      return CallRegisterEffect::Unknown;
    }

    if (callPreservesRegisterByAbi(unit)) {
      return CallRegisterEffect::Preserve;
    }
    if (signatureReturnUsesUnit(const_cast<llvm::CallBase &>(call), unit) ||
        (isIntegerAbiOutput(Abi, unit.Name) &&
         !isLikelyNonReturnIntegerAbiOutput(Abi, unit.Name))) {
      return CallRegisterEffect::ReturnValue;
    }
    return CallRegisterEffect::Clobber;
  }

  bool callReadsRegister(const llvm::CallBase &call,
                         const RegisterUnit &unit) const {
    llvm::Function *callee = call.getCalledFunction();
    if (PostSignatureCleanup) {
      return false;
    }
    if (callee != nullptr && !callee->isDeclaration()) {
      auto fnIt = SummaryFacts.find(callee);
      if (fnIt == SummaryFacts.end()) {
        return Abi.Inputs.count(unit.Name) != 0;
      }
      auto regIt = fnIt->second.Registers.find(unit.Name);
      return regIt != fnIt->second.Registers.end() && regIt->second.ReadEntry;
    }
    if (callee == nullptr) {
      return Abi.Inputs.count(unit.Name) != 0;
    }
    const SignatureShape *shape = shapeForCall(SignatureState, call);
    if (shape == nullptr) {
      return false;
    }
    if (const NativeExternalCallShape *callsiteShape =
            externalCallsiteShapeForCall(SignatureState, call)) {
      return llvm::any_of(callsiteShape->Inputs,
                          [&](const NativeRegisterCallInputSlot &input) {
                            return input.UnitName == unit.Name;
                          });
    }
    for (const NativeSignatureSlot &slot : shape->Params) {
      if (slot.Unit != nullptr && slot.Unit->Name == unit.Name) {
        return true;
      }
    }
    return false;
  }

  void collectSignatureCallArgs() {
    std::vector<llvm::CallBase *> calls;
    for (llvm::Instruction &inst : llvm::instructions(Function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr || !isAnalyzableCall(*call)) {
        continue;
      }
      if (shapeForCall(SignatureState, *call) == nullptr) {
        continue;
      }
      calls.push_back(call);
    }

    for (llvm::CallBase *call : calls) {
      if (call->getParent() == nullptr) {
        continue;
      }
      llvm::Function *callee = call->getCalledFunction();
      const SignatureShape *shape = shapeForCall(SignatureState, *call);
      if (shape == nullptr) {
        continue;
      }
      std::vector<CallArgStoreBinding> bindings =
          callArgStoreBindings(*call, *shape);
      if (bindings.empty() && shape->Params.empty() && shape->Returns.empty()) {
        continue;
      }

      SignatureState.CallArgs[call] = bindings;
      if (callee == nullptr ||
          !isUnknownExternalFunction(
              *callee, externalPrototypesForState(SignatureState))) {
        for (const CallArgStoreBinding &binding : bindings) {
          if (binding.Store != nullptr &&
              SignatureState.StoresToErase.insert(binding.Store).second) {
            ++Summary.CallArgStoresMarked;
          }
        }
      }
    }
  }

  const RegisterUnit *unitByName(llvm::StringRef name) const {
    for (const auto &[global, unit] : Units) {
      if (unit.Name == name) {
        return &unit;
      }
    }
    return nullptr;
  }

  std::optional<NativeSignatureSlot>
  signatureSlotForCallInput(const NativeRegisterCallInputSlot &input) const {
    const RegisterUnit *unit = unitByName(input.UnitName);
    if (unit == nullptr) {
      return std::nullopt;
    }
    if (!input.Float) {
      return integerSignatureSlot(*unit);
    }

    llvm::Type *type = floatTypeForSizeBits(Function.getContext(),
                                            input.SizeBits);
    if (type == nullptr) {
      return std::nullopt;
    }
    NativeSignatureSlot slot;
    slot.Kind = NativeSignatureSlotKind::FloatRegister;
    slot.Unit = unit;
    slot.AbiName = input.UnitName;
    slot.MetaType = "float";
    slot.OffsetBits = input.OffsetBits;
    slot.SizeBits = input.SizeBits;
    slot.LlvmType = type;
    return slot;
  }

  std::vector<NativeSignatureSlot> callParamSlots(llvm::CallBase &call,
                                                  const SignatureShape &shape) {
    std::vector<NativeSignatureSlot> slots = shape.Params;
    if (!shape.VarArg) {
      return slots;
    }
    const NativeExternalCallShape *callsiteShape =
        externalCallsiteShapeForCall(SignatureState, call);
    if (callsiteShape == nullptr ||
        callsiteShape->Inputs.size() <= shape.Params.size()) {
      return slots;
    }
    for (unsigned index = shape.Params.size();
         index < callsiteShape->Inputs.size(); ++index) {
      const NativeRegisterCallInputSlot &input = callsiteShape->Inputs[index];
      std::optional<NativeSignatureSlot> slot =
          signatureSlotForCallInput(input);
      if (!slot) {
        break;
      }
      slots.push_back(*slot);
    }
    return slots;
  }

  std::vector<CallArgStoreBinding>
  callArgStoreBindings(llvm::CallBase &call, const SignatureShape &shape) {
    std::vector<CallArgStoreBinding> bindings;
    std::vector<NativeSignatureSlot> slots = callParamSlots(call, shape);
    for (auto [index, slot] : llvm::enumerate(slots)) {
      const RegisterUnit *unit = slot.Unit;
      if (unit == nullptr) {
        SignatureState.Warnings.push_back(
            callSlotWarning(call, slot, "call_arg", "call_arg_slot_missing"));
        break;
      }
      llvm::Value *value = resolve(readSlotValueBefore(call, slot));
      if (value == nullptr) {
        SignatureState.Warnings.push_back(callSlotWarning(
            call, slot, "call_arg", "call_arg_binding_missing"));
        break;
      }
      if (value->getType() != slotType(slot)) {
        SignatureState.Warnings.push_back(callSlotWarning(
            call, slot, "call_arg", "call_arg_type_mismatch"));
        break;
      }
      if (mayDependOnSummaryClobberValue(value)) {
        SignatureState.Warnings.push_back(callSlotWarning(
            call, slot, "call_arg", "call_arg_binding_uses_clobber_value"));
      }
      llvm::StoreInst *store = findNearestStoreBeforeCall(call, *unit);
      bindings.push_back(CallArgStoreBinding{store, unit, value, value,
                                             slotType(slot),
                                             static_cast<unsigned>(index)});
    }
    return bindings;
  }

  llvm::Value *readSlotValueBefore(llvm::CallBase &call,
                                   const NativeSignatureSlot &slot) {
    return readSlotValueBefore(
        static_cast<llvm::Instruction &>(call), slot,
        slot.Unit == nullptr ? llvm::Twine("slot")
                             : llvm::Twine(slot.Unit->Name) + ".arg_range");
  }

  llvm::Value *readSlotValueBefore(llvm::Instruction &before,
                                   const NativeSignatureSlot &slot,
                                   llvm::Twine rangeName,
                                   bool allowUnknownSegments = false) {
    if (slot.Unit == nullptr || before.getParent() == nullptr) {
      return nullptr;
    }
    llvm::Value *rangeValue =
        readSlotRangeBefore(before, slot, rangeName, allowUnknownSegments);
    if (rangeValue != nullptr && rangeValue->getType() == slotType(slot)) {
      return rangeValue;
    }
    return nullptr;
  }

  llvm::Value *readSlotRangeBefore(llvm::Instruction &before,
                                   const NativeSignatureSlot &slot,
                                   llvm::Twine name,
                                   bool allowUnknownSegments = false) {
    if (slot.Unit == nullptr || before.getParent() == nullptr ||
        slot.SizeBits == 0) {
      return nullptr;
    }
    std::vector<RegisterRangeKey> ranges = plannedRangesCovering(
        slot.Unit->Global, slot.OffsetBits, slot.SizeBits);
    llvm::DominatorTree domTree(Function);
    llvm::Value *bits = assembleRangeReadIfDominating(
        ranges, slot.OffsetBits, slot.SizeBits, &before, name, domTree,
        allowUnknownSegments);
    bits = resolve(bits);
    if (bits == nullptr) {
      return nullptr;
    }
    if (!valueDominatesUse(bits, &before, domTree)) {
      return nullptr;
    }

    llvm::Instruction *castBefore = &before;
    if (auto *bitsInst = llvm::dyn_cast<llvm::Instruction>(bits);
        bitsInst != nullptr && bitsInst->getParent() == before.getParent() &&
        bitsInst->comesBefore(&before)) {
      if (llvm::Instruction *next = bitsInst->getNextNode()) {
        castBefore = next;
      }
    }

    llvm::Type *targetType = slotType(slot);
    if (slot.Kind == NativeSignatureSlotKind::IntegerRegister) {
      if (!targetType->isIntegerTy() || !bits->getType()->isIntegerTy()) {
        return nullptr;
      }
      llvm::IRBuilder<> builder(castBefore);
      return builder.CreateZExtOrTrunc(bits, targetType,
                                       slot.Unit->Name + ".arg_range_cast");
    }

    if (!targetType->isFloatingPointTy() || !bits->getType()->isIntegerTy() ||
        bits->getType()->getIntegerBitWidth() !=
            targetType->getScalarSizeInBits()) {
      return nullptr;
    }
    llvm::IRBuilder<> builder(castBefore);
    return builder.CreateBitCast(bits, targetType,
                                 slot.Unit->Name + ".arg_range_float");
  }

  llvm::StoreInst *findNearestStoreBeforeCall(llvm::CallBase &call,
                                              const RegisterUnit &unit) {
    for (auto it = call.getIterator(); it != call.getParent()->begin();) {
      --it;
      llvm::Instruction &inst = *it;
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && access.Unit->Global == unit.Global &&
            access.IsStorageValue) {
          return store;
        }
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        RegisterAccess access = registerLoad(*load, Units);
        if (access.Unit != nullptr && access.Unit->Global == unit.Global &&
            access.IsStorageValue &&
            load->getMetadata("notdec.register.summary_ssa.replaced") ==
                nullptr) {
          return nullptr;
        }
        continue;
      }
      if (auto *otherCall = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (std::optional<NativeRegisterPartialReadInfo> partial =
                parseNativeRegisterPartialRead(*otherCall)) {
          if (partial->Global == unit.Global) {
            return nullptr;
          }
          continue;
        }
        if (std::optional<NativeRegisterPartialWriteInfo> partial =
                parseNativeRegisterPartialWrite(*otherCall)) {
          if (partial->Global == unit.Global) {
            return nullptr;
          }
          continue;
        }
        llvm::Function *callee = otherCall->getCalledFunction();
        if (isAnalyzableCall(*otherCall) &&
            (callee == nullptr || !callee->isIntrinsic())) {
          return nullptr;
        }
        continue;
      }
      if (inst.mayWriteToMemory()) {
        return nullptr;
      }
    }
    return nullptr;
  }

  bool isEntryInputValue(llvm::Value *value) {
    value = resolve(value);
    for (const auto &[entryValue, range] : EntryInputRanges) {
      (void)range;
      if (resolve(entryValue) == value) {
        return true;
      }
    }
    return false;
  }

  llvm::Value *callRangeValue(llvm::CallBase &call,
                              const RegisterRangeKey &range,
                              llvm::StringRef kind) {
    const RegisterUnit *unit = unitForRange(range);
    if (unit == nullptr || range.BitWidth == 0) {
      return nullptr;
    }
    CallRangeValueKey key{&call, range, kind.str()};
    if (auto cached = CallRangeValues.find(key);
        cached != CallRangeValues.end()) {
      return cached->second;
    }

    llvm::Instruction *insertBefore = call.getNextNode();
    if (insertBefore == nullptr) {
      insertBefore = call.getParent()->getTerminator();
    }
    if (insertBefore == nullptr) {
      return llvm::UndefValue::get(rangeType(range));
    }
    if (kind == "return" && !isIntegerAbiOutput(Abi, unit->Name) &&
        !signatureReturnUsesUnit(call, *unit)) {
      llvm::Value *value = unknownValueBefore(
          *insertBefore, rangeType(range), unit->Name + ".range_return_unknown");
      attachUnknownValueMetadata(value, "range_return_not_abi_output",
                                 Function.getName(),
                                 calleeNameForWarning(call), unit->Name,
                                 "return");
      return value;
    }

    llvm::IRBuilder<> builder(insertBefore);
    if (kind == "return" && PostSignatureCleanup &&
        SignatureState.RewrittenCalls.count(&call) != 0) {
      const SignatureShape *shape = shapeForCall(SignatureState, call);
      if (shape != nullptr) {
        llvm::Value *value = extractReturnRange(builder, *shape, call, range);
        if (value != nullptr && value->getType() == rangeType(range)) {
          CallRangeValues.emplace(key, value);
          return value;
        }
      }
      llvm::Value *value = unknownValueAt(builder, rangeType(range),
                                          unit->Name + ".range_return_unknown");
      attachUnknownValueMetadata(value, "range_return_rewrite_missing_value",
                                 Function.getName(),
                                 calleeNameForWarning(call), unit->Name,
                                 "return");
      CallRangeValues.emplace(key, value);
      return value;
    }

    llvm::CallInst *value = builder.CreateCall(
        callRangeValueHelper(range, kind), {}, unit->Name + "." + kind.str());
    value->setMetadata("notdec.register.summary_ssa.call_value",
                       callRangeValueNode(range, kind, &call));
    CallRangeValues.emplace(key, value);
    if (kind == "return") {
      SignatureState.ReturnHelpers[&call];
      SignatureState.RangeReturnHelpers[&call].push_back(
          RangeReturnHelper{range, value});
      ++Summary.CallReturnValues;
    } else {
      ++Summary.CallClobberValues;
    }
    return value;
  }

  bool signatureReturnUsesUnit(llvm::CallBase &call,
                               const RegisterUnit &unit) const {
    llvm::Function *callee = call.getCalledFunction();
    if (callee == nullptr) {
      const SignatureShape *shape = shapeForCall(SignatureState, call);
      if (shape == nullptr) {
        return false;
      }
      for (const NativeSignatureSlot &slot : shape->Returns) {
        if (slot.Unit == &unit || slot.Unit->Name == unit.Name) {
          return true;
        }
      }
      return false;
    }
    auto shapeIt = SignatureState.Shapes.find(callee);
    if (shapeIt == SignatureState.Shapes.end()) {
      return false;
    }
    for (const NativeSignatureSlot &slot : shapeIt->second.Returns) {
      if (slot.Unit == &unit || slot.Unit->Name == unit.Name) {
        return true;
      }
    }
    return false;
  }

  llvm::FunctionCallee callRangeValueHelper(const RegisterRangeKey &range,
                                            llvm::StringRef kind) {
    llvm::Module *module = Function.getParent();
    llvm::Type *valueType = rangeType(range);
    llvm::FunctionType *functionType =
        llvm::FunctionType::get(valueType, {}, false);
    return module->getOrInsertFunction("notdec.register.summary_" + kind.str() +
                                           "." + typeSuffix(*valueType),
                                       functionType);
  }

  llvm::MDNode *registerNode(const RegisterUnit &unit) const {
    llvm::Metadata *fields[] = {
        llvm::MDString::get(Function.getContext(), "name=" + unit.Name),
    };
    return llvm::MDNode::get(Function.getContext(), fields);
  }

  llvm::MDNode *callRangeValueNode(const RegisterRangeKey &range,
                                   llvm::StringRef kind,
                                   llvm::Instruction *call) const {
    const RegisterUnit *unit = unitForRange(range);
    uint64_t index = 0;
    for (const llvm::Instruction &inst : *call->getParent()) {
      if (&inst == call) {
        break;
      }
      ++index;
    }
    llvm::Metadata *fields[] = {
        llvm::MDString::get(
            Function.getContext(),
            "name=" + (unit == nullptr ? std::string("") : unit->Name)),
        llvm::MDString::get(Function.getContext(), "kind=" + kind.str()),
        llvm::MDString::get(Function.getContext(),
                            "bit_offset=" + std::to_string(range.BitOffset)),
        llvm::MDString::get(Function.getContext(),
                            "bit_width=" + std::to_string(range.BitWidth)),
        llvm::MDString::get(Function.getContext(),
                            "call_index=" + std::to_string(index)),
    };
    return llvm::MDNode::get(Function.getContext(), fields);
  }

  void collectFunctionReturnValues() {
    auto shapeIt = SignatureState.Shapes.find(&Function);
    if (shapeIt == SignatureState.Shapes.end() ||
        shapeIt->second.Returns.empty()) {
      return;
    }
    for (llvm::BasicBlock &block : Function) {
      auto *ret =
          llvm::dyn_cast_or_null<llvm::ReturnInst>(block.getTerminator());
      if (ret == nullptr) {
        continue;
      }
      std::vector<llvm::Value *> values;
      values.reserve(shapeIt->second.Returns.size());
      for (const NativeSignatureSlot &slot : shapeIt->second.Returns) {
        const RegisterUnit *unit = slot.Unit;
        llvm::Value *value = resolve(readSlotValueBefore(
            *ret, slot, unit->Name + ".return_range",
            /*allowUnknownSegments=*/true));
        llvm::StringRef reason;
        if (value == nullptr) {
          reason = "return_binding_missing";
        } else if (value->getType() != slotType(slot)) {
          reason = "return_binding_type_mismatch";
        } else if (mayDependOnSummaryClobberValue(value)) {
          reason = "return_binding_uses_clobber_value";
        }
        if (!reason.empty()) {
          SignatureState.Warnings.push_back(
              returnSlotWarning(Function, slot, reason));
          value = unknownValueBefore(*ret, slotType(slot),
                                     unit->Name + ".return_unknown");
          attachUnknownValueMetadata(value, reason, Function.getName(),
                                     "<return>", unit->Name,
                                     "return_binding");
        }
        values.push_back(value);
      }
      SignatureState.FunctionReturns[&Function][ret] = std::move(values);
    }
  }

  llvm::MDNode *markerNode(llvm::StringRef value) const {
    llvm::Metadata *fields[] = {
        llvm::MDString::get(Function.getContext(), value),
    };
    return llvm::MDNode::get(Function.getContext(), fields);
  }

  void attachMetadata() {
    uint64_t phisRemaining = 0;
    for (const auto &[key, phi] : PendingRangePhi) {
      if (DeadPhis.count(phi) == 0 && phi->getParent() != nullptr) {
        ++phisRemaining;
      }
    }
    llvm::Metadata *fields[] = {
        llvm::MDString::get(Function.getContext(),
                            "loads_replaced=" +
                                std::to_string(Summary.LoadsReplaced)),
        llvm::MDString::get(Function.getContext(),
                            "phis_remaining=" + std::to_string(phisRemaining)),
        llvm::MDString::get(Function.getContext(),
                            "range_registers_planned=" +
                                std::to_string(Summary.RangeRegistersPlanned)),
        llvm::MDString::get(Function.getContext(),
                            "range_segments_planned=" +
                                std::to_string(Summary.RangeSegmentsPlanned)),
        llvm::MDString::get(Function.getContext(),
                            "range_read_events=" +
                                std::to_string(Summary.RangeReadEvents)),
        llvm::MDString::get(Function.getContext(),
                            "range_write_events=" +
                                std::to_string(Summary.RangeWriteEvents)),
        llvm::MDString::get(Function.getContext(),
                            "range_clobber_events=" +
                                std::to_string(Summary.RangeClobberEvents)),
    };
    Function.setMetadata("notdec.register.summary_ssa",
                         llvm::MDNode::get(Function.getContext(), fields));
  }
};

void addFunctionSummary(NativeRegisterSummarySSASummary &total,
                        const NativeRegisterSummarySSAFunctionSummary &fn) {
  total.LoadsSeen += fn.LoadsSeen;
  total.StoresSeen += fn.StoresSeen;
  total.LoadsReplaced += fn.LoadsReplaced;
  total.DeadLoadsRemoved += fn.DeadLoadsRemoved;
  total.DeadStoresRemoved += fn.DeadStoresRemoved;
  total.PhisCreated += fn.PhisCreated;
  total.PhisSimplified += fn.PhisSimplified;
  total.RangeEntryInputs += fn.RangeEntryInputs;
  total.CallReturnValues += fn.CallReturnValues;
  total.CallClobberValues += fn.CallClobberValues;
  total.CallArgStoresMarked += fn.CallArgStoresMarked;
  total.CallsRewritten += fn.CallsRewritten;
  total.FunctionsRewritten += fn.FunctionsRewritten;
  total.PreservedCalls += fn.PreservedCalls;
  total.UnknownCallEffects += fn.UnknownCallEffects;
  total.StackFrameAccessesRewritten += fn.StackFrameAccessesRewritten;
  total.StackFramePointerLoadsReplaced += fn.StackFramePointerLoadsReplaced;
  total.StackFrameRegisterLoadsRemoved += fn.StackFrameRegisterLoadsRemoved;
  total.StackFrameRegisterStoresRemoved += fn.StackFrameRegisterStoresRemoved;
  total.StackFrameAllocaLoadsRemoved += fn.StackFrameAllocaLoadsRemoved;
  total.StackFrameAllocaStoresRemoved += fn.StackFrameAllocaStoresRemoved;
  total.StackFrameAllocasRemoved += fn.StackFrameAllocasRemoved;
  total.PartialDemandCandidates += fn.PartialDemandCandidates;
  total.PartialDemandMatched += fn.PartialDemandMatched;
  total.PartialDemandRejected += fn.PartialDemandRejected;
  total.RangeRegistersPlanned += fn.RangeRegistersPlanned;
  total.RangeSegmentsPlanned += fn.RangeSegmentsPlanned;
  total.RangeReadEvents += fn.RangeReadEvents;
  total.RangeWriteEvents += fn.RangeWriteEvents;
  total.RangeClobberEvents += fn.RangeClobberEvents;
}

llvm::AttributeList attributesForNewFunction(llvm::Function &oldFunction,
                                             llvm::FunctionType &newType) {
  std::vector<llvm::AttributeSet> argAttrs(newType.getNumParams());
  return llvm::AttributeList::get(
      oldFunction.getContext(), oldFunction.getAttributes().getFnAttrs(),
      oldFunction.getAttributes().getRetAttrs(), argAttrs);
}

void copyFunctionMetadata(llvm::Function &from, llvm::Function &to) {
  llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> metadata;
  from.getAllMetadata(metadata);
  for (const auto &[kind, node] : metadata) {
    to.addMetadata(kind, *node);
  }
}

llvm::Function *createReplacementFunction(llvm::Function &oldFunction,
                                          llvm::FunctionType &newType) {
  llvm::Function *newFunction = llvm::Function::Create(
      &newType, oldFunction.getLinkage(), oldFunction.getAddressSpace());
  newFunction->copyAttributesFrom(&oldFunction);
  newFunction->setAttributes(attributesForNewFunction(oldFunction, newType));
  copyFunctionMetadata(oldFunction, *newFunction);
  newFunction->setComdat(oldFunction.getComdat());
  newFunction->setCallingConv(oldFunction.getCallingConv());
  oldFunction.getParent()->getFunctionList().insert(oldFunction.getIterator(),
                                                    newFunction);
  newFunction->takeName(&oldFunction);
  return newFunction;
}

llvm::Value *castSlotValueToRegister(llvm::IRBuilder<> &builder,
                                     llvm::Value *value,
                                     const NativeSignatureSlot &slot);

llvm::Value *buildReturnValue(llvm::IRBuilder<> &builder,
                              const SignatureShape &shape,
                              const std::vector<llvm::Value *> &values) {
  auto functionName = [&]() -> llvm::StringRef {
    llvm::BasicBlock *block = builder.GetInsertBlock();
    return block == nullptr || block->getParent() == nullptr
               ? llvm::StringRef("")
               : block->getParent()->getName();
  };
  if (shape.Returns.empty()) {
    return nullptr;
  }
  if (shape.Returns.size() == 1) {
    llvm::Type *retTy = singleReturnType(shape);
    if (values.empty() || values.front()->getType() != retTy) {
      llvm::Value *unknown =
          unknownValueAt(builder, retTy, "notdec.return_unknown");
      attachUnknownValueMetadata(unknown, "function_return_missing_value",
                                 functionName(), "", "", "function_return");
      return unknown;
    }
    return values.front();
  }
  llvm::Type *retTy = returnTypeForShape(builder.getContext(), shape);
  llvm::Value *result = llvm::UndefValue::get(retTy);
  for (unsigned index = 0; index < shape.Returns.size(); ++index) {
    const NativeSignatureSlot &slot = shape.Returns[index];
    llvm::Value *value =
        index < values.size()
            ? values[index]
            : unknownValueAt(builder, slotType(slot),
                             slot.Unit->Name + ".return_unknown");
    if (index >= values.size()) {
      attachUnknownValueMetadata(value, "function_return_missing_value",
                                 functionName(), "", slot.Unit->Name,
                                 "function_return");
    }
    if (value->getType() != slotType(slot)) {
      value = unknownValueAt(builder, slotType(slot),
                             slot.Unit->Name + ".return_unknown");
      attachUnknownValueMetadata(value, "function_return_type_mismatch",
                                 functionName(), "", slot.Unit->Name,
                                 "function_return");
    }
    result = builder.CreateInsertValue(result, value, {index});
  }
  return result;
}

llvm::Value *extractReturnRegister(llvm::IRBuilder<> &builder,
                                   const SignatureShape &shape,
                                   llvm::Value &call,
                                   llvm::StringRef registerName) {
  for (unsigned index = 0; index < shape.Returns.size(); ++index) {
    const NativeSignatureSlot &slot = shape.Returns[index];
    if (slot.Unit->Name != registerName) {
      continue;
    }
    llvm::Value *value = nullptr;
    if (shape.Returns.size() == 1) {
      value = &call;
    } else {
      value = builder.CreateExtractValue(&call, {index},
                                         registerName.str() + ".ret");
    }
    if (value->getType() == slot.Unit->Global->getValueType()) {
      return value;
    }
    return castSlotValueToRegister(builder, value, slot);
  }
  return nullptr;
}

llvm::Module *moduleForBuilder(llvm::IRBuilder<> &builder) {
  llvm::BasicBlock *block = builder.GetInsertBlock();
  return block == nullptr ? nullptr : block->getModule();
}

llvm::Value *createValueRangeExtract(llvm::IRBuilder<> &builder,
                                     llvm::Value *source, unsigned readWidth,
                                     uint64_t bitOffset, llvm::Twine name) {
  if (source == nullptr || !source->getType()->isIntegerTy() ||
      readWidth == 0) {
    return nullptr;
  }
  auto *sourceType = llvm::cast<llvm::IntegerType>(source->getType());
  if (bitOffset + readWidth > sourceType->getBitWidth()) {
    return nullptr;
  }
  auto *resultType = llvm::IntegerType::get(builder.getContext(), readWidth);
  if (bitOffset == 0 && sourceType == resultType) {
    return source;
  }
  llvm::Module *module = moduleForBuilder(builder);
  if (module == nullptr) {
    return nullptr;
  }
  llvm::Function *callee = getOrInsertNativeRegisterValueExtract(
      *module, sourceType, resultType, sourceType->getBitWidth(), readWidth);
  llvm::Value *offset = llvm::ConstantInt::get(
      llvm::Type::getInt64Ty(builder.getContext()), bitOffset);
  return builder.CreateCall(callee, {source, offset}, name);
}

llvm::Value *createValueRangeInsert(llvm::IRBuilder<> &builder,
                                    llvm::Value *base, llvm::Value *value,
                                    unsigned writeWidth, uint64_t bitOffset,
                                    llvm::Twine name) {
  if (base == nullptr || value == nullptr || !base->getType()->isIntegerTy() ||
      !value->getType()->isIntegerTy() || writeWidth == 0 ||
      value->getType()->getIntegerBitWidth() != writeWidth) {
    return nullptr;
  }
  auto *baseType = llvm::cast<llvm::IntegerType>(base->getType());
  if (bitOffset + writeWidth > baseType->getBitWidth()) {
    return nullptr;
  }
  if (bitOffset == 0 && writeWidth == baseType->getBitWidth() &&
      value->getType() == baseType) {
    return value;
  }
  llvm::Module *module = moduleForBuilder(builder);
  if (module == nullptr) {
    return nullptr;
  }
  llvm::Function *callee = getOrInsertNativeRegisterValueInsert(
      *module, baseType, value->getType(), baseType->getBitWidth(), writeWidth);
  llvm::Value *offset = llvm::ConstantInt::get(
      llvm::Type::getInt64Ty(builder.getContext()), bitOffset);
  return builder.CreateCall(callee, {base, value, offset}, name);
}

llvm::Value *extractReturnRange(llvm::IRBuilder<> &builder,
                                const SignatureShape &shape, llvm::Value &call,
                                const RegisterRangeKey &range) {
  for (unsigned index = 0; index < shape.Returns.size(); ++index) {
    const NativeSignatureSlot &slot = shape.Returns[index];
    if (slot.Unit == nullptr || slot.Unit->Global != range.Global) {
      continue;
    }
    uint64_t slotEnd = slot.OffsetBits + slot.SizeBits;
    uint64_t rangeEnd = range.BitOffset + range.BitWidth;
    if (range.BitOffset < slot.OffsetBits || rangeEnd > slotEnd) {
      continue;
    }

    llvm::Value *slotValue = nullptr;
    if (shape.Returns.size() == 1) {
      slotValue = &call;
    } else {
      slotValue = builder.CreateExtractValue(&call, {index},
                                             slot.Unit->Name + ".ret_slot");
    }
    llvm::Value *registerValue =
        castSlotValueToRegister(builder, slotValue, slot);
    if (registerValue == nullptr ||
        registerValue->getType() != slot.Unit->Global->getValueType()) {
      return nullptr;
    }
    auto *registerType =
        llvm::dyn_cast<llvm::IntegerType>(registerValue->getType());
    if (registerType == nullptr ||
        range.BitOffset + range.BitWidth > registerType->getBitWidth()) {
      return nullptr;
    }
    return createValueRangeExtract(builder, registerValue, range.BitWidth,
                                   range.BitOffset,
                                   slot.Unit->Name + ".ret_range");
  }
  return nullptr;
}

llvm::Value *foreignArgumentReplacement(
    llvm::Function &function, llvm::Argument &argument,
    std::map<llvm::Type *, llvm::Value *> &unknownByType) {
  for (llvm::Argument &candidate : function.args()) {
    if (candidate.getName() == argument.getName() &&
        candidate.getType() == argument.getType()) {
      return &candidate;
    }
  }
  auto cached = unknownByType.find(argument.getType());
  if (cached != unknownByType.end()) {
    return cached->second;
  }
  llvm::IRBuilder<> builder(&function.getEntryBlock(),
                            function.getEntryBlock().getFirstNonPHIIt());
  llvm::Value *unknown =
      unknownValueAt(builder, argument.getType(), argument.getName() + ".old");
  attachUnknownValueMetadata(unknown, "foreign_argument_replacement",
                             function.getName(), "",
                             argument.getName(), "localize");
  unknownByType.emplace(argument.getType(), unknown);
  return unknown;
}

void replaceForeignArgumentsInBody(llvm::Function &function) {
  std::map<llvm::Type *, llvm::Value *> unknownByType;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    for (llvm::Use &operand : inst.operands()) {
      auto *argument = llvm::dyn_cast<llvm::Argument>(operand.get());
      if (argument == nullptr || argument->getParent() == &function) {
        continue;
      }
      operand.set(
          foreignArgumentReplacement(function, *argument, unknownByType));
    }
  }
}

llvm::Value *localizeReturnValue(llvm::Function &function,
                                 llvm::ReturnInst &insertBefore,
                                 llvm::Value *value) {
  auto *argument = llvm::dyn_cast<llvm::Argument>(value);
  if (argument != nullptr && argument->getParent() != &function) {
    std::map<llvm::Type *, llvm::Value *> unknownByType;
    return foreignArgumentReplacement(function, *argument, unknownByType);
  }
  auto *instruction = llvm::dyn_cast<llvm::Instruction>(value);
  if (instruction != nullptr && instruction->getFunction() != &function) {
    llvm::Value *unknown =
        unknownValueBefore(insertBefore, value->getType(),
                           value->getName() + ".old");
    attachUnknownValueMetadata(unknown, "foreign_return_value_replacement",
                               function.getName(), "",
                               value->getName(), "localize_return");
    return unknown;
  }
  return value;
}

llvm::Value *localizeCallArgument(llvm::Function &function,
                                  llvm::Instruction &insertBefore,
                                  llvm::Value *value) {
  auto *argument = llvm::dyn_cast<llvm::Argument>(value);
  if (argument != nullptr && argument->getParent() != &function) {
    std::map<llvm::Type *, llvm::Value *> unknownByType;
    return foreignArgumentReplacement(function, *argument, unknownByType);
  }
  auto *instruction = llvm::dyn_cast<llvm::Instruction>(value);
  if (instruction != nullptr && instruction->getFunction() != &function) {
    llvm::Value *unknown =
        unknownValueBefore(insertBefore, value->getType(),
                           value->getName() + ".old");
    attachUnknownValueMetadata(unknown, "foreign_call_argument_replacement",
                               function.getName(), "",
                               value->getName(), "localize_call_arg");
    return unknown;
  }
  return value;
}

llvm::Value *castSlotValueToRegister(llvm::IRBuilder<> &builder,
                                     llvm::Value *value,
                                     const NativeSignatureSlot &slot) {
  if (value == nullptr) {
    return nullptr;
  }
  llvm::Type *registerType = slot.Unit->Global->getValueType();
  if (slot.Kind == NativeSignatureSlotKind::IntegerRegister) {
    if (value->getType() == registerType) {
      return value;
    }
    if (value->getType()->isIntegerTy() && registerType->isIntegerTy()) {
      unsigned srcBits = value->getType()->getIntegerBitWidth();
      unsigned dstBits = registerType->getIntegerBitWidth();
      if (srcBits < dstBits) {
        return builder.CreateZExt(value, registerType,
                                  slot.Unit->Name + ".ret_zext");
      }
      if (srcBits > dstBits) {
        return builder.CreateTrunc(value, registerType,
                                   slot.Unit->Name + ".ret_trunc");
      }
    }
    return nullptr;
  }

  if (!value->getType()->isFloatingPointTy() || !registerType->isIntegerTy()) {
    return nullptr;
  }
  unsigned registerBits = registerType->getIntegerBitWidth();
  if (slot.OffsetBits >= registerBits || slot.SizeBits == 0 ||
      slot.OffsetBits + slot.SizeBits > registerBits) {
    return nullptr;
  }
  llvm::Type *intType =
      llvm::IntegerType::get(builder.getContext(), slot.SizeBits);
  llvm::Value *bits =
      builder.CreateBitCast(value, intType, slot.Unit->Name + ".ret_bits");
  llvm::Value *zero = llvm::ConstantInt::get(registerType, 0);
  return createValueRangeInsert(builder, zero, bits, slot.SizeBits,
                                slot.OffsetBits,
                                slot.Unit->Name + ".ret_insert");
}

llvm::CallInst *rewriteCallInst(llvm::CallBase &oldCall, llvm::Value &callee,
                                llvm::FunctionType &newType,
                                const std::vector<llvm::Value *> &args) {
  llvm::SmallVector<llvm::OperandBundleDef, 2> bundles;
  oldCall.getOperandBundlesAsDefs(bundles);
  llvm::CallInst *newCall = llvm::CallInst::Create(
      &newType, &callee, args, bundles, "", oldCall.getIterator());
  if (auto *oldCallInst = llvm::dyn_cast<llvm::CallInst>(&oldCall)) {
    newCall->setTailCallKind(oldCallInst->getTailCallKind());
  }
  newCall->setCallingConv(oldCall.getCallingConv());
  std::vector<llvm::AttributeSet> argAttrs(args.size());
  newCall->setAttributes(llvm::AttributeList::get(
      oldCall.getContext(), oldCall.getAttributes().getFnAttrs(),
      oldCall.getAttributes().getRetAttrs(), argAttrs));
  newCall->copyMetadata(oldCall);
  return newCall;
}

llvm::Value *replacementForOldCallUses(llvm::IRBuilder<> &builder,
                                       llvm::CallBase &oldCall,
                                       llvm::CallInst &newCall,
                                       const SignatureShape &shape) {
  if (oldCall.getType()->isVoidTy()) {
    return nullptr;
  }
  if (oldCall.getType() == newCall.getType()) {
    return &newCall;
  }
  for (unsigned index = 0; index < shape.Returns.size(); ++index) {
    if (shape.Returns[index].Unit->Global->getValueType() !=
        oldCall.getType()) {
      continue;
    }
    if (shape.Returns.size() == 1) {
      return &newCall;
    }
    return builder.CreateExtractValue(&newCall, {index},
                                      oldCall.getName() + ".ret");
  }
  return nullptr;
}

llvm::Value *integerEntrySlotReplacement(llvm::Instruction &inst,
                                         llvm::LoadInst &entryLoad,
                                         const NativeSignatureSlot &slot,
                                         llvm::Argument &arg,
                                         llvm::IRBuilder<> &builder) {
  if (slot.Kind != NativeSignatureSlotKind::IntegerRegister ||
      !arg.getType()->isIntegerTy()) {
    return nullptr;
  }
  auto *argType = llvm::cast<llvm::IntegerType>(arg.getType());
  auto buildSubrange = [&](uint64_t offset, unsigned width) -> llvm::Value * {
    if (width == 0 || offset < slot.OffsetBits ||
        offset + width > slot.OffsetBits + slot.SizeBits) {
      return nullptr;
    }
    uint64_t argOffset = offset - slot.OffsetBits;
    if (argOffset + width > argType->getBitWidth()) {
      return nullptr;
    }
    return createValueRangeExtract(builder, &arg, width, argOffset,
                                   slot.Unit->Name + ".entry_arg");
  };

  if (inst.getType() == arg.getType() && &inst == &entryLoad &&
      slot.OffsetBits == 0 &&
      slot.SizeBits == arg.getType()->getIntegerBitWidth()) {
    return &arg;
  }
  auto *trunc = llvm::dyn_cast<llvm::TruncInst>(&inst);
  if (trunc != nullptr && trunc->getOperand(0) == &entryLoad) {
    return buildSubrange(0, trunc->getType()->getIntegerBitWidth());
  }
  auto *andOp = llvm::dyn_cast<llvm::BinaryOperator>(&inst);
  if (andOp != nullptr && andOp->getOpcode() == llvm::Instruction::And) {
    llvm::Value *source = nullptr;
    llvm::ConstantInt *mask = nullptr;
    if (andOp->getOperand(0) == &entryLoad) {
      source = andOp->getOperand(0);
      mask = llvm::dyn_cast<llvm::ConstantInt>(andOp->getOperand(1));
    } else if (andOp->getOperand(1) == &entryLoad) {
      source = andOp->getOperand(1);
      mask = llvm::dyn_cast<llvm::ConstantInt>(andOp->getOperand(0));
    }
    if (source != nullptr && mask != nullptr &&
        mask->getValue().isMask(mask->getBitWidth())) {
      unsigned width = mask->getValue().countTrailingOnes();
      llvm::Value *narrow = buildSubrange(0, width);
      if (narrow == nullptr) {
        return nullptr;
      }
      return builder.CreateZExtOrTrunc(narrow, inst.getType(),
                                       slot.Unit->Name + ".entry_arg_wide");
    }
    if (source != nullptr && mask != nullptr &&
        mask->getValue().isShiftedMask()) {
      unsigned offset = mask->getValue().countr_zero();
      unsigned width = mask->getValue().getActiveBits() - offset;
      llvm::Value *narrow = buildSubrange(offset, width);
      if (narrow == nullptr) {
        return nullptr;
      }
      auto *instType = llvm::dyn_cast<llvm::IntegerType>(inst.getType());
      if (instType == nullptr) {
        return nullptr;
      }
      llvm::Value *zero = llvm::ConstantInt::get(instType, 0);
      return createValueRangeInsert(builder, zero, narrow, width, offset,
                                    slot.Unit->Name + ".entry_arg_shifted");
    }
  }
  auto *shift = llvm::dyn_cast<llvm::LShrOperator>(&inst);
  if (shift != nullptr && shift->getOperand(0) == &entryLoad) {
    auto *amount = llvm::dyn_cast<llvm::ConstantInt>(shift->getOperand(1));
    if (amount != nullptr) {
      unsigned width = shift->getType()->getIntegerBitWidth();
      llvm::Value *narrow = buildSubrange(amount->getZExtValue(), width);
      if (narrow == nullptr) {
        return nullptr;
      }
      return builder.CreateZExtOrTrunc(narrow, shift->getType(),
                                       slot.Unit->Name + ".entry_arg_wide");
    }
  }
  if (trunc != nullptr) {
    auto *shiftOp = llvm::dyn_cast<llvm::LShrOperator>(trunc->getOperand(0));
    if (shiftOp != nullptr && shiftOp->getOperand(0) == &entryLoad) {
      auto *amount = llvm::dyn_cast<llvm::ConstantInt>(shiftOp->getOperand(1));
      if (amount != nullptr) {
        return buildSubrange(amount->getZExtValue(),
                             trunc->getType()->getIntegerBitWidth());
      }
    }
  }
  if (andOp != nullptr && andOp->getOpcode() == llvm::Instruction::And) {
    auto *shiftOp = llvm::dyn_cast<llvm::LShrOperator>(andOp->getOperand(0));
    auto *mask = llvm::dyn_cast<llvm::ConstantInt>(andOp->getOperand(1));
    if (shiftOp == nullptr || mask == nullptr) {
      shiftOp = llvm::dyn_cast<llvm::LShrOperator>(andOp->getOperand(1));
      mask = llvm::dyn_cast<llvm::ConstantInt>(andOp->getOperand(0));
    }
    if (shiftOp != nullptr && shiftOp->getOperand(0) == &entryLoad &&
        mask != nullptr && mask->getValue().isMask(mask->getBitWidth())) {
      auto *amount = llvm::dyn_cast<llvm::ConstantInt>(shiftOp->getOperand(1));
      if (amount == nullptr) {
        return nullptr;
      }
      unsigned width = mask->getValue().countTrailingOnes();
      llvm::Value *narrow = buildSubrange(amount->getZExtValue(), width);
      if (narrow == nullptr) {
        return nullptr;
      }
      return builder.CreateZExtOrTrunc(narrow, inst.getType(),
                                       slot.Unit->Name + ".entry_arg_wide");
    }
  }
  return nullptr;
}

llvm::Value *rangeEntrySlotReplacement(llvm::Instruction &inst,
                                       const NativeSignatureSlot &slot,
                                       llvm::Argument &arg,
                                       llvm::IRBuilder<> &builder) {
  llvm::MDNode *metadata =
      inst.getMetadata("notdec.register.summary_ssa.range_entry");
  if (metadata == nullptr || slot.Unit == nullptr) {
    return nullptr;
  }
  std::optional<std::string> name = mdField(metadata, "name");
  std::optional<std::string> offsetText = mdField(metadata, "bit_offset");
  std::optional<std::string> widthText = mdField(metadata, "bit_width");
  if (!name || *name != slot.Unit->Name || !offsetText || !widthText) {
    return nullptr;
  }

  uint64_t offset = 0;
  uint64_t width = 0;
  if (llvm::StringRef(*offsetText).getAsInteger(10, offset) ||
      llvm::StringRef(*widthText).getAsInteger(10, width) || width == 0 ||
      offset < slot.OffsetBits ||
      offset + width > slot.OffsetBits + slot.SizeBits) {
    return nullptr;
  }

  if (slot.Kind == NativeSignatureSlotKind::FloatRegister) {
    if (offset != slot.OffsetBits || width != slot.SizeBits ||
        !arg.getType()->isFloatingPointTy() ||
        width != arg.getType()->getScalarSizeInBits()) {
      return nullptr;
    }
    auto *bitsType = llvm::IntegerType::get(arg.getContext(), width);
    return builder.CreateBitCast(&arg, bitsType,
                                 slot.Unit->Name + ".range_entry_arg");
  }

  if (!arg.getType()->isIntegerTy()) {
    return nullptr;
  }
  auto *argType = llvm::cast<llvm::IntegerType>(arg.getType());
  uint64_t argOffset = offset - slot.OffsetBits;
  if (argOffset + width > argType->getBitWidth()) {
    return nullptr;
  }
  return createValueRangeExtract(builder, &arg, width, argOffset,
                                 slot.Unit->Name + ".range_entry_arg");
}

void rewriteInternalFunctionBody(llvm::Function &oldFunction,
                                 llvm::Function &newFunction,
                                 const SignatureShape &shape,
                                 SignatureRewriteState &state) {
  newFunction.splice(newFunction.end(), &oldFunction);
  unsigned index = 0;
  for (llvm::Argument &arg : newFunction.args()) {
    if (index >= shape.Params.size()) {
      break;
    }
    const NativeSignatureSlot &slot = shape.Params[index];
    arg.setName(slot.Unit->Name + ".arg");
    std::vector<llvm::LoadInst *> entryLoads;
    if (slot.Unit != nullptr && slot.Unit->Global != nullptr) {
      for (llvm::Instruction &inst : llvm::instructions(newFunction)) {
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
        if (load != nullptr &&
            load->getMetadata("notdec.register.summary_ssa.entry") != nullptr &&
            load->getPointerOperand()->stripPointerCasts() ==
                slot.Unit->Global) {
          entryLoads.push_back(load);
        }
      }
    }
    for (llvm::BasicBlock &block : newFunction) {
      for (auto it = block.begin(); it != block.end();) {
        llvm::Instruction &inst = *it++;
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
        if (load != nullptr &&
            load->getMetadata("notdec.register.summary_ssa.entry") != nullptr) {
          auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
              load->getPointerOperand()->stripPointerCasts());
          if (global == slot.Unit->Global && load->getType() == arg.getType()) {
            state.ValueMap[load] = &arg;
            load->replaceAllUsesWith(&arg);
          }
          continue;
        }

        if (slot.Unit == nullptr || slot.Unit->Global == nullptr) {
          continue;
        }
        if (inst.getMetadata("notdec.register.summary_ssa.range_entry") !=
            nullptr) {
          llvm::IRBuilder<> builder(&inst);
          llvm::Value *replacement =
              rangeEntrySlotReplacement(inst, slot, arg, builder);
          if (replacement != nullptr &&
              replacement->getType() == inst.getType()) {
            state.ValueMap[&inst] = replacement;
            inst.replaceAllUsesWith(replacement);
          }
          continue;
        }
        for (llvm::LoadInst *entryLoad : entryLoads) {
          llvm::IRBuilder<> builder(&inst);
          llvm::Value *replacement =
              integerEntrySlotReplacement(inst, *entryLoad, slot, arg, builder);
          if (replacement != nullptr &&
              replacement->getType() == inst.getType()) {
            state.ValueMap[&inst] = replacement;
            inst.replaceAllUsesWith(replacement);
            break;
          }
        }
      }
    }
    ++index;
  }
  replaceForeignArgumentsInBody(newFunction);

  auto returnsIt = state.FunctionReturns.find(&oldFunction);
  for (llvm::BasicBlock &block : newFunction) {
    auto *oldRet =
        llvm::dyn_cast_or_null<llvm::ReturnInst>(block.getTerminator());
    if (oldRet == nullptr) {
      continue;
    }
    std::vector<llvm::Value *> values;
    if (returnsIt != state.FunctionReturns.end()) {
      auto valueIt = returnsIt->second.find(oldRet);
      if (valueIt != returnsIt->second.end()) {
        values = valueIt->second;
      }
    }
    llvm::IRBuilder<> builder(oldRet);
    for (llvm::Value *&value : values) {
      while (state.ValueMap.count(value) != 0 &&
             state.ValueMap[value] != value) {
        value = state.ValueMap[value];
      }
      value = localizeReturnValue(newFunction, *oldRet, value);
    }
    if (shape.Returns.empty()) {
      builder.CreateRetVoid();
    } else {
      builder.CreateRet(buildReturnValue(builder, shape, values));
    }
    oldRet->eraseFromParent();
  }
}

unsigned integerBitWidthOf(llvm::Value *value) {
  if (value == nullptr || value->getType() == nullptr ||
      !value->getType()->isIntegerTy()) {
    return 0;
  }
  return value->getType()->getIntegerBitWidth();
}

llvm::Value *resolveValueMap(llvm::Value *value,
                             std::map<llvm::Value *, llvm::Value *> &valueMap) {
  while (value != nullptr && valueMap.count(value) != 0 &&
         valueMap[value] != value) {
    value = valueMap[value];
  }
  return value;
}

bool valueMayDependOnUnknownPlaceholder(
    llvm::Value *value, llvm::SmallPtrSetImpl<llvm::Value *> &seen) {
  if (value == nullptr) {
    return false;
  }
  if (llvm::isa<llvm::PoisonValue>(value) || llvm::isa<llvm::UndefValue>(value)) {
    return true;
  }
  if (llvm::isa<llvm::Constant>(value)) {
    return false;
  }
  if (!seen.insert(value).second) {
    return false;
  }

  if (auto *freeze = llvm::dyn_cast<llvm::FreezeInst>(value)) {
    return valueMayDependOnUnknownPlaceholder(freeze->getOperand(0), seen);
  }
  if (auto *call = llvm::dyn_cast<llvm::CallBase>(value)) {
    llvm::Function *callee = call->getCalledFunction();
    if (callee != nullptr &&
        (callee->getName().starts_with("notdec.register.vararg_unknown.") ||
         isNotDecOpaqueUnknownName(callee->getName()))) {
      return true;
    }
    if (std::optional<NativeRegisterValueInsertInfo> insert =
            parseNativeRegisterValueInsert(*call)) {
      return valueMayDependOnUnknownPlaceholder(insert->Base, seen) ||
             valueMayDependOnUnknownPlaceholder(insert->Value, seen);
    }
    if (std::optional<NativeRegisterValueExtractInfo> extract =
            parseNativeRegisterValueExtract(*call)) {
      return valueMayDependOnUnknownPlaceholder(extract->FullValue, seen);
    }
    return false;
  }

  // These are value-only glue nodes created while rebuilding register ranges.  If
  // any piece comes from a freeze-poison placeholder, the final vararg value is
  // not a real source value even if InstCombine can later pick 0 for it.
  bool shouldInspectOperands =
      llvm::isa<llvm::PHINode>(value) || llvm::isa<llvm::SelectInst>(value) ||
      llvm::isa<llvm::CastInst>(value) ||
      llvm::isa<llvm::BinaryOperator>(value) ||
      llvm::isa<llvm::UnaryOperator>(value) ||
      llvm::isa<llvm::ExtractValueInst>(value) ||
      llvm::isa<llvm::InsertValueInst>(value);
  if (!shouldInspectOperands) {
    return false;
  }
  auto *user = llvm::cast<llvm::User>(value);
  for (llvm::Value *operand : user->operands()) {
    if (valueMayDependOnUnknownPlaceholder(operand, seen)) {
      return true;
    }
  }
  return false;
}

bool valueMayDependOnUnknownPlaceholder(llvm::Value *value) {
  llvm::SmallPtrSet<llvm::Value *, 16> seen;
  return valueMayDependOnUnknownPlaceholder(value, seen);
}

bool shouldPoisonUnprovenVarArg(const CallArgStoreBinding &binding,
                                llvm::Value *value,
                                const llvm::DataLayout &layout,
                                const llvm::Instruction *context) {
  (void)binding;
  if (valueMayDependOnUnknownPlaceholder(value)) {
    return true;
  }
  if (llvm::isa_and_nonnull<llvm::ConstantInt>(value)) {
    return false;
  }
  if (value == nullptr || !value->getType()->isIntegerTy()) {
    return false;
  }
  llvm::KnownBits known = llvm::computeKnownBits(value, layout, nullptr, context);
  return known.isZero();
}

bool collectInsertedExtractPieces(
    llvm::Value *value, unsigned fullWidth,
    std::map<llvm::Value *, llvm::Value *> &valueMap, llvm::Value *&source,
    llvm::APInt &covered) {
  value = resolveValueMap(value, valueMap);
  if (auto *constant = llvm::dyn_cast_or_null<llvm::ConstantInt>(value)) {
    return constant->isZero() && constant->getBitWidth() == fullWidth;
  }
  auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(value);
  if (call == nullptr) {
    return false;
  }
  std::optional<NativeRegisterValueInsertInfo> insert =
      parseNativeRegisterValueInsert(*call);
  if (!insert || insert->FullWidth != fullWidth) {
    return false;
  }
  if (!collectInsertedExtractPieces(insert->Base, fullWidth, valueMap, source,
                                    covered)) {
    return false;
  }

  llvm::Value *piece = resolveValueMap(insert->Value, valueMap);
  auto *extractCall = llvm::dyn_cast_or_null<llvm::CallBase>(piece);
  std::optional<NativeRegisterValueExtractInfo> extract =
      extractCall == nullptr ? std::nullopt
                             : parseNativeRegisterValueExtract(*extractCall);
  if (!extract || extract->FullWidth != fullWidth ||
      extract->ReadWidth != insert->WriteWidth ||
      extract->BitOffset != insert->BitOffset) {
    return false;
  }
  llvm::Value *candidate = resolveValueMap(extract->FullValue, valueMap);
  if (candidate == nullptr || integerBitWidthOf(candidate) != fullWidth) {
    return false;
  }
  if (source == nullptr) {
    source = candidate;
  } else if (source != candidate) {
    return false;
  }
  covered |= llvm::APInt::getLowBitsSet(fullWidth, insert->WriteWidth)
                 .shl(insert->BitOffset);
  return true;
}

llvm::Value *
foldFullInsertOfExtracts(llvm::Value *value,
                         std::map<llvm::Value *, llvm::Value *> &valueMap) {
  value = resolveValueMap(value, valueMap);
  auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(value);
  std::optional<NativeRegisterValueInsertInfo> insert =
      call == nullptr ? std::nullopt : parseNativeRegisterValueInsert(*call);
  if (!insert || insert->FullWidth == 0) {
    return value;
  }
  llvm::Value *source = nullptr;
  llvm::APInt covered(insert->FullWidth, 0);
  if (!collectInsertedExtractPieces(value, insert->FullWidth, valueMap, source,
                                    covered) ||
      source == nullptr || !covered.isAllOnes()) {
    return value;
  }
  valueMap[value] = source;
  return source;
}

void rewriteSignatureShapes(llvm::Module &module, SignatureRewriteState &state,
                            const AbiFacts &abi,
                            NativeRegisterSummarySSASummary &summary) {
  std::map<llvm::Function *, llvm::Function *> replacements;
  std::vector<std::pair<llvm::Function *, SignatureShape>> replacementShapes;
  for (auto &[function, shape] : state.Shapes) {
    llvm::FunctionType *newType =
        functionTypeForShape(module.getContext(), shape);
    if (function->getFunctionType() == newType) {
      replacements[function] = function;
      continue;
    }
    llvm::Function *newFunction =
        createReplacementFunction(*function, *newType);
    replacements[function] = newFunction;
    replacementShapes.emplace_back(newFunction, shape);
    if (!function->isDeclaration()) {
      rewriteInternalFunctionBody(*function, *newFunction, shape, state);
    }
    ++summary.FunctionsRewritten;
  }

  std::vector<llvm::CallBase *> callsToRewrite;
  for (llvm::Function &function : module) {
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call != nullptr && shapeForCall(state, *call) != nullptr &&
          (state.CallArgs.count(call) != 0 ||
           state.ReturnHelpers.count(call) != 0 ||
           state.RangeReturnHelpers.count(call) != 0)) {
        callsToRewrite.push_back(call);
      }
    }
  }

  std::map<llvm::Value *, llvm::Value *> valueMap;
  valueMap.insert(state.ValueMap.begin(), state.ValueMap.end());
  std::vector<llvm::CallBase *> oldCallsToErase;
  std::vector<llvm::CallInst *> helpersToErase;
  auto remapValue = [&](llvm::Value *value) -> llvm::Value * {
    value = resolveValueMap(value, valueMap);
    return foldFullInsertOfExtracts(value, valueMap);
  };

  for (llvm::CallBase *oldCall : callsToRewrite) {
    if (oldCall->getParent() == nullptr) {
      continue;
    }
    std::vector<CallArgStoreBinding> &bindings = state.CallArgs[oldCall];
    llvm::Function *oldCallee = oldCall->getCalledFunction();
    const SignatureShape *shape = shapeForCall(state, *oldCall);
    if (shape == nullptr) {
      continue;
    }
    llvm::Value *newCallee = nullptr;
    llvm::FunctionType *newCallType = nullptr;
    if (oldCallee != nullptr) {
      newCallee = replacements[oldCallee];
      if (newCallee == nullptr) {
        continue;
      }
      newCallType = llvm::cast<llvm::Function>(newCallee)->getFunctionType();
    } else {
      newCallee = oldCall->getCalledOperand();
      newCallType = functionTypeForShape(module.getContext(), *shape);
    }
    std::vector<llvm::Value *> args;
    args.reserve(bindings.size());
    llvm::IRBuilder<> oldCallBuilder(oldCall);
    for (unsigned index = 0; index < shape->Params.size(); ++index) {
      const NativeSignatureSlot &slot = shape->Params[index];
      const CallArgStoreBinding *binding = bindingForIndex(bindings, index);
      llvm::Value *value = binding == nullptr ? nullptr : binding->Value;
      if (value != nullptr) {
        value = remapValue(value);
        value = localizeCallArgument(*oldCall->getFunction(), *oldCall, value);
      }
      llvm::StringRef reason;
      if (value == nullptr) {
        reason = "call_arg_rewrite_missing_binding";
      } else if (value->getType() != slotType(slot)) {
        reason = "call_arg_rewrite_type_mismatch";
      } else if (valueMayDependOnUnknownPlaceholder(value)) {
        state.Warnings.push_back(callSlotWarning(
            *oldCall, slot, "call_arg", "call_arg_uses_unknown_value"));
      } else if (mayDependOnSummaryClobberValue(value)) {
        state.Warnings.push_back(callSlotWarning(
            *oldCall, slot, "call_arg", "call_arg_uses_clobber_value"));
      }
      if (!reason.empty()) {
        state.Warnings.push_back(
            callSlotWarning(*oldCall, slot, "call_arg", reason));
        std::string regName = slotRegisterName(slot);
        value = unknownValueAt(oldCallBuilder, slotType(slot),
                               regName + ".arg_unknown");
        attachUnknownValueMetadata(value, reason,
                                   oldCall->getFunction()->getName(),
                                   calleeNameForWarning(*oldCall), regName,
                                   "call_arg");
      }
      args.push_back(value);
    }
    if (shape->VarArg) {
      for (const CallArgStoreBinding &binding : bindings) {
        if (binding.Index < shape->Params.size()) {
          continue;
        }
        llvm::Value *value = binding.Value;
        if (value == nullptr) {
          continue;
        }
        value = remapValue(value);
        value = localizeCallArgument(*oldCall->getFunction(), *oldCall, value);
        if (mayDependOnSummaryClobberValue(value)) {
          state.Warnings.push_back(callRegisterWarning(
              *oldCall, binding.Unit == nullptr ? "" : binding.Unit->Name,
              "call_arg", "vararg_arg_uses_clobber_value"));
        }
        bool unknownValue = valueMayDependOnUnknownPlaceholder(value);
        if (isLikelyX86_64SysVAbi(abi) &&
            shouldPoisonUnprovenVarArg(binding, value, module.getDataLayout(),
                                       oldCall)) {
          llvm::StringRef reason = unknownValue ? "vararg_arg_uses_unknown_value"
                                                : "vararg_arg_unproven_zero";
          state.Warnings.push_back(callRegisterWarning(
              *oldCall, binding.Unit == nullptr ? "" : binding.Unit->Name,
              "call_arg", reason));
          llvm::Function *unknown =
              getOrInsertVarArgUnknownHelper(module, value->getType());
          value = oldCallBuilder.CreateCall(unknown);
          attachUnknownValueMetadata(value, reason,
                                     oldCall->getFunction()->getName(),
                                     calleeNameForWarning(*oldCall),
                                     binding.Unit == nullptr ? ""
                                                             : binding.Unit->Name,
                                     "call_arg");
        }
        if (binding.ValueType != nullptr &&
            value->getType() != binding.ValueType) {
          state.Warnings.push_back(callRegisterWarning(
              *oldCall, binding.Unit == nullptr ? "" : binding.Unit->Name,
              "call_arg", "vararg_arg_type_mismatch"));
          continue;
        }
        args.push_back(value);
      }
    }
    llvm::CallInst *newCall =
        rewriteCallInst(*oldCall, *newCallee, *newCallType, args);
    state.RewrittenCalls.insert(newCall);
    if (!oldCall->use_empty()) {
      llvm::IRBuilder<> builder(newCall->getNextNode());
      llvm::Value *replacement =
          replacementForOldCallUses(builder, *oldCall, *newCall, *shape);
      if (replacement != nullptr) {
        oldCall->replaceAllUsesWith(replacement);
        newCall->takeName(oldCall);
      }
    }
    valueMap[oldCall] = newCall;
    oldCallsToErase.push_back(oldCall);
    auto helpersIt = state.ReturnHelpers.find(oldCall);
    if (helpersIt != state.ReturnHelpers.end() && !shape->Returns.empty()) {
      llvm::IRBuilder<> builder(newCall->getNextNode());
      for (auto &[name, helper] : helpersIt->second) {
        if (helper->getParent() == nullptr) {
          continue;
        }
        llvm::Value *value =
            extractReturnRegister(builder, *shape, *newCall, name);
        if (value == nullptr || value->getType() != helper->getType()) {
          llvm::StringRef reason = value == nullptr
                                      ? "return_helper_rewrite_missing_value"
                                      : "return_helper_rewrite_type_mismatch";
          state.Warnings.push_back(
              callRegisterWarning(*oldCall, name, "return", reason));
          value = unknownValueAt(builder, helper->getType(),
                                 name + ".return_unknown");
          attachUnknownValueMetadata(value, reason,
                                     oldCall->getFunction()->getName(),
                                     calleeNameForWarning(*oldCall), name,
                                     "return");
        }
        valueMap[helper] = value;
        helper->replaceAllUsesWith(value);
        if (helper->use_empty()) {
          helpersToErase.push_back(helper);
        }
      }
    }
    auto rangeHelpersIt = state.RangeReturnHelpers.find(oldCall);
    if (rangeHelpersIt != state.RangeReturnHelpers.end() &&
        !shape->Returns.empty()) {
      llvm::IRBuilder<> builder(newCall->getNextNode());
      for (RangeReturnHelper &rangeHelper : rangeHelpersIt->second) {
        llvm::CallInst *helper = rangeHelper.Helper;
        if (helper == nullptr || helper->getParent() == nullptr) {
          continue;
        }
        llvm::Value *value =
            extractReturnRange(builder, *shape, *newCall, rangeHelper.Range);
        if (value == nullptr || value->getType() != helper->getType()) {
          llvm::StringRef reason = value == nullptr
                                      ? "range_return_helper_rewrite_missing_value"
                                      : "range_return_helper_rewrite_type_mismatch";
          std::string regName = rangeHelper.Range.Global == nullptr
                                    ? std::string("")
                                    : rangeHelper.Range.Global->getName().str();
          state.Warnings.push_back(
              callRegisterWarning(*oldCall, regName, "return", reason));
          value = unknownValueAt(builder, helper->getType(),
                                 "range_return_unknown");
          attachUnknownValueMetadata(value, reason,
                                     oldCall->getFunction()->getName(),
                                     calleeNameForWarning(*oldCall), regName,
                                     "return");
        }
        valueMap[helper] = value;
        helper->replaceAllUsesWith(value);
        if (helper->use_empty()) {
          helpersToErase.push_back(helper);
        }
      }
    }
    ++summary.CallsRewritten;
  }

  for (llvm::CallInst *helper : helpersToErase) {
    if (helper->getParent() != nullptr && helper->use_empty()) {
      helper->eraseFromParent();
    }
  }
  for (llvm::CallBase *call : oldCallsToErase) {
    if (call->getParent() != nullptr && call->use_empty()) {
      call->eraseFromParent();
    }
  }

  for (llvm::StoreInst *store : state.StoresToErase) {
    if (store->getParent() != nullptr && store->use_empty()) {
      store->eraseFromParent();
      ++summary.DeadStoresRemoved;
    }
  }

  for (auto &[oldFunction, newFunction] : replacements) {
    if (oldFunction != newFunction && oldFunction->use_empty() &&
        oldFunction->empty()) {
      oldFunction->eraseFromParent();
    }
  }
  for (auto &[function, shape] : replacementShapes) {
    state.Shapes[function] = std::move(shape);
  }
}

std::string metadataField(const llvm::MDNode &node, llvm::StringRef key) {
  std::string prefix = (key + "=").str();
  for (const llvm::MDOperand &operand : node.operands()) {
    auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
    if (text == nullptr) {
      continue;
    }
    llvm::StringRef value = text->getString();
    if (value.consume_front(prefix)) {
      return value.str();
    }
  }
  return "";
}

std::string warningReasonForHelper(const llvm::CallInst &helper,
                                   llvm::StringRef kind,
                                   llvm::StringRef regName,
                                   const llvm::Function *callee) {
  if (kind == "return") {
    if (callee != nullptr && callee->isDeclaration()) {
      if (regName == "RDX") {
        return "non_primary_external_return_register";
      }
      return "missing_external_return_prototype";
    }
    return "unresolved_return_register";
  }
  if (kind == "clobber") {
    return "remaining_summary_clobber_value";
  }
  return "unresolved_register_helper";
}

std::vector<NativeRegisterSummarySSAWarning>
collectRemainingCallValueWarnings(const llvm::Module &module) {
  std::vector<NativeRegisterSummarySSAWarning> warnings;
  for (const llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (const llvm::Instruction &inst : llvm::instructions(function)) {
      auto *helper = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (helper == nullptr) {
        continue;
      }
      llvm::Function *helperCallee = helper->getCalledFunction();
      if (helperCallee == nullptr ||
          !helperCallee->getName().starts_with("notdec.register.summary_")) {
        continue;
      }
      llvm::MDNode *metadata =
          helper->getMetadata("notdec.register.summary_ssa.call_value");
      std::string regName =
          metadata == nullptr ? "" : metadataField(*metadata, "name");
      std::string kind =
          metadata == nullptr ? "" : metadataField(*metadata, "kind");

      const llvm::CallBase *sourceCall = nullptr;
      const llvm::Instruction *previous = helper->getPrevNode();
      while (previous != nullptr) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(previous);
        if (call != nullptr && isAnalyzableCall(*call)) {
          sourceCall = call;
          break;
        }
        previous = previous->getPrevNode();
      }
      const llvm::Function *callee =
          sourceCall == nullptr ? nullptr : sourceCall->getCalledFunction();

      NativeRegisterSummarySSAWarning warning;
      warning.FunctionName = function.getName().str();
      warning.CalleeName =
          callee == nullptr ? "<indirect>" : callee->getName().str();
      warning.RegisterName = regName;
      warning.Kind = kind;
      warning.Reason = warningReasonForHelper(*helper, kind, regName, callee);
      warning.Uses = helper->getNumUses();
      warnings.push_back(std::move(warning));
    }
  }
  return warnings;
}

} // namespace

NativeRegisterSummarySSASummary
runNativeRegisterSummarySSA(llvm::Module &module,
                            const NativeRegisterSummarySSAOptions &options) {
  NativeExternalPrototypeMap externalPrototypes =
      defaultNativeExternalPrototypes();
  if (!options.ExternalPrototypeJsonPath.empty()) {
    std::string errorMessage;
    std::optional<NativeExternalPrototypeMap> loadedPrototypes =
        loadNativeExternalPrototypesJson(options.ExternalPrototypeJsonPath,
                                         errorMessage);
    if (loadedPrototypes) {
      for (auto &[name, prototype] : *loadedPrototypes) {
        externalPrototypes[name] = std::move(prototype);
      }
    } else {
      llvm::errs() << "warning: failed to load external prototypes: "
                   << errorMessage << '\n';
    }
  }

  truncateKnownNoReturnCalls(module, externalPrototypes);

  NativeStackFrameRewriteSummary stackFrameSummary =
      runNativeStackFrameRewrite(module);
  NativeStackCanaryCleanupSummary canarySummary =
      runNativeStackCanaryCleanup(module);
  NativeRegisterSummarySSAOptions effectiveOptions = options;
  effectiveOptions.IgnoredRegisters.insert(
      stackFrameSummary.IgnoredRegisters.begin(),
      stackFrameSummary.IgnoredRegisters.end());
  std::map<llvm::GlobalVariable *, RegisterUnit> units =
      collectRegisterUnits(module);
  (void)canonicalizeRegisterPointerPhiLoads(module, units);
  NativeRegisterPreSummaryPeepholeSummary peepholeSummary =
      runNativeRegisterPreSummaryPeephole(module);
  if (options.PrintSummary) {
    printNativeRegisterPreSummaryPeepholeSummary(peepholeSummary,
                                                 llvm::errs());
  }
  AbiFacts abi = collectAbiFacts(module, units);

  NativeRegisterSummaryOptions baseSummaryOptions;
  baseSummaryOptions.AttachMetadata = false;
  baseSummaryOptions.IgnoredRegisters = effectiveOptions.IgnoredRegisters;
  baseSummaryOptions.ExternalCallShapes =
      buildExternalCallShapes(module, units, abi, externalPrototypes);
  baseSummaryOptions.UnknownExternalInputPolicy =
      NativeRegisterUnknownExternalInputPolicy::NoInputs;
  baseSummaryOptions.RunTopDownDemand = false;
  baseSummaryOptions.CollectExternalCallsiteEvidence = true;
  baseSummaryOptions.ExternalEvidenceSlots = unknownExternalEvidenceSlots(abi);
  NativeRegisterSummary baseRegisterSummary =
      runNativeRegisterSummary(module, baseSummaryOptions);
  std::map<llvm::Function *, FunctionSummaryFacts> baseFacts =
      summaryFactsByFunction(baseRegisterSummary, module);
  truncateKnownNoReturnCalls(module, externalPrototypes, &baseFacts);

  std::vector<NativeRegisterSummarySSAWarning> inferenceWarnings;
  NativeExternalCallsiteShapeMap externalCallsiteShapes =
      inferExternalCallShapes(externalPrototypes, baseRegisterSummary, units,
                              inferenceWarnings);

  NativeRegisterSummaryOptions finalSummaryOptions = baseSummaryOptions;
  finalSummaryOptions.AttachMetadata = options.AttachMetadata;
  finalSummaryOptions.RunTopDownDemand = true;
  finalSummaryOptions.ExternalCallShapes =
      buildExternalCallShapes(module, units, abi, externalPrototypes);
  finalSummaryOptions.ExternalCallsiteShapes = externalCallsiteShapes;
  finalSummaryOptions.CollectExternalCallsiteEvidence = false;
  finalSummaryOptions.ExternalEvidenceSlots.clear();
  NativeRegisterSummary registerSummary =
      runNativeRegisterSummary(module, finalSummaryOptions);
  std::map<llvm::Function *, FunctionSummaryFacts> facts =
      summaryFactsByFunction(registerSummary, module);
  truncateKnownNoReturnCalls(module, externalPrototypes, &facts);
  SignatureRewriteState signatureState;
  signatureState.ExternalPrototypes = &externalPrototypes;
  signatureState.ExternalCallsiteShapes = &externalCallsiteShapes;
  signatureState.Warnings = std::move(inferenceWarnings);
  if (options.EnableRewrite) {
    signatureState.Shapes = buildInitialSignatureShapes(
        module, units, facts, abi, externalPrototypes);
  }

  NativeRegisterSummarySSASummary summary;
  summary.StackCanaryChecksRemoved = canarySummary.CanaryChecksRemoved;
  summary.StackCanaryFailBlocksRemoved = canarySummary.FailBlocksRemoved;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    NativeRegisterSummarySSAFunctionSummary fn;
    FunctionBuilder builder(function, units, facts, abi, options, fn,
                            signatureState);
    builder.run();
    addFunctionSummary(summary, fn);
    summary.Functions.push_back(std::move(fn));
  }
  if (options.EnableRewrite) {
    refineIndirectCallsiteParamShapes(signatureState);
    addDemandedExternalReturns(signatureState, units, abi, externalPrototypes);
    markSignatureCallArgStores(signatureState, summary);
    rewriteSignatureShapes(module, signatureState, abi, summary);
    eraseUnusedSummaryHelperDeclarations(module);
    if (options.EnableResidueRemoval) {
      constexpr unsigned maxPostRewriteCleanupIterations = 10;
      for (unsigned iteration = 0; iteration < maxPostRewriteCleanupIterations;
           ++iteration) {
        if (effectiveOptions.EnablePostRewriteInstCombine) {
          runPostRewriteInstCombine(module);
        }

        uint64_t deadStoresRemovedThisIteration = 0;
        for (llvm::Function &function : module) {
          if (function.isDeclaration()) {
            continue;
          }
          NativeRegisterSummarySSAFunctionSummary cleanupFn;
          FunctionBuilder cleanup(function, units, facts, abi, options,
                                  cleanupFn, signatureState);
          cleanup.removeDeadStoresAfterSignatureRewrite();
          deadStoresRemovedThisIteration += cleanupFn.DeadStoresRemoved;
        }
        summary.DeadStoresRemoved += deadStoresRemovedThisIteration;

        if (!effectiveOptions.EnablePostRewriteInstCombine ||
            deadStoresRemovedThisIteration == 0) {
          break;
        }
      }
      NativeStackFrameCleanupOptions cleanupOptions;
      cleanupOptions.StackPointerRegister =
          stackFrameSummary.StackPointerRegister;
      cleanupOptions.Registers = effectiveOptions.IgnoredRegisters;
      NativeStackFrameCleanupSummary cleanupSummary =
          runNativeStackFrameCleanup(module, cleanupOptions);
      summary.StackFrameAccessesRewritten += cleanupSummary.AccessesRewritten;
      summary.StackFramePointerLoadsReplaced +=
          cleanupSummary.FramePointerLoadsReplaced;
      summary.StackFrameRegisterLoadsRemoved +=
          cleanupSummary.RegisterLoadsRemoved;
      summary.StackFrameRegisterStoresRemoved +=
          cleanupSummary.RegisterStoresRemoved;
      summary.StackFrameAllocaLoadsRemoved +=
          cleanupSummary.StackAllocaLoadsRemoved;
      summary.StackFrameAllocaStoresRemoved +=
          cleanupSummary.StackAllocaStoresRemoved;
      summary.StackFrameAllocasRemoved += cleanupSummary.StackAllocasRemoved;
      NativeStackCanaryCleanupSummary lateCanarySummary =
          runNativeStackCanaryCleanup(module);
      summary.StackCanaryChecksRemoved += lateCanarySummary.CanaryChecksRemoved;
      summary.StackCanaryFailBlocksRemoved +=
          lateCanarySummary.FailBlocksRemoved;
      eraseDeadSummaryCallValueHelpers(module);
      summary.DeadLoadsRemoved += eraseDeadSummarySSAEntryReads(module);
      eraseUnusedSummaryHelperDeclarations(module);
    }
  }
  summary.FunctionsSeen = summary.Functions.size();
  summary.Warnings = collectRemainingCallValueWarnings(module);
  summary.Warnings.insert(summary.Warnings.end(),
                          signatureState.Warnings.begin(),
                          signatureState.Warnings.end());
  if (options.PrintSummary) {
    printNativeRegisterSummarySSASummary(summary, llvm::errs());
  }
  return summary;
}

void printNativeRegisterSummarySSASummary(
    const NativeRegisterSummarySSASummary &summary, llvm::raw_ostream &os) {
  os << "Native register summary SSA: functions=" << summary.FunctionsSeen
     << " loads=" << summary.LoadsSeen << " stores=" << summary.StoresSeen
     << " loads_replaced=" << summary.LoadsReplaced
     << " dead_loads_removed=" << summary.DeadLoadsRemoved
     << " dead_stores_removed=" << summary.DeadStoresRemoved
     << " phis_created=" << summary.PhisCreated
     << " phis_simplified=" << summary.PhisSimplified
     << " range_entry_inputs=" << summary.RangeEntryInputs
     << " call_returns=" << summary.CallReturnValues
     << " call_clobbers=" << summary.CallClobberValues
     << " call_arg_stores_marked=" << summary.CallArgStoresMarked
     << " calls_rewritten=" << summary.CallsRewritten
     << " functions_rewritten=" << summary.FunctionsRewritten
     << " preserved_calls=" << summary.PreservedCalls
     << " unknown_call_effects=" << summary.UnknownCallEffects
     << " stack_frame_accesses_rewritten="
     << summary.StackFrameAccessesRewritten
     << " stack_frame_pointer_loads_replaced="
     << summary.StackFramePointerLoadsReplaced
     << " stack_frame_register_loads_removed="
     << summary.StackFrameRegisterLoadsRemoved
     << " stack_frame_register_stores_removed="
     << summary.StackFrameRegisterStoresRemoved
     << " stack_frame_alloca_loads_removed="
     << summary.StackFrameAllocaLoadsRemoved
     << " stack_frame_alloca_stores_removed="
     << summary.StackFrameAllocaStoresRemoved
     << " stack_frame_allocas_removed=" << summary.StackFrameAllocasRemoved
     << " stack_canary_checks_removed=" << summary.StackCanaryChecksRemoved
     << " stack_canary_fail_blocks_removed="
     << summary.StackCanaryFailBlocksRemoved
     << " partial_demand_candidates=" << summary.PartialDemandCandidates
     << " partial_demand_matched=" << summary.PartialDemandMatched
     << " partial_demand_rejected=" << summary.PartialDemandRejected
     << " range_registers_planned=" << summary.RangeRegistersPlanned
     << " range_segments_planned=" << summary.RangeSegmentsPlanned
     << " range_read_events=" << summary.RangeReadEvents
     << " range_write_events=" << summary.RangeWriteEvents
     << " range_clobber_events=" << summary.RangeClobberEvents
     << " warnings=" << summary.Warnings.size() << "\n";
  for (const NativeRegisterSummarySSAWarning &warning : summary.Warnings) {
    os << "  warning"
       << " function=" << warning.FunctionName
       << " callee=" << warning.CalleeName
       << " register=" << warning.RegisterName << " kind=" << warning.Kind
       << " reason=" << warning.Reason << " uses=" << warning.Uses << "\n";
  }
  for (const NativeRegisterSummarySSAFunctionSummary &function :
       summary.Functions) {
    os << "  " << function.FunctionName << ": loads=" << function.LoadsSeen
       << " stores=" << function.StoresSeen
       << " loads_replaced=" << function.LoadsReplaced
       << " dead_loads_removed=" << function.DeadLoadsRemoved
       << " dead_stores_removed=" << function.DeadStoresRemoved
       << " phis_created=" << function.PhisCreated
       << " phis_simplified=" << function.PhisSimplified
       << " range_entry_inputs=" << function.RangeEntryInputs
       << " call_returns=" << function.CallReturnValues
       << " call_clobbers=" << function.CallClobberValues
       << " call_arg_stores_marked=" << function.CallArgStoresMarked
       << " calls_rewritten=" << function.CallsRewritten
       << " functions_rewritten=" << function.FunctionsRewritten
       << " preserved_calls=" << function.PreservedCalls
       << " unknown_call_effects=" << function.UnknownCallEffects
       << " partial_demand_candidates=" << function.PartialDemandCandidates
       << " partial_demand_matched=" << function.PartialDemandMatched
       << " partial_demand_rejected=" << function.PartialDemandRejected
       << " range_registers_planned=" << function.RangeRegistersPlanned
       << " range_segments_planned=" << function.RangeSegmentsPlanned
       << " range_read_events=" << function.RangeReadEvents
       << " range_write_events=" << function.RangeWriteEvents
       << " range_clobber_events=" << function.RangeClobberEvents << "\n";
  }
}

} // namespace notdec::bin2llvm
