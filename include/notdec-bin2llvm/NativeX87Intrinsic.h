#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {
class Function;
class Module;
class Type;
} // namespace llvm

namespace notdec::bin2llvm {

// x87 FPU stack intrinsic.  Most x87 machine instructions are lowered into
// the ST0/ST1 window model (ST0/ST1 are real register globals the lifted IR
// computes on; see PcodeToLLVM.cpp), and the library only keeps the ST2..ST7
// slots plus the FPU environment.  The remaining intrinsic calls are:
//   notdec.x87.push(x86_fp80)              -- ST2..ST7 shift on fld/fild
//   notdec.x87.pop() -> x86_fp80           -- ST2..ST7 shift on pop
//   notdec.x87.peek(i8) -> x86_fp80        -- read slot ST(i), i >= 2
//   notdec.x87.poke(i8, x86_fp80)          -- write slot ST(i), i >= 2
//   notdec.x87.fprem(x86_fp80, x86_fp80) -> x86_fp80
//   notdec.x87.fnstsw/fnstcw/fldcw/fstenv/fldenv  -- FPU environment
// The library owns its state internally; the lifted IR has no hidden state
// parameter, and no ST2..ST7 globals exist.
bool isNativeX87IntrinsicName(llvm::StringRef name);
llvm::Function *getOrInsertNativeX87Intrinsic(llvm::Module &module,
                                              llvm::StringRef name,
                                              llvm::Type *resultType,
                                              llvm::ArrayRef<llvm::Type *> argTypes,
                                              bool accessesMemory = false);

} // namespace notdec::bin2llvm
