#include "notdec-bin2llvm/passes/NativeRegisterSummarySSA.h"

#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/passes/NativeRegisterSummary.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace notdec::bin2llvm {
namespace {

constexpr llvm::StringLiteral CallArgValuesBundleTag =
    "notdec.register.summary_ssa.call_arg_values";

struct RegisterUnit {
  llvm::GlobalVariable *Global = nullptr;
  std::string Name;
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
};

struct FunctionSummaryFacts {
  std::map<std::string, SummaryRegisterFact> Registers;
};

struct AbiFacts {
  std::set<std::string> Inputs;
  std::vector<std::string> InputsInOrder;
  std::set<std::string> Outputs;
  std::set<std::string> Unaffected;
  std::set<std::string> KilledByCall;
};

enum class CallRegisterEffect {
  Preserve,
  ReturnValue,
  Clobber,
  Unknown,
};

using BlockRegKey = std::pair<llvm::BasicBlock *, llvm::GlobalVariable *>;
using CallValueKey =
    std::tuple<llvm::Instruction *, llvm::GlobalVariable *, std::string>;

struct CallArgStoreBinding {
  llvm::StoreInst *Store = nullptr;
  const RegisterUnit *Unit = nullptr;
  llvm::Value *Value = nullptr;
  unsigned Index = 0;
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

std::string unitName(const llvm::GlobalVariable &global) {
  if (auto name = mdField(global.getMetadata("notdec.register"), "name")) {
    if (!name->empty()) {
      return *name;
    }
  }
  return global.getName().str();
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
    unit.Name = unitName(global);
    units.emplace(&global, std::move(unit));
  }
  return units;
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

bool isNotDecRegisterHelperCall(const llvm::CallBase &call) {
  llvm::Function *callee = call.getCalledFunction();
  return callee != nullptr && callee->getName().starts_with("notdec.register.");
}

bool isAnalyzableCall(const llvm::CallBase &call) {
  if (isNotDecRegisterHelperCall(call)) {
    return false;
  }
  llvm::Function *callee = call.getCalledFunction();
  return callee == nullptr || !callee->isIntrinsic();
}

AbiFacts collectAbiFacts(const llvm::Module &module) {
  AbiFacts facts;
  std::optional<NativeAbiSpec> abi = readNativeAbiMetadata(module);
  if (!abi) {
    return facts;
  }
  for (const NativeAbiParamEntry &entry : abi->Inputs) {
    if (entry.Storage.Kind == NativeAbiStorageKind::Register &&
        !entry.Storage.Name.empty()) {
      if (entry.MetaType == "float") {
        continue;
      }
      facts.Inputs.insert(entry.Storage.Name);
      facts.InputsInOrder.push_back(entry.Storage.Name);
    }
  }
  for (const NativeAbiParamEntry &entry : abi->Outputs) {
    if (entry.Storage.Kind == NativeAbiStorageKind::Register &&
        !entry.Storage.Name.empty()) {
      facts.Outputs.insert(entry.Storage.Name);
    }
  }
  for (const NativeAbiEffect &effect : abi->Effects) {
    if (effect.Storage.Kind != NativeAbiStorageKind::Register ||
        effect.Storage.Name.empty()) {
      continue;
    }
    if (effect.Kind == NativeAbiEffectKind::Unaffected) {
      facts.Unaffected.insert(effect.Storage.Name);
    } else if (effect.Kind == NativeAbiEffectKind::KilledByCall) {
      facts.KilledByCall.insert(effect.Storage.Name);
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
    for (const NativeRegisterSummaryRegister &reg : functionSummary.Registers) {
      facts.Registers.emplace(
          reg.Name, SummaryRegisterFact{reg.ReadEntry, reg.MayEntry,
                                        reg.MayNonEntry, reg.ExitDemand});
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

class FunctionBuilder {
public:
  FunctionBuilder(
      llvm::Function &function,
      const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
      const std::map<llvm::Function *, FunctionSummaryFacts> &summaryFacts,
      const AbiFacts &abiFacts, const NativeRegisterSummarySSAOptions &options,
      NativeRegisterSummarySSAFunctionSummary &summary)
      : Function(function), Units(units), SummaryFacts(summaryFacts),
        Abi(abiFacts), Options(options), Summary(summary) {}

  void run() {
    Summary.FunctionName = Function.getName().str();
    collectAccesses();
    if (Options.EnableRewrite) {
      rewriteLoads();
      markExternalCallArgumentStores();
      finalizePendingPhis();
      if (Options.EnableResidueRemoval) {
        removeDeadReplacedLoads();
        removeDeadStoresByLiveness();
      }
      eraseDeadPhis();
    }
    if (Options.AttachMetadata) {
      attachMetadata();
    }
  }

private:
  llvm::Function &Function;
  const std::map<llvm::GlobalVariable *, RegisterUnit> &Units;
  const std::map<llvm::Function *, FunctionSummaryFacts> &SummaryFacts;
  const AbiFacts &Abi;
  const NativeRegisterSummarySSAOptions &Options;
  NativeRegisterSummarySSAFunctionSummary &Summary;
  std::vector<llvm::LoadInst *> Loads;
  std::vector<llvm::LoadInst *> ReplacedLoads;
  std::map<BlockRegKey, llvm::Value *> EntryValue;
  std::map<BlockRegKey, llvm::Value *> ExitValue;
  std::map<BlockRegKey, llvm::PHINode *> PendingPhi;
  std::set<BlockRegKey> ResolvingEntry;
  std::map<llvm::Value *, llvm::Value *> Replacement;
  std::set<llvm::PHINode *> DeadPhis;
  std::map<llvm::GlobalVariable *, llvm::LoadInst *> EntryInputs;
  std::map<CallValueKey, llvm::Value *> CallValues;

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
        }
      }
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
      load->replaceAllUsesWith(value);
      load->setMetadata("notdec.register.summary_ssa.replaced",
                        markerNode("true"));
      ReplacedLoads.push_back(load);
      ++Summary.LoadsReplaced;
    }
  }

  void removeDeadReplacedLoads() {
    for (llvm::LoadInst *load : ReplacedLoads) {
      if (load->getParent() == nullptr || !load->use_empty()) {
        continue;
      }
      load->eraseFromParent();
      ++Summary.DeadLoadsRemoved;
    }
  }

  void removeDeadStoresByLiveness() {
    std::map<llvm::BasicBlock *, std::set<llvm::GlobalVariable *>> liveIn;
    std::map<llvm::BasicBlock *, std::set<llvm::GlobalVariable *>> liveOut;
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
        std::set<llvm::GlobalVariable *> out;
        for (llvm::BasicBlock *succ : llvm::successors(&block)) {
          auto succLive = liveIn.find(succ);
          if (succLive != liveIn.end()) {
            out.insert(succLive->second.begin(), succLive->second.end());
          }
        }
        if (llvm::succ_empty(&block)) {
          addExitLiveRegisters(out);
        }

        std::set<llvm::GlobalVariable *> in = transferBlockLiveness(block, out);
        changed |= liveOut[&block] != out || liveIn[&block] != in;
        liveOut[&block] = std::move(out);
        liveIn[&block] = std::move(in);
      }
    }

    for (llvm::BasicBlock &block : Function) {
      auto outIt = liveOut.find(&block);
      std::set<llvm::GlobalVariable *> live =
          outIt == liveOut.end() ? std::set<llvm::GlobalVariable *>{}
                                 : outIt->second;
      eraseDeadStoresInBlock(block, live);
    }
  }

  std::set<llvm::GlobalVariable *> transferBlockLiveness(
      llvm::BasicBlock &block, std::set<llvm::GlobalVariable *> live) {
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
        transferCallLiveness(*call, live);
        continue;
      }
    }
    return live;
  }

  void eraseDeadStoresInBlock(llvm::BasicBlock &block,
                              std::set<llvm::GlobalVariable *> live) {
    std::vector<llvm::StoreInst *> deadStores;
    for (auto it = block.rbegin(); it != block.rend(); ++it) {
      llvm::Instruction &inst = *it;
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && access.IsStorageValue &&
            live.count(access.Unit->Global) == 0) {
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
        transferCallLiveness(*call, live);
        continue;
      }
    }
    for (llvm::StoreInst *store : deadStores) {
      store->eraseFromParent();
      ++Summary.DeadStoresRemoved;
    }
  }

  void transferStoreLiveness(llvm::StoreInst &store,
                             std::set<llvm::GlobalVariable *> &live) const {
    RegisterAccess access = registerStore(store, Units);
    if (access.Unit != nullptr && access.IsStorageValue) {
      live.erase(access.Unit->Global);
    }
  }

  void transferLoadLiveness(llvm::LoadInst &load,
                            std::set<llvm::GlobalVariable *> &live) const {
    RegisterAccess access = registerLoad(load, Units);
    if (access.Unit != nullptr && access.IsStorageValue) {
      live.insert(access.Unit->Global);
    }
  }

  void transferCallLiveness(llvm::CallBase &call,
                            std::set<llvm::GlobalVariable *> &live) const {
    if (!isAnalyzableCall(call)) {
      return;
    }
    for (const auto &[global, unit] : Units) {
      CallRegisterEffect effect = callEffect(call, unit);
      if (effect == CallRegisterEffect::ReturnValue ||
          effect == CallRegisterEffect::Clobber) {
        live.erase(global);
      }
      if (callReadsRegister(call, unit)) {
        live.insert(global);
      }
    }
  }

  void addExitLiveRegisters(std::set<llvm::GlobalVariable *> &live) const {
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
        live.insert(global);
      }
    }
  }

  llvm::Value *readValueBefore(llvm::BasicBlock &block,
                               const RegisterUnit &unit,
                               llvm::Instruction *before) {
    for (auto it = before->getIterator(); it != block.begin();) {
      --it;
      llvm::Instruction &inst = *it;
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && access.Unit->Global == unit.Global &&
            access.IsStorageValue) {
          return resolve(store->getValueOperand());
        }
        continue;
      }
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (!isAnalyzableCall(*call)) {
          continue;
        }
        CallRegisterEffect effect = callEffect(*call, unit);
        if (effect == CallRegisterEffect::Preserve) {
          ++Summary.PreservedCalls;
          continue;
        }
        if (effect == CallRegisterEffect::ReturnValue) {
          return callValue(*call, unit, "return");
        }
        if (effect == CallRegisterEffect::Clobber) {
          return callValue(*call, unit, "clobber");
        }
        ++Summary.UnknownCallEffects;
        return nullptr;
      }
    }
    return readBlockEntry(block, unit);
  }

  llvm::Value *readBlockEntry(llvm::BasicBlock &block,
                              const RegisterUnit &unit) {
    BlockRegKey key{&block, unit.Global};
    if (auto cached = EntryValue.find(key); cached != EntryValue.end()) {
      return resolve(cached->second);
    }
    if (ResolvingEntry.count(key) != 0) {
      return ensurePhi(block, unit);
    }

    ResolvingEntry.insert(key);
    std::vector<llvm::BasicBlock *> preds(llvm::pred_begin(&block),
                                          llvm::pred_end(&block));
    llvm::Value *value = nullptr;
    if (preds.empty()) {
      value = entryInput(unit);
    } else if (preds.size() == 1) {
      value = readBlockExit(*preds.front(), unit);
      if (PendingPhi.count(key) != 0) {
        value = completePhi(block, unit);
      }
    } else {
      value = completePhi(block, unit);
    }
    ResolvingEntry.erase(key);
    EntryValue[key] = resolve(value);
    return EntryValue[key];
  }

  llvm::Value *readBlockExit(llvm::BasicBlock &block,
                             const RegisterUnit &unit) {
    BlockRegKey key{&block, unit.Global};
    if (auto cached = ExitValue.find(key); cached != ExitValue.end()) {
      return resolve(cached->second);
    }
    llvm::Instruction *terminator = block.getTerminator();
    llvm::Value *value = terminator == nullptr
                             ? readBlockEntry(block, unit)
                             : readValueBefore(block, unit, terminator);
    ExitValue[key] = resolve(value);
    return ExitValue[key];
  }

  llvm::PHINode *ensurePhi(llvm::BasicBlock &block, const RegisterUnit &unit) {
    BlockRegKey key{&block, unit.Global};
    if (auto existing = PendingPhi.find(key); existing != PendingPhi.end()) {
      return existing->second;
    }
    llvm::IRBuilder<> builder(&block, block.getFirstNonPHIIt());
    llvm::PHINode *phi = builder.CreatePHI(unit.Global->getValueType(), 0,
                                           unit.Name + ".summary_ssa");
    phi->setMetadata("notdec.register.summary_ssa.phi", registerNode(unit));
    PendingPhi.emplace(key, phi);
    EntryValue[key] = phi;
    ++Summary.PhisCreated;
    return phi;
  }

  llvm::Value *completePhi(llvm::BasicBlock &block, const RegisterUnit &unit) {
    llvm::PHINode *phi = ensurePhi(block, unit);
    // LLVM PHI operands are edge-based. A switch can contribute the same
    // predecessor block more than once, so count per-block occurrences.
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
      llvm::Value *incoming = resolve(readBlockExit(*pred, unit));
      if (incoming == nullptr) {
        incoming = llvm::UndefValue::get(unit.Global->getValueType());
      }
      phi->addIncoming(incoming, pred);
    }
    return simplifyPhi(*phi);
  }

  llvm::Value *simplifyPhi(llvm::PHINode &phi) {
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
      return &phi;
    }
    Replacement[&phi] = same;
    phi.replaceAllUsesWith(same);
    DeadPhis.insert(&phi);
    ++Summary.PhisSimplified;
    return same;
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
      std::vector<std::pair<llvm::BasicBlock *, const RegisterUnit *>> work;
      for (const auto &[key, phi] : PendingPhi) {
        auto unitIt = Units.find(key.second);
        if (unitIt != Units.end() && DeadPhis.count(phi) == 0 &&
            !isCompletePhi(*phi)) {
          work.push_back({key.first, &unitIt->second});
        }
      }
      for (const auto &[block, unit] : work) {
        llvm::PHINode *phi = PendingPhi[{block, unit->Global}];
        unsigned before = phi->getNumIncomingValues();
        (void)completePhi(*block, *unit);
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

  llvm::Value *resolve(llvm::Value *value) {
    while (value != nullptr) {
      auto it = Replacement.find(value);
      if (it == Replacement.end() || it->second == value) {
        return value;
      }
      value = it->second;
    }
    return nullptr;
  }

  llvm::LoadInst *entryInput(const RegisterUnit &unit) {
    if (auto cached = EntryInputs.find(unit.Global);
        cached != EntryInputs.end()) {
      return cached->second;
    }
    llvm::IRBuilder<> builder(&Function.getEntryBlock(),
                              Function.getEntryBlock().getFirstNonPHIIt());
    llvm::LoadInst *load = builder.CreateLoad(
        unit.Global->getValueType(), unit.Global, unit.Name + ".entry");
    load->setMetadata("notdec.register.summary_ssa.entry", registerNode(unit));
    EntryInputs.emplace(unit.Global, load);
    ++Summary.EntryInputs;
    return load;
  }

  CallRegisterEffect callEffect(const llvm::CallBase &call,
                                const RegisterUnit &unit) const {
    llvm::Function *callee = call.getCalledFunction();
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

    if (Abi.Unaffected.count(unit.Name) != 0) {
      return CallRegisterEffect::Preserve;
    }
    if (Abi.Outputs.count(unit.Name) != 0) {
      return CallRegisterEffect::ReturnValue;
    }
    if (Abi.KilledByCall.count(unit.Name) != 0) {
      return CallRegisterEffect::Clobber;
    }
    return CallRegisterEffect::Unknown;
  }

  bool callReadsRegister(const llvm::CallBase &call,
                         const RegisterUnit &unit) const {
    llvm::Function *callee = call.getCalledFunction();
    if (callee != nullptr && !callee->isDeclaration()) {
      auto fnIt = SummaryFacts.find(callee);
      if (fnIt == SummaryFacts.end()) {
        return Abi.Inputs.count(unit.Name) != 0;
      }
      auto regIt = fnIt->second.Registers.find(unit.Name);
      return regIt != fnIt->second.Registers.end() && regIt->second.ReadEntry;
    }
    return Abi.Inputs.count(unit.Name) != 0;
  }

  void markExternalCallArgumentStores() {
    std::vector<llvm::CallBase *> calls;
    for (llvm::Instruction &inst : llvm::instructions(Function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr || !isDirectExternalCall(*call)) {
        continue;
      }
      calls.push_back(call);
    }

    for (llvm::CallBase *call : calls) {
      if (call->getParent() == nullptr) {
        continue;
      }
      std::vector<CallArgStoreBinding> bindings = callArgStoreBindings(*call);
      if (bindings.empty()) {
        continue;
      }

      call = addCallArgValueBundle(*call, bindings);
      call->setMetadata("notdec.register.summary_ssa.call_args",
                        callArgsNode(bindings.size()));
      for (const CallArgStoreBinding &binding : bindings) {
        if (binding.Store != nullptr) {
          binding.Store->setMetadata(
              "notdec.register.summary_ssa.call_arg_store",
              callArgStoreNode(*binding.Unit, binding.Index));
          ++Summary.CallArgStoresMarked;
        }
      }
    }
  }

  llvm::CallBase *
  addCallArgValueBundle(llvm::CallBase &call,
                        const std::vector<CallArgStoreBinding> &bindings) {
    std::vector<llvm::Value *> values;
    values.reserve(bindings.size());
    for (const CallArgStoreBinding &binding : bindings) {
      values.push_back(binding.Value);
    }
    llvm::OperandBundleDef bundle(CallArgValuesBundleTag.str(), values);
    uint32_t tag =
        Function.getContext().getOrInsertBundleTag(CallArgValuesBundleTag)
            ->getValue();
    llvm::CallBase *newCall =
        llvm::CallBase::addOperandBundle(&call, tag, bundle, call.getIterator());
    if (!call.use_empty()) {
      call.replaceAllUsesWith(newCall);
      newCall->takeName(&call);
    }
    call.eraseFromParent();
    return newCall;
  }

  bool isDirectExternalCall(const llvm::CallBase &call) const {
    llvm::Function *callee = call.getCalledFunction();
    return callee != nullptr && callee->isDeclaration() &&
           !callee->isIntrinsic() && isAnalyzableCall(call);
  }

  const RegisterUnit *unitByName(llvm::StringRef name) const {
    for (const auto &[global, unit] : Units) {
      if (unit.Name == name) {
        return &unit;
      }
    }
    return nullptr;
  }

  std::vector<CallArgStoreBinding>
  callArgStoreBindings(llvm::CallBase &call) {
    std::vector<CallArgStoreBinding> bindings;
    for (const std::string &name : Abi.InputsInOrder) {
      const RegisterUnit *unit = unitByName(name);
      if (unit == nullptr) {
        break;
      }
      llvm::Value *value =
          resolve(readValueBefore(*call.getParent(), *unit, &call));
      if (value == nullptr || value->getType() != unit->Global->getValueType()) {
        break;
      }
      llvm::StoreInst *store = findStoreBeforeCall(call, *unit, value);
      if (isEntryInputValue(value) && store == nullptr) {
        break;
      }
      bindings.push_back(CallArgStoreBinding{
          store, unit, value, static_cast<unsigned>(bindings.size())});
    }
    return bindings;
  }

  llvm::StoreInst *findStoreBeforeCall(llvm::CallBase &call,
                                       const RegisterUnit &unit,
                                       llvm::Value *value) {
    for (auto it = call.getIterator(); it != call.getParent()->begin();) {
      --it;
      llvm::Instruction &inst = *it;
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && access.Unit->Global == unit.Global &&
            access.IsStorageValue) {
          return resolve(store->getValueOperand()) == value ? store : nullptr;
        }
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        RegisterAccess access = registerLoad(*load, Units);
        if (access.Unit != nullptr && access.Unit->Global == unit.Global &&
            access.IsStorageValue) {
          if (load->getMetadata("notdec.register.summary_ssa.replaced") ==
              nullptr) {
            return nullptr;
          }
        }
        continue;
      }
      if (auto *otherCall = llvm::dyn_cast<llvm::CallBase>(&inst)) {
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
    for (const auto &[global, load] : EntryInputs) {
      if (resolve(load) == value) {
        return true;
      }
    }
    return false;
  }

  llvm::Value *callValue(llvm::CallBase &call, const RegisterUnit &unit,
                         llvm::StringRef kind) {
    CallValueKey key{&call, unit.Global, kind.str()};
    if (auto cached = CallValues.find(key); cached != CallValues.end()) {
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
    llvm::CallInst *value = builder.CreateCall(callValueHelper(unit, kind), {},
                                               unit.Name + "." + kind.str());
    value->setMetadata("notdec.register.summary_ssa.call_value",
                       callValueNode(unit, kind, &call));
    CallValues.emplace(key, value);
    if (kind == "return") {
      ++Summary.CallReturnValues;
    } else {
      ++Summary.CallClobberValues;
    }
    return value;
  }

  llvm::FunctionCallee callValueHelper(const RegisterUnit &unit,
                                       llvm::StringRef kind) {
    llvm::Module *module = Function.getParent();
    llvm::Type *valueType = unit.Global->getValueType();
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

  llvm::MDNode *callValueNode(const RegisterUnit &unit, llvm::StringRef kind,
                              llvm::Instruction *call) const {
    uint64_t index = 0;
    for (const llvm::Instruction &inst : *call->getParent()) {
      if (&inst == call) {
        break;
      }
      ++index;
    }
    llvm::Metadata *fields[] = {
        llvm::MDString::get(Function.getContext(), "name=" + unit.Name),
        llvm::MDString::get(Function.getContext(), "kind=" + kind.str()),
        llvm::MDString::get(Function.getContext(),
                            "call_index=" + std::to_string(index)),
    };
    return llvm::MDNode::get(Function.getContext(), fields);
  }

  llvm::MDNode *callArgsNode(size_t count) const {
    llvm::Metadata *fields[] = {
        llvm::MDString::get(Function.getContext(),
                            "count=" + std::to_string(count)),
    };
    return llvm::MDNode::get(Function.getContext(), fields);
  }

  llvm::MDNode *callArgStoreNode(const RegisterUnit &unit,
                                 unsigned index) const {
    llvm::Metadata *fields[] = {
        llvm::MDString::get(Function.getContext(), "name=" + unit.Name),
        llvm::MDString::get(Function.getContext(),
                            "index=" + std::to_string(index)),
    };
    return llvm::MDNode::get(Function.getContext(), fields);
  }

  llvm::MDNode *markerNode(llvm::StringRef value) const {
    llvm::Metadata *fields[] = {
        llvm::MDString::get(Function.getContext(), value),
    };
    return llvm::MDNode::get(Function.getContext(), fields);
  }

  void attachMetadata() {
    uint64_t phisRemaining = 0;
    for (const auto &[key, phi] : PendingPhi) {
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
  total.EntryInputs += fn.EntryInputs;
  total.CallReturnValues += fn.CallReturnValues;
  total.CallClobberValues += fn.CallClobberValues;
  total.CallArgStoresMarked += fn.CallArgStoresMarked;
  total.PreservedCalls += fn.PreservedCalls;
  total.UnknownCallEffects += fn.UnknownCallEffects;
}

} // namespace

NativeRegisterSummarySSASummary
runNativeRegisterSummarySSA(llvm::Module &module,
                            const NativeRegisterSummarySSAOptions &options) {
  NativeRegisterSummaryOptions summaryOptions;
  summaryOptions.AttachMetadata = true;
  NativeRegisterSummary registerSummary =
      runNativeRegisterSummary(module, summaryOptions);
  std::map<llvm::Function *, FunctionSummaryFacts> facts =
      summaryFactsByFunction(registerSummary, module);
  std::map<llvm::GlobalVariable *, RegisterUnit> units =
      collectRegisterUnits(module);
  AbiFacts abi = collectAbiFacts(module);

  NativeRegisterSummarySSASummary summary;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    NativeRegisterSummarySSAFunctionSummary fn;
    FunctionBuilder builder(function, units, facts, abi, options, fn);
    builder.run();
    addFunctionSummary(summary, fn);
    summary.Functions.push_back(std::move(fn));
  }
  summary.FunctionsSeen = summary.Functions.size();
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
     << " entry_inputs=" << summary.EntryInputs
     << " call_returns=" << summary.CallReturnValues
     << " call_clobbers=" << summary.CallClobberValues
     << " call_arg_stores_marked=" << summary.CallArgStoresMarked
     << " preserved_calls=" << summary.PreservedCalls
     << " unknown_call_effects=" << summary.UnknownCallEffects << "\n";
  for (const NativeRegisterSummarySSAFunctionSummary &function :
       summary.Functions) {
    os << "  " << function.FunctionName << ": loads=" << function.LoadsSeen
       << " stores=" << function.StoresSeen
       << " loads_replaced=" << function.LoadsReplaced
       << " dead_loads_removed=" << function.DeadLoadsRemoved
       << " dead_stores_removed=" << function.DeadStoresRemoved
       << " phis_created=" << function.PhisCreated
       << " phis_simplified=" << function.PhisSimplified
       << " entry_inputs=" << function.EntryInputs
       << " call_returns=" << function.CallReturnValues
       << " call_clobbers=" << function.CallClobberValues
       << " call_arg_stores_marked=" << function.CallArgStoresMarked
       << " preserved_calls=" << function.PreservedCalls
       << " unknown_call_effects=" << function.UnknownCallEffects << "\n";
  }
}

} // namespace notdec::bin2llvm
