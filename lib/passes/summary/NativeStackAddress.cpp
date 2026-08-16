#include "notdec-bin2llvm/passes/summary/NativeStackAddress.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>

namespace notdec::bin2llvm {
namespace {

std::optional<llvm::StringRef> metadataField(const llvm::MDNode *node,
                                             llvm::StringRef key) {
  if (node == nullptr) {
    return std::nullopt;
  }
  std::string prefix = (key + "=").str();
  for (const llvm::MDOperand &operand : node->operands()) {
    auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
    if (text == nullptr) {
      continue;
    }
    llvm::StringRef value = text->getString();
    if (value.starts_with(prefix)) {
      return value.drop_front(prefix.size());
    }
  }
  return std::nullopt;
}

llvm::StringRef nativeRegisterName(const llvm::GlobalVariable &global) {
  if (std::optional<llvm::StringRef> name =
          metadataField(global.getMetadata("notdec.register"), "name");
      name && !name->empty()) {
    return *name;
  }
  return global.getName();
}

bool isStackAlignmentMask(llvm::Value *value) {
  auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value);
  if (constant == nullptr || constant->getBitWidth() > 64) {
    return false;
  }
  int64_t mask = constant->getSExtValue();
  if (mask >= -1) {
    return false;
  }
  // Do not negate INT64_MIN as a signed value.  The bit pattern is still a
  // valid power-of-two alignment mask, and unsigned subtraction gives its
  // magnitude without overflow.
  uint64_t magnitude = 0 - static_cast<uint64_t>(mask);
  return llvm::isPowerOf2_64(magnitude);
}

std::optional<int64_t> signedConstantValue(llvm::Value *value) {
  auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value);
  if (constant == nullptr || constant->getBitWidth() > 64) {
    return std::nullopt;
  }
  return constant->getSExtValue();
}

std::optional<int64_t> subtractionOffset(int64_t offset) {
  if (offset == std::numeric_limits<int64_t>::min()) {
    return std::nullopt;
  }
  return -offset;
}

bool isNativeStackAlloca(const llvm::AllocaInst &alloca) {
  return alloca.hasName() &&
         alloca.getName().starts_with("notdec_stack.native");
}

std::optional<NativeStackAddress>
nativeFrameIntegerAddress(llvm::Value *value, const llvm::DataLayout &layout,
                          llvm::SmallPtrSetImpl<llvm::Value *> &seen);

std::optional<NativeStackAddress>
nativeFramePointerAddress(llvm::Value *value, const llvm::DataLayout &layout,
                          llvm::SmallPtrSetImpl<llvm::Value *> &seen) {
  value = value->stripPointerCasts();
  if (!seen.insert(value).second) {
    return std::nullopt;
  }
  if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(value)) {
    if (isNativeStackAlloca(*alloca)) {
      return NativeStackAddress{NativeStackAddressKind::NativeFrame, alloca, 0};
    }
    return std::nullopt;
  }
  if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value)) {
    std::optional<NativeStackAddress> base =
        nativeFramePointerAddress(gep->getPointerOperand(), layout, seen);
    if (!base) {
      return std::nullopt;
    }
    llvm::APInt byteOffset(
        layout.getIndexSizeInBits(gep->getPointerAddressSpace()), 0);
    if (!gep->accumulateConstantOffset(layout, byteOffset)) {
      return std::nullopt;
    }
    return nativeStackAddressWithOffset(*base, byteOffset.getSExtValue());
  }
  if (auto *intToPtr = llvm::dyn_cast<llvm::IntToPtrInst>(value)) {
    return nativeFrameIntegerAddress(intToPtr->getOperand(0), layout, seen);
  }
  return std::nullopt;
}

std::optional<NativeStackAddress>
nativeFrameIntegerAddress(llvm::Value *value, const llvm::DataLayout &layout,
                          llvm::SmallPtrSetImpl<llvm::Value *> &seen) {
  value = value->stripPointerCasts();
  if (!seen.insert(value).second) {
    return std::nullopt;
  }
  if (auto *ptrToInt = llvm::dyn_cast<llvm::PtrToIntInst>(value)) {
    return nativeFramePointerAddress(ptrToInt->getPointerOperand(), layout,
                                     seen);
  }
  auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(value);
  if (binary == nullptr || (binary->getOpcode() != llvm::Instruction::Add &&
                            binary->getOpcode() != llvm::Instruction::Sub)) {
    return std::nullopt;
  }
  if (std::optional<NativeStackAddress> base =
          nativeFrameIntegerAddress(binary->getOperand(0), layout, seen)) {
    if (std::optional<int64_t> offset =
            signedConstantValue(binary->getOperand(1))) {
      std::optional<int64_t> delta =
          binary->getOpcode() == llvm::Instruction::Sub
              ? subtractionOffset(*offset)
              : std::optional<int64_t>(*offset);
      if (!delta) {
        return std::nullopt;
      }
      return nativeStackAddressWithOffset(*base, *delta);
    }
  }
  if (binary->getOpcode() == llvm::Instruction::Add) {
    if (std::optional<NativeStackAddress> base =
            nativeFrameIntegerAddress(binary->getOperand(1), layout, seen)) {
      if (std::optional<int64_t> offset =
              signedConstantValue(binary->getOperand(0))) {
        return nativeStackAddressWithOffset(*base, *offset);
      }
    }
  }
  return std::nullopt;
}

} // namespace

std::optional<NativeStackAddress>
nativeStackAddressWithOffset(NativeStackAddress address, int64_t offset) {
  if ((offset > 0 &&
       address.Offset > std::numeric_limits<int64_t>::max() - offset) ||
      (offset < 0 &&
       address.Offset < std::numeric_limits<int64_t>::min() - offset)) {
    return std::nullopt;
  }
  address.Offset += offset;
  return address;
}

llvm::GlobalVariable *findNativeStackPointerGlobal(llvm::Module &module,
                                                   llvm::StringRef name) {
  llvm::GlobalVariable *result = nullptr;
  for (llvm::GlobalVariable &global : module.globals()) {
    if (nativeRegisterName(global) != name) {
      continue;
    }
    if (result != nullptr && result != &global) {
      return nullptr;
    }
    result = &global;
  }
  return result;
}

NativeStackAddressAnalysis::NativeStackAddressAnalysis(
    llvm::Function &function, llvm::GlobalVariable *stackPointer,
    llvm::StringRef stackPointerName)
    : Function(function), StackPointer(stackPointer),
      StackPointerName(stackPointerName) {
  build();
}

std::optional<NativeStackAddress>
NativeStackAddressAnalysis::stackPointerBefore(
    const llvm::Instruction &instruction) const {
  auto found = Before.find(&instruction);
  if (found == Before.end() || !found->second.Reachable) {
    return std::nullopt;
  }
  return found->second.Address;
}

std::optional<NativeStackAddress>
NativeStackAddressAnalysis::addressForPointer(llvm::Value *pointer) const {
  if (pointer == nullptr) {
    return std::nullopt;
  }
  if (std::optional<NativeStackAddress> frame =
          addressForNativeFramePointer(pointer)) {
    return frame;
  }
  pointer = pointer->stripPointerCasts();
  auto *intToPtr = llvm::dyn_cast<llvm::IntToPtrInst>(pointer);
  return intToPtr == nullptr ? std::nullopt
                             : addressForInteger(intToPtr->getOperand(0));
}

std::optional<NativeStackAddress>
NativeStackAddressAnalysis::addressForIntegerValue(llvm::Value *value) const {
  return value == nullptr ? std::nullopt : addressForInteger(value);
}

void NativeStackAddressAnalysis::build() {
  if (StackPointer == nullptr || Function.empty()) {
    return;
  }

  size_t maxIterations = std::max<size_t>(1, Function.size() * 4);
  for (size_t iteration = 0; iteration < maxIterations; ++iteration) {
    bool changed = false;
    for (llvm::BasicBlock &block : Function) {
      StackPointerState state = blockEntryState(block);
      for (llvm::Instruction &instruction : block) {
        Before[&instruction] = state;
        transfer(instruction, state);
      }
      auto found = BlockOut.find(&block);
      if (found == BlockOut.end() || found->second != state) {
        BlockOut[&block] = state;
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
  }

  // Refill per-instruction states after predecessor facts have converged.
  Before.clear();
  for (llvm::BasicBlock &block : Function) {
    StackPointerState state = blockEntryState(block);
    for (llvm::Instruction &instruction : block) {
      Before[&instruction] = state;
      transfer(instruction, state);
    }
  }
}

NativeStackAddressAnalysis::StackPointerState
NativeStackAddressAnalysis::blockEntryState(
    const llvm::BasicBlock &block) const {
  if (&block == &Function.getEntryBlock()) {
    return {true, NativeStackAddress{}};
  }

  StackPointerState result;
  bool sawPredecessor = false;
  for (const llvm::BasicBlock *predecessor : llvm::predecessors(&block)) {
    auto found = BlockOut.find(predecessor);
    if (found == BlockOut.end() || !found->second.Reachable) {
      continue;
    }
    if (!sawPredecessor) {
      result = found->second;
      sawPredecessor = true;
      continue;
    }
    result.Reachable = true;
    if (result.Address != found->second.Address) {
      result.Address = std::nullopt;
    }
  }
  return result;
}

void NativeStackAddressAnalysis::transfer(const llvm::Instruction &instruction,
                                          StackPointerState &state) const {
  if (!state.Reachable || !isStackPointerStore(instruction)) {
    return;
  }
  const auto &store = llvm::cast<llvm::StoreInst>(instruction);
  state.Address =
      addressForInteger(const_cast<llvm::Value *>(store.getValueOperand()));
}

bool NativeStackAddressAnalysis::isStackPointerLoad(
    const llvm::Value &value) const {
  auto *load = llvm::dyn_cast<llvm::LoadInst>(&value);
  return load != nullptr && StackPointer != nullptr &&
         load->getType() == StackPointer->getValueType() &&
         load->getPointerOperand()->stripPointerCasts() == StackPointer;
}

bool NativeStackAddressAnalysis::isStackPointerStore(
    const llvm::Instruction &instruction) const {
  auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
  return store != nullptr && StackPointer != nullptr &&
         store->getValueOperand()->getType() == StackPointer->getValueType() &&
         store->getPointerOperand()->stripPointerCasts() == StackPointer;
}

bool NativeStackAddressAnalysis::isSummaryStackPointerValue(
    const llvm::Value &value) const {
  auto *instruction = llvm::dyn_cast<llvm::Instruction>(&value);
  if (instruction == nullptr || StackPointerName.empty()) {
    return false;
  }
  if (std::optional<llvm::StringRef> name = metadataField(
          instruction->getMetadata("notdec.register.summary_ssa.phi"), "name");
      name && *name == StackPointerName) {
    return true;
  }
  std::string prefix = StackPointerName.str() + ".range_summary_ssa";
  return value.hasName() && value.getName().starts_with(prefix);
}

std::optional<NativeStackAddress>
NativeStackAddressAnalysis::addressForInteger(llvm::Value *value) const {
  llvm::SmallPtrSet<llvm::Value *, 16> seen;
  std::function<std::optional<NativeStackAddress>(llvm::Value *)> visit =
      [&](llvm::Value *candidate) -> std::optional<NativeStackAddress> {
    candidate = candidate->stripPointerCasts();
    if (!seen.insert(candidate).second) {
      return std::nullopt;
    }
    if (isStackPointerLoad(*candidate)) {
      auto *load = llvm::cast<llvm::LoadInst>(candidate);
      return stackPointerBefore(*load);
    }
    if (isSummaryStackPointerValue(*candidate)) {
      return NativeStackAddress{NativeStackAddressKind::RelativeStackPointer,
                                candidate, 0};
    }
    if (auto *ptrToInt = llvm::dyn_cast<llvm::PtrToIntInst>(candidate)) {
      if (std::optional<NativeStackAddress> frame =
              addressForNativeFramePointer(ptrToInt->getPointerOperand())) {
        return frame;
      }
    }
    if (auto *phi = llvm::dyn_cast<llvm::PHINode>(candidate)) {
      // SummarySSA can leave a loop-invariant stack base as a phi containing
      // self edges plus one concrete entry value.  Accept only that exact form;
      // a phi with two distinct real incoming values is not one stack address.
      llvm::Value *concreteIncoming = nullptr;
      for (llvm::Value *incoming : phi->incoming_values()) {
        if (incoming == phi) {
          continue;
        }
        if (concreteIncoming != nullptr && concreteIncoming != incoming) {
          return std::nullopt;
        }
        concreteIncoming = incoming;
      }
      return concreteIncoming == nullptr ? std::nullopt
                                         : visit(concreteIncoming);
    }
    auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(candidate);
    if (binary == nullptr) {
      return std::nullopt;
    }
    if (binary->getOpcode() == llvm::Instruction::And) {
      llvm::Value *stackOperand = nullptr;
      if (isStackAlignmentMask(binary->getOperand(1))) {
        stackOperand = binary->getOperand(0);
      } else if (isStackAlignmentMask(binary->getOperand(0))) {
        stackOperand = binary->getOperand(1);
      }
      if (stackOperand != nullptr) {
        if (std::optional<NativeStackAddress> base = visit(stackOperand);
            base && base->Kind != NativeStackAddressKind::NativeFrame) {
          return NativeStackAddress{
              NativeStackAddressKind::RelativeStackPointer, binary, 0};
        }
      }
      return std::nullopt;
    }
    if (binary->getOpcode() != llvm::Instruction::Add &&
        binary->getOpcode() != llvm::Instruction::Sub) {
      return std::nullopt;
    }
    if (std::optional<NativeStackAddress> base = visit(binary->getOperand(0))) {
      if (std::optional<int64_t> offset =
              signedConstantValue(binary->getOperand(1))) {
        std::optional<int64_t> delta =
            binary->getOpcode() == llvm::Instruction::Sub
                ? subtractionOffset(*offset)
                : std::optional<int64_t>(*offset);
        if (!delta) {
          return std::nullopt;
        }
        return nativeStackAddressWithOffset(*base, *delta);
      }
    }
    if (binary->getOpcode() == llvm::Instruction::Add) {
      if (std::optional<NativeStackAddress> base =
              visit(binary->getOperand(1))) {
        if (std::optional<int64_t> offset =
                signedConstantValue(binary->getOperand(0))) {
          return nativeStackAddressWithOffset(*base, *offset);
        }
      }
    }
    return std::nullopt;
  };
  return visit(value);
}

std::optional<NativeStackAddress>
NativeStackAddressAnalysis::addressForNativeFramePointer(
    llvm::Value *value) const {
  llvm::Module *module = Function.getParent();
  if (module == nullptr) {
    return std::nullopt;
  }
  llvm::SmallPtrSet<llvm::Value *, 16> seen;
  return nativeFramePointerAddress(value, module->getDataLayout(), seen);
}

} // namespace notdec::bin2llvm
