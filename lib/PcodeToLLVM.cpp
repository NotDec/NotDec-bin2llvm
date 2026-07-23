#include "notdec-bin2llvm/PcodeToLLVM.h"
#include "notdec-bin2llvm/RegisterStorage.h"

#include "llvm/ADT/APInt.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
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
        Context, Module, program.Registers, program.IsBigEndian);

    if (program.Ops.empty() && !usesNativeCfg()) {
      return true;
    }

    CurrentProgramOps = &program.Ops;
    prepareX86CallReturnStackSuppression(program);
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
        if (SuppressedPcodeOpIndices.count(opIndex) != 0) {
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
          llvm::Value *target = resize(read(op.Inputs[0]), 8);
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
      auto *address = llvm::ConstantInt::get(intType(8), varnode.Offset);
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

    llvm::Value *address = resize(read(op.Inputs[1]), 8);
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

    llvm::Value *address = resize(read(op.Inputs[1]), 8);
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
        Builder.CreateIntToPtr(resize(read(target), 8),
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
