#include "PcodeToLLVMInternal.h"

namespace notdec::bin2llvm {

std::optional<X87WindowSpec>
  PcodeLowerer::classifyX87ByMnemonic(const PcodeProgram &program, size_t start,
                        size_t end, const std::string &mnemonic) {
    X87WindowSpec spec;
    spec.Start = start;
    spec.End = end;

    // fnstsw/fnstcw: read the FPU status/control word from the library and
    // write it to the p-code target (AX register or a memory store).
    if (mnemonic == "FNSTSW" || mnemonic == "FNSTCW") {
      spec.IntrinsicName = mnemonic == "FNSTSW" ? "notdec.x87.fnstsw"
                                                : "notdec.x87.fnstcw";
      spec.ResultType = intType(2);
      bool hasTarget = false;
      for (size_t index = start; index < end; ++index) {
        const PcodeOpView &op = program.Ops[index];
        if (op.Opcode == PcodeOpcode::Copy && op.Output &&
            op.Output->IsRegister && op.Output->Size == 2) {
          spec.ResultRegister = *op.Output;
          hasTarget = true;
          break;
        }
      }
      if (!hasTarget) {
        for (size_t index = start; index < end; ++index) {
          const PcodeOpView &op = program.Ops[index];
          if (op.Opcode == PcodeOpcode::Store && op.Inputs.size() == 3) {
            spec.StoreAddress = op.Inputs[1];
            spec.StoreValueSize = 2;
            hasTarget = true;
            break;
          }
        }
      }
      if (!hasTarget) {
        return std::nullopt;
      }
      return spec;
    }

    // fldcw: load the FPU control word from memory into the library.
    if (mnemonic == "FLDCW") {
      const PcodeOpView *load = nullptr;
      for (size_t index = start; index < end; ++index) {
        const PcodeOpView &op = program.Ops[index];
        if (op.Opcode == PcodeOpcode::Load && op.Output &&
            op.Output->Size == 2) {
          load = &op;
          break;
        }
      }
      if (load == nullptr) {
        return std::nullopt;
      }
      spec.IntrinsicName = "notdec.x87.fldcw";
      spec.Args = {*load->Output};
      spec.ArgTypes = {intType(2)};
      return spec;
    }

    // fstenv/fnstenv: save the whole FPU environment to memory.  The library
    // writes the real 28-byte x86-64 layout; the p-code's per-field STOREs
    // are suppressed.  The base address is the STORE whose address is not
    // produced inside the group (the +0 field).
    if (mnemonic == "FSTENV" || mnemonic == "FNSTENV") {
      const VarnodeView *base = nullptr;
      for (size_t index = start; index < end; ++index) {
        const PcodeOpView &op = program.Ops[index];
        if (op.Opcode != PcodeOpcode::Store || op.Inputs.size() != 3) {
          continue;
        }
        if (findProducer(program, start, end, op.Inputs[1]) == nullptr) {
          base = &op.Inputs[1];
          break;
        }
      }
      if (base == nullptr) {
        return std::nullopt;
      }
      spec.IntrinsicName = "notdec.x87.fstenv";
      spec.Args = {*base};
      spec.ArgTypes = {llvm::PointerType::get(Context, 0)};
      spec.AccessesMemory = true;
      return spec;
    }

    // fldenv: load the whole FPU environment from memory into the library.
    if (mnemonic == "FLDENV") {
      const VarnodeView *base = nullptr;
      for (size_t index = start; index < end; ++index) {
        const PcodeOpView &op = program.Ops[index];
        if (op.Opcode != PcodeOpcode::Load || op.Inputs.size() < 2) {
          continue;
        }
        if (findProducer(program, start, end, op.Inputs[1]) == nullptr) {
          base = &op.Inputs[1];
          break;
        }
      }
      if (base == nullptr) {
        return std::nullopt;
      }
      spec.IntrinsicName = "notdec.x87.fldenv";
      spec.Args = {*base};
      spec.ArgTypes = {llvm::PointerType::get(Context, 0)};
      spec.SuppressLoads = true;
      spec.AccessesMemory = true;
      return spec;
    }

    if (!touchesX87Stack(program, start, end)) {
      return std::nullopt;
    }

    // fabs/fchs: ST0 = |ST0| / ST0 = -ST0, no explicit operands.
    if (mnemonic == "FABS" || mnemonic == "FCHS") {
      spec.Kind = X87WindowKind::Unary;
      spec.Unary = mnemonic == "FABS" ? X87WindowSpec::UnaryKind::Abs
                                      : X87WindowSpec::UnaryKind::Chs;
      return spec;
    }

    // fldz / fld1: push an exact constant.
    if (mnemonic == "FLDZ" || mnemonic == "FLD1") {
      spec.Kind = X87WindowKind::Push;
      llvm::APFloat apFloat(mnemonic == "FLDZ" ? 0.0 : 1.0);
      bool ignored;
      apFloat.convert(llvm::APFloat::x87DoubleExtended(),
                      llvm::APFloat::rmNearestTiesToEven, &ignored);
      spec.PushConstant = llvm::ConstantFP::get(Context, apFloat);
      return spec;
    }

    // fsqrt: ST0 = sqrt(ST0).
    if (mnemonic == "FSQRT") {
      if (findStackWrite(program, start, end, PcodeOpcode::FloatSqrt, 0) ==
          nullptr) {
        return std::nullopt;
      }
      spec.Kind = X87WindowKind::Unary;
      spec.Unary = X87WindowSpec::UnaryKind::Sqrt;
      return spec;
    }

    // fld: push memory (f32/f64/f80) or duplicate st(i).
    if (mnemonic == "FLD") {
      spec.Kind = X87WindowKind::Push;
      // f32/f64 loads write ST0 = FLOAT2FLOAT(mem).
      const PcodeOpView *write = findStackWrite(
          program, start, end, PcodeOpcode::FloatFloat2Float, 0);
      if (write != nullptr && write->Inputs.size() == 1 &&
          !isX87StackVarnode(write->Inputs[0])) {
        llvm::Type *fpType = floatType(write->Inputs[0].Size);
        if (fpType == nullptr) {
          return std::nullopt;
        }
        spec.MemValue = write->Inputs[0];
        spec.MemValueType = fpType;
        return spec;
      }
      // f80 loads and fld %st(i) both write ST0 = COPY(unique); the producer
      // of the unique temp tells them apart.
      write = findStackWrite(program, start, end, PcodeOpcode::Copy, 0);
      if (write == nullptr || write->Inputs.size() != 1 ||
          isX87StackVarnode(write->Inputs[0])) {
        return std::nullopt;
      }
      const VarnodeView &copied = write->Inputs[0];
      const PcodeOpView *producer = findProducer(program, start, end, copied);
      if (producer != nullptr && producer->Opcode == PcodeOpcode::Copy &&
          producer->Inputs.size() == 1 &&
          isX87StackVarnode(producer->Inputs[0])) {
        spec.SlotIndex = x87StackIndex(producer->Inputs[0]);
        return spec;
      }
      llvm::Type *fpType = floatType(copied.Size);
      if (fpType == nullptr) {
        return std::nullopt;
      }
      spec.MemValue = copied;
      spec.MemValueType = fpType;
      return spec;
    }

    // fstp / fistp: store ST0 to memory or to st(i).  The mnemonic decides
    // the instruction; the STORE only supplies the target address and the
    // stored value width.  Ghidra models FISTP as round(ST0)->trunc and FSTP
    // as FLOAT2FLOAT/COPY of ST0, but the conversion is re-emitted from the
    // loaded ST0 below.
    if (mnemonic == "FSTP" || mnemonic == "FISTP") {
      const PcodeOpView *storeOp = nullptr;
      for (size_t index = start; index < end; ++index) {
        if (program.Ops[index].Opcode == PcodeOpcode::Store) {
          storeOp = &program.Ops[index];
          break;
        }
      }
      if (storeOp != nullptr && storeOp->Inputs.size() == 3) {
        const VarnodeView &storedValue = storeOp->Inputs[2];
        if (mnemonic == "FISTP") {
          if (storedValue.Size != 2 && storedValue.Size != 4 &&
              storedValue.Size != 8) {
            return std::nullopt;
          }
          spec.Kind = X87WindowKind::PopStore;
          spec.IsInt = true;
        } else {
          llvm::Type *fpType = floatType(storedValue.Size);
          if (fpType == nullptr) {
            return std::nullopt;
          }
          spec.Kind = X87WindowKind::PopStore;
          spec.MemValueType = fpType;
        }
        spec.StoreAddress = storeOp->Inputs[1];
        spec.StoreValueSize = storedValue.Size;
        return spec;
      }
      if (mnemonic == "FISTP") {
        return std::nullopt;
      }
      // Register form: fstp %st(i).  The p-code writes ST(i) = COPY(ST0)
      // before the rolling pop; st(0) is a plain pop and Ghidra may emit a
      // self-copy that gets compacted away, so default to slot 0.
      if (std::optional<uint32_t> index =
              stIndexFromSt0Copy(program, start, end)) {
        spec.SlotIndex = *index;
      } else {
        spec.SlotIndex = 0;
      }
      spec.Kind = X87WindowKind::PopSlot;
      return spec;
    }

    // fst %st(i): ST(i) = ST0, no pop.
    if (mnemonic == "FST") {
      if (std::optional<uint32_t> index =
              stIndexFromSt0Copy(program, start, end)) {
        spec.Kind = X87WindowKind::StoreSlot;
        spec.SlotIndex = *index;
        return spec;
      }
      return std::nullopt;
    }

    // fxch %st(i): swap ST0 and st(i).
    if (mnemonic == "FXCH") {
      if (std::optional<uint32_t> index =
              stIndexFromSt0WriteCopy(program, start, end)) {
        spec.Kind = X87WindowKind::Fxch;
        spec.SlotIndex = *index;
        return spec;
      }
      return std::nullopt;
    }

    // fild: integer load pushes ST0 = INT2FLOAT(mem).
    if (mnemonic == "FILD") {
      const PcodeOpView *write =
          findStackWrite(program, start, end, PcodeOpcode::FloatInt2Float, 0);
      if (write == nullptr || write->Inputs.size() != 1) {
        return std::nullopt;
      }
      const VarnodeView &input = write->Inputs[0];
      if (input.Size != 2 && input.Size != 4 && input.Size != 8) {
        return std::nullopt;
      }
      spec.Kind = X87WindowKind::Push;
      spec.IsInt = true;
      spec.MemValue = input;
      return spec;
    }

    // fcomi/fcomip/fucomi/fucomip: compare ST0 with st(i) into EFLAGS.
    // FCOMI/FUCOMI do not pop; FCOMIP/FUCOMIP do.  The st(i) operand is
    // recovered from the FLOAT_EQUAL/FLOAT_LESS inputs.
    if (mnemonic == "FCOMI" || mnemonic == "FCOMIP" ||
        mnemonic == "FUCOMI" || mnemonic == "FUCOMIP") {
      std::optional<uint32_t> index;
      for (size_t opIndex = start; opIndex < end; ++opIndex) {
        const PcodeOpView &op = program.Ops[opIndex];
        if ((op.Opcode == PcodeOpcode::FloatEqual ||
             op.Opcode == PcodeOpcode::FloatLess) &&
            op.Inputs.size() == 2 && isX87StackVarnode(op.Inputs[0]) &&
            isX87StackVarnode(op.Inputs[1])) {
          index = x87StackIndex(op.Inputs[0]) == 0
                      ? x87StackIndex(op.Inputs[1])
                      : x87StackIndex(op.Inputs[0]);
          break;
        }
      }
      if (!index) {
        return std::nullopt;
      }
      spec.Kind = X87WindowKind::Compare;
      spec.SlotIndex = *index;
      spec.Unordered = mnemonic == "FUCOMI" || mnemonic == "FUCOMIP";
      spec.Pops = mnemonic == "FCOMIP" || mnemonic == "FUCOMIP";
      return spec;
    }

    // fprem: partial-remainder loop folded into one library call.  Ghidra
    // models it as a single-shot exact remainder (FLOAT_DIV/FLOAT_MULT/
    // FLOAT_SUB) and never sets FSW C2; the library computes ST0 =
    // remainder(ST0, ST1) exactly and keeps C2 clear, so the lifted
    // "fprem; fnstsw; test ah,4; jne" retry loop runs once and exits.
    if (mnemonic == "FPREM") {
      if (findStackWrite(program, start, end, PcodeOpcode::FloatSub, 0) ==
          nullptr) {
        return std::nullopt;
      }
      spec.IntrinsicName = "notdec.x87.fprem";
      spec.Args = {StVarnodes[0], StVarnodes[1]};
      spec.ArgTypes = {floatType(10), floatType(10)};
      spec.ResultType = floatType(10);
      spec.StoreResultToST0 = true;
      return spec;
    }

    // Pop arithmetic (faddp/fsubp/fmulp/fdivp/fsubrp/fdivrp):
    // ST(i) = op(ST0, ST(i)) [or the reversed order from the p-code], pop.
    if (mnemonic == "FADDP" || mnemonic == "FSUBP" || mnemonic == "FMULP" ||
        mnemonic == "FDIVP" || mnemonic == "FSUBRP" ||
        mnemonic == "FDIVRP") {
      for (size_t index = start; index < end; ++index) {
        const PcodeOpView &op = program.Ops[index];
        if (!op.Output || !isX87StackVarnode(*op.Output) ||
            !isFloatBinaryOpcode(op.Opcode) || op.Inputs.size() != 2 ||
            !isX87StackVarnode(op.Inputs[0]) ||
            !isX87StackVarnode(op.Inputs[1])) {
          continue;
        }
        uint32_t outputIndex = x87StackIndex(*op.Output);
        if (outputIndex == 0) {
          return std::nullopt;
        }
        spec.ArithOpcode = op.Opcode;
        spec.ST0IsInput0 = x87StackIndex(op.Inputs[0]) == 0;
        if (outputIndex == 1) {
          spec.Kind = X87WindowKind::PopArith;
        } else {
          spec.Kind = X87WindowKind::PopArithSlot;
          spec.SlotIndex = outputIndex;
        }
        return spec;
      }
      return std::nullopt;
    }

    // Register or memory arithmetic on ST0.  The operand order (who is
    // input0) is taken from the p-code, so fsubr/fdivr need no special
    // handling here.
    if (mnemonic == "FADD" || mnemonic == "FSUB" || mnemonic == "FMUL" ||
        mnemonic == "FDIV" || mnemonic == "FSUBR" || mnemonic == "FDIVR") {
      const PcodeOpView *write = nullptr;
      for (size_t index = start; index < end; ++index) {
        const PcodeOpView &op = program.Ops[index];
        if (op.Output && isX87StackVarnode(*op.Output) &&
            isFloatBinaryOpcode(op.Opcode) && op.Inputs.size() == 2) {
          write = &op;
          break;
        }
      }
      if (write == nullptr || x87StackIndex(*write->Output) != 0) {
        return std::nullopt;
      }
      const VarnodeView &input0 = write->Inputs[0];
      const VarnodeView &input1 = write->Inputs[1];
      bool input0IsST = isX87StackVarnode(input0);
      bool input1IsST = isX87StackVarnode(input1);
      if (input0IsST && input1IsST) {
        // Register arithmetic: ST0 = op(ST0, st(i)) or op(st(i), ST0).
        spec.Kind = X87WindowKind::RegArith;
        spec.ArithOpcode = write->Opcode;
        spec.ST0IsInput0 = x87StackIndex(input0) == 0;
        spec.SlotIndex = x87StackIndex(input0) == 0
                             ? x87StackIndex(input1)
                             : x87StackIndex(input0);
        return spec;
      }
      if (input0IsST == input1IsST) {
        return std::nullopt;
      }
      // Memory arithmetic on ST0.  The memory operand is a FLOAT2FLOAT
      // product; recover the raw memory value to pass as the argument.
      const VarnodeView &memOperand = input0IsST ? input1 : input0;
      const PcodeOpView *producer =
          findProducer(program, start, end, memOperand);
      if (producer == nullptr ||
          producer->Opcode != PcodeOpcode::FloatFloat2Float ||
          producer->Inputs.size() != 1 ||
          isX87StackVarnode(producer->Inputs[0])) {
        return std::nullopt;
      }
      const VarnodeView &rawMem = producer->Inputs[0];
      llvm::Type *fpType = floatType(rawMem.Size);
      if (fpType == nullptr) {
        return std::nullopt;
      }
      spec.Kind = X87WindowKind::MemArith;
      spec.ArithOpcode = write->Opcode;
      spec.ST0IsInput0 = input0IsST;
      spec.MemValue = rawMem;
      spec.MemValueType = fpType;
      return spec;
    }

    return std::nullopt;
  }

std::optional<X87WindowSpec>
  PcodeLowerer::classifyX87ByShape(const PcodeProgram &program, size_t start, size_t end) {
    bool hasStackVarnode = false;
    for (size_t index = start; index < end; ++index) {
      const PcodeOpView &op = program.Ops[index];
      if ((op.Output && isX87StackVarnode(*op.Output)) ||
          std::any_of(op.Inputs.begin(), op.Inputs.end(),
                      isX87StackVarnode)) {
        hasStackVarnode = true;
        break;
      }
    }
    if (!hasStackVarnode) {
      return std::nullopt;
    }

    const PcodeOpView *storeOp = nullptr;
    const PcodeOpView *stackWriteOp = nullptr;
    for (size_t index = start; index < end; ++index) {
      const PcodeOpView &op = program.Ops[index];
      if (op.Opcode == PcodeOpcode::Store) {
        storeOp = &op;
        continue;
      }
      if (!op.Output || !isX87StackVarnode(*op.Output)) {
        continue;
      }
      switch (op.Opcode) {
      case PcodeOpcode::FloatInt2Float:
      case PcodeOpcode::FloatFloat2Float:
      case PcodeOpcode::FloatAdd:
      case PcodeOpcode::FloatSub:
      case PcodeOpcode::FloatMult:
      case PcodeOpcode::FloatDiv:
        stackWriteOp = &op;
        break;
      default:
        break;
      }
      if (stackWriteOp != nullptr) {
        break;
      }
    }
    if (stackWriteOp == nullptr && storeOp == nullptr) {
      return std::nullopt;
    }

    X87WindowSpec spec;
    spec.Start = start;
    spec.End = end;

    // fstp / fistp: the stored value is produced from ST0.  STORE inputs are
    // (space selector, address, value).
    if (storeOp != nullptr && storeOp->Inputs.size() == 3) {
      const VarnodeView &storedValue = storeOp->Inputs[2];
      const PcodeOpView *producer =
          findProducer(program, start, end, storedValue);
      if (producer != nullptr && producer->Inputs.size() == 1 &&
          isX87StackVarnode(producer->Inputs[0]) &&
          x87StackIndex(producer->Inputs[0]) == 0) {
        if (producer->Opcode == PcodeOpcode::FloatFloat2Float) {
          llvm::Type *fpType = floatType(producer->Output->Size);
          if (fpType == nullptr) {
            return std::nullopt;
          }
          spec.Kind = X87WindowKind::PopStore;
          spec.MemValueType = fpType;
        } else if (producer->Opcode == PcodeOpcode::FloatTrunc) {
          spec.Kind = X87WindowKind::PopStore;
          spec.IsInt = true;
        } else {
          return std::nullopt;
        }
        spec.StoreAddress = storeOp->Inputs[1];
        spec.StoreValueSize = producer->Output->Size;
        return spec;
      }
    }

    if (stackWriteOp == nullptr) {
      return std::nullopt;
    }

    const PcodeOpView &writeOp = *stackWriteOp;
    // push integer (fild): ST0 = INT2FLOAT(mem)
    if (writeOp.Opcode == PcodeOpcode::FloatInt2Float) {
      if (writeOp.Inputs.size() != 1) {
        return std::nullopt;
      }
      const VarnodeView &input = writeOp.Inputs[0];
      if (input.Size != 2 && input.Size != 4 && input.Size != 8) {
        return std::nullopt;
      }
      spec.Kind = X87WindowKind::Push;
      spec.IsInt = true;
      spec.MemValue = input;
      return spec;
    }

    // push float (fld): ST0 = FLOAT2FLOAT(mem)
    if (writeOp.Opcode == PcodeOpcode::FloatFloat2Float) {
      if (writeOp.Inputs.size() != 1) {
        return std::nullopt;
      }
      const VarnodeView &input = writeOp.Inputs[0];
      if (isX87StackVarnode(input)) {
        return std::nullopt;  // fld %st(i) not covered without a mnemonic
      }
      llvm::Type *fpType = floatType(input.Size);
      if (fpType == nullptr) {
        return std::nullopt;
      }
      spec.Kind = X87WindowKind::Push;
      spec.MemValue = input;
      spec.MemValueType = fpType;
      return spec;
    }

    // float arithmetic on the stack
    if (isFloatBinaryOpcode(writeOp.Opcode)) {
      if (writeOp.Inputs.size() != 2) {
        return std::nullopt;
      }
      const VarnodeView &input0 = writeOp.Inputs[0];
      const VarnodeView &input1 = writeOp.Inputs[1];
      bool input0IsST = isX87StackVarnode(input0);
      bool input1IsST = isX87StackVarnode(input1);
      if (input0IsST && input1IsST) {
        // Pop arithmetic: ST(i) = op(ST0, ST(i)) with the operand order from
        // the p-code, then pop.
        uint32_t outputIndex = x87StackIndex(*writeOp.Output);
        if (outputIndex == 0) {
          return std::nullopt;
        }
        spec.ArithOpcode = writeOp.Opcode;
        spec.ST0IsInput0 = x87StackIndex(input0) == 0;
        if (outputIndex == 1) {
          spec.Kind = X87WindowKind::PopArith;
        } else {
          spec.Kind = X87WindowKind::PopArithSlot;
          spec.SlotIndex = outputIndex;
        }
        return spec;
      }
      if (x87StackIndex(*writeOp.Output) != 0 ||
          input0IsST == input1IsST) {
        return std::nullopt;
      }
      // Memory arithmetic on ST0.  The memory operand is a FLOAT2FLOAT
      // product; recover the raw memory value to pass as the argument.
      const VarnodeView &memOperand = input0IsST ? input1 : input0;
      const PcodeOpView *producer =
          findProducer(program, start, end, memOperand);
      if (producer == nullptr ||
          producer->Opcode != PcodeOpcode::FloatFloat2Float ||
          producer->Inputs.size() != 1 ||
          isX87StackVarnode(producer->Inputs[0])) {
        return std::nullopt;
      }
      const VarnodeView &rawMem = producer->Inputs[0];
      llvm::Type *fpType = floatType(rawMem.Size);
      if (fpType == nullptr) {
        return std::nullopt;
      }
      spec.Kind = X87WindowKind::MemArith;
      spec.ArithOpcode = writeOp.Opcode;
      spec.ST0IsInput0 = input0IsST;
      spec.MemValue = rawMem;
      spec.MemValueType = fpType;
      return spec;
    }

    return std::nullopt;
  }

llvm::Value *PcodeLowerer::toIntrinsicArg(llvm::Value *value, llvm::Type *targetType) {
    if (value->getType() == targetType) {
      return value;
    }
    if (targetType->isFloatingPointTy()) {
      return Builder.CreateBitCast(value, targetType);
    }
    if (targetType->isPointerTy()) {
      return Builder.CreateIntToPtr(value, targetType);
    }
    return Builder.CreateZExtOrTrunc(value, targetType);
  }

llvm::Type *PcodeLowerer::x87Fp80() { return floatType(10); }


llvm::Value *PcodeLowerer::readX87ST(uint32_t index, bool windowShift ) {
    llvm::Value *bits = read(StVarnodes[index]);
    if (windowShift) {
      // 窗口移位读：push/pop/fxch 的槽位搬移（如 push 的 ST1 <- 旧 ST0），
      // 值只在窗口内部转移，不是对 ST0/ST1 值的真实消费。register summary
      // 看到这个标记就不会把它当成对 callee 返回值的需求。
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(bits)) {
        load->setMetadata("notdec.x87.window.shift",
                          llvm::MDNode::get(Context, {}));
      }
    }
    return Builder.CreateBitCast(bits, x87Fp80(),
                                 "st" + std::to_string(index));
  }

void PcodeLowerer::writeX87ST(uint32_t index, llvm::Value *value) {
    llvm::Value *bits = Builder.CreateBitCast(value, intType(10),
                                              "st" + std::to_string(index) +
                                                  ".bits");
    write(StVarnodes[index], bits);
  }

void PcodeLowerer::x87WindowPush(llvm::Value *value) {
    if (!X87Depth) {
      llvm::Value *oldST0 = readX87ST(0, true);
      llvm::Value *oldST1 = readX87ST(1, true);
      writeX87ST(0, value);
      writeX87ST(1, oldST0);
      llvm::Function *push = getOrInsertNativeX87Intrinsic(
          Module, "notdec.x87.push", llvm::Type::getVoidTy(Context),
          {x87Fp80()}, false);
      Builder.CreateCall(push, {oldST1});
      return;
    }
    unsigned depth = *X87Depth;
    llvm::Value *oldST0 = depth >= 1 ? readX87ST(0, true) : nullptr;
    llvm::Value *oldST1 = depth >= 2 ? readX87ST(1, true) : nullptr;
    if (oldST1 != nullptr) {
      llvm::Function *push = getOrInsertNativeX87Intrinsic(
          Module, "notdec.x87.push", llvm::Type::getVoidTy(Context),
          {x87Fp80()}, false);
      Builder.CreateCall(push, {oldST1});
    }
    if (oldST0 != nullptr) {
      writeX87ST(1, oldST0);
    }
    writeX87ST(0, value);
    ++*X87Depth;
  }

void PcodeLowerer::x87WindowPop() {
    if (!X87Depth) {
      llvm::Value *oldST1 = readX87ST(1, true);
      writeX87ST(0, oldST1);
      x87PopIntoST1();
      return;
    }
    if (*X87Depth >= 2) {
      writeX87ST(0, readX87ST(1, true));
    }
    x87PopIntoST1();
    --*X87Depth;
  }

void PcodeLowerer::x87PopIntoST1() {
    if (X87Depth && *X87Depth < 3) {
      return;  // 新 ST1 是空槽：没有值从库里出来
    }
    llvm::Function *pop = getOrInsertNativeX87Intrinsic(
        Module, "notdec.x87.pop", x87Fp80(), {}, false);
    llvm::Value *next = Builder.CreateCall(pop, {});
    writeX87ST(1, next);
  }

llvm::Value *PcodeLowerer::x87Peek(uint32_t index) {
    llvm::Function *peek = getOrInsertNativeX87Intrinsic(
        Module, "notdec.x87.peek", x87Fp80(), {intType(1)}, false);
    return Builder.CreateCall(peek,
                              {llvm::ConstantInt::get(intType(1), index)});
  }

void PcodeLowerer::x87Poke(uint32_t index, llvm::Value *value) {
    llvm::Function *poke = getOrInsertNativeX87Intrinsic(
        Module, "notdec.x87.poke", llvm::Type::getVoidTy(Context),
        {intType(1), x87Fp80()}, false);
    Builder.CreateCall(poke,
                       {llvm::ConstantInt::get(intType(1), index), value});
  }

llvm::Value *PcodeLowerer::readX87Slot(uint32_t index) {
    if (index == 0) {
      return readX87ST(0);
    }
    if (index == 1) {
      return readX87ST(1);
    }
    return x87Peek(index);
  }

llvm::Value *PcodeLowerer::x87FloatArith(PcodeOpcode opcode, llvm::Value *lhs,
                             llvm::Value *rhs) {
    switch (opcode) {
    case PcodeOpcode::FloatAdd:
      return Builder.CreateFAdd(lhs, rhs);
    case PcodeOpcode::FloatSub:
      return Builder.CreateFSub(lhs, rhs);
    case PcodeOpcode::FloatMult:
      return Builder.CreateFMul(lhs, rhs);
    case PcodeOpcode::FloatDiv:
      return Builder.CreateFDiv(lhs, rhs);
    default:
      return nullptr;
    }
  }

bool PcodeLowerer::lowerX87Library(const X87WindowSpec &spec, std::string &errorMessage) {
    llvm::Function *intrinsic = getOrInsertNativeX87Intrinsic(
        Module, spec.IntrinsicName, spec.ResultType, spec.ArgTypes,
        spec.AccessesMemory);
    std::vector<llvm::Value *> args;
    args.reserve(spec.Args.size());
    for (size_t index = 0; index < spec.Args.size(); ++index) {
      args.push_back(
          toIntrinsicArg(read(spec.Args[index]), spec.ArgTypes[index]));
    }
    llvm::Value *result = Builder.CreateCall(intrinsic, args);

    if (spec.StoreResultToST0) {
      writeX87ST(0, result);
      return true;
    }

    if (spec.ResultRegister) {
      write(*spec.ResultRegister, result);
    }

    if (spec.StoreAddress && spec.StoreValueSize) {
      llvm::Value *stored = result;
      if (stored->getType()->isFloatingPointTy()) {
        stored = Builder.CreateBitCast(stored, intType(*spec.StoreValueSize));
      } else {
        stored = Builder.CreateZExtOrTrunc(stored,
                                           intType(*spec.StoreValueSize));
      }
      auto *store = Builder.CreateStore(
          stored, memoryPointer(resize(read(*spec.StoreAddress),
                                       pointerByteSize())));
      store->setAlignment(llvm::Align(1));
    }
    return true;
  }

bool PcodeLowerer::lowerX87Push(const X87WindowSpec &spec, std::string &errorMessage) {
    llvm::Value *value = nullptr;
    if (spec.PushConstant != nullptr) {
      value = spec.PushConstant;
    } else if (spec.IsInt) {
      value = Builder.CreateSIToFP(read(*spec.MemValue), x87Fp80());
    } else if (spec.MemValue) {
      // The memory operand is stored as integer bits; convert to the source
      // float type and widen to x86_fp80.
      llvm::Value *raw = read(*spec.MemValue);
      llvm::Value *fp = Builder.CreateBitCast(raw, spec.MemValueType);
      if (spec.MemValueType->isX86_FP80Ty()) {
        value = fp;
      } else {
        value = Builder.CreateFPExt(fp, x87Fp80());
      }
    } else {
      value = readX87Slot(spec.SlotIndex);
    }
    if (value == nullptr) {
      return false;
    }
    x87WindowPush(value);
    return true;
  }

bool PcodeLowerer::lowerX87PopStore(const X87WindowSpec &spec,
                        std::string &errorMessage) {
    llvm::Value *value = readX87ST(0);
    llvm::Value *stored = nullptr;
    if (spec.IsInt) {
      stored = Builder.CreateFPToSI(value, intType(*spec.StoreValueSize));
    } else if (spec.MemValueType->isX86_FP80Ty()) {
      stored = Builder.CreateBitCast(value, intType(*spec.StoreValueSize));
    } else {
      llvm::Value *fp = Builder.CreateFPTrunc(value, spec.MemValueType);
      stored = Builder.CreateBitCast(fp, intType(*spec.StoreValueSize));
    }
    auto *store = Builder.CreateStore(
        stored,
        memoryPointer(resize(read(*spec.StoreAddress), pointerByteSize())));
    store->setAlignment(llvm::Align(1));
    x87WindowPop();
    return true;
  }

bool PcodeLowerer::lowerX87PopSlot(const X87WindowSpec &spec, std::string &errorMessage) {
    if (spec.SlotIndex == 1) {
      // fstp %st(1): ST1 <- ST0, then pop leaves ST0 = old ST1 = old ST0.
      x87PopIntoST1();
      if (X87Depth) {
        --*X87Depth;
      }
      return true;
    }
    if (spec.SlotIndex >= 2) {
      // ST(i) <- ST0; the pop then shifts the poked value into the window.
      x87Poke(spec.SlotIndex, readX87ST(0));
    }
    x87WindowPop();
    return true;
  }

bool PcodeLowerer::lowerX87StoreSlot(const X87WindowSpec &spec,
                         std::string &errorMessage) {
    if (spec.SlotIndex == 0) {
      return true;  // fst %st(0) is a no-op
    }
    if (spec.SlotIndex == 1) {
      writeX87ST(1, readX87ST(0));
      return true;
    }
    x87Poke(spec.SlotIndex, readX87ST(0));
    return true;
  }

bool PcodeLowerer::lowerX87PopArith(const X87WindowSpec &spec,
                        std::string &errorMessage) {
    llvm::Value *st0 = readX87ST(0);
    if (spec.Kind == X87WindowKind::PopArith) {
      llvm::Value *st1 = readX87ST(1);
      llvm::Value *result = spec.ST0IsInput0
                                ? x87FloatArith(spec.ArithOpcode, st0, st1)
                                : x87FloatArith(spec.ArithOpcode, st1, st0);
      if (result == nullptr) {
        return false;
      }
      // faddp: ST1 = op, then pop makes it the new ST0.
      writeX87ST(0, result);
      x87PopIntoST1();
      if (X87Depth) {
        --*X87Depth;
      }
      return true;
    }
    // PopArithSlot: ST(i) = op(ST0, ST(i)), then pop.
    llvm::Value *slot = x87Peek(spec.SlotIndex);
    llvm::Value *result = spec.ST0IsInput0
                              ? x87FloatArith(spec.ArithOpcode, st0, slot)
                              : x87FloatArith(spec.ArithOpcode, slot, st0);
    if (result == nullptr) {
      return false;
    }
    x87Poke(spec.SlotIndex, result);
    // The pop shifts ST(i) into ST(i-1), so for st(2) the poked value lands
    // in the window as the new ST1.
    x87WindowPop();
    return true;
  }

bool PcodeLowerer::lowerX87RegArith(const X87WindowSpec &spec,
                        std::string &errorMessage) {
    llvm::Value *st0 = readX87ST(0);
    llvm::Value *slot = readX87Slot(spec.SlotIndex);
    llvm::Value *result = spec.ST0IsInput0
                              ? x87FloatArith(spec.ArithOpcode, st0, slot)
                              : x87FloatArith(spec.ArithOpcode, slot, st0);
    if (result == nullptr) {
      return false;
    }
    writeX87ST(0, result);
    return true;
  }

bool PcodeLowerer::lowerX87MemArith(const X87WindowSpec &spec,
                        std::string &errorMessage) {
    llvm::Value *st0 = readX87ST(0);
    llvm::Value *raw = read(*spec.MemValue);
    llvm::Value *mem = Builder.CreateBitCast(raw, spec.MemValueType);
    if (!spec.MemValueType->isX86_FP80Ty()) {
      mem = Builder.CreateFPExt(mem, x87Fp80());
    }
    llvm::Value *result = spec.ST0IsInput0
                              ? x87FloatArith(spec.ArithOpcode, st0, mem)
                              : x87FloatArith(spec.ArithOpcode, mem, st0);
    if (result == nullptr) {
      return false;
    }
    writeX87ST(0, result);
    return true;
  }

bool PcodeLowerer::lowerX87Unary(const X87WindowSpec &spec, std::string &errorMessage) {
    llvm::Value *st0 = readX87ST(0);
    llvm::Value *result = nullptr;
    switch (spec.Unary) {
    case X87WindowSpec::UnaryKind::Sqrt: {
      llvm::Function *sqrt = llvm::Intrinsic::getOrInsertDeclaration(
          &Module, llvm::Intrinsic::sqrt, {x87Fp80()});
      result = Builder.CreateCall(sqrt, {st0});
      break;
    }
    case X87WindowSpec::UnaryKind::Abs: {
      llvm::Function *fabs = llvm::Intrinsic::getOrInsertDeclaration(
          &Module, llvm::Intrinsic::fabs, {x87Fp80()});
      result = Builder.CreateCall(fabs, {st0});
      break;
    }
    case X87WindowSpec::UnaryKind::Chs:
      result = Builder.CreateFNeg(st0);
      break;
    }
    writeX87ST(0, result);
    return true;
  }

bool PcodeLowerer::lowerX87Compare(const X87WindowSpec &spec,
                       std::string &errorMessage) {
    llvm::Value *st0 = readX87ST(0);
    llvm::Value *slot = readX87Slot(spec.SlotIndex);
    // Same flag shape as the x86.sinc fcomi macro: PF on unordered, ZF/CF
    // include the unordered case, AF/SF/OF cleared.  FCOMI and FUCOMI set the
    // same flags; they differ only in the invalid-operation exception.
    llvm::Value *pf = Builder.CreateFCmpUNO(st0, slot);
    llvm::Value *zf = Builder.CreateOr(pf, Builder.CreateFCmpOEQ(st0, slot));
    llvm::Value *cf = Builder.CreateOr(pf, Builder.CreateFCmpOLT(st0, slot));
    auto flagVarnode = [](uint64_t offset) {
      VarnodeView varnode;
      varnode.Space = "register";
      varnode.Offset = offset;
      varnode.Size = 1;
      varnode.IsRegister = true;
      return varnode;
    };
    write(flagVarnode(0x200), cf);  // CF
    write(flagVarnode(0x202), pf);  // PF
    write(flagVarnode(0x204),
          llvm::ConstantInt::get(intType(1), 0));  // AF
    write(flagVarnode(0x206), zf);  // ZF
    write(flagVarnode(0x207),
          llvm::ConstantInt::get(intType(1), 0));  // SF
    write(flagVarnode(0x20b),
          llvm::ConstantInt::get(intType(1), 0));  // OF
    if (spec.Pops) {
      x87WindowPop();
    }
    return true;
  }

bool PcodeLowerer::lowerX87Fxch(const X87WindowSpec &spec, std::string &errorMessage) {
    if (spec.SlotIndex == 0) {
      return true;  // fxch %st(0) is a no-op
    }
    if (spec.SlotIndex == 1) {
      llvm::Value *a = readX87ST(0, true);
      llvm::Value *b = readX87ST(1, true);
      writeX87ST(0, b);
      writeX87ST(1, a);
      return true;
    }
    llvm::Value *slot = x87Peek(spec.SlotIndex);
    x87Poke(spec.SlotIndex, readX87ST(0));
    writeX87ST(0, slot);
    return true;
  }

std::optional<int> PcodeLowerer::x87BlockDepthDelta(size_t blockIndex) const {
    int delta = 0;
    for (size_t opIndex = BlockStarts[blockIndex];
         opIndex < BlockEnds[blockIndex]; ++opIndex) {
      auto groupIt = X87Groups.find(opIndex);
      if (groupIt != X87Groups.end()) {
        const X87WindowSpec &spec = groupIt->second;
        switch (spec.Kind) {
        case X87WindowKind::Push:
          ++delta;
          break;
        case X87WindowKind::PopStore:
        case X87WindowKind::PopSlot:
        case X87WindowKind::PopArith:
        case X87WindowKind::PopArithSlot:
          --delta;
          break;
        case X87WindowKind::Library:
          return std::nullopt;
        default:
          if (spec.Pops) {
            --delta;
          }
          break;
        }
        opIndex = spec.End - 1;
        continue;
      }
      if (SuppressedPcodeOpIndices.count(opIndex) != 0) {
        continue;
      }
      const PcodeOpView &op = (*CurrentProgramOps)[opIndex];
      if ((op.Output && isX87StackVarnode(*op.Output)) ||
          std::any_of(op.Inputs.begin(), op.Inputs.end(),
                      [](const VarnodeView &input) {
                        return isX87StackVarnode(input);
                      })) {
        return std::nullopt;
      }
    }
    return delta;
  }

bool PcodeLowerer::computeX87BlockDepths(const PcodeProgram &program,
                             llvm::BasicBlock &blockForEntry) {
    const size_t blockCount = BlockStarts.size();
    std::map<const llvm::BasicBlock *, size_t> indexForBlock;
    for (size_t i = 0; i < blockCount; ++i) {
      indexForBlock.emplace(BlockForStart[BlockStarts[i]], i);
    }
    auto entryIt = indexForBlock.find(&blockForEntry);
    if (entryIt == indexForBlock.end()) {
      return false;
    }

    std::vector<std::optional<int>> deltas(blockCount);
    for (size_t i = 0; i < blockCount; ++i) {
      deltas[i] = x87BlockDepthDelta(i);
    }

    std::vector<std::optional<std::vector<size_t>>> successors(blockCount);
    for (size_t i = 0; i < blockCount; ++i) {
      std::vector<size_t> result;
      bool unknown = false;
      if (usesNativeCfg()) {
        uint64_t factAddress = 0;
        const std::vector<uint64_t> *native =
            nativeSuccessorsForBlockIndex(i, factAddress);
        if (native == nullptr) {
          unknown = true;
        } else {
          for (uint64_t address : *native) {
            auto blockIt = BlockForAddress.find(address);
            if (blockIt == BlockForAddress.end()) {
              continue;  // 跳出函数（尾跳/外部目标）：没有内部边
            }
            auto indexIt = indexForBlock.find(blockIt->second);
            if (indexIt == indexForBlock.end()) {
              unknown = true;  // 空 native 块：不追这条边
              break;
            }
            result.push_back(indexIt->second);
          }
        }
      } else {
        size_t terminatorIndex = BlockEnds[i] - 1;
        // 末块没有终结符就是 p-code 到此为止（lowerer 会补 ret），算函数出口；
        // 非末块没有终结符说明块结构不认识。
        const bool lastBlock = (i + 1 == blockCount);
        if (terminatorIndex >= program.Ops.size()) {
          unknown = true;
        } else {
          const PcodeOpView &op = program.Ops[terminatorIndex];
          if (op.Opcode == PcodeOpcode::BranchInd) {
            unknown = true;
          } else if (op.Opcode == PcodeOpcode::Branch ||
                     op.Opcode == PcodeOpcode::CBranch) {
            std::optional<uint64_t> target = directTarget(op, 0);
            if (!target) {
              if (auto index = relativeTargetIndex(terminatorIndex, op, 0,
                                                   program.Ops.size())) {
                target = program.Ops[*index].Address;
              }
            }
            if (!target) {
              unknown = true;
            } else {
              auto blockIt = BlockForAddress.find(*target);
              if (blockIt != BlockForAddress.end()) {
                auto indexIt = indexForBlock.find(blockIt->second);
                if (indexIt == indexForBlock.end()) {
                  unknown = true;
                } else {
                  result.push_back(indexIt->second);
                }
              }
              if (op.Opcode == PcodeOpcode::CBranch && !unknown &&
                  i + 1 < blockCount &&
                  BlockStarts[i + 1] == terminatorIndex + 1) {
                result.push_back(i + 1);
              }
            }
          } else if (op.Opcode != PcodeOpcode::Return && !lastBlock) {
            unknown = true;
          }
        }
      }
      successors[i] = unknown ? std::nullopt
                              : std::optional<std::vector<size_t>>(std::move(result));
    }

    // 可达块必须深度可证。
    std::vector<bool> reachable(blockCount, false);
    std::vector<size_t> worklist{entryIt->second};
    reachable[entryIt->second] = true;
    while (!worklist.empty()) {
      size_t index = worklist.back();
      worklist.pop_back();
      if (!deltas[index] || !successors[index]) {
        return false;
      }
      for (size_t next : *successors[index]) {
        if (!reachable[next]) {
          reachable[next] = true;
          worklist.push_back(next);
        }
      }
    }

    std::vector<std::optional<int64_t>> depth(blockCount, std::nullopt);
    depth[entryIt->second] = 0;
    bool changed = true;
    while (changed) {
      changed = false;
      for (size_t i = 0; i < blockCount; ++i) {
        if (!reachable[i] || !depth[i]) {
          continue;
        }
        int64_t exitDepth = *depth[i] + *deltas[i];
        if (exitDepth < 0) {
          return false;
        }
        for (size_t next : *successors[i]) {
          if (!depth[next]) {
            depth[next] = exitDepth;
            changed = true;
          } else if (*depth[next] != exitDepth) {
            return false;  // 汇合处深度不一致
          }
        }
      }
    }
    for (size_t i = 0; i < blockCount; ++i) {
      if (reachable[i]) {
        if (!depth[i]) {
          return false;
        }
        X87BlockEntryDepth[BlockStarts[i]] = static_cast<unsigned>(*depth[i]);
      }
    }
    return true;
  }

bool PcodeLowerer::lowerX87Window(const X87WindowSpec &spec, std::string &errorMessage) {
    // Lower the non-stack ops (LOAD / INT_ADD address computation) so the
    // operand values are available through the normal p-code SSA cache.
    // Comparison instructions (fcomi/fucomip) have no memory operand and
    // their unique temps are group-private, so nothing needs lowering there.
    // fnstsw/fnstcw must not lower the COPY that reads the FPU word and
    // fldcw must not lower the COPY that writes it: the intrinsic returns or
    // consumes the word instead.  The same applies to all ST0..ST7 rolling
    // writes, which the window model re-emits below.
    if (!spec.WritesComparisonFlags) {
      for (size_t index = spec.Start; index < spec.End; ++index) {
        const PcodeOpView &op = (*CurrentProgramOps)[index];
        if ((spec.SuppressLoads && op.Opcode == PcodeOpcode::Load) ||
            op.Opcode == PcodeOpcode::Store ||
            (op.Output &&
             (isX87StackVarnode(*op.Output) ||
              isFpuEnvVarnode(*op.Output))) ||
            std::any_of(op.Inputs.begin(), op.Inputs.end(),
                        [](const VarnodeView &varnode) {
                          return isX87StackVarnode(varnode) ||
                                 isFpuEnvVarnode(varnode);
                        })) {
          continue;
        }
        if (!lowerOp(op, errorMessage)) {
          return false;
        }
      }
    }

    switch (spec.Kind) {
    case X87WindowKind::Library:
      return lowerX87Library(spec, errorMessage);
    case X87WindowKind::Push:
      return lowerX87Push(spec, errorMessage);
    case X87WindowKind::PopStore:
      return lowerX87PopStore(spec, errorMessage);
    case X87WindowKind::PopSlot:
      return lowerX87PopSlot(spec, errorMessage);
    case X87WindowKind::StoreSlot:
      return lowerX87StoreSlot(spec, errorMessage);
    case X87WindowKind::PopArith:
    case X87WindowKind::PopArithSlot:
      return lowerX87PopArith(spec, errorMessage);
    case X87WindowKind::RegArith:
      return lowerX87RegArith(spec, errorMessage);
    case X87WindowKind::MemArith:
      return lowerX87MemArith(spec, errorMessage);
    case X87WindowKind::Unary:
      return lowerX87Unary(spec, errorMessage);
    case X87WindowKind::Compare:
      return lowerX87Compare(spec, errorMessage);
    case X87WindowKind::Fxch:
      return lowerX87Fxch(spec, errorMessage);
    }
    return false;
  }

std::string PcodeLowerer::blockName(uint64_t address) {

    std::ostringstream os;
    os << "bb_" << std::hex << address;
    return os.str();
  }

} // namespace notdec::bin2llvm
