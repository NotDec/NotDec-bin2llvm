#include "notdec-bin2llvm/passes/summary/NativeStackFrame.h"

#include "notdec-bin2llvm/NativeAbi.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Local.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace notdec::bin2llvm {
namespace {

struct StackFrameAddressValue {
  llvm::Instruction *Instruction = nullptr;
  int64_t Offset = 0;
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

bool isStackAlignmentMask(llvm::Value *value) {
  auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value);
  if (constant == nullptr || constant->getBitWidth() > 64) {
    return false;
  }
  int64_t mask = constant->getSExtValue();
  if (mask >= -1) {
    return false;
  }
  uint64_t alignment = static_cast<uint64_t>(-mask);
  return llvm::isPowerOf2_64(alignment);
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
  if (op->getOpcode() == llvm::Instruction::And) {
    if (auto lhs = stackOffsetFromBase(op->getOperand(0), base, seen)) {
      if (*lhs == 0 && isStackAlignmentMask(op->getOperand(1))) {
        return 0;
      }
    }
    if (auto rhs = stackOffsetFromBase(op->getOperand(1), base, seen)) {
      if (*rhs == 0 && isStackAlignmentMask(op->getOperand(0))) {
        return 0;
      }
    }
  }
  return std::nullopt;
}

std::optional<int64_t> functionStackLowOffset(llvm::Function &function,
                                              llvm::Value *stackBase) {
  std::optional<int64_t> low;
  for (llvm::Instruction &instruction : llvm::instructions(function)) {
    std::set<llvm::Value *> seen;
    std::optional<int64_t> offset =
        stackOffsetFromBase(&instruction, stackBase, seen);
    if (!offset || *offset >= 0) {
      continue;
    }
    low = low ? std::min(*low, *offset) : *offset;
  }
  return low;
}

llvm::Value *createStackFramePointer(llvm::IRBuilder<> &builder,
                                     llvm::AllocaInst &storage,
                                     int64_t frameLow, int64_t offset,
                                     llvm::StringRef name) {
  llvm::Value *byteOffset = llvm::ConstantInt::get(
      llvm::Type::getInt64Ty(storage.getContext()),
      static_cast<uint64_t>(offset - frameLow));
  return builder.CreateInBoundsGEP(llvm::Type::getInt8Ty(storage.getContext()),
                                   &storage, byteOffset, name);
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

bool isFramePointerRegisterName(llvm::StringRef name) {
  return name == "RBP" || name == "EBP" || name == "BP";
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

  std::optional<int64_t> low = functionStackLowOffset(function, stackBase);
  if (!low) {
    return false;
  }
  uint64_t frameSize = static_cast<uint64_t>(-*low);
  if (frameSize > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    return false;
  }

  std::vector<StackFrameAddressValue> integerAddresses;
  std::vector<StackFrameAddressValue> pointerAddresses;
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &instruction : block) {
      if (auto *pointer = llvm::dyn_cast<llvm::IntToPtrInst>(&instruction)) {
        std::set<llvm::Value *> seen;
        std::optional<int64_t> offset =
            stackOffsetFromBase(pointer->getOperand(0), stackBase, seen);
        if (offset && *offset >= *low && *offset < 0) {
          pointerAddresses.push_back({&instruction, *offset});
        }
        continue;
      }
      std::set<llvm::Value *> seen;
      std::optional<int64_t> offset =
          stackOffsetFromBase(&instruction, stackBase, seen);
      if (!offset || *offset < *low || *offset >= 0) {
        continue;
      }
      if (instruction.getType()->isIntegerTy()) {
        integerAddresses.push_back({&instruction, *offset});
      }
    }
  }
  if (integerAddresses.empty() && pointerAddresses.empty()) {
    return false;
  }

  llvm::IRBuilder<> entryBuilder(&function.getEntryBlock(),
                                 function.getEntryBlock().begin());
  auto *byteType = llvm::Type::getInt8Ty(function.getContext());
  auto *arrayType = llvm::ArrayType::get(byteType, frameSize);
  llvm::AllocaInst *storage =
      entryBuilder.CreateAlloca(arrayType, nullptr, "notdec_stack.native");
  storage->setAlignment(llvm::Align(16));

  // Keep one entry-dominating pointer per concrete native stack offset.  The
  // cleanup pass may compare rewritten stack accesses by pointer identity, so
  // repeated offsets should not produce unrelated GEP values.
  llvm::IRBuilder<> stackValueBuilder(storage->getNextNode());
  std::map<int64_t, llvm::Value *> stackPointers;
  std::map<std::pair<int64_t, llvm::Type *>, llvm::Value *> stackIntegers;
  auto stackPointerForOffset = [&](int64_t offset) -> llvm::Value * {
    auto found = stackPointers.find(offset);
    if (found != stackPointers.end()) {
      return found->second;
    }
    llvm::Value *pointer = createStackFramePointer(
        stackValueBuilder, *storage, *low, offset, "notdec_stack.native.ptr");
    stackPointers[offset] = pointer;
    return pointer;
  };
  auto stackIntegerForOffset = [&](int64_t offset,
                                   llvm::Type *integerType) -> llvm::Value * {
    auto key = std::make_pair(offset, integerType);
    auto found = stackIntegers.find(key);
    if (found != stackIntegers.end()) {
      return found->second;
    }
    llvm::Value *integer = stackValueBuilder.CreatePtrToInt(
        stackPointerForOffset(offset), integerType, "notdec_stack.native.int");
    stackIntegers[key] = integer;
    return integer;
  };

  uint64_t rewritten = 0;
  for (const StackFrameAddressValue &address : pointerAddresses) {
    if (address.Instruction->getParent() == nullptr) {
      continue;
    }
    llvm::Value *localPointer = stackPointerForOffset(address.Offset);
    address.Instruction->replaceAllUsesWith(localPointer);
    ++rewritten;
  }

  for (const StackFrameAddressValue &address : integerAddresses) {
    if (address.Instruction->getParent() == nullptr ||
        address.Instruction->use_empty()) {
      continue;
    }
    llvm::Value *localInteger = stackIntegerForOffset(
        address.Offset, address.Instruction->getType());
    address.Instruction->replaceAllUsesWith(localInteger);
    ++rewritten;
  }

  for (const StackFrameAddressValue &address : pointerAddresses) {
    if (address.Instruction->getParent() != nullptr &&
        address.Instruction->use_empty()) {
      llvm::RecursivelyDeleteTriviallyDeadInstructions(address.Instruction);
    }
  }
  for (const StackFrameAddressValue &address : integerAddresses) {
    if (address.Instruction->getParent() != nullptr &&
        address.Instruction->use_empty()) {
      llvm::RecursivelyDeleteTriviallyDeadInstructions(address.Instruction);
    }
  }
  if (rewritten == 0) {
    if (storage->use_empty()) {
      storage->eraseFromParent();
    }
    return false;
  }
  summary.AccessesRewritten += rewritten;
  return true;
}

void eraseInstructionOnly(llvm::Instruction &inst) {
  inst.eraseFromParent();
}

bool stackAllocaPointer(llvm::Value *value) {
  value = value->stripPointerCasts();
  auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(value);
  if (alloca != nullptr) {
    return alloca->hasName() &&
           alloca->getName().starts_with("notdec_stack.native");
  }
  auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value);
  return gep != nullptr && stackAllocaPointer(gep->getPointerOperand());
}

bool stackAllocaStoreIsLoaded(
    llvm::StoreInst &store, const std::vector<llvm::LoadInst *> &loads) {
  llvm::Value *storePointer = store.getPointerOperand()->stripPointerCasts();
  for (llvm::LoadInst *load : loads) {
    if (load->getPointerOperand()->stripPointerCasts() == storePointer) {
      return true;
    }
  }
  return false;
}

void cleanupStackAllocaAccesses(llvm::Function &function,
                                NativeStackFrameCleanupSummary &summary) {
  bool changed = true;
  while (changed) {
    changed = false;
    std::vector<llvm::LoadInst *> stackLoads;
    std::vector<llvm::StoreInst *> stackStores;
    std::vector<llvm::AllocaInst *> stackAllocas;
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&inst)) {
        if (alloca->hasName() &&
            alloca->getName().starts_with("notdec_stack.native")) {
          stackAllocas.push_back(alloca);
        }
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        if (stackAllocaPointer(load->getPointerOperand())) {
          stackLoads.push_back(load);
        }
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        if (stackAllocaPointer(store->getPointerOperand())) {
          stackStores.push_back(store);
        }
      }
    }

    for (llvm::LoadInst *load : stackLoads) {
      if (load->getParent() != nullptr && load->use_empty() &&
          !load->isVolatile() && !load->isAtomic()) {
        eraseInstructionOnly(*load);
        ++summary.StackAllocaLoadsRemoved;
        changed = true;
      }
    }
    if (changed) {
      continue;
    }

    stackLoads.clear();
    stackStores.clear();
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        if (stackAllocaPointer(load->getPointerOperand())) {
          stackLoads.push_back(load);
        }
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        if (stackAllocaPointer(store->getPointerOperand())) {
          stackStores.push_back(store);
        }
      }
    }
    for (llvm::StoreInst *store : stackStores) {
      if (store->getParent() != nullptr && !store->isVolatile() &&
          !store->isAtomic() && !stackAllocaStoreIsLoaded(*store, stackLoads)) {
        eraseInstructionOnly(*store);
        ++summary.StackAllocaStoresRemoved;
        changed = true;
      }
    }

    for (llvm::AllocaInst *alloca : stackAllocas) {
      if (alloca->getParent() != nullptr && alloca->use_empty()) {
        alloca->eraseFromParent();
        ++summary.StackAllocasRemoved;
        changed = true;
      }
    }
  }
}

void cleanupRegisterBookkeeping(llvm::Function &function,
                                const std::set<std::string> &registers,
                                NativeStackFrameCleanupSummary &summary) {
  std::map<llvm::BasicBlock *, std::set<std::string>> liveIn;
  std::map<llvm::BasicBlock *, std::set<std::string>> liveOut;
  std::vector<llvm::BasicBlock *> blocks;
  for (llvm::BasicBlock &block : function) {
    blocks.push_back(&block);
  }

  auto transferBlock = [&](llvm::BasicBlock &block,
                           std::set<std::string> live) {
    for (auto instIt = block.rbegin(); instIt != block.rend(); ++instIt) {
      llvm::Instruction &inst = *instIt;
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        if (!load->use_empty()) {
          for (const std::string &name : registers) {
            if (loadReadsRegister(*load, name)) {
              live.insert(name);
              break;
            }
          }
        }
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        for (const std::string &name : registers) {
          if (isRegisterAccess(*store, name)) {
            live.erase(name);
            break;
          }
        }
      }
    }
    return live;
  };

  bool changed = true;
  while (changed) {
    changed = false;
    for (auto blockIt = blocks.rbegin(); blockIt != blocks.rend(); ++blockIt) {
      llvm::BasicBlock &block = **blockIt;
      std::set<std::string> out;
      for (llvm::BasicBlock *succ : llvm::successors(&block)) {
        auto found = liveIn.find(succ);
        if (found != liveIn.end()) {
          out.insert(found->second.begin(), found->second.end());
        }
      }
      std::set<std::string> in = transferBlock(block, out);
      changed |= liveOut[&block] != out || liveIn[&block] != in;
      liveOut[&block] = std::move(out);
      liveIn[&block] = std::move(in);
    }
  }

  std::vector<llvm::Instruction *> dead;
  for (llvm::BasicBlock &block : function) {
    std::set<std::string> live = liveOut[&block];
    for (auto instIt = block.rbegin(); instIt != block.rend(); ++instIt) {
      llvm::Instruction &inst = *instIt;
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        for (const std::string &name : registers) {
          if (loadReadsRegister(*load, name)) {
            if (load->use_empty()) {
              dead.push_back(load);
            } else {
              live.insert(name);
            }
            break;
          }
        }
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        for (const std::string &name : registers) {
          if (isRegisterAccess(*store, name)) {
            if (live.count(name) == 0) {
              dead.push_back(store);
            } else {
              live.erase(name);
            }
            break;
          }
        }
      }
    }
  }

  for (llvm::Instruction *inst : dead) {
    if (inst->getParent() == nullptr) {
      continue;
    }
    if (llvm::isa<llvm::LoadInst>(inst)) {
      ++summary.RegisterLoadsRemoved;
    } else if (llvm::isa<llvm::StoreInst>(inst)) {
      ++summary.RegisterStoresRemoved;
    }
    eraseInstructionOnly(*inst);
  }
}

} // namespace

NativeStackFrameRewriteSummary runNativeStackFrameRewrite(
    llvm::Module &module, const NativeStackFrameRewriteOptions &options) {
  NativeStackFrameRewriteSummary summary;
  std::optional<NativeAbiSpec> abi = readNativeAbiMetadata(module);
  if (!abi || abi->StackPointerRegister.empty()) {
    return summary;
  }

  summary.StackPointerRegister = abi->StackPointerRegister;
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
        // The replacement is path-sensitive: it only changes loads whose
        // current value is proven to come from RSP.  The same register may
        // still carry arguments or ordinary local values elsewhere in this
        // function or another function, so it cannot be ignored module-wide.
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

NativeStackFrameCleanupSummary runNativeStackFrameCleanup(
    llvm::Module &module, const NativeStackFrameCleanupOptions &options) {
  NativeStackFrameCleanupSummary summary;
  if (options.Registers.empty()) {
    return summary;
  }
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    ++summary.FunctionsSeen;
    NativeStackFrameRewriteSummary rewriteSummary;
    for (const std::string &name : options.Registers) {
      if (isFramePointerRegisterName(name) &&
          !options.StackPointerRegister.empty()) {
        rewriteSummary.FramePointerLoadsReplaced += replaceFramePointerLoads(
            function, name, options.StackPointerRegister);
      }
    }
    if (!options.StackPointerRegister.empty() &&
        rewriteFunctionStackAccesses(function, options.StackPointerRegister,
                                     rewriteSummary)) {
      summary.AccessesRewritten += rewriteSummary.AccessesRewritten;
    }
    summary.FramePointerLoadsReplaced +=
        rewriteSummary.FramePointerLoadsReplaced;
    cleanupRegisterBookkeeping(function, options.Registers, summary);
    cleanupStackAllocaAccesses(function, summary);
    cleanupRegisterBookkeeping(function, options.Registers, summary);
  }
  if (options.PrintSummary) {
    printNativeStackFrameCleanupSummary(summary, llvm::errs());
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

void printNativeStackFrameCleanupSummary(
    const NativeStackFrameCleanupSummary &summary, llvm::raw_ostream &os) {
  os << "Native stack frame cleanup: functions=" << summary.FunctionsSeen
     << " accesses=" << summary.AccessesRewritten
     << " frame_pointer_loads=" << summary.FramePointerLoadsReplaced
     << " register_loads=" << summary.RegisterLoadsRemoved
     << " register_stores=" << summary.RegisterStoresRemoved
     << " alloca_loads=" << summary.StackAllocaLoadsRemoved
     << " alloca_stores=" << summary.StackAllocaStoresRemoved
     << " allocas=" << summary.StackAllocasRemoved << '\n';
}

} // namespace notdec::bin2llvm
