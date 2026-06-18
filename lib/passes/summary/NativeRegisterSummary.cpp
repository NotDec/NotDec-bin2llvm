#include "notdec-bin2llvm/passes/summary/NativeRegisterSummary.h"

#include "notdec-bin2llvm/NativeAbi.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace notdec::bin2llvm {
namespace {

struct RegisterUnit {
  llvm::GlobalVariable *Global = nullptr;
  std::string Name;
};

struct RegisterAccess {
  const RegisterUnit *Unit = nullptr;
};

// Forward abstract state for one backing register. Missing map entries use this
// default value, but only inside a reachable block state.
struct Cell {
  bool MayEntry = true;
  bool MayNonEntry = false;
  bool ReadEntry = false;

  bool operator==(const Cell &other) const {
    return MayEntry == other.MayEntry && MayNonEntry == other.MayNonEntry &&
           ReadEntry == other.ReadEntry;
  }
};

struct State {
  bool Reachable = false;
  std::map<llvm::GlobalVariable *, Cell> Cells;

  bool operator==(const State &other) const {
    return Reachable == other.Reachable && Cells == other.Cells;
  }
};

// Bottom-up summary: the CFG-level effect visible at normal function exits.
struct FunctionEffect {
  std::map<llvm::GlobalVariable *, Cell> Registers;

  bool operator==(const FunctionEffect &other) const {
    return Registers == other.Registers;
  }
};

// Top-down summary: which changed exit registers are actually observed by
// callers, plus an audit value for entry registers that feed those
// observations.
struct FunctionDemand {
  std::set<llvm::GlobalVariable *> ExitDemand;
  std::set<llvm::GlobalVariable *> EntryDemand;

  bool operator==(const FunctionDemand &other) const {
    return ExitDemand == other.ExitDemand && EntryDemand == other.EntryDemand;
  }
};

// ABI fallback used for external and indirect calls. OutputOrder keeps the
// prototype order so root demand can seed only the regular first return value.
struct AbiFacts {
  std::set<std::string> Inputs;
  std::set<std::string> Outputs;
  std::vector<std::string> OutputOrder;
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

std::map<std::string, llvm::GlobalVariable *>
registersByName(const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  std::map<std::string, llvm::GlobalVariable *> byName;
  for (const auto &[global, unit] : units) {
    byName.emplace(unit.Name, global);
  }
  return byName;
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
  return RegisterAccess{&it->second};
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
  return RegisterAccess{&it->second};
}

bool isNotDecRegisterHelperCall(const llvm::CallBase &call) {
  llvm::Function *callee = call.getCalledFunction();
  return callee != nullptr && callee->getName().starts_with("notdec.register.");
}

bool isAnalyzableCall(const llvm::Instruction &inst) {
  auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
  if (call == nullptr || isNotDecRegisterHelperCall(*call)) {
    return false;
  }
  llvm::Function *callee = call->getCalledFunction();
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
      facts.Inputs.insert(entry.Storage.Name);
    }
  }
  for (const NativeAbiParamEntry &entry : abi->Outputs) {
    if (entry.Storage.Kind == NativeAbiStorageKind::Register &&
        !entry.Storage.Name.empty()) {
      facts.Outputs.insert(entry.Storage.Name);
      facts.OutputOrder.push_back(entry.Storage.Name);
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

Cell &cellFor(State &state, llvm::GlobalVariable *global) {
  return state.Cells[global];
}

Cell cellIn(const State &state, llvm::GlobalVariable *global) {
  auto it = state.Cells.find(global);
  if (it == state.Cells.end()) {
    return Cell{};
  }
  return it->second;
}

Cell cellIn(const FunctionEffect &effect, llvm::GlobalVariable *global) {
  auto it = effect.Registers.find(global);
  if (it == effect.Registers.end()) {
    return Cell{};
  }
  return it->second;
}

bool joinCell(Cell &lhs, const Cell &rhs) {
  Cell old = lhs;
  lhs.MayEntry = lhs.MayEntry || rhs.MayEntry;
  lhs.MayNonEntry = lhs.MayNonEntry || rhs.MayNonEntry;
  lhs.ReadEntry = lhs.ReadEntry || rhs.ReadEntry;
  return !(lhs == old);
}

bool joinState(State &target, const State &source) {
  if (!source.Reachable) {
    return false;
  }
  if (!target.Reachable) {
    target = source;
    return true;
  }
  bool changed = false;
  for (const auto &[global, cell] : source.Cells) {
    changed |= joinCell(target.Cells[global], cell);
  }
  // Sparse maps treat missing cells as untouched. If target already contains a
  // register and source omits it, the join still has to include that default
  // untouched path; otherwise predecessor order would change the result.
  for (auto &[global, cell] : target.Cells) {
    if (source.Cells.count(global) == 0) {
      changed |= joinCell(cell, Cell{});
    }
  }
  return changed;
}

void readRegister(State &state, llvm::GlobalVariable *global) {
  Cell &cell = cellFor(state, global);
  cell.ReadEntry = cell.ReadEntry || cell.MayEntry;
}

void writeRegister(State &state, llvm::GlobalVariable *global) {
  Cell &cell = cellFor(state, global);
  cell.MayEntry = false;
  cell.MayNonEntry = true;
}

bool isIgnored(const RegisterUnit &unit,
               const NativeRegisterSummaryOptions &options) {
  return options.IgnoredRegisters.count(unit.Name) != 0;
}

class Analyzer {
public:
  Analyzer(llvm::Module &module, NativeRegisterSummaryOptions options)
      : Module(module), Options(std::move(options)),
        Units(collectRegisterUnits(module)),
        UnitsByName(registersByName(Units)), Abi(collectAbiFacts(module)) {
    for (llvm::Function &function : Module) {
      if (!function.isDeclaration()) {
        Functions.push_back(&function);
      }
    }
    buildCallGraph();
  }

  NativeRegisterSummary run() {
    runBottomUp();
    runTopDownDemand();
    if (Options.AttachMetadata) {
      attachMetadata();
    }
    return buildPublicSummary();
  }

private:
  llvm::Module &Module;
  NativeRegisterSummaryOptions Options;
  std::map<llvm::GlobalVariable *, RegisterUnit> Units;
  std::map<std::string, llvm::GlobalVariable *> UnitsByName;
  AbiFacts Abi;
  std::vector<llvm::Function *> Functions;
  std::map<llvm::Function *, std::set<llvm::Function *>> Calls;
  std::map<llvm::Function *, std::set<llvm::Function *>> Callers;
  std::map<llvm::Function *, FunctionEffect> Effects;
  std::map<llvm::Function *, FunctionDemand> Demands;

  void buildCallGraph() {
    std::set<llvm::Function *> defined(Functions.begin(), Functions.end());
    for (llvm::Function *function : Functions) {
      for (llvm::BasicBlock &block : *function) {
        for (llvm::Instruction &inst : block) {
          auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
          if (call == nullptr || isNotDecRegisterHelperCall(*call)) {
            continue;
          }
          llvm::Function *callee = call->getCalledFunction();
          if (callee == nullptr || defined.count(callee) == 0) {
            continue;
          }
          Calls[function].insert(callee);
          Callers[callee].insert(function);
        }
      }
    }
  }

  std::vector<std::vector<llvm::Function *>> sccs() const {
    std::vector<std::vector<llvm::Function *>> result;
    std::map<llvm::Function *, unsigned> index;
    std::map<llvm::Function *, unsigned> lowlink;
    std::vector<llvm::Function *> stack;
    std::set<llvm::Function *> onStack;
    unsigned nextIndex = 0;

    std::function<void(llvm::Function *)> visit = [&](llvm::Function *fn) {
      index[fn] = nextIndex;
      lowlink[fn] = nextIndex;
      ++nextIndex;
      stack.push_back(fn);
      onStack.insert(fn);

      auto edgeIt = Calls.find(fn);
      if (edgeIt != Calls.end()) {
        for (llvm::Function *callee : edgeIt->second) {
          if (index.count(callee) == 0) {
            visit(callee);
            lowlink[fn] = std::min(lowlink[fn], lowlink[callee]);
          } else if (onStack.count(callee) != 0) {
            lowlink[fn] = std::min(lowlink[fn], index[callee]);
          }
        }
      }

      if (lowlink[fn] != index[fn]) {
        return;
      }
      std::vector<llvm::Function *> component;
      while (!stack.empty()) {
        llvm::Function *member = stack.back();
        stack.pop_back();
        onStack.erase(member);
        component.push_back(member);
        if (member == fn) {
          break;
        }
      }
      result.push_back(std::move(component));
    };

    for (llvm::Function *function : Functions) {
      if (index.count(function) == 0) {
        visit(function);
      }
    }
    return result;
  }

  void runBottomUp() {
    std::vector<std::vector<llvm::Function *>> components = sccs();
    std::map<llvm::Function *, unsigned> componentOf;
    for (unsigned id = 0; id < components.size(); ++id) {
      for (llvm::Function *function : components[id]) {
        componentOf[function] = id;
      }
    }

    std::set<unsigned> done;
    std::function<void(unsigned)> process = [&](unsigned id) {
      if (done.count(id) != 0) {
        return;
      }
      for (llvm::Function *function : components[id]) {
        auto it = Calls.find(function);
        if (it == Calls.end()) {
          continue;
        }
        for (llvm::Function *callee : it->second) {
          unsigned calleeId = componentOf[callee];
          if (calleeId != id) {
            process(calleeId);
          }
        }
      }

      bool changed = true;
      while (changed) {
        changed = false;
        for (llvm::Function *function : components[id]) {
          FunctionEffect next = analyzeFunction(*function);
          if (!(Effects[function] == next)) {
            Effects[function] = std::move(next);
            changed = true;
          }
        }
      }
      done.insert(id);
    };

    for (unsigned id = 0; id < components.size(); ++id) {
      process(id);
    }
  }

  FunctionEffect analyzeFunction(llvm::Function &function) {
    std::map<llvm::BasicBlock *, State> in;
    std::map<llvm::BasicBlock *, State> out;
    in[&function.getEntryBlock()].Reachable = true;

    bool changed = true;
    while (changed) {
      changed = false;
      for (llvm::BasicBlock &block : function) {
        if (&block != &function.getEntryBlock()) {
          State joined;
          for (llvm::BasicBlock *pred : llvm::predecessors(&block)) {
            joinState(joined, out[pred]);
          }
          if (!(in[&block] == joined)) {
            in[&block] = std::move(joined);
            changed = true;
          }
        }
        State next = transferBlock(block, in[&block]);
        if (!(out[&block] == next)) {
          out[&block] = std::move(next);
          changed = true;
        }
      }
    }

    State exits;
    for (llvm::BasicBlock &block : function) {
      if (llvm::isa<llvm::ReturnInst>(block.getTerminator())) {
        joinState(exits, out[&block]);
      }
    }
    FunctionEffect effect;
    if (exits.Reachable) {
      effect.Registers = std::move(exits.Cells);
    }
    return effect;
  }

  State transferBlock(llvm::BasicBlock &block, State state) {
    if (!state.Reachable) {
      return state;
    }
    for (llvm::Instruction &inst : block) {
      transferInstruction(inst, state);
    }
    return state;
  }

  void transferInstruction(llvm::Instruction &inst, State &state) {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
      RegisterAccess access = registerLoad(*load, Units);
      if (access.Unit != nullptr && !isIgnored(*access.Unit, Options)) {
        readRegister(state, access.Unit->Global);
      }
      return;
    }
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      RegisterAccess access = registerStore(*store, Units);
      if (access.Unit != nullptr && !isIgnored(*access.Unit, Options)) {
        writeRegister(state, access.Unit->Global);
      }
      return;
    }
    if (isAnalyzableCall(inst)) {
      transferCall(llvm::cast<llvm::CallBase>(inst), state);
    }
  }

  void transferCall(llvm::CallBase &call, State &state) {
    llvm::Function *callee = call.getCalledFunction();
    if (callee != nullptr && !callee->isDeclaration() &&
        Effects.count(callee) != 0) {
      applyFunctionEffect(Effects[callee], state);
      return;
    }
    applyAbiCallEffect(state);
  }

  void applyFunctionEffect(const FunctionEffect &effect, State &state) {
    for (const auto &[global, unit] : Units) {
      if (isIgnored(unit, Options)) {
        continue;
      }
      Cell pre = cellIn(state, global);
      Cell callee = cellIn(effect, global);
      Cell post;
      post.ReadEntry = pre.ReadEntry || (callee.ReadEntry && pre.MayEntry);
      post.MayEntry = callee.MayEntry && pre.MayEntry;
      post.MayNonEntry =
          (callee.MayEntry && pre.MayNonEntry) || callee.MayNonEntry;
      state.Cells[global] = post;
    }
  }

  void applyAbiCallEffect(State &state) {
    for (const auto &[global, unit] : Units) {
      if (isIgnored(unit, Options)) {
        continue;
      }
      if (Abi.Inputs.count(unit.Name) != 0) {
        readRegister(state, global);
      }
      if (Abi.Unaffected.count(unit.Name) != 0) {
        continue;
      }
      if (Abi.Outputs.count(unit.Name) != 0 ||
          Abi.KilledByCall.count(unit.Name) != 0) {
        writeRegister(state, global);
      }
    }
  }

  void runTopDownDemand() {
    for (llvm::Function *function : Functions) {
      if (Callers[function].empty()) {
        seedAbiReturns(Demands[function]);
      }
    }

    bool changed = true;
    while (changed) {
      changed = false;
      for (llvm::Function *function : Functions) {
        std::map<llvm::Function *, std::set<llvm::GlobalVariable *>> additions =
            analyzeCallerDemand(*function);
        for (auto &[callee, registers] : additions) {
          std::set<llvm::GlobalVariable *> &demand = Demands[callee].ExitDemand;
          size_t oldSize = demand.size();
          demand.insert(registers.begin(), registers.end());
          changed |= demand.size() != oldSize;
        }
      }
    }
  }

  void seedAbiReturns(FunctionDemand &demand) const {
    if (Abi.OutputOrder.empty()) {
      return;
    }
    auto it = UnitsByName.find(Abi.OutputOrder.front());
    if (it != UnitsByName.end()) {
      demand.ExitDemand.insert(it->second);
    }
  }

  std::map<llvm::Function *, std::set<llvm::GlobalVariable *>>
  analyzeCallerDemand(llvm::Function &function) {
    std::map<llvm::BasicBlock *, std::set<llvm::GlobalVariable *>> in;
    std::map<llvm::BasicBlock *, std::set<llvm::GlobalVariable *>> out;
    std::map<llvm::Function *, std::set<llvm::GlobalVariable *>> additions;

    bool changed = true;
    while (changed) {
      changed = false;
      for (llvm::BasicBlock &block : llvm::reverse(function)) {
        std::set<llvm::GlobalVariable *> liveOut;
        for (llvm::BasicBlock *succ : llvm::successors(&block)) {
          liveOut.insert(in[succ].begin(), in[succ].end());
        }
        if (llvm::isa<llvm::ReturnInst>(block.getTerminator())) {
          const std::set<llvm::GlobalVariable *> &exit =
              Demands[&function].ExitDemand;
          liveOut.insert(exit.begin(), exit.end());
        }
        std::set<llvm::GlobalVariable *> live =
            liveBeforeBlock(block, liveOut, additions);
        if (out[&block] != liveOut) {
          out[&block] = std::move(liveOut);
          changed = true;
        }
        if (in[&block] != live) {
          in[&block] = std::move(live);
          changed = true;
        }
      }
    }
    Demands[&function].EntryDemand = in[&function.getEntryBlock()];
    return additions;
  }

  std::set<llvm::GlobalVariable *> liveBeforeBlock(
      llvm::BasicBlock &block, std::set<llvm::GlobalVariable *> live,
      std::map<llvm::Function *, std::set<llvm::GlobalVariable *>> &additions) {
    for (llvm::Instruction &inst : llvm::reverse(block)) {
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (!isNotDecRegisterHelperCall(*call)) {
          applyBackwardCallDemand(*call, live, additions);
        }
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && !isIgnored(*access.Unit, Options)) {
          live.erase(access.Unit->Global);
        }
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        RegisterAccess access = registerLoad(*load, Units);
        if (access.Unit != nullptr && !isIgnored(*access.Unit, Options)) {
          live.insert(access.Unit->Global);
        }
      }
    }
    return live;
  }

  void applyBackwardCallDemand(
      llvm::CallBase &call, std::set<llvm::GlobalVariable *> &live,
      std::map<llvm::Function *, std::set<llvm::GlobalVariable *>> &additions) {
    llvm::Function *callee = call.getCalledFunction();
    if (callee != nullptr && !callee->isDeclaration() &&
        Effects.count(callee) != 0) {
      const FunctionEffect &effect = Effects[callee];
      for (const auto &[global, unit] : Units) {
        if (isIgnored(unit, Options) || live.count(global) == 0) {
          continue;
        }
        Cell calleeCell = cellIn(effect, global);
        if (calleeCell.MayNonEntry) {
          additions[callee].insert(global);
        }
        if (!calleeCell.MayEntry) {
          live.erase(global);
        }
      }
      return;
    }

    for (const auto &[global, unit] : Units) {
      if (isIgnored(unit, Options) || live.count(global) == 0) {
        continue;
      }
      if (Abi.Outputs.count(unit.Name) != 0 ||
          Abi.KilledByCall.count(unit.Name) != 0) {
        live.erase(global);
      }
    }
  }

  llvm::MDNode *registerNode(llvm::LLVMContext &context,
                             const RegisterUnit &unit) const {
    llvm::Metadata *fields[] = {
        llvm::MDString::get(context, "name=" + unit.Name),
    };
    return llvm::MDNode::get(context, fields);
  }

  llvm::MDNode *registerSummaryNode(llvm::LLVMContext &context,
                                    llvm::GlobalVariable *global,
                                    const Cell &cell, bool exitDemand) const {
    const RegisterUnit &unit = Units.at(global);
    llvm::Metadata *fields[] = {
        llvm::MDString::get(context, "name=" + unit.Name),
        llvm::MDString::get(context, std::string("read_entry=") +
                                         (cell.ReadEntry ? "true" : "false")),
        llvm::MDString::get(context, std::string("may_entry=") +
                                         (cell.MayEntry ? "true" : "false")),
        llvm::MDString::get(context, std::string("may_non_entry=") +
                                         (cell.MayNonEntry ? "true" : "false")),
        llvm::MDString::get(context, std::string("exit_demand=") +
                                         (exitDemand ? "true" : "false")),
    };
    return llvm::MDNode::get(context, fields);
  }

  void attachMetadata() {
    llvm::LLVMContext &context = Module.getContext();
    for (llvm::Function *function : Functions) {
      std::vector<llvm::Metadata *> all;
      std::vector<llvm::Metadata *> reads;
      std::vector<llvm::Metadata *> preserves;
      std::vector<llvm::Metadata *> modifies;
      std::vector<llvm::Metadata *> demandedReturns;
      FunctionEffect &effect = Effects[function];
      FunctionDemand &demand = Demands[function];
      for (const auto &[global, unit] : Units) {
        if (isIgnored(unit, Options)) {
          continue;
        }
        Cell cell = cellIn(effect, global);
        bool exitDemand = demand.ExitDemand.count(global) != 0;
        all.push_back(registerSummaryNode(context, global, cell, exitDemand));
        if (cell.ReadEntry) {
          reads.push_back(registerNode(context, unit));
        }
        if (cell.MayEntry && !cell.MayNonEntry) {
          preserves.push_back(registerNode(context, unit));
        }
        if (cell.MayNonEntry) {
          modifies.push_back(registerNode(context, unit));
        }
        if (exitDemand && cell.MayNonEntry &&
            Abi.Outputs.count(unit.Name) != 0) {
          demandedReturns.push_back(registerNode(context, unit));
        }
      }
      function->setMetadata("notdec.register.summary",
                            llvm::MDNode::get(context, all));
      function->setMetadata("notdec.register.summary.read_entry",
                            llvm::MDNode::get(context, reads));
      function->setMetadata("notdec.register.summary.preserves",
                            llvm::MDNode::get(context, preserves));
      function->setMetadata("notdec.register.summary.modifies",
                            llvm::MDNode::get(context, modifies));
      function->setMetadata("notdec.register.summary.demanded_returns",
                            llvm::MDNode::get(context, demandedReturns));
    }
  }

  NativeRegisterSummary buildPublicSummary() const {
    NativeRegisterSummary summary;
    summary.FunctionsSeen = Functions.size();
    for (llvm::Function *function : Functions) {
      NativeRegisterSummaryFunction fn;
      fn.FunctionName = function->getName().str();
      for (llvm::BasicBlock &block : *function) {
        for (llvm::Instruction &inst : block) {
          if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
            RegisterAccess access = registerLoad(*load, Units);
            if (access.Unit != nullptr && !isIgnored(*access.Unit, Options)) {
              ++fn.LoadsSeen;
            }
          } else if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
            RegisterAccess access = registerStore(*store, Units);
            if (access.Unit != nullptr && !isIgnored(*access.Unit, Options)) {
              ++fn.StoresSeen;
            }
          } else if (isAnalyzableCall(inst)) {
            ++fn.CallsSeen;
          }
        }
      }

      const FunctionEffect &effect = Effects.at(function);
      const FunctionDemand &demand =
          Demands.count(function) == 0 ? EmptyDemand : Demands.at(function);
      for (const auto &[global, unit] : Units) {
        if (isIgnored(unit, Options)) {
          continue;
        }
        Cell cell = cellIn(effect, global);
        bool exitDemand = demand.ExitDemand.count(global) != 0;
        NativeRegisterSummaryRegister reg;
        reg.Name = unit.Name;
        reg.ReadEntry = cell.ReadEntry;
        reg.MayEntry = cell.MayEntry;
        reg.MayNonEntry = cell.MayNonEntry;
        reg.ExitDemand = exitDemand;
        fn.Registers.push_back(reg);
        if (cell.ReadEntry) {
          ++fn.ReadEntryRegisters;
        }
        if (cell.MayNonEntry) {
          ++fn.ModifiedRegisters;
        }
        if (cell.MayEntry && !cell.MayNonEntry) {
          ++fn.PreservedRegisters;
        }
        if (exitDemand && cell.MayNonEntry && Abi.Outputs.count(unit.Name)) {
          ++fn.DemandedReturns;
        }
      }
      summary.LoadsSeen += fn.LoadsSeen;
      summary.StoresSeen += fn.StoresSeen;
      summary.CallsSeen += fn.CallsSeen;
      summary.ReadEntryRegisters += fn.ReadEntryRegisters;
      summary.ModifiedRegisters += fn.ModifiedRegisters;
      summary.PreservedRegisters += fn.PreservedRegisters;
      summary.DemandedReturns += fn.DemandedReturns;
      summary.Functions.push_back(std::move(fn));
    }
    return summary;
  }

  static const FunctionDemand EmptyDemand;
};

const FunctionDemand Analyzer::EmptyDemand = {};

} // namespace

NativeRegisterSummary
runNativeRegisterSummary(llvm::Module &module,
                         const NativeRegisterSummaryOptions &options) {
  Analyzer analyzer(module, options);
  NativeRegisterSummary summary = analyzer.run();
  if (options.PrintSummary) {
    printNativeRegisterSummary(summary, llvm::errs());
  }
  return summary;
}

void printNativeRegisterSummary(const NativeRegisterSummary &summary,
                                llvm::raw_ostream &os) {
  os << "Native register summary: functions=" << summary.FunctionsSeen
     << " loads=" << summary.LoadsSeen << " stores=" << summary.StoresSeen
     << " calls=" << summary.CallsSeen
     << " read_entry=" << summary.ReadEntryRegisters
     << " modified=" << summary.ModifiedRegisters
     << " preserved=" << summary.PreservedRegisters
     << " demanded_returns=" << summary.DemandedReturns << "\n";
  for (const NativeRegisterSummaryFunction &function : summary.Functions) {
    os << "  " << function.FunctionName << ": loads=" << function.LoadsSeen
       << " stores=" << function.StoresSeen << " calls=" << function.CallsSeen
       << " read_entry=" << function.ReadEntryRegisters
       << " modified=" << function.ModifiedRegisters
       << " preserved=" << function.PreservedRegisters
       << " demanded_returns=" << function.DemandedReturns << "\n";
  }
}

} // namespace notdec::bin2llvm
