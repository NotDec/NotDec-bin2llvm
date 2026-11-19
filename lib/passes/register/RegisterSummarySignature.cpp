#include "RegisterSummaryInternal.h"

namespace notdec::bin2llvm {

namespace {
// 临时诊断（NOTDEC_TRACE_ZMM0_DEMAND=1）：打印 ZMM0 需求的"种子读"和
// "沿调用边传给 callee 的退出需求"，用来回答"这个 128 位需求到底从哪来"。
bool traceZmm0Demand() {
  static const bool enabled =
      ::getenv("NOTDEC_TRACE_ZMM0_DEMAND") != nullptr;
  return enabled;
}
} // namespace

RegisterDemand
  Analyzer::liveBeforeBlock(llvm::BasicBlock &block, RegisterDemand live,
                  std::map<llvm::Function *, RegisterDemand> &additions,
                  const std::map<llvm::Value *, llvm::APInt> &valueDemands) {
    for (llvm::Instruction &inst : llvm::reverse(block)) {
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (std::optional<NativeRegisterPartialReadInfo> partial =
                parseNativeRegisterPartialRead(*call)) {
          // 只喂给"没人读"的帧槽的死读不产生需求（见 helper 注释）。
          if (!partialReadFeedsOnlyDeadFrameSlots(*call)) {
            // valueDemand 是"读结果"的按位需求（结果坐标，从 bit 0 起）；寄存器
            // 需求要放到 partial->BitOffset 处。少了这个移位，读 DH（offset=8）
            // 会把需求记到 DL 上，调用点之后真正被读的位永远进不了 callee 的
            // ExitDemand，签名就不给返回槽 → call_effect_mixed unknown。
            llvm::APInt mask = valueDemand(call, valueDemands);
            if (partial->BitOffset != 0 && mask.getBitWidth() != 0) {
              mask = mask.zext(mask.getBitWidth() + partial->BitOffset)
                         .shl(partial->BitOffset);
            }
            addDemand(live, partial->Global, mask);
            if (traceZmm0Demand()) {
              auto unitIt = Units.find(partial->Global);
              if (unitIt != Units.end() && unitIt->second.Name == "ZMM0") {
                llvm::errs() << "ZMM0READ fn=" << block.getParent()->getName()
                             << " bb=" << block.getName() << " off="
                             << partial->BitOffset << " width="
                             << partial->ReadWidth << " mask="
                             << maskToHex(mask) << "\n";
              }
            }
          }
          continue;
        }
        if (std::optional<NativeRegisterPartialWriteInfo> partial =
                parseNativeRegisterPartialWrite(*call)) {
          auto unitIt = Units.find(partial->Global);
          if (unitIt != Units.end() &&
              isX64Low32GprWrite(*partial, unitIt->second)) {
            eraseDemand(live, partial->Global);
          } else {
            // partial write 定义写入位：擦除这些位的 live demand（写覆盖旧
            // 需求），未写位保留（读改写需要旧值）。写入值自己的位需求由
            // value 链上的显式 load 追踪，不能反过来加到 global 上——否则
            // 调用点之后的传参写（如 lua_pushnumber 的 double 参数写 ZMM0）
            // 会把 ZMM0 需求误传给更早的 callee，给不返回 double 的函数
            // 误加 ZMM0 返回槽。
            llvm::APInt writeMask = registerRangeMask(
                partial->Global, partial->BitOffset, partial->WriteWidth);
            eraseDemand(live, partial->Global, writeMask);
          }
          continue;
        }
        llvm::Function *calleeF = call->getCalledFunction();
        if (!isNotDecRegisterHelperCall(*call) &&
            (calleeF == nullptr || !calleeF->isIntrinsic())) {
          applyBackwardCallDemand(*call, live, additions);
        }
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && !isIgnored(*access.Unit, Options)) {
          eraseDemand(live, access.Unit->Global);
        }
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        RegisterAccess access = registerLoad(*load, Units);
        if (access.Unit != nullptr && !isIgnored(*access.Unit, Options)) {
          addDemand(live, access.Unit->Global, valueDemand(load, valueDemands));
        }
      }
    }
    return live;
  }

void Analyzer::applyBackwardCallDemand(
      llvm::CallBase &call, RegisterDemand &live,
      std::map<llvm::Function *, RegisterDemand> &additions) {
    llvm::Function *callee = call.getCalledFunction();
    if (callee != nullptr && !callee->isDeclaration() &&
        Effects.count(callee) != 0) {
      const FunctionEffect &effect = Effects[callee];
      if (effect.NoReturn) {
        live.clear();
        return;
      }
      for (const auto &[global, unit] : Units) {
        llvm::APInt mask = demandFor(live, global);
        if (isIgnored(unit, Options) || mask.getBitWidth() == 0 ||
            mask.isZero()) {
          continue;
        }
        Cell calleeCell = cellIn(effect, global);
        // 只把 callee 实际改写（MayNonEntry）的位加进 callee 的退出需求；
        // 调用后仍可能是入口值（MayEntry）的位，live 需求保留。
        llvm::APInt nonEntryBits = calleeCell.MayNonEntry & mask;
        if (maskNonZero(nonEntryBits)) {
          addDemand(additions[callee], global, nonEntryBits);
          DemandEdges[unit.Name][call.getFunction()].insert(callee);
          if (traceZmm0Demand() && unit.Name == "ZMM0") {
            llvm::errs() << "ZMM0EXIT caller="
                         << call.getParent()->getParent()->getName()
                         << " callee=" << callee->getName()
                         << " bits=" << maskToHex(nonEntryBits)
                         << " live=" << maskToHex(mask) << "\n";
          }
        }
        llvm::APInt killedBits = ~calleeCell.MayEntry & mask;
        if (maskNonZero(killedBits)) {
          eraseDemand(live, global, killedBits);
        }
      }
      // callee 在入口需要的位，调用点必须已经有值——但只要求"签名确实会带进来的位"，
      // 也就是镜像 shapeForInternalFunction 的入口形参判据：
      //   ReadEntryMask                —— callee 真读的入口位；
      //   ReturnEntryMask ∩ live        —— callee 出口透传、而调用点之后还要用的位
      //                                    （不被需要的透传位是 don't-care，调用点用
      //                                    入口读兜底即可，不必为它造载体）。
      // 不用 EntryDemandMask：它含 partial write 的写保留位等垃圾位（见
      // shapeForInternalFunction 的注释），拿它传播会把假需求放大到整条调用链。
      // 已知外部 callee 的 addInputDemand 做的是同一件事（那边用静态 shape）。
      // ReturnEntryMask 依赖 callee 的 ExitDemand，所以这个函数不再是"只读自己的
      // ExitDemand"；ExitDemand 变化由 runTopDownDemand 的 worklist 重新入队 caller。
      auto calleeDemandIt = Demands.find(callee);
      for (const auto &[global, unit] : Units) {
        if (isIgnored(unit, Options)) {
          continue;
        }
        Cell calleeCell = cellIn(effect, global);
        llvm::APInt entryNeeds = calleeCell.ReadEntry;
        // 浮点返回寄存器（XMM/ZMM/ST0）不做入口侧闭合（和
        // shapeForInternalFunction 的取舍一致）：调用点还答不出整条向量。
        bool floatReturnUnit =
            Abi.Outputs.count(unit.Name) != 0 &&
            std::find(Abi.IntegerOutputOrder.begin(),
                      Abi.IntegerOutputOrder.end(),
                      unit.Name) == Abi.IntegerOutputOrder.end();
        llvm::APInt liveMask = demandFor(live, global);
        llvm::APInt exitDemand =
            calleeDemandIt == Demands.end()
                ? llvm::APInt()
                : demandFor(calleeDemandIt->second.ExitDemand, global);
        if (!floatReturnUnit && !isNativeVarArgFunction(*callee) &&
            exitDemand.getBitWidth() != 0 && liveMask.getBitWidth() != 0 &&
            exitDemand.getBitWidth() == liveMask.getBitWidth() &&
            calleeCell.MayEntry.getBitWidth() == exitDemand.getBitWidth()) {
          // callee 出口透传（MayEntry ∩ MayNonEntry）∩ 调用点之后还需要（live）
          llvm::APInt returnEntry =
              calleeCell.MayEntry & calleeCell.MayNonEntry & exitDemand & liveMask;
          if (maskNonZero(returnEntry)) {
            entryNeeds = entryNeeds.getBitWidth() == 0
                             ? returnEntry
                             : entryNeeds | returnEntry;
          }
        }
        if (maskNonZero(entryNeeds)) {
          addDemand(live, global, entryNeeds);
        }
      }
      return;
    }

    for (const auto &[global, unit] : Units) {
      if (isIgnored(unit, Options) || Abi.Unaffected.count(unit.Name) != 0 ||
          isSegmentBaseUnit(unit.Name)) {
        continue;
      }
      llvm::APInt clobberMask = fullMask(unit);
      if (clobberMask.getBitWidth() != 0 && !clobberMask.isZero()) {
        eraseDemand(live, global, clobberMask);
      }
    }

    auto addInputDemand = [&](const NativeRegisterCallInputSlot &input) {
      if (input.Kind != NativeRegisterCallInputSlotKind::Register) {
        return;
      }
      auto unitIt = UnitsByName.find(input.UnitName);
      if (unitIt == UnitsByName.end()) {
        return;
      }
      llvm::GlobalVariable *global = unitIt->second;
      const RegisterUnit &unit = Units.at(global);
      if (isIgnored(unit, Options)) {
        return;
      }
      llvm::APInt mask =
          registerRangeMask(global, input.OffsetBits, input.SizeBits);
      addDemand(live, global, mask);
    };

    if (const NativeExternalCallShape *shape = externalCallShape(call)) {
      if (shape->NoReturn) {
        live.clear();
      }
      for (const NativeRegisterCallInputSlot &input : shape->Inputs) {
        addInputDemand(input);
      }
      return;
    }
    if (callee != nullptr && callee->isDeclaration() &&
        callee->hasFnAttribute(llvm::Attribute::NoReturn)) {
      live.clear();
    }
    if (Options.UnknownExternalInputPolicy !=
        NativeRegisterUnknownExternalInputPolicy::AbiInputs) {
      return;
    }
    for (const std::string &name : Abi.InputOrder) {
      auto unitIt = UnitsByName.find(name);
      if (unitIt == UnitsByName.end()) {
        continue;
      }
      const RegisterUnit &unit = Units.at(unitIt->second);
      if (!isIgnored(unit, Options)) {
        addDemand(live, unit.Global, namedMaskOrFull(Abi.InputMasks, unit));
      }
    }
  }

llvm::MDNode *Analyzer::registerNode(llvm::LLVMContext &context,
                             const RegisterUnit &unit) const {
    llvm::Metadata *fields[] = {
        llvm::MDString::get(context, "name=" + unit.Name),
    };
    return llvm::MDNode::get(context, fields);
  }

llvm::MDNode *Analyzer::registerSummaryNode(llvm::LLVMContext &context,
                                    llvm::GlobalVariable *global,
                                    const Cell &cell,
                                    const FunctionDemand &demand) const {
    const RegisterUnit &unit = Units.at(global);
    bool exitDemand = demand.ExitDemand.count(global) != 0;
    // bool 字段是 mask 的投影（mask 非零），mask 字段给出按位信息。
    std::vector<llvm::Metadata *> fields{
        llvm::MDString::get(context, "name=" + unit.Name),
        llvm::MDString::get(context, std::string("read_entry=") +
                                         (maskNonZero(cell.ReadEntry)
                                              ? "true"
                                              : "false")),
        llvm::MDString::get(context, "read_entry_mask=0x" +
                                         maskToHex(cell.ReadEntry)),
        llvm::MDString::get(context, std::string("may_entry=") +
                                         (maskNonZero(cell.MayEntry) ? "true"
                                                                     : "false")),
        llvm::MDString::get(context,
                            "may_entry_mask=0x" + maskToHex(cell.MayEntry)),
        llvm::MDString::get(context,
                            std::string("may_non_entry=") +
                                (maskNonZero(cell.MayNonEntry) ? "true"
                                                               : "false")),
        llvm::MDString::get(context, "may_non_entry_mask=0x" +
                                         maskToHex(cell.MayNonEntry)),
        llvm::MDString::get(context, std::string("exit_demand=") +
                                         (exitDemand ? "true" : "false")),
    };
    llvm::APInt entryMask = demandFor(demand.EntryDemand, global);
    if (entryMask.getBitWidth() != 0 && !entryMask.isZero()) {
      fields.push_back(llvm::MDString::get(context, "entry_demand_mask=0x" +
                                                        maskToHex(entryMask)));
    }
    llvm::APInt exitMask = demandFor(demand.ExitDemand, global);
    if (exitMask.getBitWidth() != 0 && !exitMask.isZero()) {
      fields.push_back(llvm::MDString::get(context, "exit_demand_mask=0x" +
                                                        maskToHex(exitMask)));
    }
    // 双向位：返回槽要交回、但有些路径上还是入口值的位。出口靠返回槽，入口必须
    // 靠形参，否则按位 SSA 只能裸读寄存器 global（Arch2.md 第 13 节）。
    llvm::APInt returnEntryMask =
        intersectBitMasks({exitMask, cell.MayEntry, cell.MayNonEntry});
    if (returnEntryMask.getBitWidth() != 0 && !returnEntryMask.isZero()) {
      fields.push_back(llvm::MDString::get(context, "return_entry_mask=0x" +
                                                        maskToHex(returnEntryMask)));
    }
    return llvm::MDNode::get(context, fields);
  }

void Analyzer::attachMetadata() {
    llvm::LLVMContext &context = Module.getContext();
    for (llvm::Function *function : Functions) {
      std::vector<llvm::Metadata *> all;
      std::vector<llvm::Metadata *> reads;
      std::vector<llvm::Metadata *> preserves;
      std::vector<llvm::Metadata *> modifies;
      std::vector<llvm::Metadata *> demandedReturns;
      FunctionEffect &effect = Effects[function];
      FunctionDemand &demand = Demands[function];
      for (const RegisterUnit *unitPtr : unitsInNameOrder(Units)) {
        llvm::GlobalVariable *global = unitPtr->Global;
        const RegisterUnit &unit = *unitPtr;
        if (isIgnored(unit, Options)) {
          continue;
        }
        Cell cell = cellIn(effect, global);
        bool exitDemand = demand.ExitDemand.count(global) != 0;
        all.push_back(registerSummaryNode(context, global, cell, demand));
        if (maskNonZero(cell.ReadEntry)) {
          reads.push_back(registerNode(context, unit));
        }
        if (maskNonZero(cell.MayEntry) && cell.MayNonEntry.isZero()) {
          preserves.push_back(registerNode(context, unit));
        }
        if (maskNonZero(cell.MayNonEntry)) {
          modifies.push_back(registerNode(context, unit));
        }
        if (exitDemand && maskNonZero(cell.MayNonEntry) &&
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
      if (effect.NoReturn) {
        function->addFnAttr(llvm::Attribute::NoReturn);
        function->setMetadata("notdec.register.summary.noreturn",
                              llvm::MDNode::get(context, {}));
      }
    }
  }


namespace {

bool functionTouchesRegister(llvm::Function &function,
                             llvm::GlobalVariable *global) {
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &inst : block) {
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        if (load->getPointerOperand()->stripPointerCasts() == global) {
          return true;
        }
      } else if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        if (store->getPointerOperand()->stripPointerCasts() == global) {
          return true;
        }
      } else if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (std::optional<NativeRegisterPartialReadInfo> read =
                parseNativeRegisterPartialRead(*call);
            read && read->Global == global) {
          return true;
        }
        if (std::optional<NativeRegisterPartialWriteInfo> write =
                parseNativeRegisterPartialWrite(*call);
            write && write->Global == global) {
          return true;
        }
      }
    }
  }
  return false;
}

} // namespace

namespace {

// 从"可观察 sink"反向收集所有被需要的 SSA 值。
// 和 computeValueDemands 的区别：这里去掉两个会放大需求的 seed——
//   * partial_write 的值（写回寄存器不等于被观察，只有寄存器后续被可观察读才算）
//   * icmp 的操作数（比较结果可能只喂给死掉的 flag store）
// 保留的 sink 都是"函数外可见"的：ret、br/switch 条件、store 到非寄存器内存。
// 内部 callee 的实参由 usedInputs 定点决定；外部调用没有显式实参（参数在寄存器
// 里），用 callsExternal 标记，由 computeUsedInputs 把 ABI 输入寄存器整体算 used。
std::set<llvm::Value *>
observableValuesInFunction(llvm::Function &function,
                           const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
                           bool &callsExternal) {
  std::set<llvm::Value *> needed;
  std::vector<llvm::Value *> worklist;
  auto mark = [&](llvm::Value *value) {
    if (value == nullptr || llvm::isa<llvm::Constant>(value)) {
      return;
    }
    if (needed.insert(value).second) {
      worklist.push_back(value);
    }
  };
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &inst : block) {
      if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
        mark(ret->getReturnValue());
        continue;
      }
      if (auto *branch = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
        if (branch->isConditional()) {
          mark(branch->getCondition());
        }
        continue;
      }
      if (auto *sw = llvm::dyn_cast<llvm::SwitchInst>(&inst)) {
        mark(sw->getCondition());
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        if (registerStore(*store, units).Unit == nullptr) {
          mark(store->getValueOperand());
        }
        continue;
      }
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr || isNotDecRegisterHelperCall(*call)) {
        continue;
      }
      llvm::Function *callee = call->getCalledFunction();
      if (callee == nullptr || callee->isDeclaration()) {
        callsExternal = true;
      }
    }
  }
  while (!worklist.empty()) {
    llvm::Value *value = worklist.back();
    worklist.pop_back();
    auto *inst = llvm::dyn_cast<llvm::Instruction>(value);
    if (inst == nullptr) {
      continue;
    }
    for (llvm::Value *operand : inst->operand_values()) {
      mark(operand);
    }
  }
  return needed;
}

std::vector<llvm::Value *>
registerReadsInFunction(llvm::Function &function,
                        llvm::GlobalVariable *global) {
  std::vector<llvm::Value *> reads;
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &inst : block) {
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        if (load->getPointerOperand()->stripPointerCasts() == global) {
          reads.push_back(load);
        }
        continue;
      }
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr) {
        continue;
      }
      if (std::optional<NativeRegisterPartialReadInfo> read =
              parseNativeRegisterPartialRead(*call);
          read && read->Global == global) {
        reads.push_back(call);
      }
    }
  }
  return reads;
}

} // namespace

void Analyzer::computeUsedInputs() {
  std::map<llvm::Function *, std::set<llvm::GlobalVariable *>> localUsed;
  std::map<llvm::Function *, bool> callsExternal;
  std::map<llvm::Function *, std::set<llvm::Function *>> internalCallees;
  const std::set<llvm::Function *> defined(Functions.begin(), Functions.end());

  for (llvm::Function *function : Functions) {
    bool external = false;
    std::set<llvm::Value *> needed =
        observableValuesInFunction(*function, Units, external);
    callsExternal[function] = external;
    for (const auto &[global, unit] : Units) {
      if (isIgnored(unit, Options)) {
        continue;
      }
      for (llvm::Value *read : registerReadsInFunction(*function, global)) {
        if (needed.find(read) != needed.end()) {
          localUsed[function].insert(global);
          break;
        }
      }
    }
    for (llvm::BasicBlock &block : *function) {
      for (llvm::Instruction &inst : block) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
        if (call == nullptr || isNotDecRegisterHelperCall(*call)) {
          continue;
        }
        llvm::Function *callee = call->getCalledFunction();
        if (callee != nullptr && !callee->isDeclaration() &&
            defined.count(callee) != 0) {
          internalCallees[function].insert(callee);
        }
      }
    }
  }

  std::deque<llvm::Function *> worklist;
  std::set<llvm::Function *> inQueue;
  auto enqueue = [&](llvm::Function *function) {
    if (function != nullptr && inQueue.insert(function).second) {
      worklist.push_back(function);
    }
  };
  for (llvm::Function *function : Functions) {
    // 接口不可改（地址被取，可能被我们看不到的代码调用）：整条保守算 used。
    const bool interfaceFixed = function->hasAddressTaken();
    for (const auto &[global, unit] : Units) {
      if (isIgnored(unit, Options)) {
        continue;
      }
      const bool abiInputAndCallsOut =
          callsExternal[function] != 0 && Abi.Inputs.count(unit.Name) != 0;
      if (localUsed[function].count(global) != 0 || interfaceFixed ||
          abiInputAndCallsOut) {
        UsedInputs[function].insert(unit.Name);
      }
    }
    enqueue(function);
  }

  while (!worklist.empty()) {
    llvm::Function *function = worklist.front();
    worklist.pop_front();
    inQueue.erase(function);
    bool changed = false;
    for (llvm::Function *callee : internalCallees[function]) {
      const auto calleeIt = UsedInputs.find(callee);
      if (calleeIt == UsedInputs.end()) {
        continue;
      }
      for (const std::string &name : calleeIt->second) {
        if (UsedInputs[function].insert(name).second) {
          changed = true;
        }
      }
    }
    if (changed) {
      const auto callersIt = Callers.find(function);
      if (callersIt != Callers.end()) {
        for (llvm::Function *caller : callersIt->second) {
          enqueue(caller);
        }
      }
    }
  }
}

void Analyzer::reportUnobservableInputs() {
  for (const auto &[global, unit] : Units) {
    if (isIgnored(unit, Options)) {
      continue;
    }
    std::vector<std::string> unobservable;
    for (llvm::Function *function : Functions) {
      const auto factIt = Effects.find(function);
      if (factIt == Effects.end()) {
        continue;
      }
      const Cell cell = cellIn(factIt->second, global);
      if (!maskNonZero(cell.ReadEntry)) {
        continue;
      }
      const auto usedIt = UsedInputs.find(function);
      if (usedIt != UsedInputs.end() &&
          usedIt->second.count(unit.Name) != 0) {
        continue;
      }
      unobservable.push_back(function->getName().str());
    }
    // 调用点：callee 读入口、但读到的值没人观察 ⇒ 调用点不需要为它准备值。
    uint64_t cuttableCallSites = 0;
    for (const auto &[caller, callees] : Calls) {
      (void)caller;
      for (llvm::Function *callee : callees) {
        const auto factIt = Effects.find(callee);
        if (factIt == Effects.end()) {
          continue;
        }
        if (!maskNonZero(cellIn(factIt->second, global).ReadEntry)) {
          continue;
        }
        const auto usedIt = UsedInputs.find(callee);
        if (usedIt != UsedInputs.end() &&
            usedIt->second.count(unit.Name) != 0) {
          continue;
        }
        ++cuttableCallSites;
      }
    }
    if (unobservable.empty() && cuttableCallSites == 0) {
      continue;
    }
    llvm::errs() << "Native register observable inputs: " << unit.Name
                 << " read_entry_functions=" << unobservable.size()
                 << " cuttable_callsites=" << cuttableCallSites;
    if (!unobservable.empty()) {
      llvm::errs() << " examples:";
      const size_t limit = std::min<size_t>(unobservable.size(), 6);
      for (size_t index = 0; index < limit; ++index) {
        llvm::errs() << ' ' << unobservable[index];
      }
      if (unobservable.size() > limit) {
        llvm::errs() << " ...(+" << unobservable.size() - limit << ')';
      }
    }
    llvm::errs() << '\n';
  }
}

void Analyzer::reportDemandOrigins() {
  for (const auto &[registerName, edges] : DemandEdges) {
    if (edges.empty()) {
      continue;
    }
    auto unitIt = UnitsByName.find(registerName);
    if (unitIt == UnitsByName.end()) {
      continue;
    }
    llvm::GlobalVariable *global = unitIt->second;

    std::set<llvm::Function *> nodes;
    std::map<llvm::Function *, unsigned> inDegree;
    uint64_t edgeCount = 0;
    for (const auto &[caller, callees] : edges) {
      nodes.insert(caller);
      for (llvm::Function *callee : callees) {
        nodes.insert(callee);
        ++inDegree[callee];
        ++edgeCount;
      }
    }

    std::vector<std::string> sources;
    std::vector<std::string> passThrough;
    std::vector<std::string> sinks;
    for (llvm::Function *function : nodes) {
      const bool hasIncoming = inDegree[function] != 0;
      const auto outIt = edges.find(function);
      const bool hasOutgoing = outIt != edges.end() && !outIt->second.empty();
      if (!hasIncoming) {
        sources.push_back(function->getName().str());
      }
      if (!hasOutgoing) {
        sinks.push_back(function->getName().str());
      }
      if (!functionTouchesRegister(*function, global)) {
        passThrough.push_back(function->getName().str());
      }
    }

    llvm::errs() << "Native register demand: " << registerName
                 << " nodes=" << nodes.size() << " edges=" << edgeCount
                 << " sources=" << sources.size()
                 << " sinks=" << sinks.size()
                 << " pass_through=" << passThrough.size() << '\n';
    if (!passThrough.empty()) {
      llvm::errs()
          << "  warning: " << passThrough.size() << '/' << nodes.size()
          << " functions carry a " << registerName
          << " demand without any local access to that register; the demand is"
             " pass-through, not a real value need. Fix the source(s) first"
             " instead of chasing the chain.\n";
      auto printList = [&](llvm::StringRef label,
                           const std::vector<std::string> &values) {
        if (values.empty()) {
          return;
        }
        llvm::errs() << "  " << label << ":";
        const size_t limit = std::min<size_t>(values.size(), 8);
        for (size_t index = 0; index < limit; ++index) {
          llvm::errs() << ' ' << values[index];
        }
        if (values.size() > limit) {
          llvm::errs() << " ...(+" << values.size() - limit << ')';
        }
        llvm::errs() << '\n';
      };
      printList("sources", sources);
      printList("pass_through_examples", passThrough);
    }
  }
}

NativeRegisterSummary Analyzer::buildPublicSummary() const {
    NativeRegisterSummary summary;
    summary.FunctionsSeen = Functions.size();
    summary.ExternalCallsites = ExternalCallsites;
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
      fn.NoReturn = effect.NoReturn;
      const FunctionDemand &demand =
          Demands.count(function) == 0 ? EmptyDemand : Demands.at(function);
      for (const RegisterUnit *unitPtr : unitsInNameOrder(Units)) {
        llvm::GlobalVariable *global = unitPtr->Global;
        const RegisterUnit &unit = *unitPtr;
        if (isIgnored(unit, Options)) {
          continue;
        }
        Cell cell = cellIn(effect, global);
        bool exitDemand = demand.ExitDemand.count(global) != 0;
        NativeRegisterSummaryRegister reg;
        reg.Name = unit.Name;
        // bool 字段保留为 mask 非零的投影，测试和计数器继续用它们；
        // 精确的按位信息在新增的 *MaskHex 字段里。
        reg.ReadEntry = maskNonZero(cell.ReadEntry);
        reg.MayEntry = maskNonZero(cell.MayEntry);
        reg.MayNonEntry = maskNonZero(cell.MayNonEntry);
        reg.ExitDemand = exitDemand;
        reg.ReadEntryMaskHex = maskToHex(cell.ReadEntry);
        reg.MayEntryMaskHex = maskToHex(cell.MayEntry);
        reg.MayNonEntryMaskHex = maskToHex(cell.MayNonEntry);
        reg.EntryDemandMaskHex =
            maskToHex(demandFor(demand.EntryDemand, global));
        reg.ExitDemandMaskHex = maskToHex(demandFor(demand.ExitDemand, global));
        reg.ReturnEntryMaskHex = maskToHex(intersectBitMasks(
            {demandFor(demand.ExitDemand, global), cell.MayEntry,
             cell.MayNonEntry}));
        fn.Registers.push_back(reg);
        if (reg.ReadEntry) {
          ++fn.ReadEntryRegisters;
        }
        if (reg.MayNonEntry) {
          ++fn.ModifiedRegisters;
        }
        if (reg.MayEntry && !reg.MayNonEntry) {
          ++fn.PreservedRegisters;
        }
        if (exitDemand && reg.MayNonEntry && Abi.Outputs.count(unit.Name)) {
          ++fn.DemandedReturns;
        }
      }
      summary.LoadsSeen += fn.LoadsSeen;
      summary.StoresSeen += fn.StoresSeen;
      summary.CallsSeen += fn.CallsSeen;
      if (fn.NoReturn) {
        ++summary.NoReturnFunctions;
      }
      summary.ReadEntryRegisters += fn.ReadEntryRegisters;
      summary.ModifiedRegisters += fn.ModifiedRegisters;
      summary.PreservedRegisters += fn.PreservedRegisters;
      summary.DemandedReturns += fn.DemandedReturns;
      summary.Functions.push_back(std::move(fn));
    }
    return summary;
  }

} // namespace notdec::bin2llvm
