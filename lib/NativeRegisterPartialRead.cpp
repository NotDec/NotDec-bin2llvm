#include "notdec-bin2llvm/NativeRegisterPartialRead.h"

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

constexpr llvm::StringLiteral Prefix("notdec.partial_read.");

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

bool isNativeRegisterPartialReadName(llvm::StringRef name) {
  return name.starts_with(Prefix);
}

std::optional<std::pair<uint32_t, uint32_t>>
parseNativeRegisterPartialReadName(llvm::StringRef name) {
  if (!name.consume_front(Prefix)) {
    return std::nullopt;
  }
  llvm::StringRef fullText;
  llvm::StringRef readText;
  std::tie(fullText, readText) = name.split('.');
  std::optional<uint32_t> fullWidth = parseWidth(fullText);
  std::optional<uint32_t> readWidth = parseWidth(readText);
  if (!fullWidth || !readWidth || *readWidth > *fullWidth) {
    return std::nullopt;
  }
  return std::make_pair(*fullWidth, *readWidth);
}

std::string nativeRegisterPartialReadName(uint32_t fullWidth,
                                          uint32_t readWidth) {
  return ("notdec.partial_read.i" + llvm::Twine(fullWidth) + ".i" +
          llvm::Twine(readWidth))
      .str();
}

llvm::Function *
getOrInsertNativeRegisterPartialRead(llvm::Module &module, llvm::Type *ptrType,
                                     llvm::Type *resultType,
                                     uint32_t fullWidth, uint32_t readWidth) {
  llvm::LLVMContext &context = module.getContext();
  llvm::FunctionType *type = llvm::FunctionType::get(
      resultType, {ptrType, llvm::Type::getInt64Ty(context)}, false);
  llvm::FunctionCallee callee = module.getOrInsertFunction(
      nativeRegisterPartialReadName(fullWidth, readWidth), type);
  return llvm::cast<llvm::Function>(callee.getCallee());
}

std::optional<NativeRegisterPartialReadInfo>
parseNativeRegisterPartialRead(const llvm::CallBase &call) {
  llvm::Function *callee = call.getCalledFunction();
  if (callee == nullptr || call.arg_size() != 2) {
    return std::nullopt;
  }
  std::optional<std::pair<uint32_t, uint32_t>> widths =
      parseNativeRegisterPartialReadName(callee->getName());
  if (!widths) {
    return std::nullopt;
  }
  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      call.getArgOperand(0)->stripPointerCasts());
  auto *offset = llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(1));
  if (global == nullptr || offset == nullptr) {
    return std::nullopt;
  }
  NativeRegisterPartialReadInfo info;
  info.Global = global;
  info.FullWidth = widths->first;
  info.ReadWidth = widths->second;
  info.BitOffset = offset->getZExtValue();
  if (info.BitOffset + info.ReadWidth > info.FullWidth) {
    return std::nullopt;
  }
  return info;
}

} // namespace notdec::bin2llvm
