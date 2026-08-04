#include "notdec-bin2llvm/PcodeToLLVM.h"
#include "notdec-bin2llvm/NativeX87Intrinsic.h"
#include "notdec-bin2llvm/RegisterStorage.h"

#include "llvm/ADT/APInt.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace notdec::bin2llvm {
namespace {

std::string varnodeKey(const VarnodeView &varnode) {
  std::ostringstream os;
  os << varnode.Space << ':' << std::hex << varnode.Offset << ':' << std::dec
     << varnode.Size;
  return os.str();
}

std::string llvmTypeName(llvm::Type *type) {
  std::string result;
  llvm::raw_string_ostream os(result);
  if (type != nullptr) {
    type->print(os);
  } else {
    os << "<null>";
  }
  return os.str();
}

llvm::Function *getOrInsertUnknownValueHelper(llvm::Module &module,
                                              llvm::Type *type) {
  std::string name = "notdec.unknown." + llvmTypeName(type);
  llvm::FunctionType *functionType =
      llvm::FunctionType::get(type, {}, false);
  if (auto *function = module.getFunction(name)) {
    if (function->getFunctionType() == functionType) {
      return function;
    }
  }
  std::string prefix = name + ".";
  for (llvm::Function &function : module.functions()) {
    if (function.getName().starts_with(prefix) &&
        function.getFunctionType() == functionType) {
      return &function;
    }
  }
  if (module.getNamedValue(name) != nullptr) {
    name += ".typed";
  }
  return llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage,
                                name, module);
}

llvm::Value *unknownValueAt(llvm::IRBuilder<> &builder, llvm::Module &module,
                            llvm::Type *type, llvm::Twine name) {
  return builder.CreateCall(getOrInsertUnknownValueHelper(module, type), {},
                            name);
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

// x87 FPU stack: ST0..ST7 live at register-space offsets 0x1100..0x1140, each
// 10 bytes.  MMX aliases the same offsets with size 8, so the size check keeps
// MMX accesses out of the x87 path.  These registers are never lowered to LLVM
// globals: every x87 machine instruction is folded into one notdec.x87.* call
// and the physical stack state stays inside that library.
bool isX87StackVarnode(const VarnodeView &varnode) {
  if (!varnode.IsRegister || varnode.Size != 10) {
    return false;
  }
  if (varnode.RegisterName && varnode.RegisterName->size() == 3 &&
      (*varnode.RegisterName)[0] == 'S' && (*varnode.RegisterName)[1] == 'T') {
    return true;
  }
  return varnode.Space == "register" && varnode.Offset >= 0x1100 &&
         varnode.Offset < 0x1140;
}

uint32_t x87StackIndex(const VarnodeView &varnode) {
  return static_cast<uint32_t>((varnode.Offset - 0x1100) / 0x10);
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
registersWithoutX87Stack(const std::vector<RegisterInfo> &registers) {
  std::vector<RegisterInfo> result;
  result.reserve(registers.size());
  for (const RegisterInfo &reg : registers) {
    if (reg.Space == "register" && reg.Size == 10 && reg.Offset >= 0x1100 &&
        reg.Offset < 0x1140) {
      continue;
    }
    result.push_back(reg);
  }
  return result;
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

// Intrinsic name suffix for the LLVM type an x87 operand is lifted to.
std::string fpSuffix(llvm::Type *type) {
  if (type->isX86_FP80Ty()) {
    return "f80";
  }
  return type->isDoubleTy() ? "f64" : "f32";
}

// Pop arithmetic (faddp/fmulp/...): ST1 = op(ST0, ST1).  Reverse forms
// (fsubrp/fdivrp) are ST1 = op(ST1, ST0), i.e. input0 is the st(i) slot.
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

// Memory arithmetic on ST0: ST0 = op(mem, ST0) is the reverse form
// (fsubr/fdivr) because the memory operand comes first in the p-code.
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

// Register arithmetic on ST0: ST0 = op(ST0, st(i)).
std::string regArithName(PcodeOpcode opcode) {
  switch (opcode) {
  case PcodeOpcode::FloatAdd:
    return "fadd";
  case PcodeOpcode::FloatSub:
    return "fsub";
  case PcodeOpcode::FloatMult:
    return "fmul";
  case PcodeOpcode::FloatDiv:
    return "fdiv";
  default:
    return "";
  }
}

class PcodeLowerer {
public:
  PcodeLowerer(llvm::LLVMContext &context, llvm::Module &module,
               llvm::Function &function, llvm::IRBuilder<> &builder,
               const PcodeLoweringConfig &config)
      : Context(context), Module(module), Function(function), Builder(builder),
        Config(config) {
  }

  bool lower(const PcodeProgram &program, std::string &errorMessage) {
    Registers = std::make_unique<RegisterStorage>(
        Context, Module, registersWithoutX87Stack(program.Registers),
        program.IsBigEndian);

    if (program.Ops.empty() && !usesNativeCfg()) {
      return true;
    }

    CurrentProgramOps = &program.Ops;
    prepareX86CallReturnStackSuppression(program);
    prepareX86PcThunkSuppression(program);
    prepareX87IntrinsicSuppression(program);
    if (!buildBasicBlocks(program, errorMessage)) {
      return false;
    }
    llvm::BasicBlock *entryBlock = entryBlockForProgram(errorMessage);
    if (entryBlock == nullptr) {
      return false;
    }
    Builder.CreateBr(entryBlock);

    for (size_t blockIndex = 0; blockIndex < BlockStarts.size(); ++blockIndex) {
      size_t start = BlockStarts[blockIndex];
      size_t end = BlockEnds[blockIndex];
      Builder.SetInsertPoint(BlockForStart[start]);
      // Native mode may split one machine block into several LLVM blocks for
      // instruction-internal p-code control flow, e.g. CMOV. These internal
      // blocks still belong to the same native block, so unique temporaries
      // computed before the split must remain visible.
      if (!isInternalPcodeBlock(blockIndex)) {
        Values.clear();
      }
      llvm::BasicBlock *nativeFallback = usesNativeCfg() ? nullptr
                                                         : nextBlock(blockIndex);

      bool ended = false;
      for (size_t opIndex = start; opIndex < end; ++opIndex) {
        auto x87It = X87Groups.find(opIndex);
        if (x87It != X87Groups.end()) {
          if (!lowerX87Group(x87It->second, errorMessage)) {
            return false;
          }
          opIndex = x87It->second.End - 1;
          continue;
        }
        if (SuppressedPcodeOpIndices.count(opIndex) != 0) {
          auto baseIt = ThunkBaseWrites.find(opIndex);
          if (baseIt != ThunkBaseWrites.end()) {
            llvm::Value *value = llvm::ConstantInt::get(
                intType(baseIt->second.first.Size), baseIt->second.second);
            write(baseIt->second.first, value);
          }
          continue;
        }

        const PcodeOpView &op = program.Ops[opIndex];
        if (isTerminator(op.Opcode)) {
          if (!lowerTerminator(blockIndex, opIndex, op, nativeFallback,
                               errorMessage)) {
            return false;
          }
          ended = true;
          break;
        }

        if (!lowerOp(op, errorMessage)) {
          return false;
        }
      }

      if (!ended) {
        llvm::BasicBlock *next = nullptr;
        if (!nativeFallthroughBlock(blockIndex, next, errorMessage)) {
          return false;
        }
        if (next != nullptr) {
          Builder.CreateBr(next);
        } else {
          Builder.CreateRetVoid();
        }
      }
    }

    for (uint64_t blockAddress : EmptyNativeBlockAddresses) {
      auto blockIt = BlockForAddress.find(blockAddress);
      if (blockIt == BlockForAddress.end()) {
        errorMessage = "empty native block is missing an LLVM block";
        return false;
      }
      Builder.SetInsertPoint(blockIt->second);

      llvm::BasicBlock *successor = nullptr;
      if (!nativeEmptyBlockSuccessor(blockAddress, successor, errorMessage)) {
        return false;
      }
      if (successor != nullptr) {
        Builder.CreateBr(successor);
      } else {
        Builder.CreateRetVoid();
      }
    }

    for (llvm::BasicBlock *target : ExternalTargetBlocks) {
      Builder.SetInsertPoint(target);
      Builder.CreateRetVoid();
    }

    llvm::EliminateUnreachableBlocks(Function);
    return true;
  }

private:
  bool usesNativeCfg() const {
    return !Config.BlockRanges.empty() || !Config.BlockSuccessors.empty();
  }

  std::vector<std::pair<uint64_t, uint64_t>> sortedNativeRanges() const {
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    ranges.reserve(Config.BlockRanges.size());
    for (const auto &[blockAddress, blockEnd] : Config.BlockRanges) {
      ranges.push_back({blockAddress, blockEnd});
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const auto &lhs, const auto &rhs) {
                return lhs.first < rhs.first;
              });
    return ranges;
  }

  bool nativeRangesCoverAddress(uint64_t address) const {
    for (const auto &[blockAddress, blockEnd] : Config.BlockRanges) {
      if (address >= blockAddress && address < blockEnd) {
        return true;
      }
    }
    return false;
  }

  std::optional<std::pair<uint64_t, uint64_t>>
  nativeRangeForAddress(uint64_t address) const {
    for (const auto &[blockAddress, blockEnd] : Config.BlockRanges) {
      if (address >= blockAddress && address < blockEnd) {
        return std::make_pair(blockAddress, blockEnd);
      }
    }
    return std::nullopt;
  }

  llvm::BasicBlock *entryBlockForProgram(std::string &errorMessage) {
    if (!Config.EntryAddress) {
      return BlockForStart[BlockStarts.front()];
    }

    uint64_t entryAddress = *Config.EntryAddress;
    if (usesNativeCfg()) {
      for (const auto &[blockAddress, blockEnd] : sortedNativeRanges()) {
        if (entryAddress >= blockAddress && entryAddress < blockEnd) {
          auto blockIt = BlockForAddress.find(blockAddress);
          if (blockIt != BlockForAddress.end()) {
            return blockIt->second;
          }
          break;
        }
      }
    } else {
      auto blockIt = BlockForAddress.find(entryAddress);
      if (blockIt != BlockForAddress.end()) {
        return blockIt->second;
      }
    }

    std::ostringstream os;
    os << "entry address 0x" << std::hex << entryAddress
       << " is not covered by lowered p-code blocks";
    errorMessage = os.str();
    return nullptr;
  }


  // One x87 machine instruction folded into a single notdec.x87.* call.  The
  // library owns the physical FPU stack, so the lifted IR keeps none of the
  // rolling ST0..ST7 p-code writes; only the memory operand reads stay in the
  // IR and feed the call arguments.  For fstp/fistp the call result is stored
  // by the lowering to the original p-code store address.
  struct X87IntrinsicSpec {
    size_t Start = 0;
    size_t End = 0;
    std::string IntrinsicName;
    std::vector<VarnodeView> Args;
    std::vector<llvm::Type *> ArgTypes;
    llvm::Type *ResultType = nullptr;
    std::optional<VarnodeView> StoreAddress;
    std::optional<uint32_t> StoreValueSize;
    // fcomi/fucomip: the intrinsic returns a packed i8 (bit0=CF, bit1=PF,
    // bit2=ZF); lowering writes the EFLAGS bits that integer code observes.
    // The FPU status word stays library-internal like the FPU stack.
    bool WritesComparisonFlags = false;
  };

  static bool isTerminator(PcodeOpcode opcode) {

    return opcode == PcodeOpcode::Branch ||
           opcode == PcodeOpcode::BranchInd ||
           opcode == PcodeOpcode::CBranch || opcode == PcodeOpcode::Return;
  }

  static bool sameVarnode(const VarnodeView &lhs, const VarnodeView &rhs) {
    return lhs.Space == rhs.Space && lhs.Offset == rhs.Offset &&
           lhs.Size == rhs.Size;
  }

  static bool isConstValue(const VarnodeView &varnode, uint64_t value,
                           uint32_t size) {
    return varnode.Space == "const" && varnode.Size == size &&
           varnode.Offset == value;
  }

  static bool isRegisterNamed(const VarnodeView &varnode,
                              const char *registerName, uint32_t size) {
    return varnode.IsRegister && varnode.RegisterName &&
           *varnode.RegisterName == registerName && varnode.Size == size;
  }

  struct X86CallStackSpec {
    const char *ArchName = "";
    const char *StackPointerRegister = "";
    uint32_t PointerSize = 0;
  };

  static std::optional<X86CallStackSpec>
  x86CallStackSpec(const PcodeProgram &program) {
    if (program.IsBigEndian) {
      return std::nullopt;
    }

    bool hasRsp = false;
    bool hasRip = false;
    bool hasEsp = false;
    bool hasEip = false;
    for (const RegisterInfo &reg : program.Registers) {
      hasRsp |= reg.Name == "RSP" && reg.Size == 8;
      hasRip |= reg.Name == "RIP" && reg.Size == 8;
      hasEsp |= reg.Name == "ESP" && reg.Size == 4;
      hasEip |= reg.Name == "EIP" && reg.Size == 4;
    }
    if (hasRsp && hasRip) {
      return X86CallStackSpec{"x64", "RSP", 8};
    }
    if (hasEsp && hasEip) {
      return X86CallStackSpec{"i386", "ESP", 4};
    }
    return std::nullopt;
  }

  static bool isX86StackPointer(const VarnodeView &varnode,
                                const X86CallStackSpec &spec) {
    return isRegisterNamed(varnode, spec.StackPointerRegister,
                           spec.PointerSize);
  }

  static bool isX86StackPointerAdjust(const PcodeOpView &op,
                                      PcodeOpcode opcode,
                                      const X86CallStackSpec &spec) {
    return op.Opcode == opcode && op.Output &&
           isX86StackPointer(*op.Output, spec) && op.Inputs.size() == 2 &&
           isX86StackPointer(op.Inputs[0], spec) &&
           isConstValue(op.Inputs[1], spec.PointerSize, spec.PointerSize);
  }

  static bool isX86ReturnAddressStore(const PcodeOpView &op,
                                      const VarnodeView &stackPointer,
                                      uint64_t fallthrough,
                                      const X86CallStackSpec &spec) {
    return op.Opcode == PcodeOpcode::Store && op.Inputs.size() == 3 &&
           sameVarnode(op.Inputs[1], stackPointer) &&
           isConstValue(op.Inputs[2], fallthrough, spec.PointerSize);
  }

  static bool isX86ReturnAddressLoad(const PcodeOpView &op,
                                     const VarnodeView &returnTarget,
                                     const X86CallStackSpec &spec) {
    return op.Opcode == PcodeOpcode::Load && op.Output &&
           sameVarnode(*op.Output, returnTarget) && op.Inputs.size() == 2 &&
           isX86StackPointer(op.Inputs[1], spec);
  }

  bool suppressX86CallStackEffect(size_t start, size_t end,
                                  const X86CallStackSpec &spec) {
    std::optional<size_t> stackSubtractIndex;
    std::optional<size_t> storeIndex;
    uint64_t fallthrough = 0;

    for (size_t index = start; index < end; ++index) {
      const PcodeOpView &op = (*CurrentProgramOps)[index];
      if (!isX86StackPointerAdjust(op, PcodeOpcode::IntSub, spec) ||
          op.InstructionSize == 0) {
        continue;
      }
      stackSubtractIndex = index;
      fallthrough = op.Address + op.InstructionSize;
      break;
    }
    if (!stackSubtractIndex) {
      return false;
    }

    const VarnodeView &stackPointer =
        *(*CurrentProgramOps)[*stackSubtractIndex].Output;
    for (size_t index = *stackSubtractIndex + 1; index < end; ++index) {
      if (isX86ReturnAddressStore((*CurrentProgramOps)[index], stackPointer,
                                  fallthrough, spec)) {
        storeIndex = index;
        break;
      }
    }
    if (!storeIndex) {
      return false;
    }

    SuppressedPcodeOpIndices.insert(*stackSubtractIndex);
    SuppressedPcodeOpIndices.insert(*storeIndex);
    return true;
  }

  bool suppressX86ReturnStackEffect(size_t start, size_t end,
                                    const X86CallStackSpec &spec) {
    std::optional<size_t> returnIndex;
    for (size_t index = start; index < end; ++index) {
      if ((*CurrentProgramOps)[index].Opcode == PcodeOpcode::Return) {
        returnIndex = index;
        break;
      }
    }
    if (!returnIndex || (*CurrentProgramOps)[*returnIndex].Inputs.size() != 1) {
      return false;
    }

    const VarnodeView &returnTarget =
        (*CurrentProgramOps)[*returnIndex].Inputs[0];
    std::optional<size_t> loadIndex;
    for (size_t index = start; index < *returnIndex; ++index) {
      if (isX86ReturnAddressLoad((*CurrentProgramOps)[index], returnTarget,
                                 spec)) {
        loadIndex = index;
        break;
      }
    }
    if (!loadIndex) {
      return false;
    }

    std::optional<size_t> rspAddIndex;
    for (size_t index = *loadIndex + 1; index < *returnIndex; ++index) {
      if (isX86StackPointerAdjust((*CurrentProgramOps)[index],
                                  PcodeOpcode::IntAdd, spec)) {
        rspAddIndex = index;
        break;
      }
    }
    if (!rspAddIndex) {
      return false;
    }

    SuppressedPcodeOpIndices.insert(*loadIndex);
    SuppressedPcodeOpIndices.insert(*rspAddIndex);
    return true;
  }

  void warnX86StackPatternMiss(uint64_t address, const char *kind,
                               const X86CallStackSpec &spec) {
    llvm::errs() << "warning: " << spec.ArchName << ' ' << kind << " at 0x";
    llvm::errs().write_hex(address);
    llvm::errs() << " did not match implicit return-address stack pattern\n";
  }

  void prepareX86CallReturnStackSuppression(const PcodeProgram &program) {
    SuppressedPcodeOpIndices.clear();
    std::optional<X86CallStackSpec> spec = x86CallStackSpec(program);
    if (!spec) {
      return;
    }

    for (size_t start = 0; start < program.Ops.size();) {
      size_t end = start + 1;
      while (end < program.Ops.size() &&
             program.Ops[end].Address == program.Ops[start].Address) {
        ++end;
      }

      bool hasCall = false;
      bool hasReturn = false;
      for (size_t index = start; index < end; ++index) {
        PcodeOpcode opcode = program.Ops[index].Opcode;
        hasCall |= opcode == PcodeOpcode::Call ||
                   opcode == PcodeOpcode::CallInd;
        hasReturn |= opcode == PcodeOpcode::Return;
      }

      if (hasCall) {
        if (!suppressX86CallStackEffect(start, end, *spec)) {
          warnX86StackPatternMiss(program.Ops[start].Address, "CALL", *spec);
        }
      }
      if (hasReturn && !suppressX86ReturnStackEffect(start, end, *spec)) {
        warnX86StackPatternMiss(program.Ops[start].Address, "RET", *spec);
      }

      start = end;
    }
  }

  // Fold x86 PIC `call <get_pc_thunk>; add $imm, %reg` into a constant base
  // address.  The thunk is `mov (%esp), %reg; ret`, so %reg ends up as the
  // call fallthrough plus the following immediate.  Suppressing the call here
  // also stops the pushed return address from being treated as an argument.
  void prepareX86PcThunkSuppression(const PcodeProgram &program) {
    ThunkBaseWrites.clear();
    if (Config.ThunkCallTargets.empty()) {
      return;
    }

    for (size_t start = 0; start < program.Ops.size();) {
      size_t end = start + 1;
      while (end < program.Ops.size() &&
             program.Ops[end].Address == program.Ops[start].Address) {
        ++end;
      }

      std::optional<uint64_t> callTarget;
      for (size_t index = start; index < end; ++index) {
        if (program.Ops[index].Opcode == PcodeOpcode::Call) {
          if (std::optional<uint64_t> target = directTarget(program.Ops[index], 0)) {
            callTarget = *target;
            break;
          }
        }
      }
      auto thunkIt = callTarget
                         ? Config.ThunkCallTargets.find(*callTarget)
                         : Config.ThunkCallTargets.end();
      if (thunkIt == Config.ThunkCallTargets.end()) {
        start = end;
        continue;
      }

      uint64_t fallthrough =
          program.Ops[start].Address + program.Ops[start].InstructionSize;
      std::optional<size_t> addIndex;
      std::optional<VarnodeView> baseRegister;
      uint64_t immediate = 0;
      for (size_t index = end; index < program.Ops.size(); ++index) {
        const PcodeOpView &op = program.Ops[index];
        if (isTerminator(op.Opcode) || op.Opcode == PcodeOpcode::Call ||
            op.Opcode == PcodeOpcode::CallInd) {
          break;
        }
        if (op.Opcode != PcodeOpcode::IntAdd || !op.Output ||
            !op.Output->IsRegister ||
            op.Output->RegisterName != thunkIt->second) {
          continue;
        }
        bool hasBaseInput = false;
        std::optional<uint64_t> constInput;
        for (const VarnodeView &input : op.Inputs) {
          if (input.IsRegister && input.RegisterName == thunkIt->second) {
            hasBaseInput = true;
          } else if (input.Space == "const") {
            constInput = input.Offset;
          }
        }
        if (!hasBaseInput || !constInput) {
          continue;
        }
        addIndex = index;
        baseRegister = *op.Output;
        immediate = *constInput;
        break;
      }
      if (!addIndex || !baseRegister) {
        start = end;
        continue;
      }

      for (size_t index = start; index < end; ++index) {
        SuppressedPcodeOpIndices.insert(index);
      }
      size_t addEnd = *addIndex + 1;
      while (addEnd < program.Ops.size() &&
             program.Ops[addEnd].Address == program.Ops[*addIndex].Address) {
        ++addEnd;
      }
      for (size_t index = *addIndex; index < addEnd; ++index) {
        SuppressedPcodeOpIndices.insert(index);
      }
      ThunkBaseWrites.emplace(start,
                              std::make_pair(*baseRegister,
                                             fallthrough + immediate));
      start = end;
    }
  }


  // Fold x87 instructions into notdec.x87.* calls.  The physical FPU stack
  // state lives inside the library, so the rolling ST0..ST7 p-code writes are
  // all suppressed and the call arguments are the explicit assembly operands.
  void prepareX87IntrinsicSuppression(const PcodeProgram &program) {
    X87Groups.clear();
    for (size_t start = 0; start < program.Ops.size();) {
      size_t end = start + 1;
      while (end < program.Ops.size() &&
             program.Ops[end].Address == program.Ops[start].Address) {
        ++end;
      }
      if (std::optional<X87IntrinsicSpec> spec =
              classifyX87Intrinsic(program, start, end)) {
        X87Groups.emplace(start, std::move(*spec));
      } else if (!program.Ops[start].Mnemonic.empty() &&
                 touchesX87Stack(program, start, end)) {
        llvm::errs() << "warning: x87 instruction "
                     << program.Ops[start].Mnemonic << " at 0x";
        llvm::errs().write_hex(program.Ops[start].Address);
        llvm::errs() << " was not folded into a notdec.x87 intrinsic\n";
      }
      start = end;
    }
  }

  static const PcodeOpView *findProducer(const PcodeProgram &program,
                                         size_t start, size_t end,
                                         const VarnodeView &target) {
    for (size_t index = start; index < end; ++index) {
      const PcodeOpView &op = program.Ops[index];
      if (op.Output && sameVarnode(*op.Output, target)) {
        return &op;
      }
    }
    return nullptr;
  }

  // Dispatch x87 classification by the machine instruction mnemonic when one
  // is available (native lifting); heritage JSON input carries no mnemonic
  // and falls back to the p-code shape classifier below.
  std::optional<X87IntrinsicSpec>
  classifyX87Intrinsic(const PcodeProgram &program, size_t start,
                       size_t end) {
    const std::string &mnemonic = program.Ops[start].Mnemonic;
    if (!mnemonic.empty()) {
      return classifyX87ByMnemonic(program, start, end, mnemonic);
    }
    return classifyX87ByShape(program, start, end);
  }

  // First p-code op writing an x87 stack varnode with the given opcode.
  // outputIndex -1 matches any ST slot.
  static const PcodeOpView *
  findStackWrite(const PcodeProgram &program, size_t start, size_t end,
                 PcodeOpcode opcode, int outputIndex) {
    for (size_t index = start; index < end; ++index) {
      const PcodeOpView &op = program.Ops[index];
      if (!op.Output || !isX87StackVarnode(*op.Output) ||
          op.Opcode != opcode) {
        continue;
      }
      if (outputIndex >= 0 &&
          static_cast<int>(x87StackIndex(*op.Output)) != outputIndex) {
        continue;
      }
      return &op;
    }
    return nullptr;
  }

  // fstp %st(i): ST(i) = COPY(ST0) before the pop rotation.
  static std::optional<uint32_t>
  stIndexFromSt0Copy(const PcodeProgram &program, size_t start, size_t end) {
    for (size_t index = start; index < end; ++index) {
      const PcodeOpView &op = program.Ops[index];
      if (op.Opcode != PcodeOpcode::Copy || op.Inputs.size() != 1 ||
          !op.Output || !isX87StackVarnode(*op.Output) ||
          !isX87StackVarnode(op.Inputs[0]) ||
          x87StackIndex(op.Inputs[0]) != 0) {
        continue;
      }
      return x87StackIndex(*op.Output);
    }
    return std::nullopt;
  }

  // fxch %st(i): ST0 = COPY(st(i)).
  static std::optional<uint32_t>
  stIndexFromSt0WriteCopy(const PcodeProgram &program, size_t start,
                          size_t end) {
    for (size_t index = start; index < end; ++index) {
      const PcodeOpView &op = program.Ops[index];
      if (op.Opcode != PcodeOpcode::Copy || op.Inputs.size() != 1 ||
          !op.Output || !isX87StackVarnode(*op.Output) ||
          x87StackIndex(*op.Output) != 0 ||
          !isX87StackVarnode(op.Inputs[0])) {
        continue;
      }
      return x87StackIndex(op.Inputs[0]);
    }
    return std::nullopt;
  }

  // Constant varnode carrying an st(i) slot index argument.
  static VarnodeView stIndexVarnode(uint32_t index) {
    VarnodeView varnode;
    varnode.Space = "const";
    varnode.Offset = index;
    varnode.Size = 1;
    return varnode;
  }

  // Classify one x87 instruction by its Ghidra mnemonic.  The mnemonic
  // decides the instruction category; the p-code only supplies the operand
  // values (memory address, const, st(i) index), so new instructions do not
  // need a p-code shape matcher.  Uncovered mnemonics (fcomi/fucomip and
  // friends) fall back to ordinary p-code lowering.
  std::optional<X87IntrinsicSpec>
  classifyX87ByMnemonic(const PcodeProgram &program, size_t start,
                        size_t end, const std::string &mnemonic) {
    if (!touchesX87Stack(program, start, end)) {
      return std::nullopt;
    }

    X87IntrinsicSpec spec;
    spec.Start = start;
    spec.End = end;

    // fldz / fld1: push a constant with no explicit operand.
    if (mnemonic == "FLDZ" || mnemonic == "FLD1") {
      spec.IntrinsicName =
          "notdec.x87." + std::string(mnemonic == "FLDZ" ? "fldz" : "fld1");
      spec.ResultType = llvm::Type::getVoidTy(Context);
      return spec;
    }

    // fsqrt: ST0 = sqrt(ST0).
    if (mnemonic == "FSQRT") {
      if (findStackWrite(program, start, end, PcodeOpcode::FloatSqrt, 0) ==
          nullptr) {
        return std::nullopt;
      }
      spec.IntrinsicName = "notdec.x87.fsqrt";
      spec.ResultType = llvm::Type::getVoidTy(Context);
      return spec;
    }

    // fld: push memory (f32/f64/f80) or duplicate st(i).
    if (mnemonic == "FLD") {
      // f32/f64 loads write ST0 = FLOAT2FLOAT(mem).
      const PcodeOpView *write = findStackWrite(
          program, start, end, PcodeOpcode::FloatFloat2Float, 0);
      if (write != nullptr && write->Inputs.size() == 1 &&
          !isX87StackVarnode(write->Inputs[0])) {
        llvm::Type *fpType = floatType(write->Inputs[0].Size);
        if (fpType != nullptr) {
          spec.IntrinsicName = "notdec.x87.fld." + fpSuffix(fpType);
          spec.Args = {write->Inputs[0]};
          spec.ArgTypes = {fpType};
          spec.ResultType = llvm::Type::getVoidTy(Context);
          return spec;
        }
        return std::nullopt;
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
        spec.IntrinsicName = "notdec.x87.fld.sti";
        spec.Args = {stIndexVarnode(x87StackIndex(producer->Inputs[0]))};
        spec.ArgTypes = {intType(1)};
        spec.ResultType = llvm::Type::getVoidTy(Context);
        return spec;
      }
      llvm::Type *fpType = floatType(copied.Size);
      if (fpType == nullptr) {
        return std::nullopt;
      }
      spec.IntrinsicName = "notdec.x87.fld." + fpSuffix(fpType);
      spec.Args = {copied};
      spec.ArgTypes = {fpType};
      spec.ResultType = llvm::Type::getVoidTy(Context);
      return spec;
    }

    // fstp / fistp: store ST0 to memory or to st(i) (st(0) is a plain pop).
    if (mnemonic == "FSTP" || mnemonic == "FISTP") {
      const PcodeOpView *storeOp = nullptr;
      for (size_t index = start; index < end; ++index) {
        if (program.Ops[index].Opcode == PcodeOpcode::Store) {
          storeOp = &program.Ops[index];
          break;
        }
      }
      // STORE inputs are (space selector, address, value).
      if (storeOp != nullptr && storeOp->Inputs.size() == 3) {
        const VarnodeView &storedValue = storeOp->Inputs[2];
        const PcodeOpView *producer =
            findProducer(program, start, end, storedValue);
        if (producer == nullptr || producer->Inputs.size() != 1 ||
            !isX87StackVarnode(producer->Inputs[0]) ||
            x87StackIndex(producer->Inputs[0]) != 0) {
          return std::nullopt;
        }
        if (mnemonic == "FISTP") {
          if (producer->Opcode != PcodeOpcode::FloatTrunc) {
            return std::nullopt;
          }
          spec.IntrinsicName = "notdec.x87.fistp.i" +
                               std::to_string(producer->Output->Size * 8);
          spec.ResultType = intType(producer->Output->Size);
        } else {
          // f32/f64 go through FLOAT2FLOAT, f80 through a plain COPY.
          if (producer->Opcode != PcodeOpcode::FloatFloat2Float &&
              producer->Opcode != PcodeOpcode::Copy) {
            return std::nullopt;
          }
          llvm::Type *fpType = floatType(producer->Output->Size);
          if (fpType == nullptr) {
            return std::nullopt;
          }
          spec.IntrinsicName = "notdec.x87.fstp." + fpSuffix(fpType);
          spec.ResultType = fpType;
        }
        spec.StoreAddress = storeOp->Inputs[1];
        spec.StoreValueSize = producer->Output->Size;
        return spec;
      }
      if (mnemonic == "FISTP") {
        return std::nullopt;
      }
      if (std::optional<uint32_t> index =
              stIndexFromSt0Copy(program, start, end)) {
        spec.IntrinsicName = "notdec.x87.fstp.sti";
        spec.Args = {stIndexVarnode(*index)};
        spec.ArgTypes = {intType(1)};
        spec.ResultType = llvm::Type::getVoidTy(Context);
        return spec;
      }
      return std::nullopt;
    }

    // fxch %st(i): swap ST0 and st(i).
    if (mnemonic == "FXCH") {
      if (std::optional<uint32_t> index =
              stIndexFromSt0WriteCopy(program, start, end)) {
        spec.IntrinsicName = "notdec.x87.fxch.sti";
        spec.Args = {stIndexVarnode(*index)};
        spec.ArgTypes = {intType(1)};
        spec.ResultType = llvm::Type::getVoidTy(Context);
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
      spec.IntrinsicName =
          "notdec.x87.fild.i" + std::to_string(input.Size * 8);
      spec.Args = {input};
      spec.ArgTypes = {intType(input.Size)};
      spec.ResultType = llvm::Type::getVoidTy(Context);
      return spec;
    }

    // fcomi/fucomip: compare ST0 with st(i), return packed CF|PF|ZF flags.
    // FCOMI/FUCOMI do not pop; FCOMIP/FUCOMIP pop inside the library.  The
    // st(i) operand is recovered from the FLOAT_EQUAL/FLOAT_LESS inputs.
    if (mnemonic == "FCOMI" || mnemonic == "FCOMIP" ||
        mnemonic == "FUCOMI" || mnemonic == "FUCOMIP") {
      std::optional<uint32_t> index;
      for (size_t opIndex = start; opIndex < end; ++opIndex) {
        const PcodeOpView &op = program.Ops[opIndex];
        if ((op.Opcode == PcodeOpcode::FloatEqual ||
             op.Opcode == PcodeOpcode::FloatLess) &&
            op.Inputs.size() == 2 &&
            isX87StackVarnode(op.Inputs[0]) &&
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
      std::string name = mnemonic == "FCOMI"
                             ? "fcomi"
                             : mnemonic == "FCOMIP"
                                   ? "fcomip"
                                   : mnemonic == "FUCOMI" ? "fucomi"
                                                          : "fucomip";
      spec.IntrinsicName = "notdec.x87." + name + ".sti";
      spec.Args = {stIndexVarnode(*index)};
      spec.ArgTypes = {intType(1)};
      spec.ResultType = intType(1);
      spec.WritesComparisonFlags = true;
      return spec;
    }

    // Pop arithmetic (faddp/fsubp/fmulp/fdivp/fsubrp/fdivrp):
    // ST1 = op(ST0, ST1), then pop.
    if (mnemonic == "FADDP" || mnemonic == "FSUBP" || mnemonic == "FMULP" ||
        mnemonic == "FDIVP" || mnemonic == "FSUBRP" ||
        mnemonic == "FDIVRP") {
      for (size_t index = start; index < end; ++index) {
        const PcodeOpView &op = program.Ops[index];
        if (!op.Output || !isX87StackVarnode(*op.Output) ||
            !isFloatBinaryOpcode(op.Opcode) || op.Inputs.size() != 2) {
          continue;
        }
        uint32_t outputIndex = x87StackIndex(*op.Output);
        if (outputIndex == 0 || !isX87StackVarnode(op.Inputs[0]) ||
            !isX87StackVarnode(op.Inputs[1])) {
          return std::nullopt;
        }
        std::string name = popArithName(op.Opcode, op.Inputs[0]);
        if (name.empty()) {
          return std::nullopt;
        }
        // st(1) keeps the legacy no-argument name; other slots take the
        // explicit slot index.
        if (outputIndex == 1) {
          spec.IntrinsicName = "notdec.x87." + name;
        } else {
          spec.IntrinsicName = "notdec.x87." + name + ".sti";
          spec.Args = {stIndexVarnode(outputIndex)};
          spec.ArgTypes = {intType(1)};
        }
        spec.ResultType = llvm::Type::getVoidTy(Context);
        return spec;
      }
      return std::nullopt;
    }

    // Register or memory arithmetic on ST0.
    if (mnemonic == "FADD" || mnemonic == "FSUB" || mnemonic == "FMUL" ||
        mnemonic == "FDIV") {
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
        // Register arithmetic: ST0 = op(ST0, st(i)).
        uint32_t index = x87StackIndex(input0) == 0
                             ? x87StackIndex(input1)
                             : x87StackIndex(input0);
        spec.IntrinsicName = "notdec.x87." + regArithName(write->Opcode) +
                             ".sti";
        spec.Args = {stIndexVarnode(index)};
        spec.ArgTypes = {intType(1)};
        spec.ResultType = llvm::Type::getVoidTy(Context);
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
      std::string name = memArithName(write->Opcode, !input0IsST);
      if (name.empty()) {
        return std::nullopt;
      }
      spec.IntrinsicName = "notdec.x87." + name + "." + fpSuffix(fpType);
      spec.Args = {rawMem};
      spec.ArgTypes = {fpType};
      spec.ResultType = llvm::Type::getVoidTy(Context);
      return spec;
    }

    return std::nullopt;
  }

  // Recognize one x87 instruction from its p-code expansion (heritage JSON
  // fallback, where no mnemonic is available).  Returns the folded intrinsic
  // description, or nullopt when the instruction does not match a known shape
  // and has to fall back to ordinary p-code lowering.
  std::optional<X87IntrinsicSpec>
  classifyX87ByShape(const PcodeProgram &program, size_t start, size_t end) {
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
      case PcodeOpcode::FloatTrunc:
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

    X87IntrinsicSpec spec;
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
          spec.IntrinsicName = "notdec.x87.fstp." + fpSuffix(fpType);
          spec.ResultType = fpType;
        } else if (producer->Opcode == PcodeOpcode::FloatTrunc) {
          spec.IntrinsicName = "notdec.x87.fistp.i" +
                               std::to_string(producer->Output->Size * 8);
          spec.ResultType = intType(producer->Output->Size);
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
      spec.IntrinsicName =
          "notdec.x87.fild.i" + std::to_string(input.Size * 8);
      spec.Args = {input};
      spec.ArgTypes = {intType(input.Size)};
      spec.ResultType = llvm::Type::getVoidTy(Context);
      return spec;
    }

    // push float (fld): ST0 = FLOAT2FLOAT(mem)
    if (writeOp.Opcode == PcodeOpcode::FloatFloat2Float) {
      if (writeOp.Inputs.size() != 1) {
        return std::nullopt;
      }
      const VarnodeView &input = writeOp.Inputs[0];
      if (isX87StackVarnode(input)) {
        return std::nullopt; // fld %st(i) not covered yet
      }
      llvm::Type *fpType = floatType(input.Size);
      if (fpType == nullptr) {
        return std::nullopt;
      }
      spec.IntrinsicName = "notdec.x87.fld." + fpSuffix(fpType);
      spec.Args = {input};
      spec.ArgTypes = {fpType};
      spec.ResultType = llvm::Type::getVoidTy(Context);
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
        // Pop arithmetic (faddp/fmulp/...): ST1 = op(ST0, ST1), then pop.
        if (x87StackIndex(*writeOp.Output) != 1) {
          return std::nullopt;
        }
        std::string name = popArithName(writeOp.Opcode, input0);
        if (name.empty()) {
          return std::nullopt;
        }
        spec.IntrinsicName = "notdec.x87." + name;
        spec.ResultType = llvm::Type::getVoidTy(Context);
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
      std::string name = memArithName(writeOp.Opcode, !input0IsST);
      if (name.empty()) {
        return std::nullopt;
      }
      spec.IntrinsicName = "notdec.x87." + name + "." + fpSuffix(fpType);
      spec.Args = {rawMem};
      spec.ArgTypes = {fpType};
      spec.ResultType = llvm::Type::getVoidTy(Context);
      return spec;
    }

    return std::nullopt;
  }

  llvm::Value *toIntrinsicArg(llvm::Value *value, llvm::Type *targetType) {
    if (value->getType() == targetType) {
      return value;
    }
    if (targetType->isFloatingPointTy()) {
      return Builder.CreateBitCast(value, targetType);
    }
    return Builder.CreateZExtOrTrunc(value, targetType);
  }

  bool lowerX87Group(const X87IntrinsicSpec &spec,
                     std::string &errorMessage) {
    // Lower the non-stack ops (LOAD / INT_ADD address computation) so the call
    // argument values are available through the normal p-code SSA cache.
    // Comparison instructions (fcomi/fucomip) have no memory operand and
    // their unique temps are group-private, so nothing needs lowering here.
    if (!spec.WritesComparisonFlags) {
      for (size_t index = spec.Start; index < spec.End; ++index) {
        const PcodeOpView &op = (*CurrentProgramOps)[index];
        if (op.Opcode == PcodeOpcode::Store ||
            (op.Output && isX87StackVarnode(*op.Output)) ||
            std::any_of(op.Inputs.begin(), op.Inputs.end(),
                        isX87StackVarnode)) {
          continue;
        }
        if (!lowerOp(op, errorMessage)) {
          return false;
        }
      }
    }

    llvm::Function *intrinsic = getOrInsertNativeX87Intrinsic(
        Module, spec.IntrinsicName, spec.ResultType, spec.ArgTypes);
    std::vector<llvm::Value *> args;
    args.reserve(spec.Args.size());
    for (size_t index = 0; index < spec.Args.size(); ++index) {
      args.push_back(
          toIntrinsicArg(read(spec.Args[index]), spec.ArgTypes[index]));
    }
    llvm::Value *result = Builder.CreateCall(intrinsic, args);

    if (spec.WritesComparisonFlags) {
      // The intrinsic returns CF|PF|ZF packed into an i8 (bits 0/1/2) and
      // mirrors the p-code flag writes that non-x87 instructions can observe:
      // EFLAGS CF/PF/ZF are set from the result and AF/SF/OF are cleared.
      // The FPU-internal side (FSW C1 bit and the C1 register) is owned by the
      // intrinsic library together with the FPU stack, so it is not modelled
      // here; the library must clear C1 in its internal status word.
      auto flagVarnode = [](uint64_t offset, uint32_t size) {
        VarnodeView varnode;
        varnode.Space = "register";
        varnode.Offset = offset;
        varnode.Size = size;
        varnode.IsRegister = true;
        return varnode;
      };
      const uint64_t flagOffsets[3] = {0x200, 0x202, 0x206}; // CF, PF, ZF
      for (unsigned bit = 0; bit < 3; ++bit) {
        llvm::Value *flag = Builder.CreateAnd(
            Builder.CreateLShr(result, bit), 1);
        write(flagVarnode(flagOffsets[bit], 1), flag);
      }
      llvm::Value *zero = llvm::ConstantInt::get(intType(1), 0);
      write(flagVarnode(0x204, 1), zero); // AF
      write(flagVarnode(0x207, 1), zero); // SF
      write(flagVarnode(0x20b, 1), zero); // OF
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

  std::string blockName(uint64_t address) {

    std::ostringstream os;
    os << "bb_" << std::hex << address;
    return os.str();
  }

  std::string addressFunctionName(uint64_t address) {
    std::ostringstream os;
    os << "notdec_native_" << std::hex << address;
    return os.str();
  }

  std::optional<uint64_t> directTarget(const PcodeOpView &op,
                                       size_t inputIndex) {
    if (op.Inputs.size() <= inputIndex ||
        op.Inputs[inputIndex].Space != "ram") {
      return std::nullopt;
    }
    return op.Inputs[inputIndex].Offset;
  }

  std::optional<size_t> relativeTargetIndex(size_t opIndex,
                                            const PcodeOpView &op,
                                            size_t inputIndex,
                                            size_t opCount) {
    if (op.Inputs.size() <= inputIndex ||
        op.Inputs[inputIndex].Space != "const") {
      return std::nullopt;
    }

    int64_t offset = relativeBranchOffset(op.Inputs[inputIndex]);
    if (offset < 0) {
      uint64_t magnitude = static_cast<uint64_t>(-(offset + 1)) + 1;
      if (opIndex < magnitude) {
        return std::nullopt;
      }
    }
    if (offset > 0 &&
        static_cast<uint64_t>(offset) > opCount - opIndex) {
      return std::nullopt;
    }
    size_t target =
        static_cast<size_t>(static_cast<int64_t>(opIndex) + offset);
    if (target >= opCount) {
      return std::nullopt;
    }
    return target;
  }

  int64_t relativeBranchOffset(const VarnodeView &target) {
    uint32_t bits = bitWidth(target.Size);
    if (bits >= 64) {
      return static_cast<int64_t>(target.Offset);
    }
    uint64_t signBit = uint64_t{1} << (bits - 1);
    uint64_t mask = (uint64_t{1} << bits) - 1;
    uint64_t value = target.Offset & mask;
    if ((value & signBit) == 0) {
      return static_cast<int64_t>(value);
    }
    return -static_cast<int64_t>((~value & mask) + 1);
  }

  void addBlockStart(std::set<size_t> &starts,
                     const std::map<uint64_t, size_t> &firstOpForAddress,
                     uint64_t address) {
    auto it = firstOpForAddress.find(address);
    if (it != firstOpForAddress.end()) {
      starts.insert(it->second);
    }
  }

  void addNativeBlockStart(std::set<size_t> &starts, size_t start,
                           uint64_t blockAddress, uint64_t blockEnd,
                           bool internalPcodeBlock = false) {
    // A p-code branch can target the start of its own machine block, for
    // example a tight loop ending in `jae block_start`.  That is still a real
    // native block, not an instruction-internal p-code split.  Keep the first
    // native mapping so conditional lowering will consult native CFG facts for
    // the false edge.
    if (internalPcodeBlock && starts.count(start) != 0 &&
        NativeInternalPcodeStarts.count(start) == 0) {
      return;
    }
    starts.insert(start);
    NativeBlockAddressForStart[start] =
        internalPcodeBlock ? (*CurrentProgramOps)[start].Address : blockAddress;
    NativeParentBlockAddressForStart[start] = blockAddress;
    NativeBlockEndForStart[start] = blockEnd;
    if (internalPcodeBlock) {
      NativeInternalPcodeStarts.insert(start);
    }
  }

  bool buildBasicBlocks(const PcodeProgram &program,
                        std::string &errorMessage) {
    std::map<uint64_t, size_t> firstOpForAddress;
    for (size_t index = 0; index < program.Ops.size(); ++index) {
      firstOpForAddress.try_emplace(program.Ops[index].Address, index);
    }

    std::set<size_t> starts;
    if (usesNativeCfg()) {
      for (const auto &[blockAddress, blockEnd] : sortedNativeRanges()) {
        if (blockEnd <= blockAddress) {
          std::ostringstream os;
          os << "native block 0x" << std::hex << blockAddress
             << " has an invalid block range ending at 0x" << blockEnd;
          errorMessage = os.str();
          return false;
        }
        bool foundPcodeInBlock = false;
        for (size_t index = 0; index < program.Ops.size(); ++index) {
          uint64_t opAddress = program.Ops[index].Address;
          if (opAddress >= blockAddress && opAddress < blockEnd) {
            addNativeBlockStart(starts, index, blockAddress, blockEnd);
            foundPcodeInBlock = true;
            break;
          }
        }
        if (!foundPcodeInBlock) {
          EmptyNativeBlockAddresses.push_back(blockAddress);
        }
      }
      if (starts.empty() && EmptyNativeBlockAddresses.empty()) {
        errorMessage = "native block ranges do not cover any p-code op";
        return false;
      }
      for (size_t index = 0; index < program.Ops.size(); ++index) {
        const PcodeOpView &op = program.Ops[index];
        if (!isTerminator(op.Opcode)) {
          continue;
        }
        auto range = nativeRangeForAddress(op.Address);
        if (!range) {
          continue;
        }
        auto [blockAddress, blockEnd] = *range;
        if (index + 1 < program.Ops.size() &&
            program.Ops[index + 1].Address >= blockAddress &&
            program.Ops[index + 1].Address < blockEnd) {
          addNativeBlockStart(starts, index + 1, blockAddress, blockEnd,
                              /*internalPcodeBlock=*/true);
        }
        if (op.Opcode == PcodeOpcode::Branch ||
            op.Opcode == PcodeOpcode::CBranch) {
          if (auto target = directTarget(op, 0)) {
            auto targetIt = firstOpForAddress.find(*target);
            if (targetIt != firstOpForAddress.end() &&
                *target >= blockAddress && *target < blockEnd) {
              addNativeBlockStart(starts, targetIt->second, blockAddress,
                                  blockEnd,
                                  /*internalPcodeBlock=*/true);
            }
          } else if (auto targetIndex = relativeTargetIndex(
                         index, op, 0, program.Ops.size())) {
            uint64_t targetAddress = program.Ops[*targetIndex].Address;
            if (targetAddress >= blockAddress && targetAddress < blockEnd) {
              addNativeBlockStart(starts, *targetIndex, blockAddress, blockEnd,
                                  /*internalPcodeBlock=*/true);
            }
          }
        }
      }
    }
    if (!usesNativeCfg()) {
      starts.insert(0);
      for (size_t index = 0; index < program.Ops.size(); ++index) {
        const PcodeOpView &op = program.Ops[index];
        if (op.Opcode == PcodeOpcode::Branch ||
            op.Opcode == PcodeOpcode::BranchInd ||
            op.Opcode == PcodeOpcode::CBranch) {
          if (auto target = directTarget(op, 0)) {
            addBlockStart(starts, firstOpForAddress, *target);
          } else if (auto targetIndex = relativeTargetIndex(
                         index, op, 0, program.Ops.size())) {
            starts.insert(*targetIndex);
          }
        }

        if (isTerminator(op.Opcode) && index + 1 < program.Ops.size()) {
          starts.insert(index + 1);
        }
      }
    }
    if (usesNativeCfg()) {
      for (const PcodeOpView &op : program.Ops) {
        bool covered = false;
        for (const auto &[blockAddress, blockEnd] : sortedNativeRanges()) {
          if (op.Address >= blockAddress && op.Address < blockEnd) {
            covered = true;
            break;
          }
        }
        if (!covered) {
          std::ostringstream os;
          os << "p-code op at 0x" << std::hex << op.Address
             << " is outside native block ranges";
          errorMessage = os.str();
          return false;
        }
      }
    }

    BlockStarts.assign(starts.begin(), starts.end());
    for (size_t index = 0; index < BlockStarts.size(); ++index) {
      size_t start = BlockStarts[index];
      uint64_t address = blockAddressForStart(start);
      auto end = blockEndForStart(program, index, errorMessage);
      if (!end) {
        return false;
      }
      BlockEnds.push_back(*end);
      llvm::BasicBlock *block =
          llvm::BasicBlock::Create(Context, blockName(address), &Function);
      BlockForStart[start] = block;
      BlockForAddress.try_emplace(address, block);
    }
    for (uint64_t blockAddress : EmptyNativeBlockAddresses) {
      llvm::BasicBlock *block =
          llvm::BasicBlock::Create(Context, blockName(blockAddress), &Function);
      BlockForAddress.try_emplace(blockAddress, block);
    }
    return true;
  }

  std::optional<size_t> blockEndForStart(const PcodeProgram &program,
                                         size_t blockIndex,
                                         std::string &errorMessage) {
    size_t start = BlockStarts[blockIndex];
    if (!usesNativeCfg()) {
      return blockIndex + 1 < BlockStarts.size() ? BlockStarts[blockIndex + 1]
                                                 : program.Ops.size();
    }

    auto rangeIt = NativeBlockEndForStart.find(start);
    if (rangeIt == NativeBlockEndForStart.end()) {
      std::ostringstream os;
      os << "native block 0x" << std::hex << blockAddressForStart(start)
         << " is missing a block range";
      errorMessage = os.str();
      return std::nullopt;
    }

    uint64_t blockEndAddress = rangeIt->second;
    uint64_t blockStartAddress = parentBlockAddressForIndex(blockIndex);
    if (blockIndex + 1 < BlockStarts.size()) {
      size_t nextStart = BlockStarts[blockIndex + 1];
      auto nextRangeIt = NativeBlockEndForStart.find(nextStart);
      if (nextRangeIt != NativeBlockEndForStart.end() &&
          nextRangeIt->second == blockEndAddress &&
          (*CurrentProgramOps)[nextStart].Address < blockEndAddress) {
        return nextStart;
      }
    }
    size_t end = start;
    while (end < program.Ops.size() &&
           program.Ops[end].Address >= blockStartAddress &&
           program.Ops[end].Address < blockEndAddress) {
      ++end;
    }
    return end;
  }

  uint64_t blockAddressForStart(size_t start) const {
    auto it = NativeBlockAddressForStart.find(start);
    if (it != NativeBlockAddressForStart.end()) {
      return it->second;
    }
    return (*CurrentProgramOps)[start].Address;
  }

  uint64_t blockAddressForIndex(size_t blockIndex) const {
    return blockAddressForStart(BlockStarts[blockIndex]);
  }

  uint64_t parentBlockAddressForIndex(size_t blockIndex) const {
    size_t start = BlockStarts[blockIndex];
    auto it = NativeParentBlockAddressForStart.find(start);
    if (it != NativeParentBlockAddressForStart.end()) {
      return it->second;
    }
    return blockAddressForStart(start);
  }

  const std::vector<uint64_t> *
  nativeSuccessorsForBlockIndex(size_t blockIndex,
                                uint64_t &factBlockAddress) const {
    factBlockAddress = blockAddressForIndex(blockIndex);
    auto successorIt = Config.BlockSuccessors.find(factBlockAddress);
    if (successorIt != Config.BlockSuccessors.end()) {
      return &successorIt->second;
    }

    // Native CFG facts are keyed by machine block start. P-code can split that
    // block internally for instruction semantics, so internal blocks must read
    // the parent machine block's successors.
    uint64_t parentAddress = parentBlockAddressForIndex(blockIndex);
    if (parentAddress == factBlockAddress) {
      return nullptr;
    }
    successorIt = Config.BlockSuccessors.find(parentAddress);
    if (successorIt == Config.BlockSuccessors.end()) {
      return nullptr;
    }
    factBlockAddress = parentAddress;
    return &successorIt->second;
  }

  bool isInternalPcodeBlock(size_t blockIndex) const {
    return NativeInternalPcodeStarts.count(BlockStarts[blockIndex]) != 0;
  }

  llvm::BasicBlock *nextBlock(size_t blockIndex) {
    if (blockIndex + 1 >= BlockStarts.size()) {
      return nullptr;
    }
    return BlockForStart[BlockStarts[blockIndex + 1]];
  }

  llvm::BasicBlock *internalPcodeContinuation(size_t blockIndex,
                                              size_t opIndex) {
    if (!usesNativeCfg() || opIndex + 1 >= CurrentProgramOps->size()) {
      return nullptr;
    }
    auto blockIt = BlockForStart.find(opIndex + 1);
    if (blockIt == BlockForStart.end()) {
      return nullptr;
    }
    auto rangeIt = NativeBlockEndForStart.find(BlockStarts[blockIndex]);
    if (rangeIt == NativeBlockEndForStart.end()) {
      return nullptr;
    }
    uint64_t parentAddress = parentBlockAddressForIndex(blockIndex);
    uint64_t blockEnd = rangeIt->second;
    uint64_t nextAddress = (*CurrentProgramOps)[opIndex + 1].Address;
    if (nextAddress < parentAddress || nextAddress >= blockEnd) {
      return nullptr;
    }
    return blockIt->second;
  }

  bool nativeFallthroughBlock(size_t blockIndex, llvm::BasicBlock *&result,
                              std::string &errorMessage) {
    result = nullptr;
    if (isInternalPcodeBlock(blockIndex)) {
      result = nextBlock(blockIndex);
      return true;
    }
    uint64_t blockAddress = blockAddressForIndex(blockIndex);
    auto successorIt = Config.BlockSuccessors.find(blockAddress);
    if (successorIt == Config.BlockSuccessors.end()) {
      if (usesNativeCfg()) {
        std::ostringstream os;
        os << "native block 0x" << std::hex << blockAddress
           << " is missing successor facts";
        errorMessage = os.str();
        return false;
      }
      result = nextBlock(blockIndex);
      return true;
    }
    if (successorIt->second.empty()) {
      return true;
    }
    if (successorIt->second.size() > 1) {
      std::ostringstream os;
      os << "native block 0x" << std::hex << blockAddress << " has "
         << std::dec << successorIt->second.size()
         << " successors but no p-code terminator";
      errorMessage = os.str();
      return false;
    }
    result = blockForNativeTarget(successorIt->second.front(), errorMessage);
    return result != nullptr;
  }

  bool nativeConditionalFalseBlock(size_t blockIndex, uint64_t trueTarget,
                                   llvm::BasicBlock *fallback,
                                   llvm::BasicBlock *&result,
                                   std::string &errorMessage) {
    result = nullptr;
    if (isInternalPcodeBlock(blockIndex) && fallback != nullptr) {
      result = fallback;
      return true;
    }
    uint64_t blockAddress = blockAddressForIndex(blockIndex);
    uint64_t factBlockAddress = blockAddress;
    const std::vector<uint64_t> *successors =
        nativeSuccessorsForBlockIndex(blockIndex, factBlockAddress);
    if (successors == nullptr) {
      if (usesNativeCfg()) {
        std::ostringstream os;
        os << "native conditional block 0x" << std::hex << blockAddress
           << " is missing successor facts";
        errorMessage = os.str();
        return false;
      }
      result = usesNativeCfg() ? nullptr : fallback;
      return true;
    }

    // SLEIGH CBRANCH only records the taken target.  Native discovery already
    // knows the machine-level false edge, which is safer than assuming the next
    // p-code block is the fallthrough for sparse or out-of-order ranges.
    bool sawTrueTarget = !nativeRangesCoverAddress(trueTarget);
    std::optional<uint64_t> falseTarget;
    for (uint64_t successor : *successors) {
      if (successor == trueTarget) {
        sawTrueTarget = true;
        continue;
      }
      if (falseTarget) {
        std::ostringstream os;
        os << "native conditional block 0x" << std::hex << factBlockAddress
           << " has multiple false successors";
        errorMessage = os.str();
        return false;
      }
      falseTarget = successor;
    }

    if (!sawTrueTarget) {
      std::ostringstream os;
      os << "native conditional block 0x" << std::hex << factBlockAddress
         << " is missing true successor 0x" << trueTarget;
      errorMessage = os.str();
      return false;
    }
    if (falseTarget) {
      result = blockForNativeTarget(*falseTarget, errorMessage);
      return result != nullptr;
    }
    if (usesNativeCfg()) {
      std::ostringstream os;
      os << "native conditional block 0x" << std::hex << blockAddress
         << " is missing false successor for true target 0x" << trueTarget;
      errorMessage = os.str();
      return false;
    }
    result = fallback;
    return true;
  }

  bool nativeEmptyBlockSuccessor(uint64_t blockAddress,
                                 llvm::BasicBlock *&result,
                                 std::string &errorMessage) {
    result = nullptr;
    auto successorIt = Config.BlockSuccessors.find(blockAddress);
    if (successorIt == Config.BlockSuccessors.end()) {
      if (usesNativeCfg()) {
        std::ostringstream os;
        os << "empty native block 0x" << std::hex << blockAddress
           << " is missing successor facts";
        errorMessage = os.str();
        return false;
      }
      return true;
    }
    if (successorIt->second.empty()) {
      return true;
    }
    if (successorIt->second.size() != 1) {
      std::ostringstream os;
      os << "empty native block 0x" << std::hex << blockAddress
         << " has multiple successors";
      errorMessage = os.str();
      return false;
    }
    result = blockForNativeTarget(successorIt->second.front(), errorMessage);
    return result != nullptr;
  }

  bool nativeIndirectBranchSuccessor(
      size_t blockIndex,
      std::vector<std::pair<uint64_t, llvm::BasicBlock *>> &result,
      std::string &errorMessage) {
    result.clear();
    if (!usesNativeCfg()) {
      return true;
    }

    uint64_t blockAddress = blockAddressForIndex(blockIndex);
    uint64_t factBlockAddress = blockAddress;
    const std::vector<uint64_t> *successors =
        nativeSuccessorsForBlockIndex(blockIndex, factBlockAddress);
    if (successors == nullptr) {
      std::ostringstream os;
      os << "native indirect branch block 0x" << std::hex << blockAddress
         << " is missing successor facts";
      errorMessage = os.str();
      return false;
    }
    if (successors->empty()) {
      return true;
    }
    for (uint64_t successor : *successors) {
      llvm::BasicBlock *block = blockForNativeTarget(successor, errorMessage);
      if (block == nullptr) {
        return false;
      }
      result.push_back({successor, block});
    }
    return true;
  }

  llvm::BasicBlock *blockForTarget(uint64_t address) {
    auto it = BlockForAddress.find(address);
    if (it != BlockForAddress.end()) {
      return it->second;
    }

    auto *block =
        llvm::BasicBlock::Create(Context, blockName(address), &Function);
    BlockForAddress[address] = block;
    ExternalTargetBlocks.push_back(block);
    return block;
  }

  llvm::BasicBlock *blockForNativeTarget(uint64_t address,
                                         std::string &errorMessage) {
    auto it = BlockForAddress.find(address);
    if (it != BlockForAddress.end()) {
      return it->second;
    }

    std::ostringstream os;
    os << "native branch target 0x" << std::hex << address
       << " is missing a native block";
    errorMessage = os.str();
    return nullptr;
  }

  bool isInternalPcodeTarget(size_t blockIndex, llvm::BasicBlock *targetBlock) {
    if (targetBlock == nullptr || !usesNativeCfg()) {
      return false;
    }
    for (size_t start : NativeInternalPcodeStarts) {
      auto blockIt = BlockForStart.find(start);
      if (blockIt == BlockForStart.end() || blockIt->second != targetBlock) {
        continue;
      }
      auto parentIt = NativeParentBlockAddressForStart.find(start);
      return parentIt != NativeParentBlockAddressForStart.end() &&
             parentIt->second == parentBlockAddressForIndex(blockIndex);
    }
    return false;
  }

  bool nativeDirectBranchTarget(size_t blockIndex, uint64_t target,
                                llvm::BasicBlock *targetBlock,
                                std::string &errorMessage) {
    if (!usesNativeCfg() || isInternalPcodeTarget(blockIndex, targetBlock)) {
      return true;
    }
    auto targetBlockIt = BlockForAddress.find(target);
    if (targetBlockIt == BlockForAddress.end() ||
        targetBlockIt->second != targetBlock) {
      std::ostringstream os;
      os << "native branch target 0x" << std::hex << target
         << " is missing a native block";
      errorMessage = os.str();
      return false;
    }
    uint64_t blockAddress = blockAddressForIndex(blockIndex);
    uint64_t factBlockAddress = blockAddress;
    const std::vector<uint64_t> *successors =
        nativeSuccessorsForBlockIndex(blockIndex, factBlockAddress);
    if (successors == nullptr) {
      std::ostringstream os;
      os << "native direct branch block 0x" << std::hex << blockAddress
         << " is missing successor facts";
      errorMessage = os.str();
      return false;
    }
    if (std::find(successors->begin(), successors->end(), target) !=
        successors->end()) {
      return true;
    }
    std::ostringstream os;
    os << "native direct branch block 0x" << std::hex << factBlockAddress
       << " is missing successor 0x" << target;
    errorMessage = os.str();
    return false;
  }

  bool nativeConditionalTrueTarget(size_t blockIndex, uint64_t target,
                                   llvm::BasicBlock *targetBlock,
                                   std::string &errorMessage) {
    if (!usesNativeCfg() || !nativeRangesCoverAddress(target)) {
      return true;
    }
    return nativeDirectBranchTarget(blockIndex, target, targetBlock,
                                    errorMessage);
  }

  llvm::BasicBlock *
  tailJumpBlockForKnownFunction(uint64_t address,
                                const std::string &calleeName) {
    auto it = TailJumpBlockForAddress.find(address);
    if (it != TailJumpBlockForAddress.end()) {
      return it->second;
    }

    llvm::BasicBlock *currentBlock = Builder.GetInsertBlock();
    auto *block =
        llvm::BasicBlock::Create(Context, "tail_" + blockName(address),
                                 &Function);
    TailJumpBlockForAddress[address] = block;

    Builder.SetInsertPoint(block);
    lowerKnownVoidTailJump(calleeName);
    Builder.SetInsertPoint(currentBlock);
    return block;
  }

  llvm::BasicBlock *blockForRelativeTarget(size_t opIndex,
                                           const PcodeOpView &op,
                                           size_t inputIndex,
                                           std::string &errorMessage) {
    auto targetIndex = relativeTargetIndex(opIndex, op, inputIndex,
                                           CurrentProgramOps->size());
    if (!targetIndex) {
      return nullptr;
    }
    auto it = BlockForStart.find(*targetIndex);
    if (it != BlockForStart.end()) {
      if (usesNativeCfg()) {
        uint64_t targetAddress = (*CurrentProgramOps)[*targetIndex].Address;
        uint64_t targetBlockAddress = blockAddressForStart(*targetIndex);
        if (targetBlockAddress != targetAddress) {
          std::ostringstream os;
          os << "native relative branch target 0x" << std::hex
             << targetAddress << " is missing a native block";
          errorMessage = os.str();
          return nullptr;
        }
      }
      return it->second;
    }
    if (usesNativeCfg()) {
      std::ostringstream os;
      os << "native relative branch target 0x" << std::hex
         << (*CurrentProgramOps)[*targetIndex].Address
         << " is missing a native block";
      errorMessage = os.str();
      return nullptr;
    }
    return blockForTarget((*CurrentProgramOps)[*targetIndex].Address);
  }

  llvm::BasicBlock *exitBlock() {
    if (!ExitBlock) {
      ExitBlock = llvm::BasicBlock::Create(Context, "notdec_exit", &Function);
      ExternalTargetBlocks.push_back(ExitBlock);
    }
    return ExitBlock;
  }

  llvm::BasicBlock *nativeIndirectSwitchDefaultBlock(uint64_t blockAddress) {
    // Native successor facts are authoritative here; a missing switch case is
    // a bad fact or unsupported dynamic path, not a normal function return.
    std::ostringstream name;
    name << "native_indirect_default_" << std::hex << blockAddress;
    llvm::BasicBlock *block =
        llvm::BasicBlock::Create(Context, name.str(), &Function);
    llvm::IRBuilder<> trapBuilder(block);
    llvm::Function *trap = llvm::Intrinsic::getOrInsertDeclaration(
        &Module, llvm::Intrinsic::trap);
    trapBuilder.CreateCall(trap);
    trapBuilder.CreateUnreachable();
    return block;
  }

  llvm::Value *asCondition(llvm::Value *value) {
    if (value->getType()->isIntegerTy(1)) {
      return value;
    }
    return Builder.CreateICmpNE(value,
                                llvm::ConstantInt::get(value->getType(), 0));
  }

  bool lowerTerminator(size_t blockIndex, size_t opIndex, const PcodeOpView &op,
                       llvm::BasicBlock *fallthrough,
                       std::string &errorMessage) {
    switch (op.Opcode) {
    case PcodeOpcode::Branch: {
      if (!requireInputCount(op, 1, errorMessage)) {
        return false;
      }
      auto target = directTarget(op, 0);
      if (target) {
        auto blockIt = BlockForAddress.find(*target);
        if (blockIt == BlockForAddress.end()) {
          auto externalIt = Config.ExternalCallTargets.find(*target);
          if (externalIt != Config.ExternalCallTargets.end()) {
            return lowerKnownVoidTailJump(externalIt->second);
          }
          auto directIt = Config.DirectCallTargets.find(*target);
          if (directIt != Config.DirectCallTargets.end()) {
            return lowerKnownVoidTailJump(directIt->second);
          }
          if (usesNativeCfg()) {
            if (!nativeRangesCoverAddress(*target)) {
              return lowerKnownVoidTailJump(addressFunctionName(*target));
            }
            (void)blockForNativeTarget(*target, errorMessage);
            return false;
          }
        }
        llvm::BasicBlock *targetBlock =
            blockIt != BlockForAddress.end() ? blockIt->second
                                             : blockForTarget(*target);
        if (!nativeDirectBranchTarget(blockIndex, *target, targetBlock,
                                      errorMessage)) {
          return false;
        }
        Builder.CreateBr(targetBlock);
        return true;
      }
      auto relativeIndex =
          relativeTargetIndex(opIndex, op, 0, CurrentProgramOps->size());
      llvm::BasicBlock *relativeTarget =
          relativeIndex ? blockForRelativeTarget(opIndex, op, 0, errorMessage)
                        : nullptr;
      if (relativeTarget == nullptr || !relativeIndex) {
        if (errorMessage.empty()) {
          errorMessage = "BRANCH target must be direct ram or relative const";
        }
        return false;
      }
      uint64_t relativeAddress = (*CurrentProgramOps)[*relativeIndex].Address;
      if (!nativeDirectBranchTarget(blockIndex, relativeAddress, relativeTarget,
                                    errorMessage)) {
        return false;
      }
      Builder.CreateBr(relativeTarget);
      return true;
    }

    case PcodeOpcode::CBranch: {
      if (!requireInputCount(op, 2, errorMessage)) {
        return false;
      }
      auto target = directTarget(op, 0);
      llvm::BasicBlock *trueBlock = nullptr;
      std::optional<uint64_t> trueAddress;
      if (target) {
        trueAddress = *target;
        if (usesNativeCfg()) {
          if (auto blockIt = BlockForAddress.find(*target);
              blockIt != BlockForAddress.end()) {
            trueBlock = blockIt->second;
          } else if (auto externalIt = Config.ExternalCallTargets.find(*target);
                     externalIt != Config.ExternalCallTargets.end()) {
            trueBlock =
                tailJumpBlockForKnownFunction(*target, externalIt->second);
          } else if (auto directIt = Config.DirectCallTargets.find(*target);
                     directIt != Config.DirectCallTargets.end()) {
            trueBlock =
                tailJumpBlockForKnownFunction(*target, directIt->second);
          } else if (!nativeRangesCoverAddress(*target)) {
            trueBlock = tailJumpBlockForKnownFunction(
                *target, addressFunctionName(*target));
          } else {
            trueBlock = blockForNativeTarget(*target, errorMessage);
          }
        } else {
          trueBlock = blockForTarget(*target);
        }
        if (trueBlock == nullptr) {
          return false;
        }
      } else {
        auto targetIndex = relativeTargetIndex(opIndex, op, 0,
                                               CurrentProgramOps->size());
        if (targetIndex) {
          trueAddress = (*CurrentProgramOps)[*targetIndex].Address;
          trueBlock =
              blockForRelativeTarget(opIndex, op, 0, errorMessage);
        }
      }
      if (trueBlock == nullptr) {
        if (errorMessage.empty()) {
          errorMessage = "CBRANCH target must be direct ram or relative const";
        }
        return false;
      }
      llvm::BasicBlock *falseBlock = fallthrough;
      if (trueAddress) {
        if (!nativeConditionalTrueTarget(blockIndex, *trueAddress, trueBlock,
                                         errorMessage)) {
          return false;
        }
        if (llvm::BasicBlock *internalContinuation =
                internalPcodeContinuation(blockIndex, opIndex)) {
          falseBlock = internalContinuation;
        } else {
          llvm::BasicBlock *nativeFalseBlock = nullptr;
          if (!nativeConditionalFalseBlock(blockIndex, *trueAddress, falseBlock,
                                           nativeFalseBlock, errorMessage)) {
            return false;
          }
          falseBlock = nativeFalseBlock;
        }
      }
      if (falseBlock == nullptr) {
        if (usesNativeCfg()) {
          std::ostringstream os;
          os << "native conditional block 0x" << std::hex
             << blockAddressForIndex(blockIndex)
             << " is missing false successor";
          if (trueAddress) {
            os << " for true target 0x" << *trueAddress;
          }
          errorMessage = os.str();
          return false;
        }
        falseBlock = exitBlock();
      }
      Builder.CreateCondBr(asCondition(read(op.Inputs[1])), trueBlock,
                           falseBlock);
      return true;
    }

    case PcodeOpcode::BranchInd:
      if (!requireInputCount(op, 1, errorMessage)) {
        return false;
      }
      if (auto gotAddress = sourceRam(op.Inputs[0])) {
        auto it = Config.IndirectExternalCallTargets.find(*gotAddress);
        if (it != Config.IndirectExternalCallTargets.end()) {
          return lowerKnownVoidTailJump(it->second);
        }
      }
      if (usesNativeCfg()) {
        std::vector<std::pair<uint64_t, llvm::BasicBlock *>> successors;
        if (!nativeIndirectBranchSuccessor(blockIndex, successors,
                                           errorMessage)) {
          return false;
        }
        if (successors.size() == 1) {
          Builder.CreateBr(successors.front().second);
          return true;
        }
        if (successors.empty()) {
          return lowerUnknownVoidIndirectTailJump(op.Inputs[0]);
        }
        if (!successors.empty()) {
          llvm::Value *target = resize(read(op.Inputs[0]), pointerByteSize());
          auto *switchInst =
              Builder.CreateSwitch(target,
                                   nativeIndirectSwitchDefaultBlock(
                                       blockAddressForIndex(blockIndex)),
                                   successors.size());
          for (const auto &[address, successor] : successors) {
            switchInst->addCase(
                llvm::cast<llvm::ConstantInt>(
                    llvm::ConstantInt::get(target->getType(), address)),
                successor);
          }
          return true;
        }
      }
      Builder.CreateBr(exitBlock());
      return true;

    case PcodeOpcode::Return:
      if (!requireInputCount(op, 1, errorMessage)) {
        return false;
      }
      Builder.CreateRetVoid();
      return true;

    default:
      errorMessage = "not a terminator opcode: " + op.OpcodeName;
      return false;
    }
  }

  llvm::Type *intType(uint32_t byteSize) {
    return llvm::IntegerType::get(Context, bitWidth(byteSize));
  }

  llvm::Type *floatType(uint32_t byteSize) {
    if (byteSize == 4) {
      return llvm::Type::getFloatTy(Context);
    }
    if (byteSize == 8) {
      return llvm::Type::getDoubleTy(Context);
    }
    if (byteSize == 10) {
      return llvm::Type::getX86_FP80Ty(Context);
    }
    return nullptr;
  }

  uint32_t floatByteSize(llvm::Type *type) const {
    if (type->isX86_FP80Ty()) {
      return 10;
    }
    return type->isDoubleTy() ? 8 : 4;
  }

  llvm::Value *readFloatBits(const VarnodeView &varnode, llvm::Type *floatTy) {
    return Builder.CreateBitCast(resize(read(varnode), floatByteSize(floatTy)),
                                 floatTy);
  }

  void writeFloatBits(const VarnodeView &varnode, llvm::Value *value) {
    llvm::Type *bitsTy = intType(floatByteSize(value->getType()));
    write(varnode, Builder.CreateBitCast(value, bitsTy));
  }

  llvm::Value *resize(llvm::Value *value, uint32_t byteSize) {
    llvm::Type *targetType = intType(byteSize);
    if (value->getType() == targetType) {
      return value;
    }

    unsigned sourceBits = value->getType()->getIntegerBitWidth();
    unsigned targetBits = targetType->getIntegerBitWidth();
    if (sourceBits < targetBits) {
      return Builder.CreateZExt(value, targetType);
    }
    return Builder.CreateTrunc(value, targetType);
  }

  uint32_t pointerByteSize() const {
    return static_cast<uint32_t>(Module.getDataLayout().getPointerSize());
  }

  llvm::Value *read(const VarnodeView &varnode) {
    llvm::Type *type = intType(varnode.Size);
    if (varnode.Space == "const") {
      return llvm::ConstantInt::get(
          type, llvm::APInt(bitWidth(varnode.Size), varnode.Offset));
    }

    auto it = Values.find(varnodeKey(varnode));
    if (it != Values.end()) {
      return visibleCachedValue(varnode, it->second);
    }

    if (varnode.Space == "ram") {
      auto *address =
          llvm::ConstantInt::get(intType(pointerByteSize()), varnode.Offset);
      auto *load = Builder.CreateLoad(type, memoryPointer(address),
                                      valueName(varnode) + "_in");
      load->setAlignment(llvm::Align(1));
      return load;
    }

    if (varnode.IsRegister && Registers != nullptr) {
      RegisterAccess access{varnode.Space, varnode.Offset, varnode.Size,
                            varnode.RegisterName};
      if (llvm::Value *value = Registers->read(Builder, access)) {
        return value;
      }
    }

    return unknownValueAt(Builder, Module, type, valueName(varnode) + "_in");
  }

  std::optional<uint64_t> sourceRam(const VarnodeView &varnode) const {
    if (varnode.Space == "ram") {
      return varnode.Offset;
    }
    auto it = SourceRamByVarnode.find(varnodeKey(varnode));
    if (it == SourceRamByVarnode.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  void setSourceRam(const VarnodeView &varnode, std::optional<uint64_t> source) {
    std::string key = varnodeKey(varnode);
    if (source) {
      SourceRamByVarnode[key] = *source;
    } else {
      SourceRamByVarnode.erase(key);
    }
  }

  void write(const VarnodeView &varnode, llvm::Value *value) {
    SourceRamByVarnode.erase(varnodeKey(varnode));
    llvm::Value *resized = resize(value, varnode.Size);
    if (varnode.IsRegister && Registers != nullptr) {
      RegisterAccess access{varnode.Space, varnode.Offset, varnode.Size,
                            varnode.RegisterName};
      if (Registers->hasRegister(access)) {
        Registers->write(Builder, access, resized);
        return;
      }
    }

    if (auto *instruction = llvm::dyn_cast<llvm::Instruction>(resized)) {
      instruction->setName(valueName(varnode));
    }
    Values[varnodeKey(varnode)] = resized;
  }

  // Native CFG can split one machine instruction into several LLVM blocks.
  // A unique temp may then be defined on one internal path and read after the
  // paths join.  Reusing the cached SSA value directly would violate LLVM
  // dominance, so create a local PHI that keeps the real value on defining
  // edges and poison on edges where the temp is not defined.
  llvm::Value *visibleCachedValue(const VarnodeView &varnode,
                                  llvm::Value *value) {
    llvm::Value *resized = resize(value, varnode.Size);
    auto *instruction = llvm::dyn_cast<llvm::Instruction>(resized);
    if (instruction == nullptr || instruction->getFunction() != &Function) {
      return resized;
    }

    llvm::BasicBlock *block = Builder.GetInsertBlock();
    if (block == nullptr) {
      return resized;
    }
    bool directPredDef = instructionDefBlockIsDirectPred(*instruction, *block);
    if ((!directPredDef && instructionDominatesBlock(*instruction, *block)) ||
        (directPredDef && llvm::pred_size(block) == 1)) {
      return resized;
    }
    if (llvm::pred_empty(block)) {
      return unknownValueAt(Builder, Module, resized->getType(),
                            valueName(varnode) + ".missing");
    }

    auto key = std::make_pair(varnodeKey(varnode), block);
    auto cached = NonDominatingReadValues.find(key);
    if (cached != NonDominatingReadValues.end()) {
      return cached->second;
    }

    llvm::IRBuilder<> phiBuilder(block, block->getFirstNonPHIIt());
    llvm::PHINode *phi =
        phiBuilder.CreatePHI(resized->getType(), llvm::pred_size(block),
                             valueName(varnode) + ".join");
    for (llvm::BasicBlock *pred : llvm::predecessors(block)) {
      llvm::Value *incoming = nullptr;
      if (pred == instruction->getParent()) {
        incoming = resized;
      } else if (!directPredDef && instructionDominatesBlock(*instruction,
                                                             *pred)) {
        incoming = resized;
      } else {
        incoming = unknownValueAtEnd(*pred, resized->getType(),
                                     valueName(varnode) + ".missing");
      }
      phi->addIncoming(incoming, pred);
    }
    NonDominatingReadValues.emplace(key, phi);
    return phi;
  }

  bool instructionDefBlockIsDirectPred(llvm::Instruction &instruction,
                                       llvm::BasicBlock &block) {
    for (llvm::BasicBlock *pred : llvm::predecessors(&block)) {
      if (pred == instruction.getParent()) {
        return true;
      }
    }
    return false;
  }

  bool instructionDominatesBlock(llvm::Instruction &instruction,
                                 llvm::BasicBlock &block) {
    if (instruction.getParent() == &block) {
      llvm::Instruction *terminator = block.getTerminator();
      return terminator == nullptr || instruction.comesBefore(terminator);
    }
    llvm::DominatorTree domTree(Function);
    return domTree.dominates(instruction.getParent(), &block);
  }

  llvm::Value *unknownValueAtEnd(llvm::BasicBlock &block, llvm::Type *type,
                                 llvm::Twine name) {
    llvm::Instruction *terminator = block.getTerminator();
    if (terminator == nullptr) {
      return llvm::PoisonValue::get(type);
    }
    llvm::IRBuilder<> builder(terminator);
    return unknownValueAt(builder, Module, type, name);
  }

  bool requireInputCount(const PcodeOpView &op, size_t count,
                         std::string &errorMessage) {
    if (op.Inputs.size() == count) {
      return true;
    }

    std::ostringstream os;
    os << op.OpcodeName << " needs " << count << " inputs, got "
       << op.Inputs.size();
    errorMessage = os.str();
    return false;
  }

  bool lowerBinary(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }

    if (op.Opcode == PcodeOpcode::IntXor &&
        sameVarnode(op.Inputs[0], op.Inputs[1])) {
      write(*op.Output, llvm::ConstantInt::get(intType(op.Output->Size), 0));
      return true;
    }

    llvm::Value *lhs = resize(read(op.Inputs[0]), op.Output->Size);
    llvm::Value *rhs = resize(read(op.Inputs[1]), op.Output->Size);
    llvm::Value *result = nullptr;

    switch (op.Opcode) {
    case PcodeOpcode::IntAdd:
      result = Builder.CreateAdd(lhs, rhs);
      break;
    case PcodeOpcode::IntSub:
      result = Builder.CreateSub(lhs, rhs);
      break;
    case PcodeOpcode::IntMult:
      result = Builder.CreateMul(lhs, rhs);
      break;
    case PcodeOpcode::IntDiv:
      result = Builder.CreateUDiv(lhs, rhs);
      break;
    case PcodeOpcode::IntSDiv:
      result = Builder.CreateSDiv(lhs, rhs);
      break;
    case PcodeOpcode::IntRem:
      result = Builder.CreateURem(lhs, rhs);
      break;
    case PcodeOpcode::IntSRem:
      result = Builder.CreateSRem(lhs, rhs);
      break;
    case PcodeOpcode::IntAnd:
      result = Builder.CreateAnd(lhs, rhs);
      break;
    case PcodeOpcode::IntOr:
      result = Builder.CreateOr(lhs, rhs);
      break;
    case PcodeOpcode::IntXor:
      result = Builder.CreateXor(lhs, rhs);
      break;
    case PcodeOpcode::IntLeft:
      result = Builder.CreateShl(lhs, rhs);
      break;
    case PcodeOpcode::IntRight:
      result = Builder.CreateLShr(lhs, rhs);
      break;
    case PcodeOpcode::IntSRight:
      result = Builder.CreateAShr(lhs, rhs);
      break;
    default:
      return false;
    }

    write(*op.Output, result);
    return true;
  }

  bool lowerCompare(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }

    uint32_t inputSize = op.Inputs[0].Size;
    llvm::Value *lhs = resize(read(op.Inputs[0]), inputSize);
    llvm::Value *rhs = resize(read(op.Inputs[1]), inputSize);
    llvm::Value *result = nullptr;

    switch (op.Opcode) {
    case PcodeOpcode::IntEqual:
      result = Builder.CreateICmpEQ(lhs, rhs);
      break;
    case PcodeOpcode::IntNotEqual:
      result = Builder.CreateICmpNE(lhs, rhs);
      break;
    case PcodeOpcode::IntLess:
      result = Builder.CreateICmpULT(lhs, rhs);
      break;
    case PcodeOpcode::IntLessEqual:
      result = Builder.CreateICmpULE(lhs, rhs);
      break;
    case PcodeOpcode::IntSLess:
      result = Builder.CreateICmpSLT(lhs, rhs);
      break;
    case PcodeOpcode::IntSLessEqual:
      result = Builder.CreateICmpSLE(lhs, rhs);
      break;
    default:
      return false;
    }

    write(*op.Output, result);
    return true;
  }

  bool lowerCast(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
      return false;
    }

    llvm::Value *input = read(op.Inputs[0]);
    llvm::Type *outputType = intType(op.Output->Size);
    llvm::Value *result = nullptr;
    if (op.Opcode == PcodeOpcode::IntZExt) {
      result = Builder.CreateZExt(input, outputType);
    } else {
      result = Builder.CreateSExt(input, outputType);
    }
    write(*op.Output, result);
    return true;
  }

  bool lowerUnary(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
      return false;
    }

    llvm::Value *input = resize(read(op.Inputs[0]), op.Output->Size);
    llvm::Value *result = nullptr;
    if (op.Opcode == PcodeOpcode::IntNegate) {
      result = Builder.CreateNot(input);
    } else if (op.Opcode == PcodeOpcode::Int2Comp) {
      result = Builder.CreateNeg(input);
    } else {
      errorMessage = "unsupported unary opcode: " + op.OpcodeName;
      return false;
    }
    write(*op.Output, result);
    return true;
  }

  bool lowerSubpiece(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }
    if (op.Inputs[1].Space != "const") {
      errorMessage = "SUBPIECE offset must be constant";
      return false;
    }

    llvm::Value *input = read(op.Inputs[0]);
    uint64_t shiftBits = op.Inputs[1].Offset * 8;
    llvm::Value *shift = llvm::ConstantInt::get(input->getType(), shiftBits);
    write(*op.Output, Builder.CreateLShr(input, shift));
    return true;
  }

  bool lowerPiece(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }

    llvm::Type *outputType = intType(op.Output->Size);
    llvm::Value *high = Builder.CreateZExt(read(op.Inputs[0]), outputType);
    llvm::Value *low = Builder.CreateZExt(read(op.Inputs[1]), outputType);
    llvm::Value *shift =
        llvm::ConstantInt::get(outputType, bitWidth(op.Inputs[1].Size));
    write(*op.Output, Builder.CreateOr(Builder.CreateShl(high, shift), low));
    return true;
  }

  bool lowerCountBits(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
      return false;
    }

    llvm::Value *input = read(op.Inputs[0]);
    llvm::Intrinsic::ID intrinsicId =
        op.Opcode == PcodeOpcode::Popcount ? llvm::Intrinsic::ctpop
                                           : llvm::Intrinsic::ctlz;
    llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
        &Module, intrinsicId, {input->getType()});
    if (op.Opcode == PcodeOpcode::Lzcount) {
      auto *zeroUndef = llvm::ConstantInt::getFalse(Context);
      write(*op.Output, Builder.CreateCall(intrinsic, {input, zeroUndef}));
    } else {
      write(*op.Output, Builder.CreateCall(intrinsic, {input}));
    }
    return true;
  }

  bool lowerCopyLike(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
      return false;
    }
    write(*op.Output, read(op.Inputs[0]));
    return true;
  }

  bool lowerPtrAdd(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 3, errorMessage)) {
      return false;
    }
    if (op.Inputs[2].Space != "const") {
      errorMessage = "PTRADD element size must be constant";
      return false;
    }

    llvm::Value *base = resize(read(op.Inputs[0]), op.Output->Size);
    llvm::Value *index = resize(read(op.Inputs[1]), op.Output->Size);
    llvm::Value *scale =
        llvm::ConstantInt::get(base->getType(), op.Inputs[2].Offset);
    write(*op.Output, Builder.CreateAdd(base, Builder.CreateMul(index, scale)));
    return true;
  }

  bool lowerPtrSub(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }
    if (op.Inputs[1].Space != "const") {
      errorMessage = "PTRSUB offset must be constant";
      return false;
    }

    llvm::Value *base = resize(read(op.Inputs[0]), op.Output->Size);
    llvm::Value *offset =
        llvm::ConstantInt::get(base->getType(), op.Inputs[1].Offset);
    write(*op.Output, Builder.CreateAdd(base, offset));
    return true;
  }

  bool lowerSignedBorrow(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }

    llvm::Value *lhs = read(op.Inputs[0]);
    llvm::Value *rhs = resize(read(op.Inputs[1]), op.Inputs[0].Size);
    llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
        &Module, llvm::Intrinsic::ssub_with_overflow, {lhs->getType()});
    llvm::Value *call = Builder.CreateCall(intrinsic, {lhs, rhs});
    write(*op.Output, Builder.CreateExtractValue(call, 1));
    return true;
  }

  bool lowerAddOverflow(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }

    llvm::Value *lhs = read(op.Inputs[0]);
    llvm::Value *rhs = resize(read(op.Inputs[1]), op.Inputs[0].Size);
    llvm::Intrinsic::ID intrinsicId =
        op.Opcode == PcodeOpcode::IntCarry
            ? llvm::Intrinsic::uadd_with_overflow
            : llvm::Intrinsic::sadd_with_overflow;
    llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
        &Module, intrinsicId, {lhs->getType()});
    llvm::Value *call = Builder.CreateCall(intrinsic, {lhs, rhs});
    write(*op.Output, Builder.CreateExtractValue(call, 1));
    return true;
  }

  bool lowerBoolNegate(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
      return false;
    }

    llvm::Value *input = asCondition(read(op.Inputs[0]));
    llvm::Value *result = Builder.CreateNot(input);
    write(*op.Output, result);
    return true;
  }

  bool lowerBoolBinary(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }

    llvm::Value *lhs = asCondition(read(op.Inputs[0]));
    llvm::Value *rhs = asCondition(read(op.Inputs[1]));
    llvm::Value *result = nullptr;
    switch (op.Opcode) {
    case PcodeOpcode::BoolAnd:
      result = Builder.CreateAnd(lhs, rhs);
      break;
    case PcodeOpcode::BoolOr:
      result = Builder.CreateOr(lhs, rhs);
      break;
    case PcodeOpcode::BoolXor:
      result = Builder.CreateXor(lhs, rhs);
      break;
    default:
      return false;
    }
    write(*op.Output, result);
    return true;
  }

  bool lowerFloatBinary(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }
    llvm::Type *floatTy = floatType(op.Output->Size);
    if (floatTy == nullptr) {
      return lowerHelperCall(op, errorMessage);
    }

    llvm::Value *lhs = readFloatBits(op.Inputs[0], floatTy);
    llvm::Value *rhs = readFloatBits(op.Inputs[1], floatTy);
    llvm::Value *result = nullptr;
    switch (op.Opcode) {
    case PcodeOpcode::FloatAdd:
      result = Builder.CreateFAdd(lhs, rhs);
      break;
    case PcodeOpcode::FloatSub:
      result = Builder.CreateFSub(lhs, rhs);
      break;
    case PcodeOpcode::FloatMult:
      result = Builder.CreateFMul(lhs, rhs);
      break;
    case PcodeOpcode::FloatDiv:
      result = Builder.CreateFDiv(lhs, rhs);
      break;
    default:
      errorMessage = "unsupported float binary opcode: " + op.OpcodeName;
      return false;
    }
    writeFloatBits(*op.Output, result);
    return true;
  }

  bool lowerFloatCompare(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage)) {
      return false;
    }
    llvm::Type *floatTy = floatType(op.Inputs[0].Size);
    if (floatTy == nullptr) {
      return lowerHelperCall(op, errorMessage);
    }

    llvm::Value *lhs = readFloatBits(op.Inputs[0], floatTy);
    llvm::Value *rhs = readFloatBits(op.Inputs[1], floatTy);
    llvm::Value *result = nullptr;
    switch (op.Opcode) {
    case PcodeOpcode::FloatEqual:
      result = Builder.CreateFCmpOEQ(lhs, rhs);
      break;
    case PcodeOpcode::FloatNotEqual:
      result = Builder.CreateFCmpUNE(lhs, rhs);
      break;
    case PcodeOpcode::FloatLess:
      result = Builder.CreateFCmpOLT(lhs, rhs);
      break;
    case PcodeOpcode::FloatLessEqual:
      result = Builder.CreateFCmpOLE(lhs, rhs);
      break;
    default:
      errorMessage = "unsupported float compare opcode: " + op.OpcodeName;
      return false;
    }
    write(*op.Output, result);
    return true;
  }

  bool lowerFloatNan(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
      return false;
    }
    llvm::Type *floatTy = floatType(op.Inputs[0].Size);
    if (floatTy == nullptr) {
      return lowerHelperCall(op, errorMessage);
    }
    llvm::Value *input = readFloatBits(op.Inputs[0], floatTy);
    write(*op.Output, Builder.CreateFCmpUNO(input, input));
    return true;
  }

  bool lowerFloatUnary(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
      return false;
    }
    llvm::Type *floatTy = floatType(op.Output->Size);
    if (floatTy == nullptr) {
      return lowerHelperCall(op, errorMessage);
    }

    llvm::Value *input = readFloatBits(op.Inputs[0], floatTy);
    llvm::Value *result = nullptr;
    if (op.Opcode == PcodeOpcode::FloatNeg) {
      result = Builder.CreateFNeg(input);
    } else if (op.Opcode == PcodeOpcode::FloatAbs) {
      llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
          &Module, llvm::Intrinsic::fabs, {floatTy});
      result = Builder.CreateCall(intrinsic, {input});
    } else if (op.Opcode == PcodeOpcode::FloatSqrt) {
      llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
          &Module, llvm::Intrinsic::sqrt, {floatTy});
      result = Builder.CreateCall(intrinsic, {input});
    } else if (op.Opcode == PcodeOpcode::FloatCeil ||
               op.Opcode == PcodeOpcode::FloatFloor ||
               op.Opcode == PcodeOpcode::FloatRound) {
      llvm::Intrinsic::ID intrinsicId = llvm::Intrinsic::not_intrinsic;
      if (op.Opcode == PcodeOpcode::FloatCeil) {
        intrinsicId = llvm::Intrinsic::ceil;
      } else if (op.Opcode == PcodeOpcode::FloatFloor) {
        intrinsicId = llvm::Intrinsic::floor;
      } else {
        intrinsicId = llvm::Intrinsic::round;
      }
      llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
          &Module, intrinsicId, {floatTy});
      result = Builder.CreateCall(intrinsic, {input});
    } else {
      errorMessage = "unsupported float unary opcode: " + op.OpcodeName;
      return false;
    }
    writeFloatBits(*op.Output, result);
    return true;
  }

  bool lowerFloatCast(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
      return false;
    }

    llvm::Type *outputFloatTy = floatType(op.Output->Size);
    if (op.Opcode == PcodeOpcode::FloatInt2Float) {
      if (outputFloatTy == nullptr) {
        return lowerHelperCall(op, errorMessage);
      }
      writeFloatBits(*op.Output,
                     Builder.CreateSIToFP(read(op.Inputs[0]), outputFloatTy));
      return true;
    }

    llvm::Type *inputFloatTy = floatType(op.Inputs[0].Size);
    if (inputFloatTy == nullptr) {
      return lowerHelperCall(op, errorMessage);
    }
    llvm::Value *input = readFloatBits(op.Inputs[0], inputFloatTy);
    if (op.Opcode == PcodeOpcode::FloatFloat2Float) {
      if (outputFloatTy == nullptr) {
        return lowerHelperCall(op, errorMessage);
      }
      writeFloatBits(*op.Output, Builder.CreateFPCast(input, outputFloatTy));
      return true;
    }
    if (op.Opcode == PcodeOpcode::FloatTrunc) {
      write(*op.Output, Builder.CreateFPToSI(input, intType(op.Output->Size)));
      return true;
    }

    errorMessage = "unsupported float cast opcode: " + op.OpcodeName;
    return false;
  }

  llvm::GlobalVariable *memoryGlobal() {
    if (Memory) {
      return Memory;
    }

    // First memory model: one external byte array.  It keeps LOAD/STORE visible
    // in IR without pretending we already understand binary sections or ABI.
    auto *byteType = llvm::Type::getInt8Ty(Context);
    auto *arrayType = llvm::ArrayType::get(byteType, 1024 * 1024);
    std::string name = "notdec_ram";
    unsigned index = 1;
    while (llvm::Value *existing = Module.getNamedValue(name)) {
      auto *global = llvm::dyn_cast<llvm::GlobalVariable>(existing);
      if (global != nullptr && global->getValueType() == arrayType) {
        Memory = global;
        return Memory;
      }
      name = "notdec_ram." + std::to_string(index++);
    }
    Memory = new llvm::GlobalVariable(Module, arrayType, false,
                                      llvm::GlobalValue::ExternalLinkage,
                                      nullptr, name);
    return Memory;
  }

  llvm::Value *memoryPointer(llvm::Value *address) {
    address = resize(address, pointerByteSize());
    if (Config.MemoryModel == PcodeMemoryModel::IntToPtr) {
      return Builder.CreateIntToPtr(address, llvm::PointerType::get(Context, 0),
                                    "notdec_ram_ptr");
    }

    llvm::Value *zero = llvm::ConstantInt::get(address->getType(), 0);
    return Builder.CreateGEP(memoryGlobal()->getValueType(), memoryGlobal(),
                             {zero, address}, "notdec_ram_ptr");
  }

  bool requireConstSpaceSelector(const PcodeOpView &op,
                                 std::string &errorMessage) {
    if (!op.Inputs.empty() && op.Inputs[0].Space == "const") {
      return true;
    }

    errorMessage = op.OpcodeName + " address space selector must be const";
    return false;
  }

  bool lowerLoad(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 2, errorMessage) ||
        !requireConstSpaceSelector(op, errorMessage)) {
      return false;
    }

    llvm::Value *address = resize(read(op.Inputs[1]), pointerByteSize());
    auto *load =
        Builder.CreateLoad(intType(op.Output->Size), memoryPointer(address),
                           valueName(*op.Output));
    load->setAlignment(llvm::Align(1));
    write(*op.Output, load);
    return true;
  }

  bool lowerStore(const PcodeOpView &op, std::string &errorMessage) {
    if (!requireInputCount(op, 3, errorMessage) ||
        !requireConstSpaceSelector(op, errorMessage)) {
      return false;
    }

    llvm::Value *address = resize(read(op.Inputs[1]), pointerByteSize());
    llvm::Value *value = read(op.Inputs[2]);
    auto *store = Builder.CreateStore(value, memoryPointer(address));
    store->setAlignment(llvm::Align(1));
    return true;
  }

  bool lowerHelperCall(const PcodeOpView &op, std::string &errorMessage) {
    llvm::Type *returnType = llvm::Type::getVoidTy(Context);
    std::string helperName = "notdec_pcode_" + op.OpcodeName + "_void";
    if (op.Output) {
      returnType = intType(op.Output->Size);
      helperName = "notdec_pcode_" + op.OpcodeName + "_i" +
                   std::to_string(bitWidth(op.Output->Size));
    }

    std::vector<llvm::Value *> args;
    args.reserve(op.Inputs.size());
    for (const VarnodeView &input : op.Inputs) {
      if (input.Space == "ram") {
        args.push_back(
            llvm::ConstantInt::get(intType(input.Size), input.Offset));
      } else {
        args.push_back(read(input));
      }
    }

    auto *helperType = llvm::FunctionType::get(returnType, {}, true);
    llvm::FunctionCallee helper =
        Module.getOrInsertFunction(helperName, helperType);
    llvm::CallInst *call = Builder.CreateCall(helper, args);
    if (!op.Output) {
      return true;
    }
    write(*op.Output, call);
    return true;
  }

  llvm::CallInst *lowerKnownVoidCall(const std::string &calleeName) {
    auto *calleeType =
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
    llvm::FunctionCallee callee =
        Module.getOrInsertFunction(calleeName, calleeType);
    return Builder.CreateCall(callee, {});
  }

  llvm::CallInst *createUnknownVoidIndirectCall(const VarnodeView &target) {
    auto *calleeType =
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
    auto *calleePointer =
        Builder.CreateIntToPtr(resize(read(target), pointerByteSize()),
                               llvm::PointerType::getUnqual(Context));
    return Builder.CreateCall(calleeType, calleePointer, {});
  }

  bool lowerUnknownVoidIndirectCall(const VarnodeView &target) {
    createUnknownVoidIndirectCall(target);
    return true;
  }

  bool lowerUnknownVoidIndirectTailJump(const VarnodeView &target) {
    llvm::CallInst *call = createUnknownVoidIndirectCall(target);
    call->setTailCallKind(llvm::CallInst::TCK_Tail);
    Builder.CreateRetVoid();
    return true;
  }

  bool lowerKnownVoidTailJump(const std::string &calleeName) {
    // Current native lowering has no ABI/prototype model.  For proven external
    // tail jumps, preserve the handoff to the external symbol and end this body.
    llvm::CallInst *call = lowerKnownVoidCall(calleeName);
    call->setTailCallKind(llvm::CallInst::TCK_Tail);
    Builder.CreateRetVoid();
    return true;
  }

  bool lowerCall(const PcodeOpView &op, std::string &errorMessage) {
    // Minimal inter-function lowering: when --all-confirmed has already planned
    // a symbol for this direct target, emit a real LLVM call.  Calls with a
    // modeled return value stay on the helper path until native prototype
    // recovery exists.
    if (!op.Output) {
      if (auto target = directTarget(op, 0)) {
        auto externalIt = Config.ExternalCallTargets.find(*target);
        if (externalIt != Config.ExternalCallTargets.end()) {
          lowerKnownVoidCall(externalIt->second);
          return true;
        }

        auto it = Config.DirectCallTargets.find(*target);
        if (it != Config.DirectCallTargets.end()) {
          lowerKnownVoidCall(it->second);
          return true;
        }

        lowerKnownVoidCall(addressFunctionName(*target));
        return true;
      }
    }

    return lowerHelperCall(op, errorMessage);
  }

  bool lowerCallInd(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output && requireInputCount(op, 1, errorMessage)) {
      if (auto gotAddress = sourceRam(op.Inputs[0])) {
        auto it = Config.IndirectExternalCallTargets.find(*gotAddress);
        if (it != Config.IndirectExternalCallTargets.end()) {
          lowerKnownVoidCall(it->second);
          return true;
        }
      }
      return lowerUnknownVoidIndirectCall(op.Inputs[0]);
    }
    return lowerHelperCall(op, errorMessage);
  }

  bool lowerX86Movmskpd(const PcodeOpView &op, std::string &errorMessage) {
    if (!op.Output || !requireInputCount(op, 3, errorMessage)) {
      return false;
    }
    if (op.Inputs[2].Size == 0 || op.Inputs[2].Size % 8 != 0) {
      return lowerHelperCall(op, errorMessage);
    }

    llvm::Value *source = read(op.Inputs[2]);
    llvm::Value *mask = llvm::ConstantInt::get(intType(op.Output->Size), 0);
    uint32_t laneCount = op.Inputs[2].Size / 8;
    for (uint32_t lane = 0; lane < laneCount; ++lane) {
      llvm::Value *shifted = Builder.CreateLShr(
          source, llvm::ConstantInt::get(source->getType(), lane * 64 + 63));
      llvm::Value *bit = Builder.CreateTrunc(
          Builder.CreateAnd(shifted,
                            llvm::ConstantInt::get(source->getType(), 1)),
          intType(1));
      llvm::Value *wideBit = resize(bit, op.Output->Size);
      if (lane != 0) {
        wideBit =
            Builder.CreateShl(wideBit,
                              llvm::ConstantInt::get(wideBit->getType(), lane));
      }
      mask = Builder.CreateOr(mask, wideBit);
    }
    write(*op.Output, mask);
    return true;
  }

  bool lowerCallOther(const PcodeOpView &op, std::string &errorMessage) {
    if (op.Output && requireInputCount(op, 1, errorMessage) &&
        op.Inputs[0].Space == "const" && op.Inputs[0].Offset == 77) {
      llvm::Function *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
          &Module, llvm::Intrinsic::readcyclecounter);
      write(*op.Output, resize(Builder.CreateCall(intrinsic, {}), op.Output->Size));
      return true;
    }

    // x86 LOCK/UNLOCK are Sleigh userops around normal memory P-Code.  The
    // read/write ops still carry the value semantics, so keep only that part.
    if (!op.Output && requireInputCount(op, 1, errorMessage) &&
        op.Inputs[0].Space == "const" &&
        (op.Inputs[0].Offset == 17 || op.Inputs[0].Offset == 18)) {
      return true;
    }

    // Ghidra x86 emits MOVMSKPD as a userop.  The operation extracts the sign
    // bit of each packed double lane into the low bits of the integer result.
    if (op.Output && !op.Inputs.empty() && op.Inputs[0].Space == "const" &&
        op.Inputs[0].Offset == 128) {
      return lowerX86Movmskpd(op, errorMessage);
    }
    return lowerHelperCall(op, errorMessage);
  }

  bool lowerOp(const PcodeOpView &op, std::string &errorMessage) {
    switch (op.Opcode) {
    case PcodeOpcode::Copy:
      if (!op.Output || !requireInputCount(op, 1, errorMessage)) {
        return false;
      }
      {
        std::optional<uint64_t> source = sourceRam(op.Inputs[0]);
        write(*op.Output, read(op.Inputs[0]));
        setSourceRam(*op.Output, source);
      }
      return true;
    case PcodeOpcode::Load:
      return lowerLoad(op, errorMessage);
    case PcodeOpcode::Store:
      return lowerStore(op, errorMessage);
    case PcodeOpcode::Call:
      return lowerCall(op, errorMessage);
    case PcodeOpcode::CallInd:
      return lowerCallInd(op, errorMessage);
    case PcodeOpcode::CallOther:
      return lowerCallOther(op, errorMessage);
    case PcodeOpcode::Branch:
    case PcodeOpcode::BranchInd:
    case PcodeOpcode::CBranch:
    case PcodeOpcode::Return:
      errorMessage = op.OpcodeName + " appeared in non-terminator position";
      return false;
    case PcodeOpcode::IntAdd:
    case PcodeOpcode::IntSub:
    case PcodeOpcode::IntMult:
    case PcodeOpcode::IntDiv:
    case PcodeOpcode::IntSDiv:
    case PcodeOpcode::IntRem:
    case PcodeOpcode::IntSRem:
    case PcodeOpcode::IntAnd:
    case PcodeOpcode::IntOr:
    case PcodeOpcode::IntXor:
    case PcodeOpcode::IntLeft:
    case PcodeOpcode::IntRight:
    case PcodeOpcode::IntSRight:
      return lowerBinary(op, errorMessage);
    case PcodeOpcode::IntEqual:
    case PcodeOpcode::IntNotEqual:
    case PcodeOpcode::IntLess:
    case PcodeOpcode::IntSLess:
    case PcodeOpcode::IntLessEqual:
    case PcodeOpcode::IntSLessEqual:
      return lowerCompare(op, errorMessage);
    case PcodeOpcode::IntZExt:
    case PcodeOpcode::IntSExt:
      return lowerCast(op, errorMessage);
    case PcodeOpcode::IntNegate:
    case PcodeOpcode::Int2Comp:
      return lowerUnary(op, errorMessage);
    case PcodeOpcode::Cast:
    case PcodeOpcode::Indirect:
      return lowerCopyLike(op, errorMessage);
    case PcodeOpcode::Piece:
      return lowerPiece(op, errorMessage);
    case PcodeOpcode::Subpiece:
      return lowerSubpiece(op, errorMessage);
    case PcodeOpcode::PtrAdd:
      return lowerPtrAdd(op, errorMessage);
    case PcodeOpcode::PtrSub:
      return lowerPtrSub(op, errorMessage);
    case PcodeOpcode::Popcount:
    case PcodeOpcode::Lzcount:
      return lowerCountBits(op, errorMessage);
    case PcodeOpcode::IntCarry:
    case PcodeOpcode::IntSCarry:
      return lowerAddOverflow(op, errorMessage);
    case PcodeOpcode::IntSBorrow:
      return lowerSignedBorrow(op, errorMessage);
    case PcodeOpcode::BoolNegate:
      return lowerBoolNegate(op, errorMessage);
    case PcodeOpcode::BoolAnd:
    case PcodeOpcode::BoolOr:
    case PcodeOpcode::BoolXor:
      return lowerBoolBinary(op, errorMessage);
    case PcodeOpcode::FloatEqual:
    case PcodeOpcode::FloatNotEqual:
    case PcodeOpcode::FloatLess:
    case PcodeOpcode::FloatLessEqual:
      return lowerFloatCompare(op, errorMessage);
    case PcodeOpcode::FloatNan:
      return lowerFloatNan(op, errorMessage);
    case PcodeOpcode::FloatAdd:
    case PcodeOpcode::FloatDiv:
    case PcodeOpcode::FloatMult:
    case PcodeOpcode::FloatSub:
      return lowerFloatBinary(op, errorMessage);
    case PcodeOpcode::FloatInt2Float:
    case PcodeOpcode::FloatFloat2Float:
    case PcodeOpcode::FloatTrunc:
      return lowerFloatCast(op, errorMessage);
    case PcodeOpcode::FloatNeg:
    case PcodeOpcode::FloatAbs:
    case PcodeOpcode::FloatSqrt:
    case PcodeOpcode::FloatCeil:
    case PcodeOpcode::FloatFloor:
    case PcodeOpcode::FloatRound:
      return lowerFloatUnary(op, errorMessage);
    case PcodeOpcode::SegmentOp:
    case PcodeOpcode::CpoolRef:
    case PcodeOpcode::New:
    case PcodeOpcode::Insert:
    case PcodeOpcode::Extract:
      return lowerHelperCall(op, errorMessage);
    case PcodeOpcode::Multiequal:
      errorMessage = "MULTIEQUAL is not expected in raw Sleigh P-Code";
      return false;
    case PcodeOpcode::Unsupported:
      break;
    }

    errorMessage = "unsupported p-code opcode: " + op.OpcodeName;
    return false;
  }

  llvm::LLVMContext &Context;
  llvm::Module &Module;
  llvm::Function &Function;
  llvm::IRBuilder<> &Builder;
  const PcodeLoweringConfig &Config;
  std::unordered_map<std::string, llvm::Value *> Values;
  std::map<std::pair<std::string, llvm::BasicBlock *>, llvm::Value *>
      NonDominatingReadValues;
  std::unordered_map<std::string, uint64_t> SourceRamByVarnode;
  std::vector<size_t> BlockStarts;
  std::vector<size_t> BlockEnds;
  std::unordered_map<size_t, uint64_t> NativeBlockAddressForStart;
  std::unordered_map<size_t, uint64_t> NativeParentBlockAddressForStart;
  std::unordered_map<size_t, uint64_t> NativeBlockEndForStart;
  std::set<size_t> NativeInternalPcodeStarts;
  std::unordered_map<size_t, llvm::BasicBlock *> BlockForStart;
  std::unordered_map<uint64_t, llvm::BasicBlock *> BlockForAddress;
  std::unordered_map<uint64_t, llvm::BasicBlock *> TailJumpBlockForAddress;
  std::set<size_t> SuppressedPcodeOpIndices;
  // op index of a folded get_pc thunk call -> (written register, constant
  // base).  The write is emitted at the suppressed call position.
  std::map<size_t, std::pair<VarnodeView, uint64_t>> ThunkBaseWrites;
  // First op index of an x87 instruction -> folded intrinsic description.
  std::map<size_t, X87IntrinsicSpec> X87Groups;
  std::vector<uint64_t> EmptyNativeBlockAddresses;
  std::vector<llvm::BasicBlock *> ExternalTargetBlocks;
  const std::vector<PcodeOpView> *CurrentProgramOps = nullptr;
  llvm::BasicBlock *ExitBlock = nullptr;
  llvm::GlobalVariable *Memory = nullptr;
  std::unique_ptr<RegisterStorage> Registers;
};

} // namespace

bool appendPcodeFunction(llvm::LLVMContext &context, llvm::Module &module,
                         const PcodeProgram &program,
                         const PcodeLoweringConfig &config,
                         std::string &errorMessage) {
  if (auto pointerByteSize =
          inferPointerByteSizeFromRegisters(program.Registers)) {
    setModuleDataLayoutFromPointerSize(module, *pointerByteSize,
                                       program.IsBigEndian);
  }

  auto *functionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
  llvm::Function *function = module.getFunction(config.EntryFunctionName);
  if (function != nullptr) {
    if (!function->empty()) {
      errorMessage = "duplicate function name: " + config.EntryFunctionName;
      return false;
    }
    if (function->getFunctionType() != functionType) {
      errorMessage =
          "function declaration type mismatch: " + config.EntryFunctionName;
      return false;
    }
    function->setLinkage(config.EntryFunctionLinkage);
  } else {
    function = llvm::Function::Create(functionType,
                                      config.EntryFunctionLinkage,
                                      config.EntryFunctionName, &module);
  }

  auto *entryBlock = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entryBlock);
  PcodeLowerer lowerer(context, module, *function, builder, config);
  if (!lowerer.lower(program, errorMessage)) {
    function->eraseFromParent();
    return false;
  }
  return true;
}

std::unique_ptr<llvm::Module>
buildPcodeModule(llvm::LLVMContext &context, const PcodeProgram &program,
                 const PcodeLoweringConfig &config, std::string &errorMessage) {
  auto module = std::make_unique<llvm::Module>(config.ModuleName, context);
  if (!appendPcodeFunction(context, *module, program, config, errorMessage)) {
    return nullptr;
  }
  return module;
}

} // namespace notdec::bin2llvm
