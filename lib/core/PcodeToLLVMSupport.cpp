#include "PcodeToLLVMInternal.h"

namespace notdec::bin2llvm {

std::string varnodeKey(const VarnodeView &varnode) {
  std::ostringstream os;
  os << varnode.Space << ':' << std::hex << varnode.Offset << ':' << std::dec
     << varnode.Size;
  return os.str();
}

std::string valueName(const VarnodeView &varnode) {
  std::ostringstream os;
  os << varnode.Space << '_' << std::hex << varnode.Offset << '_' << std::dec
     << varnode.Size;
  return os.str();
}

unsigned bitWidth(uint32_t byteSize) {
  return byteSize == 0 ? 1 : byteSize * 8;
}

std::optional<uint32_t>
inferPointerByteSizeFromRegisters(const std::vector<RegisterInfo> &registers) {
  bool hasRsp = false;
  bool hasRip = false;
  bool hasEsp = false;
  bool hasEip = false;
  for (const RegisterInfo &reg : registers) {
    hasRsp |= reg.Name == "RSP" && reg.Size == 8;
    hasRip |= reg.Name == "RIP" && reg.Size == 8;
    hasEsp |= reg.Name == "ESP" && reg.Size == 4;
    hasEip |= reg.Name == "EIP" && reg.Size == 4;
  }
  if (hasRsp && hasRip) {
    return 8;
  }
  if (hasEsp && hasEip) {
    return 4;
  }
  return std::nullopt;
}

void setModuleDataLayoutFromPointerSize(llvm::Module &module,
                                        uint32_t pointerByteSize,
                                        bool isBigEndian) {
  if (!module.getDataLayout().isDefault()) {
    return;
  }
  if (pointerByteSize != 4 && pointerByteSize != 8) {
    return;
  }
  std::string layout = isBigEndian ? "E" : "e";
  uint32_t pointerBits = pointerByteSize * 8;
  layout += "-p:" + std::to_string(pointerBits) + ":" +
            std::to_string(pointerBits);
  module.setDataLayout(layout);
}

bool isX87StackVarnode(const VarnodeView &varnode) {
  if (!varnode.IsRegister || varnode.Size != 10) {
    return false;
  }
  if (varnode.RegisterName && varnode.RegisterName->size() == 3 &&
      (*varnode.RegisterName)[0] == 'S' && (*varnode.RegisterName)[1] == 'T') {
    return true;
  }
  return varnode.Space == "register" && varnode.Offset >= 0x1100 &&
         varnode.Offset < 0x1180;
}

uint32_t x87StackIndex(const VarnodeView &varnode) {
  if (varnode.RegisterName && varnode.RegisterName->size() == 3) {
    unsigned index = (*varnode.RegisterName)[2] - '0';
    if (index <= 7) {
      return index;
    }
  }
  // Architecture register tables can use either 0x1106 or the slot-aligned
  // 0x1100 convention; both give the same index after truncation.
  return static_cast<uint32_t>((varnode.Offset - 0x1100) / 0x10);
}

bool isFpuEnvVarnode(const VarnodeView &varnode) {
  return varnode.IsRegister && varnode.Space == "register" &&
         varnode.Offset >= 0x10a0 && varnode.Offset < 0x10b8;
}

bool touchesX87Stack(const PcodeProgram &program, size_t start, size_t end) {
  for (size_t index = start; index < end; ++index) {
    const PcodeOpView &op = program.Ops[index];
    if ((op.Output && isX87StackVarnode(*op.Output)) ||
        std::any_of(op.Inputs.begin(), op.Inputs.end(), isX87StackVarnode)) {
      return true;
    }
  }
  return false;
}

std::vector<RegisterInfo>
x87WindowRegisters(const std::vector<RegisterInfo> &registers) {
  std::vector<RegisterInfo> result;
  result.reserve(registers.size() + 2);
  bool hasST0 = false;
  bool hasST1 = false;
  for (const RegisterInfo &reg : registers) {
    if (reg.Space == "register" && reg.Size == 10) {
      uint32_t index = 8;
      if (reg.Name.size() == 3 && reg.Name[0] == 'S' && reg.Name[1] == 'T') {
        index = reg.Name[2] - '0';
      } else if (reg.Offset >= 0x1100 && reg.Offset < 0x1180) {
        index = static_cast<uint32_t>((reg.Offset - 0x1100) / 0x10);
      }
      if (index == 0) {
        result.push_back(reg);
        hasST0 = true;
        continue;
      }
      if (index == 1) {
        result.push_back(reg);
        hasST1 = true;
        continue;
      }
      if (index < 8) {
        continue;
      }
    }
    result.push_back(reg);
  }
  if (!hasST0) {
    RegisterInfo st0;
    st0.Space = "register";
    st0.Offset = 0x1100;
    st0.Size = 10;
    st0.Name = "ST0";
    result.push_back(st0);
  }
  if (!hasST1) {
    RegisterInfo st1;
    st1.Space = "register";
    st1.Offset = 0x1110;
    st1.Size = 10;
    st1.Name = "ST1";
    result.push_back(st1);
  }
  return result;
}

VarnodeView x87StackVarnode(const std::vector<RegisterInfo> &registers,
                            uint32_t index) {
  for (const RegisterInfo &reg : registers) {
    if (reg.Space != "register" || reg.Size != 10) {
      continue;
    }
    if (reg.Name == "ST" + std::to_string(index)) {
      VarnodeView varnode;
      varnode.Space = reg.Space;
      varnode.Offset = reg.Offset;
      varnode.Size = reg.Size;
      varnode.IsRegister = true;
      varnode.RegisterName = reg.Name;
      return varnode;
    }
  }
  VarnodeView varnode;
  varnode.Space = "register";
  varnode.Offset = 0x1100 + index * 0x10;
  varnode.Size = 10;
  varnode.IsRegister = true;
  varnode.RegisterName = "ST" + std::to_string(index);
  return varnode;
}

bool isFloatBinaryOpcode(PcodeOpcode opcode) {
  switch (opcode) {
  case PcodeOpcode::FloatAdd:
  case PcodeOpcode::FloatSub:
  case PcodeOpcode::FloatMult:
  case PcodeOpcode::FloatDiv:
    return true;
  default:
    return false;
  }
}

bool isSseFloatWriteMnemonic(llvm::StringRef mnemonic) {
  return llvm::StringSwitch<bool>(mnemonic)
      .Cases({"MOVSD", "MOVSS", "MOVAPD", "MOVAPS", "MOVUPD", "MOVUPS",
              "MOVHPD", "MOVLPD", "MOVHPS", "MOVLPS", "MOVDDUP", "MOVSLDUP",
              "MOVSHDUP", "MOVNTPS", "MOVNTPD"},
             true)
      .Cases({"CVTSI2SD", "CVTSI2SS", "CVTSS2SD", "CVTSD2SS", "CVTDQ2PD",
              "CVTDQ2PS", "CVTPD2DQ", "CVTPS2DQ", "CVTPD2PS", "CVTPS2PD",
              "CVTTPS2DQ", "CVTTPD2DQ"},
             true)
      .Cases({"ADDSD", "ADDSS", "SUBSD", "SUBSS", "MULSD", "MULSS", "DIVSD",
              "DIVSS", "SQRTSD", "SQRTSS", "MAXSD", "MAXSS", "MINSD",
              "MINSS", "RCPSS", "RSQRTSS", "RCPPS", "RSQRTPS", "SQRTPS",
              "SQRTPD", "ADDPS", "ADDPD", "SUBPS", "SUBPD", "MULPS",
              "MULPD", "DIVPS", "DIVPD", "ADDSUBPS", "ADDSUBPD", "HADDPS",
              "HADDPD", "HSUBPS", "HSUBPD"},
             true)
      .Cases({"COMISD", "COMISS", "UCOMISD", "UCOMISS", "CMPSD", "CMPSS",
              "CMPPS", "CMPPD"},
             true)
      .Cases({"ANDPD", "ANDPS", "ANDNPD", "ANDNPS", "ORPD", "ORPS", "XORPD",
              "XORPS"},
             true)
      .Default(false);
}

std::string fpSuffix(llvm::Type *type) {
  if (type->isX86_FP80Ty()) {
    return "f80";
  }
  return type->isDoubleTy() ? "f64" : "f32";
}

std::string popArithName(PcodeOpcode opcode, const VarnodeView &input0) {
  bool reverse = x87StackIndex(input0) != 0;
  switch (opcode) {
  case PcodeOpcode::FloatAdd:
    return "faddp";
  case PcodeOpcode::FloatMult:
    return "fmulp";
  case PcodeOpcode::FloatSub:
    return reverse ? "fsubrp" : "fsubp";
  case PcodeOpcode::FloatDiv:
    return reverse ? "fdivrp" : "fdivp";
  default:
    return "";
  }
}

std::string memArithName(PcodeOpcode opcode, bool memIsInput0) {
  switch (opcode) {
  case PcodeOpcode::FloatAdd:
    return "fadd";
  case PcodeOpcode::FloatMult:
    return "fmul";
  case PcodeOpcode::FloatSub:
    return memIsInput0 ? "fsubr" : "fsub";
  case PcodeOpcode::FloatDiv:
    return memIsInput0 ? "fdivr" : "fdiv";
  default:
    return "";
  }
}

std::string regArithName(PcodeOpcode opcode, const VarnodeView &input0) {
  bool reverse = x87StackIndex(input0) != 0;
  switch (opcode) {
  case PcodeOpcode::FloatAdd:
    return "fadd";
  case PcodeOpcode::FloatSub:
    return reverse ? "fsubr" : "fsub";
  case PcodeOpcode::FloatMult:
    return "fmul";
  case PcodeOpcode::FloatDiv:
    return reverse ? "fdivr" : "fdiv";
  default:
    return "";
  }
}

} // namespace notdec::bin2llvm
