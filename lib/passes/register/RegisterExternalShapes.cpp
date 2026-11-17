#include "RegisterEliminationInternal.h"

namespace notdec::bin2llvm {

NativeRegisterCallInputSlot callInputSlot(const NativeSignatureSlot &slot) {
  NativeRegisterCallInputSlot input;
  if (slot.Kind == NativeSignatureSlotKind::IntegerStack ||
      slot.Kind == NativeSignatureSlotKind::FloatStack) {
    input.Kind = NativeRegisterCallInputSlotKind::Stack;
    input.StackSpace = slot.StackSpace;
    input.StackOffset = slot.StackOffset;
    input.StackSize = slot.StackSize;
    input.StackAlign = slot.StackAlign;
  } else {
    input.Kind = NativeRegisterCallInputSlotKind::Register;
    input.UnitName = slot.Unit->Name;
  }
  input.OffsetBits = slot.OffsetBits;
  input.SizeBits = slot.SizeBits;
  input.Float = slot.Kind == NativeSignatureSlotKind::FloatRegister ||
                slot.Kind == NativeSignatureSlotKind::FloatStack;
  return input;
}

NativeRegisterCallInputSlot callInputSlot(const AbiFacts::RegisterSlot &slot) {
  NativeRegisterCallInputSlot input;
  input.Kind = NativeRegisterCallInputSlotKind::Register;
  input.UnitName = slot.UnitName;
  input.OffsetBits = slot.OffsetBits;
  input.SizeBits = slot.SizeBits;
  input.Float = slot.MetaType == "float";
  return input;
}

NativeRegisterCallInputSlot callInputSlot(const AbiFacts::StackSlot &slot,
                                          unsigned stackIndex,
                                          llvm::Type *llvmType) {
  NativeRegisterCallInputSlot input;
  input.Kind = NativeRegisterCallInputSlotKind::Stack;
  input.StackSpace = slot.Space;
  uint32_t sizeBytes =
      static_cast<uint32_t>((llvmType->getScalarSizeInBits() + 7) / 8);
  uint64_t relative = stackIndex * stackSlotStep(sizeBytes, slot.Align);
  if (relative > slot.MaxSize || sizeBytes > slot.MaxSize - relative) {
    return input;
  }
  input.StackOffset = slot.Offset + relative;
  input.StackSize = sizeBytes;
  input.StackAlign = slot.Align;
  input.SizeBits = sizeBytes * 8;
  input.Float = llvmType->isFloatingPointTy();
  return input;
}

bool sameCallInputSlot(const NativeRegisterCallInputSlot &lhs,
                       const NativeRegisterCallInputSlot &rhs) {
  if (lhs.Kind != rhs.Kind || lhs.OffsetBits != rhs.OffsetBits ||
      lhs.SizeBits != rhs.SizeBits || lhs.Float != rhs.Float) {
    return false;
  }
  if (lhs.Kind == NativeRegisterCallInputSlotKind::Stack) {
    return lhs.StackSpace == rhs.StackSpace &&
           lhs.StackOffset == rhs.StackOffset &&
           lhs.StackSize == rhs.StackSize && lhs.StackAlign == rhs.StackAlign;
  }
  return lhs.UnitName == rhs.UnitName;
}

void addStackEvidenceSlots(const AbiFacts &abi, llvm::Module &module,
                           std::vector<NativeRegisterCallInputSlot> &slots,
                           unsigned startIndex, unsigned count,
                           llvm::ArrayRef<NativeRegisterCallInputSlot> fixed) {
  if (abi.StackInputsInOrder.empty() || count == 0) {
    return;
  }
  llvm::Type *slotType = llvmTypeForKnownValue(
      module, NativeExternalPrototype::ValueType::PointerSized);
  const AbiFacts::StackSlot &stack = abi.StackInputsInOrder.front();
  for (unsigned index = 0; index < count; ++index) {
    NativeRegisterCallInputSlot input =
        callInputSlot(stack, startIndex + index, slotType);
    if (input.SizeBits == 0) {
      break;
    }
    if (std::none_of(fixed.begin(), fixed.end(),
                     [&](const NativeRegisterCallInputSlot &existing) {
                       return sameCallInputSlot(existing, input);
                     })) {
      slots.push_back(std::move(input));
    }
  }
}

NativeExternalCallShapeMap buildExternalCallShapes(
    llvm::Module &module,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi, const NativeExternalPrototypeMap &prototypes) {
  NativeExternalCallShapeMap result;
  for (llvm::Function &function : module) {
    if (function.isIntrinsic()) {
      continue;
    }
    const KnownExternalPrototype *known =
        knownExternalPrototype(prototypes, function.getName());
    if (known == nullptr) {
      continue;
    }
    // Internal definitions normally follow interprocedural register allocation,
    // not the external ABI.  A trusted vararg prototype is the exception used
    // for lifted helpers such as aprintf.
    if (!function.isDeclaration() && !known->VarArg) {
      continue;
    }
    SignatureShape signature =
        shapeForKnownExternal(function, units, abi, prototypes);
    NativeExternalCallShape callShape;
    callShape.FixedArgs = known->FixedArgs;
    callShape.VarArg = known->VarArg;
    callShape.NoReturn = known->NoReturn;
    callShape.MaxArgs = known->MaxArgs;
    callShape.FixedInputsComplete = signature.Params.size() == known->FixedArgs;
    for (const NativeSignatureSlot &slot : signature.Params) {
      callShape.Inputs.push_back(callInputSlot(slot));
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
      unsigned fixedStack = 0;
      for (const NativeRegisterCallInputSlot &input : callShape.Inputs) {
        if (input.Kind == NativeRegisterCallInputSlotKind::Stack) {
          ++fixedStack;
        }
      }
      unsigned tailCount =
          callShape.MaxArgs == 0 ? 16 : callShape.MaxArgs - callShape.FixedArgs;
      addStackEvidenceSlots(abi, module, callShape.VarArgInputs, fixedStack,
                            tailCount, callShape.Inputs);
    }
    result.emplace(function.getName().str(), std::move(callShape));
  }
  return result;
}

std::vector<NativeRegisterCallInputSlot>
unknownExternalEvidenceSlots(const AbiFacts &abi, llvm::Module &module) {
  std::vector<NativeRegisterCallInputSlot> result;
  result.reserve(abi.IntegerInputsInOrder.size() +
                 abi.FloatInputsInOrder.size() + 16);
  for (const AbiFacts::RegisterSlot &slot : abi.IntegerInputsInOrder) {
    result.push_back(callInputSlot(slot));
  }
  // Float (SSE) arguments live in XMM registers, which lift as the low lanes
  // of the ZMM globals.  Without these slots, unknown externals that only take
  // floating-point arguments (e.g. round()) infer arity 0 and get rewritten
  // with no arguments at all.
  for (const AbiFacts::RegisterSlot &slot : abi.FloatInputsInOrder) {
    result.push_back(callInputSlot(slot));
  }
  if (!abi.StackInputsInOrder.empty()) {
    // Unknown externals need a finite evidence window.  The offsets still come
    // from cspec; this bound only stops unbounded pentry expansion.
    addStackEvidenceSlots(abi, module, result, 0, 16, {});
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
  // 参数证据 = 调用点前槽被显式写（LocalDefinition），入口值透传
  // （ForwardedEntry）在 local 之前算"有值"：wrapper 函数把自己收到的
  // 参数（第一个参数通常是 handle）直接传下去时，该寄存器从未被写过，
  // 只会留下 Entry 标记。local 之后的 entry 是入口残留不是参数，直接
  // 截断（避免 local 之后夹 entry 再续 local 的过估）。没有任何 local
  // 时按 0 处理。
  unsigned leadingEntry = 0;
  for (const NativeRegisterCallsiteSlotEvidence &slot : slots) {
    if (slot.Index != leadingEntry) {
      break;
    }
    if (slot.Origin != NativeRegisterCallsiteValueOrigin::ForwardedEntry) {
      break;
    }
    ++leadingEntry;
  }
  unsigned localCount = 0;
  unsigned trailingConsumed = 0;
  for (const NativeRegisterCallsiteSlotEvidence &slot : slots.drop_front(
           std::min<size_t>(leadingEntry, slots.size()))) {
    if (slot.Origin != NativeRegisterCallsiteValueOrigin::LocalDefinition) {
      break;
    }
    ++localCount;
    // 已被读过的 local 只在连续证据的尾部排除：尾部之外（后面还有别的局部
    // 实参）说明这个寄存器仍然要给调用留值，例如 suffix 先放进 R8、再
    // `mov %r8,%rdx` 填 prefix，此时 R8 和 RDX 都是实参。
    trailingConsumed = slot.ConsumedLocal ? trailingConsumed + 1 : 0;
  }
  // 纯 entry（没有任何 local）是"入口寄存器没被用过"，不是参数证据。
  if (localCount == 0) {
    return 0;
  }
  return leadingEntry + localCount - trailingConsumed;
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
  // Stack evidence slots extend past the outgoing argument area into the
  // local frame, where local-variable stores are also LocalDefinition.  A
  // "hole" in stack origins is therefore normally a frame overlap, not a real
  // argument discontinuity, so only register slots keep this signal.
  return llvm::any_of(
      slots.drop_front(std::min<size_t>(prefix, slots.size())),
      [](const NativeRegisterCallsiteSlotEvidence &slot) {
        return slot.Origin ==
                   NativeRegisterCallsiteValueOrigin::LocalDefinition &&
               slot.Kind != NativeRegisterCallInputSlotKind::Stack;
      });
}

NativeRegisterCallInputSlot
callInputSlot(const NativeRegisterCallsiteSlotEvidence &slot) {
  NativeRegisterCallInputSlot input;
  input.Kind = slot.Kind;
  input.UnitName = slot.UnitName;
  input.StackSpace = slot.StackSpace;
  input.StackOffset = slot.StackOffset;
  input.StackSize = slot.StackSize;
  input.StackAlign = slot.StackAlign;
  input.OffsetBits = slot.OffsetBits;
  input.SizeBits = slot.SizeBits;
  input.Float = slot.Float;
  return input;
}

std::optional<NativeSignatureSlot>
makeSignatureSlotForCallInput(
    const NativeRegisterCallInputSlot &input,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    llvm::LLVMContext &context) {
  if (input.Kind == NativeRegisterCallInputSlotKind::Stack) {
    if (input.Float) {
      llvm::Type *type = floatTypeForSizeBits(context, input.SizeBits);
      if (type == nullptr) {
        return std::nullopt;
      }
      NativeSignatureSlot slot;
      slot.Kind = NativeSignatureSlotKind::FloatStack;
      slot.AbiName =
          input.StackSpace + "+" + std::to_string(input.StackOffset);
      slot.StackSpace = input.StackSpace;
      slot.StackOffset = input.StackOffset;
      slot.StackSize = input.StackSize;
      slot.StackAlign = input.StackAlign;
      slot.OffsetBits = 0;
      slot.SizeBits = input.SizeBits;
      slot.LlvmType = type;
      return slot;
    }
    llvm::Type *type = llvm::IntegerType::get(context, input.SizeBits);
    NativeSignatureSlot slot;
    slot.Kind = NativeSignatureSlotKind::IntegerStack;
    slot.AbiName = input.StackSpace + "+" + std::to_string(input.StackOffset);
    slot.StackSpace = input.StackSpace;
    slot.StackOffset = input.StackOffset;
    slot.StackSize = input.StackSize;
    slot.StackAlign = input.StackAlign;
    slot.OffsetBits = 0;
    slot.SizeBits = input.SizeBits;
    slot.LlvmType = type;
    return slot;
  }

  const RegisterUnit *unit = unitByName(units, input.UnitName);
  if (unit == nullptr) {
    return std::nullopt;
  }
  if (!input.Float) {
    return integerSignatureSlot(*unit);
  }
  llvm::Type *type = floatTypeForSizeBits(context, input.SizeBits);
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

std::optional<unsigned> constantLowByteBeforeCall(
    const NativeRegisterExternalCallsite &callsite,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    llvm::StringRef registerName) {
  auto unitIt =
      std::find_if(units.begin(), units.end(), [&](const auto &entry) {
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

NativeRegisterEliminationWarning externalInferenceWarning(llvm::StringRef callee,
                                                         llvm::StringRef detail,
                                                         llvm::StringRef reason,
                                                         unsigned uses) {
  NativeRegisterEliminationWarning warning;
  warning.FunctionName = "<external-signature>";
  warning.CalleeName = callee.str();
  warning.RegisterName = detail.str();
  warning.Kind = "signature";
  warning.Reason = reason.str();
  warning.Uses = uses;
  return warning;
}

NativeRegisterEliminationWarning
varArgInferenceWarning(const NativeRegisterExternalCallsite &callsite,
                       llvm::StringRef detail, llvm::StringRef reason) {
  NativeRegisterEliminationWarning warning;
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
  if (slot.Kind == NativeRegisterCallInputSlotKind::Stack) {
    return slot.StackSpace + "+" + std::to_string(slot.StackOffset) + "[" +
           std::to_string(slot.SizeBits) + "]=" + originName(slot.Origin).str();
  }
  return slot.UnitName + "[" + std::to_string(slot.OffsetBits) + ":" +
         std::to_string(slot.SizeBits) + "]=" + originName(slot.Origin).str();
}

void warnIfStoppedAtCallClobber(
    std::vector<NativeRegisterEliminationWarning> &warnings,
    const NativeRegisterExternalCallsite &callsite,
    llvm::ArrayRef<NativeRegisterCallsiteSlotEvidence> slots, unsigned prefix,
    llvm::StringRef reason) {
  const NativeRegisterCallsiteSlotEvidence *slot = slotAtPrefix(slots, prefix);
  if (slot == nullptr ||
      slot->Origin != NativeRegisterCallsiteValueOrigin::CallClobber) {
    return;
  }
  warnings.push_back(
      varArgInferenceWarning(callsite, slotDetail(*slot), reason));
}

std::string calleeNameForWarning(const llvm::CallBase &call) {
  llvm::Function *callee = call.getCalledFunction();
  return callee == nullptr ? std::string("<indirect>")
                           : callee->getName().str();
}

std::string slotRegisterName(const NativeSignatureSlot &slot) {
  if (slot.Kind == NativeSignatureSlotKind::IntegerStack ||
      slot.Kind == NativeSignatureSlotKind::FloatStack) {
    return slot.StackSpace + "+" + std::to_string(slot.StackOffset);
  }
  if (slot.Unit != nullptr) {
    return slot.Unit->Name;
  }
  return slot.AbiName;
}

NativeRegisterEliminationWarning callSlotWarning(const llvm::CallBase &call,
                                                const NativeSignatureSlot &slot,
                                                llvm::StringRef kind,
                                                llvm::StringRef reason) {
  NativeRegisterEliminationWarning warning;
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

NativeRegisterEliminationWarning
callRegisterWarning(const llvm::CallBase &call, llvm::StringRef registerName,
                    llvm::StringRef kind, llvm::StringRef reason) {
  NativeRegisterEliminationWarning warning;
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

std::string registerNameForRange(const RegisterRangeKey &range) {
  return range.Global == nullptr ? std::string("")
                                 : range.Global->getName().str();
}

NativeRegisterEliminationWarning callRangeWarning(const llvm::CallBase &call,
                                                 const RegisterRangeKey &range,
                                                 llvm::StringRef kind,
                                                 llvm::StringRef reason) {
  NativeRegisterEliminationWarning warning =
      callRegisterWarning(call, registerNameForRange(range), kind, reason);
  warning.RangeBitOffset = range.BitOffset;
  warning.RangeBitWidth = range.BitWidth;
  return warning;
}

NativeRegisterEliminationWarning
returnSlotWarning(const llvm::Function &function,
                  const NativeSignatureSlot &slot, llvm::StringRef reason) {
  NativeRegisterEliminationWarning warning;
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
    std::vector<NativeRegisterEliminationWarning> &warnings) {
  NativeExternalCallsiteShapeMap callsiteShapes;
  std::map<std::string, std::vector<unsigned>> aritiesByCallee;
  std::map<std::string, std::vector<std::pair<unsigned, unsigned>>>
      intFloatAritiesByCallee;
  // 未知外部（没有原型）各调用点的证据槽位。元数不一致时要按"固定 min + 每点
  // 自己的尾巴"建模，必须保留每个点自己的前缀。
  struct UnknownExternalSite {
    const llvm::CallBase *Call = nullptr;
    unsigned IntegerCount = 0;
    unsigned FloatCount = 0;
    std::vector<NativeRegisterCallsiteSlotEvidence> IntegerSlots;
    std::vector<NativeRegisterCallsiteSlotEvidence> FloatSlots;
  };
  std::map<std::string, std::vector<UnknownExternalSite>> unknownSitesByCallee;
  for (const NativeRegisterExternalCallsite &callsite :
       baseSummary.ExternalCallsites) {
    if (callsite.Kind == NativeRegisterExternalCallsiteKind::UnknownExternal) {
      if (!callsite.Indirect) {
        // Integer and float (SSE) arguments occupy independent register
        // sequences in the SysV ABI, so count each sequence separately.  A
        // plain serialized prefix would stop at the first argument of the
        // other class (e.g. f(int, double) would stop at RSI) and undercount.
        std::vector<NativeRegisterCallsiteSlotEvidence> integerSlots =
            renumberedSlotsByKind(callsite, false);
        std::vector<NativeRegisterCallsiteSlotEvidence> floatSlots =
            renumberedSlotsByKind(callsite, true);
        unsigned integerCount = localDefinitionPrefix(integerSlots);
        unsigned floatCount = localDefinitionPrefix(floatSlots);
        aritiesByCallee[callsite.CalleeName].push_back(integerCount +
                                                       floatCount);
        intFloatAritiesByCallee[callsite.CalleeName].push_back(
            {integerCount, floatCount});
        warnIfStoppedAtCallClobber(
            warnings, callsite, integerSlots, integerCount,
            "unknown_external_arity_stopped_at_call_clobber");
        // 浮点侧的 ZMM 寄存器在函数入口就是 ABI call-clobber，绝大多数调用点
        // 根本没有浮点参数证据（floatCount==0），第一个槽就是 clobber 不算
        // "证据被打断"，只在已有浮点参数证据后才报告。
        if (floatCount > 0) {
          warnIfStoppedAtCallClobber(
              warnings, callsite, floatSlots, floatCount,
              "float_unknown_external_arity_stopped_at_call_clobber");
        }
        if (callsite.Call != nullptr) {
          UnknownExternalSite site;
          site.Call = callsite.Call;
          site.IntegerCount = integerCount;
          site.FloatCount = floatCount;
          site.IntegerSlots = std::move(integerSlots);
          site.FloatSlots = std::move(floatSlots);
          unknownSitesByCallee[callsite.CalleeName].push_back(std::move(site));
        }
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
        warnings.push_back(
            varArgInferenceWarning(callsite,
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

    // Walk the evidence in source order.  A store wider than the evidence
    // grid (e.g. an i386 8-byte fstpl double inside a 4-byte grid) widens the
    // vararg slot and covers the following grid slots; without this, binding
    // would mismatch on the store type and the covered upper half would look
    // like a hole that truncates the tail.
    std::vector<NativeRegisterCallInputSlot> tailInputs;
    unsigned consumedSlots = 0;
    for (unsigned cursor = 0; cursor < integerSlots.size();) {
      const NativeRegisterCallsiteSlotEvidence &evidence = integerSlots[cursor];
      if (evidence.Origin !=
          NativeRegisterCallsiteValueOrigin::LocalDefinition) {
        break;
      }
      NativeRegisterCallInputSlot input = callInputSlot(evidence);
      unsigned covered = 1;
      if (evidence.StoreSizeBytes != 0 && evidence.StackSize != 0 &&
          evidence.StoreSizeBytes % evidence.StackSize == 0) {
        unsigned wideSlots = evidence.StoreSizeBytes / evidence.StackSize;
        if (wideSlots >= 1) {
          input.StackSize = evidence.StoreSizeBytes;
          input.SizeBits = evidence.StoreSizeBytes * 8;
          input.Float = evidence.StoreIsFloat;
          covered = std::min<unsigned>(
              wideSlots, static_cast<unsigned>(integerSlots.size() - cursor));
        }
      }
      tailInputs.push_back(std::move(input));
      consumedSlots += covered;
      cursor += covered;
    }
    unsigned rawPrefix = tailInputs.size();
    unsigned allowed = integerSlots.size();
    if (callsite.MaxArgs != 0) {
      unsigned maxTail = callsite.MaxArgs > callsite.FixedArgs
                             ? callsite.MaxArgs - callsite.FixedArgs
                             : 0;
      allowed = std::min(allowed, maxTail);
    }
    unsigned inferredTail = std::min(rawPrefix, allowed);
    for (unsigned index = 0; index < inferredTail; ++index) {
      shape.Inputs.push_back(tailInputs[index]);
    }
    if (rawPrefix > allowed) {
      warnings.push_back(
          varArgInferenceWarning(callsite,
                                 "tail=" + std::to_string(rawPrefix) +
                                     "/max=" + std::to_string(allowed),
                                 "vararg_evidence_truncated_by_max_args"));
    }
    if (hasLocalDefinitionAfter(integerSlots, consumedSlots + 1)) {
      warnings.push_back(varArgInferenceWarning(
          callsite, "tail=" + std::to_string(inferredTail),
          "non_contiguous_vararg_evidence"));
    }
    warnIfStoppedAtCallClobber(warnings, callsite, integerSlots, consumedSlots,
                               "vararg_evidence_stopped_at_call_clobber");
    callsiteShapes.emplace(callsite.Call, std::move(shape));
  }

  for (const auto &[callee, arities] : aritiesByCallee) {
    unsigned minArity = *std::min_element(arities.begin(), arities.end());
    unsigned maxArity = *std::max_element(arities.begin(), arities.end());
    std::string detail = "arity=" + std::to_string(minArity) + ".." +
                         std::to_string(maxArity) +
                         (minArity != maxArity
                              ? "/final=(fixed=" + std::to_string(minArity) +
                                    ",...)"
                              : "/final=" + std::to_string(maxArity));
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
    // 各调用点证据推出来的元数不一致时，真实函数几乎只能是变参：非变参函数在
    // 所有调用点元数相同。按"固定 min + 每点自己的尾巴"建模（声明成
    // (fixed, ...)），避免把 max 强加给所有调用点 —— 那会让只传 3 个参数的
    // av_log(NULL, level, "msg") 也去凑 11 个没人传的实参，每个都变成 unknown。
    // 单点元数可以小于 min 的只有推理噪声；min 作为固定前缀是这里最保守的
    // 公共部分，剩下的按点传。
    bool inferredVarArg = minArity != maxArity;
    NativeExternalPrototype prototype;
    prototype.FixedArgs = inferredVarArg ? minArity : maxArity;
    prototype.VarArg = inferredVarArg;
    prototype.MaxArgs = inferredVarArg ? maxArity : 0;
    // The first pass only infers input arity.  Do not cap unknown external
    // returns to the built-in one-register default; later demand decides them.
    prototype.MaxReturnRegisters = std::numeric_limits<unsigned>::max();
    // Prefer the callsite that reaches maxArity with the most float
    // arguments: typed params let the rewrite bind them to XMM slots instead
    // of overflowing into integer/stack slots.  Integer arguments are assumed
    // to come first (the common C shape is a handle/pointer followed by
    // floating data); interleaved float-in-the-middle shapes stay untyped.
    // 变参原型只给固定前缀定型，尾巴由每个调用点自己的证据决定。
    unsigned bestIntegerCount = 0;
    unsigned bestFloatCount = 0;
    for (const auto &[integerCount, floatCount] :
         intFloatAritiesByCallee[callee]) {
      if (integerCount + floatCount == maxArity &&
          floatCount > bestFloatCount) {
        bestIntegerCount = integerCount;
        bestFloatCount = floatCount;
      }
    }
    if (!inferredVarArg && bestFloatCount > 0) {
      prototype.TypedParams.reserve(maxArity);
      for (unsigned index = 0; index < bestIntegerCount; ++index) {
        prototype.TypedParams.push_back(
            NativeExternalPrototype::ValueType::PointerSized);
      }
      for (unsigned index = 0; index < bestFloatCount; ++index) {
        prototype.TypedParams.push_back(
            NativeExternalPrototype::ValueType::Double);
      }
    }
    prototypes[callee] = std::move(prototype);
    if (inferredVarArg) {
      auto sitesIt = unknownSitesByCallee.find(callee);
      if (sitesIt != unknownSitesByCallee.end()) {
        for (const UnknownExternalSite &site : sitesIt->second) {
          if (site.Call == nullptr) {
            continue;
          }
          // Inputs 的前 shape.Params.size() 项对应固定前缀（callParamSlots
          // 只取后面的尾巴），所以顺序按证据的整数序列在前、浮点序列在后，
          // 与上面 TypedParams 的"整数参数在前"约定一致。
          NativeExternalCallShape shape;
          shape.VarArg = true;
          shape.FixedArgs = minArity;
          shape.MaxArgs = maxArity;
          shape.FixedInputsComplete = true;
          for (unsigned index = 0; index < site.IntegerCount; ++index) {
            shape.Inputs.push_back(callInputSlot(site.IntegerSlots[index]));
          }
          for (unsigned index = 0; index < site.FloatCount; ++index) {
            shape.Inputs.push_back(callInputSlot(site.FloatSlots[index]));
          }
          callsiteShapes.emplace(site.Call, std::move(shape));
        }
      }
    }
    warnings.push_back(externalInferenceWarning(
        callee, detail,
        inferredVarArg ? "inferred_unknown_external_vararg"
                       : "inferred_unknown_external_arity",
        arities.size()));
  }
  return callsiteShapes;
}

void inferIndirectCallInputs(
    const NativeRegisterSummary &baseSummary,
    std::map<const llvm::CallBase *,
             std::vector<NativeRegisterCallInputSlot>> &out) {
  for (const NativeRegisterExternalCallsite &callsite :
       baseSummary.ExternalCallsites) {
    if (callsite.Call == nullptr || !callsite.Indirect ||
        callsite.Kind != NativeRegisterExternalCallsiteKind::UnknownExternal) {
      continue;
    }
    std::vector<NativeRegisterCallsiteSlotEvidence> integerSlots =
        renumberedSlotsByKind(callsite, false);
    std::vector<NativeRegisterCallsiteSlotEvidence> floatSlots =
        renumberedSlotsByKind(callsite, true);
    unsigned integerCount = localDefinitionPrefix(integerSlots);
    unsigned floatCount = localDefinitionPrefix(floatSlots);
    if (integerCount == 0 && floatCount == 0) {
      continue;
    }

    std::vector<NativeRegisterCallInputSlot> inputs;
    unsigned integerSeen = 0;
    unsigned floatSeen = 0;
    for (const NativeRegisterCallsiteSlotEvidence &slot : callsite.Slots) {
      if (slot.Float) {
        if (floatSeen++ < floatCount) {
          inputs.push_back(callInputSlot(slot));
        }
      } else if (integerSeen++ < integerCount) {
        inputs.push_back(callInputSlot(slot));
      }
    }
    if (!inputs.empty()) {
      out[callsite.Call] = std::move(inputs);
    }
  }
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

void addConservativeUnknownReturnSlots(
    SignatureShape &shape, llvm::Type *oldReturnType,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi,
    const std::function<bool(llvm::StringRef)> &hasLiveReadEvidence) {
  auto addReturnSlot = [&](const RegisterUnit &unit, bool floatSlot) {
    if (signatureHasReturnForRegister(shape, unit)) {
      return;
    }
    std::optional<NativeSignatureSlot> slot =
        floatSlot ? floatReturnSlotForUnit(unit, units, abi)
                  : integerReturnSlotForUnit(unit, abi);
    if (slot) {
      shape.Returns.push_back(*slot);
    }
  };

  const bool oldTypeFloat =
      oldReturnType != nullptr && oldReturnType->isFloatingPointTy();
  const bool oldTypeSt0 =
      oldTypeFloat && oldReturnType->getPrimitiveSizeInBits() > 64;

  const AbiFacts::RegisterSlot *firstFloat = nullptr;
  if (!abi.FloatOutputsInOrder.empty()) {
    firstFloat = &abi.FloatOutputsInOrder.front();
  }
  const AbiFacts::RegisterSlot *firstInt = nullptr;
  if (!abi.IntegerOutputsInOrder.empty()) {
    firstInt = &abi.IntegerOutputsInOrder.front();
  }
  const RegisterUnit *st0 = unitByName(units, "ST0");

  // Floating returns and integer returns are mutually exclusive in the
  // conservative model.  XMM0 is the ordinary SSE return; ST0 is only used
  // when there is explicit x87 return evidence.
  if (!oldTypeSt0 && firstFloat != nullptr &&
      (oldTypeFloat || hasLiveReadEvidence(firstFloat->UnitName))) {
    if (const RegisterUnit *unit = unitByName(units, firstFloat->UnitName)) {
      addReturnSlot(*unit, true);
      return;
    }
  }
  if (st0 != nullptr && (oldTypeSt0 || hasLiveReadEvidence("ST0"))) {
    addReturnSlot(*st0, true);
    return;
  }

  // Integer return: RAX is the first output; RDX is treated as a clobber
  // unless a trusted prototype explicitly says otherwise.  For direct unknown
  // externals this matches SysV practice; indirect calls use the same policy.
  for (const std::string &name : abi.OutputsInOrder) {
    if (isFloatAbiOutputUnit(abi, name)) {
      continue;
    }
    if (isLikelyNonReturnIntegerAbiOutput(abi, name)) {
      break;
    }
    const bool isFirstInt = firstInt != nullptr && name == firstInt->UnitName;
    const bool demanded =
        isFirstInt ? ((oldReturnType != nullptr &&
                       !oldReturnType->isVoidTy()) ||
                      hasLiveReadEvidence(name))
                   : hasLiveReadEvidence(name);
    if (!demanded) {
      continue;
    }
    if (const RegisterUnit *unit = unitByName(units, name)) {
      addReturnSlot(*unit, false);
    }
  }
}

void addIndirectCallsiteShapes(
    SignatureRewriteState &state,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi) {
  auto addIndirectInputParams = [&](llvm::CallBase *call,
                                    SignatureShape &shape) {
    if (!shape.Params.empty()) {
      return;
    }
    auto inputIt = state.IndirectCallInputs.find(call);
    if (inputIt == state.IndirectCallInputs.end()) {
      // 证据机制成立的前提：真实参要么是调用点前显式写进实参寄存器
      // （LocalDefinition），要么是入口透传（ForwardedEntry）。两者都没有时，
      // 寄存器里只是调用者对 caller-saved 寄存器的残留，把 6 个 ABI 入参寄存器
      // 全当实参只会在调用点凭空造出 5 个 unknown（lighttpd plugins_call_init
      // 那类，最后以裸读形式保留，见 entryRangeInput 的闸门）。所以无证据就不加形参。
      return;
    }
    for (const NativeRegisterCallInputSlot &input : inputIt->second) {
      if (std::optional<NativeSignatureSlot> slot =
              makeSignatureSlotForCallInput(input, units,
                                            call->getContext())) {
        shape.Params.push_back(*slot);
      }
    }
  };

  for (const auto &[call, helpers] : state.ReturnHelpers) {
    if (call == nullptr || call->getParent() == nullptr ||
        call->getCalledFunction() != nullptr) {
      continue;
    }
    SignatureShape &shape = state.IndirectCallShapes[call];
    addIndirectInputParams(call, shape);
    for (const auto &[name, helper] : helpers) {
      if (helper == nullptr || helper->use_empty()) {
        continue;
      }
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
    addIndirectInputParams(call, shape);
    // RangeReturnHelpers are generated for every ABI output register after an
    // analyzable call, including outputs that are never read.  An indirect
    // callee is unknown, so apply the same conservative single-class return
    // policy as direct unknown externals: old call return type first, then
    // live post-call reads, but never promote every live ABI output into a
    // multi-register aggregate.
    auto hasLiveReadEvidence = [&](llvm::StringRef name) {
      for (const RangeReturnHelper &helper : helpers) {
        if (helper.Helper == nullptr || helper.Helper->use_empty()) {
          continue;
        }
        auto unitIt = units.find(helper.Range.Global);
        if (unitIt != units.end() && unitIt->second.Name == name) {
          return true;
        }
      }
      return false;
    };
    addConservativeUnknownReturnSlots(shape, call->getType(), units, abi,
                                      hasLiveReadEvidence);
  }
}

void addDemandedExternalReturns(
    llvm::Module &module, SignatureRewriteState &state,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi, const NativeExternalPrototypeMap &prototypes) {
  // 收集有返回占位的调用点。不能直接遍历 ReturnHelpers/RangeReturnHelpers
  // （std::map<CallBase*>，顺序随堆布局），也不能遍历 std::set<CallBase*>：
  // 同一个外部 callee 的返回槽是按调用点顺序追加的，顺序一变返回结构体字段
  // 和后续取值的编号就会漂移。按 module/指令顺序收集，保证同一 IR 上稳定。
  std::set<llvm::CallBase *> wantedCalls;
  for (const auto &[call, helpers] : state.ReturnHelpers) {
    (void)helpers;
    wantedCalls.insert(call);
  }
  for (const auto &[call, helpers] : state.RangeReturnHelpers) {
    (void)helpers;
    wantedCalls.insert(call);
  }
  std::vector<llvm::CallBase *> calls;
  for (llvm::Function &function : module) {
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call != nullptr && wantedCalls.count(call) != 0) {
        calls.push_back(call);
      }
    }
  }

  for (llvm::CallBase *call : calls) {
    llvm::Function *callee = call->getCalledFunction();
    if (callee == nullptr || !callee->isDeclaration()) {
      continue;
    }
    auto shapeIt = state.Shapes.try_emplace(callee).first;
    auto rangeHelpersIt = state.RangeReturnHelpers.find(call);
    auto hasAnyRangeHelper = [&](llvm::StringRef name) {
      if (rangeHelpersIt == state.RangeReturnHelpers.end()) {
        return false;
      }
      for (const RangeReturnHelper &helper : rangeHelpersIt->second) {
        auto unitIt = units.find(helper.Range.Global);
        if (unitIt != units.end() && unitIt->second.Name == name) {
          return true;
        }
      }
      return false;
    };
    // 读证据：只有"调用后状态"占位被实际读取（有 use）才算。第一遍对每个
    // ABI 输出寄存器都会生成占位（callRangeValue），没被读的占位只是死值，
    // 不能当成"该寄存器是返回寄存器"的证据。
    auto hasLiveReadEvidence = [&](llvm::StringRef name) {
      if (rangeHelpersIt == state.RangeReturnHelpers.end()) {
        return false;
      }
      for (const RangeReturnHelper &helper : rangeHelpersIt->second) {
        if (helper.Helper == nullptr || helper.Helper->use_empty()) {
          continue;
        }
        auto unitIt = units.find(helper.Range.Global);
        if (unitIt != units.end() && unitIt->second.Name == name) {
          return true;
        }
      }
      return false;
    };
    const KnownExternalPrototype *known =
        knownExternalPrototype(prototypes, callee->getName());
    // arity 推断产物（MaxReturnRegisters = UINT_MAX，见
    // inferUnknownExternalCallsiteShapes）没有可信返回形状，返回槽推断和
    // 完全未知外部走同一套保守规则；只有内置/JSON 原型才按
    // MaxReturnRegisters 上限补槽。
    const bool trustedReturns =
        known != nullptr &&
        known->MaxReturnRegisters != std::numeric_limits<unsigned>::max();
    const std::optional<unsigned> maxReturnRegisters =
        trustedReturns
            ? std::optional<unsigned>(known->NoReturn ? 0
                                                      : known->MaxReturnRegisters)
            : std::nullopt;

    // A typed return is the complete trusted prototype.  Post-call register
    // placeholders are clobber bookkeeping and must not append RAX (or another
    // ABI output) to an explicitly selected XMM/ST0 return.
    if (known != nullptr && known->TypedReturn) {
      continue;
    }

    auto addReturnSlot = [&](const RegisterUnit &unit, bool floatSlot) {
      if (signatureHasReturnForRegister(shapeIt->second, unit)) {
        return;
      }
      std::optional<NativeSignatureSlot> slot =
          floatSlot ? floatReturnSlotForUnit(unit, units, abi)
                    : integerReturnSlotForUnit(unit, abi);
      if (slot) {
        shapeIt->second.Returns.push_back(*slot);
      }
    };

    if (trustedReturns) {
      unsigned returnIndex = 0;
      for (const std::string &name : abi.OutputsInOrder) {
        if (returnIndex >= *maxReturnRegisters) {
          break;
        }
        if (!hasAnyRangeHelper(name)) {
          ++returnIndex;
          continue;
        }
        const RegisterUnit *unit = unitByName(units, name);
        if (unit != nullptr) {
          addReturnSlot(*unit, isFloatAbiOutputUnit(abi, name));
        }
        ++returnIndex;
      }
      continue;
    }

    // 未知外部（含 arity 推断产物）：SysV 下普通函数返回要么走整型
    // （RAX，结构体可带 RDX 等），要么走浮点（XMM0 或 x87 ST0），浮点
    // 返回和整型返回互斥，保守只取一类。具体规则见
    // addConservativeUnknownReturnSlots()。
    addConservativeUnknownReturnSlots(shapeIt->second, call->getType(), units,
                                      abi, hasLiveReadEvidence);
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

unsigned classifyValueSources(
    const llvm::Value *value,
    llvm::SmallPtrSetImpl<const llvm::Value *> &visiting) {
  if (value == nullptr || !visiting.insert(value).second) {
    return 0;
  }
  if (llvm::isa<llvm::PoisonValue>(value) ||
      llvm::isa<llvm::UndefValue>(value)) {
    return SourceUnknown;
  }
  if (isDirectSummaryClobberValue(value)) {
    return SourceClobber;
  }
  if (auto *call = llvm::dyn_cast<llvm::CallBase>(value)) {
    llvm::Function *callee = call->getCalledFunction();
    if (callee != nullptr &&
        (isNotDecOpaqueUnknownName(callee->getName()) ||
         callee->getName().starts_with("notdec.register.vararg_unknown."))) {
      return SourceUnknown;
    }
    return SourceReal;
  }
  if (auto *select = llvm::dyn_cast<llvm::SelectInst>(value)) {
    return classifyValueSources(select->getTrueValue(), visiting) |
           classifyValueSources(select->getFalseValue(), visiting);
  }
  auto *user = llvm::dyn_cast<llvm::User>(value);
  if (user != nullptr &&
      (llvm::isa<llvm::PHINode>(user) || llvm::isa<llvm::CastInst>(user) ||
       llvm::isa<llvm::UnaryOperator>(user) ||
       llvm::isa<llvm::BinaryOperator>(user))) {
    unsigned sources = 0;
    for (const llvm::Use &operand : user->operands()) {
      sources |= classifyValueSources(operand.get(), visiting);
    }
    return sources;
  }
  return SourceReal;
}

unsigned classifyValueSources(const llvm::Value *value) {
  llvm::SmallPtrSet<const llvm::Value *, 16> visiting;
  return classifyValueSources(value, visiting);
}

bool mayDependOnSummaryClobberValue(const llvm::Value *value) {
  return (classifyValueSources(value) & SourceClobber) != 0;
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

void refineInternalStackParamShapes(SignatureRewriteState &state) {
  for (auto &[function, shape] : state.Shapes) {
    if (function == nullptr || function->isDeclaration()) {
      continue;
    }
    auto firstStack = std::find_if(
        shape.Params.begin(), shape.Params.end(),
        [](const NativeSignatureSlot &slot) {
          return slot.Kind == NativeSignatureSlotKind::IntegerStack;
        });
    if (firstStack == shape.Params.end()) {
      continue;
    }

    unsigned finalArity = shape.Params.size();
    bool hasDirectCall = false;
    bool hasTailCall = false;
    for (auto &[call, bindings] : state.CallArgs) {
      if (call == nullptr || call->getCalledFunction() != function) {
        continue;
      }
      hasDirectCall = true;
      if (auto *callInst = llvm::dyn_cast<llvm::CallInst>(call);
          callInst != nullptr && callInst->isTailCall()) {
        hasTailCall = true;
      }
      finalArity = std::min(finalArity, callsiteBoundArgPrefix(bindings));
    }
    if (!hasDirectCall || finalArity >= shape.Params.size()) {
      continue;
    }
    // Tail jumps preserve the caller's incoming stack slots.  Propagated
    // wrapper stack params are likewise authoritative even though their own
    // caller may be a normal call.
    bool hasPreservedParam =
        std::any_of(shape.Params.begin(), shape.Params.end(),
                    [](const NativeSignatureSlot &slot) {
                      return slot.PreserveParam && isStackSignatureSlot(slot);
                    });
    if (hasTailCall || hasPreservedParam) {
      continue;
    }
    shape.Params.resize(finalArity);
    for (auto &[call, bindings] : state.CallArgs) {
      if (call == nullptr || call->getCalledFunction() != function) {
        continue;
      }
      bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
                                    [&](const CallArgStoreBinding &binding) {
                                      return binding.Index >= finalArity;
                                    }),
                     bindings.end());
    }
  }
}

void markSignatureCallArgStores(SignatureRewriteState &state,
                                NativeRegisterEliminationSummary &summary) {
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
    const AbiFacts &abi, const NativeExternalPrototypeMap &prototypes,
    bool abiForAllFunctions) {
  std::map<llvm::Function *, SignatureShape> shapes;
  for (llvm::Function &function : module) {
    // 内置 intrinsic（notdec.register.* 命名空间 + 历史前缀）不参与签名重写，
    // 也就不会在调用点生成 summary_clobber 占位。
    if (function.isIntrinsic() ||
        function.getName().starts_with("notdec.register.") ||
        isNotDecOpaqueUnknownName(function.getName()) ||
        isNativeRegisterValueRangeName(function.getName()) ||
        isNativeRegisterPartialReadName(function.getName()) ||
        isNativeRegisterPartialWriteName(function.getName())) {
      continue;
    }
    const KnownExternalPrototype *known =
        function.isDeclaration()
            ? nullptr
            : knownExternalPrototype(prototypes, function.getName());
    SignatureShape shape =
        function.isDeclaration() || (known != nullptr && known->VarArg)
            ? shapeForKnownExternal(function, units, abi, prototypes)
            : shapeForInternalFunction(
                  function, units, summaryFacts, abi,
                  abiConventionFor(function, abiForAllFunctions));
    if (!shape.Params.empty() || !shape.Returns.empty() || shape.VarArg) {
      shapes.emplace(&function, std::move(shape));
    }
  }
  return shapes;
}

bool isStackSignatureSlot(const NativeSignatureSlot &slot) {
  return slot.Kind == NativeSignatureSlotKind::IntegerStack ||
         slot.Kind == NativeSignatureSlotKind::FloatStack;
}

bool sameStackSignatureSlot(const NativeSignatureSlot &lhs,
                            const NativeSignatureSlot &rhs) {
  return lhs.StackSpace == rhs.StackSpace &&
         lhs.StackOffset == rhs.StackOffset &&
         lhs.StackSize == rhs.StackSize;
}

void propagateTailCallStackParams(llvm::Module &module,
                                  SignatureRewriteState &state) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (llvm::Function &caller : module) {
      if (caller.isDeclaration()) {
        continue;
      }
      auto callerIt = state.Shapes.find(&caller);
      if (callerIt == state.Shapes.end()) {
        continue;
      }
      for (llvm::Instruction &inst : llvm::instructions(caller)) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
        if (call == nullptr || !call->isTailCall()) {
          continue;
        }
        llvm::Function *callee = call->getCalledFunction();
        if (callee == nullptr || callee == &caller) {
          continue;
        }
        auto calleeIt = state.Shapes.find(callee);
        if (calleeIt == state.Shapes.end()) {
          continue;
        }
        for (const NativeSignatureSlot &calleeSlot : calleeIt->second.Params) {
          if (!isStackSignatureSlot(calleeSlot)) {
            continue;
          }
          auto existing = std::find_if(
              callerIt->second.Params.begin(), callerIt->second.Params.end(),
              [&](const NativeSignatureSlot &slot) {
                return isStackSignatureSlot(slot) &&
                       sameStackSignatureSlot(slot, calleeSlot);
              });
          if (existing != callerIt->second.Params.end()) {
            if (!existing->PreserveParam) {
              existing->PreserveParam = true;
              changed = true;
            }
            continue;
          }
          NativeSignatureSlot propagated = calleeSlot;
          propagated.PreserveParam = true;
          callerIt->second.Params.push_back(std::move(propagated));
          changed = true;
        }
      }
    }
  }
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

llvm::Value *buildReturnValue(llvm::IRBuilder<> &builder,
                              const SignatureShape &shape,
                              const std::vector<llvm::Value *> &values);

bool runPostRewriteInstCombine(llvm::Module &module) {
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
  bool changed = false;
  for (llvm::Function &function : module) {
    if (!function.isDeclaration()) {
      llvm::PreservedAnalyses preserved =
          functionPasses.run(function, functionAnalysis);
      changed |= !preserved.areAllPreserved();
    }
  }
  return changed;
}

} // namespace notdec::bin2llvm
