#include "RegisterEliminationInternal.h"

namespace notdec::bin2llvm {

void addFunctionSummary(NativeRegisterEliminationSummary &total,
                        const NativeRegisterEliminationFunctionSummary &fn) {
  total.LoadsSeen += fn.LoadsSeen;
  total.StoresSeen += fn.StoresSeen;
  total.LoadsReplaced += fn.LoadsReplaced;
  total.DeadLoadsRemoved += fn.DeadLoadsRemoved;
  total.DeadStoresRemoved += fn.DeadStoresRemoved;
  total.PhisCreated += fn.PhisCreated;
  total.PhisSimplified += fn.PhisSimplified;
  total.RangeEntryInputs += fn.RangeEntryInputs;
  total.EntryRangeRejectedReads += fn.EntryRangeRejectedReads;
  total.CallReturnValues += fn.CallReturnValues;
  total.CallClobberValues += fn.CallClobberValues;
  total.AbiUndefinedReturnValues += fn.AbiUndefinedReturnValues;
  total.CallArgStoresMarked += fn.CallArgStoresMarked;
  total.CallsRewritten += fn.CallsRewritten;
  total.FunctionsRewritten += fn.FunctionsRewritten;
  total.PreservedCalls += fn.PreservedCalls;
  total.UnknownCallEffects += fn.UnknownCallEffects;
  total.StackFrameAccessesRewritten += fn.StackFrameAccessesRewritten;
  total.StackFramePointerLoadsReplaced += fn.StackFramePointerLoadsReplaced;
  total.StackFrameRegisterLoadsRemoved += fn.StackFrameRegisterLoadsRemoved;
  total.StackFrameRegisterStoresRemoved += fn.StackFrameRegisterStoresRemoved;
  total.StackFrameAllocaLoadsRemoved += fn.StackFrameAllocaLoadsRemoved;
  total.StackFrameAllocaStoresRemoved += fn.StackFrameAllocaStoresRemoved;
  total.StackFrameAllocasRemoved += fn.StackFrameAllocasRemoved;
  total.PartialDemandCandidates += fn.PartialDemandCandidates;
  total.PartialDemandMatched += fn.PartialDemandMatched;
  total.PartialDemandRejected += fn.PartialDemandRejected;
  total.RangeRegistersPlanned += fn.RangeRegistersPlanned;
  total.RangeSegmentsPlanned += fn.RangeSegmentsPlanned;
  total.RangeReadEvents += fn.RangeReadEvents;
  total.RangeWriteEvents += fn.RangeWriteEvents;
  total.RangeClobberEvents += fn.RangeClobberEvents;
}

llvm::AttributeList attributesForNewFunction(llvm::Function &oldFunction,
                                             llvm::FunctionType &newType) {
  std::vector<llvm::AttributeSet> argAttrs(newType.getNumParams());
  return llvm::AttributeList::get(
      oldFunction.getContext(), oldFunction.getAttributes().getFnAttrs(),
      oldFunction.getAttributes().getRetAttrs(), argAttrs);
}

void copyFunctionMetadata(llvm::Function &from, llvm::Function &to) {
  llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> metadata;
  from.getAllMetadata(metadata);
  for (const auto &[kind, node] : metadata) {
    to.addMetadata(kind, *node);
  }
}

llvm::Function *createReplacementFunction(llvm::Function &oldFunction,
                                          llvm::FunctionType &newType) {
  llvm::Function *newFunction = llvm::Function::Create(
      &newType, oldFunction.getLinkage(), oldFunction.getAddressSpace());
  newFunction->copyAttributesFrom(&oldFunction);
  newFunction->setAttributes(attributesForNewFunction(oldFunction, newType));
  copyFunctionMetadata(oldFunction, *newFunction);
  newFunction->setComdat(oldFunction.getComdat());
  newFunction->setCallingConv(oldFunction.getCallingConv());
  oldFunction.getParent()->getFunctionList().insert(oldFunction.getIterator(),
                                                    newFunction);
  newFunction->takeName(&oldFunction);
  return newFunction;
}

llvm::Value *castSlotValueToRegister(llvm::IRBuilder<> &builder,
                                     llvm::Value *value,
                                     const NativeSignatureSlot &slot);

llvm::Value *buildReturnValue(llvm::IRBuilder<> &builder,
                              const SignatureShape &shape,
                              const std::vector<llvm::Value *> &values) {
  auto functionName = [&]() -> llvm::StringRef {
    llvm::BasicBlock *block = builder.GetInsertBlock();
    return block == nullptr || block->getParent() == nullptr
               ? llvm::StringRef("")
               : block->getParent()->getName();
  };
  if (shape.Returns.empty()) {
    return nullptr;
  }
  if (shape.Returns.size() == 1) {
    llvm::Type *retTy = singleReturnType(shape);
    if (values.empty() || values.front()->getType() != retTy) {
      llvm::Value *unknown =
          unknownValueAt(builder, retTy, "notdec.return_unknown");
      attachUnknownValueMetadata(unknown, "function_return_missing_value",
                                 functionName(), "", "", "function_return");
      return unknown;
    }
    return values.front();
  }
  llvm::Type *retTy = returnTypeForShape(builder.getContext(), shape);
  llvm::Value *result = llvm::UndefValue::get(retTy);
  for (unsigned index = 0; index < shape.Returns.size(); ++index) {
    const NativeSignatureSlot &slot = shape.Returns[index];
    llvm::Value *value =
        index < values.size()
            ? values[index]
            : unknownValueAt(builder, slotType(slot),
                             slot.Unit->Name + ".return_unknown");
    if (index >= values.size()) {
      attachUnknownValueMetadata(value, "function_return_missing_value",
                                 functionName(), "", slot.Unit->Name,
                                 "function_return");
    }
    if (value->getType() != slotType(slot)) {
      value = unknownValueAt(builder, slotType(slot),
                             slot.Unit->Name + ".return_unknown");
      attachUnknownValueMetadata(value, "function_return_type_mismatch",
                                 functionName(), "", slot.Unit->Name,
                                 "function_return");
    }
    result = builder.CreateInsertValue(result, value, {index});
  }
  return result;
}

llvm::Value *extractReturnRegister(llvm::IRBuilder<> &builder,
                                   const SignatureShape &shape,
                                   llvm::Value &call,
                                   llvm::StringRef registerName) {
  for (unsigned index = 0; index < shape.Returns.size(); ++index) {
    const NativeSignatureSlot &slot = shape.Returns[index];
    if (slot.Unit->Name != registerName) {
      continue;
    }
    llvm::Value *value = nullptr;
    if (shape.Returns.size() == 1) {
      value = &call;
    } else {
      value = builder.CreateExtractValue(&call, {index},
                                         registerName.str() + ".ret");
    }
    if (value->getType() == slot.Unit->Global->getValueType()) {
      return value;
    }
    return castSlotValueToRegister(builder, value, slot);
  }
  return nullptr;
}

llvm::Module *moduleForBuilder(llvm::IRBuilder<> &builder) {
  llvm::BasicBlock *block = builder.GetInsertBlock();
  return block == nullptr ? nullptr : block->getModule();
}

llvm::Value *createValueRangeExtract(llvm::IRBuilder<> &builder,
                                     llvm::Value *source, unsigned readWidth,
                                     uint64_t bitOffset, llvm::Twine name) {
  if (source == nullptr || !source->getType()->isIntegerTy() ||
      readWidth == 0) {
    return nullptr;
  }
  auto *sourceType = llvm::cast<llvm::IntegerType>(source->getType());
  if (bitOffset + readWidth > sourceType->getBitWidth()) {
    return nullptr;
  }
  auto *resultType = llvm::IntegerType::get(builder.getContext(), readWidth);
  if (bitOffset == 0 && sourceType == resultType) {
    return source;
  }
  llvm::Module *module = moduleForBuilder(builder);
  if (module == nullptr) {
    return nullptr;
  }
  llvm::Function *callee = getOrInsertNativeRegisterValueExtract(
      *module, sourceType, resultType, sourceType->getBitWidth(), readWidth);
  llvm::Value *offset = llvm::ConstantInt::get(
      llvm::Type::getInt64Ty(builder.getContext()), bitOffset);
  return builder.CreateCall(callee, {source, offset}, name);
}

llvm::Value *createValueRangeInsert(llvm::IRBuilder<> &builder,
                                    llvm::Value *base, llvm::Value *value,
                                    unsigned writeWidth, uint64_t bitOffset,
                                    llvm::Twine name) {
  if (base == nullptr || value == nullptr || !base->getType()->isIntegerTy() ||
      !value->getType()->isIntegerTy() || writeWidth == 0 ||
      value->getType()->getIntegerBitWidth() != writeWidth) {
    return nullptr;
  }
  auto *baseType = llvm::cast<llvm::IntegerType>(base->getType());
  if (bitOffset + writeWidth > baseType->getBitWidth()) {
    return nullptr;
  }
  if (bitOffset == 0 && writeWidth == baseType->getBitWidth() &&
      value->getType() == baseType) {
    return value;
  }
  llvm::Module *module = moduleForBuilder(builder);
  if (module == nullptr) {
    return nullptr;
  }
  llvm::Function *callee = getOrInsertNativeRegisterValueInsert(
      *module, baseType, value->getType(), baseType->getBitWidth(), writeWidth);
  llvm::Value *offset = llvm::ConstantInt::get(
      llvm::Type::getInt64Ty(builder.getContext()), bitOffset);
  return builder.CreateCall(callee, {base, value, offset}, name);
}

llvm::Value *extractReturnRange(llvm::IRBuilder<> &builder,
                                const SignatureShape &shape, llvm::Value &call,
                                const RegisterRangeKey &range) {
  for (unsigned index = 0; index < shape.Returns.size(); ++index) {
    const NativeSignatureSlot &slot = shape.Returns[index];
    if (slot.Unit == nullptr || slot.Unit->Global != range.Global) {
      continue;
    }
    uint64_t slotEnd = slot.OffsetBits + slot.SizeBits;
    uint64_t rangeEnd = range.BitOffset + range.BitWidth;
    if (range.BitOffset < slot.OffsetBits || rangeEnd > slotEnd) {
      continue;
    }

    llvm::Value *slotValue = nullptr;
    if (shape.Returns.size() == 1) {
      slotValue = &call;
    } else {
      slotValue = builder.CreateExtractValue(&call, {index},
                                             slot.Unit->Name + ".ret_slot");
    }
    llvm::Value *registerValue =
        castSlotValueToRegister(builder, slotValue, slot);
    if (registerValue == nullptr ||
        registerValue->getType() != slot.Unit->Global->getValueType()) {
      return nullptr;
    }
    auto *registerType =
        llvm::dyn_cast<llvm::IntegerType>(registerValue->getType());
    if (registerType == nullptr ||
        range.BitOffset + range.BitWidth > registerType->getBitWidth()) {
      return nullptr;
    }
    return createValueRangeExtract(builder, registerValue, range.BitWidth,
                                   range.BitOffset,
                                   slot.Unit->Name + ".ret_range");
  }
  return nullptr;
}

llvm::Value *foreignArgumentReplacement(
    llvm::Function &function, llvm::Argument &argument,
    std::map<llvm::Type *, llvm::Value *> &unknownByType) {
  for (llvm::Argument &candidate : function.args()) {
    if (candidate.getName() == argument.getName() &&
        candidate.getType() == argument.getType()) {
      return &candidate;
    }
  }
  auto cached = unknownByType.find(argument.getType());
  if (cached != unknownByType.end()) {
    return cached->second;
  }
  llvm::IRBuilder<> builder(&function.getEntryBlock(),
                            function.getEntryBlock().getFirstNonPHIIt());
  llvm::Value *unknown =
      unknownValueAt(builder, argument.getType(), argument.getName() + ".old");
  attachUnknownValueMetadata(unknown, "foreign_argument_replacement",
                             function.getName(), "", argument.getName(),
                             "localize");
  unknownByType.emplace(argument.getType(), unknown);
  return unknown;
}

void replaceForeignArgumentsInBody(llvm::Function &function) {
  std::map<llvm::Type *, llvm::Value *> unknownByType;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    for (llvm::Use &operand : inst.operands()) {
      auto *argument = llvm::dyn_cast<llvm::Argument>(operand.get());
      if (argument == nullptr || argument->getParent() == &function) {
        continue;
      }
      operand.set(
          foreignArgumentReplacement(function, *argument, unknownByType));
    }
  }
}

llvm::Value *localizeReturnValue(llvm::Function &function,
                                 llvm::ReturnInst &insertBefore,
                                 llvm::Value *value) {
  auto *argument = llvm::dyn_cast<llvm::Argument>(value);
  if (argument != nullptr && argument->getParent() != &function) {
    std::map<llvm::Type *, llvm::Value *> unknownByType;
    return foreignArgumentReplacement(function, *argument, unknownByType);
  }
  auto *instruction = llvm::dyn_cast<llvm::Instruction>(value);
  if (instruction != nullptr && instruction->getFunction() != &function) {
    llvm::Value *unknown = unknownValueBefore(insertBefore, value->getType(),
                                              value->getName() + ".old");
    attachUnknownValueMetadata(unknown, "foreign_return_value_replacement",
                               function.getName(), "", value->getName(),
                               "localize_return");
    return unknown;
  }
  return value;
}

llvm::Value *localizeCallArgument(llvm::Function &function,
                                  llvm::Instruction &insertBefore,
                                  llvm::Value *value) {
  auto *argument = llvm::dyn_cast<llvm::Argument>(value);
  if (argument != nullptr && argument->getParent() != &function) {
    std::map<llvm::Type *, llvm::Value *> unknownByType;
    return foreignArgumentReplacement(function, *argument, unknownByType);
  }
  auto *instruction = llvm::dyn_cast<llvm::Instruction>(value);
  if (instruction != nullptr && instruction->getFunction() != &function) {
    llvm::Value *unknown = unknownValueBefore(insertBefore, value->getType(),
                                              value->getName() + ".old");
    attachUnknownValueMetadata(unknown, "foreign_call_argument_replacement",
                               function.getName(), "", value->getName(),
                               "localize_call_arg");
    return unknown;
  }
  return value;
}

llvm::Value *castSlotValueToRegister(llvm::IRBuilder<> &builder,
                                     llvm::Value *value,
                                     const NativeSignatureSlot &slot) {
  if (value == nullptr) {
    return nullptr;
  }
  llvm::Type *registerType = slot.Unit->Global->getValueType();
  if (slot.Kind == NativeSignatureSlotKind::IntegerRegister) {
    if (value->getType() == registerType) {
      return value;
    }
    if (value->getType()->isIntegerTy() && registerType->isIntegerTy()) {
      unsigned srcBits = value->getType()->getIntegerBitWidth();
      unsigned dstBits = registerType->getIntegerBitWidth();
      if (srcBits < dstBits) {
        return builder.CreateZExt(value, registerType,
                                  slot.Unit->Name + ".ret_zext");
      }
      if (srcBits > dstBits) {
        return builder.CreateTrunc(value, registerType,
                                   slot.Unit->Name + ".ret_trunc");
      }
    }
    return nullptr;
  }

  if (!registerType->isIntegerTy()) {
    return nullptr;
  }
  unsigned registerBits = registerType->getIntegerBitWidth();
  if (slot.OffsetBits >= registerBits || slot.SizeBits == 0 ||
      slot.OffsetBits + slot.SizeBits > registerBits) {
    return nullptr;
  }
  // 浮点/向量返回槽的值可能是 float/double（bitcast 成位模式），也可能是整数
  // （`return: "xmm128"` 这种 128 位向量槽用 i128 表示）。两种都按位宽铺到
  // 寄存器槽位里。
  llvm::Type *intType =
      llvm::IntegerType::get(builder.getContext(), slot.SizeBits);
  llvm::Value *bits = nullptr;
  if (value->getType() == intType) {
    bits = value;
  } else if (value->getType()->isFloatingPointTy()) {
    bits = builder.CreateBitCast(value, intType, slot.Unit->Name + ".ret_bits");
  } else if (value->getType()->isIntegerTy() &&
             value->getType()->getIntegerBitWidth() < slot.SizeBits) {
    bits = builder.CreateZExt(value, intType,
                              slot.Unit->Name + ".ret_zext_bits");
  } else {
    return nullptr;
  }
  llvm::Value *zero = llvm::ConstantInt::get(registerType, 0);
  return createValueRangeInsert(builder, zero, bits, slot.SizeBits,
                                slot.OffsetBits,
                                slot.Unit->Name + ".ret_insert");
}

llvm::CallInst *rewriteCallInst(llvm::CallBase &oldCall, llvm::Value &callee,
                                llvm::FunctionType &newType,
                                const std::vector<llvm::Value *> &args) {
  llvm::SmallVector<llvm::OperandBundleDef, 2> bundles;
  oldCall.getOperandBundlesAsDefs(bundles);
  llvm::CallInst *newCall = llvm::CallInst::Create(
      &newType, &callee, args, bundles, "", oldCall.getIterator());
  if (auto *oldCallInst = llvm::dyn_cast<llvm::CallInst>(&oldCall)) {
    newCall->setTailCallKind(oldCallInst->getTailCallKind());
  }
  newCall->setCallingConv(oldCall.getCallingConv());
  std::vector<llvm::AttributeSet> argAttrs(args.size());
  newCall->setAttributes(llvm::AttributeList::get(
      oldCall.getContext(), oldCall.getAttributes().getFnAttrs(),
      oldCall.getAttributes().getRetAttrs(), argAttrs));
  newCall->copyMetadata(oldCall);
  return newCall;
}

llvm::Value *replacementForOldCallUses(llvm::IRBuilder<> &builder,
                                       llvm::CallBase &oldCall,
                                       llvm::CallInst &newCall,
                                       const SignatureShape &shape) {
  if (oldCall.getType()->isVoidTy()) {
    return nullptr;
  }
  if (oldCall.getType() == newCall.getType()) {
    return &newCall;
  }
  for (unsigned index = 0; index < shape.Returns.size(); ++index) {
    if (shape.Returns[index].Unit->Global->getValueType() !=
        oldCall.getType()) {
      continue;
    }
    if (shape.Returns.size() == 1) {
      return &newCall;
    }
    return builder.CreateExtractValue(&newCall, {index},
                                      oldCall.getName() + ".ret");
  }
  return nullptr;
}

llvm::Value *integerEntrySlotReplacement(llvm::Instruction &inst,
                                         llvm::LoadInst &entryLoad,
                                         const NativeSignatureSlot &slot,
                                         llvm::Argument &arg,
                                         llvm::IRBuilder<> &builder) {
  if (slot.Kind != NativeSignatureSlotKind::IntegerRegister ||
      !arg.getType()->isIntegerTy()) {
    return nullptr;
  }
  auto *argType = llvm::cast<llvm::IntegerType>(arg.getType());
  auto buildSubrange = [&](uint64_t offset, unsigned width) -> llvm::Value * {
    if (width == 0 || offset < slot.OffsetBits ||
        offset + width > slot.OffsetBits + slot.SizeBits) {
      return nullptr;
    }
    uint64_t argOffset = offset - slot.OffsetBits;
    if (argOffset + width > argType->getBitWidth()) {
      return nullptr;
    }
    return createValueRangeExtract(builder, &arg, width, argOffset,
                                   slot.Unit->Name + ".entry_arg");
  };

  if (inst.getType() == arg.getType() && &inst == &entryLoad &&
      slot.OffsetBits == 0 &&
      slot.SizeBits == arg.getType()->getIntegerBitWidth()) {
    return &arg;
  }
  auto *trunc = llvm::dyn_cast<llvm::TruncInst>(&inst);
  if (trunc != nullptr && trunc->getOperand(0) == &entryLoad) {
    return buildSubrange(0, trunc->getType()->getIntegerBitWidth());
  }
  auto *andOp = llvm::dyn_cast<llvm::BinaryOperator>(&inst);
  if (andOp != nullptr && andOp->getOpcode() == llvm::Instruction::And) {
    llvm::Value *source = nullptr;
    llvm::ConstantInt *mask = nullptr;
    if (andOp->getOperand(0) == &entryLoad) {
      source = andOp->getOperand(0);
      mask = llvm::dyn_cast<llvm::ConstantInt>(andOp->getOperand(1));
    } else if (andOp->getOperand(1) == &entryLoad) {
      source = andOp->getOperand(1);
      mask = llvm::dyn_cast<llvm::ConstantInt>(andOp->getOperand(0));
    }
    if (source != nullptr && mask != nullptr &&
        mask->getValue().isMask(mask->getBitWidth())) {
      unsigned width = mask->getValue().countTrailingOnes();
      llvm::Value *narrow = buildSubrange(0, width);
      if (narrow == nullptr) {
        return nullptr;
      }
      return builder.CreateZExtOrTrunc(narrow, inst.getType(),
                                       slot.Unit->Name + ".entry_arg_wide");
    }
    if (source != nullptr && mask != nullptr &&
        mask->getValue().isShiftedMask()) {
      unsigned offset = mask->getValue().countr_zero();
      unsigned width = mask->getValue().getActiveBits() - offset;
      llvm::Value *narrow = buildSubrange(offset, width);
      if (narrow == nullptr) {
        return nullptr;
      }
      auto *instType = llvm::dyn_cast<llvm::IntegerType>(inst.getType());
      if (instType == nullptr) {
        return nullptr;
      }
      llvm::Value *zero = llvm::ConstantInt::get(instType, 0);
      return createValueRangeInsert(builder, zero, narrow, width, offset,
                                    slot.Unit->Name + ".entry_arg_shifted");
    }
  }
  auto *shift = llvm::dyn_cast<llvm::LShrOperator>(&inst);
  if (shift != nullptr && shift->getOperand(0) == &entryLoad) {
    auto *amount = llvm::dyn_cast<llvm::ConstantInt>(shift->getOperand(1));
    if (amount != nullptr) {
      unsigned width = shift->getType()->getIntegerBitWidth();
      llvm::Value *narrow = buildSubrange(amount->getZExtValue(), width);
      if (narrow == nullptr) {
        return nullptr;
      }
      return builder.CreateZExtOrTrunc(narrow, shift->getType(),
                                       slot.Unit->Name + ".entry_arg_wide");
    }
  }
  if (trunc != nullptr) {
    auto *shiftOp = llvm::dyn_cast<llvm::LShrOperator>(trunc->getOperand(0));
    if (shiftOp != nullptr && shiftOp->getOperand(0) == &entryLoad) {
      auto *amount = llvm::dyn_cast<llvm::ConstantInt>(shiftOp->getOperand(1));
      if (amount != nullptr) {
        return buildSubrange(amount->getZExtValue(),
                             trunc->getType()->getIntegerBitWidth());
      }
    }
  }
  if (andOp != nullptr && andOp->getOpcode() == llvm::Instruction::And) {
    auto *shiftOp = llvm::dyn_cast<llvm::LShrOperator>(andOp->getOperand(0));
    auto *mask = llvm::dyn_cast<llvm::ConstantInt>(andOp->getOperand(1));
    if (shiftOp == nullptr || mask == nullptr) {
      shiftOp = llvm::dyn_cast<llvm::LShrOperator>(andOp->getOperand(1));
      mask = llvm::dyn_cast<llvm::ConstantInt>(andOp->getOperand(0));
    }
    if (shiftOp != nullptr && shiftOp->getOperand(0) == &entryLoad &&
        mask != nullptr && mask->getValue().isMask(mask->getBitWidth())) {
      auto *amount = llvm::dyn_cast<llvm::ConstantInt>(shiftOp->getOperand(1));
      if (amount == nullptr) {
        return nullptr;
      }
      unsigned width = mask->getValue().countTrailingOnes();
      llvm::Value *narrow = buildSubrange(amount->getZExtValue(), width);
      if (narrow == nullptr) {
        return nullptr;
      }
      return builder.CreateZExtOrTrunc(narrow, inst.getType(),
                                       slot.Unit->Name + ".entry_arg_wide");
    }
  }
  return nullptr;
}

llvm::Value *rangeEntrySlotReplacement(llvm::Instruction &inst,
                                       const NativeSignatureSlot &slot,
                                       llvm::Argument &arg,
                                       llvm::IRBuilder<> &builder) {
  llvm::MDNode *metadata =
      inst.getMetadata("notdec.register.summary_ssa.range_entry");
  if (metadata == nullptr || slot.Unit == nullptr) {
    return nullptr;
  }
  std::optional<std::string> name = mdField(metadata, "name");
  std::optional<std::string> offsetText = mdField(metadata, "bit_offset");
  std::optional<std::string> widthText = mdField(metadata, "bit_width");
  if (!name || *name != slot.Unit->Name || !offsetText || !widthText) {
    return nullptr;
  }

  uint64_t offset = 0;
  uint64_t width = 0;
  if (llvm::StringRef(*offsetText).getAsInteger(10, offset) ||
      llvm::StringRef(*widthText).getAsInteger(10, width) || width == 0 ||
      offset < slot.OffsetBits ||
      offset + width > slot.OffsetBits + slot.SizeBits) {
    return nullptr;
  }

  if (slot.Kind == NativeSignatureSlotKind::FloatRegister) {
    if (offset != slot.OffsetBits || width != slot.SizeBits ||
        !arg.getType()->isFloatingPointTy() ||
        width != arg.getType()->getScalarSizeInBits()) {
      return nullptr;
    }
    auto *bitsType = llvm::IntegerType::get(arg.getContext(), width);
    return builder.CreateBitCast(&arg, bitsType,
                                 slot.Unit->Name + ".range_entry_arg");
  }

  if (!arg.getType()->isIntegerTy()) {
    return nullptr;
  }
  auto *argType = llvm::cast<llvm::IntegerType>(arg.getType());
  uint64_t argOffset = offset - slot.OffsetBits;
  if (argOffset + width > argType->getBitWidth()) {
    return nullptr;
  }
  return createValueRangeExtract(builder, &arg, width, argOffset,
                                 slot.Unit->Name + ".range_entry_arg");
}

void rewriteInternalFunctionBody(
    llvm::Function &oldFunction, llvm::Function &newFunction,
    const SignatureShape &shape, SignatureRewriteState &state,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi) {
  newFunction.splice(newFunction.end(), &oldFunction);
  const RegisterUnit *stackPointer = stackPointerUnit(units, abi);
  NativeStackAddressAnalysis stackAddresses(
      newFunction, stackPointer == nullptr ? nullptr : stackPointer->Global,
      abi.StackPointer);
  unsigned index = 0;
  for (llvm::Argument &arg : newFunction.args()) {
    if (index >= shape.Params.size()) {
      break;
    }
    const NativeSignatureSlot &slot = shape.Params[index];
    arg.setName(slotRegisterName(slot) + ".arg");
    std::vector<llvm::LoadInst *> entryLoads;
    if (slot.Unit != nullptr && slot.Unit->Global != nullptr) {
      for (llvm::Instruction &inst : llvm::instructions(newFunction)) {
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
        if (load != nullptr &&
            load->getMetadata("notdec.register.summary_ssa.entry") != nullptr &&
            load->getPointerOperand()->stripPointerCasts() ==
                slot.Unit->Global) {
          entryLoads.push_back(load);
        }
      }
    }
    for (llvm::BasicBlock &block : newFunction) {
      for (auto it = block.begin(); it != block.end();) {
        llvm::Instruction &inst = *it++;
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
        if (slot.Kind == NativeSignatureSlotKind::IntegerStack) {
          if (load != nullptr && load->getType() == arg.getType()) {
            std::optional<NativeSignatureSlot> loadSlot =
                stackInputSlotForLoad(*load, stackAddresses, abi);
            if (loadSlot && loadSlot->StackOffset == slot.StackOffset &&
                loadSlot->SizeBits == slot.SizeBits) {
              state.ValueMap[load] = &arg;
              load->replaceAllUsesWith(&arg);
            }
          }
          continue;
        }
        if (slot.Unit == nullptr || slot.Unit->Global == nullptr) {
          continue;
        }
        if (load != nullptr &&
            load->getMetadata("notdec.register.summary_ssa.entry") != nullptr) {
          auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
              load->getPointerOperand()->stripPointerCasts());
          if (global == slot.Unit->Global && load->getType() == arg.getType()) {
            state.ValueMap[load] = &arg;
            load->replaceAllUsesWith(&arg);
          }
          continue;
        }

        if (slot.Unit == nullptr || slot.Unit->Global == nullptr) {
          continue;
        }
        if (inst.getMetadata("notdec.register.summary_ssa.range_entry") !=
            nullptr) {
          llvm::IRBuilder<> builder(&inst);
          llvm::Value *replacement =
              rangeEntrySlotReplacement(inst, slot, arg, builder);
          if (replacement != nullptr &&
              replacement->getType() == inst.getType()) {
            state.ValueMap[&inst] = replacement;
            inst.replaceAllUsesWith(replacement);
          }
          continue;
        }
        for (llvm::LoadInst *entryLoad : entryLoads) {
          llvm::IRBuilder<> builder(&inst);
          llvm::Value *replacement =
              integerEntrySlotReplacement(inst, *entryLoad, slot, arg, builder);
          if (replacement != nullptr &&
              replacement->getType() == inst.getType()) {
            state.ValueMap[&inst] = replacement;
            inst.replaceAllUsesWith(replacement);
            break;
          }
        }
      }
    }
    ++index;
  }
  replaceForeignArgumentsInBody(newFunction);

  auto returnsIt = state.FunctionReturns.find(&oldFunction);
  for (llvm::BasicBlock &block : newFunction) {
    auto *oldRet =
        llvm::dyn_cast_or_null<llvm::ReturnInst>(block.getTerminator());
    if (oldRet == nullptr) {
      continue;
    }
    std::vector<llvm::Value *> values;
    if (returnsIt != state.FunctionReturns.end()) {
      auto valueIt = returnsIt->second.find(oldRet);
      if (valueIt != returnsIt->second.end()) {
        values = valueIt->second;
      }
    }
    llvm::IRBuilder<> builder(oldRet);
    for (llvm::Value *&value : values) {
      while (state.ValueMap.count(value) != 0 &&
             state.ValueMap[value] != value) {
        value = state.ValueMap[value];
      }
      value = localizeReturnValue(newFunction, *oldRet, value);
    }
    if (shape.Returns.empty()) {
      builder.CreateRetVoid();
    } else {
      builder.CreateRet(buildReturnValue(builder, shape, values));
    }
    oldRet->eraseFromParent();
  }
}

unsigned integerBitWidthOf(llvm::Value *value) {
  if (value == nullptr || value->getType() == nullptr ||
      !value->getType()->isIntegerTy()) {
    return 0;
  }
  return value->getType()->getIntegerBitWidth();
}

llvm::Value *resolveValueMap(llvm::Value *value,
                             std::map<llvm::Value *, llvm::Value *> &valueMap) {
  while (value != nullptr && valueMap.count(value) != 0 &&
         valueMap[value] != value) {
    value = valueMap[value];
  }
  return value;
}

bool valueMayDependOnUnknownPlaceholder(
    llvm::Value *value, llvm::SmallPtrSetImpl<llvm::Value *> &seen) {
  if (value == nullptr) {
    return false;
  }
  if (llvm::isa<llvm::PoisonValue>(value) ||
      llvm::isa<llvm::UndefValue>(value)) {
    return true;
  }
  if (llvm::isa<llvm::Constant>(value)) {
    return false;
  }
  if (!seen.insert(value).second) {
    return false;
  }

  if (auto *freeze = llvm::dyn_cast<llvm::FreezeInst>(value)) {
    return valueMayDependOnUnknownPlaceholder(freeze->getOperand(0), seen);
  }
  if (auto *call = llvm::dyn_cast<llvm::CallBase>(value)) {
    llvm::Function *callee = call->getCalledFunction();
    if (callee != nullptr &&
        (callee->getName().starts_with("notdec.register.vararg_unknown.") ||
         isNotDecOpaqueUnknownName(callee->getName()))) {
      return true;
    }
    if (std::optional<NativeRegisterValueInsertInfo> insert =
            parseNativeRegisterValueInsert(*call)) {
      return valueMayDependOnUnknownPlaceholder(insert->Base, seen) ||
             valueMayDependOnUnknownPlaceholder(insert->Value, seen);
    }
    if (std::optional<NativeRegisterValueExtractInfo> extract =
            parseNativeRegisterValueExtract(*call)) {
      return valueMayDependOnUnknownPlaceholder(extract->FullValue, seen);
    }
    return false;
  }

  // These are value-only glue nodes created while rebuilding register ranges.
  // If any piece comes from a freeze-poison placeholder, the final vararg value
  // is not a real source value even if InstCombine can later pick 0 for it.
  bool shouldInspectOperands = llvm::isa<llvm::PHINode>(value) ||
                               llvm::isa<llvm::SelectInst>(value) ||
                               llvm::isa<llvm::CastInst>(value) ||
                               llvm::isa<llvm::BinaryOperator>(value) ||
                               llvm::isa<llvm::UnaryOperator>(value) ||
                               llvm::isa<llvm::ExtractValueInst>(value) ||
                               llvm::isa<llvm::InsertValueInst>(value);
  if (!shouldInspectOperands) {
    return false;
  }
  auto *user = llvm::cast<llvm::User>(value);
  for (llvm::Value *operand : user->operands()) {
    if (valueMayDependOnUnknownPlaceholder(operand, seen)) {
      return true;
    }
  }
  return false;
}

// 值本身或它的计算链里是否含 poison/undef（含 freeze(poison)、phi 的 poison
// 入边、select 的 poison 分支）。这类值不能当实参传给 callee：调用点会变成
// UB，而 poison 又带不了 !notdec.unknown.source 归因。
bool valueDependsOnPoison(llvm::Value *value,
                          llvm::SmallPtrSetImpl<llvm::Value *> &seen) {
  if (value == nullptr) {
    return false;
  }
  if (llvm::isa<llvm::PoisonValue>(value) ||
      llvm::isa<llvm::UndefValue>(value)) {
    return true;
  }
  if (llvm::isa<llvm::Constant>(value)) {
    return false;
  }
  if (!seen.insert(value).second) {
    return false;
  }
  if (auto *freeze = llvm::dyn_cast<llvm::FreezeInst>(value)) {
    return valueDependsOnPoison(freeze->getOperand(0), seen);
  }
  if (llvm::isa<llvm::PHINode>(value) || llvm::isa<llvm::SelectInst>(value) ||
      llvm::isa<llvm::CastInst>(value) ||
      llvm::isa<llvm::BinaryOperator>(value) ||
      llvm::isa<llvm::UnaryOperator>(value) ||
      llvm::isa<llvm::ExtractValueInst>(value) ||
      llvm::isa<llvm::InsertValueInst>(value)) {
    for (llvm::Value *operand :
         llvm::cast<llvm::User>(value)->operand_values()) {
      if (valueDependsOnPoison(operand, seen)) {
        return true;
      }
    }
  }
  return false;
}

bool valueDependsOnPoison(llvm::Value *value) {
  llvm::SmallPtrSet<llvm::Value *, 16> seen;
  return valueDependsOnPoison(value, seen);
}

bool valueMayDependOnUnknownPlaceholder(llvm::Value *value) {
  llvm::SmallPtrSet<llvm::Value *, 16> seen;
  return valueMayDependOnUnknownPlaceholder(value, seen);
}

bool shouldPoisonUnprovenVarArg(const CallArgStoreBinding &binding,
                                llvm::Value *value,
                                const llvm::DataLayout &layout,
                                const llvm::Instruction *context) {
  (void)binding;
  (void)layout;
  (void)context;
  // 只要值不依赖 poison / undef / freeze / unknown 占位，它就是数据流里
  // 真实的值；即使恰好是 0（`xorl %edi,%edi`、`movl $0,...` 这类真实初始化）
  // 也应当原样传进变参槽。旧规则"known-zero 就换占位"会把这类真实零也变成
  // vararg_unknown→poison：既丢了 !notdec.unknown.source 归因，又让调用点
  // 成为 UB（hexx64 全量 3227 个变参占位里有 1878 个是这种假零）。
  return valueMayDependOnUnknownPlaceholder(value);
}

bool collectInsertedExtractPieces(
    llvm::Value *value, unsigned fullWidth,
    std::map<llvm::Value *, llvm::Value *> &valueMap, llvm::Value *&source,
    llvm::APInt &covered) {
  value = resolveValueMap(value, valueMap);
  if (auto *constant = llvm::dyn_cast_or_null<llvm::ConstantInt>(value)) {
    return constant->isZero() && constant->getBitWidth() == fullWidth;
  }
  auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(value);
  if (call == nullptr) {
    return false;
  }
  std::optional<NativeRegisterValueInsertInfo> insert =
      parseNativeRegisterValueInsert(*call);
  if (!insert || insert->FullWidth != fullWidth) {
    return false;
  }
  if (!collectInsertedExtractPieces(insert->Base, fullWidth, valueMap, source,
                                    covered)) {
    return false;
  }

  llvm::Value *piece = resolveValueMap(insert->Value, valueMap);
  auto *extractCall = llvm::dyn_cast_or_null<llvm::CallBase>(piece);
  std::optional<NativeRegisterValueExtractInfo> extract =
      extractCall == nullptr ? std::nullopt
                             : parseNativeRegisterValueExtract(*extractCall);
  if (!extract || extract->FullWidth != fullWidth ||
      extract->ReadWidth != insert->WriteWidth ||
      extract->BitOffset != insert->BitOffset) {
    return false;
  }
  llvm::Value *candidate = resolveValueMap(extract->FullValue, valueMap);
  if (candidate == nullptr || integerBitWidthOf(candidate) != fullWidth) {
    return false;
  }
  if (source == nullptr) {
    source = candidate;
  } else if (source != candidate) {
    return false;
  }
  covered |= llvm::APInt::getLowBitsSet(fullWidth, insert->WriteWidth)
                 .shl(insert->BitOffset);
  return true;
}

llvm::Value *
foldFullInsertOfExtracts(llvm::Value *value,
                         std::map<llvm::Value *, llvm::Value *> &valueMap) {
  value = resolveValueMap(value, valueMap);
  auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(value);
  std::optional<NativeRegisterValueInsertInfo> insert =
      call == nullptr ? std::nullopt : parseNativeRegisterValueInsert(*call);
  if (!insert || insert->FullWidth == 0) {
    return value;
  }
  llvm::Value *source = nullptr;
  llvm::APInt covered(insert->FullWidth, 0);
  if (!collectInsertedExtractPieces(value, insert->FullWidth, valueMap, source,
                                    covered) ||
      source == nullptr || !covered.isAllOnes()) {
    return value;
  }
  valueMap[value] = source;
  return source;
}

void redirectAddressTakenUses(llvm::Function &oldFunction,
                              llvm::Function &newFunction) {
  std::vector<std::pair<llvm::ConstantExpr *, llvm::Constant *>> replacements;
  for (llvm::User *user : oldFunction.users()) {
    auto *constant = llvm::dyn_cast<llvm::ConstantExpr>(user);
    if (constant == nullptr ||
        constant->getOpcode() != llvm::Instruction::PtrToInt) {
      continue;
    }
    replacements.emplace_back(
        constant,
        llvm::ConstantExpr::getPtrToInt(&newFunction, constant->getType()));
  }
  for (auto &[oldConstant, newConstant] : replacements) {
    if (oldConstant == newConstant) {
      continue;
    }
    // RAUW only moves the ConstantExpr's users.  The old expression itself
    // still lists oldFunction as an operand, so destroy it after it becomes
    // dead; otherwise oldFunction keeps a use and cannot be erased.
    oldConstant->replaceAllUsesWith(newConstant);
    if (oldConstant->use_empty()) {
      oldConstant->destroyConstant();
    }
  }
}

llvm::Value *callerEntryArgumentForSlot(
    llvm::Function &caller, const SignatureShape &callerShape,
    const NativeSignatureSlot &slot, llvm::IRBuilder<> &builder) {
  if (slot.Unit == nullptr || slot.Unit->Global == nullptr) {
    return nullptr;
  }
  for (unsigned index = 0;
       index < callerShape.Params.size() && index < caller.arg_size();
       ++index) {
    const NativeSignatureSlot &callerSlot = callerShape.Params[index];
    if (callerSlot.Unit == nullptr ||
        callerSlot.Unit->Global != slot.Unit->Global) {
      continue;
    }
    if (callerSlot.OffsetBits != 0 || slot.OffsetBits != 0) {
      continue;
    }
    llvm::Value *argument = caller.getArg(index);
    llvm::Type *targetType = slotType(slot);
    if (argument->getType() == targetType) {
      return argument;
    }
    if (argument->getType()->isIntegerTy() && targetType->isIntegerTy()) {
      return builder.CreateZExtOrTrunc(argument, targetType,
                                       slot.Unit->Name + ".entry_arg");
    }
    if (argument->getType()->getScalarSizeInBits() ==
            targetType->getScalarSizeInBits() &&
        ((argument->getType()->isFloatingPointTy() &&
          targetType->isIntegerTy()) ||
         (argument->getType()->isIntegerTy() &&
          targetType->isFloatingPointTy()))) {
      return builder.CreateBitCast(argument, targetType,
                                   slot.Unit->Name + ".entry_arg_bits");
    }
    return nullptr;
  }
  return nullptr;
}

void rewriteSignatureShapes(
    llvm::Module &module, SignatureRewriteState &state,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi, NativeRegisterEliminationSummary &summary) {
  std::map<llvm::Function *, llvm::Function *> replacements;
  std::map<llvm::Function *, const SignatureShape *> rewrittenFunctionShapes;
  std::vector<std::pair<llvm::Function *, SignatureShape>> replacementShapes;
  // state.Shapes is keyed by Function*, so iterating it directly makes the
  // rewrite order (and therefore every IR value name it creates) depend on
  // heap layout.  Rewrite in module order instead: that order comes from
  // lifting and is stable across builds.
  std::vector<llvm::Function *> orderedFunctions;
  orderedFunctions.reserve(state.Shapes.size());
  for (llvm::Function &function : module) {
    if (state.Shapes.count(&function) != 0) {
      orderedFunctions.push_back(&function);
    }
  }
  for (llvm::Function *function : orderedFunctions) {
    SignatureShape &shape = state.Shapes.find(function)->second;
    llvm::FunctionType *newType =
        functionTypeForShape(module.getContext(), shape);
    if (function->getFunctionType() == newType) {
      replacements[function] = function;
      rewrittenFunctionShapes[function] = &shape;
      continue;
    }
    llvm::Function *newFunction =
        createReplacementFunction(*function, *newType);
    replacements[function] = newFunction;
    rewrittenFunctionShapes[newFunction] = &shape;
    replacementShapes.emplace_back(newFunction, shape);
    if (!function->isDeclaration()) {
      rewriteInternalFunctionBody(*function, *newFunction, shape, state, units,
                                  abi);
    }
    ++summary.FunctionsRewritten;
  }

  std::vector<llvm::CallBase *> callsToRewrite;
  for (llvm::Function &function : module) {
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call != nullptr && shapeForCall(state, *call) != nullptr &&
          (state.CallArgs.count(call) != 0 ||
           state.ReturnHelpers.count(call) != 0 ||
           state.RangeReturnHelpers.count(call) != 0)) {
        callsToRewrite.push_back(call);
      }
    }
  }

  std::map<llvm::Value *, llvm::Value *> valueMap;
  valueMap.insert(state.ValueMap.begin(), state.ValueMap.end());
  std::vector<llvm::CallBase *> oldCallsToErase;
  std::vector<llvm::CallInst *> helpersToErase;
  auto remapValue = [&](llvm::Value *value) -> llvm::Value * {
    value = resolveValueMap(value, valueMap);
    return foldFullInsertOfExtracts(value, valueMap);
  };

  for (llvm::CallBase *oldCall : callsToRewrite) {
    if (oldCall->getParent() == nullptr) {
      continue;
    }
    std::vector<CallArgStoreBinding> &bindings = state.CallArgs[oldCall];
    llvm::Function *oldCallee = oldCall->getCalledFunction();
    const SignatureShape *shape = shapeForCall(state, *oldCall);
    if (shape == nullptr) {
      continue;
    }
    llvm::Value *newCallee = nullptr;
    llvm::FunctionType *newCallType = nullptr;
    if (oldCallee != nullptr) {
      newCallee = replacements[oldCallee];
      if (newCallee == nullptr) {
        continue;
      }
      newCallType = llvm::cast<llvm::Function>(newCallee)->getFunctionType();
    } else {
      newCallee = oldCall->getCalledOperand();
      newCallType = functionTypeForShape(module.getContext(), *shape);
    }
    // A replacement function takes the old callee's name before call
    // arguments are rebuilt.  Warnings made from oldCall would therefore lose
    // the direct callee name.  Keep the final callee name explicitly until the
    // new call exists.
    const std::string rewrittenCalleeName =
        oldCallee == nullptr
            ? std::string("<indirect>")
            : llvm::cast<llvm::Function>(newCallee)->getName().str();
    auto finalCallSlotWarning = [&](const NativeSignatureSlot &slot,
                                    llvm::StringRef kind,
                                    llvm::StringRef reason) {
      NativeRegisterEliminationWarning warning =
          callSlotWarning(*oldCall, slot, kind, reason);
      warning.CalleeName = rewrittenCalleeName;
      return warning;
    };
    auto finalCallRegisterWarning = [&](llvm::StringRef registerName,
                                        llvm::StringRef kind,
                                        llvm::StringRef reason) {
      NativeRegisterEliminationWarning warning =
          callRegisterWarning(*oldCall, registerName, kind, reason);
      warning.CalleeName = rewrittenCalleeName;
      return warning;
    };
    // Tail calls reuse the caller's incoming stack slots.  The wrapper may have
    // gained the corresponding stack parameter through propagation but have no
    // explicit store binding; pass the new caller argument directly to the
    // callee's matching stack slot.
    auto tailCallStackValue = [&](const NativeSignatureSlot &slot)
        -> llvm::Value * {
      if (!isStackSignatureSlot(slot)) {
        return nullptr;
      }
      auto *oldCallInst = llvm::dyn_cast<llvm::CallInst>(oldCall);
      if (oldCallInst == nullptr || !oldCallInst->isTailCall()) {
        return nullptr;
      }
      llvm::Function *newCaller = oldCall->getFunction();
      if (newCaller == nullptr) {
        return nullptr;
      }
      auto callerShapeIt = rewrittenFunctionShapes.find(newCaller);
      if (callerShapeIt == rewrittenFunctionShapes.end()) {
        return nullptr;
      }
      const std::vector<NativeSignatureSlot> &callerParams =
          callerShapeIt->second->Params;
      for (unsigned index = 0; index < callerParams.size(); ++index) {
        const NativeSignatureSlot &callerSlot = callerParams[index];
        if (!isStackSignatureSlot(callerSlot) ||
            !sameStackSignatureSlot(callerSlot, slot)) {
          continue;
        }
        if (index < newCaller->arg_size()) {
          return newCaller->getArg(index);
        }
      }
      return nullptr;
    };
    std::vector<llvm::Value *> args;
    args.reserve(bindings.size());
    llvm::IRBuilder<> oldCallBuilder(oldCall);
    for (unsigned index = 0; index < shape->Params.size(); ++index) {
      const NativeSignatureSlot &slot = shape->Params[index];
      const CallArgStoreBinding *binding = bindingForIndex(bindings, index);
      llvm::Value *value = binding == nullptr ? nullptr : binding->Value;
      if (value == nullptr) {
        value = tailCallStackValue(slot);
      }
      if (value == nullptr) {
        // 绑定收集停在第一个读不出值的槽上，后面的槽也可能没绑定。构建期
        // 已经逐槽记下"调用前那一刻"的 range 值，这里按 index 取回来，比
        // 直接造 notdec.unknown 更接近真实来源（日志 §15）。
        auto callIt = state.CallArgRangeValues.find(oldCall);
        if (callIt != state.CallArgRangeValues.end()) {
          auto valueIt = callIt->second.find(index);
          if (valueIt != callIt->second.end() &&
              valueIt->second->getType() == slotType(slot)) {
            value = valueIt->second;
          }
        }
      }
      auto entryArgumentForSlot = [&]() -> llvm::Value * {
        // §15 ②：这个槽就是调用者自己的入参寄存器时，直接传调用者的形参。
        llvm::Function *newCaller = oldCall->getFunction();
        if (newCaller == nullptr) {
          return nullptr;
        }
        auto callerShapeIt = rewrittenFunctionShapes.find(newCaller);
        if (callerShapeIt == rewrittenFunctionShapes.end()) {
          return nullptr;
        }
        return callerEntryArgumentForSlot(*newCaller, *callerShapeIt->second,
                                          slot, oldCallBuilder);
      };
      if (value == nullptr) {
        value = entryArgumentForSlot();
      }
      bool poisonedArgument = false;
      // 依赖 poison/undef 的值不能当实参传给 callee：调用点会变成 UB，而
      // poison 又带不了 !notdec.unknown.source 归因。先回落调用者入口形参
      // （thunk/递归调用转发自己形参的场景），回落不到再换成带归因的 unknown。
      if (value != nullptr && valueDependsOnPoison(value)) {
        if (llvm::Value *entryArgument = entryArgumentForSlot()) {
          value = entryArgument;
        } else {
          value = nullptr;
          poisonedArgument = true;
        }
      }
      // 类型对不上时也先试调用者入口形参：thunk 转发自己形参时，range SSA
      // 给出的常是窄类型值，而形参本身就是要传的值（hexx64 FUN_5d030）。
      if (value != nullptr && value->getType() != slotType(slot)) {
        if (llvm::Value *entryArgument = entryArgumentForSlot()) {
          value = entryArgument;
        }
      }
      if (value != nullptr) {
        value = remapValue(value);
        value = localizeCallArgument(*oldCall->getFunction(), *oldCall, value);
      }
      llvm::StringRef reason;
      if (value == nullptr) {
        reason = poisonedArgument ? "call_arg_uses_poison_value"
                                  : "call_arg_rewrite_missing_binding";
      } else if (value->getType() != slotType(slot)) {
        reason = "call_arg_rewrite_type_mismatch";
      } else if (valueMayDependOnUnknownPlaceholder(value)) {
        state.Warnings.push_back(finalCallSlotWarning(
            slot, "call_arg", "call_arg_uses_unknown_value"));
      } else if (mayDependOnSummaryClobberValue(value)) {
        state.Warnings.push_back(finalCallSlotWarning(
            slot, "call_arg", "call_arg_uses_clobber_value"));
      }
      if (!reason.empty()) {
        state.Warnings.push_back(
            finalCallSlotWarning(slot, "call_arg", reason));
        std::string regName = slotRegisterName(slot);
        value = unknownValueAt(oldCallBuilder, slotType(slot),
                               regName + ".arg_unknown");
        attachUnknownValueMetadata(value, reason,
                                   oldCall->getFunction()->getName(),
                                   rewrittenCalleeName, regName, "call_arg");
      }
      args.push_back(value);
    }
    if (shape->VarArg) {
      for (const CallArgStoreBinding &binding : bindings) {
        if (binding.Index < shape->Params.size()) {
          continue;
        }
        llvm::Value *value = binding.Value;
        if (value == nullptr) {
          continue;
        }
        value = remapValue(value);
        value = localizeCallArgument(*oldCall->getFunction(), *oldCall, value);
        if (mayDependOnSummaryClobberValue(value)) {
          state.Warnings.push_back(finalCallRegisterWarning(
              binding.Unit == nullptr ? "" : binding.Unit->Name, "call_arg",
              "vararg_arg_uses_clobber_value"));
        }
        bool unknownValue = valueMayDependOnUnknownPlaceholder(value);
        // poison/undef 不能进变参槽：调用点会变成 UB，poison 也带不了
        // !notdec.unknown.source 归因。换成带归因的 unknown，并且优先于下面的
        // vararg_unknown 路径（否则又会被降级回 poison）。
        if (valueDependsOnPoison(value)) {
          state.Warnings.push_back(finalCallRegisterWarning(
              binding.Unit == nullptr ? "" : binding.Unit->Name, "call_arg",
              "vararg_arg_uses_poison_value"));
          llvm::Value *poisonReplacement = unknownValueAt(
              oldCallBuilder, value->getType(),
              (binding.Unit == nullptr ? llvm::Twine("vararg")
                                       : llvm::Twine(binding.Unit->Name)) +
                  ".arg_unknown");
          attachUnknownValueMetadata(
              poisonReplacement, "vararg_arg_uses_poison_value",
              oldCall->getFunction()->getName(), rewrittenCalleeName,
              binding.Unit == nullptr ? "" : binding.Unit->Name, "call_arg");
          value = poisonReplacement;
          unknownValue = true;
        } else if (isLikelyX86_64SysVAbi(abi) &&
                   shouldPoisonUnprovenVarArg(binding, value,
                                              module.getDataLayout(),
                                              oldCall)) {
          llvm::StringRef reason = unknownValue
                                       ? "vararg_arg_uses_unknown_value"
                                       : "vararg_arg_unproven_zero";
          state.Warnings.push_back(finalCallRegisterWarning(
              binding.Unit == nullptr ? "" : binding.Unit->Name, "call_arg",
              reason));
          llvm::Function *unknown =
              getOrInsertVarArgUnknownHelper(module, value->getType());
          value = oldCallBuilder.CreateCall(unknown);
          attachUnknownValueMetadata(
              value, reason, oldCall->getFunction()->getName(),
              rewrittenCalleeName,
              binding.Unit == nullptr ? "" : binding.Unit->Name, "call_arg");
        }
        if (binding.ValueType != nullptr &&
            value->getType() != binding.ValueType) {
          state.Warnings.push_back(finalCallRegisterWarning(
              binding.Unit == nullptr ? "" : binding.Unit->Name, "call_arg",
              "vararg_arg_type_mismatch"));
          continue;
        }
        args.push_back(value);
      }
    }
    llvm::CallInst *newCall =
        rewriteCallInst(*oldCall, *newCallee, *newCallType, args);
    state.RewrittenCalls.insert(newCall);
    if (!oldCall->use_empty()) {
      llvm::IRBuilder<> builder(newCall->getNextNode());
      llvm::Value *replacement =
          replacementForOldCallUses(builder, *oldCall, *newCall, *shape);
      if (replacement != nullptr) {
        oldCall->replaceAllUsesWith(replacement);
        newCall->takeName(oldCall);
      }
    }
    valueMap[oldCall] = newCall;
    oldCallsToErase.push_back(oldCall);
    auto helpersIt = state.ReturnHelpers.find(oldCall);
    if (helpersIt != state.ReturnHelpers.end() && !shape->Returns.empty()) {
      llvm::IRBuilder<> builder(newCall->getNextNode());
      for (auto &[name, helper] : helpersIt->second) {
        if (helper->getParent() == nullptr) {
          continue;
        }
        llvm::Value *value =
            extractReturnRegister(builder, *shape, *newCall, name);
        if (value == nullptr || value->getType() != helper->getType()) {
          // 解析不出返回值、helper 又已经没人用：它是重写过程中被摘掉的死
          // helper，不要为它再物化一个 unknown 占位（那会留下没人用的
          // notdec.unknown 调用），直接收集删除。
          if (helper->use_empty()) {
            helpersToErase.push_back(helper);
            continue;
          }
          llvm::StringRef reason = value == nullptr
                                       ? "return_helper_rewrite_missing_value"
                                       : "return_helper_rewrite_type_mismatch";
          state.Warnings.push_back(
              callRegisterWarning(*newCall, name, "return", reason));
          value = unknownValueAt(builder, helper->getType(),
                                 name + ".return_unknown");
          attachUnknownValueMetadata(
              value, reason, oldCall->getFunction()->getName(),
              calleeNameForWarning(*newCall), name, "return");
        }
        valueMap[helper] = value;
        helper->replaceAllUsesWith(value);
        if (helper->use_empty()) {
          helpersToErase.push_back(helper);
        }
      }
    }
    auto rangeHelpersIt = state.RangeReturnHelpers.find(oldCall);
    if (rangeHelpersIt != state.RangeReturnHelpers.end() &&
        !shape->Returns.empty()) {
      // RangeReturnHelpers are appended in the order the SSA walk first reads
      // the post-call registers.  That order is stable only if every walk is
      // stable; make it explicit so the extract/trunc sequence (and therefore
      // the IR text) does not depend on which use was visited first.
      std::stable_sort(
          rangeHelpersIt->second.begin(), rangeHelpersIt->second.end(),
          [](const RangeReturnHelper &lhs, const RangeReturnHelper &rhs) {
            const std::string lhsName =
                lhs.Range.Global == nullptr ? std::string()
                                            : lhs.Range.Global->getName().str();
            const std::string rhsName =
                rhs.Range.Global == nullptr ? std::string()
                                            : rhs.Range.Global->getName().str();
            if (lhsName != rhsName) {
              return lhsName < rhsName;
            }
            if (lhs.Range.BitOffset != rhs.Range.BitOffset) {
              return lhs.Range.BitOffset < rhs.Range.BitOffset;
            }
            return lhs.Range.BitWidth < rhs.Range.BitWidth;
          });
      llvm::IRBuilder<> builder(newCall->getNextNode());
      for (RangeReturnHelper &rangeHelper : rangeHelpersIt->second) {
        llvm::CallInst *helper = rangeHelper.Helper;
        if (helper == nullptr || helper->getParent() == nullptr) {
          continue;
        }
        llvm::Value *value =
            extractReturnRange(builder, *shape, *newCall, rangeHelper.Range);
        if (value == nullptr || value->getType() != helper->getType()) {
          // 同 register helper：死 helper 不造 unknown，直接删。
          if (helper->use_empty()) {
            helpersToErase.push_back(helper);
            continue;
          }
          // 第一遍的证据集是"调用后被读的 ABI 输出寄存器"（含 ST0/ZMM0
          // 这类只被 clobber 的），返回槽由签名决定。callee 的返回槽里
          // 没有这个寄存器时，解析失败是预期行为，静默转成 unknown；
          // 只有确实在返回槽里却解析不出来才值得告警。
          // 返回槽判定要和 extractReturnRange 的覆盖条件一致：只有当请求
          // 的 range 完全落在某个返回槽的位段内才算"确实在返回槽里"。
          // ZMM0[64:64]/[128:384] 这类上层 lane（keep-high 部分写）不在
          // double 返回槽覆盖范围内，按 clobber 静默转 unknown。
          bool inReturnSlot = false;
          for (const NativeSignatureSlot &slot : shape->Returns) {
            if (slot.Unit == nullptr ||
                slot.Unit->Global != rangeHelper.Range.Global) {
              continue;
            }
            uint64_t slotEnd = slot.OffsetBits + slot.SizeBits;
            uint64_t rangeEnd =
                rangeHelper.Range.BitOffset + rangeHelper.Range.BitWidth;
            if (rangeHelper.Range.BitOffset >= slot.OffsetBits &&
                rangeEnd <= slotEnd) {
              inReturnSlot = true;
              break;
            }
          }
          llvm::StringRef reason;
          if (inReturnSlot) {
            reason = value == nullptr
                         ? "range_return_helper_rewrite_missing_value"
                         : "range_return_helper_rewrite_type_mismatch";
            state.Warnings.push_back(
                callRangeWarning(*newCall, rangeHelper.Range, "return",
                                 reason));
          } else {
            reason = "range_return_helper_clobbered_register";
          }
          std::string regName = registerNameForRange(rangeHelper.Range);
          if (reason == "range_return_helper_clobbered_register") {
            // 这个寄存器在该调用点不是返回值而是 clobber：用与调用点绑定的
            // clobber 占位（和同一调用点其它 clobber 寄存器一致），而不是
            // notdec.unknown。两者都是 opaque 占位，但 clobber 占位说明"值来自
            // callee 边界"，unknown 只留给真正的建模缺口。
            llvm::Module *module = oldCall->getModule();
            llvm::FunctionCallee clobberHelper = module->getOrInsertFunction(
                "notdec.register.summary_clobber." +
                    typeSuffix(*helper->getType()),
                llvm::FunctionType::get(helper->getType(), {}, false));
            value = builder.CreateCall(clobberHelper, {}, "notdec.range_clobber");
            llvm::LLVMContext &context = module->getContext();
            std::vector<llvm::Metadata *> fields;
            fields.push_back(llvm::MDString::get(context, "name=" + regName));
            fields.push_back(llvm::MDString::get(context, "kind=clobber"));
            uint64_t callIndex = 0;
            for (const llvm::Instruction &inst : *newCall->getParent()) {
              if (&inst == newCall) {
                break;
              }
              ++callIndex;
            }
            fields.push_back(llvm::MDString::get(
                context, "call_index=" + std::to_string(callIndex)));
            if (auto *valueInst = llvm::dyn_cast<llvm::Instruction>(value)) {
              valueInst->setMetadata("notdec.register.summary_ssa.call_value",
                                     llvm::MDNode::get(context, fields));
            }
          } else {
            value = unknownValueAt(builder, helper->getType(),
                                   "range_return_unknown");
            attachUnknownValueMetadata(
                value, reason, oldCall->getFunction()->getName(),
                calleeNameForWarning(*newCall), regName, "return");
          }
        }
        valueMap[helper] = value;
        helper->replaceAllUsesWith(value);
        if (helper->use_empty()) {
          helpersToErase.push_back(helper);
        }
      }
    }
    ++summary.CallsRewritten;
  }

  for (llvm::CallInst *helper : helpersToErase) {
    if (helper->getParent() != nullptr && helper->use_empty()) {
      helper->eraseFromParent();
    }
  }
  for (llvm::CallBase *call : oldCallsToErase) {
    if (call->getParent() != nullptr && call->use_empty()) {
      call->eraseFromParent();
    }
  }

  for (llvm::StoreInst *store : state.StoresToErase) {
    if (store->getParent() != nullptr && store->use_empty()) {
      store->eraseFromParent();
      ++summary.DeadStoresRemoved;
    }
  }

  for (auto &[oldFunction, newFunction] : replacements) {
    if (oldFunction == newFunction) {
      continue;
    }
    redirectAddressTakenUses(*oldFunction, *newFunction);
    if (oldFunction->use_empty() && oldFunction->empty()) {
      oldFunction->eraseFromParent();
    }
  }
  for (auto &[function, shape] : replacementShapes) {
    state.Shapes[function] = std::move(shape);
  }
}

std::string metadataField(const llvm::MDNode &node, llvm::StringRef key) {
  std::string prefix = (key + "=").str();
  for (const llvm::MDOperand &operand : node.operands()) {
    auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
    if (text == nullptr) {
      continue;
    }
    llvm::StringRef value = text->getString();
    if (value.consume_front(prefix)) {
      return value.str();
    }
  }
  return "";
}

uint64_t metadataUInt64Field(const llvm::MDNode &node, llvm::StringRef key) {
  std::string valueText = metadataField(node, key);
  uint64_t value = 0;
  if (!valueText.empty()) {
    (void)llvm::StringRef(valueText).getAsInteger(10, value);
  }
  return value;
}

std::string warningReasonForHelper(const llvm::CallInst &helper,
                                   llvm::StringRef kind,
                                   llvm::StringRef regName,
                                   const llvm::Function *callee) {
  if (kind == "return") {
    if (callee != nullptr && callee->isDeclaration()) {
      if (regName == "RDX") {
        return "non_primary_external_return_register";
      }
      return "missing_external_return_prototype";
    }
    return "unresolved_return_register";
  }
  if (kind == "clobber") {
    return "remaining_summary_clobber_value";
  }
  return "unresolved_register_helper";
}

std::vector<NativeRegisterEliminationWarning>
collectRemainingCallValueWarnings(const llvm::Module &module) {
  std::vector<NativeRegisterEliminationWarning> warnings;
  for (const llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (const llvm::Instruction &inst : llvm::instructions(function)) {
      auto *helper = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (helper == nullptr) {
        continue;
      }
      llvm::Function *helperCallee = helper->getCalledFunction();
      if (helperCallee == nullptr ||
          !helperCallee->getName().starts_with("notdec.register.summary_")) {
        continue;
      }
      llvm::MDNode *metadata =
          helper->getMetadata("notdec.register.summary_ssa.call_value");
      std::string regName =
          metadata == nullptr ? "" : metadataField(*metadata, "name");
      std::string kind =
          metadata == nullptr ? "" : metadataField(*metadata, "kind");

      const llvm::CallBase *sourceCall = nullptr;
      const llvm::Instruction *previous = helper->getPrevNode();
      while (previous != nullptr) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(previous);
        if (call != nullptr && isAnalyzableCall(*call)) {
          sourceCall = call;
          break;
        }
        previous = previous->getPrevNode();
      }
      const llvm::Function *callee =
          sourceCall == nullptr ? nullptr : sourceCall->getCalledFunction();

      NativeRegisterEliminationWarning warning;
      warning.FunctionName = function.getName().str();
      warning.CalleeName =
          callee == nullptr ? "<indirect>" : callee->getName().str();
      warning.RegisterName = regName;
      if (metadata != nullptr) {
        warning.RangeBitOffset = metadataUInt64Field(*metadata, "bit_offset");
        warning.RangeBitWidth = metadataUInt64Field(*metadata, "bit_width");
      }
      warning.Kind = kind;
      warning.Reason = warningReasonForHelper(*helper, kind, regName, callee);
      warning.Uses = helper->getNumUses();
      warnings.push_back(std::move(warning));
    }
  }
  return warnings;
}

} // namespace notdec::bin2llvm
