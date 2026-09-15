#include "notdec-bin2llvm/NativeFunctionPointerPromotion.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <optional>
#include <set>
#include <string>

namespace notdec::bin2llvm {
namespace {

bool isNativeRelocationSlot(const llvm::GlobalVariable &global) {
  return global.hasName() &&
         global.getName().starts_with("notdec.reloc.0x");
}

std::optional<llvm::Function *>
functionFromPointerToIntConstant(const llvm::Constant *constant) {
  auto *expression = llvm::dyn_cast<llvm::ConstantExpr>(constant);
  if (expression == nullptr ||
      expression->getOpcode() != llvm::Instruction::PtrToInt ||
      expression->getNumOperands() != 1) {
    return std::nullopt;
  }
  auto *function =
      llvm::dyn_cast<llvm::Function>(expression->getOperand(0));
  if (function == nullptr) {
    return std::nullopt;
  }
  return function;
}

struct SlotTargets {
  std::set<llvm::Function *> Functions;
  bool Unknown = false;
};

SlotTargets collectSlotTargets(llvm::GlobalVariable &slot) {
  SlotTargets targets;
  if (!slot.hasInitializer()) {
    targets.Unknown = true;
    return targets;
  }
  if (auto function = functionFromPointerToIntConstant(slot.getInitializer())) {
    targets.Functions.insert(*function);
  } else {
    targets.Unknown = true;
  }

  for (llvm::User *user : slot.users()) {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(user)) {
      if (load->getPointerOperand() != &slot) {
        targets.Unknown = true;
      }
      continue;
    }
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(user)) {
      if (store->getPointerOperand() != &slot) {
        targets.Unknown = true;
        continue;
      }
      auto function = functionFromPointerToIntConstant(
          llvm::dyn_cast<llvm::Constant>(store->getValueOperand()));
      if (function) {
        targets.Functions.insert(*function);
      } else {
        targets.Unknown = true;
      }
      continue;
    }
    // Address escapes into a call/GEP/other computation: an external write
    // cannot be ruled out from the module alone.
    targets.Unknown = true;
  }
  return targets;
}

llvm::GlobalVariable *slotLoadedByCall(llvm::Value *value) {
  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(value)) {
    return llvm::dyn_cast<llvm::GlobalVariable>(
        load->getPointerOperand()->stripPointerCasts());
  }
  if (auto *intToPtr = llvm::dyn_cast<llvm::IntToPtrInst>(value)) {
    return slotLoadedByCall(intToPtr->getOperand(0));
  }
  if (auto *constant = llvm::dyn_cast<llvm::ConstantExpr>(value)) {
    if (constant->getOpcode() == llvm::Instruction::IntToPtr &&
        constant->getNumOperands() == 1) {
      return slotLoadedByCall(constant->getOperand(0));
    }
    if (constant->getOpcode() == llvm::Instruction::BitCast &&
        constant->getNumOperands() == 1) {
      return slotLoadedByCall(constant->getOperand(0));
    }
  }
  if (auto *cast = llvm::dyn_cast<llvm::CastInst>(value)) {
    if (cast->getNumOperands() == 1) {
      return slotLoadedByCall(cast->getOperand(0));
    }
  }
  return nullptr;
}

void promoteCall(llvm::CallInst &oldCall, llvm::Function &target) {
  llvm::SmallVector<llvm::Value *, 8> arguments(oldCall.arg_begin(),
                                                oldCall.arg_end());
  llvm::IRBuilder<> builder(&oldCall);
  llvm::CallInst *newCall = builder.CreateCall(
      oldCall.getFunctionType(), &target, arguments, oldCall.getName());
  newCall->setCallingConv(oldCall.getCallingConv());
  newCall->setTailCallKind(oldCall.getTailCallKind());
  newCall->setAttributes(oldCall.getAttributes());
  newCall->copyMetadata(oldCall);
  newCall->setDebugLoc(oldCall.getDebugLoc());
  oldCall.replaceAllUsesWith(newCall);
  oldCall.eraseFromParent();
}

} // namespace

NativeFunctionPointerPromotionSummary runNativeFunctionPointerPromotion(
    llvm::Module &module,
    const NativeFunctionPointerPromotionOptions &options) {
  (void)options;
  NativeFunctionPointerPromotionSummary summary;
  for (llvm::GlobalVariable &global : module.globals()) {
    if (!isNativeRelocationSlot(global)) {
      continue;
    }
    SlotTargets targets = collectSlotTargets(global);
    if (targets.Unknown) {
      ++summary.SlotsWithUnknownWrites;
    } else if (targets.Functions.size() > 1) {
      ++summary.SlotsWithMultipleTargets;
    } else if (targets.Functions.size() == 1) {
      ++summary.SlotsWithKnownSingleTarget;
    }
  }

  std::vector<llvm::CallInst *> calls;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &instruction : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
      if (call == nullptr || call->getCalledFunction() != nullptr ||
          call->isMustTailCall()) {
        continue;
      }
      ++summary.IndirectCallsSeen;
      calls.push_back(call);
    }
  }

  for (llvm::CallInst *call : calls) {
    llvm::GlobalVariable *slot =
        slotLoadedByCall(call->getCalledOperand());
    if (slot == nullptr || !isNativeRelocationSlot(*slot)) {
      continue;
    }
    SlotTargets targets = collectSlotTargets(*slot);
    if (targets.Unknown || targets.Functions.size() != 1) {
      continue;
    }
    llvm::Function *target = *targets.Functions.begin();
    if (target->isDeclaration()) {
      continue;
    }
    promoteCall(*call, *target);
    ++summary.IndirectCallsPromoted;
  }
  return summary;
}

} // namespace notdec::bin2llvm
