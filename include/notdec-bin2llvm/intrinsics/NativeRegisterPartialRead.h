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
} // namespace llvm

namespace notdec::bin2llvm {

// Parsed form of notdec.partial_read.iFULL.iREAD.
//
// The helper keeps a narrow register read as a read of only that bit range.
// Without this marker, lifting has to emit a full register load plus
// shift/trunc, and register elimination can mistake unused high bits for a
// real entry-register dependency.
struct NativeRegisterPartialReadInfo {
  llvm::GlobalVariable *Global = nullptr;
  uint32_t FullWidth = 0;
  uint32_t ReadWidth = 0;
  uint64_t BitOffset = 0;
};

bool isNativeRegisterPartialReadName(llvm::StringRef name);
std::optional<std::pair<uint32_t, uint32_t>>
parseNativeRegisterPartialReadName(llvm::StringRef name);
std::string nativeRegisterPartialReadName(uint32_t fullWidth,
                                          uint32_t readWidth);
llvm::Function *
getOrInsertNativeRegisterPartialRead(llvm::Module &module, llvm::Type *ptrType,
                                     llvm::Type *resultType,
                                     uint32_t fullWidth, uint32_t readWidth);
std::optional<NativeRegisterPartialReadInfo>
parseNativeRegisterPartialRead(const llvm::CallBase &call);

} // namespace notdec::bin2llvm
