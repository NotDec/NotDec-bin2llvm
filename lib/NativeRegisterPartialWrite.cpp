#include "notdec-bin2llvm/NativeRegisterPartialWrite.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

#include <string>
#include <utility>

namespace notdec::bin2llvm {
namespace {

constexpr llvm::StringLiteral Prefix("notdec.partial_write.");

std::optional<uint32_t> parseWidth(llvm::StringRef text) {
  if (!text.consume_front("i")) {
    return std::nullopt;
  }
  uint32_t width = 0;
  if (text.empty() || text.getAsInteger(10, width) || width == 0) {
    return std::nullopt;
  }
  return width;
}

} // namespace

bool isNativeRegisterPartialWriteName(llvm::StringRef name) {
  return name.starts_with(Prefix);
}

std::optional<std::pair<uint32_t, uint32_t>>
parseNativeRegisterPartialWriteName(llvm::StringRef name) {
  if (!name.consume_front(Prefix)) {
    return std::nullopt;
  }
  llvm::StringRef fullText;
  llvm::StringRef writeText;
  std::tie(fullText, writeText) = name.split('.');
  std::optional<uint32_t> fullWidth = parseWidth(fullText);
  std::optional<uint32_t> writeWidth = parseWidth(writeText);
  if (!fullWidth || !writeWidth || *writeWidth > *fullWidth) {
    return std::nullopt;
  }
  return std::make_pair(*fullWidth, *writeWidth);
}

std::string nativeRegisterPartialWriteName(uint32_t fullWidth,
                                           uint32_t writeWidth) {
  return ("notdec.partial_write.i" + llvm::Twine(fullWidth) + ".i" +
          llvm::Twine(writeWidth))
      .str();
}

llvm::Function *
getOrInsertNativeRegisterPartialWrite(llvm::Module &module, llvm::Type *ptrType,
                                      llvm::Type *valueType,
                                      uint32_t fullWidth,
                                      uint32_t writeWidth) {
  llvm::LLVMContext &context = module.getContext();
  llvm::FunctionType *type = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context),
      {ptrType, valueType, llvm::Type::getInt64Ty(context)}, false);
  llvm::FunctionCallee callee = module.getOrInsertFunction(
      nativeRegisterPartialWriteName(fullWidth, writeWidth), type);
  return llvm::cast<llvm::Function>(callee.getCallee());
}

std::optional<NativeRegisterPartialWriteInfo>
parseNativeRegisterPartialWrite(const llvm::CallBase &call) {
  llvm::Function *callee = call.getCalledFunction();
  if (callee == nullptr || call.arg_size() != 3) {
    return std::nullopt;
  }
  std::optional<std::pair<uint32_t, uint32_t>> widths =
      parseNativeRegisterPartialWriteName(callee->getName());
  if (!widths) {
    return std::nullopt;
  }
  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      call.getArgOperand(0)->stripPointerCasts());
  auto *offset = llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(2));
  if (global == nullptr || offset == nullptr) {
    return std::nullopt;
  }
  NativeRegisterPartialWriteInfo info;
  info.Global = global;
  info.Value = call.getArgOperand(1);
  info.FullWidth = widths->first;
  info.WriteWidth = widths->second;
  info.BitOffset = offset->getZExtValue();
  if (info.BitOffset + info.WriteWidth > info.FullWidth) {
    return std::nullopt;
  }
  return info;
}

} // namespace notdec::bin2llvm
