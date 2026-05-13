#include "notdec-bin2llvm/Pcode.h"

namespace notdec::bin2llvm {

const char *pcodeOpcodeName(PcodeOpcode opcode) {
  switch (opcode) {
  case PcodeOpcode::Copy:
    return "COPY";
  case PcodeOpcode::Load:
    return "LOAD";
  case PcodeOpcode::Store:
    return "STORE";
  case PcodeOpcode::IntEqual:
    return "INT_EQUAL";
  case PcodeOpcode::IntNotEqual:
    return "INT_NOTEQUAL";
  case PcodeOpcode::IntSLess:
    return "INT_SLESS";
  case PcodeOpcode::IntLess:
    return "INT_LESS";
  case PcodeOpcode::IntZExt:
    return "INT_ZEXT";
  case PcodeOpcode::IntSExt:
    return "INT_SEXT";
  case PcodeOpcode::IntAdd:
    return "INT_ADD";
  case PcodeOpcode::IntSub:
    return "INT_SUB";
  case PcodeOpcode::IntSBorrow:
    return "INT_SBORROW";
  case PcodeOpcode::IntXor:
    return "INT_XOR";
  case PcodeOpcode::IntAnd:
    return "INT_AND";
  case PcodeOpcode::IntOr:
    return "INT_OR";
  case PcodeOpcode::IntLeft:
    return "INT_LEFT";
  case PcodeOpcode::IntRight:
    return "INT_RIGHT";
  case PcodeOpcode::IntSRight:
    return "INT_SRIGHT";
  case PcodeOpcode::IntMult:
    return "INT_MULT";
  case PcodeOpcode::Piece:
    return "PIECE";
  case PcodeOpcode::Subpiece:
    return "SUBPIECE";
  case PcodeOpcode::Popcount:
    return "POPCOUNT";
  case PcodeOpcode::Unsupported:
    return "UNSUPPORTED";
  }
  return "UNSUPPORTED";
}

} // namespace notdec::bin2llvm
