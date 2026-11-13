#include "RegisterEliminationInternal.h"

namespace notdec::bin2llvm {

const CallArgStoreBinding *
bindingForIndex(const std::vector<CallArgStoreBinding> &bindings,
                unsigned index) {
  auto it = std::find_if(bindings.begin(), bindings.end(),
                         [&](const CallArgStoreBinding &binding) {
                           return binding.Index == index;
                         });
  return it == bindings.end() ? nullptr : &*it;
}

bool sameStackSignatureSlot(const NativeSignatureSlot &lhs,
                            const NativeSignatureSlot &rhs);

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

void attachUnknownValueMetadata(llvm::Value *value, llvm::StringRef reason,
                                llvm::StringRef functionName ,
                                llvm::StringRef calleeName ,
                                llvm::StringRef registerName ,
                                llvm::StringRef kind ,
                                llvm::StringRef detail ) {
  auto *inst = llvm::dyn_cast_or_null<llvm::Instruction>(value);
  if (inst == nullptr) {
    return;
  }
  llvm::LLVMContext &context = inst->getContext();
  std::vector<llvm::Metadata *> fields;
  fields.push_back(llvm::MDString::get(context, "reason=" + reason.str()));
  if (!functionName.empty()) {
    fields.push_back(
        llvm::MDString::get(context, "function=" + functionName.str()));
  }
  if (!calleeName.empty()) {
    fields.push_back(
        llvm::MDString::get(context, "callee=" + calleeName.str()));
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
  inst->setMetadata("notdec.unknown.source",
                    llvm::MDNode::get(context, fields));
}

llvm::Value *unknownValueBefore(llvm::Instruction &insertBefore,
                                llvm::Type *type, llvm::Twine name) {
  llvm::IRBuilder<> builder(&insertBefore);
  return unknownValueAt(builder, type, name);
}

llvm::Function *getOrInsertVarArgUnknownHelper(llvm::Module &module,
                                               llvm::Type *type) {
  std::string name = "notdec.register.vararg_unknown." + llvmTypeName(type);
  llvm::FunctionType *functionType = llvm::FunctionType::get(type, {}, false);
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

  // Poison cannot carry metadata.  Mark the user instruction instead so a
  // demand-pruned operand can be traced without changing the operand type.
  std::vector<llvm::Metadata *> fields = {
      llvm::MDString::get(context, "operand=" + std::to_string(operandIndex)),
      llvm::MDString::get(context,
                          "original_type=" + llvmTypeName(original.getType())),
      llvm::MDString::get(context, "replacement=poison"),
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
  // notdec.register.* 是内置 intrinsic 命名空间（外加历史前缀），不是"未知外部
  // 函数"：寄存器影响由各自的语义表达，不能按未知外部调用来处理。
  return function.isDeclaration() && !function.isIntrinsic() &&
         !function.getName().starts_with("notdec.register.") &&
         !isNotDecOpaqueUnknownName(function.getName()) &&
         !isNativeRegisterValueRangeName(function.getName()) &&
         !isNativeRegisterPartialReadName(function.getName()) &&
         !isNativeRegisterPartialWriteName(function.getName()) &&
         !isNativeX87IntrinsicName(function.getName()) &&
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
    const std::map<llvm::Function *, FunctionSummaryFacts> *facts ) {
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

bool registerValueSourceIsControlled(const llvm::Function &function) {
  return function.hasLocalLinkage() && !function.hasAddressTaken();
}

AbiConvention abiConventionFor(const llvm::Function &function,
                               bool abiForAllFunctions) {
  if (function.isDeclaration()) {
    return AbiConvention::External;
  }
  if (abiForAllFunctions) {
    return AbiConvention::External;
  }
  return registerValueSourceIsControlled(function) ? AbiConvention::Internal
                                                   : AbiConvention::External;
}

bool pointerIsRegisterGlobal(const llvm::Value *value) {
  const llvm::Value *stripped = value->stripPointerCasts();
  const auto *global = llvm::dyn_cast<llvm::GlobalVariable>(stripped);
  return global != nullptr && global->getMetadata("notdec.register") != nullptr;
}

NativeRegisterValueSourceAudit
auditRegisterValueSourcesImpl(llvm::Module &module, bool abiForAllFunctions) {
  NativeRegisterValueSourceAudit audit;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    // 和签名构建用同一个判定：只有“接口可改”的函数的残留寄存器读才算 local，
    // 遵守 ABI 的函数（导出/地址被取，或开关强制）算边界读。
    const bool controlled = abiConventionFor(function, abiForAllFunctions) ==
                            AbiConvention::Internal;
    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &instruction : block) {
        if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
          if (pointerIsRegisterGlobal(load->getPointerOperand())) {
            if (controlled) {
              ++audit.LocalRegisterReads;
            } else {
              ++audit.BoundaryRegisterReads;
            }
          }
          continue;
        }
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (call == nullptr) {
          continue;
        }
        llvm::Function *callee = call->getCalledFunction();
        if (callee == nullptr) {
          continue;
        }
        llvm::StringRef name = callee->getName();
        if (isNativeRegisterPartialReadName(name) && call->arg_size() >= 1 &&
            pointerIsRegisterGlobal(call->getArgOperand(0))) {
          if (controlled) {
            ++audit.LocalRegisterReads;
          } else {
            ++audit.BoundaryRegisterReads;
          }
        } else if (isNativeRegisterPartialWriteName(name)) {
          ++audit.PartialWriteHelpers;
        } else if (name.starts_with("notdec.register.summary_clobber.")) {
          ++audit.ClobberPlaceholders;
        } else if (isNotDecOpaqueUnknownName(name)) {
          std::string reason =
              mdField(call->getMetadata("notdec.unknown.source"), "reason")
                  .value_or("no_reason");
          ++audit.UnknownValuesByReason[reason];
        }
      }
    }
  }
  return audit;
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

uint64_t canonicalizeX87WindowAccesses(
    llvm::Module &module,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  std::vector<llvm::Instruction *> accesses;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
      auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
      if (load == nullptr && store == nullptr) {
        continue;
      }
      llvm::Value *pointer = load != nullptr ? load->getPointerOperand()
                                             : store->getPointerOperand();
      llvm::GlobalVariable *global = registerGlobalFromValue(pointer, units);
      if (global == nullptr) {
        continue;
      }
      const RegisterUnit &unit = units.find(global)->second;
      if (unit.Name != "ST0" && unit.Name != "ST1") {
        continue;
      }
      llvm::Type *accessType = load != nullptr ? load->getType()
                                               : store->getValueOperand()
                                                     ->getType();
      if (accessType->isX86_FP80Ty()) {
        accesses.push_back(&inst);
      }
    }
  }

  uint64_t rewritten = 0;
  for (llvm::Instruction *inst : accesses) {
    if (inst->getParent() == nullptr) {
      continue;
    }
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
      llvm::IRBuilder<> builder(load);
      auto *bits = builder.CreateLoad(
          llvm::IntegerType::get(module.getContext(), 80),
          load->getPointerOperand(), "x87.fp80.bits");
      bits->setAlignment(load->getAlign());
      bits->copyMetadata(*load);
      llvm::Value *value =
          builder.CreateBitCast(bits, load->getType(), "x87.fp80.value");
      load->replaceAllUsesWith(value);
      load->eraseFromParent();
      ++rewritten;
      continue;
    }
    auto *store = llvm::cast<llvm::StoreInst>(inst);
    llvm::IRBuilder<> builder(store);
    llvm::Value *bits = builder.CreateBitCast(
        store->getValueOperand(),
        llvm::IntegerType::get(module.getContext(), 80), "x87.fp80.bits");
    auto *newStore = builder.CreateStore(bits, store->getPointerOperand());
    newStore->setAlignment(store->getAlign());
    newStore->copyMetadata(*store);
    store->eraseFromParent();
    ++rewritten;
  }
  return rewritten;
}

bool isAbiBoundaryZeroRegister(llvm::StringRef name) {
  return name == "DF";
}

bool isX87StackRegisterName(llvm::StringRef name) {
  return name.size() == 3 && name.starts_with("ST") && name[2] >= '0' &&
         name[2] <= '7';
}

bool isValueCarryingRegisterName(llvm::StringRef name) {
  return name == "RAX" || name == "RBX" || name == "RCX" || name == "RDX" ||
         name == "RSI" || name == "RDI" || name == "RBP" || name == "RSP" ||
         name == "R8" || name == "R9" || name == "R10" || name == "R11" ||
         name == "R12" || name == "R13" || name == "R14" || name == "R15";
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

AbiFacts::StackSlot abiStackSlot(const NativeAbiParamEntry &entry,
                                 size_t abiIndex) {
  AbiFacts::StackSlot slot;
  slot.Space = entry.Storage.Space;
  slot.MetaType = entry.MetaType;
  slot.Offset = entry.Storage.Offset;
  slot.MinSize = entry.MinSize;
  slot.MaxSize = entry.MaxSize;
  slot.Align = entry.Align;
  slot.AbiIndex = abiIndex;
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
  for (auto [abiIndex, entry] : llvm::enumerate(abi->Inputs)) {
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
    } else if (entry.Storage.Kind == NativeAbiStorageKind::Stack &&
               !entry.Storage.Space.empty()) {
      facts.StackInputsInOrder.push_back(abiStackSlot(entry, abiIndex));
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
  // SysV 的 cspec 不写 x87 寄存器，但 long double 返回约定把结果留在 ST0
  // （80 位）。手工把 ST0 补成浮点输出槽：根函数 seed ExitDemand、内部函数
  // 从调用方返回需求传播，最后经 floatSlotForDemand 变成 x86_fp80 返回槽。
  // ST1 只是窗口内部状态（pop 的中间值），不参与 ABI。
  for (const auto &[global, unit] : units) {
    (void)global;
    if (unit.Name != "ST0") {
      continue;
    }
    facts.Outputs.insert("ST0");
    pushUnique(facts.OutputsInOrder, "ST0");
    AbiFacts::RegisterSlot slot;
    slot.UnitName = "ST0";
    slot.AbiName = "ST0";
    slot.MetaType = "float";
    slot.OffsetBits = 0;
    slot.SizeBits = static_cast<unsigned>(unit.Size * 8);
    facts.FloatOutputsInOrder.push_back(slot);
    facts.InternalReturnRegisters.insert("ST0");
    break;
  }
  facts.StackPointer = abi->StackPointerRegister;
  facts.StackShift = abi->StackShift;
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
      fact.ReadEntryMask = parseMask(reg.ReadEntryMaskHex);
      fact.MayEntryMask = parseMask(reg.MayEntryMaskHex);
      fact.MayNonEntryMask = parseMask(reg.MayNonEntryMaskHex);
      fact.EntryDemandMask = parseMask(reg.EntryDemandMaskHex);
      fact.ExitDemandMask = parseMask(reg.ExitDemandMaskHex);
      fact.ReturnEntryMask = parseMask(reg.ReturnEntryMaskHex);
      if (fact.ReturnEntryMask.getBitWidth() == 0) {
        // 旧 summary 没有这个字段：按定义补算一次（Arch2.md 第 13 节）。
        fact.ReturnEntryMask = intersectBitMasks(
            {fact.ExitDemandMask, fact.MayEntryMask, fact.MayNonEntryMask});
      }
      facts.Registers.emplace(reg.Name, std::move(fact));
    }
    result.emplace(function, std::move(facts));
  }
  return result;
}

} // namespace notdec::bin2llvm
