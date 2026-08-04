#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {
class Function;
class Module;
class Type;
} // namespace llvm

namespace notdec::bin2llvm {

// x87 FPU stack intrinsic.  Each x87 machine instruction is lifted into a
// call to one external `notdec.x87.*` function.  The library owns the physical
// FPU stack, so the lifted IR has no ST0..ST7 globals and no state parameter:
// the intrinsic arguments are the explicit assembly operands and the return
// value (fstp/fistp) is what the instruction stores to memory.
bool isNativeX87IntrinsicName(llvm::StringRef name);
llvm::Function *getOrInsertNativeX87Intrinsic(llvm::Module &module,
                                              llvm::StringRef name,
                                              llvm::Type *resultType,
                                              llvm::ArrayRef<llvm::Type *> argTypes,
                                              bool accessesMemory = false);

} // namespace notdec::bin2llvm
