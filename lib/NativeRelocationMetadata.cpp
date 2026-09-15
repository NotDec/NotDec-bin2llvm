#include "notdec-bin2llvm/NativeRelocationMetadata.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

#include <optional>

namespace notdec::bin2llvm {
namespace {

constexpr const char *FunctionPointerMetadataName =
    "notdec.relocation.function_pointer";

std::optional<uint64_t> metadataConstantUint64(const llvm::MDOperand &operand) {
  auto *constant = llvm::dyn_cast<llvm::ConstantAsMetadata>(operand.get());
  if (constant == nullptr) {
    return std::nullopt;
  }
  auto *integer = llvm::dyn_cast<llvm::ConstantInt>(constant->getValue());
  if (integer == nullptr || integer->getBitWidth() > 64) {
    return std::nullopt;
  }
  return integer->getZExtValue();
}

llvm::ConstantAsMetadata *uint64Metadata(llvm::LLVMContext &context,
                                         uint64_t value) {
  return llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
      llvm::Type::getInt64Ty(context), value));
}

} // namespace

void attachNativeFunctionPointerMetadata(
    llvm::Module &module, llvm::ArrayRef<NativeFunctionPointerSlot> slots) {
  llvm::NamedMDNode *named =
      module.getOrInsertNamedMetadata(FunctionPointerMetadataName);
  named->clearOperands();
  llvm::LLVMContext &context = module.getContext();
  for (const NativeFunctionPointerSlot &slot : slots) {
    if (slot.Address == 0 || slot.Width == 0 || slot.FunctionName.empty()) {
      continue;
    }
    named->addOperand(llvm::MDNode::get(
        context, {uint64Metadata(context, slot.Address),
                  uint64Metadata(context, slot.Width),
                  llvm::MDString::get(context, slot.FunctionName)}));
  }
}

llvm::SmallVector<NativeFunctionPointerSlotInfo, 16>
readNativeFunctionPointerMetadata(llvm::Module &module) {
  llvm::SmallVector<NativeFunctionPointerSlotInfo, 16> slots;
  llvm::NamedMDNode *named =
      module.getNamedMetadata(FunctionPointerMetadataName);
  if (named == nullptr) {
    return slots;
  }
  for (const llvm::MDNode *node : named->operands()) {
    if (node == nullptr || node->getNumOperands() != 3) {
      continue;
    }
    auto address = metadataConstantUint64(node->getOperand(0));
    auto width = metadataConstantUint64(node->getOperand(1));
    auto *name = llvm::dyn_cast<llvm::MDString>(node->getOperand(2).get());
    if (!address || !width || name == nullptr) {
      continue;
    }
    NativeFunctionPointerSlotInfo info;
    info.Address = *address;
    info.Width = *width;
    info.Target = module.getFunction(name->getString());
    slots.push_back(info);
  }
  return slots;
}

} // namespace notdec::bin2llvm
