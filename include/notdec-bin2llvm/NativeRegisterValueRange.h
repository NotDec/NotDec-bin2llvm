#pragma once

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace llvm {
class CallBase;
class Function;
class Module;
class Type;
class Value;
} // namespace llvm

namespace notdec::bin2llvm {

// Parsed form of notdec.reg.extract.iFULL.iREAD.
//
// This is the value-level counterpart of notdec.partial_read.*.  It is used
// only inside register SSA rewriting when a bit range has to be materialized
// from an already-available SSA value.  Keeping this as a helper prevents
// temporary range glue from being confused with real program shl/or/trunc
// instructions before final cleanup lowers it.
struct NativeRegisterValueExtractInfo {
  llvm::Value *FullValue = nullptr;
  uint32_t FullWidth = 0;
  uint32_t ReadWidth = 0;
  uint64_t BitOffset = 0;
};

// Parsed form of notdec.reg.insert.iFULL.iWRITE.
//
// The helper means: return Base with Value written into
// [BitOffset, BitOffset + WriteWidth).  It is also used to compose a full value
// from segments, but only when the caller has already proved full bit coverage.
struct NativeRegisterValueInsertInfo {
  llvm::Value *Base = nullptr;
  llvm::Value *Value = nullptr;
  uint32_t FullWidth = 0;
  uint32_t WriteWidth = 0;
  uint64_t BitOffset = 0;
};

bool isNativeRegisterValueExtractName(llvm::StringRef name);
bool isNativeRegisterValueInsertName(llvm::StringRef name);
bool isNativeRegisterValueRangeName(llvm::StringRef name);

std::optional<std::pair<uint32_t, uint32_t>>
parseNativeRegisterValueExtractName(llvm::StringRef name);
std::optional<std::pair<uint32_t, uint32_t>>
parseNativeRegisterValueInsertName(llvm::StringRef name);

std::string nativeRegisterValueExtractName(uint32_t fullWidth,
                                           uint32_t readWidth);
std::string nativeRegisterValueInsertName(uint32_t fullWidth,
                                          uint32_t writeWidth);

llvm::Function *getOrInsertNativeRegisterValueExtract(llvm::Module &module,
                                                      llvm::Type *fullType,
                                                      llvm::Type *resultType,
                                                      uint32_t fullWidth,
                                                      uint32_t readWidth);
llvm::Function *getOrInsertNativeRegisterValueInsert(llvm::Module &module,
                                                     llvm::Type *fullType,
                                                     llvm::Type *valueType,
                                                     uint32_t fullWidth,
                                                     uint32_t writeWidth);

std::optional<NativeRegisterValueExtractInfo>
parseNativeRegisterValueExtract(const llvm::CallBase &call);
std::optional<NativeRegisterValueInsertInfo>
parseNativeRegisterValueInsert(const llvm::CallBase &call);

} // namespace notdec::bin2llvm
