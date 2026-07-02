#pragma once

#include "notdec-bin2llvm/RegisterStorage.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace notdec::bin2llvm {

enum class PcodeOpcode {
  Copy,
  Load,
  Store,
  Call,
  CallInd,
  CallOther,
  Branch,
  BranchInd,
  CBranch,
  Return,
  IntEqual,
  IntNotEqual,
  IntSLess,
  IntSLessEqual,
  IntLess,
  IntLessEqual,
  IntZExt,
  IntSExt,
  IntAdd,
  IntCarry,
  IntSCarry,
  IntSub,
  IntSBorrow,
  Int2Comp,
  IntNegate,
  IntXor,
  IntAnd,
  IntOr,
  IntLeft,
  IntRight,
  IntSRight,
  IntMult,
  IntDiv,
  IntSDiv,
  IntRem,
  IntSRem,
  BoolNegate,
  BoolAnd,
  BoolOr,
  BoolXor,
  FloatEqual,
  FloatNotEqual,
  FloatLess,
  FloatLessEqual,
  FloatNan,
  FloatAdd,
  FloatDiv,
  FloatMult,
  FloatSub,
  FloatNeg,
  FloatAbs,
  FloatSqrt,
  FloatInt2Float,
  FloatFloat2Float,
  FloatTrunc,
  FloatCeil,
  FloatFloor,
  FloatRound,
  Multiequal,
  Indirect,
  Piece,
  Subpiece,
  Cast,
  PtrAdd,
  PtrSub,
  SegmentOp,
  CpoolRef,
  New,
  Insert,
  Extract,
  Popcount,
  Lzcount,
  Unsupported,
};

// A varnode is the smallest P-Code storage reference.  Register metadata is
// optional because JSON heritage input already carries it, while raw Sleigh
// P-Code has to recover it from the architecture definition.
struct VarnodeView {
  std::string Space;
  uint64_t Offset = 0;
  uint32_t Size = 0;
  bool IsRegister = false;
  std::optional<std::string> RegisterName;
};

// One decoded P-Code op.  Output is absent for STORE and control-flow style
// ops.
struct PcodeOpView {
  uint64_t Address = 0;
  // Size of the machine instruction that emitted this p-code op.  It lets
  // lowering recognize instruction-local effects such as x64 CALL writing the
  // fallthrough return address to the stack.
  uint64_t InstructionSize = 0;
  PcodeOpcode Opcode = PcodeOpcode::Unsupported;
  std::string OpcodeName;
  std::optional<VarnodeView> Output;
  std::vector<VarnodeView> Inputs;
};

struct PcodeProgram {
  std::vector<PcodeOpView> Ops;
  std::vector<RegisterInfo> Registers;
  bool IsBigEndian = false;
};

const char *pcodeOpcodeName(PcodeOpcode opcode);

} // namespace notdec::bin2llvm
