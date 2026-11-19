#include "RegisterSummaryInternal.h"

#include "notdec-bin2llvm/passes/register/NativeRegisterValueDce.h"

namespace notdec::bin2llvm {

Analyzer::Analyzer(llvm::Module &module, NativeRegisterSummaryOptions options)
      : Module(module), Options(std::move(options)),
        Units(collectRegisterUnits(module)),
        UnitsByName(registersByName(Units)),
        Abi(collectSummaryAbiFacts(module, Units)) {
    for (llvm::Function &function : Module) {
      if (!function.isDeclaration()) {
        Functions.push_back(&function);
      }
    }
    buildCallGraph();
    collectCrossBlockValues();
  }

NativeRegisterSummary Analyzer::run() {
    runBottomUp();
    if (Options.CollectExternalCallsiteEvidence) {
      collectExternalCallsiteEvidence();
    }
    if (Options.RunTopDownDemand) {
      // 可观察性分析：先算出"入口值有没有人真正观察"，再跑 demand。
      // 目前只打印（阶段 1），还没有用它裁剪 ReadEntry；诊断输出跟
      // Options.PrintSummary（--register-ssa-summary）走，普通运行保持安静。
      computeUsedInputs();
      if (Options.PrintSummary && !Options.PruneDeadRegisterValues) {
        reportUnobservableInputs();
      }
      runTopDownDemand();
      if (Options.PrintSummary && !Options.PruneDeadRegisterValues) {
        reportWriteObservations();
        reportDemandOrigins();
      }
      if (Options.PruneDeadRegisterValues) {
        // 寄存器感知 DCE：观察位为 0 的写没人读，把写入值换成 0 并删掉因此
        // 没人用的计算链。IR 变了，Effects/ReadEntry 都依赖这些读，所以重新
        // 跑一遍完整分析（递归一次，PruneDeadRegisterValues=false）。
        NativeRegisterValueDceSummary dce =
            pruneDeadRegisterValues(Module, collectDeadRegisterWrites());
        if (Options.PrintSummary) {
          printNativeRegisterValueDceSummary(dce, llvm::errs());
        }
        NativeRegisterSummaryOptions next = Options;
        next.PruneDeadRegisterValues = false;
        return Analyzer(Module, next).run();
      }
    }
    if (Options.AttachMetadata) {
      attachMetadata();
    }
    return buildPublicSummary();
  }

void Analyzer::buildCallGraph() {
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

void Analyzer::collectCrossBlockValues() {
    for (llvm::Function *function : Functions) {
      for (llvm::Instruction &inst : llvm::instructions(function)) {
        llvm::BasicBlock *parent = inst.getParent();
        for (llvm::User *user : inst.users()) {
          auto *userInst = llvm::dyn_cast<llvm::Instruction>(user);
          if (userInst != nullptr && userInst->getParent() != parent) {
            CrossBlockValues.insert(&inst);
            break;
          }
        }
      }
    }
  }

std::vector<std::vector<llvm::Function *>> Analyzer::sccs() const {
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

void Analyzer::runBottomUp() {
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
          FunctionFlow flow = solveFunction(*function);
          FunctionEffect next = std::move(flow.Effect);
          if (!(Effects[function] == next)) {
            Effects[function] = std::move(next);
            changed = true;
          }
          if (Options.CollectExternalCallsiteEvidence) {
            StableFlows[function] = std::move(flow);
          }
        }
      }
      done.insert(id);
    };

    for (unsigned id = 0; id < components.size(); ++id) {
      process(id);
    }
  }

FunctionFlow Analyzer::solveFunction(llvm::Function &function) {
    FunctionFlow flow;
    flow.In[&function.getEntryBlock()].Reachable = true;

    // 工作队列：只重算"前驱出口变了"的块。原来是 while(changed){for each block}，
    // 每轮把所有块从头转移一遍，对长链/大 CFG 是 轮数 × 块数（实测 wolfssl sp_mul
    // 5 轮 × 98 块；每块拷贝一次 State，大函数 1.5ms/次，lighttpd 平均 0.14ms/次）。
    // 转移函数对各 Cell 位是单调的 gen/kill，工作队列在同一个格上做混沌迭代，收敛到
    // 同一个不动点；顺序用 FIFO，和原来的函数序更接近。
    std::deque<llvm::BasicBlock *> worklist;
    llvm::SmallPtrSet<llvm::BasicBlock *, 32> queued;
    auto enqueue = [&](llvm::BasicBlock *block) {
      if (queued.insert(block).second) {
        worklist.push_back(block);
      }
    };
    for (llvm::BasicBlock &block : function) {
      enqueue(&block);
    }

    while (!worklist.empty()) {
      llvm::BasicBlock &block = *worklist.front();
      worklist.pop_front();
      queued.erase(&block);
      if (&block != &function.getEntryBlock()) {
        State joined;
        for (llvm::BasicBlock *pred : llvm::predecessors(&block)) {
          joinState(joined, flow.Out[pred]);
        }
        flow.In[&block] = std::move(joined);
      }
      State next = transferBlock(block, flow.In[&block]);
      bool outputChanged = Options.CollectExternalCallsiteEvidence
                               ? !(flow.Out[&block] == next)
                               : !flow.Out[&block].sameSummaryLattice(next);
      flow.Out[&block] = std::move(next);
      if (outputChanged) {
        for (llvm::BasicBlock *succ : llvm::successors(&block)) {
          enqueue(succ);
        }
      }
    }

    State exits;
    bool sawNoReturnExit = false;
    std::map<llvm::GlobalVariable *, Cell> noReturnReads;
    for (llvm::BasicBlock &block : function) {
      if (llvm::isa<llvm::ReturnInst>(block.getTerminator())) {
        joinState(exits, exitStateWithSavedRegisters(flow.Out[&block]));
      }
      const State &out = flow.Out[&block];
      if (out.EndsInNoReturn) {
        sawNoReturnExit = true;
        joinReadEntryOnly(noReturnReads, out);
      }
    }
    if (exits.Reachable) {
      flow.Effect.Registers = std::move(exits.Cells);
      for (const auto &[global, cell] : noReturnReads) {
        if (cell.ReadEntry.getBitWidth() == 0 || cell.ReadEntry.isZero()) {
          continue;
        }
        auto [it, inserted] =
            flow.Effect.Registers.try_emplace(global, defaultCell(global));
        if (inserted) {
          it->second.ReadEntry = cell.ReadEntry;
        } else {
          it->second.ReadEntry |= cell.ReadEntry;
        }
      }
    } else if (sawNoReturnExit) {
      flow.Effect.Registers = std::move(noReturnReads);
      flow.Effect.NoReturn = true;
    }
    return flow;
  }

const NativeExternalCallShape *
  Analyzer::externalCallShape(const llvm::CallBase &call) const {
    auto callsiteIt = Options.ExternalCallsiteShapes.find(&call);
    if (callsiteIt != Options.ExternalCallsiteShapes.end()) {
      return &callsiteIt->second;
    }
    llvm::Function *callee = call.getCalledFunction();
    if (callee == nullptr) {
      return nullptr;
    }
    auto shapeIt = Options.ExternalCallShapes.find(callee->getName().str());
    return shapeIt == Options.ExternalCallShapes.end() ? nullptr
                                                       : &shapeIt->second;
  }

bool Analyzer::isUnknownExternalCall(const llvm::CallBase &call) const {
    llvm::Function *callee = call.getCalledFunction();
    if (callee != nullptr &&
        (!callee->isDeclaration() || callee->isIntrinsic())) {
      return false;
    }
    if (isNotDecRegisterHelperCall(call)) {
      return false;
    }
    return externalCallShape(call) == nullptr;
  }

const NativeExternalCallShape *
  Analyzer::knownVarArgExternalShape(const llvm::CallBase &call) const {
    llvm::Function *callee = call.getCalledFunction();
    if (callee == nullptr || callee->isIntrinsic()) {
      return nullptr;
    }
    auto shapeIt = Options.ExternalCallShapes.find(callee->getName().str());
    if (shapeIt == Options.ExternalCallShapes.end() ||
        !shapeIt->second.VarArg) {
      return nullptr;
    }
    return &shapeIt->second;
  }

bool Analyzer::loadValueIsMovedAway(const llvm::LoadInst &load) const {
    llvm::SmallVector<const llvm::Value *, 8> worklist{&load};
    llvm::SmallPtrSet<const llvm::Value *, 8> seen;
    unsigned budget = 16;
    while (!worklist.empty() && budget-- != 0) {
      const llvm::Value *value = worklist.pop_back_val();
      if (!seen.insert(value).second) {
        continue;
      }
      for (const llvm::User *user : value->users()) {
        if (llvm::isa<llvm::StoreInst>(user)) {
          return true;
        }
        if (auto *cast = llvm::dyn_cast<llvm::CastInst>(user)) {
          worklist.push_back(cast);
        }
      }
    }
    return false;
  }

bool Analyzer::registerValuePendingAtCall(const llvm::GlobalVariable &global,
                                  const llvm::CallBase &call) const {
    const llvm::BasicBlock *block = call.getParent();
    if (block == nullptr) {
      return true;
    }
    for (auto it = call.getIterator(); it != block->begin();) {
      --it;
      const llvm::Instruction &instruction = *it;
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        if (load->getPointerOperand()->stripPointerCasts() == &global) {
          if (!loadValueIsMovedAway(*load)) {
            continue;
          }
          return false;
        }
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        if (store->getPointerOperand()->stripPointerCasts() == &global) {
          return true;
        }
      }
    }
    return true;
  }

const FunctionDemand Analyzer::EmptyDemand = {};

} // namespace notdec::bin2llvm
