#include "RegisterSummaryInternal.h"

namespace notdec::bin2llvm {

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
