#include "notdec-bin2llvm/NativeRelocationData.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <map>
#include <optional>
#include <sstream>
#include <string>

namespace notdec::bin2llvm {
namespace {

std::string slotGlobalName(uint64_t address) {
  std::ostringstream os;
  os << "notdec.reloc.0x" << std::hex << address;
  return os.str();
}

std::optional<uint64_t> constantIntToUint64(const llvm::ConstantInt *value) {
  if (value == nullptr || value->getBitWidth() > 64) {
    return std::nullopt;
  }
  return value->getZExtValue();
}

// Return the constant address when the value is exactly inttoptr(constant).
// GEP/bitcast wrappers are intentionally not followed here; the low-level
// model first targets direct slot accesses and leaves general data-image
// address folding to the next stage.
std::optional<uint64_t> directIntToPtrAddress(const llvm::Value *pointer) {
  if (auto *intToPtr = llvm::dyn_cast<llvm::IntToPtrInst>(pointer)) {
    return constantIntToUint64(
        llvm::dyn_cast<llvm::ConstantInt>(intToPtr->getOperand(0)));
  }
  auto *constant = llvm::dyn_cast<llvm::ConstantExpr>(pointer);
  if (constant != nullptr &&
      constant->getOpcode() == llvm::Instruction::IntToPtr &&
      constant->getNumOperands() == 1) {
    return constantIntToUint64(
        llvm::dyn_cast<llvm::ConstantInt>(constant->getOperand(0)));
  }
  return std::nullopt;
}

std::optional<uint64_t> accessSize(const llvm::DataLayout &layout,
                                   const llvm::Instruction &instruction) {
  llvm::Type *type = nullptr;
  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
    type = load->getType();
  } else if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
    type = store->getValueOperand()->getType();
  }
  if (type == nullptr) {
    return std::nullopt;
  }
  llvm::TypeSize size = layout.getTypeStoreSize(type);
  if (size.isScalable()) {
    return std::nullopt;
  }
  return size.getFixedValue();
}

llvm::Value *pointerOperand(llvm::Instruction &instruction) {
  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
    return load->getPointerOperand();
  }
  if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
    return store->getPointerOperand();
  }
  return nullptr;
}

void setPointerOperand(llvm::Instruction &instruction, llvm::Value *pointer) {
  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
    load->setOperand(load->getPointerOperandIndex(), pointer);
    return;
  }
  auto *store = llvm::cast<llvm::StoreInst>(&instruction);
  store->setOperand(store->getPointerOperandIndex(), pointer);
}

} // namespace

NativeRelocationDataSummary materializeNativeFunctionPointerSlots(
    llvm::Module &module, llvm::ArrayRef<NativeFunctionPointerSlot> slots) {
  NativeRelocationDataSummary summary;
  if (slots.empty()) {
    return summary;
  }

  llvm::LLVMContext &context = module.getContext();
  const llvm::DataLayout &layout = module.getDataLayout();
  std::map<uint64_t, llvm::GlobalVariable *> globalForSlot;

  for (const NativeFunctionPointerSlot &slot : slots) {
    if (slot.Width == 0 || slot.Width > 8 || slot.FunctionName.empty()) {
      continue;
    }
    llvm::Function *function = module.getFunction(slot.FunctionName);
    if (function == nullptr) {
      continue;
    }
    auto *integerType =
        llvm::IntegerType::get(context, static_cast<unsigned>(slot.Width * 8));
    llvm::Constant *initializer =
        llvm::ConstantExpr::getPtrToInt(function, integerType);
    if (initializer == nullptr) {
      continue;
    }

    std::string name = slotGlobalName(slot.Address);
    llvm::GlobalVariable *global = module.getNamedGlobal(name);
    if (global == nullptr) {
      global = new llvm::GlobalVariable(
          module, integerType, /*isConstant=*/false,
          llvm::GlobalValue::InternalLinkage, initializer, name);
      global->setAlignment(llvm::Align(1));
      ++summary.SlotsCreated;
    } else if (global->getValueType() == integerType &&
               global->hasInitializer() &&
               global->getInitializer() == initializer) {
      ++summary.SlotsReused;
    } else {
      // A different definition already owns this name; stay conservative.
      continue;
    }
    globalForSlot.emplace(slot.Address, global);
  }

  if (globalForSlot.empty()) {
    return summary;
  }

  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &instruction : llvm::instructions(function)) {
      llvm::Value *pointer = pointerOperand(instruction);
      if (pointer == nullptr) {
        continue;
      }
      std::optional<uint64_t> address = directIntToPtrAddress(pointer);
      if (!address) {
        continue;
      }
      auto slot = globalForSlot.find(*address);
      if (slot == globalForSlot.end()) {
        continue;
      }
      std::optional<uint64_t> size = accessSize(layout, instruction);
      auto *slotType =
          llvm::cast<llvm::IntegerType>(slot->second->getValueType());
      if (!size || *size != slotType->getBitWidth() / 8) {
        continue;
      }
      setPointerOperand(instruction, slot->second);
      ++summary.AccessesRewritten;
    }
  }
  return summary;
}

} // namespace notdec::bin2llvm
