#include "RegisterEliminationInternal.h"

namespace notdec::bin2llvm {

FunctionBuilder::FunctionBuilder( llvm::Function &function, const std::map<llvm::GlobalVariable *, RegisterUnit> &units, const std::map<llvm::Function *, FunctionSummaryFacts> &summaryFacts, const AbiFacts &abiFacts, const NativeRegisterEliminationOptions &options, NativeRegisterEliminationFunctionSummary &summary, SignatureRewriteState &signatureState) : Function(function), Units(units), SummaryFacts(summaryFacts), Abi(abiFacts), Options(options), Summary(summary), SignatureState(signatureState) {}

void FunctionBuilder::run() {
    Summary.FunctionName = Function.getName().str();
    collectAccesses();
    planRegisterRanges();
    if (Options.EnableRewrite) {
      rewritePartialReads();
      foldDuplicatePartialReadXors();
      rewriteLoads();
      addIndirectCallsiteShapes(SignatureState, Units, Abi);
      collectSignatureCallArgs(/*stackSlotsOnly=*/false);
      // 第一遍只读退出点前 ABI 输出槽位生成"调用后被读"证据
      // （RangeReturnHelpers），供 addDemandedExternalReturns 给未知外部
      // 补返回槽（sqrtl 的 ST0 就靠它）；不记录返回绑定、不改 ret。
      // 但返回槽值本身仍是 partial-demand 的 observer，会记录到
      // ReturnObservationValues，不能被延迟绑定一起丢掉。
      // 真正的返回绑定在重写后的清理遍收集：第一遍时 callee 返回槽可能
      // 还没补全，且链上占位跨函数重写存活不了。
      collectFunctionReturnValues(/*recordBinding=*/false);
      rewritePartialWrites();
      finalizePendingPhis();
      // Stack addresses can depend on loop-carried RSP/RBP range phis.  Bind
      // stack arguments only after those phis have reached their final value.
      collectSignatureCallArgs(/*stackSlotsOnly=*/true);
      if (Options.EnableResidueRemoval) {
        removeDeadStoresByLiveness();
        // Keep replaced reads/xors/loads physically alive until every range
        // read and call binding is done.  They stay as Replacement keys while
        // the builder is active; deleting them earlier can leave stale keys if
        // LLVM later reuses the same address for a new helper.
        removeDeadPartialReads();
        removeDeadFoldedZeroXors();
        removeDeadReplacedLoads();
        removeDeadEntryReads();
      }
      eraseDeadPhis();
    }
    if (Options.AttachMetadata) {
      attachMetadata();
    }
  }

void FunctionBuilder::removeDeadStoresAfterSignatureRewrite(bool collectReturns ) {
    Summary.FunctionName = Function.getName().str();
    PostSignatureCleanup = true;
    collectAccesses();
    planRegisterRanges();
    rewritePartialReads();
    foldDuplicatePartialReadXors();
    rewriteLoads();
    if (collectReturns) {
      collectFunctionReturnValues();
    }
    rewritePartialWrites();
    // This cleanup run still creates new range helper calls while rewriting
    // loads/writes, so physical deletion stays after liveness cleanup.
    removeDeadStoresByLiveness();
    removeDeadPartialReads();
    removeDeadFoldedZeroXors();
    removeDeadReplacedLoads();
    removeDeadEntryReads();
  }

unsigned FunctionBuilder::valueBitWidth(llvm::Value *value) {
    if (value == nullptr) {
      return 0;
    }
    llvm::Type *type = value->getType();
    if (type == nullptr || !type->isSized()) {
      return 0;
    }
    if (auto *integerType = llvm::dyn_cast<llvm::IntegerType>(type)) {
      return integerType->getBitWidth();
    }
    return type->getScalarSizeInBits();
  }

llvm::APInt FunctionBuilder::fullMaskFor(llvm::Value *value) {
    unsigned bitWidth = valueBitWidth(value);
    if (bitWidth == 0) {
      return llvm::APInt();
    }
    return llvm::APInt::getAllOnes(bitWidth);
  }

bool FunctionBuilder::callPreservesRegisterByAbi(const RegisterUnit &unit) const {
    return isSegmentBaseUnit(unit.Name) || Abi.Unaffected.count(unit.Name) != 0;
  }

llvm::APInt FunctionBuilder::maskForLowBits(unsigned bitWidth, unsigned lowBits) {
    if (bitWidth == 0 || lowBits == 0) {
      return llvm::APInt(bitWidth, 0);
    }
    unsigned count = std::min(bitWidth, lowBits);
    return llvm::APInt::getLowBitsSet(bitWidth, count);
  }

llvm::APInt FunctionBuilder::shiftedMask(const llvm::APInt &mask, unsigned shift,
                                 unsigned width) {
    if (width == 0) {
      return llvm::APInt();
    }
    llvm::APInt wide = PartialDemandState::trimmedMask(mask, width);
    if (wide.isZero()) {
      return wide;
    }
    if (shift >= width) {
      return llvm::APInt(width, 0);
    }
    wide <<= shift;
    return PartialDemandState::trimmedMask(wide, width);
  }

llvm::APInt FunctionBuilder::lshrSourceDemand(const llvm::APInt &resultDemand,
                                      unsigned shift, unsigned sourceWidth) {
    if (sourceWidth == 0 || shift >= sourceWidth) {
      return llvm::APInt(sourceWidth, 0);
    }
    return shiftedMask(resultDemand, shift, sourceWidth);
  }

llvm::APInt FunctionBuilder::shlSourceDemand(const llvm::APInt &resultDemand,
                                     unsigned shift, unsigned sourceWidth) {
    if (sourceWidth == 0 || shift >= sourceWidth) {
      return llvm::APInt(sourceWidth, 0);
    }
    return PartialDemandState::trimmedMask(resultDemand.lshr(shift),
                                           sourceWidth);
  }

llvm::APInt FunctionBuilder::partialWriteMask(unsigned fullWidth, unsigned writeWidth,
                                      uint64_t bitOffset) {
    if (fullWidth == 0 || writeWidth == 0 || bitOffset >= fullWidth ||
        bitOffset + writeWidth > fullWidth) {
      return llvm::APInt(fullWidth, 0);
    }
    return llvm::APInt::getLowBitsSet(fullWidth, writeWidth).shl(bitOffset);
  }

bool FunctionBuilder::isDisjointOr(const llvm::Instruction &inst) {
    auto *possiblyDisjoint = llvm::dyn_cast<llvm::PossiblyDisjointInst>(&inst);
    return possiblyDisjoint != nullptr && possiblyDisjoint->isDisjoint();
  }

llvm::APInt
  FunctionBuilder::demandedBits(llvm::Value *value,
               const std::map<llvm::Value *, llvm::APInt> &demands) const {
    unsigned width = valueBitWidth(value);
    if (width == 0) {
      return llvm::APInt();
    }
    auto it = demands.find(value);
    if (it == demands.end()) {
      return llvm::APInt(width, 0);
    }
    return PartialDemandState::trimmedMask(it->second, width);
  }

llvm::Value *FunctionBuilder::undemandedStoreOperandPoison(llvm::Type *type) const {
    if (type == nullptr || !type->isIntegerTy()) {
      return nullptr;
    }
    return llvm::PoisonValue::get(type);
  }

llvm::Value *
  FunctionBuilder::replacePartialRegisterValue(const RegisterUnit &unit, llvm::Value *oldValue,
                              llvm::Value *partialValue, unsigned writeWidth,
                              uint64_t bitOffset, llvm::Instruction *before) {
    if (oldValue == nullptr || partialValue == nullptr || before == nullptr) {
      return nullptr;
    }
    auto *baseType =
        llvm::dyn_cast<llvm::IntegerType>(unit.Global->getValueType());
    if (baseType == nullptr || oldValue->getType() != baseType) {
      return nullptr;
    }
    unsigned fullWidth = baseType->getBitWidth();
    llvm::APInt writeMask = partialWriteMask(fullWidth, writeWidth, bitOffset);
    if (writeMask.isZero()) {
      return nullptr;
    }

    (void)writeMask;
    return insertBitsIntoInteger(oldValue, partialValue, writeWidth, bitOffset,
                                 before, unit.Name + ".partial_write");
  }

llvm::Value *FunctionBuilder::insertBitsIntoInteger(llvm::Value *base, llvm::Value *value,
                                     unsigned writeWidth, uint64_t bitOffset,
                                     llvm::Instruction *before,
                                     llvm::Twine name) {
    if (base == nullptr || value == nullptr || before == nullptr ||
        writeWidth == 0) {
      return nullptr;
    }
    auto *baseType = llvm::dyn_cast<llvm::IntegerType>(base->getType());
    if (baseType == nullptr || value->getType() == nullptr ||
        !value->getType()->isIntegerTy() ||
        value->getType()->getIntegerBitWidth() != writeWidth ||
        bitOffset + writeWidth > baseType->getBitWidth()) {
      return nullptr;
    }
    if (bitOffset == 0 && writeWidth == baseType->getBitWidth() &&
        value->getType() == baseType) {
      return value;
    }

    llvm::IRBuilder<> builder(before);
    llvm::Function *callee = getOrInsertNativeRegisterValueInsert(
        *Function.getParent(), baseType, value->getType(),
        baseType->getBitWidth(), writeWidth);
    llvm::Value *offset = llvm::ConstantInt::get(
        llvm::Type::getInt64Ty(Function.getContext()), bitOffset);
    return builder.CreateCall(callee, {base, value, offset}, name);
  }

llvm::Value *FunctionBuilder::extractBitsFromIntegerValue(llvm::Value *value,
                                           unsigned readWidth,
                                           uint64_t bitOffset,
                                           llvm::Instruction *before,
                                           llvm::Twine name) {
    if (value == nullptr || before == nullptr) {
      return nullptr;
    }
    auto *sourceType = llvm::dyn_cast<llvm::IntegerType>(value->getType());
    if (sourceType == nullptr || readWidth == 0 ||
        bitOffset + readWidth > sourceType->getBitWidth()) {
      return nullptr;
    }
    auto *resultType = llvm::IntegerType::get(Function.getContext(), readWidth);
    if (bitOffset == 0 && resultType == sourceType) {
      return value;
    }

    llvm::IRBuilder<> builder(before);
    llvm::Function *callee = getOrInsertNativeRegisterValueExtract(
        *Function.getParent(), sourceType, resultType,
        sourceType->getBitWidth(), readWidth);
    llvm::Value *offset = llvm::ConstantInt::get(
        llvm::Type::getInt64Ty(Function.getContext()), bitOffset);
    return builder.CreateCall(callee, {value, offset}, name);
  }

llvm::Value *FunctionBuilder::extractPartialRegisterValue(const RegisterUnit &unit,
                                           llvm::Value *fullValue,
                                           unsigned readWidth,
                                           uint64_t bitOffset,
                                           llvm::Instruction *before) {
    if (fullValue == nullptr || before == nullptr) {
      return nullptr;
    }
    auto *baseType =
        llvm::dyn_cast<llvm::IntegerType>(unit.Global->getValueType());
    if (baseType == nullptr || fullValue->getType() != baseType ||
        readWidth == 0 || bitOffset + readWidth > baseType->getBitWidth()) {
      return nullptr;
    }
    return extractBitsFromIntegerValue(fullValue, readWidth, bitOffset, before,
                                       unit.Name + ".partial_read");
  }

llvm::Value *FunctionBuilder::extractBitsFromInteger(llvm::Value *value, unsigned readWidth,
                                      uint64_t bitOffset,
                                      llvm::Instruction *before,
                                      llvm::Twine name) {
    if (value == nullptr || before == nullptr) {
      return nullptr;
    }
    auto *sourceType = llvm::dyn_cast<llvm::IntegerType>(value->getType());
    if (sourceType == nullptr || readWidth == 0 ||
        bitOffset + readWidth > sourceType->getBitWidth()) {
      return nullptr;
    }
    return extractBitsFromIntegerValue(value, readWidth, bitOffset, before,
                                       name);
  }

void FunctionBuilder::eraseTriviallyDeadNonPhiTree(llvm::Instruction *root) {
    std::vector<llvm::Instruction *> worklist;
    worklist.push_back(root);
    while (!worklist.empty()) {
      llvm::Instruction *inst = worklist.back();
      worklist.pop_back();
      if (inst == nullptr || inst->getParent() == nullptr ||
          llvm::isa<llvm::PHINode>(inst) ||
          isRecordedFunctionReturnValue(inst) ||
          !llvm::isInstructionTriviallyDead(inst)) {
        continue;
      }

      std::vector<llvm::Instruction *> operands;
      for (llvm::Value *operand : inst->operand_values()) {
        if (auto *operandInst = llvm::dyn_cast<llvm::Instruction>(operand)) {
          operands.push_back(operandInst);
        }
      }
      inst->eraseFromParent();
      for (llvm::Instruction *operand : operands) {
        if (operand->use_empty() && !isRecordedFunctionReturnValue(operand)) {
          worklist.push_back(operand);
        }
      }
    }
  }

bool FunctionBuilder::rewriteUndemandedRegisterStoreOperands(
      llvm::Value *value, const std::map<llvm::Value *, llvm::APInt> &demands,
      llvm::SmallPtrSetImpl<llvm::Value *> &visiting) {
    auto *inst = llvm::dyn_cast_or_null<llvm::Instruction>(value);
    if (inst == nullptr || !visiting.insert(inst).second) {
      return false;
    }
    // 这棵子树在本次 rewritePartialWrites 里已经处理完：demands 不变、poison 替换
    // 幂等，重走一遍不会再有任何改动（见 UndemandedWalkClean 的注释）。
    if (UndemandedWalkClean.count(inst) != 0) {
      return false;
    }
    if (llvm::isa<llvm::CallBase>(inst) || inst->mayHaveSideEffects()) {
      UndemandedWalkClean.insert(inst);
      return false;
    }

    bool changed = false;
    unsigned operandIndex = 0;
    for (llvm::Use &operandUse : inst->operands()) {
      unsigned currentOperandIndex = operandIndex++;
      llvm::Value *operand = operandUse.get();
      if (llvm::isa<llvm::Constant>(operand)) {
        continue;
      }

      llvm::APInt demand = demandedBits(operand, demands);
      if (!demand.isZero()) {
        changed |=
            rewriteUndemandedRegisterStoreOperands(operand, demands, visiting);
        continue;
      }

      // This only rewrites register-store dataflow.  Replacing an undemanded
      // integer operand with poison keeps a missed observer visible instead of
      // silently manufacturing a valid-looking zero; normal DCE can still
      // remove stale register loads when the demand result is correct.
      llvm::Value *poison =
          undemandedStoreOperandPoison(operand->getType());
      if (poison == nullptr) {
        changed |=
            rewriteUndemandedRegisterStoreOperands(operand, demands, visiting);
        continue;
      }

      attachZeroDemandOperandMetadata(*inst, currentOperandIndex, *operand);
      operandUse.set(poison);
      if (auto *operandInst = llvm::dyn_cast<llvm::Instruction>(operand)) {
        eraseTriviallyDeadNonPhiTree(operandInst);
      }
      changed = true;
    }
    UndemandedWalkClean.insert(inst);
    return changed;
  }

std::map<llvm::Value *, llvm::APInt> FunctionBuilder::computePartialDemands() {
    std::map<llvm::Value *, llvm::APInt> demands;
    std::map<llvm::Value *, llvm::APInt> knownMasks;
    std::vector<llvm::Value *> worklist;
    std::function<llvm::APInt(llvm::Value *)> knownValueMask =
        [&](llvm::Value *value) -> llvm::APInt {
      if (value == nullptr) {
        return llvm::APInt();
      }
      auto cached = knownMasks.find(value);
      if (cached != knownMasks.end()) {
        return cached->second;
      }
      unsigned width = valueBitWidth(value);
      if (width == 0) {
        return llvm::APInt();
      }
      llvm::APInt result(width, 0);
      if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value)) {
        result = PartialDemandState::trimmedMask(constant->getValue(), width);
        knownMasks.emplace(value, result);
        return result;
      }
      auto *inst = llvm::dyn_cast<llvm::Instruction>(value);
      if (inst == nullptr) {
        result = fullMaskFor(value);
        knownMasks.emplace(value, result);
        return result;
      }
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(inst)) {
        if (std::optional<NativeRegisterValueExtractInfo> extract =
                parseNativeRegisterValueExtract(*call)) {
          result = fullMaskFor(value);
          knownMasks.emplace(value, result);
          return result;
        }
        if (std::optional<NativeRegisterValueInsertInfo> insert =
                parseNativeRegisterValueInsert(*call)) {
          llvm::APInt baseMask = knownValueMask(insert->Base);
          llvm::APInt valueMask = knownValueMask(insert->Value);
          llvm::APInt writeMask = partialWriteMask(
              insert->FullWidth, insert->WriteWidth, insert->BitOffset);
          result = (baseMask & ~writeMask) |
                   shiftedMask(valueMask, insert->BitOffset, insert->FullWidth);
          knownMasks.emplace(value, result);
          return result;
        }
      }
      auto computeUnaryMask = [&](unsigned operandIndex) -> llvm::APInt {
        return knownValueMask(inst->getOperand(operandIndex));
      };
      switch (inst->getOpcode()) {
      case llvm::Instruction::And:
        if (auto *lhs =
                llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(0))) {
          result = PartialDemandState::trimmedMask(lhs->getValue(), width);
          break;
        }
        if (auto *rhs =
                llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1))) {
          result = PartialDemandState::trimmedMask(rhs->getValue(), width);
          break;
        }
        result = computeUnaryMask(0) & computeUnaryMask(1);
        break;
      case llvm::Instruction::Or:
      case llvm::Instruction::Xor:
        if (inst->getOpcode() == llvm::Instruction::Xor &&
            inst->getOperand(0) == inst->getOperand(1)) {
          result = llvm::APInt(width, 0);
        } else {
          result = computeUnaryMask(0) | computeUnaryMask(1);
        }
        break;
      case llvm::Instruction::Trunc:
        result = llvm::APInt::getAllOnes(width);
        break;
      case llvm::Instruction::ZExt: {
        unsigned srcWidth = valueBitWidth(inst->getOperand(0));
        result = maskForLowBits(width, srcWidth);
        break;
      }
      case llvm::Instruction::SExt: {
        unsigned srcWidth = valueBitWidth(inst->getOperand(0));
        result = maskForLowBits(width, srcWidth);
        break;
      }
      case llvm::Instruction::Shl:
        if (auto *shift =
                llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1))) {
          unsigned amount = shift->getLimitedValue();
          if (amount < width) {
            result = shiftedMask(computeUnaryMask(0), amount, width);
          }
          break;
        }
        result = fullMaskFor(value);
        break;
      case llvm::Instruction::LShr:
        if (auto *shift =
                llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1))) {
          unsigned amount = shift->getLimitedValue();
          if (amount < width) {
            result = computeUnaryMask(0).lshr(amount).zextOrTrunc(width);
          }
          break;
        }
        result = fullMaskFor(value);
        break;
      case llvm::Instruction::Select:
        result = computeUnaryMask(1) | computeUnaryMask(2);
        break;
      case llvm::Instruction::ExtractValue:
      case llvm::Instruction::InsertValue:
        // Partial demand tracks scalar register bits, not aggregate layout.
        // Conservatively keep the scalar result instead of merging masks from
        // aggregate operands with unrelated or zero bit widths.
        result = fullMaskFor(value);
        break;
      default:
        result = fullMaskFor(value);
        break;
      }
      knownMasks.emplace(value, result);
      return result;
    };
    auto enqueue = [&](llvm::Value *value, llvm::APInt demand) {
      if (value == nullptr || demand.isZero()) {
        return;
      }
      unsigned bitWidth = valueBitWidth(value);
      if (bitWidth == 0) {
        return;
      }
      demand = PartialDemandState::trimmedMask(demand, bitWidth);
      if (demand.isZero()) {
        return;
      }
      auto it = demands.find(value);
      if (it == demands.end()) {
        demands.emplace(value, demand);
        worklist.push_back(value);
        return;
      }
      llvm::APInt merged = it->second | demand;
      if (merged != it->second) {
        it->second = merged;
        worklist.push_back(value);
      }
    };
    auto seedOperand = [&](llvm::Value *value) {
      enqueue(value, fullMaskFor(value));
    };
    auto seedMemoryPointer = [&](llvm::Value *pointer) {
      seedOperand(pointer);
      auto *op = llvm::dyn_cast_or_null<llvm::Operator>(pointer);
      if (op != nullptr && op->getOpcode() == llvm::Instruction::IntToPtr &&
          op->getNumOperands() != 0) {
        seedOperand(op->getOperand(0));
      }
    };

    for (llvm::WeakVH &handle : ReturnObservationValues) {
      if (llvm::Value *value =
              llvm::dyn_cast_or_null<llvm::Value>(handle)) {
        seedOperand(resolve(value));
      }
    }

    if (auto returnsIt = SignatureState.FunctionReturns.find(&Function);
        returnsIt != SignatureState.FunctionReturns.end()) {
      for (const auto &[ret, values] : returnsIt->second) {
        (void)ret;
        for (llvm::Value *value : values) {
          if (isRecordedFunctionReturnValue(value)) {
            seedOperand(resolve(value));
          }
        }
      }
    }

    for (llvm::Instruction &inst : llvm::instructions(Function)) {
      if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
        if (llvm::Value *value = ret->getReturnValue()) {
          seedOperand(value);
        }
        continue;
      }
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (parseNativeRegisterValueExtract(*call) ||
            parseNativeRegisterValueInsert(*call)) {
          continue;
        }
        for (llvm::Use &operand : call->args()) {
          seedOperand(operand.get());
        }
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        RegisterAccess access = registerLoad(*load, Units);
        if (access.Unit == nullptr) {
          seedMemoryPointer(load->getPointerOperand());
        }
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit == nullptr) {
          seedMemoryPointer(store->getPointerOperand());
          seedOperand(store->getValueOperand());
        }
        continue;
      }
      if (auto *br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
        if (!br->isUnconditional()) {
          seedOperand(br->getCondition());
        }
        continue;
      }
      if (auto *sw = llvm::dyn_cast<llvm::SwitchInst>(&inst)) {
        seedOperand(sw->getCondition());
        continue;
      }
      if (auto *icmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
        seedOperand(icmp->getOperand(0));
        seedOperand(icmp->getOperand(1));
        continue;
      }
      if (auto *fcmp = llvm::dyn_cast<llvm::FCmpInst>(&inst)) {
        seedOperand(fcmp->getOperand(0));
        seedOperand(fcmp->getOperand(1));
        continue;
      }
    }

    while (!worklist.empty()) {
      llvm::Value *value = worklist.back();
      worklist.pop_back();
      llvm::APInt demand = demands[value];
      auto *inst = llvm::dyn_cast<llvm::Instruction>(value);
      if (inst == nullptr) {
        continue;
      }
      llvm::APInt inputDemand = demand;
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(inst)) {
        if (std::optional<NativeRegisterValueExtractInfo> extract =
                parseNativeRegisterValueExtract(*call)) {
          enqueue(
              extract->FullValue,
              shiftedMask(inputDemand, extract->BitOffset, extract->FullWidth));
          continue;
        }
        if (std::optional<NativeRegisterValueInsertInfo> insert =
                parseNativeRegisterValueInsert(*call)) {
          llvm::APInt writeMask = partialWriteMask(
              insert->FullWidth, insert->WriteWidth, insert->BitOffset);
          enqueue(insert->Base, inputDemand & ~writeMask);
          enqueue(insert->Value,
                  shlSourceDemand(inputDemand & writeMask, insert->BitOffset,
                                  insert->WriteWidth));
          continue;
        }
      }
      switch (inst->getOpcode()) {
      case llvm::Instruction::Trunc:
        enqueue(inst->getOperand(0), inputDemand);
        break;
      case llvm::Instruction::ZExt:
        enqueue(inst->getOperand(0),
                PartialDemandState::trimmedMask(
                    inputDemand, valueBitWidth(inst->getOperand(0))));
        break;
      case llvm::Instruction::SExt:
        if (auto *srcType = llvm::dyn_cast<llvm::IntegerType>(
                inst->getOperand(0)->getType())) {
          unsigned srcWidth = srcType->getBitWidth();
          llvm::APInt sourceDemand =
              PartialDemandState::trimmedMask(inputDemand, srcWidth);
          llvm::APInt highMask =
              inputDemand & ~maskForLowBits(valueBitWidth(inst), srcWidth);
          if (!highMask.isZero() && srcWidth > 0) {
            sourceDemand |= llvm::APInt::getSignMask(srcWidth);
          }
          enqueue(inst->getOperand(0), sourceDemand);
        } else {
          enqueue(inst->getOperand(0), inputDemand);
        }
        break;
      case llvm::Instruction::BitCast:
      case llvm::Instruction::Freeze:
        enqueue(inst->getOperand(0), inputDemand);
        break;
      case llvm::Instruction::PHI:
        for (llvm::Value *incoming : inst->operand_values()) {
          enqueue(incoming, inputDemand);
        }
        break;
      case llvm::Instruction::Select:
        enqueue(inst->getOperand(1), inputDemand);
        enqueue(inst->getOperand(2), inputDemand);
        break;
      case llvm::Instruction::And: {
        llvm::ConstantInt *constOp = nullptr;
        if (auto *lhs =
                llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(0))) {
          constOp = lhs;
          enqueue(inst->getOperand(1), inputDemand & lhs->getValue());
        } else if (auto *rhs =
                       llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1))) {
          constOp = rhs;
          enqueue(inst->getOperand(0), inputDemand & rhs->getValue());
        } else {
          enqueue(inst->getOperand(0), inputDemand);
          enqueue(inst->getOperand(1), inputDemand);
        }
        (void)constOp;
        break;
      }
      case llvm::Instruction::Or:
      case llvm::Instruction::Xor: {
        if (inst->getOpcode() == llvm::Instruction::Xor &&
            inst->getOperand(0) == inst->getOperand(1)) {
          break;
        }
        llvm::APInt lhsMask = knownValueMask(inst->getOperand(0));
        llvm::APInt rhsMask = knownValueMask(inst->getOperand(1));
        bool canSplit = inst->getOpcode() == llvm::Instruction::Or &&
                        isDisjointOr(*inst) &&
                        lhsMask.getBitWidth() == rhsMask.getBitWidth() &&
                        !lhsMask.isZero() && !rhsMask.isZero();
        if (canSplit) {
          enqueue(inst->getOperand(0), inputDemand & lhsMask);
          enqueue(inst->getOperand(1), inputDemand & rhsMask);
        } else {
          enqueue(inst->getOperand(0), inputDemand);
          enqueue(inst->getOperand(1), inputDemand);
        }
        break;
      }
      case llvm::Instruction::Add:
      case llvm::Instruction::Sub:
      case llvm::Instruction::Mul: {
        unsigned srcWidth = valueBitWidth(inst->getOperand(0));
        unsigned resultWidth = valueBitWidth(inst);
        // 乘积位 [0,W) 只依赖两个操作数的 [0,W)（模 2^W 可交换），位 [W,R) 依赖
        // 操作数的全部位。操作数与结果同宽时（W == R，lift 出来的 mulq 就是
        // `mul nuw i128 (zext),(zext)`）`~maskForLowBits(R, W)` 恒为空，旧代码
        // 会把高位段按 lane 只 demand 操作数的同一段；两个 zext 操作数一裁剪就
        // 得到零 demand，被 partial-demand 当成无用值替换成 poison，进而毒化整条
        // 乘法链（连分支条件一起）。同宽乘法没有"低位段"可依赖，任何一位需求都
        // 必须 demand 全部操作数。Add/Sub 不在此列：它们的 lane demand 只是保守
        // 度问题，改动会放大全程序的需求面。
        bool sameWidthMul = inst->getOpcode() == llvm::Instruction::Mul &&
                            srcWidth >= resultWidth;
        llvm::APInt lowDemand =
            PartialDemandState::trimmedMask(inputDemand, srcWidth);
        llvm::APInt highDemand =
            sameWidthMul
                ? inputDemand
                : inputDemand & ~maskForLowBits(resultWidth, srcWidth);
        if (!highDemand.isZero()) {
          enqueue(inst->getOperand(0), fullMaskFor(inst->getOperand(0)));
          enqueue(inst->getOperand(1), fullMaskFor(inst->getOperand(1)));
        } else {
          enqueue(inst->getOperand(0), lowDemand);
          enqueue(inst->getOperand(1), lowDemand);
        }
        break;
      }
      case llvm::Instruction::Shl:
      case llvm::Instruction::LShr:
      case llvm::Instruction::AShr: {
        auto *shift = llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1));
        if (shift == nullptr) {
          // 动态移位量没有逐位映射关系：任何源位都可能被移到 demanded 位置，
          // 移位量本身也完全参与结果。两者都必须保持全量 demand，否则
          // partial demand 会把移位量整块替换成 poison，结果随之变成 poison，
          // InstCombine/SimplifyCFG 再把依赖它的分支折叠成常量，删掉另一条
          // 分支上的真实调用和基本块。
          enqueue(inst->getOperand(0), fullMaskFor(inst->getOperand(0)));
          enqueue(inst->getOperand(1), fullMaskFor(inst->getOperand(1)));
          break;
        }
        unsigned amount = shift->getLimitedValue();
        switch (inst->getOpcode()) {
        case llvm::Instruction::Shl:
          enqueue(inst->getOperand(0),
                  shlSourceDemand(inputDemand, amount,
                                  valueBitWidth(inst->getOperand(0))));
          break;
        case llvm::Instruction::LShr:
          enqueue(inst->getOperand(0),
                  lshrSourceDemand(inputDemand, amount,
                                   valueBitWidth(inst->getOperand(0))));
          break;
        default:
          // AShr 符号位扩散没有逐位逆映射，保持原有 demand 行为。
          enqueue(inst->getOperand(0), inputDemand);
          break;
        }
        break;
      }
      case llvm::Instruction::PtrToInt:
      case llvm::Instruction::IntToPtr:
      case llvm::Instruction::AddrSpaceCast:
        enqueue(inst->getOperand(0), inputDemand);
        break;
      case llvm::Instruction::Load:
        seedMemoryPointer(inst->getOperand(0));
        break;
      case llvm::Instruction::InsertValue:
      case llvm::Instruction::ExtractValue:
        for (llvm::Value *operand : inst->operand_values()) {
          enqueue(operand, inputDemand);
        }
        break;
      default:
        for (llvm::Value *operand : inst->operand_values()) {
          enqueue(operand, inputDemand);
        }
        break;
      }
    }
    return demands;
  }

void FunctionBuilder::rewritePartialWrites() {
    UndemandedWalkClean.clear();
    std::map<llvm::Value *, llvm::APInt> demands = computePartialDemands();
    std::vector<llvm::StoreInst *> stores;
    std::vector<llvm::CallBase *> partialWrites;
    for (llvm::Instruction &inst : llvm::instructions(Function)) {
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        stores.push_back(store);
        continue;
      }
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (parseNativeRegisterPartialWrite(*call)) {
          partialWrites.push_back(call);
        }
      }
    }
    for (llvm::StoreInst *store : stores) {
      if (store->getParent() == nullptr) {
        continue;
      }
      RegisterAccess access = registerStore(*store, Units);
      if (access.Unit == nullptr || !access.IsStorageValue) {
        continue;
      }
      ++Summary.PartialDemandCandidates;
      if (!PostSignatureCleanup &&
          (isRecordedCallArgStore(store) ||
           isRecordedCallArgValue(store->getValueOperand()))) {
        ++Summary.PartialDemandRejected;
        continue;
      }
      llvm::SmallPtrSet<llvm::Value *, 16> visiting;
      if (rewriteUndemandedRegisterStoreOperands(store->getValueOperand(),
                                                 demands, visiting)) {
        ++Summary.PartialDemandMatched;
      } else {
        ++Summary.PartialDemandRejected;
      }
    }
    for (llvm::CallBase *call : partialWrites) {
      if (call->getParent() == nullptr) {
        continue;
      }
      std::optional<NativeRegisterPartialWriteInfo> partial =
          parseNativeRegisterPartialWrite(*call);
      if (!partial || Units.count(partial->Global) == 0) {
        continue;
      }
      ++Summary.PartialDemandCandidates;
      llvm::SmallPtrSet<llvm::Value *, 16> visiting;
      if (rewriteUndemandedRegisterStoreOperands(partial->Value, demands,
                                                 visiting)) {
        ++Summary.PartialDemandMatched;
      } else {
        ++Summary.PartialDemandRejected;
      }
    }
  }

void FunctionBuilder::eraseDeadPartialWriteCall(llvm::CallBase *call) {
    eraseInstructionFromParent(*call);
    ++Summary.DeadStoresRemoved;
  }

void FunctionBuilder::collectAccesses() {
    for (llvm::BasicBlock &block : Function) {
      for (llvm::Instruction &inst : block) {
        if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
          RegisterAccess access =
              registerLoad(*load, Units,
                             RegisterReadOptions{/*IncludeX87WindowShift=*/true});
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
          continue;
        }
        if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
          std::optional<NativeRegisterPartialReadInfo> partialRead =
              parseNativeRegisterPartialRead(*call);
          if (partialRead && Units.count(partialRead->Global) != 0) {
            ++Summary.LoadsSeen;
            if (call->getMetadata("notdec.register.summary_ssa.range_entry") !=
                nullptr) {
              continue;
            }
            PartialReads.push_back(call);
            continue;
          }
          std::optional<NativeRegisterPartialWriteInfo> partial =
              parseNativeRegisterPartialWrite(*call);
          if (partial && Units.count(partial->Global) != 0) {
            ++Summary.StoresSeen;
          }
        }
      }
    }
  }

llvm::MDNode *FunctionBuilder::markerNode(llvm::StringRef value) const {
    llvm::Metadata *fields[] = {
        llvm::MDString::get(Function.getContext(), value),
    };
    return llvm::MDNode::get(Function.getContext(), fields);
  }

void FunctionBuilder::attachMetadata() {
    uint64_t phisRemaining = 0;
    for (const auto &[key, phi] : PendingRangePhi) {
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
        llvm::MDString::get(Function.getContext(),
                            "range_registers_planned=" +
                                std::to_string(Summary.RangeRegistersPlanned)),
        llvm::MDString::get(Function.getContext(),
                            "range_segments_planned=" +
                                std::to_string(Summary.RangeSegmentsPlanned)),
        llvm::MDString::get(Function.getContext(),
                            "range_read_events=" +
                                std::to_string(Summary.RangeReadEvents)),
        llvm::MDString::get(Function.getContext(),
                            "range_write_events=" +
                                std::to_string(Summary.RangeWriteEvents)),
        llvm::MDString::get(Function.getContext(),
                            "range_clobber_events=" +
                                std::to_string(Summary.RangeClobberEvents)),
    };
    Function.setMetadata("notdec.register.summary_ssa",
                         llvm::MDNode::get(Function.getContext(), fields));
  }

} // namespace notdec::bin2llvm
