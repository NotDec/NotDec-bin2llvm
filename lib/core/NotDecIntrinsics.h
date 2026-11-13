#pragma once

#include "llvm/ADT/Twine.h"

#include <string>

namespace llvm {
class Function;
class IRBuilderBase;
class Module;
class Type;
class Value;
} // namespace llvm

namespace notdec::bin2llvm {

// Shared helpers for the `notdec.unknown.*` convention: an opaque call stands
// in for "this value is unknown", and the optimizer must not be able to refine
// it.  Both lifting (PcodeToLLVM) and register elimination emit these helpers,
// so the naming and deduplication rules live here instead of in two TUs.
std::string llvmTypeName(llvm::Type *type);

llvm::Function *getOrInsertUnknownValueHelper(llvm::Module &module,
                                              llvm::Type *type);

// Emits `call notdec.unknown.<type>()` through the builder.  The module is
// derived from the builder's insert block; a builder without a block has no
// place to insert the call and falls back to poison.
llvm::Value *unknownValueAt(llvm::IRBuilderBase &builder, llvm::Type *type,
                            llvm::Twine name);

llvm::Value *unknownValueAt(llvm::IRBuilderBase &builder, llvm::Module &module,
                            llvm::Type *type, llvm::Twine name);

} // namespace notdec::bin2llvm
