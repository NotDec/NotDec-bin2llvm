#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace notdec::bin2llvm {

enum class PcodeOpcode {
  Copy,
  Load,
  Store,
  IntEqual,
  IntNotEqual,
  IntSLess,
  IntLess,
  IntZExt,
  IntSExt,
  IntAdd,
  IntSub,
  IntSBorrow,
  IntXor,
  IntAnd,
  IntOr,
  IntLeft,
  IntRight,
  IntSRight,
  IntMult,
  Piece,
  Subpiece,
  Popcount,
  Unsupported,
};

// A varnode is the smallest P-Code storage reference.  Keep only the parts the
// first lowering needs: address space, offset, and byte width.
struct VarnodeView {
  std::string Space;
  uint64_t Offset = 0;
  uint32_t Size = 0;
};

// One decoded P-Code op.  Output is absent for STORE and control-flow style
// ops.
struct PcodeOpView {
  PcodeOpcode Opcode = PcodeOpcode::Unsupported;
  std::string OpcodeName;
  std::optional<VarnodeView> Output;
  std::vector<VarnodeView> Inputs;
};

struct PcodeProgram {
  std::vector<PcodeOpView> Ops;
};

const char *pcodeOpcodeName(PcodeOpcode opcode);

} // namespace notdec::bin2llvm
