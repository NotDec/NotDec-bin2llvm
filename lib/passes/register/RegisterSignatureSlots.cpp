#include "RegisterEliminationInternal.h"

namespace notdec::bin2llvm {

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

bool isVectorRegisterName(llvm::StringRef name) {
  return name.starts_with("XMM") || name.starts_with("YMM") ||
         name.starts_with("ZMM");
}

const RegisterUnit *resolveRegisterUnitByName(
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    llvm::StringRef name) {
  if (const RegisterUnit *unit = unitByName(units, name)) {
    return unit;
  }
  // lifting 只保留最宽的寄存器 global（ZMM0），ABI 里的 XMM0/YMM0 要映射过去。
  for (llvm::StringRef prefix : {llvm::StringRef("XMM"), llvm::StringRef("YMM")}) {
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
    if (const RegisterUnit *unit = unitByName(units, candidate)) {
      return unit;
    }
  }
  return nullptr;
}

unsigned defaultSlotWidth(llvm::StringRef name, const RegisterUnit &unit) {
  if (name.starts_with("XMM")) {
    return 128;
  }
  if (name.starts_with("YMM")) {
    return 256;
  }
  if (name.starts_with("ZMM")) {
    return 512;
  }
  return static_cast<unsigned>(unit.Size * 8);
}

// 用户显式声明的返回槽（`returns: [{"register": ..., "width": ...}]`）。
std::optional<NativeSignatureSlot> explicitRegisterSlot(
    const NativeRegisterSlotSpec &spec,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    llvm::Module &module) {
  const RegisterUnit *unit = resolveRegisterUnitByName(units, spec.Register);
  if (unit == nullptr) {
    return std::nullopt;
  }
  unsigned unitBits = static_cast<unsigned>(unit->Size * 8);
  unsigned width = spec.Width != 0 ? spec.Width : defaultSlotWidth(spec.Register, *unit);
  if (width == 0 || width > unitBits) {
    width = unitBits;
  }
  if (width == 0) {
    return std::nullopt;
  }
  NativeSignatureSlot slot;
  slot.Kind = isVectorRegisterName(spec.Register) ||
                      isVectorRegisterName(unit->Name)
                  ? NativeSignatureSlotKind::FloatRegister
                  : NativeSignatureSlotKind::IntegerRegister;
  slot.Unit = unit;
  slot.AbiName = spec.Register;
  slot.OffsetBits = 0;
  slot.SizeBits = width;
  slot.LlvmType = llvm::IntegerType::get(module.getContext(), width);
  return slot;
}

llvm::Type *llvmTypeForKnownValue(llvm::Module &module,
                                  NativeExternalPrototype::ValueType type) {
  llvm::LLVMContext &context = module.getContext();
  switch (type) {
  case NativeExternalPrototype::ValueType::I32:
    return llvm::Type::getInt32Ty(context);
  case NativeExternalPrototype::ValueType::I64:
    return llvm::Type::getInt64Ty(context);
  case NativeExternalPrototype::ValueType::PointerSized:
    return llvm::IntegerType::get(
        context, module.getDataLayout().getPointerSizeInBits());
  case NativeExternalPrototype::ValueType::Float:
    return llvm::Type::getFloatTy(context);
  case NativeExternalPrototype::ValueType::Double:
    return llvm::Type::getDoubleTy(context);
  case NativeExternalPrototype::ValueType::LongDouble:
    return llvm::Type::getX86_FP80Ty(context);
  case NativeExternalPrototype::ValueType::Vector128:
    return llvm::IntegerType::get(context, 128);
  }
  return llvm::Type::getInt64Ty(context);
}

bool isFloatKnownValue(NativeExternalPrototype::ValueType type) {
  return type == NativeExternalPrototype::ValueType::Float ||
         type == NativeExternalPrototype::ValueType::Double ||
         type == NativeExternalPrototype::ValueType::Vector128;
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

uint64_t stackSlotStep(uint32_t sizeBytes, uint32_t alignBytes) {
  if (alignBytes == 0) {
    return sizeBytes;
  }
  uint64_t rounded =
      ((static_cast<uint64_t>(sizeBytes) + alignBytes - 1) / alignBytes) *
      alignBytes;
  return std::max<uint64_t>(rounded, alignBytes);
}

std::optional<NativeSignatureSlot>
signatureStackSlotFromAbi(const AbiFacts::StackSlot &abiSlot,
                          unsigned stackIndex, llvm::Type *llvmType,
                          NativeSignatureSlotKind kind) {
  if (llvmType == nullptr || !llvmType->isSized() || abiSlot.MaxSize == 0) {
    return std::nullopt;
  }
  uint32_t sizeBytes =
      static_cast<uint32_t>((llvmType->getScalarSizeInBits() + 7) / 8);
  if (sizeBytes == 0) {
    return std::nullopt;
  }
  if (abiSlot.MinSize != 0 && sizeBytes < abiSlot.MinSize) {
    sizeBytes = abiSlot.MinSize;
  }
  if (sizeBytes > abiSlot.MaxSize) {
    return std::nullopt;
  }
  uint64_t relative = stackIndex * stackSlotStep(sizeBytes, abiSlot.Align);
  if (relative > abiSlot.MaxSize || sizeBytes > abiSlot.MaxSize - relative) {
    return std::nullopt;
  }
  NativeSignatureSlot slot;
  slot.Kind = kind;
  slot.AbiName = abiSlot.Space + "+" + std::to_string(abiSlot.Offset);
  slot.MetaType = abiSlot.MetaType;
  slot.StackSpace = abiSlot.Space;
  slot.StackOffset = abiSlot.Offset + relative;
  slot.StackSize = sizeBytes;
  slot.StackAlign = abiSlot.Align;
  slot.OffsetBits = 0;
  slot.SizeBits = sizeBytes * 8;
  slot.LlvmType = llvmType;
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
  if (sizeBits <= 80) {
    return llvm::Type::getX86_FP80Ty(context);
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

const RegisterUnit *
stackPointerUnit(const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
                 const AbiFacts &abi) {
  if (abi.StackPointer.empty()) {
    return nullptr;
  }
  for (const auto &[global, unit] : units) {
    (void)global;
    if (unit.Name == abi.StackPointer) {
      return &unit;
    }
  }
  return nullptr;
}

std::optional<NativeSignatureSlot>
stackInputSlotForLoad(llvm::LoadInst &load,
                      const NativeStackAddressAnalysis &stackAddresses,
                      const AbiFacts &abi) {
  if (abi.StackInputsInOrder.empty() || !load.getType()->isSized()) {
    return std::nullopt;
  }
  std::optional<NativeStackAddress> address =
      stackAddresses.addressForPointer(load.getPointerOperand());
  if (!address || address->Kind != NativeStackAddressKind::EntryStackPointer ||
      address->Offset < 0) {
    return std::nullopt;
  }
  uint64_t offset = static_cast<uint64_t>(address->Offset);
  uint32_t sizeBytes =
      static_cast<uint32_t>((load.getType()->getScalarSizeInBits() + 7) / 8);
  if (sizeBytes == 0) {
    return std::nullopt;
  }
  for (const AbiFacts::StackSlot &abiSlot : abi.StackInputsInOrder) {
    if (offset < abiSlot.Offset || sizeBytes < abiSlot.MinSize ||
        sizeBytes > abiSlot.MaxSize) {
      continue;
    }
    uint64_t relative = offset - abiSlot.Offset;
    if (relative > abiSlot.MaxSize || sizeBytes > abiSlot.MaxSize - relative) {
      continue;
    }
    if (abiSlot.Align != 0 && relative % abiSlot.Align != 0) {
      continue;
    }
    NativeSignatureSlot slot;
    slot.Kind = NativeSignatureSlotKind::IntegerStack;
    slot.AbiName = abiSlot.Space + "+" + std::to_string(offset);
    slot.MetaType = abiSlot.MetaType;
    slot.StackSpace = abiSlot.Space;
    slot.StackOffset = offset;
    slot.StackSize = sizeBytes;
    slot.StackAlign = abiSlot.Align;
    slot.OffsetBits = 0;
    slot.SizeBits = sizeBytes * 8;
    slot.LlvmType = load.getType();
    return slot;
  }
  return std::nullopt;
}

void appendInternalStackParams(
    SignatureShape &shape, llvm::Function &function,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi) {
  const RegisterUnit *stackPointer = stackPointerUnit(units, abi);
  if (stackPointer == nullptr) {
    return;
  }
  NativeStackAddressAnalysis stackAddresses(function, stackPointer->Global,
                                            abi.StackPointer);
  std::map<std::pair<uint64_t, unsigned>, NativeSignatureSlot> stackSlots;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
    if (load == nullptr || load->use_empty()) {
      continue;
    }
    if (std::optional<NativeSignatureSlot> slot =
            stackInputSlotForLoad(*load, stackAddresses, abi)) {
      stackSlots.emplace(std::make_pair(slot->StackOffset, slot->SizeBits),
                         *slot);
    }
  }
  for (const auto &[key, slot] : stackSlots) {
    (void)key;
    shape.Params.push_back(slot);
  }
}

SignatureShape shapeForInternalFunction(
    llvm::Function &function,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const std::map<llvm::Function *, FunctionSummaryFacts> &summaryFacts,
    const AbiFacts &abi, AbiConvention own) {
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

  // 变参识别结果（NativeVarArgRewrite 的 !notdec.vararg）：函数体里的手写保存区
  // 已经换成 llvm.va_start，变参寄存器不再是入口参数；签名要带 ...，调用点才能
  // 只传固定参数 + 真正的变参。固定参数仍然由 facts 的 ReadEntry 决定。
  shape.VarArg = isNativeVarArgFunction(function);

  // Internal native functions can be compiled with interprocedural register
  // allocation, so their real interface is not limited to the external ABI.
  // The register summary decides which registers are real inputs; ABI order
  // decides where those inputs appear in the rewritten LLVM function type.
  auto addParamForUnit = [&](const RegisterUnit &unit,
                             const AbiFacts::RegisterSlot *floatSlot) {
    if (addedParamUnits.count(unit.Name) != 0) {
      return;
    }
    // SysV 不通过 x87 栈传参：ST0/ST1 是窗口内的调用点垃圾值，入口读它们
    // 是 unknown，不能因为 InternalReturnRegisters 里有 ST0 就把它加成参数。
    if (unit.Name == "ST0" || unit.Name == "ST1") {
      return;
    }
    auto regIt = facts.Registers.find(unit.Name);
    if (regIt == facts.Registers.end() || !regIt->second.ReadEntry) {
      return;
    }
    // 遵守 ABI 的函数：形参只能是 ABI 输入寄存器（facts.Inputs 就是 cspec 标注的
    // 寄存器输入）。接口可改的函数沿用旧集合（ABI 输入 ∪ 返回/unaffected 寄存器），
    // 那本身就是本 pass 自定义的内部约定。
    if (own == AbiConvention::External) {
      if (abi.Inputs.count(unit.Name) == 0) {
        return;
      }
    } else if (abi.InternalParamRegisters.count(unit.Name) == 0 &&
               abi.InternalReturnRegisters.count(unit.Name) == 0 &&
               !isValueCarryingRegisterName(unit.Name)) {
      return;
    }
    // 返回槽里"可能还是入口值"的位（ReturnEntryMask = ExitDemand ∩ MayEntry ∩
    // MayNonEntry）也必须由形参承载：出口要交回它们，而"没写过的路径"上它们的值
    // 只能从入口来（Arch2.md 第 13 节）。调用点侧靠 demand 把"callee 入口需要的
    // 位"算成调用点的 live 需求，值才能从更早的返回槽接上（见
    // NativeRegisterSummary.cpp 的 applyBackwardCallDemand）。
    // 两类先不做（沿用 1c 的实验取舍）：浮点返回寄存器、变参函数——调用点侧还答
    // 不出整条向量/被变参占用的槽。
    // 参数宽度以 ReadEntryMask 为准："入口值在被覆盖前被读过"的位才是
    // 函数真正需要的参数位。不用 EntryDemandMask：它是反向 liveness 的
    // "入口处有有效值"，包含写保留（partial write 读改写）这类没被语义
    // 读过的位，会把参数宽度撑大。旧产物没有 ReadEntryMask 时退回
    // EntryDemandMask，再退到整寄存器；返回槽透传的位并进 paramMask。
    // "返回槽里可能还是入口值"的位要形参承载，这属于自定义约定的一部分：遵守 ABI
    // 的函数不能凭空多出形参，所以只在接口可改的函数上做。
    // 参数侧和返回侧必须用同一个集合：能在返回侧承载值的通用寄存器，也要能从形参
    // 把入口值接进来（否则返回值里"没写过的那条路径"只能裸读寄存器 global）。
    llvm::APInt returnEntryMask;
    if (own == AbiConvention::Internal &&
        (abi.InternalReturnRegisters.count(unit.Name) != 0 ||
         isValueCarryingRegisterName(unit.Name)) &&
        !shape.VarArg && !isFloatAbiOutputUnit(abi, unit.Name)) {
      returnEntryMask = regIt->second.ReturnEntryMask;
    }
    if (!regIt->second.ReadEntry &&
        (returnEntryMask.getBitWidth() == 0 || returnEntryMask.isZero())) {
      return;
    }
    llvm::APInt paramMask = regIt->second.ReadEntryMask;
    if (paramMask.getBitWidth() == 0) {
      paramMask = regIt->second.EntryDemandMask;
    }
    if (returnEntryMask.getBitWidth() != 0 && !returnEntryMask.isZero()) {
      unsigned width =
          std::max(paramMask.getBitWidth(), returnEntryMask.getBitWidth());
      paramMask = paramMask.getBitWidth() == 0
                      ? returnEntryMask.zextOrTrunc(width)
                      : paramMask.zextOrTrunc(width) |
                            returnEntryMask.zextOrTrunc(width);
    }
    // 入口读只覆盖寄存器中段时（例如 R8 的 bit 8..32：低字节在另一条路径上先被
    // 写过），这不是真正的参数 —— ABI 参数永远从 bit 0 开始，这种读是"部分重读
    // 局部写过的值 + 死 scratch 位"的组合。给它建窄形参（i24 R8.arg）会逼所有
    // 调用点去凑那几位，而调用点的值多半来自 Mixed 调用 → 一连串 unknown。
    // 不建形参：调用点不再要这几位；函数体内那一次入口读退化成裸读（partial_read，
    // 保留寄存器/位/位置），不再落成 notdec.unknown —— 一个裸读通常比每个调用点
    // 各一个 unknown 划算，而且后续还能把它提成形参/实参。
    if (paramMask.getBitWidth() != 0 && !paramMask.isZero() &&
        paramMask.countr_zero() != 0) {
      return;
    }
    if (floatSlot != nullptr) {
      if (paramMask.getBitWidth() != 0 && !paramMask.isZero()) {
        if (std::optional<NativeSignatureSlot> slot =
                floatSlotForDemand(function.getContext(), *floatSlot, units,
                                   paramMask)) {
          shape.Params.push_back(*slot);
        } else if (std::optional<NativeSignatureSlot> rangeSlot =
                       integerSlotForSingleDemandRange(unit, paramMask)) {
          shape.Params.push_back(*rangeSlot);
        } else {
          shape.Params.push_back(integerSignatureSlot(unit));
        }
      } else {
        // ReadEntry already says the internal function needs an incoming
        // value.  If the mask walker did not recover a float lane mask, use
        // the backing register type rather than leaving an entry global load
        // in the IR.
        shape.Params.push_back(integerSignatureSlot(unit));
      }
    } else if (std::optional<NativeSignatureSlot> rangeSlot =
                   integerSlotForSingleDemandRange(unit, paramMask)) {
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

  appendInternalStackParams(shape, function, units, abi);

  for (const RegisterUnit *unit : orderedUnits) {
    const bool abiReturn = abi.InternalReturnRegisters.count(unit->Name) != 0;
    if (own == AbiConvention::External && !abiReturn) {
      // 遵守 ABI：只有 ABI 输出寄存器能做返回槽。
      continue;
    }
    if (!abiReturn) {
      // 接口可改：caller-saved 的通用寄存器（RCX/R8/R9/…）也能承载返回值——返回值就是
      // 它在调用后真实持有的值，caller-saved 的语义本来就不保证它被保留，所以把它当
      // 额外返回通道是安全的。只放通用寄存器：标志位/段基址/x87 窗口/浮点向量不承载
      // C 语义的值（early 版本把 OF 也当成了返回寄存器）。
      if (!isValueCarryingRegisterName(unit->Name)) {
        continue;
      }
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
      // 返回槽只覆盖"真被写过 且 被需要"的位：从不写的位出口值就等于入口值，
      // 调用者自己的调用前值就是答案，塞进返回槽只会逼入口给它找载体
      // （Arch2.md 第 13 节）。
      llvm::APInt returnMask = intersectBitMasks(
          {regIt->second.ExitDemandMask, regIt->second.MayNonEntryMask});
      if (returnMask.getBitWidth() != 0 && returnMask.isZero()) {
        continue;
      }
      if (const AbiFacts::RegisterSlot *slot =
              floatAbiOutputSlotForUnit(abi, unit->Name)) {
        if (std::optional<NativeSignatureSlot> floatSlot =
                floatSlotForDemand(function.getContext(), *slot, units,
                                   returnMask)) {
          shape.Returns.push_back(*floatSlot);
        } else if (function.getReturnType()->isVoidTy() &&
                   returnMask.getBitWidth() != 0 && !returnMask.isZero()) {
          if (std::optional<NativeSignatureSlot> rangeSlot =
                  integerSlotForSingleDemandRange(*unit, returnMask)) {
            shape.Returns.push_back(*rangeSlot);
          } else {
            shape.Returns.push_back(integerSignatureSlot(*unit));
          }
        }
        continue;
      }
      if (std::optional<NativeSignatureSlot> rangeSlot =
              integerSlotForSingleDemandRange(*unit, returnMask)) {
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
               unsigned &floatIndex, unsigned &stackIndex,
               const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
               const AbiFacts &abi, llvm::Module &module) {
  llvm::Type *llvmType = llvmTypeForKnownValue(module, type);
  if (type == NativeExternalPrototype::ValueType::LongDouble) {
    // SysV x86 long double is an x87 memory-class argument.  It must bypass
    // the XMM pentries even though its LLVM type is floating-point.
    if (abi.StackInputsInOrder.empty()) {
      return std::nullopt;
    }
    return signatureStackSlotFromAbi(
        abi.StackInputsInOrder.front(), stackIndex++, llvmType,
        NativeSignatureSlotKind::FloatStack);
  }
  if (isFloatKnownValue(type)) {
    if (floatIndex < abi.FloatInputsInOrder.size()) {
      return signatureSlotFromAbi(abi.FloatInputsInOrder[floatIndex++], units,
                                  llvmType,
                                  NativeSignatureSlotKind::FloatRegister);
    }
    if (!abi.StackInputsInOrder.empty()) {
      return signatureStackSlotFromAbi(abi.StackInputsInOrder.front(),
                                       stackIndex++, llvmType,
                                       NativeSignatureSlotKind::IntegerStack);
    }
    return std::nullopt;
  }
  if (integerIndex < abi.IntegerInputsInOrder.size()) {
    return signatureSlotFromAbi(abi.IntegerInputsInOrder[integerIndex++], units,
                                llvmType,
                                NativeSignatureSlotKind::IntegerRegister);
  }
  if (!abi.StackInputsInOrder.empty()) {
    return signatureStackSlotFromAbi(abi.StackInputsInOrder.front(),
                                     stackIndex++, llvmType,
                                     NativeSignatureSlotKind::IntegerStack);
  }
  return std::nullopt;
}

std::optional<NativeSignatureSlot> untypedIntegerParamSlot(
    unsigned index, const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi, llvm::Module &module) {
  if (index < abi.IntegerInputsInOrder.size()) {
    return signatureSlotFromAbi(abi.IntegerInputsInOrder[index], units, nullptr,
                                NativeSignatureSlotKind::IntegerRegister);
  }
  if (abi.StackInputsInOrder.empty()) {
    return std::nullopt;
  }
  llvm::Type *llvmType = llvmTypeForKnownValue(
      module, NativeExternalPrototype::ValueType::PointerSized);
  return signatureStackSlotFromAbi(
      abi.StackInputsInOrder.front(), index - abi.IntegerInputsInOrder.size(),
      llvmType, NativeSignatureSlotKind::IntegerStack);
}

std::optional<NativeSignatureSlot>
typedReturnSlot(NativeExternalPrototype::ValueType type,
                const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
                const AbiFacts &abi, llvm::Module &module) {
  llvm::Type *llvmType = llvmTypeForKnownValue(module, type);
  if (type == NativeExternalPrototype::ValueType::LongDouble) {
    // XMM0 is earlier in the generic float output list.  Select ST0 explicitly
    // so an x87 long double return cannot be mistaken for a double return.
    for (const AbiFacts::RegisterSlot &slot : abi.FloatOutputsInOrder) {
      if (slot.UnitName == "ST0") {
        return signatureSlotFromAbi(slot, units, llvmType,
                                    NativeSignatureSlotKind::FloatRegister);
      }
    }
    return std::nullopt;
  }
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

std::optional<NativeSignatureSlot>
floatReturnSlotForUnit(const RegisterUnit &unit,
                       const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
                       const AbiFacts &abi) {
  // 浮点 ABI 输出（XMM0/ST0）的返回槽：按输出槽的位宽生成
  // double/x86_fp80，而不是整型 i64/i80。
  for (const AbiFacts::RegisterSlot &slot : abi.FloatOutputsInOrder) {
    if (slot.UnitName != unit.Name) {
      continue;
    }
    llvm::Type *type = floatTypeForSlot(unit.Global->getContext(), slot);
    if (type == nullptr) {
      return std::nullopt;
    }
    return signatureSlotFromAbi(slot, units, type,
                                NativeSignatureSlotKind::FloatRegister);
  }
  return std::nullopt;
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
  if (!known->ReturnSlots.empty()) {
    // 显式多寄存器返回：一个 callee 可以同时在 RAX/XMM0 等多个寄存器里返回，
    // 每个槽自己带宽度（i128 之类可以直接铺到 ZMM0 的 128 位子范围）。
    llvm::Module *module = function.getParent();
    if (module == nullptr) {
      return SignatureShape{};
    }
    unsigned integerIndex = 0;
    unsigned floatIndex = 0;
    unsigned stackIndex = 0;
    if (!known->TypedParams.empty()) {
      for (NativeExternalPrototype::ValueType type : known->TypedParams) {
        std::optional<NativeSignatureSlot> slot = typedParamSlot(
            type, integerIndex, floatIndex, stackIndex, units, abi, *module);
        if (!slot) {
          return SignatureShape{};
        }
        shape.Params.push_back(*slot);
      }
    } else {
      for (unsigned index = 0; index < known->FixedArgs; ++index) {
        std::optional<NativeSignatureSlot> slot =
            untypedIntegerParamSlot(index, units, abi, *module);
        if (!slot) {
          return SignatureShape{};
        }
        shape.Params.push_back(*slot);
      }
    }
    for (const NativeRegisterSlotSpec &spec : known->ReturnSlots) {
      std::optional<NativeSignatureSlot> slot =
          explicitRegisterSlot(spec, units, *module);
      if (!slot) {
        // 寄存器名在本模块里不存在：退化成"未知外部"，不要用一个残缺的形状
        // 把调用点改坏。诊断信息见 unresolved_external_return_register。
        return SignatureShape{};
      }
      shape.Returns.push_back(*slot);
    }
    shape.VarArg = known->VarArg;
    shape.MaxArgs = known->MaxArgs;
    return shape;
  }
  if (!known->TypedParams.empty() || known->TypedReturn) {
    llvm::Module *module = function.getParent();
    if (module == nullptr) {
      return shape;
    }
    unsigned integerIndex = 0;
    unsigned floatIndex = 0;
    unsigned stackIndex = 0;
    if (!known->TypedParams.empty()) {
      for (NativeExternalPrototype::ValueType type : known->TypedParams) {
        std::optional<NativeSignatureSlot> slot = typedParamSlot(
            type, integerIndex, floatIndex, stackIndex, units, abi, *module);
        if (!slot) {
          return shape;
        }
        shape.Params.push_back(*slot);
      }
    } else {
      // 只写了类型化 return、没写 params 的原型：fixed_args 仍然是形参个数。
      // 之前这里直接跳过，导致 `{"fixed_args": 1, "return": "i64"}` 的调用点
      // 被改写成零实参（悄悄丢参数）。
      for (unsigned index = 0; index < known->FixedArgs; ++index) {
        std::optional<NativeSignatureSlot> slot =
            untypedIntegerParamSlot(index, units, abi, *module);
        if (!slot) {
          return shape;
        }
        shape.Params.push_back(*slot);
      }
    }
    if (known->TypedReturn) {
      std::optional<NativeSignatureSlot> slot =
          typedReturnSlot(*known->TypedReturn, units, abi, *module);
      if (slot) {
        shape.Returns.push_back(*slot);
      }
    }
    shape.VarArg = known->VarArg;
    shape.MaxArgs = known->MaxArgs;
    return shape;
  }
  shape.VarArg = known->VarArg;
  shape.MaxArgs = known->MaxArgs;
  if (llvm::Module *module = function.getParent()) {
    for (unsigned index = 0; index < known->FixedArgs; ++index) {
      std::optional<NativeSignatureSlot> slot =
          untypedIntegerParamSlot(index, units, abi, *module);
      if (!slot) {
        break;
      }
      shape.Params.push_back(*slot);
    }
  }
  return shape;
}

} // namespace notdec::bin2llvm
