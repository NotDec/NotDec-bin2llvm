#include "notdec-bin2llvm/passes/heritage/NativeHeritageSSA.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/APInt.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace notdec::bin2llvm {
namespace {

// RegisterUnit is the pass-side view of one RegisterStorage backing global.
// The first version intentionally uses this coarse unit so aliases such as
// RAX/EAX/AX/AL are not split into unrelated SSA values.
struct RegisterUnit {
  llvm::GlobalVariable *Global = nullptr;
  std::string Name;
  uint32_t Offset = 0;
  uint32_t Size = 0;
};

// AccessInfo separates "this is a register access" from "this is safe to
// replace with the backing storage value". Metadata gives the architectural
// access range, while the IR type tells whether this instruction carries the
// full backing value used by RegisterStorage.
struct AccessInfo {
  RegisterUnit *Unit = nullptr;
  bool IsRegisterAccess = false;
  bool IsFullUnit = false;
  bool IsStorageValue = false;
  uint32_t Offset = 0;
  uint32_t Size = 0;
  std::string Name;
};

using BlockRegKey = std::pair<llvm::BasicBlock *, llvm::GlobalVariable *>;
using CallEffectKey =
    std::tuple<llvm::Instruction *, llvm::GlobalVariable *, std::string>;

struct AbiRegisterEffects {
  std::set<std::string> Unaffected;
  std::set<std::string> KilledByCall;
  std::set<std::string> Outputs;
  std::vector<std::string> Inputs;
  std::string StackPointerRegister;
};

struct FlagBlockLiveness {
  std::set<llvm::GlobalVariable *> LiveIn;
  std::set<llvm::GlobalVariable *> LiveOut;
};

enum class PendingPhiState {
  Incomplete,
  Completing,
  Complete,
};

// PendingPhiInfo is the explicit state for Braun-style temporary PHIs.  The CFG
// is already known, so `Incomplete` means "created to break recursion and still
// needs finalize"; `Complete` means all known predecessor incoming values were
// added.
struct PendingPhiInfo {
  llvm::PHINode *Phi = nullptr;
  PendingPhiState State = PendingPhiState::Incomplete;
};

// CallEffectInfo keeps the data-flow effect separate from why that effect was
// chosen.  The kind drives SSA behavior; the source is audit metadata for later
// prototype recovery decisions.
struct CallEffectInfo {
  std::string Kind;
  std::string Source;
};

struct CallsiteInfo {
  uint64_t Index = 0;
  std::string Id;
};

// CallInputPathInfo is the native-side counterpart of Ghidra's TraverseNode
// flags.  The booleans are kept as decision data; `AuditFlags` preserves the
// current human-readable metadata while this is being migrated.
struct CallInputPathInfo {
  bool ActionAlt = false;
  bool Indirect = false;
  bool IndirectAlt = false;
  bool LsbTruncated = false;
  bool ConcatHigh = false;
  bool SawPhi = false;
  bool SawLoop = false;
  bool SawUnknown = false;
  bool SawDoubleCallUse = false;
  bool SawExternalUse = false;
  bool SawBlockedUse = false;
  bool SawAggregate = false;
  bool SawCast = false;
  std::vector<std::string> AuditFlags;
};

// Call input trial annotation is still conservative: `Strength` records the
// old local evidence for audit, `State` is the Ghidra-style decision that later
// prototype recovery consumes, and `Reason` keeps the current explanation
// stable when the evidence field is replaced.
struct CallInputTrialInfo {
  std::string Strength;
  std::string State;
  std::string Reason;
  std::vector<std::string> Flags;
  llvm::Instruction *DoubleUseCandidate = nullptr;
  bool HasBeforeDoubleUseInfo = false;
  std::string BeforeDoubleUseStrength;
  std::string BeforeDoubleUseState;
  std::string BeforeDoubleUseReason;
  std::vector<std::string> BeforeDoubleUseFlags;
  std::string DoubleUseTargetCallsiteId;
  std::string DoubleUseTargetCallee;
  std::string DoubleUseTargetRegister;
  std::string DoubleUseTargetState;
  std::vector<std::string> PathFlags;
  std::vector<std::string> BeforeDoubleUsePathFlags;
  CallInputPathInfo Path;
  CallInputPathInfo BeforeDoubleUsePath;
};

// CallInputTrialContext identifies the helper that is currently being checked.
// LLVM still contains register-state stores, so the use check needs this
// context to distinguish "the value defines this register before the call" from
// "the value is consumed somewhere else".
struct CallInputTrialContext {
  llvm::Instruction *Candidate = nullptr;
  std::string CallsiteId;
  std::string RegisterName;
  std::string CalleeName;
};

// Internal table row for call input trial finalization.  This is deliberately
// kept inside RegisterSSA for now: PrototypeRecovery should keep consuming the
// finalized metadata, while double-use/final-check rules can later look across
// rows before metadata is written back.
struct CallInputTrialRecord {
  llvm::Instruction *Candidate = nullptr;
  llvm::Value *CandidateValue = nullptr;
  llvm::MDNode *Metadata = nullptr;
  CallInputTrialContext Context;
  CallInputTrialInfo Info;
  CallInputTrialRecord *DoubleUseRecord = nullptr;
  bool Checked = false;
};

enum class CallInputUseCheckResult {
  OnlyCurrentCall,
  SharedUse,
  DoubleCallUse,
};

// Result of descendant-use checking.  The enum keeps the current decision
// stable, while DoubleUseCandidate preserves the other helper for later
// trial-table finalization.
struct CallInputUseCheckInfo {
  CallInputUseCheckResult Result = CallInputUseCheckResult::OnlyCurrentCall;
  llvm::Instruction *DoubleUseCandidate = nullptr;
  CallInputPathInfo Path;
};

bool isFlagRegisterName(llvm::StringRef name) {
  return name == "CF" || name == "PF" || name == "AF" || name == "ZF" ||
         name == "SF" || name == "TF" || name == "IF" || name == "DF" ||
         name == "OF";
}

bool isInstructionPointerName(llvm::StringRef name) {
  return name == "RIP";
}

bool isNotDecRegisterHelperCall(const llvm::CallBase &call) {
  llvm::Function *callee = call.getCalledFunction();
  return callee != nullptr &&
         callee->getName().starts_with("notdec.register.");
}

uint32_t bitWidth(uint32_t byteSize) {
  return byteSize * 8;
}

std::optional<uint32_t> bitOffset(const AccessInfo &access) {
  if (access.Unit == nullptr || access.Offset < access.Unit->Offset ||
      access.Size == 0 ||
      access.Offset + access.Size > access.Unit->Offset + access.Unit->Size) {
    return std::nullopt;
  }
  return bitWidth(access.Offset - access.Unit->Offset);
}

bool isIntegerUnit(const AccessInfo &access) {
  if (access.Unit == nullptr || access.Unit->Global == nullptr) {
    return false;
  }
  return llvm::isa<llvm::IntegerType>(access.Unit->Global->getValueType());
}

bool canPromotePartialAccess(const AccessInfo &access) {
  return access.Unit != nullptr && access.IsRegisterAccess &&
         !access.IsFullUnit && isIntegerUnit(access) &&
         !isFlagRegisterName(access.Unit->Name) &&
         !isInstructionPointerName(access.Unit->Name) &&
         bitOffset(access).has_value();
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

uint32_t parseU32(const std::optional<std::string> &text) {
  if (!text) {
    return 0;
  }
  try {
    size_t parsed = 0;
    unsigned long value = std::stoul(*text, &parsed, 0);
    if (parsed != text->size()) {
      return 0;
    }
    return static_cast<uint32_t>(value);
  } catch (...) {
    return 0;
  }
}

std::string unitName(const llvm::GlobalVariable &global) {
  auto *node = global.getMetadata("notdec.register");
  if (auto name = mdField(node, "name")) {
    if (!name->empty()) {
      return *name;
    }
  }
  return global.getName().str();
}

std::map<llvm::GlobalVariable *, RegisterUnit> collectRegisterUnits(
    llvm::Module &module) {
  std::map<llvm::GlobalVariable *, RegisterUnit> units;
  for (llvm::GlobalVariable &global : module.globals()) {
    auto *node = global.getMetadata("notdec.register");
    if (node == nullptr) {
      continue;
    }
    RegisterUnit unit;
    unit.Global = &global;
    unit.Name = unitName(global);
    unit.Offset = parseU32(mdField(node, "offset"));
    unit.Size = parseU32(mdField(node, "size"));
    units.emplace(&global, std::move(unit));
  }
  return units;
}

llvm::MDNode *fullRegisterAccessMetadata(llvm::LLVMContext &context,
                                         const RegisterUnit &unit) {
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "base=" + unit.Name),
      llvm::MDString::get(context, "space=register"),
      llvm::MDString::get(context, "offset=" + std::to_string(unit.Offset)),
      llvm::MDString::get(context, "size=" + std::to_string(unit.Size)),
      llvm::MDString::get(context, "name=" + unit.Name),
  };
  return llvm::MDNode::get(context, fields);
}

AbiRegisterEffects collectAbiRegisterEffects(llvm::Module &module) {
  AbiRegisterEffects effects;
  llvm::NamedMDNode *abiMetadata = module.getNamedMetadata("notdec.abi");
  if (abiMetadata == nullptr) {
    return effects;
  }

  for (llvm::MDNode *abiNode : abiMetadata->operands()) {
    if (std::optional<std::string> stackPointer =
            mdField(abiNode, "stackpointer.register")) {
      effects.StackPointerRegister = *stackPointer;
    }
    std::vector<llvm::MDNode *> childLists;
    for (const llvm::MDOperand &operand : abiNode->operands()) {
      auto *child = llvm::dyn_cast_or_null<llvm::MDNode>(operand.get());
      if (child != nullptr) {
        childLists.push_back(child);
      }
    }
    if (childLists.size() >= 2) {
      for (const llvm::MDOperand &entryOperand : childLists[0]->operands()) {
        auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(entryOperand.get());
        if (entry == nullptr) {
          continue;
        }
        for (const llvm::MDOperand &storageOperand : entry->operands()) {
          auto *storageNode =
              llvm::dyn_cast_or_null<llvm::MDNode>(storageOperand.get());
          if (storageNode == nullptr ||
              mdField(storageNode, "kind") !=
                  std::optional<std::string>("register")) {
            continue;
          }
          std::optional<std::string> name = mdField(storageNode, "name");
          if (name && !name->empty()) {
            effects.Inputs.push_back(*name);
          }
        }
      }
      for (const llvm::MDOperand &entryOperand : childLists[1]->operands()) {
        auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(entryOperand.get());
        if (entry == nullptr) {
          continue;
        }
        for (const llvm::MDOperand &storageOperand : entry->operands()) {
          auto *storageNode =
              llvm::dyn_cast_or_null<llvm::MDNode>(storageOperand.get());
          if (storageNode == nullptr ||
              mdField(storageNode, "kind") !=
                  std::optional<std::string>("register")) {
            continue;
          }
          std::optional<std::string> name = mdField(storageNode, "name");
          if (name && !name->empty()) {
            effects.Outputs.insert(*name);
          }
        }
      }
    }
    if (childLists.size() < 3) {
      return effects;
    }
    llvm::MDNode *effectList = childLists[2];
    for (const llvm::MDOperand &effectOperand : effectList->operands()) {
      auto *effectNode =
          llvm::dyn_cast_or_null<llvm::MDNode>(effectOperand.get());
      if (effectNode == nullptr) {
        continue;
      }
      std::optional<std::string> effectKind = mdField(effectNode, "effect");
      bool isUnaffected = effectKind == std::optional<std::string>("unaffected");
      bool isKilledByCall =
          effectKind == std::optional<std::string>("killedbycall");
      if (!isUnaffected && !isKilledByCall) {
        continue;
      }
      for (const llvm::MDOperand &storageOperand : effectNode->operands()) {
        auto *storageNode =
            llvm::dyn_cast_or_null<llvm::MDNode>(storageOperand.get());
        if (storageNode == nullptr ||
            mdField(storageNode, "kind") !=
                std::optional<std::string>("register")) {
          continue;
        }
        std::optional<std::string> name = mdField(storageNode, "name");
        if (name && !name->empty()) {
          if (isUnaffected) {
            effects.Unaffected.insert(*name);
          } else {
            effects.KilledByCall.insert(*name);
          }
        }
      }
    }
  }
  return effects;
}

AccessInfo registerLoad(llvm::LoadInst &load,
                        std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      load.getPointerOperand()->stripPointerCasts());
  if (global == nullptr) {
    return {};
  }
  auto it = units.find(global);
  if (it == units.end()) {
    return {};
  }
  llvm::MDNode *accessMetadata = load.getMetadata("notdec.register.access");
  if (accessMetadata == nullptr &&
      global->getMetadata("notdec.register") == nullptr) {
    return {};
  }
  RegisterUnit &unit = it->second;
  if (accessMetadata == nullptr &&
      load.getMetadata("notdec.register.external_input") == nullptr) {
    accessMetadata = fullRegisterAccessMetadata(load.getContext(), unit);
    load.setMetadata("notdec.register.access", accessMetadata);
  }
  uint32_t offset = unit.Offset;
  uint32_t size = unit.Size;
  std::string name = unit.Name;
  if (accessMetadata != nullptr) {
    offset = parseU32(mdField(accessMetadata, "offset"));
    size = parseU32(mdField(accessMetadata, "size"));
    if (auto metadataName = mdField(accessMetadata, "name")) {
      name = *metadataName;
    }
  }
  bool fullUnit = offset == unit.Offset && size == unit.Size;
  bool storageValue = load.getType() == global->getValueType();
  return AccessInfo{&unit, true, fullUnit, storageValue, offset, size, name};
}

AccessInfo registerStore(
    llvm::StoreInst &store,
    std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      store.getPointerOperand()->stripPointerCasts());
  if (global == nullptr) {
    return {};
  }
  auto it = units.find(global);
  if (it == units.end()) {
    return {};
  }
  llvm::MDNode *accessMetadata = store.getMetadata("notdec.register.access");
  if (accessMetadata == nullptr &&
      global->getMetadata("notdec.register") == nullptr) {
    return {};
  }
  RegisterUnit &unit = it->second;
  if (accessMetadata == nullptr) {
    accessMetadata = fullRegisterAccessMetadata(store.getContext(), unit);
    store.setMetadata("notdec.register.access", accessMetadata);
  }
  uint32_t offset = parseU32(mdField(accessMetadata, "offset"));
  uint32_t size = parseU32(mdField(accessMetadata, "size"));
  std::string name = unit.Name;
  if (auto metadataName = mdField(accessMetadata, "name")) {
    name = *metadataName;
  }
  bool fullUnit = offset == unit.Offset && size == unit.Size;
  bool storageValue = store.getValueOperand()->getType() == global->getValueType();
  return AccessInfo{&unit, true, fullUnit, storageValue, offset, size, name};
}

bool isRegisterClobberCall(const llvm::Instruction &inst) {
  auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
  if (call == nullptr) {
    return false;
  }
  llvm::Function *callee = call->getCalledFunction();
  if (isNotDecRegisterHelperCall(*call)) {
    return false;
  }
  return callee == nullptr || !callee->isIntrinsic();
}

bool functionMetadataHasRegister(const llvm::Function &function,
                                 llvm::StringRef metadataName,
                                 llvm::StringRef registerName) {
  llvm::MDNode *node = function.getMetadata(metadataName);
  if (node == nullptr) {
    return false;
  }
  std::string expected = ("name=" + registerName).str();
  for (const llvm::MDOperand &operand : node->operands()) {
    auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(operand.get());
    if (entry == nullptr) {
      continue;
    }
    for (const llvm::MDOperand &fieldOperand : entry->operands()) {
      auto *field = llvm::dyn_cast_or_null<llvm::MDString>(fieldOperand.get());
      if (field != nullptr && field->getString() == expected) {
        return true;
      }
    }
  }
  return false;
}

bool recoveredPrototypeReturnsRegister(const llvm::Function &function,
                                       llvm::StringRef registerName) {
  llvm::MDNode *prototype = function.getMetadata("notdec.prototype.recovered");
  if (prototype == nullptr || prototype->getNumOperands() != 5) {
    return false;
  }
  auto *returns = llvm::dyn_cast_or_null<llvm::MDNode>(prototype->getOperand(4));
  if (returns == nullptr) {
    return false;
  }
  std::string expectedName = ("name=" + registerName).str();
  for (const llvm::MDOperand &operand : returns->operands()) {
    auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(operand.get());
    if (entry == nullptr) {
      continue;
    }
    bool isRegister = false;
    bool hasName = false;
    for (const llvm::MDOperand &fieldOperand : entry->operands()) {
      auto *field = llvm::dyn_cast_or_null<llvm::MDString>(fieldOperand.get());
      if (field == nullptr) {
        continue;
      }
      isRegister |= field->getString() == "storage=register";
      hasName |= field->getString() == expectedName;
    }
    if (isRegister && hasName) {
      return true;
    }
  }
  return false;
}

bool functionHasRecoveredReturns(const llvm::Function &function) {
  llvm::MDNode *prototype = function.getMetadata("notdec.prototype.recovered");
  if (prototype == nullptr || prototype->getNumOperands() != 5) {
    return false;
  }
  auto *returns = llvm::dyn_cast_or_null<llvm::MDNode>(prototype->getOperand(4));
  return returns != nullptr && returns->getNumOperands() != 0;
}

// FunctionPromoter owns the per-function SSA caches. It follows the on-demand
// SSA construction shape: a read asks for the reaching value, predecessor reads
// create PHIs only when needed, and unresolved external reads become one
// function-entry value with metadata.
class FunctionPromoter {
public:
  FunctionPromoter(llvm::Function &function,
                   std::map<llvm::GlobalVariable *, RegisterUnit> &units,
                   const AbiRegisterEffects &abiEffects,
                   bool enableRewrite,
                   NativeHeritageSSAFunctionSummary &summary)
      : Function(function), Units(units), AbiEffects(abiEffects), EnableRewrite(enableRewrite),
        Summary(summary) {}

  void run() {
    Summary.FunctionName = Function.getName().str();
    for (llvm::BasicBlock &block : Function) {
      scanBlock(block);
    }

    if (EnableRewrite) {
      rewritePartialStores();
      attachCallInputCandidates();
      rewriteLoads();
      annotateCallInputTrials();
      removeLocalDeadStores();
      removeUnreadFlagStores();
      removeUnreadRipStores();
      attachRegisterEffectMetadata();
      removeDeadExternalInputs();
      finalizePendingPhis();
      eraseDeadPhis();
      eraseUnusedPendingPhis();
    } else {
      attachCallInputCandidates();
      collectExternalInputsOnly();
      annotateCallInputTrials();
      attachRegisterEffectMetadata();
      finalizePendingPhis();
      eraseUnusedPendingPhis();
    }
    attachExternalInputMetadata();
  }

private:
  void scanBlock(llvm::BasicBlock &block) {
    for (llvm::Instruction &inst : block) {
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        AccessInfo access = registerLoad(*load, Units);
        if (access.Unit != nullptr) {
          ++Summary.LoadsSeen;
          LoadedUnits.insert(access.Unit->Global);
          if (access.IsStorageValue || canPromotePartialAccess(access)) {
            Loads.push_back(load);
          }
        }
        continue;
      }

      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        AccessInfo access = registerStore(*store, Units);
        if (access.Unit != nullptr) {
          ++Summary.StoresSeen;
          if (access.IsStorageValue) {
            StoredFullUnits.insert(access.Unit->Global);
          }
        }
        continue;
      }

      if (isRegisterClobberCall(inst)) {
        ++Summary.CallsSeen;
        HasCall.insert(&block);
        uint64_t index = CallsiteIds.size();
        CallsiteIds.try_emplace(
            &inst, CallsiteInfo{
                       index,
                       Function.getName().str() + ":" + std::to_string(index)});
      }
    }
  }

  void rewriteLoads() {
    for (llvm::LoadInst *load : Loads) {
      AccessInfo access = registerLoad(*load, Units);
      if (access.Unit == nullptr ||
          (!access.IsStorageValue && !canPromotePartialAccess(access))) {
        continue;
      }

      llvm::Value *value = readRegister(*load->getParent(), *access.Unit, load);
      if (value == nullptr || value == load) {
        continue;
      }
      if (!access.IsFullUnit) {
        value = extractPartialValue(access, value, load);
        llvm::IRBuilder<> builder(load);
        value = resizeInteger(builder, value,
                              llvm::cast<llvm::IntegerType>(load->getType()));
      }
      Replacement[load] = value;
      load->replaceAllUsesWith(value);
      PendingErase.push_back(load);
      ++Summary.LoadsReplaced;
    }

    for (llvm::Instruction *inst : PendingErase) {
      inst->eraseFromParent();
    }
  }

  void collectExternalInputsOnly() {
    for (llvm::LoadInst *load : Loads) {
      AccessInfo access = registerLoad(*load, Units);
      if (access.Unit == nullptr ||
          (!access.IsStorageValue && !canPromotePartialAccess(access))) {
        continue;
      }
      (void)readRegister(*load->getParent(), *access.Unit, load);
    }
  }

  void rewritePartialStores() {
    std::vector<llvm::StoreInst *> stores;
    for (llvm::BasicBlock &block : Function) {
      for (llvm::Instruction &inst : block) {
        auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
        if (store == nullptr) {
          continue;
        }
        AccessInfo access = registerStore(*store, Units);
        if (canPromotePartialAccess(access)) {
          stores.push_back(store);
        }
      }
    }

    for (llvm::StoreInst *store : stores) {
      AccessInfo access = registerStore(*store, Units);
      if (!canPromotePartialAccess(access)) {
        continue;
      }
      llvm::Value *oldValue =
          readRegister(*store->getParent(), *access.Unit, store);
      if (oldValue == nullptr) {
        continue;
      }
      llvm::Value *newValue =
          replacePartialValue(access, oldValue, store->getValueOperand(), store);
      llvm::IRBuilder<> builder(store);
      llvm::StoreInst *newStore = builder.CreateStore(newValue,
                                                      access.Unit->Global);
      newStore->setMetadata("notdec.register.access",
                            store->getMetadata("notdec.register.access"));
      markSyntheticPartialStore(*newStore);
      StoredFullUnits.insert(access.Unit->Global);
      Replacement[store] = newStore;
      store->eraseFromParent();
    }
  }

  void removeLocalDeadStores() {
    std::vector<llvm::Instruction *> deadStores;
    for (llvm::BasicBlock &block : Function) {
      std::map<llvm::GlobalVariable *, std::vector<llvm::StoreInst *>>
          pendingStores;
      for (llvm::Instruction &inst : block) {
        if (inst.isTerminator()) {
          pendingStores.clear();
          continue;
        }
        if (isRegisterClobberCall(inst)) {
          pendingStores.clear();
          continue;
        }
        if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
          AccessInfo access = registerLoad(*load, Units);
          if (access.Unit != nullptr && access.IsRegisterAccess) {
            pendingStores.erase(access.Unit->Global);
          }
          continue;
        }
        auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
        if (store == nullptr) {
          continue;
        }
        AccessInfo access = registerStore(*store, Units);
        if (access.Unit == nullptr || !access.IsRegisterAccess) {
          continue;
        }
        std::vector<llvm::StoreInst *> &stores =
            pendingStores[access.Unit->Global];
        if (access.IsStorageValue) {
          deadStores.insert(deadStores.end(), stores.begin(), stores.end());
          stores.clear();
        }
        stores.push_back(store);
      }
    }

    std::set<llvm::Instruction *> uniqueDeadStores(deadStores.begin(),
                                                   deadStores.end());
    for (llvm::Instruction *inst : uniqueDeadStores) {
      if (inst->use_empty()) {
        auto *store = llvm::cast<llvm::StoreInst>(inst);
        llvm::Value *storedValue = store->getValueOperand();
        inst->eraseFromParent();
        llvm::RecursivelyDeleteTriviallyDeadInstructions(
            storedValue, nullptr, nullptr,
            [this](llvm::Value *value) { forgetExternalInputValue(value); });
        ++Summary.DeadStoresRemoved;
      }
    }
  }

  void removeUnreadFlagStores() {
    std::map<llvm::BasicBlock *, FlagBlockLiveness> blockLiveness;
    bool changed = true;
    while (changed) {
      changed = false;
      for (llvm::BasicBlock &block : llvm::reverse(Function)) {
        std::set<llvm::GlobalVariable *> liveOut;
        for (llvm::BasicBlock *successor : llvm::successors(&block)) {
          const auto &successorLiveIn = blockLiveness[successor].LiveIn;
          liveOut.insert(successorLiveIn.begin(), successorLiveIn.end());
        }

        std::set<llvm::GlobalVariable *> liveIn =
            flagLiveBeforeBlock(block, liveOut);
        FlagBlockLiveness &liveness = blockLiveness[&block];
        if (liveness.LiveOut != liveOut || liveness.LiveIn != liveIn) {
          liveness.LiveOut = std::move(liveOut);
          liveness.LiveIn = std::move(liveIn);
          changed = true;
        }
      }
    }

    std::vector<llvm::StoreInst *> deadStores;
    for (llvm::BasicBlock &block : Function) {
      std::set<llvm::GlobalVariable *> live =
          blockLiveness[&block].LiveOut;
      for (llvm::Instruction &inst : llvm::reverse(block)) {
        if (isRegisterClobberCall(inst)) {
          eraseClobberedFlagsFromLiveSet(inst, live);
          continue;
        }
        if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
          AccessInfo access = registerLoad(*load, Units);
          if (access.Unit != nullptr && isFlagRegisterName(access.Unit->Name)) {
            live.insert(access.Unit->Global);
          }
          continue;
        }
        auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
        if (store == nullptr) {
          continue;
        }
        AccessInfo access = registerStore(*store, Units);
        if (access.Unit == nullptr || !isFlagRegisterName(access.Unit->Name)) {
          continue;
        }
        if (live.count(access.Unit->Global) == 0) {
          deadStores.push_back(store);
        } else {
          live.erase(access.Unit->Global);
        }
      }
    }

    std::set<llvm::GlobalVariable *> touchedFlags;
    for (llvm::StoreInst *store : deadStores) {
      AccessInfo access = registerStore(*store, Units);
      if (access.Unit != nullptr) {
        touchedFlags.insert(access.Unit->Global);
      }
      llvm::Value *storedValue = store->getValueOperand();
      if (store->use_empty()) {
        store->eraseFromParent();
        llvm::RecursivelyDeleteTriviallyDeadInstructions(
            storedValue, nullptr, nullptr,
            [this](llvm::Value *value) { forgetExternalInputValue(value); });
        ++Summary.UnreadFlagStoresRemoved;
      }
    }
    removeDeadFlagExternalInputUsers();
    for (llvm::GlobalVariable *global : touchedFlags) {
      if (!hasRemainingStorageStore(*global)) {
        StoredFullUnits.erase(global);
      }
    }
  }

  std::set<llvm::GlobalVariable *> flagLiveBeforeBlock(
      llvm::BasicBlock &block,
      const std::set<llvm::GlobalVariable *> &liveOut) {
    std::set<llvm::GlobalVariable *> live = liveOut;
    for (llvm::Instruction &inst : llvm::reverse(block)) {
      if (isRegisterClobberCall(inst)) {
        eraseClobberedFlagsFromLiveSet(inst, live);
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        AccessInfo access = registerLoad(*load, Units);
        if (access.Unit != nullptr && isFlagRegisterName(access.Unit->Name)) {
          live.insert(access.Unit->Global);
        }
        continue;
      }
      auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
      if (store == nullptr) {
        continue;
      }
      AccessInfo access = registerStore(*store, Units);
      if (access.Unit != nullptr && isFlagRegisterName(access.Unit->Name)) {
        live.erase(access.Unit->Global);
      }
    }
    return live;
  }

  void eraseClobberedFlagsFromLiveSet(
      const llvm::Instruction &inst,
      std::set<llvm::GlobalVariable *> &live) const {
    for (auto &[global, unit] : Units) {
      if (isFlagRegisterName(unit.Name) && callClobbersRegister(inst, unit)) {
        live.erase(global);
      }
    }
  }

  bool hasRemainingStorageStore(llvm::GlobalVariable &global) {
    for (llvm::BasicBlock &block : Function) {
      for (llvm::Instruction &inst : block) {
        auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
        if (store == nullptr) {
          continue;
        }
        AccessInfo access = registerStore(*store, Units);
        if (access.Unit != nullptr && access.Unit->Global == &global &&
            access.IsStorageValue) {
          return true;
        }
      }
    }
    return false;
  }

  void forgetExternalInputValue(llvm::Value *value) {
    for (auto it = ExternalInputValue.begin(); it != ExternalInputValue.end();) {
      if (it->second == value) {
        ExternalInputs.erase(it->first);
        it = ExternalInputValue.erase(it);
      } else {
        ++it;
      }
    }
  }

  void removeDeadExternalInputs() {
    std::vector<llvm::LoadInst *> deadLoads;
    for (auto &[global, value] : ExternalInputValue) {
      auto *load = llvm::dyn_cast_or_null<llvm::LoadInst>(value);
      if (load != nullptr && load->use_empty()) {
        deadLoads.push_back(load);
      }
    }
    for (llvm::LoadInst *load : deadLoads) {
      forgetExternalInputValue(load);
      load->eraseFromParent();
    }
  }

  void removeDeadFlagExternalInputUsers() {
    std::vector<llvm::Instruction *> deadUsers;
    for (llvm::BasicBlock &block : Function) {
      for (llvm::Instruction &inst : block) {
        if (inst.use_empty() && valueDependsOnFlagExternalInput(inst)) {
          deadUsers.push_back(&inst);
        }
      }
    }
    for (llvm::Instruction *inst : deadUsers) {
      if (inst->getParent() != nullptr && inst->use_empty()) {
        llvm::RecursivelyDeleteTriviallyDeadInstructions(
            inst, nullptr, nullptr,
            [this](llvm::Value *value) { forgetExternalInputValue(value); });
      }
    }
  }

  bool valueDependsOnFlagExternalInput(llvm::Value &value) {
    std::set<llvm::Value *> seen;
    return valueDependsOnFlagExternalInput(value, seen);
  }

  bool valueDependsOnFlagExternalInput(llvm::Value &value,
                                       std::set<llvm::Value *> &seen) {
    if (!seen.insert(&value).second) {
      return false;
    }
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&value)) {
      if (load->getMetadata("notdec.register.external_input") != nullptr) {
        AccessInfo access = registerLoad(*load, Units);
        return access.Unit != nullptr && isFlagRegisterName(access.Unit->Name);
      }
    }
    auto *inst = llvm::dyn_cast<llvm::Instruction>(&value);
    if (inst == nullptr) {
      return false;
    }
    for (llvm::Value *operand : inst->operands()) {
      if (valueDependsOnFlagExternalInput(*operand, seen)) {
        return true;
      }
    }
    return false;
  }

  bool valueDependsOnRegisterExternalInput(llvm::Value &value) {
    std::set<llvm::Value *> seen;
    return valueDependsOnRegisterExternalInput(value, seen);
  }

  bool valueDependsOnRegisterExternalInput(llvm::Value &value,
                                           std::set<llvm::Value *> &seen) {
    if (!seen.insert(&value).second) {
      return false;
    }
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&value)) {
      if (load->getMetadata("notdec.register.external_input") != nullptr) {
        AccessInfo access = registerLoad(*load, Units);
        return access.Unit != nullptr;
      }
    }
    auto *inst = llvm::dyn_cast<llvm::Instruction>(&value);
    if (inst == nullptr) {
      return false;
    }
    for (llvm::Value *operand : inst->operands()) {
      if (valueDependsOnRegisterExternalInput(*operand, seen)) {
        return true;
      }
    }
    return false;
  }

  llvm::Value *resizeInteger(llvm::IRBuilder<> &builder, llvm::Value *value,
                             llvm::IntegerType *targetType) {
    auto *valueType = llvm::dyn_cast<llvm::IntegerType>(value->getType());
    if (valueType == nullptr || valueType == targetType) {
      return value;
    }
    if (valueType->getBitWidth() < targetType->getBitWidth()) {
      return builder.CreateZExt(value, targetType);
    }
    return builder.CreateTrunc(value, targetType);
  }

  llvm::Value *extractPartialValue(const AccessInfo &access,
                                   llvm::Value *baseValue,
                                   llvm::Instruction *before) {
    std::optional<uint32_t> shift = bitOffset(access);
    if (!shift) {
      return baseValue;
    }
    llvm::IRBuilder<> builder(before);
    auto *baseType = llvm::cast<llvm::IntegerType>(baseValue->getType());
    llvm::Value *shifted = baseValue;
    if (*shift != 0) {
      shifted = builder.CreateLShr(
          baseValue, llvm::ConstantInt::get(baseType, *shift));
    }
    auto *targetType = llvm::IntegerType::get(Function.getContext(),
                                             bitWidth(access.Size));
    if (baseType == targetType) {
      return shifted;
    }
    return builder.CreateTrunc(shifted, targetType);
  }

  llvm::Value *replacePartialValue(const AccessInfo &access,
                                   llvm::Value *oldValue,
                                   llvm::Value *partialValue,
                                   llvm::Instruction *before) {
    std::optional<uint32_t> shift = bitOffset(access);
    if (!shift) {
      return oldValue;
    }
    llvm::IRBuilder<> builder(before);
    auto *baseType = llvm::cast<llvm::IntegerType>(oldValue->getType());
    llvm::Value *resized = resizeInteger(builder, partialValue, baseType);
    unsigned baseBits = baseType->getBitWidth();
    unsigned accessBits = bitWidth(access.Size);
    llvm::APInt mask = llvm::APInt::getLowBitsSet(baseBits, accessBits)
                           .shl(*shift);
    llvm::Value *cleared =
        builder.CreateAnd(oldValue, llvm::ConstantInt::get(baseType, ~mask));
    llvm::Value *shifted = resized;
    if (*shift != 0) {
      shifted =
          builder.CreateShl(resized, llvm::ConstantInt::get(baseType, *shift));
    }
    llvm::Value *masked =
        builder.CreateAnd(shifted, llvm::ConstantInt::get(baseType, mask));
    return builder.CreateOr(cleared, masked);
  }

  void markSyntheticPartialStore(llvm::StoreInst &store) {
    llvm::LLVMContext &context = Function.getContext();
    llvm::Metadata *fields[] = {
        llvm::MDString::get(context, "partial_storage_ssa"),
    };
    store.setMetadata("notdec.register.synthetic",
                      llvm::MDNode::get(context, fields));
  }

  void removeUnreadRipStores() {
    bool hasRipLoad = false;
    for (llvm::GlobalVariable *global : LoadedUnits) {
      auto it = Units.find(global);
      if (it != Units.end() && isInstructionPointerName(it->second.Name)) {
        hasRipLoad = true;
        break;
      }
    }
    if (hasRipLoad) {
      return;
    }

    std::vector<llvm::StoreInst *> deadStores;
    for (llvm::BasicBlock &block : Function) {
      for (llvm::Instruction &inst : block) {
        auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
        if (store == nullptr) {
          continue;
        }
        AccessInfo access = registerStore(*store, Units);
        if (access.Unit != nullptr &&
            isInstructionPointerName(access.Unit->Name)) {
          deadStores.push_back(store);
        }
      }
    }

    for (llvm::StoreInst *store : deadStores) {
      if (store->use_empty()) {
        store->eraseFromParent();
        ++Summary.UnreadRipStoresRemoved;
      }
    }
  }

  llvm::Value *readRegister(llvm::BasicBlock &block, RegisterUnit &unit,
                            llvm::Instruction *before) {
    llvm::Value *local = localValueBefore(block, unit, before);
    if (local != nullptr) {
      return local;
    }
    return readBlockEntry(block, unit);
  }

  llvm::Value *localValueBefore(llvm::BasicBlock &block, RegisterUnit &unit,
                                llvm::Instruction *before) {
    for (llvm::Instruction &inst : llvm::reverse(block)) {
      if (&inst == before) {
        continue;
      }
      if (before != nullptr && !inst.comesBefore(before)) {
        continue;
      }
      if (isRegisterClobberCall(inst)) {
        if (std::optional<CallEffectInfo> effect =
                callEffectKind(inst, unit)) {
          return callEffectValue(inst, unit, *effect);
        }
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        AccessInfo access = registerLoad(*load, Units);
        if (access.Unit == &unit && access.IsStorageValue) {
          return resolveValue(load);
        }
        continue;
      }
      auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
      if (store == nullptr) {
        continue;
      }
      AccessInfo access = registerStore(*store, Units);
      if (access.Unit == &unit && access.IsStorageValue) {
        return resolveValue(store->getValueOperand());
      }
      if (access.Unit == &unit && canPromotePartialAccess(access)) {
        llvm::Value *oldValue = readRegister(block, unit, store);
        if (oldValue == nullptr) {
          return nullptr;
        }
        return replacePartialValue(access, oldValue, store->getValueOperand(),
                                   before);
      }
    }
    return nullptr;
  }

  void attachCallInputCandidates() {
    if (AbiEffects.Inputs.empty()) {
      return;
    }

    std::map<std::string, RegisterUnit *> inputUnits;
    for (auto &[global, unit] : Units) {
      (void)global;
      inputUnits.emplace(unit.Name, &unit);
    }

    for (llvm::BasicBlock &block : Function) {
      for (llvm::Instruction &inst : block) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
        if (call == nullptr || !isRegisterClobberCall(inst)) {
          continue;
        }
        attachCallInputCandidates(*call, inputUnits);
      }
    }
  }

  void attachCallInputCandidates(
      llvm::CallBase &call,
      const std::map<std::string, RegisterUnit *> &inputUnits) {
    llvm::LLVMContext &context = Function.getContext();
    std::vector<llvm::Metadata *> entries;
    uint64_t slot = 0;
    for (const std::string &registerName : AbiEffects.Inputs) {
      auto unitIt = inputUnits.find(registerName);
      if (unitIt == inputUnits.end()) {
        ++slot;
        continue;
      }
      RegisterUnit &unit = *unitIt->second;
      auto *integerType =
          llvm::dyn_cast<llvm::IntegerType>(unit.Global->getValueType());
      if (integerType == nullptr || integerType->getBitWidth() != 64) {
        ++slot;
        continue;
      }

      llvm::IRBuilder<> builder(&call);
      llvm::LoadInst *load =
          builder.CreateLoad(unit.Global->getValueType(), unit.Global,
                             unit.Name + ".before_call");
      load->setMetadata("notdec.register.access",
                        fullRegisterAccessMetadata(context, unit));
      Loads.push_back(load);
      llvm::Metadata *fields[] = {
          llvm::MDString::get(context,
                              "callsite_id=" + callsiteId(call)),
          llvm::MDString::get(context, "slot=" + std::to_string(slot)),
          llvm::MDString::get(context, "callee=" + directCalleeName(call)),
          llvm::MDString::get(context, "storage=register"),
          llvm::MDString::get(context, "base=" + unit.Name),
          llvm::MDString::get(context, "space=register"),
          llvm::MDString::get(context, "offset=" + std::to_string(unit.Offset)),
          llvm::MDString::get(context, "size=" + std::to_string(unit.Size)),
          llvm::MDString::get(context, "register=" + unit.Name),
          llvm::ValueAsMetadata::get(unit.Global),
      };
      llvm::CallInst *candidate =
          builder.CreateCall(callInputHelper(unit), {load});
      candidate->setMetadata("notdec.register.call_input_candidate",
                             llvm::MDNode::get(context, fields));
      ++Summary.CallInputHelpers;
      entries.push_back(llvm::MDNode::get(context, fields));
      ++slot;
    }

    if (entries.empty()) {
      call.setMetadata("notdec.register.call_input_candidates", nullptr);
      return;
    }
    call.setMetadata("notdec.register.call_input_candidates",
                     llvm::MDNode::get(context, entries));
  }

  void annotateCallInputTrials() {
    std::vector<CallInputTrialRecord> trials;
    for (llvm::BasicBlock &block : Function) {
      for (llvm::Instruction &inst : block) {
        llvm::MDNode *metadata =
            inst.getMetadata("notdec.register.call_input_candidate");
        if (metadata == nullptr) {
          continue;
        }
        llvm::Value *candidateValue = &inst;
        if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
          if (isNotDecRegisterHelperCall(*call) && call->arg_size() == 1) {
            candidateValue = call->getArgOperand(0);
          }
        }
        CallInputTrialContext trialContext;
        trialContext.Candidate = &inst;
        trialContext.CallsiteId = mdField(metadata, "callsite_id").value_or("");
        trialContext.RegisterName = mdField(metadata, "register").value_or("");
        trialContext.CalleeName = mdField(metadata, "callee").value_or("");
        trials.push_back(
            CallInputTrialRecord{&inst, candidateValue, metadata, trialContext});
      }
    }

    for (CallInputTrialRecord &record : trials) {
      std::set<llvm::Value *> visiting;
      record.Info =
          callInputTrialInfo(record.CandidateValue, record.Context, visiting);
      record.Checked = true;
    }

    finalizeCallInputTrials(trials);

    for (CallInputTrialRecord &record : trials) {
      llvm::MDNode *trialMetadata = withMetadataField(
          *withMetadataField(
              *withMetadataField(*record.Metadata, "strength",
                                 record.Info.Strength),
              "trial_state", record.Info.State),
          "trial_reason", record.Info.Reason);
      if (!record.Info.DoubleUseTargetCallsiteId.empty()) {
        trialMetadata =
            withMetadataField(*trialMetadata, "double_use_target_callsite_id",
                              record.Info.DoubleUseTargetCallsiteId);
      }
      if (!record.Info.DoubleUseTargetCallee.empty()) {
        trialMetadata =
            withMetadataField(*trialMetadata, "double_use_target_callee",
                              record.Info.DoubleUseTargetCallee);
      }
      if (!record.Info.DoubleUseTargetRegister.empty()) {
        trialMetadata =
            withMetadataField(*trialMetadata, "double_use_target_register",
                              record.Info.DoubleUseTargetRegister);
      }
      if (!record.Info.DoubleUseTargetState.empty()) {
        trialMetadata =
            withMetadataField(*trialMetadata, "double_use_target_state",
                              record.Info.DoubleUseTargetState);
      }
      if (!record.Info.PathFlags.empty()) {
        trialMetadata =
            withMetadataField(*trialMetadata, "trial_path_flags",
                              callInputTrialFlagsText(record.Info.PathFlags));
      }
      if (!record.Info.Flags.empty()) {
        trialMetadata =
            withMetadataField(*trialMetadata, "trial_flags",
                              callInputTrialFlagsText(record.Info.Flags));
      }
      record.Candidate->setMetadata("notdec.register.call_input_candidate",
                                    trialMetadata);
      countCallInputStrength(record.Info.Strength);
      countCallInputTrialState(record.Info.State);
      countCallInputTrialReason(record.Info.Reason);
      countCallInputTrialFlags(record.Info.Flags);
    }

    for (llvm::BasicBlock &block : Function) {
      for (llvm::Instruction &inst : block) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
        if (call == nullptr || !isRegisterClobberCall(inst)) {
          continue;
        }
        refreshCallInputCandidateList(*call);
      }
    }
  }

  void finalizeCallInputTrials(std::vector<CallInputTrialRecord> &trials) {
    std::map<llvm::Instruction *, CallInputTrialRecord *> trialByCandidate;
    for (CallInputTrialRecord &record : trials) {
      if (record.Candidate != nullptr) {
        trialByCandidate.emplace(record.Candidate, &record);
      }
    }

    for (CallInputTrialRecord &record : trials) {
      if (!record.Checked) {
        continue;
      }
      if (record.Info.DoubleUseCandidate != nullptr) {
        auto found = trialByCandidate.find(record.Info.DoubleUseCandidate);
        if (found != trialByCandidate.end()) {
          record.DoubleUseRecord = found->second;
        }
      }
    }

    // Finalization has cross-trial dependencies.  Conditional checks can
    // downgrade a trial that another double-use check just read, while a
    // double-use check can restore a conditional trial.  Keep this bounded:
    // current rules are monotonic toward inactive/no_use after the before-state
    // restore has fired once, so a small fixed limit exposes bugs without
    // risking an unbounded pass.
    for (unsigned iteration = 0; iteration < 4; ++iteration) {
      std::vector<std::string> before = callInputTrialSignatures(trials);
      for (CallInputTrialRecord &record : trials) {
        if (!record.Checked) {
          continue;
        }
        applyConditionalCallInputFinalCheck(record.Info);
      }
      for (CallInputTrialRecord &record : trials) {
        if (!record.Checked) {
          continue;
        }
        applyDoubleUseFinalCheck(record);
      }
      if (before == callInputTrialSignatures(trials)) {
        break;
      }
    }

    for (CallInputTrialRecord &record : trials) {
      if (!record.Checked) {
        continue;
      }
      applyConditionalCallInputFinalCheck(record.Info);
      addCallInputPathFlag(record.Info);
    }
  }

  std::vector<std::string>
  callInputTrialSignatures(const std::vector<CallInputTrialRecord> &trials)
      const {
    std::vector<std::string> signatures;
    signatures.reserve(trials.size());
    for (const CallInputTrialRecord &record : trials) {
      signatures.push_back(callInputTrialSignature(record.Info));
    }
    return signatures;
  }

  std::string callInputTrialSignature(const CallInputTrialInfo &info) const {
    return info.Strength + "|" + info.State + "|" + info.Reason + "|" +
           callInputTrialFlagsText(info.Flags) + "|" +
           callInputTrialFlagsText(info.PathFlags) + "|" +
           info.DoubleUseTargetCallsiteId + "|" + info.DoubleUseTargetCallee +
           "|" + info.DoubleUseTargetRegister + "|" +
           info.DoubleUseTargetState;
  }

  void applyDoubleUseFinalCheck(CallInputTrialRecord &record) const {
    if (record.Info.Reason != "local_double_call_use") {
      return;
    }
    if (!record.Info.HasBeforeDoubleUseInfo) {
      addCallInputTrialFlag(record.Info, "double_use_no_before_state");
    }
    if (record.DoubleUseRecord == nullptr) {
      recordDoubleUseTargetFields(record.Info, record.Info.DoubleUseCandidate,
                                  "");
      addCallInputTrialFlag(record.Info, "double_use_target_unresolved");
      return;
    }
    recordDoubleUseTargetFields(record.Info, record.DoubleUseRecord->Candidate,
                                record.DoubleUseRecord->Info.State);
    if (isSameTargetEarlierDoubleUse(record)) {
      addCallInputTrialFlag(record.Info, "double_use_same_target_earlier");
      restoreBeforeDoubleUseState(record.Info);
      return;
    }
    if (!record.DoubleUseRecord->Checked) {
      if (isNativeAlternatePathValid(record.Info.Path)) {
        addCallInputTrialFlag(record.Info, "double_use_alternate_path_valid");
      }
      addCallInputTrialFlag(record.Info, "double_use_target_unchecked");
      return;
    }
    if (record.DoubleUseRecord->Info.State == "active") {
      addCallInputTrialFlag(record.Info, "double_use_target_active");
      return;
    }
    addCallInputTrialFlag(record.Info, "double_use_target_non_active");
    restoreBeforeDoubleUseState(record.Info);
  }

  void restoreBeforeDoubleUseState(CallInputTrialInfo &info) const {
    if (!info.HasBeforeDoubleUseInfo) {
      return;
    }
    info.Strength = info.BeforeDoubleUseStrength;
    info.State = info.BeforeDoubleUseState;
    info.Reason = info.BeforeDoubleUseReason;
    info.Flags = info.BeforeDoubleUseFlags;
    info.PathFlags = info.BeforeDoubleUsePathFlags;
    info.Path = info.BeforeDoubleUsePath;
  }

  bool isSameTargetEarlierDoubleUse(const CallInputTrialRecord &record) const {
    if (record.Context.Candidate == nullptr ||
        record.DoubleUseRecord == nullptr ||
        record.DoubleUseRecord->Candidate == nullptr ||
        record.Context.CalleeName.empty() ||
        record.Context.RegisterName.empty()) {
      return false;
    }
    const CallInputTrialContext &other = record.DoubleUseRecord->Context;
    if (other.CalleeName != record.Context.CalleeName ||
        other.RegisterName != record.Context.RegisterName) {
      return false;
    }
    if (record.Context.Candidate->getParent() !=
        record.DoubleUseRecord->Candidate->getParent()) {
      return true;
    }
    return instructionComesBefore(*record.Context.Candidate,
                                  *record.DoubleUseRecord->Candidate);
  }

  bool isNativeAlternatePathValid(const CallInputPathInfo &path) const {
    if (path.Indirect && !path.IndirectAlt) {
      return true;
    }
    if (!path.Indirect && path.IndirectAlt) {
      return false;
    }
    if (path.ActionAlt) {
      return true;
    }
    return false;
  }

  void recordDoubleUseTargetFields(CallInputTrialInfo &info,
                                   llvm::Instruction *target,
                                   llvm::StringRef state) const {
    if (target == nullptr) {
      return;
    }
    llvm::MDNode *metadata =
        target->getMetadata("notdec.register.call_input_candidate");
    if (metadata == nullptr) {
      return;
    }
    info.DoubleUseTargetCallsiteId =
        mdField(metadata, "callsite_id").value_or("");
    info.DoubleUseTargetCallee = mdField(metadata, "callee").value_or("");
    info.DoubleUseTargetRegister = mdField(metadata, "register").value_or("");
    info.DoubleUseTargetState = state.str();
  }

  void refreshCallInputCandidateList(llvm::CallBase &call) {
    llvm::LLVMContext &context = Function.getContext();
    std::vector<llvm::Metadata *> entries;
    for (auto iter = llvm::BasicBlock::reverse_iterator(call.getIterator()),
              end = call.getParent()->rend();
         iter != end; ++iter) {
      auto *candidateCall = llvm::dyn_cast<llvm::CallBase>(&*iter);
      if (candidateCall == nullptr ||
          !isNotDecRegisterHelperCall(*candidateCall)) {
        break;
      }
      llvm::MDNode *metadata =
          candidateCall->getMetadata("notdec.register.call_input_candidate");
      if (metadata != nullptr) {
        entries.push_back(metadata);
      }
    }
    std::reverse(entries.begin(), entries.end());
    if (entries.empty()) {
      call.setMetadata("notdec.register.call_input_candidates", nullptr);
      return;
    }
    call.setMetadata("notdec.register.call_input_candidates",
                     llvm::MDNode::get(context, entries));
  }

  llvm::MDNode *withMetadataField(llvm::MDNode &metadata, llvm::StringRef key,
                                  llvm::StringRef value) {
    llvm::LLVMContext &context = Function.getContext();
    std::string prefix = (key + "=").str();
    std::vector<llvm::Metadata *> fields;
    for (const llvm::MDOperand &operand : metadata.operands()) {
      auto *field = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
      if (field != nullptr && field->getString().starts_with(prefix)) {
        continue;
      }
      fields.push_back(operand.get());
    }
    fields.push_back(llvm::MDString::get(context,
                                         prefix + value.str()));
    return llvm::MDNode::get(context, fields);
  }

  std::string
  callInputTrialFlagsText(const std::vector<std::string> &flags) const {
    std::string text;
    for (llvm::StringRef flag : flags) {
      if (!text.empty()) {
        text += ",";
      }
      text += flag.str();
    }
    return text;
  }

  void addCallInputPathFlag(CallInputTrialInfo &trial) const {
    if (callInputTrialHasFlag(trial, "conditional_effect")) {
      addCallInputTrialFlag(trial, "path_conditional");
      return;
    }
    if (trial.State == "active") {
      addCallInputTrialFlag(trial, "path_realistic");
      return;
    }
    addCallInputTrialFlag(trial, "path_blocked");
  }

  bool callInputTrialHasFlag(const CallInputTrialInfo &trial,
                             llvm::StringRef expected) const {
    return llvm::is_contained(trial.Flags, expected);
  }

  void addCallInputTrialFlag(CallInputTrialInfo &trial,
                             llvm::StringRef flag) const {
    if (!callInputTrialHasFlag(trial, flag)) {
      trial.Flags.push_back(flag.str());
    }
  }

  void applyConditionalCallInputFinalCheck(CallInputTrialInfo &trial) const {
    if (trial.State != "active" ||
        !callInputTrialHasFlag(trial, "conditional_effect")) {
      return;
    }
    trial.State = "no_use";
    addCallInputTrialFlag(trial, "final_checked");
  }

  CallInputTrialInfo callInputTrialInfo(
      llvm::Value *value, const CallInputTrialContext &context,
      std::set<llvm::Value *> &visiting) {
    value = resolveValue(value);
    if (value == nullptr || !visiting.insert(value).second) {
      return CallInputTrialInfo{"weak_entry_input", "inactive",
                                "entry_input"};
    }
    if (llvm::isa<llvm::Constant>(value)) {
      return CallInputTrialInfo{"strong_local_def", "active", "local_const"};
    }
    if (llvm::isa<llvm::Argument>(value)) {
      return CallInputTrialInfo{"weak_entry_input", "inactive",
                                "entry_input"};
    }
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(value)) {
      if (load->getMetadata("notdec.register.external_input") != nullptr) {
        return CallInputTrialInfo{"weak_entry_input", "inactive",
                                  "entry_input"};
      }
      return CallInputTrialInfo{"weak_entry_input", "inactive", "local_load"};
    }
    if (auto *phi = llvm::dyn_cast<llvm::PHINode>(value)) {
      CallInputTrialInfo trial = phiInputTrialInfo(*phi, context, visiting);
      if (trial.State == "active") {
        CallInputTrialInfo useTrial = callInputUseTrialInfo(*phi, context);
        if (useTrial.State != "active") {
          return withBeforeDoubleUseInfo(useTrial, trial);
        }
      }
      return trial;
    }
    if (auto *inst = llvm::dyn_cast<llvm::Instruction>(value)) {
      if (llvm::MDNode *effect =
              inst->getMetadata("notdec.register.call_effect")) {
        std::optional<std::string> kind = mdField(effect, "kind");
        if (kind == std::optional<std::string>("return")) {
          return CallInputTrialInfo{"return_forward", "inactive",
                                    "return_forward"};
        }
        return CallInputTrialInfo{
            "blocked_call_effect", "no_use", "call_effect",
            {"definitely_not_used", "killed_by_call"}};
      }
      if (inst->getFunction() == &Function) {
        if (isSafeCallInputArithmetic(*inst)) {
          if (valueDependsOnRegisterExternalInput(*inst)) {
            return CallInputTrialInfo{"weak_entry_input", "inactive",
                                      "entry_input"};
          }
          CallInputTrialInfo useTrial = callInputUseTrialInfo(*inst, context);
          if (useTrial.State != "active") {
            return withBeforeDoubleUseInfo(
                useTrial,
                CallInputTrialInfo{"strong_local_def", "active",
                                   "local_arith"});
          }
          return CallInputTrialInfo{"strong_local_def", "active",
                                    "local_arith"};
        }
        if (isSafeCallInputCastOrAddress(*inst)) {
          if (valueDependsOnRegisterExternalInput(*inst)) {
            return CallInputTrialInfo{"weak_entry_input", "inactive",
                                      "entry_input"};
          }
          CallInputTrialInfo useTrial = callInputUseTrialInfo(*inst, context);
          if (useTrial.State != "active") {
            return withBeforeDoubleUseInfo(
                useTrial,
                CallInputTrialInfo{"strong_local_def", "active",
                                   "local_cast"});
          }
          return CallInputTrialInfo{"strong_local_def", "active",
                                    "local_cast"};
        }
        return CallInputTrialInfo{"weak_entry_input", "inactive",
                                  "local_unknown_inst"};
      }
    }
    return CallInputTrialInfo{"weak_entry_input", "inactive", "entry_input"};
  }

  CallInputTrialInfo withBeforeDoubleUseInfo(CallInputTrialInfo blocked,
                                             const CallInputTrialInfo &source)
      const {
    if (blocked.Reason != "local_double_call_use") {
      return blocked;
    }
    blocked.HasBeforeDoubleUseInfo = true;
    blocked.BeforeDoubleUseStrength = source.Strength;
    blocked.BeforeDoubleUseState = source.State;
    blocked.BeforeDoubleUseReason = source.Reason;
    blocked.BeforeDoubleUseFlags = source.Flags;
    blocked.BeforeDoubleUsePathFlags = source.PathFlags;
    blocked.BeforeDoubleUsePath = source.Path;
    return blocked;
  }

  bool isSafeCallInputArithmetic(const llvm::Instruction &inst) const {
    switch (inst.getOpcode()) {
    case llvm::Instruction::Add:
    case llvm::Instruction::Sub:
    case llvm::Instruction::Mul:
    case llvm::Instruction::Shl:
    case llvm::Instruction::LShr:
    case llvm::Instruction::AShr:
    case llvm::Instruction::And:
    case llvm::Instruction::Or:
    case llvm::Instruction::Xor:
      return true;
    default:
      return false;
    }
  }

  bool isSafeCallInputCastOrAddress(const llvm::Instruction &inst) const {
    return llvm::isa<llvm::CastInst>(inst) ||
           llvm::isa<llvm::GetElementPtrInst>(inst) ||
           llvm::isa<llvm::ExtractValueInst>(inst) ||
           llvm::isa<llvm::InsertValueInst>(inst);
  }

  CallInputTrialInfo callInputUseTrialInfo(
      llvm::Instruction &value, const CallInputTrialContext &context) {
    CallInputUseCheckInfo useInfo = checkCallInputUses(value, context);
    CallInputTrialInfo trial;
    switch (useInfo.Result) {
    case CallInputUseCheckResult::OnlyCurrentCall:
      trial = CallInputTrialInfo{"strong_local_def", "active", "only_use"};
      break;
    case CallInputUseCheckResult::DoubleCallUse:
      trial = CallInputTrialInfo{"weak_entry_input", "inactive",
                                 "local_double_call_use"};
      trial.DoubleUseCandidate = useInfo.DoubleUseCandidate;
      break;
    case CallInputUseCheckResult::SharedUse:
      trial = CallInputTrialInfo{"weak_entry_input", "inactive",
                                 "local_shared_use"};
      break;
    }
    trial.Path = useInfo.Path;
    trial.PathFlags = pathInfoFlags(useInfo.Path);
    return trial;
  }

  CallInputUseCheckInfo checkCallInputUses(
      llvm::Instruction &value, const CallInputTrialContext &context) {
    std::set<llvm::Instruction *> visiting;
    return checkCallInputUses(value, context, visiting);
  }

  CallInputUseCheckInfo checkCallInputUses(
      llvm::Instruction &value, const CallInputTrialContext &context,
      std::set<llvm::Instruction *> &visiting) {
    if (!visiting.insert(&value).second) {
      CallInputPathInfo path;
      path.SawLoop = true;
      addCallInputPathAuditFlag(path, "path_loop");
      return {CallInputUseCheckResult::OnlyCurrentCall, nullptr, path};
    }
    llvm::Instruction *doubleUseCandidate = nullptr;
    CallInputPathInfo path;
    for (const llvm::Use &use : value.uses()) {
      auto *userInst = llvm::dyn_cast<llvm::Instruction>(use.getUser());
      if (userInst == nullptr) {
        path.SawExternalUse = true;
        addCallInputPathAuditFlag(path, "path_external_use");
        return {CallInputUseCheckResult::SharedUse, nullptr, path};
      }
      if (isSameCallsiteInputHelper(*userInst, context)) {
        continue;
      }
      if (isOtherCallsiteInputHelper(*userInst, context)) {
        if (doubleUseCandidate == nullptr) {
          doubleUseCandidate = userInst;
        }
        path.SawDoubleCallUse = true;
        addCallInputPathAuditFlag(path, "path_double_call_use");
        continue;
      }
      if (isSameRegisterDefinitionStore(*userInst, value, context)) {
        continue;
      }
      if (isTransparentCallInputDescendant(*userInst)) {
        addInstructionPathInfo(path, *userInst);
        CallInputUseCheckInfo result =
            checkCallInputUses(*userInst, context, visiting);
        if (result.Result == CallInputUseCheckResult::SharedUse) {
          mergeCallInputPathInfo(path, result.Path);
          return {CallInputUseCheckResult::SharedUse, nullptr, path};
        }
        if (result.Result == CallInputUseCheckResult::DoubleCallUse &&
            doubleUseCandidate == nullptr) {
          doubleUseCandidate = result.DoubleUseCandidate;
        }
        mergeCallInputPathInfo(path, result.Path);
        continue;
      }
      path.SawBlockedUse = true;
      addCallInputPathAuditFlag(path, "path_blocked");
      return {CallInputUseCheckResult::SharedUse, nullptr, path};
    }
    // A real non-call use beats double-use as the reason.  Only report
    // double-use after the full descendant scan has found no ordinary use.
    if (doubleUseCandidate != nullptr) {
      path.SawDoubleCallUse = true;
      addCallInputPathAuditFlag(path, "path_double_call_use");
      return {CallInputUseCheckResult::DoubleCallUse, doubleUseCandidate, path};
    }
    if (path.AuditFlags.empty()) {
      addCallInputPathAuditFlag(path, "path_realistic");
    }
    return {CallInputUseCheckResult::OnlyCurrentCall, nullptr, path};
  }

  bool isTransparentCallInputDescendant(const llvm::Instruction &inst) const {
    return llvm::isa<llvm::CastInst>(inst) || llvm::isa<llvm::PHINode>(inst) ||
           llvm::isa<llvm::ExtractValueInst>(inst) ||
           llvm::isa<llvm::InsertValueInst>(inst);
  }

  void addInstructionPathInfo(CallInputPathInfo &path,
                              const llvm::Instruction &inst) const {
    if (llvm::isa<llvm::PHINode>(inst)) {
      path.SawPhi = true;
      addCallInputPathAuditFlag(path, "path_phi");
      return;
    }
    if (llvm::isa<llvm::CastInst>(inst)) {
      path.SawCast = true;
      addCallInputPathAuditFlag(path, "path_cast");
      return;
    }
    if (llvm::isa<llvm::ExtractValueInst>(inst) ||
        llvm::isa<llvm::InsertValueInst>(inst)) {
      path.SawAggregate = true;
      addCallInputPathAuditFlag(path, "path_aggregate");
      return;
    }
    addCallInputPathAuditFlag(path, "path_transparent");
  }

  void addCallInputPathAuditFlag(CallInputPathInfo &path,
                                 llvm::StringRef flag) const {
    if (!llvm::is_contained(path.AuditFlags, flag)) {
      path.AuditFlags.push_back(flag.str());
    }
  }

  void mergeCallInputPathInfo(CallInputPathInfo &into,
                              const CallInputPathInfo &from) const {
    into.ActionAlt = into.ActionAlt || from.ActionAlt;
    into.Indirect = into.Indirect || from.Indirect;
    into.IndirectAlt = into.IndirectAlt || from.IndirectAlt;
    into.LsbTruncated = into.LsbTruncated || from.LsbTruncated;
    into.ConcatHigh = into.ConcatHigh || from.ConcatHigh;
    into.SawPhi = into.SawPhi || from.SawPhi;
    into.SawLoop = into.SawLoop || from.SawLoop;
    into.SawUnknown = into.SawUnknown || from.SawUnknown;
    into.SawDoubleCallUse = into.SawDoubleCallUse || from.SawDoubleCallUse;
    into.SawExternalUse = into.SawExternalUse || from.SawExternalUse;
    into.SawBlockedUse = into.SawBlockedUse || from.SawBlockedUse;
    into.SawAggregate = into.SawAggregate || from.SawAggregate;
    into.SawCast = into.SawCast || from.SawCast;
    for (llvm::StringRef flag : from.AuditFlags) {
      addCallInputPathAuditFlag(into, flag);
    }
  }

  std::vector<std::string> pathInfoFlags(const CallInputPathInfo &path) const {
    return path.AuditFlags;
  }

  bool instructionComesBefore(const llvm::Instruction &first,
                              const llvm::Instruction &second) const {
    if (first.getParent() != second.getParent()) {
      return false;
    }
    for (auto iter = first.getIterator(), end = first.getParent()->end();
         iter != end; ++iter) {
      if (&*iter == &second) {
        return true;
      }
    }
    return false;
  }

  bool isSameCallsiteInputHelper(llvm::Instruction &user,
                                 const CallInputTrialContext &context) const {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&user);
    if (call == nullptr || !isNotDecRegisterHelperCall(*call)) {
      return false;
    }
    llvm::MDNode *metadata =
        call->getMetadata("notdec.register.call_input_candidate");
    if (metadata == nullptr) {
      return false;
    }
    if (&user == context.Candidate) {
      return true;
    }
    return !context.CallsiteId.empty() &&
           mdField(metadata, "callsite_id") ==
               std::optional<std::string>(context.CallsiteId);
  }

  bool isOtherCallsiteInputHelper(llvm::Instruction &user,
                                  const CallInputTrialContext &context) const {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&user);
    if (call == nullptr || !isNotDecRegisterHelperCall(*call)) {
      return false;
    }
    llvm::MDNode *metadata =
        call->getMetadata("notdec.register.call_input_candidate");
    if (metadata == nullptr || &user == context.Candidate) {
      return false;
    }
    std::optional<std::string> callsiteId = mdField(metadata, "callsite_id");
    return !context.CallsiteId.empty() && callsiteId.has_value() &&
           *callsiteId != context.CallsiteId;
  }

  bool isSameRegisterDefinitionStore(
      llvm::Instruction &user, const llvm::Instruction &value,
      const CallInputTrialContext &context) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&user);
    if (store == nullptr || store->getValueOperand() != &value ||
        context.RegisterName.empty()) {
      return false;
    }
    AccessInfo access = registerStore(*store, Units);
    return access.Unit != nullptr && access.IsStorageValue &&
           access.Unit->Name == context.RegisterName;
  }

  CallInputTrialInfo phiInputTrialInfo(
      llvm::PHINode &phi, const CallInputTrialContext &context,
      std::set<llvm::Value *> &visiting) {
    bool sawInactive = false;
    bool sawReturn = false;
    bool sawStrong = false;
    std::string inactiveReason = "entry_input";
    std::optional<CallInputTrialInfo> doubleUseTrial;
    for (llvm::Value *incoming : phi.incoming_values()) {
      if (incoming == &phi) {
        continue;
      }
      CallInputTrialInfo trial = callInputTrialInfo(incoming, context, visiting);
      if (trial.State == "no_use") {
        return trial;
      }
      if (trial.State == "inactive") {
        sawInactive = true;
        if (trial.Reason == "return_forward") {
          sawReturn = true;
        } else if (inactiveReason == "entry_input") {
          inactiveReason = trial.Reason;
        }
        if (trial.Reason == "local_double_call_use" &&
            trial.DoubleUseCandidate != nullptr && !doubleUseTrial.has_value()) {
          doubleUseTrial = trial;
        }
      } else {
        sawStrong = true;
      }
    }
    if (sawInactive) {
      if (sawReturn) {
        return CallInputTrialInfo{"return_forward", "inactive",
                                  "return_forward"};
      }
      if (sawStrong) {
        CallInputTrialInfo phiTrial{"strong_phi", "active", "phi",
                                    {"conditional_effect"}};
        // Ghidra's MULTIEQUAL handling can keep following a useful incoming,
        // but double-use is decided later against the other call's trial state.
        // Preserve that incoming-level block here so finalize can make the
        // checked/non-active decision instead of losing the target helper.
        if (doubleUseTrial.has_value()) {
          return withBeforeDoubleUseInfo(*doubleUseTrial, phiTrial);
        }
        return phiTrial;
      }
      if (doubleUseTrial.has_value()) {
        return *doubleUseTrial;
      }
      return CallInputTrialInfo{"weak_entry_input", "inactive",
                                inactiveReason};
    }
    if (sawStrong) {
      return CallInputTrialInfo{"strong_phi", "active", "phi"};
    }
    return CallInputTrialInfo{"weak_entry_input", "inactive", "entry_input"};
  }

  void countCallInputStrength(llvm::StringRef strength) {
    if (strength == "strong_local_def" || strength == "strong_phi") {
      ++Summary.StrongCallInputs;
      return;
    }
    if (strength == "blocked_call_effect") {
      ++Summary.BlockedCallInputs;
      return;
    }
    ++Summary.WeakCallInputs;
  }

  void countCallInputTrialState(llvm::StringRef state) {
    if (state == "active") {
      ++Summary.ActiveCallInputTrials;
      return;
    }
    if (state == "inactive") {
      ++Summary.InactiveCallInputTrials;
      return;
    }
    if (state == "no_use") {
      ++Summary.NoUseCallInputTrials;
      return;
    }
    ++Summary.BlockedCallInputTrials;
  }

  void countCallInputTrialReason(llvm::StringRef reason) {
    if (reason == "local_def") {
      ++Summary.LocalDefCallInputTrials;
      return;
    }
    if (reason == "local_const") {
      ++Summary.LocalConstCallInputTrials;
      return;
    }
    if (reason == "local_arith") {
      ++Summary.LocalArithCallInputTrials;
      return;
    }
    if (reason == "local_cast") {
      ++Summary.LocalCastCallInputTrials;
      return;
    }
    if (reason == "local_load") {
      ++Summary.LocalLoadCallInputTrials;
      return;
    }
    if (reason == "local_unknown_inst") {
      ++Summary.LocalUnknownCallInputTrials;
      return;
    }
    if (reason == "local_shared_use") {
      ++Summary.LocalSharedUseCallInputTrials;
      return;
    }
    if (reason == "local_double_call_use") {
      ++Summary.LocalDoubleCallUseCallInputTrials;
      return;
    }
    if (reason == "phi") {
      ++Summary.PhiCallInputTrials;
      return;
    }
    if (reason == "entry_input") {
      ++Summary.EntryInputCallInputTrials;
      return;
    }
    if (reason == "call_effect") {
      ++Summary.CallEffectCallInputTrials;
      return;
    }
    if (reason == "return_forward") {
      ++Summary.ReturnForwardCallInputTrials;
      return;
    }
  }

  void countCallInputTrialFlags(const std::vector<std::string> &flags) {
    for (llvm::StringRef flag : flags) {
      if (flag == "definitely_not_used") {
        ++Summary.DefinitelyNotUsedCallInputTrials;
      } else if (flag == "killed_by_call") {
        ++Summary.KilledByCallInputTrials;
      } else if (flag == "conditional_effect") {
        ++Summary.ConditionalEffectCallInputTrials;
      } else if (flag == "final_checked") {
        ++Summary.ConditionalFinalCheckCallInputTrials;
      } else if (flag == "path_realistic") {
        ++Summary.PathRealisticCallInputTrials;
      } else if (flag == "path_conditional") {
        ++Summary.PathConditionalCallInputTrials;
      } else if (flag == "path_blocked") {
        ++Summary.PathBlockedCallInputTrials;
      }
    }
  }

  llvm::Value *resolveValue(llvm::Value *value) {
    std::set<llvm::Value *> seen;
    while (value != nullptr && seen.insert(value).second) {
      auto replacement = Replacement.find(value);
      if (replacement == Replacement.end()) {
        return value;
      }
      value = replacement->second;
    }
    return value;
  }

  bool callClobbersRegister(const llvm::Instruction &inst,
                            const RegisterUnit &unit) const {
    return callEffectKind(inst, unit).has_value();
  }

  std::optional<CallEffectInfo> callEffectKind(
      const llvm::Instruction &inst, const RegisterUnit &unit) const {
    if (!AbiEffects.StackPointerRegister.empty() &&
        unit.Name == AbiEffects.StackPointerRegister) {
      return std::nullopt;
    }
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    llvm::Function *callee =
        call == nullptr ? nullptr : call->getCalledFunction();
    bool calleeHasRecoveredReturns =
        callee != nullptr && !callee->isDeclaration() &&
        functionHasRecoveredReturns(*callee);
    if (callee != nullptr && !callee->isDeclaration()) {
      if (functionMetadataHasRegister(*callee, "notdec.register.preserves",
                                      unit.Name)) {
        return std::nullopt;
      }
      if (functionMetadataHasRegister(*callee, "notdec.register.clobbers",
                                      unit.Name)) {
        return CallEffectInfo{"clobber_unknown", "callee_clobbers"};
      }
      if (calleeHasRecoveredReturns &&
          recoveredPrototypeReturnsRegister(*callee, unit.Name)) {
        return CallEffectInfo{"return", "callee_recovered_return"};
      }
    }
    if (AbiEffects.Unaffected.count(unit.Name) != 0) {
      return std::nullopt;
    }
    if (!calleeHasRecoveredReturns && AbiEffects.Outputs.count(unit.Name) != 0) {
      return CallEffectInfo{"return", "abi_output"};
    }
    if (AbiEffects.KilledByCall.count(unit.Name) != 0) {
      return CallEffectInfo{"clobber_unknown", "abi_killedbycall"};
    }
    return CallEffectInfo{"unknown_effect", "abi_unknown"};
  }

  llvm::Value *readBlockEntry(llvm::BasicBlock &block, RegisterUnit &unit) {
    BlockRegKey key{&block, unit.Global};
    auto cached = EntryValue.find(key);
    if (cached != EntryValue.end()) {
      return resolveValue(cached->second);
    }
    if (ResolvingEntry.count(key) != 0) {
      return ensurePhi(block, unit);
    }
    ResolvingEntry.insert(key);

    llvm::pred_iterator predIt = llvm::pred_begin(&block);
    llvm::pred_iterator predEnd = llvm::pred_end(&block);
    if (predIt == predEnd) {
      llvm::Value *input = externalInput(unit);
      EntryValue.emplace(key, input);
      ResolvingEntry.erase(key);
      return input;
    }

    std::vector<llvm::BasicBlock *> preds(predIt, predEnd);

    std::vector<std::pair<llvm::BasicBlock *, llvm::Value *>> incomingValues;
    for (llvm::BasicBlock *pred : preds) {
      llvm::Value *incoming = readBlockExit(*pred, unit);
      incoming = resolveValue(incoming);
      if (incoming == nullptr) {
        ResolvingEntry.erase(key);
        return nullptr;
      }
      incomingValues.push_back({pred, incoming});
    }

    auto pendingPhi = PendingPhi.find(key);
    if (preds.size() == 1 && pendingPhi == PendingPhi.end()) {
      llvm::Value *value = incomingValues.front().second;
      EntryValue.emplace(key, value);
      ResolvingEntry.erase(key);
      return value;
    }

    llvm::PHINode *phi =
        pendingPhi != PendingPhi.end() ? pendingPhi->second.Phi
                                       : ensurePhi(block, unit);
    EntryValue.emplace(key, phi);
    for (const auto &[pred, incoming] : incomingValues) {
      phi->addIncoming(incoming, pred);
    }
    markPendingPhiComplete(*phi);

    llvm::Value *simplified = simplifyPhi(phi);
    EntryValue[key] = simplified;
    ResolvingEntry.erase(key);
    return simplified;
  }

  llvm::Value *readBlockExit(llvm::BasicBlock &block, RegisterUnit &unit) {
    BlockRegKey key{&block, unit.Global};
    auto cached = ExitValue.find(key);
    if (cached != ExitValue.end()) {
      return resolveValue(cached->second);
    }
    llvm::Value *local = localValueBefore(block, unit, block.getTerminator());
    if (local != nullptr) {
      ExitValue.emplace(key, local);
      return local;
    }
    if (blockHasClobberingCall(block, unit)) {
      llvm::Instruction *terminator = block.getTerminator();
      llvm::Value *effect = terminator == nullptr
                                ? nullptr
                                : localValueBefore(block, unit, terminator);
      ExitValue.emplace(key, effect);
      return effect;
    }
    llvm::Value *entry = readBlockEntry(block, unit);
    ExitValue.emplace(key, entry);
    return entry;
  }

  llvm::PHINode *ensurePhi(llvm::BasicBlock &block, RegisterUnit &unit) {
    BlockRegKey key{&block, unit.Global};
    auto existing = PendingPhi.find(key);
    if (existing != PendingPhi.end()) {
      return existing->second.Phi;
    }

    llvm::IRBuilder<> builder(&*block.getFirstInsertionPt());
    auto *phi = builder.CreatePHI(unit.Global->getValueType(), 0,
                                  unit.Name + ".regssa");
    PendingPhi.emplace(key, PendingPhiInfo{phi, PendingPhiState::Incomplete});
    ++Summary.PhisCreated;
    return phi;
  }

  void markPendingPhiComplete(llvm::PHINode &phi) {
    for (auto &[key, info] : PendingPhi) {
      (void)key;
      if (info.Phi == &phi) {
        info.State = PendingPhiState::Complete;
        return;
      }
    }
  }

  llvm::Value *simplifyPhi(llvm::PHINode *phi) {
    std::set<llvm::PHINode *> visiting;
    return simplifyPhi(phi, visiting);
  }

  llvm::Value *simplifyPhi(llvm::PHINode *phi,
                           std::set<llvm::PHINode *> &visiting) {
    if (phi == nullptr || phi->getParent() == nullptr ||
        DeadPhiSet.count(phi) != 0 || !visiting.insert(phi).second) {
      return phi;
    }
    llvm::Value *same = nullptr;
    for (llvm::Value *incoming : phi->incoming_values()) {
      if (incoming == phi) {
        continue;
      }
      if (same == nullptr) {
        same = incoming;
        continue;
      }
      if (same != incoming) {
        visiting.erase(phi);
        return phi;
      }
    }
    if (same == nullptr) {
      visiting.erase(phi);
      return phi;
    }
    std::vector<llvm::PHINode *> phiUsers;
    for (llvm::User *user : phi->users()) {
      if (auto *userPhi = llvm::dyn_cast<llvm::PHINode>(user)) {
        phiUsers.push_back(userPhi);
      }
    }
    replaceCachedValue(phi, same);
    phi->replaceAllUsesWith(same);
    DeadPhis.push_back(phi);
    DeadPhiSet.insert(phi);
    ++Summary.PhisSimplified;
    for (llvm::PHINode *userPhi : phiUsers) {
      if (userPhi->getParent() != nullptr) {
        (void)simplifyPhi(userPhi, visiting);
      }
    }
    visiting.erase(phi);
    return same;
  }

  void replaceCachedValue(llvm::Value *oldValue, llvm::Value *newValue) {
    if (oldValue == nullptr || newValue == nullptr || oldValue == newValue) {
      return;
    }
    for (auto &[key, value] : EntryValue) {
      (void)key;
      if (value == oldValue) {
        value = newValue;
      }
    }
    for (auto &[key, value] : ExitValue) {
      (void)key;
      if (value == oldValue) {
        value = newValue;
      }
    }
    for (auto &[value, replacement] : Replacement) {
      (void)value;
      if (replacement == oldValue) {
        replacement = newValue;
      }
    }
  }

  void eraseDeadPhis() {
    for (llvm::PHINode *phi : DeadPhis) {
      if (phi != nullptr && phi->getParent() != nullptr && phi->use_empty()) {
        forgetPendingPhi(*phi);
        phi->eraseFromParent();
      }
    }
  }

  void finalizePendingPhis() {
    for (auto &[key, info] : PendingPhi) {
      (void)key;
      llvm::PHINode *phi = info.Phi;
      if (phi == nullptr || phi->getParent() == nullptr ||
          info.State == PendingPhiState::Complete) {
        continue;
      }
      info.State = PendingPhiState::Completing;
      completePhiIncoming(*phi);
      info.State = PendingPhiState::Complete;
      (void)simplifyPhi(phi);
    }
  }

  void completePhiIncoming(llvm::PHINode &phi) {
    llvm::BasicBlock *block = phi.getParent();
    if (block == nullptr) {
      return;
    }

    // Braun-style lazy SSA may create a temporary PHI while recursive lookup is
    // still resolving a loop.  The original algorithm later seals the block and
    // fills every predecessor operand.  Our CFG is already complete, so the pass
    // end is the seal point: no PHI may be left with fewer incoming edges than
    // the LLVM CFG requires.
    std::vector<llvm::BasicBlock *> unmatchedIncoming;
    for (llvm::BasicBlock *incomingBlock : phi.blocks()) {
      unmatchedIncoming.push_back(incomingBlock);
    }

    for (llvm::BasicBlock *pred : llvm::predecessors(block)) {
      auto matched = std::find(unmatchedIncoming.begin(),
                               unmatchedIncoming.end(), pred);
      if (matched != unmatchedIncoming.end()) {
        unmatchedIncoming.erase(matched);
        continue;
      }

      llvm::Value *incoming = missingPhiIncomingValue(*pred, phi);
      if (incoming != nullptr) {
        phi.addIncoming(incoming, pred);
      }
    }
  }

  llvm::Value *missingPhiIncomingValue(llvm::BasicBlock &pred,
                                       llvm::PHINode &phi) {
    llvm::GlobalVariable *global = pendingPhiRegister(phi);
    if (global == nullptr) {
      return nullptr;
    }
    auto unitIterator = Units.find(global);
    if (unitIterator == Units.end()) {
      return nullptr;
    }

    llvm::Value *incoming = resolveValue(readBlockExit(pred, unitIterator->second));
    if (incoming != nullptr) {
      return incoming;
    }
    return nullptr;
  }

  llvm::Value *callEffectValue(llvm::Instruction &call, RegisterUnit &unit,
                               const CallEffectInfo &effectInfo) {
    CallEffectKey key{&call, unit.Global, effectInfo.Kind};
    auto cached = CallEffectValue.find(key);
    if (cached != CallEffectValue.end()) {
      return cached->second;
    }

    llvm::Instruction *insertBefore = call.getNextNode();
    if (insertBefore == nullptr) {
      insertBefore = call.getParent()->getTerminator();
    }
    if (insertBefore == nullptr) {
      return llvm::UndefValue::get(unit.Global->getValueType());
    }

    llvm::IRBuilder<> builder(insertBefore);
    std::string name =
        unit.Name + "." + effectInfo.Kind + ".call_effect";
    llvm::CallInst *effect =
        builder.CreateCall(callEffectHelper(unit, effectInfo.Kind), {}, name);
    effect->setMetadata("notdec.register.call_effect",
                        callEffectMetadata(unit, effectInfo.Kind,
                                           effectInfo.Source, &call));
    llvm::StoreInst *store = builder.CreateStore(effect, unit.Global);
    store->setMetadata("notdec.register.access",
                       fullRegisterAccessMetadata(Function.getContext(), unit));
    StoredFullUnits.insert(unit.Global);
    if (effectInfo.Kind == "return") {
      ++Summary.CallReturnHelpers;
    } else {
      ++Summary.CallEffectHelpers;
    }
    CallEffectValue.emplace(key, effect);
    return effect;
  }

  llvm::FunctionCallee callInputHelper(RegisterUnit &unit) {
    llvm::Module *module = Function.getParent();
    llvm::Type *valueType = unit.Global->getValueType();
    llvm::FunctionType *functionType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(Function.getContext()), {valueType}, false);
    return module->getOrInsertFunction(
        "notdec.register.call_input." + typeSuffix(*valueType), functionType);
  }

  llvm::FunctionCallee callEffectHelper(RegisterUnit &unit,
                                        llvm::StringRef effectKind) {
    llvm::Module *module = Function.getParent();
    llvm::Type *valueType = unit.Global->getValueType();
    llvm::FunctionType *functionType =
        llvm::FunctionType::get(valueType, {}, false);
    std::string helperKind =
        effectKind == "return" ? "call_return" : "call_effect";
    return module->getOrInsertFunction(
        "notdec.register." + helperKind + "." + typeSuffix(*valueType),
        functionType);
  }

  std::string typeSuffix(llvm::Type &type) const {
    if (auto *integerType = llvm::dyn_cast<llvm::IntegerType>(&type)) {
      return "i" + std::to_string(integerType->getBitWidth());
    }
    return "value";
  }

  llvm::GlobalVariable *pendingPhiRegister(llvm::PHINode &phi) const {
    for (const auto &[key, pendingPhi] : PendingPhi) {
      if (pendingPhi.Phi == &phi) {
        return key.second;
      }
    }
    return nullptr;
  }

  llvm::MDNode *callEffectMetadata(RegisterUnit &unit,
                                   llvm::StringRef effectKind,
                                   llvm::StringRef effectSource,
                                   llvm::Instruction *call) {
    llvm::LLVMContext &context = Function.getContext();
    std::vector<llvm::Metadata *> fields;
    fields.push_back(llvm::MDString::get(context,
                                         "kind=" + effectKind.str()));
    fields.push_back(llvm::MDString::get(context,
                                         "source=" + effectSource.str()));
    fields.push_back(llvm::MDString::get(context, "register=" + unit.Name));
    fields.push_back(llvm::ValueAsMetadata::get(unit.Global));
    if (call != nullptr) {
      fields.push_back(llvm::MDString::get(
          context, "callsite_id=" + callsiteId(*call)));
      if (llvm::BasicBlock *parent = call->getParent()) {
        fields.push_back(llvm::MDString::get(
            context, ("call_block=" + parent->getName()).str()));
      }
      if (auto *callBase = llvm::dyn_cast<llvm::CallBase>(call)) {
        if (llvm::Function *callee = callBase->getCalledFunction()) {
          fields.push_back(llvm::MDString::get(
              context, ("callee=" + callee->getName()).str()));
        }
      }
    }
    return llvm::MDNode::getDistinct(context, fields);
  }

  std::string callsiteId(const llvm::Instruction &call) const {
    auto found = CallsiteIds.find(&call);
    if (found == CallsiteIds.end()) {
      return "";
    }
    return found->second.Id;
  }

  std::string directCalleeName(const llvm::CallBase &call) const {
    llvm::Function *callee = call.getCalledFunction();
    if (callee == nullptr) {
      return "";
    }
    return callee->getName().str();
  }

  void eraseUnusedPendingPhis() {
    for (auto it = PendingPhi.begin(); it != PendingPhi.end();) {
      llvm::PHINode *phi = it->second.Phi;
      if (phi != nullptr && phi->getParent() != nullptr && phi->use_empty()) {
        phi->eraseFromParent();
        it = PendingPhi.erase(it);
        continue;
      }
      ++it;
    }
  }

  void forgetPendingPhi(llvm::PHINode &phi) {
    for (auto it = PendingPhi.begin(); it != PendingPhi.end();) {
      if (it->second.Phi == &phi) {
        it = PendingPhi.erase(it);
        continue;
      }
      ++it;
    }
  }

  llvm::Value *externalInput(RegisterUnit &unit) {
    auto inserted = ExternalInputs.insert(unit.Global);
    if (inserted.second) {
      ++Summary.ExternalInputs;
    }
    auto existing = ExternalInputValue.find(unit.Global);
    if (existing != ExternalInputValue.end()) {
      return existing->second;
    }

    llvm::BasicBlock &entry = Function.getEntryBlock();
    llvm::IRBuilder<> builder(&*entry.getFirstInsertionPt());
    llvm::LoadInst *load =
        builder.CreateLoad(unit.Global->getValueType(), unit.Global,
                           unit.Name + ".external_input");
    llvm::LLVMContext &context = Function.getContext();
    llvm::Metadata *metadata[] = {
        llvm::MDString::get(context, "name=" + unit.Name),
        llvm::ValueAsMetadata::get(unit.Global),
    };
    load->setMetadata("notdec.register.external_input",
                      llvm::MDNode::get(context, metadata));
    ExternalInputValue.emplace(unit.Global, load);
    return load;
  }

  void attachExternalInputMetadata() {
    if (ExternalInputs.empty()) {
      Function.setMetadata("notdec.register.external_inputs", nullptr);
      return;
    }

    llvm::LLVMContext &context = Function.getContext();
    std::vector<llvm::Metadata *> entries;
    std::vector<llvm::GlobalVariable *> sorted(ExternalInputs.begin(),
                                               ExternalInputs.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const llvm::GlobalVariable *lhs,
                 const llvm::GlobalVariable *rhs) {
                return lhs->getName() < rhs->getName();
              });
    for (llvm::GlobalVariable *global : sorted) {
      std::string name = unitName(*global);
      llvm::Metadata *fields[] = {
          llvm::MDString::get(context, "name=" + name),
          llvm::ValueAsMetadata::get(global),
      };
      entries.push_back(llvm::MDNode::get(context, fields));
    }
    Function.setMetadata("notdec.register.external_inputs",
                         llvm::MDNode::get(context, entries));
  }

  llvm::MDNode *registerEffectMetadata(
      llvm::LLVMContext &context,
      const std::vector<llvm::GlobalVariable *> &globals) {
    if (globals.empty()) {
      return nullptr;
    }
    std::vector<llvm::Metadata *> entries;
    for (llvm::GlobalVariable *global : globals) {
      std::string name = unitName(*global);
      llvm::Metadata *fields[] = {
          llvm::MDString::get(context, "name=" + name),
          llvm::ValueAsMetadata::get(global),
      };
      entries.push_back(llvm::MDNode::get(context, fields));
    }
    return llvm::MDNode::get(context, entries);
  }

  void attachRegisterEffectMetadata() {
    if (AbiEffects.Unaffected.empty() && AbiEffects.KilledByCall.empty()) {
      Function.setMetadata("notdec.register.preserves", nullptr);
      Function.setMetadata("notdec.register.clobbers", nullptr);
      return;
    }

    llvm::LLVMContext &context = Function.getContext();
    std::vector<llvm::GlobalVariable *> preserved;
    std::vector<llvm::GlobalVariable *> clobbered;
    std::set<llvm::GlobalVariable *> clobberedSet;
    for (auto &[global, unit] : Units) {
      if (AbiEffects.KilledByCall.count(unit.Name) != 0 &&
          StoredFullUnits.count(global) != 0 &&
          clobberedSet.insert(global).second) {
        clobbered.push_back(global);
      }
      if (AbiEffects.Unaffected.count(unit.Name) == 0) {
        continue;
      }
      auto inputIt = ExternalInputValue.find(global);
      if (inputIt == ExternalInputValue.end()) {
        continue;
      }
      llvm::Value *input = resolveValue(inputIt->second);
      if (input == nullptr) {
        continue;
      }
      if (isPreservedOnAllReturns(unit, input)) {
        preserved.push_back(global);
      } else if (clobberedSet.insert(global).second) {
        clobbered.push_back(global);
      }
    }

    Function.setMetadata("notdec.register.preserves",
                         registerEffectMetadata(context, preserved));
    Function.setMetadata("notdec.register.clobbers",
                         registerEffectMetadata(context, clobbered));
    Summary.PreservedRegisters = preserved.size();
    Summary.ClobberedRegisters = clobbered.size();
  }

  bool isPreservedOnAllReturns(RegisterUnit &unit, llvm::Value *input) {
    bool sawReturn = false;
    for (llvm::BasicBlock &block : Function) {
      if (!llvm::isa<llvm::ReturnInst>(block.getTerminator())) {
        continue;
      }
      sawReturn = true;
      llvm::Value *exit = resolveValue(readBlockExit(block, unit));
      if (exit != input) {
        return false;
      }
    }
    return sawReturn;
  }

  bool blockHasClobberingCall(llvm::BasicBlock &block,
                              const RegisterUnit &unit) const {
    if (HasCall.count(&block) == 0) {
      return false;
    }
    for (llvm::Instruction &inst : block) {
      if (isRegisterClobberCall(inst) && callClobbersRegister(inst, unit)) {
        return true;
      }
    }
    return false;
  }

  llvm::Function &Function;
  std::map<llvm::GlobalVariable *, RegisterUnit> &Units;
  const AbiRegisterEffects &AbiEffects;
  bool EnableRewrite = true;
  NativeHeritageSSAFunctionSummary &Summary;
  std::vector<llvm::LoadInst *> Loads;
  std::vector<llvm::Instruction *> PendingErase;
  std::vector<llvm::PHINode *> DeadPhis;
  std::set<llvm::PHINode *> DeadPhiSet;
  std::set<llvm::GlobalVariable *> StoredFullUnits;
  std::set<llvm::GlobalVariable *> LoadedUnits;
  std::map<llvm::Value *, llvm::Value *> Replacement;
  std::map<BlockRegKey, llvm::Value *> EntryValue;
  std::map<BlockRegKey, llvm::Value *> ExitValue;
  std::map<BlockRegKey, PendingPhiInfo> PendingPhi;
  std::map<CallEffectKey, llvm::Value *> CallEffectValue;
  std::map<const llvm::Instruction *, CallsiteInfo> CallsiteIds;
  std::set<BlockRegKey> ResolvingEntry;
  std::map<llvm::GlobalVariable *, llvm::Value *> ExternalInputValue;
  std::set<llvm::BasicBlock *> HasCall;
  std::set<llvm::GlobalVariable *> ExternalInputs;
};

void addFunctionSummary(NativeHeritageSSASummary &total,
                        const NativeHeritageSSAFunctionSummary &function) {
  total.FunctionsSeen += 1;
  total.LoadsSeen += function.LoadsSeen;
  total.StoresSeen += function.StoresSeen;
  total.LoadsReplaced += function.LoadsReplaced;
  total.DeadStoresRemoved += function.DeadStoresRemoved;
  total.UnreadFlagStoresRemoved += function.UnreadFlagStoresRemoved;
  total.UnreadRipStoresRemoved += function.UnreadRipStoresRemoved;
  total.PhisCreated += function.PhisCreated;
  total.PhisSimplified += function.PhisSimplified;
  total.ExternalInputs += function.ExternalInputs;
  total.CallsSeen += function.CallsSeen;
  total.PreservedRegisters += function.PreservedRegisters;
  total.ClobberedRegisters += function.ClobberedRegisters;
  total.CallInputHelpers += function.CallInputHelpers;
  total.CallReturnHelpers += function.CallReturnHelpers;
  total.CallEffectHelpers += function.CallEffectHelpers;
  total.StrongCallInputs += function.StrongCallInputs;
  total.WeakCallInputs += function.WeakCallInputs;
  total.BlockedCallInputs += function.BlockedCallInputs;
  total.ActiveCallInputTrials += function.ActiveCallInputTrials;
  total.InactiveCallInputTrials += function.InactiveCallInputTrials;
  total.NoUseCallInputTrials += function.NoUseCallInputTrials;
  total.BlockedCallInputTrials += function.BlockedCallInputTrials;
  total.DefinitelyNotUsedCallInputTrials +=
      function.DefinitelyNotUsedCallInputTrials;
  total.KilledByCallInputTrials += function.KilledByCallInputTrials;
  total.ConditionalEffectCallInputTrials +=
      function.ConditionalEffectCallInputTrials;
  total.ConditionalFinalCheckCallInputTrials +=
      function.ConditionalFinalCheckCallInputTrials;
  total.PathRealisticCallInputTrials += function.PathRealisticCallInputTrials;
  total.PathConditionalCallInputTrials +=
      function.PathConditionalCallInputTrials;
  total.PathBlockedCallInputTrials += function.PathBlockedCallInputTrials;
  total.LocalDefCallInputTrials += function.LocalDefCallInputTrials;
  total.LocalConstCallInputTrials += function.LocalConstCallInputTrials;
  total.LocalArithCallInputTrials += function.LocalArithCallInputTrials;
  total.LocalCastCallInputTrials += function.LocalCastCallInputTrials;
  total.LocalLoadCallInputTrials += function.LocalLoadCallInputTrials;
  total.LocalUnknownCallInputTrials += function.LocalUnknownCallInputTrials;
  total.LocalSharedUseCallInputTrials +=
      function.LocalSharedUseCallInputTrials;
  total.LocalDoubleCallUseCallInputTrials +=
      function.LocalDoubleCallUseCallInputTrials;
  total.PhiCallInputTrials += function.PhiCallInputTrials;
  total.EntryInputCallInputTrials += function.EntryInputCallInputTrials;
  total.CallEffectCallInputTrials += function.CallEffectCallInputTrials;
  total.ReturnForwardCallInputTrials += function.ReturnForwardCallInputTrials;
  total.Functions.push_back(function);
}

void visitDirectCalleesFirst(llvm::Function &function,
                             std::set<llvm::Function *> &visiting,
                             std::set<llvm::Function *> &visited,
                             std::vector<llvm::Function *> &ordered) {
  if (visited.count(&function) != 0) {
    return;
  }
  if (!visiting.insert(&function).second) {
    return;
  }

  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &inst : block) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      llvm::Function *callee =
          call == nullptr ? nullptr : call->getCalledFunction();
      if (callee == nullptr || callee->isDeclaration()) {
        continue;
      }
      visitDirectCalleesFirst(*callee, visiting, visited, ordered);
    }
  }

  visiting.erase(&function);
  visited.insert(&function);
  ordered.push_back(&function);
}

std::vector<llvm::Function *> directCalleeFirstOrder(llvm::Module &module) {
  // This is a small substitute for a real call-effect fixpoint: non-recursive
  // direct callees get their register effect metadata before their callers.
  std::vector<llvm::Function *> ordered;
  std::set<llvm::Function *> visiting;
  std::set<llvm::Function *> visited;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    visitDirectCalleesFirst(function, visiting, visited, ordered);
  }
  return ordered;
}

} // namespace

NativeHeritageSSASummary
runNativeHeritageSSA(llvm::Module &module,
                     const NativeHeritageSSAOptions &options) {
  NativeHeritageSSASummary summary;
  std::map<llvm::GlobalVariable *, RegisterUnit> units =
      collectRegisterUnits(module);
  if (units.empty()) {
    return summary;
  }
  AbiRegisterEffects abiEffects = collectAbiRegisterEffects(module);

  for (llvm::Function &function : module) {
    if (!function.isDeclaration()) {
      llvm::EliminateUnreachableBlocks(function);
    }
  }

  for (llvm::Function *function : directCalleeFirstOrder(module)) {
    NativeHeritageSSAFunctionSummary functionSummary;
    FunctionPromoter promoter(*function, units, abiEffects,
                              options.EnableRewrite, functionSummary);
    promoter.run();
    addFunctionSummary(summary, functionSummary);
  }

  if (options.PrintSummary) {
    printNativeHeritageSSASummary(summary, llvm::errs());
  }
  return summary;
}

void printNativeHeritageSSASummary(const NativeHeritageSSASummary &summary,
                                   llvm::raw_ostream &os) {
  os << "native heritage ssa summary\n";
  os << "  functions: " << summary.FunctionsSeen << '\n';
  os << "  loads: " << summary.LoadsSeen << '\n';
  os << "  stores: " << summary.StoresSeen << '\n';
  os << "  loads replaced: " << summary.LoadsReplaced << '\n';
  os << "  dead stores removed: " << summary.DeadStoresRemoved << '\n';
  os << "  unread flag stores removed: " << summary.UnreadFlagStoresRemoved
     << '\n';
  os << "  unread RIP stores removed: " << summary.UnreadRipStoresRemoved
     << '\n';
  os << "  phis created: " << summary.PhisCreated << '\n';
  os << "  phis simplified: " << summary.PhisSimplified << '\n';
  os << "  external inputs: " << summary.ExternalInputs << '\n';
  os << "  calls: " << summary.CallsSeen << '\n';
  os << "  preserved registers: " << summary.PreservedRegisters << '\n';
  os << "  clobbered registers: " << summary.ClobberedRegisters << '\n';
  os << "  call input helpers: " << summary.CallInputHelpers << '\n';
  os << "  call return helpers: " << summary.CallReturnHelpers << '\n';
  os << "  call effect helpers: " << summary.CallEffectHelpers << '\n';
  os << "  call input strong: " << summary.StrongCallInputs << '\n';
  os << "  call input weak: " << summary.WeakCallInputs << '\n';
  os << "  call input blocked: " << summary.BlockedCallInputs << '\n';
  os << "  call input trials active: " << summary.ActiveCallInputTrials
     << '\n';
  os << "  call input trials inactive: " << summary.InactiveCallInputTrials
     << '\n';
  os << "  call input trials no use: " << summary.NoUseCallInputTrials
     << '\n';
  os << "  call input trials blocked: " << summary.BlockedCallInputTrials
     << '\n';
  os << "  call input trial flags definitely not used: "
     << summary.DefinitelyNotUsedCallInputTrials << '\n';
  os << "  call input trial flags killed by call: "
     << summary.KilledByCallInputTrials << '\n';
  os << "  call input trial flags conditional effect: "
     << summary.ConditionalEffectCallInputTrials << '\n';
  os << "  call input trial flags conditional final check: "
     << summary.ConditionalFinalCheckCallInputTrials << '\n';
  os << "  call input trial flags path realistic: "
     << summary.PathRealisticCallInputTrials << '\n';
  os << "  call input trial flags path conditional: "
     << summary.PathConditionalCallInputTrials << '\n';
  os << "  call input trial flags path blocked: "
     << summary.PathBlockedCallInputTrials << '\n';
  os << "  call input trial reasons local def: "
     << summary.LocalDefCallInputTrials << '\n';
  os << "  call input trial reasons local const: "
     << summary.LocalConstCallInputTrials << '\n';
  os << "  call input trial reasons local arith: "
     << summary.LocalArithCallInputTrials << '\n';
  os << "  call input trial reasons local cast: "
     << summary.LocalCastCallInputTrials << '\n';
  os << "  call input trial reasons local load: "
     << summary.LocalLoadCallInputTrials << '\n';
  os << "  call input trial reasons local unknown inst: "
     << summary.LocalUnknownCallInputTrials << '\n';
  os << "  call input trial reasons local shared use: "
     << summary.LocalSharedUseCallInputTrials << '\n';
  os << "  call input trial reasons local double call use: "
     << summary.LocalDoubleCallUseCallInputTrials << '\n';
  os << "  call input trial reasons phi: " << summary.PhiCallInputTrials
     << '\n';
  os << "  call input trial reasons entry input: "
     << summary.EntryInputCallInputTrials << '\n';
  os << "  call input trial reasons call effect: "
     << summary.CallEffectCallInputTrials << '\n';
  os << "  call input trial reasons return forward: "
     << summary.ReturnForwardCallInputTrials << '\n';
  for (const NativeHeritageSSAFunctionSummary &function : summary.Functions) {
    os << "  function " << function.FunctionName << ": loads="
       << function.LoadsSeen << " stores=" << function.StoresSeen
       << " replaced=" << function.LoadsReplaced
       << " dead_stores_removed=" << function.DeadStoresRemoved
       << " phis=" << function.PhisCreated
       << " simplified=" << function.PhisSimplified
       << " external_inputs=" << function.ExternalInputs
       << " calls=" << function.CallsSeen
       << " preserved=" << function.PreservedRegisters
       << " clobbered=" << function.ClobberedRegisters
       << " call_input_helpers=" << function.CallInputHelpers
       << " call_return_helpers=" << function.CallReturnHelpers
       << " call_effect_helpers=" << function.CallEffectHelpers
       << " call_input_strong=" << function.StrongCallInputs
       << " call_input_weak=" << function.WeakCallInputs
       << " call_input_blocked=" << function.BlockedCallInputs
       << " call_input_trials_active=" << function.ActiveCallInputTrials
       << " call_input_trials_inactive=" << function.InactiveCallInputTrials
       << " call_input_trials_no_use=" << function.NoUseCallInputTrials
       << " call_input_trials_blocked=" << function.BlockedCallInputTrials
       << " call_input_trial_flags_definitely_not_used="
       << function.DefinitelyNotUsedCallInputTrials
       << " call_input_trial_flags_killed_by_call="
       << function.KilledByCallInputTrials
       << " call_input_trial_flags_conditional_effect="
       << function.ConditionalEffectCallInputTrials
       << " call_input_trial_flags_conditional_final_check="
       << function.ConditionalFinalCheckCallInputTrials
       << " call_input_trial_flags_path_realistic="
       << function.PathRealisticCallInputTrials
       << " call_input_trial_flags_path_conditional="
       << function.PathConditionalCallInputTrials
       << " call_input_trial_flags_path_blocked="
       << function.PathBlockedCallInputTrials
       << " call_input_trial_reasons_local_def="
       << function.LocalDefCallInputTrials
       << " call_input_trial_reasons_local_const="
       << function.LocalConstCallInputTrials
       << " call_input_trial_reasons_local_arith="
       << function.LocalArithCallInputTrials
       << " call_input_trial_reasons_local_cast="
       << function.LocalCastCallInputTrials
       << " call_input_trial_reasons_local_load="
       << function.LocalLoadCallInputTrials
       << " call_input_trial_reasons_local_unknown_inst="
       << function.LocalUnknownCallInputTrials
       << " call_input_trial_reasons_local_shared_use="
       << function.LocalSharedUseCallInputTrials
       << " call_input_trial_reasons_local_double_call_use="
       << function.LocalDoubleCallUseCallInputTrials
       << " call_input_trial_reasons_phi=" << function.PhiCallInputTrials
       << " call_input_trial_reasons_entry_input="
       << function.EntryInputCallInputTrials
       << " call_input_trial_reasons_call_effect="
       << function.CallEffectCallInputTrials
       << " call_input_trial_reasons_return_forward="
       << function.ReturnForwardCallInputTrials
       << '\n';
  }
}

} // namespace notdec::bin2llvm
