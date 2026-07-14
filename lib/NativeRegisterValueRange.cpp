#include "notdec-bin2llvm/NativeRegisterValueRange.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/ModRef.h"

#include <string>
#include <utility>

namespace notdec::bin2llvm {
namespace {

constexpr llvm::StringLiteral ExtractPrefix("notdec.reg.extract.");
constexpr llvm::StringLiteral InsertPrefix("notdec.reg.insert.");

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

std::optional<std::pair<uint32_t, uint32_t>>
parseTwoWidths(llvm::StringRef name, llvm::StringRef prefix) {
  if (!name.consume_front(prefix)) {
    return std::nullopt;
  }
  llvm::StringRef fullText;
  llvm::StringRef partText;
  std::tie(fullText, partText) = name.split('.');
  std::optional<uint32_t> fullWidth = parseWidth(fullText);
  std::optional<uint32_t> partWidth = parseWidth(partText);
  if (!fullWidth || !partWidth || *partWidth > *fullWidth) {
    return std::nullopt;
  }
  return std::make_pair(*fullWidth, *partWidth);
}

void markRangeHelperAttributes(llvm::Function &function) {
  function.addFnAttr(llvm::Attribute::NoUnwind);
  function.addFnAttr(llvm::Attribute::WillReturn);
  function.addFnAttr(llvm::Attribute::NoSync);
  function.setMemoryEffects(llvm::MemoryEffects::none());
}

} // namespace

bool isNativeRegisterValueExtractName(llvm::StringRef name) {
  return name.starts_with(ExtractPrefix);
}

bool isNativeRegisterValueInsertName(llvm::StringRef name) {
  return name.starts_with(InsertPrefix);
}

bool isNativeRegisterValueRangeName(llvm::StringRef name) {
  return isNativeRegisterValueExtractName(name) ||
         isNativeRegisterValueInsertName(name);
}

std::optional<std::pair<uint32_t, uint32_t>>
parseNativeRegisterValueExtractName(llvm::StringRef name) {
  return parseTwoWidths(name, ExtractPrefix);
}

std::optional<std::pair<uint32_t, uint32_t>>
parseNativeRegisterValueInsertName(llvm::StringRef name) {
  return parseTwoWidths(name, InsertPrefix);
}

std::string nativeRegisterValueExtractName(uint32_t fullWidth,
                                           uint32_t readWidth) {
  return ("notdec.reg.extract.i" + llvm::Twine(fullWidth) + ".i" +
          llvm::Twine(readWidth))
      .str();
}

std::string nativeRegisterValueInsertName(uint32_t fullWidth,
                                          uint32_t writeWidth) {
  return ("notdec.reg.insert.i" + llvm::Twine(fullWidth) + ".i" +
          llvm::Twine(writeWidth))
      .str();
}

llvm::Function *getOrInsertNativeRegisterValueExtract(llvm::Module &module,
                                                      llvm::Type *fullType,
                                                      llvm::Type *resultType,
                                                      uint32_t fullWidth,
                                                      uint32_t readWidth) {
  llvm::LLVMContext &context = module.getContext();
  llvm::FunctionType *type = llvm::FunctionType::get(
      resultType, {fullType, llvm::Type::getInt64Ty(context)}, false);
  llvm::FunctionCallee callee = module.getOrInsertFunction(
      nativeRegisterValueExtractName(fullWidth, readWidth), type);
  llvm::Function *function = llvm::cast<llvm::Function>(callee.getCallee());
  markRangeHelperAttributes(*function);
  return function;
}

llvm::Function *getOrInsertNativeRegisterValueInsert(llvm::Module &module,
                                                     llvm::Type *fullType,
                                                     llvm::Type *valueType,
                                                     uint32_t fullWidth,
                                                     uint32_t writeWidth) {
  llvm::LLVMContext &context = module.getContext();
  llvm::FunctionType *type = llvm::FunctionType::get(
      fullType, {fullType, valueType, llvm::Type::getInt64Ty(context)}, false);
  llvm::FunctionCallee callee = module.getOrInsertFunction(
      nativeRegisterValueInsertName(fullWidth, writeWidth), type);
  llvm::Function *function = llvm::cast<llvm::Function>(callee.getCallee());
  markRangeHelperAttributes(*function);
  return function;
}

std::optional<NativeRegisterValueExtractInfo>
parseNativeRegisterValueExtract(const llvm::CallBase &call) {
  llvm::Function *callee = call.getCalledFunction();
  if (callee == nullptr || call.arg_size() != 2) {
    return std::nullopt;
  }
  std::optional<std::pair<uint32_t, uint32_t>> widths =
      parseNativeRegisterValueExtractName(callee->getName());
  if (!widths) {
    return std::nullopt;
  }
  auto *offset = llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(1));
  if (offset == nullptr) {
    return std::nullopt;
  }
  NativeRegisterValueExtractInfo info;
  info.FullValue = call.getArgOperand(0);
  info.FullWidth = widths->first;
  info.ReadWidth = widths->second;
  info.BitOffset = offset->getZExtValue();
  if (info.BitOffset + info.ReadWidth > info.FullWidth) {
    return std::nullopt;
  }
  return info;
}

std::optional<NativeRegisterValueInsertInfo>
parseNativeRegisterValueInsert(const llvm::CallBase &call) {
  llvm::Function *callee = call.getCalledFunction();
  if (callee == nullptr || call.arg_size() != 3) {
    return std::nullopt;
  }
  std::optional<std::pair<uint32_t, uint32_t>> widths =
      parseNativeRegisterValueInsertName(callee->getName());
  if (!widths) {
    return std::nullopt;
  }
  auto *offset = llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(2));
  if (offset == nullptr) {
    return std::nullopt;
  }
  NativeRegisterValueInsertInfo info;
  info.Base = call.getArgOperand(0);
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
