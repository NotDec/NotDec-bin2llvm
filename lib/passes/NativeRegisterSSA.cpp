#include "notdec-bin2llvm/passes/NativeRegisterSSA.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
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
  uint32_t Size = 0;
};

// AccessInfo separates "this is a register access" from "this is safe to
// promote". Partial accesses stay in memory form for now.
struct AccessInfo {
  RegisterUnit *Unit = nullptr;
  bool IsFullUnit = false;
};

using BlockRegKey = std::pair<llvm::BasicBlock *, llvm::GlobalVariable *>;

struct AbiRegisterEffects {
  std::set<std::string> Unaffected;
  std::set<std::string> KilledByCall;
};

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
    unit.Size = parseU32(mdField(node, "size"));
    units.emplace(&global, std::move(unit));
  }
  return units;
}

AbiRegisterEffects collectAbiRegisterEffects(llvm::Module &module) {
  AbiRegisterEffects effects;
  llvm::NamedMDNode *abiMetadata = module.getNamedMetadata("notdec.abi");
  if (abiMetadata == nullptr) {
    return effects;
  }

  for (llvm::MDNode *abiNode : abiMetadata->operands()) {
    for (const llvm::MDOperand &operand : abiNode->operands()) {
      auto *effectList = llvm::dyn_cast_or_null<llvm::MDNode>(operand.get());
      if (effectList == nullptr) {
        continue;
      }
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
          if (storageNode == nullptr || mdField(storageNode, "kind") !=
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
  if (load.getMetadata("notdec.register.access") == nullptr &&
      global->getMetadata("notdec.register") == nullptr) {
    return {};
  }
  bool fullUnit = load.getType() == global->getValueType();
  return AccessInfo{&it->second, fullUnit};
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
  if (store.getMetadata("notdec.register.access") == nullptr) {
    return {};
  }
  bool fullUnit = store.getValueOperand()->getType() == global->getValueType();
  return AccessInfo{&it->second, fullUnit};
}

bool isRegisterClobberCall(const llvm::Instruction &inst) {
  auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
  if (call == nullptr) {
    return false;
  }
  llvm::Function *callee = call->getCalledFunction();
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
                   NativeRegisterSSAFunctionSummary &summary)
      : Function(function), Units(units), AbiEffects(abiEffects), EnableRewrite(enableRewrite),
        Summary(summary) {}

  void run() {
    Summary.FunctionName = Function.getName().str();
    for (llvm::BasicBlock &block : Function) {
      scanBlock(block);
    }

    if (EnableRewrite) {
      rewriteLoads();
      attachRegisterEffectMetadata();
      eraseDeadPhis();
    } else {
      collectExternalInputsOnly();
      attachRegisterEffectMetadata();
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
          if (access.IsFullUnit) {
            Loads.push_back(load);
          }
        }
        continue;
      }

      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        AccessInfo access = registerStore(*store, Units);
        if (access.Unit != nullptr) {
          ++Summary.StoresSeen;
          if (access.IsFullUnit) {
            StoredFullUnits.insert(access.Unit->Global);
          }
        }
        continue;
      }

      if (isRegisterClobberCall(inst)) {
        ++Summary.CallsSeen;
        HasCall.insert(&block);
      }
    }
  }

  void rewriteLoads() {
    for (llvm::LoadInst *load : Loads) {
      AccessInfo access = registerLoad(*load, Units);
      if (access.Unit == nullptr || !access.IsFullUnit) {
        continue;
      }

      llvm::Value *value = readRegister(*load->getParent(), *access.Unit, load);
      if (value == nullptr || value == load) {
        continue;
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
      if (access.Unit == nullptr || !access.IsFullUnit) {
        continue;
      }
      (void)readRegister(*load->getParent(), *access.Unit, load);
    }
  }

  llvm::Value *readRegister(llvm::BasicBlock &block, RegisterUnit &unit,
                            llvm::Instruction *before) {
    llvm::Value *local = localValueBefore(block, unit, before);
    if (local != nullptr) {
      return local;
    }
    if (hasCallBefore(block, unit, before)) {
      return nullptr;
    }
    return readBlockEntry(block, unit);
  }

  llvm::Value *localValueBefore(llvm::BasicBlock &block, RegisterUnit &unit,
                                llvm::Instruction *before) {
    bool passedBarrier = false;
    for (llvm::Instruction &inst : llvm::reverse(block)) {
      if (&inst == before) {
        continue;
      }
      if (before != nullptr && !inst.comesBefore(before)) {
        continue;
      }
      if (isRegisterClobberCall(inst) && callClobbersRegister(inst, unit)) {
        passedBarrier = true;
        continue;
      }
      auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
      if (store == nullptr) {
        continue;
      }
      if (passedBarrier) {
        continue;
      }
      AccessInfo access = registerStore(*store, Units);
      if (access.Unit == &unit && access.IsFullUnit) {
        return resolveValue(store->getValueOperand());
      }
    }
    return nullptr;
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

  bool hasCallBefore(llvm::BasicBlock &block, RegisterUnit &unit,
                     llvm::Instruction *before) {
    for (llvm::Instruction &inst : block) {
      if (&inst == before) {
        return false;
      }
      if (isRegisterClobberCall(inst) && callClobbersRegister(inst, unit)) {
        return true;
      }
    }
    return false;
  }

  bool callClobbersRegister(const llvm::Instruction &inst,
                            const RegisterUnit &unit) const {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
    llvm::Function *callee =
        call == nullptr ? nullptr : call->getCalledFunction();
    if (callee != nullptr && !callee->isDeclaration()) {
      if (functionMetadataHasRegister(*callee, "notdec.register.preserves",
                                      unit.Name)) {
        return false;
      }
      if (functionMetadataHasRegister(*callee, "notdec.register.clobbers",
                                      unit.Name)) {
        return true;
      }
    }
    if (AbiEffects.Unaffected.count(unit.Name) != 0) {
      return false;
    }
    return true;
  }

  llvm::Value *readBlockEntry(llvm::BasicBlock &block, RegisterUnit &unit) {
    BlockRegKey key{&block, unit.Global};
    auto cached = EntryValue.find(key);
    if (cached != EntryValue.end()) {
      return resolveValue(cached->second);
    }
    if (ResolvingEntry.count(key) != 0) {
      return nullptr;
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
    if (preds.size() == 1) {
      llvm::Value *value = readBlockExit(*preds.front(), unit);
      value = resolveValue(value);
      EntryValue.emplace(key, value);
      ResolvingEntry.erase(key);
      return value;
    }

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

    llvm::PHINode *phi = ensurePhi(block, unit);
    EntryValue.emplace(key, phi);
    for (const auto &[pred, incoming] : incomingValues) {
      phi->addIncoming(incoming, pred);
    }

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
      ExitValue.emplace(key, nullptr);
      return nullptr;
    }
    llvm::Value *entry = readBlockEntry(block, unit);
    ExitValue.emplace(key, entry);
    return entry;
  }

  llvm::PHINode *ensurePhi(llvm::BasicBlock &block, RegisterUnit &unit) {
    BlockRegKey key{&block, unit.Global};
    auto existing = PendingPhi.find(key);
    if (existing != PendingPhi.end()) {
      return existing->second;
    }

    llvm::IRBuilder<> builder(&*block.getFirstInsertionPt());
    auto *phi = builder.CreatePHI(unit.Global->getValueType(), 0,
                                  unit.Name + ".regssa");
    PendingPhi.emplace(key, phi);
    ++Summary.PhisCreated;
    return phi;
  }

  llvm::Value *simplifyPhi(llvm::PHINode *phi) {
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
        return phi;
      }
    }
    if (same == nullptr) {
      return phi;
    }
    phi->replaceAllUsesWith(same);
    DeadPhis.push_back(phi);
    ++Summary.PhisSimplified;
    return same;
  }

  void eraseDeadPhis() {
    for (llvm::PHINode *phi : DeadPhis) {
      if (phi->use_empty()) {
        phi->eraseFromParent();
      }
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
  NativeRegisterSSAFunctionSummary &Summary;
  std::vector<llvm::LoadInst *> Loads;
  std::vector<llvm::Instruction *> PendingErase;
  std::vector<llvm::PHINode *> DeadPhis;
  std::set<llvm::GlobalVariable *> StoredFullUnits;
  std::map<llvm::Value *, llvm::Value *> Replacement;
  std::map<BlockRegKey, llvm::Value *> EntryValue;
  std::map<BlockRegKey, llvm::Value *> ExitValue;
  std::map<BlockRegKey, llvm::PHINode *> PendingPhi;
  std::set<BlockRegKey> ResolvingEntry;
  std::map<llvm::GlobalVariable *, llvm::Value *> ExternalInputValue;
  std::set<llvm::BasicBlock *> HasCall;
  std::set<llvm::GlobalVariable *> ExternalInputs;
};

void addFunctionSummary(NativeRegisterSSASummary &total,
                        const NativeRegisterSSAFunctionSummary &function) {
  total.FunctionsSeen += 1;
  total.LoadsSeen += function.LoadsSeen;
  total.StoresSeen += function.StoresSeen;
  total.LoadsReplaced += function.LoadsReplaced;
  total.PhisCreated += function.PhisCreated;
  total.PhisSimplified += function.PhisSimplified;
  total.ExternalInputs += function.ExternalInputs;
  total.CallsSeen += function.CallsSeen;
  total.PreservedRegisters += function.PreservedRegisters;
  total.ClobberedRegisters += function.ClobberedRegisters;
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

NativeRegisterSSASummary
runNativeRegisterSSA(llvm::Module &module,
                     const NativeRegisterSSAOptions &options) {
  NativeRegisterSSASummary summary;
  std::map<llvm::GlobalVariable *, RegisterUnit> units =
      collectRegisterUnits(module);
  if (units.empty()) {
    return summary;
  }
  AbiRegisterEffects abiEffects = collectAbiRegisterEffects(module);

  for (llvm::Function *function : directCalleeFirstOrder(module)) {
    NativeRegisterSSAFunctionSummary functionSummary;
    FunctionPromoter promoter(*function, units, abiEffects,
                              options.EnableRewrite, functionSummary);
    promoter.run();
    addFunctionSummary(summary, functionSummary);
  }

  if (options.PrintSummary) {
    printNativeRegisterSSASummary(summary, llvm::errs());
  }
  return summary;
}

void printNativeRegisterSSASummary(const NativeRegisterSSASummary &summary,
                                   llvm::raw_ostream &os) {
  os << "native register ssa summary\n";
  os << "  functions: " << summary.FunctionsSeen << '\n';
  os << "  loads: " << summary.LoadsSeen << '\n';
  os << "  stores: " << summary.StoresSeen << '\n';
  os << "  loads replaced: " << summary.LoadsReplaced << '\n';
  os << "  phis created: " << summary.PhisCreated << '\n';
  os << "  phis simplified: " << summary.PhisSimplified << '\n';
  os << "  external inputs: " << summary.ExternalInputs << '\n';
  os << "  calls: " << summary.CallsSeen << '\n';
  os << "  preserved registers: " << summary.PreservedRegisters << '\n';
  os << "  clobbered registers: " << summary.ClobberedRegisters << '\n';
  for (const NativeRegisterSSAFunctionSummary &function : summary.Functions) {
    os << "  function " << function.FunctionName << ": loads="
       << function.LoadsSeen << " stores=" << function.StoresSeen
       << " replaced=" << function.LoadsReplaced
       << " phis=" << function.PhisCreated
       << " simplified=" << function.PhisSimplified
       << " external_inputs=" << function.ExternalInputs
       << " calls=" << function.CallsSeen
       << " preserved=" << function.PreservedRegisters
       << " clobbered=" << function.ClobberedRegisters << '\n';
  }
}

} // namespace notdec::bin2llvm
