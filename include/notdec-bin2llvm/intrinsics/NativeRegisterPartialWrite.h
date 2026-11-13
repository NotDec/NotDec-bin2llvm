#pragma once

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace llvm {
class CallBase;
class Function;
class GlobalVariable;
class Module;
class Type;
class Value;
} // namespace llvm

namespace notdec::bin2llvm {

// Parsed form of notdec.partial_write.iFULL.iWRITE.
struct NativeRegisterPartialWriteInfo {
  llvm::GlobalVariable *Global = nullptr;
  llvm::Value *Value = nullptr;
  uint32_t FullWidth = 0;
  uint32_t WriteWidth = 0;
  uint64_t BitOffset = 0;
};

bool isNativeRegisterPartialWriteName(llvm::StringRef name);
std::optional<std::pair<uint32_t, uint32_t>>
parseNativeRegisterPartialWriteName(llvm::StringRef name);
std::string nativeRegisterPartialWriteName(uint32_t fullWidth,
                                           uint32_t writeWidth);
llvm::Function *
getOrInsertNativeRegisterPartialWrite(llvm::Module &module, llvm::Type *ptrType,
                                      llvm::Type *valueType,
                                      uint32_t fullWidth,
                                      uint32_t writeWidth);
std::optional<NativeRegisterPartialWriteInfo>
parseNativeRegisterPartialWrite(const llvm::CallBase &call);

} // namespace notdec::bin2llvm
