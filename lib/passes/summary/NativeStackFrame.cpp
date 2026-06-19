#include "notdec-bin2llvm/passes/summary/NativeStackFrame.h"

#include "notdec-bin2llvm/NativeAbi.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace notdec::bin2llvm {
namespace {

struct StaticStackAccess {
  llvm::Instruction *Memory = nullptr;
  llvm::IntToPtrInst *Pointer = nullptr;
  int64_t Offset = 0;
  uint64_t Size = 0;
};

std::optional<std::string> mdField(const llvm::MDNode *node,
                                   llvm::StringRef key) {
  if (node == nullptr) {
    return std::nullopt;
  }
  std::string prefix = (key + "=").str();
  for (const llvm::MDOperand &operand : node->operands()) {
    auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
    if (text == nullptr) {
      continue;
    }
    llvm::StringRef value = text->getString();
    if (value.starts_with(prefix)) {
      return value.drop_front(prefix.size()).str();
    }
  }
  return std::nullopt;
}

bool accessMatchesRegister(const llvm::MDNode *access,
                           llvm::StringRef registerName) {
  if (access == nullptr) {
    return false;
  }
  if (auto name = mdField(access, "name")) {
    if (*name == registerName) {
      return true;
    }
  }
  if (auto base = mdField(access, "base")) {
    return *base == registerName;
  }
  return false;
}

std::string registerName(const llvm::GlobalVariable &global) {
  if (auto name = mdField(global.getMetadata("notdec.register"), "name")) {
    if (!name->empty()) {
      return *name;
    }
  }
  return global.getName().str();
}

bool isRegisterAccess(const llvm::Instruction &inst,
                      llvm::StringRef registerName) {
  return accessMatchesRegister(inst.getMetadata("notdec.register.access"),
                               registerName);
}

bool loadReadsRegister(const llvm::LoadInst &load,
                       llvm::StringRef wantedRegister) {
  if (isRegisterAccess(load, wantedRegister)) {
    return true;
  }
  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      load.getPointerOperand()->stripPointerCasts());
  return global != nullptr && global->getMetadata("notdec.register") != nullptr &&
         registerName(*global) == wantedRegister;
}

bool valueUsesRegisterLoad(llvm::Value &value, llvm::StringRef registerName,
                           unsigned depth = 0) {
  if (depth >= 12) {
    return false;
  }
  llvm::Value *stripped = value.stripPointerCasts();
  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(stripped)) {
    return loadReadsRegister(*load, registerName);
  }
  if (auto *phi = llvm::dyn_cast<llvm::PHINode>(stripped)) {
    for (llvm::Value *incoming : phi->incoming_values()) {
      if (valueUsesRegisterLoad(*incoming, registerName, depth + 1)) {
        return true;
      }
    }
    return false;
  }
  auto *op = llvm::dyn_cast<llvm::Operator>(stripped);
  if (op == nullptr) {
    return false;
  }
  for (llvm::Value *operand : op->operands()) {
    if (valueUsesRegisterLoad(*operand, registerName, depth + 1)) {
      return true;
    }
  }
  return false;
}

std::optional<int64_t> signedConstantValue(llvm::Value *value) {
  auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value);
  if (constant == nullptr || constant->getBitWidth() > 64) {
    return std::nullopt;
  }
  return constant->getSExtValue();
}

std::optional<int64_t> stackOffsetFromBase(llvm::Value *value,
                                           llvm::Value *base,
                                           std::set<llvm::Value *> &seen) {
  value = value->stripPointerCasts();
  if (value == base) {
    return 0;
  }
  if (!seen.insert(value).second) {
    return std::nullopt;
  }

  auto *op = llvm::dyn_cast<llvm::Operator>(value);
  if (op == nullptr) {
    return std::nullopt;
  }
  if (op->getOpcode() == llvm::Instruction::Add) {
    if (auto lhs = stackOffsetFromBase(op->getOperand(0), base, seen)) {
      if (auto rhs = signedConstantValue(op->getOperand(1))) {
        return *lhs + *rhs;
      }
    }
    if (auto rhs = stackOffsetFromBase(op->getOperand(1), base, seen)) {
      if (auto lhs = signedConstantValue(op->getOperand(0))) {
        return *lhs + *rhs;
      }
    }
  }
  if (op->getOpcode() == llvm::Instruction::Sub) {
    if (auto lhs = stackOffsetFromBase(op->getOperand(0), base, seen)) {
      if (auto rhs = signedConstantValue(op->getOperand(1))) {
        return *lhs - *rhs;
      }
    }
  }
  return std::nullopt;
}

std::optional<uint64_t> fixedTypeStoreSize(const llvm::DataLayout &layout,
                                           llvm::Type *type) {
  llvm::TypeSize size = layout.getTypeStoreSize(type);
  if (size.isScalable()) {
    return std::nullopt;
  }
  return size.getFixedValue();
}

std::optional<uint64_t> memoryAccessSize(const llvm::DataLayout &layout,
                                         llvm::Instruction &instruction) {
  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
    return fixedTypeStoreSize(layout, load->getType());
  }
  if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
    return fixedTypeStoreSize(layout, store->getValueOperand()->getType());
  }
  return std::nullopt;
}

bool hasExistingStackAlloca(const llvm::Function &function) {
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &instruction : block) {
      auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
      if (alloca != nullptr && alloca->hasName() &&
          alloca->getName().starts_with("notdec_stack")) {
        return true;
      }
    }
  }
  return false;
}

llvm::LoadInst *singleRegisterLoad(llvm::Function &function,
                                   llvm::StringRef registerName) {
  llvm::LoadInst *result = nullptr;
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &instruction : block) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (load == nullptr || !loadReadsRegister(*load, registerName)) {
        continue;
      }
      if (result != nullptr) {
        return nullptr;
      }
      result = load;
    }
  }
  return result;
}

std::optional<llvm::Value *>
mergeKnownValueAtBlockEntry(llvm::BasicBlock &block,
                            const std::map<llvm::BasicBlock *, llvm::Value *>
                                &blockOut) {
  llvm::Value *merged = nullptr;
  bool sawPredecessor = false;
  for (llvm::BasicBlock *predecessor : llvm::predecessors(&block)) {
    sawPredecessor = true;
    auto found = blockOut.find(predecessor);
    if (found == blockOut.end() || found->second == nullptr) {
      return std::nullopt;
    }
    if (merged == nullptr) {
      merged = found->second;
    } else if (merged != found->second) {
      return std::nullopt;
    }
  }
  if (!sawPredecessor) {
    return std::nullopt;
  }
  return merged;
}

llvm::Value *knownFrameValueAfterBlock(llvm::BasicBlock &block,
                                       llvm::Value *currentValue,
                                       llvm::StringRef frameRegisterName,
                                       llvm::StringRef stackRegisterName) {
  for (llvm::Instruction &instruction : block) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
    if (store == nullptr || !isRegisterAccess(*store, frameRegisterName)) {
      continue;
    }
    llvm::Value *storedValue = store->getValueOperand();
    currentValue = valueUsesRegisterLoad(*storedValue, stackRegisterName)
                       ? storedValue
                       : nullptr;
  }
  return currentValue;
}

uint64_t replaceFramePointerLoads(llvm::Function &function,
                                  llvm::StringRef frameRegisterName,
                                  llvm::StringRef stackRegisterName) {
  std::map<llvm::BasicBlock *, llvm::Value *> blockOut;
  bool changed = true;
  size_t iterations = 0;
  size_t maxIterations = std::max<size_t>(1, function.size() * 4);
  while (changed && iterations++ < maxIterations) {
    changed = false;
    for (llvm::BasicBlock &block : function) {
      llvm::Value *entryValue = nullptr;
      if (auto merged = mergeKnownValueAtBlockEntry(block, blockOut)) {
        entryValue = *merged;
      }
      llvm::Value *outValue = knownFrameValueAfterBlock(
          block, entryValue, frameRegisterName, stackRegisterName);
      auto found = blockOut.find(&block);
      if (found == blockOut.end() || found->second != outValue) {
        blockOut[&block] = outValue;
        changed = true;
      }
    }
  }

  std::vector<llvm::LoadInst *> deadLoads;
  for (llvm::BasicBlock &block : function) {
    llvm::Value *currentValue = nullptr;
    if (auto merged = mergeKnownValueAtBlockEntry(block, blockOut)) {
      currentValue = *merged;
    }
    for (llvm::Instruction &instruction : block) {
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        if (currentValue != nullptr &&
            loadReadsRegister(*load, frameRegisterName)) {
          load->replaceAllUsesWith(currentValue);
          deadLoads.push_back(load);
          continue;
        }
      }
      auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
      if (store == nullptr || !isRegisterAccess(*store, frameRegisterName)) {
        continue;
      }
      llvm::Value *storedValue = store->getValueOperand();
      currentValue = valueUsesRegisterLoad(*storedValue, stackRegisterName)
                         ? storedValue
                         : nullptr;
    }
  }

  for (llvm::LoadInst *load : deadLoads) {
    load->eraseFromParent();
  }
  return deadLoads.size();
}

std::vector<std::string> framePointerRegisterNames(const NativeAbiSpec &abi) {
  std::vector<std::string> names;
  for (const NativeAbiEffect &effect : abi.Effects) {
    if (effect.Kind == NativeAbiEffectKind::Unaffected &&
        effect.Storage.Kind == NativeAbiStorageKind::Register &&
        (effect.Storage.Name == "RBP" || effect.Storage.Name == "EBP" ||
         effect.Storage.Name == "BP")) {
      names.push_back(effect.Storage.Name);
    }
  }
  return names;
}

bool rewriteFunctionStackAccesses(llvm::Function &function,
                                  llvm::StringRef stackRegisterName,
                                  NativeStackFrameRewriteSummary &summary) {
  if (hasExistingStackAlloca(function)) {
    return false;
  }
  llvm::LoadInst *stackBase = singleRegisterLoad(function, stackRegisterName);
  if (stackBase == nullptr) {
    return false;
  }

  const llvm::DataLayout &layout = function.getParent()->getDataLayout();
  std::vector<StaticStackAccess> accesses;
  int64_t low = 0;
  int64_t high = 0;
  bool sawAccess = false;
  bool failed = false;
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &instruction : block) {
      auto *pointer = llvm::dyn_cast<llvm::IntToPtrInst>(&instruction);
      if (pointer == nullptr) {
        continue;
      }
      std::set<llvm::Value *> seen;
      std::optional<int64_t> offset =
          stackOffsetFromBase(pointer->getOperand(0), stackBase, seen);
      if (!offset || *offset >= 0) {
        continue;
      }

      for (llvm::User *user : pointer->users()) {
        auto *memory = llvm::dyn_cast<llvm::Instruction>(user);
        if (memory == nullptr ||
            (llvm::isa<llvm::LoadInst>(memory) && memory->getOperand(0) != pointer) ||
            (llvm::isa<llvm::StoreInst>(memory) && memory->getOperand(1) != pointer) ||
            (!llvm::isa<llvm::LoadInst>(memory) &&
             !llvm::isa<llvm::StoreInst>(memory))) {
          failed = true;
          break;
        }
        std::optional<uint64_t> size = memoryAccessSize(layout, *memory);
        if (!size || *size == 0) {
          failed = true;
          break;
        }
        int64_t end = *offset + static_cast<int64_t>(*size);
        if (end > 0) {
          failed = true;
          break;
        }
        accesses.push_back({memory, pointer, *offset, *size});
        low = sawAccess ? std::min(low, *offset) : *offset;
        high = sawAccess ? std::max(high, end) : end;
        sawAccess = true;
      }
      if (failed) {
        break;
      }
    }
    if (failed) {
      break;
    }
  }
  if (failed || accesses.empty() || high <= low) {
    return false;
  }

  uint64_t frameSize = static_cast<uint64_t>(high - low);
  if (frameSize > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    return false;
  }

  llvm::IRBuilder<> entryBuilder(&function.getEntryBlock(),
                                 function.getEntryBlock().begin());
  auto *byteType = llvm::Type::getInt8Ty(function.getContext());
  auto *arrayType = llvm::ArrayType::get(byteType, frameSize);
  llvm::AllocaInst *storage =
      entryBuilder.CreateAlloca(arrayType, nullptr, "notdec_stack.native");
  storage->setAlignment(llvm::Align(16));

  for (const StaticStackAccess &access : accesses) {
    llvm::IRBuilder<> builder(access.Memory);
    llvm::Value *byteOffset = llvm::ConstantInt::get(
        llvm::Type::getInt64Ty(function.getContext()),
        static_cast<uint64_t>(access.Offset - low));
    llvm::Value *localPointer = builder.CreateInBoundsGEP(
        llvm::Type::getInt8Ty(function.getContext()), storage, byteOffset,
        "notdec_stack.native.ptr");
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(access.Memory)) {
      load->setOperand(0, localPointer);
    } else if (auto *store = llvm::dyn_cast<llvm::StoreInst>(access.Memory)) {
      store->setOperand(1, localPointer);
    }
  }

  std::set<llvm::IntToPtrInst *> pointers;
  for (const StaticStackAccess &access : accesses) {
    if (pointers.insert(access.Pointer).second && access.Pointer->use_empty()) {
      llvm::RecursivelyDeleteTriviallyDeadInstructions(access.Pointer);
    }
  }
  summary.AccessesRewritten += accesses.size();
  return true;
}

} // namespace

NativeStackFrameRewriteSummary runNativeStackFrameRewrite(
    llvm::Module &module, const NativeStackFrameRewriteOptions &options) {
  NativeStackFrameRewriteSummary summary;
  std::optional<NativeAbiSpec> abi = readNativeAbiMetadata(module);
  if (!abi || abi->StackPointerRegister.empty()) {
    return summary;
  }

  summary.IgnoredRegisters.insert(abi->StackPointerRegister);
  std::vector<std::string> framePointers = framePointerRegisterNames(*abi);
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    ++summary.FunctionsSeen;
    bool rewritten = false;
    for (const std::string &framePointer : framePointers) {
      uint64_t replaced = replaceFramePointerLoads(
          function, framePointer, abi->StackPointerRegister);
      if (replaced != 0) {
        summary.FramePointerLoadsReplaced += replaced;
        summary.IgnoredRegisters.insert(framePointer);
        rewritten = true;
      }
    }
    if (rewriteFunctionStackAccesses(function, abi->StackPointerRegister,
                                     summary)) {
      rewritten = true;
    }
    if (rewritten) {
      ++summary.FunctionsRewritten;
    }
  }

  if (options.PrintSummary) {
    printNativeStackFrameRewriteSummary(summary, llvm::errs());
  }
  return summary;
}

void printNativeStackFrameRewriteSummary(
    const NativeStackFrameRewriteSummary &summary, llvm::raw_ostream &os) {
  os << "Native stack frame rewrite: functions=" << summary.FunctionsSeen
     << " rewritten=" << summary.FunctionsRewritten
     << " accesses=" << summary.AccessesRewritten
     << " frame_pointer_loads=" << summary.FramePointerLoadsReplaced
     << " ignored_registers=";
  for (const std::string &name : summary.IgnoredRegisters) {
    os << name << ' ';
  }
  os << '\n';
}

} // namespace notdec::bin2llvm
