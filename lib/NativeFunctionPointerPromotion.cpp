#include "notdec-bin2llvm/NativeFunctionPointerPromotion.h"

#include "notdec-bin2llvm/NativeRelocationMetadata.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

#include <map>
#include <optional>
#include <set>

namespace notdec::bin2llvm {
namespace {

constexpr unsigned MaxAddressChainDepth = 8;

std::optional<uint64_t> constantUint64(llvm::Value *value) {
  auto *constant = llvm::dyn_cast_or_null<llvm::ConstantInt>(value);
  if (constant == nullptr || constant->getBitWidth() > 64) {
    return std::nullopt;
  }
  return constant->getZExtValue();
}

std::optional<llvm::Function *>
functionFromPointerToIntConstant(llvm::Value *value) {
  auto *constant = llvm::dyn_cast_or_null<llvm::Constant>(value);
  if (constant == nullptr) {
    return std::nullopt;
  }
  auto *expression = llvm::dyn_cast<llvm::ConstantExpr>(constant);
  if (expression == nullptr ||
      expression->getOpcode() != llvm::Instruction::PtrToInt ||
      expression->getNumOperands() != 1) {
    return std::nullopt;
  }
  auto *function = llvm::dyn_cast<llvm::Function>(expression->getOperand(0));
  if (function == nullptr) {
    return std::nullopt;
  }
  return function;
}

// A constant anchor inside an integer address chain: the only constant leaf
// reachable through add/sub/or/xor/zext/sext wrappers, plus whether the chain
// also has dynamic input.
struct IntegerChain {
  std::optional<uint64_t> Constant;
  bool Dynamic = false;
};

IntegerChain analyzeIntegerChain(llvm::Value *value, unsigned depth) {
  IntegerChain result;
  if (value == nullptr || depth > MaxAddressChainDepth) {
    result.Dynamic = true;
    return result;
  }
  if (auto address = constantUint64(value)) {
    result.Constant = *address;
    return result;
  }
  auto *op = llvm::dyn_cast<llvm::Operator>(value);
  if (op == nullptr) {
    result.Dynamic = true;
    return result;
  }
  switch (op->getOpcode()) {
  case llvm::Instruction::Add:
  case llvm::Instruction::Sub:
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor: {
    IntegerChain lhs = analyzeIntegerChain(op->getOperand(0), depth + 1);
    IntegerChain rhs = analyzeIntegerChain(op->getOperand(1), depth + 1);
    result.Dynamic = lhs.Dynamic || rhs.Dynamic;
    if (lhs.Constant && rhs.Constant) {
      switch (op->getOpcode()) {
      case llvm::Instruction::Add:
        result.Constant = *lhs.Constant + *rhs.Constant;
        break;
      case llvm::Instruction::Sub:
        result.Constant = *lhs.Constant - *rhs.Constant;
        break;
      default:
        result.Constant = *lhs.Constant | *rhs.Constant;
        break;
      }
    } else if (lhs.Constant) {
      result.Constant = lhs.Constant;
    } else if (rhs.Constant) {
      result.Constant = rhs.Constant;
    }
    return result;
  }
  case llvm::Instruction::ZExt:
  case llvm::Instruction::SExt:
  case llvm::Instruction::Trunc:
  case llvm::Instruction::PtrToInt:
    return analyzeIntegerChain(op->getOperand(0), depth + 1);
  default:
    result.Dynamic = true;
    return result;
  }
}

// An inttoptr-based pointer reduced to its constant anchor.  Only the common
// "constant slot address (+/- constant or dynamic offset)" shapes are
// recognized; anything else returns an invalid chain.
struct AddressChain {
  uint64_t Base = 0;
  bool Dynamic = false;
  bool Valid = false;
};

AddressChain analyzePointerChain(llvm::Value *pointer,
                                 const llvm::DataLayout &layout,
                                 unsigned depth) {
  AddressChain result;
  if (pointer == nullptr || depth > MaxAddressChainDepth) {
    return result;
  }
  pointer = pointer->stripPointerCasts();
  if (auto *gep = llvm::dyn_cast<llvm::GEPOperator>(pointer)) {
    AddressChain base =
        analyzePointerChain(gep->getPointerOperand(), layout, depth + 1);
    if (!base.Valid) {
      return result;
    }
    llvm::APInt accumulated(layout.getIndexSizeInBits(0), 0);
    if (gep->accumulateConstantOffset(layout, accumulated)) {
      base.Base += accumulated.getSExtValue();
    } else {
      base.Dynamic = true;
    }
    return base;
  }
  llvm::Value *integer = nullptr;
  if (auto *intToPtr = llvm::dyn_cast<llvm::IntToPtrInst>(pointer)) {
    integer = intToPtr->getOperand(0);
  } else if (auto *expression = llvm::dyn_cast<llvm::ConstantExpr>(pointer)) {
    if (expression->getOpcode() == llvm::Instruction::IntToPtr &&
        expression->getNumOperands() == 1) {
      integer = expression->getOperand(0);
    }
  } else if (auto *cast = llvm::dyn_cast<llvm::CastInst>(pointer)) {
    return analyzePointerChain(cast->getOperand(0), layout, depth + 1);
  }
  if (integer == nullptr) {
    return result;
  }
  IntegerChain chain = analyzeIntegerChain(integer, depth + 1);
  if (!chain.Constant) {
    return result;
  }
  result.Base = *chain.Constant;
  result.Dynamic = chain.Dynamic;
  result.Valid = true;
  return result;
}

struct SlotState {
  llvm::Function *Target = nullptr;
  std::set<llvm::Function *> Targets;
  bool UnknownWrite = false;
  bool Escaped = false;
};

// Collects the metadata slots, the visible writes to each slot, and whether a
// slot address escapes as a plain integer (a callee receiving the address can
// write it without this pass seeing the store).
class PromotionAnalysis {
public:
  explicit PromotionAnalysis(llvm::Module &module)
      : Layout(module.getDataLayout()) {
    for (const NativeFunctionPointerSlotInfo &slot :
         readNativeFunctionPointerMetadata(module)) {
      SlotState state;
      state.Target = slot.Target;
      if (slot.Target != nullptr) {
        state.Targets.insert(slot.Target);
      }
      Slots[slot.Address] = state;
    }
    if (Slots.empty()) {
      return;
    }
    scanModule(module);
  }

  bool empty() const { return Slots.empty(); }

  const SlotState *stateFor(uint64_t address) const {
    auto it = Slots.find(address);
    return it == Slots.end() ? nullptr : &it->second;
  }

private:
  void scanModule(llvm::Module &module) {
    for (llvm::Function &function : module) {
      if (function.isDeclaration()) {
        continue;
      }
      for (llvm::Instruction &instruction : llvm::instructions(function)) {
        scanEscapes(instruction);
        if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
          scanStore(*store);
        }
      }
    }
    for (llvm::GlobalVariable &global : module.globals()) {
      if (global.hasInitializer()) {
        scanEscapesInConstant(global.getInitializer());
      }
    }
  }

  void scanEscapes(llvm::Instruction &instruction) {
    for (unsigned index = 0; index < instruction.getNumOperands(); ++index) {
      auto *constant =
          llvm::dyn_cast<llvm::ConstantInt>(instruction.getOperand(index));
      if (constant == nullptr || constant->getBitWidth() > 64) {
        continue;
      }
      auto slot = Slots.find(constant->getZExtValue());
      if (slot == Slots.end()) {
        continue;
      }
      // Converting the constant to a pointer is the read path, not an escape.
      if (auto *cast = llvm::dyn_cast<llvm::IntToPtrInst>(&instruction)) {
        if (cast->getOperand(0) == constant) {
          continue;
        }
      }
      slot->second.Escaped = true;
    }
  }

  void scanEscapesInConstant(llvm::Constant *constant, unsigned depth = 0) {
    if (constant == nullptr || depth > MaxAddressChainDepth) {
      return;
    }
    if (auto *integer = llvm::dyn_cast<llvm::ConstantInt>(constant)) {
      if (integer->getBitWidth() <= 64) {
        auto slot = Slots.find(integer->getZExtValue());
        if (slot != Slots.end()) {
          slot->second.Escaped = true;
        }
      }
      return;
    }
    for (const llvm::Use &operand : constant->operands()) {
      scanEscapesInConstant(llvm::dyn_cast<llvm::Constant>(operand.get()),
                            depth + 1);
    }
  }

  void scanStore(llvm::StoreInst &store) {
    AddressChain chain =
        analyzePointerChain(store.getPointerOperand(), Layout, 0);
    if (!chain.Valid) {
      return;
    }
    auto slot = Slots.find(chain.Base);
    if (slot == Slots.end()) {
      return;
    }
    if (chain.Dynamic) {
      // A dynamic offset from this base can land on the slot.
      slot->second.UnknownWrite = true;
      return;
    }
    auto function = functionFromPointerToIntConstant(store.getValueOperand());
    if (function && *function == slot->second.Target) {
      return;
    }
    if (function) {
      slot->second.Targets.insert(*function);
      return;
    }
    slot->second.UnknownWrite = true;
  }

  const llvm::DataLayout &Layout;
  std::map<uint64_t, SlotState> Slots;
};

// Trace a call target back to "load from a relocation slot".
std::optional<uint64_t> slotLoadedByCall(llvm::Value *value,
                                         const llvm::DataLayout &layout,
                                         unsigned depth = 0) {
  if (value == nullptr || depth > MaxAddressChainDepth) {
    return std::nullopt;
  }
  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(value)) {
    AddressChain chain = analyzePointerChain(load->getPointerOperand(), layout,
                                             depth + 1);
    if (!chain.Valid || chain.Dynamic) {
      return std::nullopt;
    }
    return chain.Base;
  }
  if (auto *intToPtr = llvm::dyn_cast<llvm::IntToPtrInst>(value)) {
    return slotLoadedByCall(intToPtr->getOperand(0), layout, depth + 1);
  }
  if (auto *constant = llvm::dyn_cast<llvm::ConstantExpr>(value)) {
    if ((constant->getOpcode() == llvm::Instruction::IntToPtr ||
         constant->getOpcode() == llvm::Instruction::BitCast) &&
        constant->getNumOperands() == 1) {
      return slotLoadedByCall(constant->getOperand(0), layout, depth + 1);
    }
    return std::nullopt;
  }
  if (auto *cast = llvm::dyn_cast<llvm::CastInst>(value)) {
    return slotLoadedByCall(cast->getOperand(0), layout, depth + 1);
  }
  return std::nullopt;
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
  PromotionAnalysis analysis(module);
  if (analysis.empty()) {
    return summary;
  }

  // Collect first: promoteCall() erases the old call and would invalidate an
  // in-flight instruction iterator.
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
    auto address =
        slotLoadedByCall(call->getCalledOperand(), module.getDataLayout());
    if (!address) {
      continue;
    }
    const SlotState *state = analysis.stateFor(*address);
    if (state == nullptr || state->UnknownWrite || state->Escaped ||
        state->Targets.size() != 1) {
      continue;
    }
    llvm::Function *target = *state->Targets.begin();
    if (target->isDeclaration()) {
      continue;
    }
    promoteCall(*call, *target);
    ++summary.IndirectCallsPromoted;
  }

  for (const NativeFunctionPointerSlotInfo &slot :
       readNativeFunctionPointerMetadata(module)) {
    const SlotState *state = analysis.stateFor(slot.Address);
    if (state == nullptr) {
      continue;
    }
    if (state->UnknownWrite) {
      ++summary.SlotsWithUnknownWrites;
    } else if (state->Escaped) {
      ++summary.SlotsWithEscapedAddress;
    } else if (state->Targets.size() > 1) {
      ++summary.SlotsWithMultipleTargets;
    } else if (state->Targets.size() == 1) {
      ++summary.SlotsWithKnownSingleTarget;
    }
  }
  return summary;
}

} // namespace notdec::bin2llvm
