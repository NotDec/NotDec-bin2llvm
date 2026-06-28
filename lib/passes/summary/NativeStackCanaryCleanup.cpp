#include "notdec-bin2llvm/passes/summary/NativeStackCanaryCleanup.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Local.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace notdec::bin2llvm {
namespace {

struct FsCanaryAddress {
  llvm::LoadInst *BaseLoad = nullptr;
  std::set<llvm::Instruction *> AddressChain;
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

bool loadReadsRegister(const llvm::LoadInst &load,
                       llvm::StringRef wantedRegister) {
  if (accessMatchesRegister(load.getMetadata("notdec.register.access"),
                            wantedRegister)) {
    return true;
  }
  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      load.getPointerOperand()->stripPointerCasts());
  return global != nullptr && global->getMetadata("notdec.register") != nullptr &&
         registerName(*global) == wantedRegister;
}

bool zeroIntegerConstant(llvm::Value *value) {
  auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value);
  return constant != nullptr && constant->isZero();
}

std::optional<int64_t> signedConstantValue(llvm::Value *value) {
  auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value);
  if (constant == nullptr || constant->getBitWidth() > 64) {
    return std::nullopt;
  }
  return constant->getSExtValue();
}

std::optional<int64_t>
integerOffsetFromBase(llvm::Value *value, llvm::Value *base,
                      std::set<llvm::Value *> &seen,
                      std::set<llvm::Instruction *> &chain) {
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

  std::optional<int64_t> result;
  if (op->getOpcode() == llvm::Instruction::Add) {
    if (auto lhs = integerOffsetFromBase(op->getOperand(0), base, seen, chain)) {
      if (auto rhs = signedConstantValue(op->getOperand(1))) {
        result = *lhs + *rhs;
      }
    }
    if (!result) {
      if (auto rhs =
              integerOffsetFromBase(op->getOperand(1), base, seen, chain)) {
        if (auto lhs = signedConstantValue(op->getOperand(0))) {
          result = *lhs + *rhs;
        }
      }
    }
  } else if (op->getOpcode() == llvm::Instruction::Sub) {
    if (auto lhs = integerOffsetFromBase(op->getOperand(0), base, seen, chain)) {
      if (auto rhs = signedConstantValue(op->getOperand(1))) {
        result = *lhs - *rhs;
      }
    }
  }

  if (result) {
    if (auto *instruction = llvm::dyn_cast<llvm::Instruction>(value)) {
      chain.insert(instruction);
    }
  }
  return result;
}

bool valueIsFsBase(llvm::Value *value, std::set<llvm::Value *> &visiting,
                   std::map<llvm::Value *, bool> &cache,
                   std::set<llvm::Instruction *> &chain,
                   llvm::LoadInst **baseLoad) {
  value = value->stripPointerCasts();
  auto cached = cache.find(value);
  if (cached != cache.end()) {
    return cached->second;
  }
  if (!visiting.insert(value).second) {
    return false;
  }
  auto finish = [&](bool result) {
    visiting.erase(value);
    cache[value] = result;
    return result;
  };

  auto *load = llvm::dyn_cast<llvm::LoadInst>(value);
  if (load != nullptr && loadReadsRegister(*load, "FS_OFFSET")) {
    if (baseLoad != nullptr && *baseLoad == nullptr) {
      *baseLoad = load;
    }
    chain.insert(load);
    return finish(true);
  }

  auto *phi = llvm::dyn_cast<llvm::PHINode>(value);
  if (phi == nullptr) {
    return finish(false);
  }

  // SummarySSA can leave an FS base PHI with real FS values on live edges and
  // zero on edges where the original register value was not used.  Only accept
  // that narrow shape so ordinary constants do not become TLS bases.
  bool sawFsInput = false;
  for (llvm::Value *incoming : phi->incoming_values()) {
    if (llvm::isa<llvm::UndefValue>(incoming) || zeroIntegerConstant(incoming)) {
      continue;
    }
    if (visiting.count(incoming->stripPointerCasts()) != 0) {
      continue;
    }
    if (!valueIsFsBase(incoming, visiting, cache, chain, baseLoad)) {
      return finish(false);
    }
    sawFsInput = true;
  }
  if (!sawFsInput) {
    return finish(false);
  }
  chain.insert(phi);
  return finish(true);
}

std::optional<int64_t> integerOffsetFromFsBase(
    llvm::Value *value, std::set<llvm::Value *> &visiting,
    std::map<llvm::Value *, std::optional<int64_t>> &cache,
    std::set<llvm::Instruction *> &chain, llvm::LoadInst **baseLoad) {
  value = value->stripPointerCasts();
  auto cached = cache.find(value);
  if (cached != cache.end()) {
    return cached->second;
  }
  if (!visiting.insert(value).second) {
    return std::nullopt;
  }
  auto finish = [&](std::optional<int64_t> result) {
    visiting.erase(value);
    cache[value] = result;
    return result;
  };

  std::set<llvm::Value *> baseVisiting;
  std::map<llvm::Value *, bool> baseCache;
  if (valueIsFsBase(value, baseVisiting, baseCache, chain, baseLoad)) {
    return finish(0);
  }
  if (std::optional<int64_t> constant = signedConstantValue(value)) {
    return finish(*constant);
  }

  if (auto *phi = llvm::dyn_cast<llvm::PHINode>(value)) {
    bool sawInput = false;
    for (llvm::Value *incoming : phi->incoming_values()) {
      if (visiting.count(incoming->stripPointerCasts()) != 0) {
        continue;
      }
      std::optional<int64_t> incomingOffset =
          integerOffsetFromFsBase(incoming, visiting, cache, chain, baseLoad);
      if (!incomingOffset) {
        return finish(std::nullopt);
      }
      if (*incomingOffset != 40) {
        return finish(std::nullopt);
      }
      sawInput = true;
    }
    if (!sawInput) {
      return finish(std::nullopt);
    }
    chain.insert(phi);
    return finish(40);
  }

  auto *op = llvm::dyn_cast<llvm::Operator>(value);
  if (op == nullptr) {
    return finish(std::nullopt);
  }

  std::optional<int64_t> result;
  if (op->getOpcode() == llvm::Instruction::Add) {
    if (auto lhs =
            integerOffsetFromFsBase(op->getOperand(0), visiting, cache, chain,
                                    baseLoad)) {
      if (auto rhs = signedConstantValue(op->getOperand(1))) {
        result = *lhs + *rhs;
      }
    }
    if (!result) {
      if (auto rhs = integerOffsetFromFsBase(op->getOperand(1), visiting,
                                             cache, chain, baseLoad)) {
        if (auto lhs = signedConstantValue(op->getOperand(0))) {
          result = *lhs + *rhs;
        }
      }
    }
  } else if (op->getOpcode() == llvm::Instruction::Sub) {
    if (auto lhs =
            integerOffsetFromFsBase(op->getOperand(0), visiting, cache, chain,
                                    baseLoad)) {
      if (auto rhs = signedConstantValue(op->getOperand(1))) {
        result = *lhs - *rhs;
      }
    }
  }

  if (result) {
    if (auto *instruction = llvm::dyn_cast<llvm::Instruction>(value)) {
      chain.insert(instruction);
    }
  }
  return finish(result);
}

llvm::Value *intToPtrIntegerOperand(llvm::Value *pointer) {
  if (auto *inst = llvm::dyn_cast<llvm::IntToPtrInst>(pointer)) {
    return inst->getOperand(0);
  }
  if (auto *constant = llvm::dyn_cast<llvm::ConstantExpr>(pointer)) {
    if (constant->getOpcode() == llvm::Instruction::IntToPtr) {
      return constant->getOperand(0);
    }
  }
  pointer = pointer->stripPointerCasts();
  if (auto *inst = llvm::dyn_cast<llvm::IntToPtrInst>(pointer)) {
    return inst->getOperand(0);
  }
  if (auto *constant = llvm::dyn_cast<llvm::ConstantExpr>(pointer)) {
    if (constant->getOpcode() == llvm::Instruction::IntToPtr) {
      return constant->getOperand(0);
    }
  }
  return nullptr;
}

std::optional<int64_t> pointerOffsetFromIntegerBase(
    llvm::Value *pointer, llvm::Value *base,
    std::set<llvm::Instruction *> &addressChain) {
  if (auto *inst = llvm::dyn_cast<llvm::Instruction>(pointer)) {
    addressChain.insert(inst);
  }
  llvm::Value *integer = intToPtrIntegerOperand(pointer);
  if (integer == nullptr) {
    return std::nullopt;
  }
  std::set<llvm::Value *> seen;
  return integerOffsetFromBase(integer, base, seen, addressChain);
}

std::optional<FsCanaryAddress>
findZeroBaseFsCanaryAddress(llvm::LoadInst &load) {
  std::set<llvm::Instruction *> addressChain;
  if (auto *inst = llvm::dyn_cast<llvm::Instruction>(load.getPointerOperand())) {
    addressChain.insert(inst);
  }

  llvm::Value *integer = intToPtrIntegerOperand(load.getPointerOperand());
  if (integer == nullptr) {
    return std::nullopt;
  }
  std::optional<int64_t> offset = signedConstantValue(integer);
  if (!offset || *offset != 40) {
    return std::nullopt;
  }

  // SummarySSA may replace an unneeded FS base with zero before the late canary
  // cleanup runs.  At that point FS:0x28 is visible as a plain address 40.
  FsCanaryAddress address;
  address.AddressChain = std::move(addressChain);
  return address;
}

std::optional<FsCanaryAddress>
findFsCanaryIntegerAddress(llvm::LoadInst &load) {
  std::set<llvm::Instruction *> addressChain;
  if (auto *inst = llvm::dyn_cast<llvm::Instruction>(load.getPointerOperand())) {
    addressChain.insert(inst);
  }

  llvm::Value *integer = intToPtrIntegerOperand(load.getPointerOperand());
  if (integer == nullptr) {
    return std::nullopt;
  }

  std::set<llvm::Value *> visiting;
  std::map<llvm::Value *, std::optional<int64_t>> cache;
  FsCanaryAddress address;
  address.AddressChain = std::move(addressChain);
  std::optional<int64_t> offset =
      integerOffsetFromFsBase(integer, visiting, cache, address.AddressChain,
                              &address.BaseLoad);
  if (offset && *offset == 40) {
    return address;
  }
  return std::nullopt;
}

bool stackAllocaPointer(llvm::Value *value) {
  value = value->stripPointerCasts();
  auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(value);
  if (alloca != nullptr) {
    return alloca->hasName() &&
           alloca->getName().starts_with("notdec_stack.native");
  }
  if (auto *gep = llvm::dyn_cast<llvm::GEPOperator>(value)) {
    return stackAllocaPointer(gep->getPointerOperand());
  }
  return false;
}

std::optional<int64_t> offsetFromFramePointer(llvm::Value *value,
                                              std::set<llvm::Value *> &seen) {
  value = value->stripPointerCasts();
  if (!seen.insert(value).second) {
    return std::nullopt;
  }

  auto *load = llvm::dyn_cast<llvm::LoadInst>(value);
  if (load != nullptr && (loadReadsRegister(*load, "RBP") ||
                          loadReadsRegister(*load, "EBP") ||
                          loadReadsRegister(*load, "BP"))) {
    return 0;
  }

  auto *op = llvm::dyn_cast<llvm::Operator>(value);
  if (op == nullptr) {
    return std::nullopt;
  }
  if (op->getOpcode() == llvm::Instruction::Add) {
    if (auto lhs = offsetFromFramePointer(op->getOperand(0), seen)) {
      if (auto rhs = signedConstantValue(op->getOperand(1))) {
        return *lhs + *rhs;
      }
    }
    if (auto rhs = offsetFromFramePointer(op->getOperand(1), seen)) {
      if (auto lhs = signedConstantValue(op->getOperand(0))) {
        return *lhs + *rhs;
      }
    }
  }
  if (op->getOpcode() == llvm::Instruction::Sub) {
    if (auto lhs = offsetFromFramePointer(op->getOperand(0), seen)) {
      if (auto rhs = signedConstantValue(op->getOperand(1))) {
        return *lhs - *rhs;
      }
    }
  }
  return std::nullopt;
}

bool framePointerSavedCanaryPointer(llvm::Value *pointer) {
  llvm::Value *integer = intToPtrIntegerOperand(pointer);
  if (integer == nullptr) {
    return false;
  }
  std::set<llvm::Value *> seen;
  std::optional<int64_t> offset = offsetFromFramePointer(integer, seen);
  return offset && *offset < 0;
}

bool loadIsSavedCanarySlot(llvm::LoadInst &load) {
  return !load.isVolatile() && !load.isAtomic() &&
         (stackAllocaPointer(load.getPointerOperand()) ||
          framePointerSavedCanaryPointer(load.getPointerOperand()));
}

std::optional<FsCanaryAddress> findFsCanaryAddress(llvm::LoadInst &load) {
  if (load.isVolatile() || load.isAtomic()) {
    return std::nullopt;
  }
  llvm::Function *function = load.getFunction();
  if (function == nullptr) {
    return std::nullopt;
  }
  for (llvm::BasicBlock &block : *function) {
    for (llvm::Instruction &inst : block) {
      auto *baseLoad = llvm::dyn_cast<llvm::LoadInst>(&inst);
      if (baseLoad == nullptr || !loadReadsRegister(*baseLoad, "FS_OFFSET")) {
        continue;
      }
      FsCanaryAddress address;
      address.BaseLoad = baseLoad;
      std::optional<int64_t> offset = pointerOffsetFromIntegerBase(
          load.getPointerOperand(), baseLoad, address.AddressChain);
      if (offset && *offset == 40) {
        return address;
      }
    }
  }
  for (llvm::BasicBlock &block : *function) {
    for (llvm::Instruction &inst : block) {
      std::set<llvm::Value *> visiting;
      std::map<llvm::Value *, bool> cache;
      FsCanaryAddress address;
      if (!valueIsFsBase(&inst, visiting, cache, address.AddressChain,
                         &address.BaseLoad)) {
        continue;
      }
      std::optional<int64_t> offset = pointerOffsetFromIntegerBase(
          load.getPointerOperand(), &inst, address.AddressChain);
      if (offset && *offset == 40) {
        return address;
      }
    }
  }
  if (std::optional<FsCanaryAddress> address =
          findZeroBaseFsCanaryAddress(load)) {
    return address;
  }
  if (std::optional<FsCanaryAddress> address =
          findFsCanaryIntegerAddress(load)) {
    return address;
  }
  return std::nullopt;
}

bool hasOnlyUser(llvm::Instruction &inst, llvm::User &expectedUser) {
  return llvm::all_of(inst.users(),
                      [&](llvm::User *user) { return user == &expectedUser; });
}

std::optional<bool> conditionTrueMeansCanaryEqual(llvm::Value *condition,
                                                  llvm::ICmpInst **equalCmp) {
  auto *cmp = llvm::dyn_cast_or_null<llvm::ICmpInst>(condition);
  if (cmp == nullptr) {
    return std::nullopt;
  }
  if (cmp->getPredicate() != llvm::ICmpInst::ICMP_EQ &&
      cmp->getPredicate() != llvm::ICmpInst::ICMP_NE) {
    return std::nullopt;
  }

  llvm::Value *maybeZero = nullptr;
  llvm::Value *maybeZext = nullptr;
  if (llvm::isa<llvm::ConstantInt>(cmp->getOperand(0))) {
    maybeZero = cmp->getOperand(0);
    maybeZext = cmp->getOperand(1);
  } else if (llvm::isa<llvm::ConstantInt>(cmp->getOperand(1))) {
    maybeZero = cmp->getOperand(1);
    maybeZext = cmp->getOperand(0);
  }
  auto *zero = llvm::dyn_cast_or_null<llvm::ConstantInt>(maybeZero);
  auto *zext = llvm::dyn_cast_or_null<llvm::ZExtInst>(maybeZext);
  if (zero != nullptr && zero->isZero() && zext != nullptr) {
    auto *inner = llvm::dyn_cast<llvm::ICmpInst>(zext->getOperand(0));
    if (inner == nullptr ||
        (inner->getPredicate() != llvm::ICmpInst::ICMP_EQ &&
         inner->getPredicate() != llvm::ICmpInst::ICMP_NE)) {
      return std::nullopt;
    }
    bool outerTrueMeansInnerTrue =
        cmp->getPredicate() == llvm::ICmpInst::ICMP_NE;
    bool innerTrueMeansEqual =
        inner->getPredicate() == llvm::ICmpInst::ICMP_EQ;
    *equalCmp = inner;
    return outerTrueMeansInnerTrue ? innerTrueMeansEqual
                                   : !innerTrueMeansEqual;
  }

  *equalCmp = cmp;
  return cmp->getPredicate() == llvm::ICmpInst::ICMP_EQ;
}

bool conditionUseChainIsPrivate(llvm::BranchInst &branch,
                                llvm::ICmpInst &equalCmp) {
  llvm::Value *condition = branch.getCondition();
  if (condition == &equalCmp) {
    return hasOnlyUser(equalCmp, branch);
  }

  auto *outer = llvm::dyn_cast<llvm::ICmpInst>(condition);
  if (outer == nullptr || !hasOnlyUser(*outer, branch)) {
    return false;
  }
  llvm::Value *maybeZext = llvm::isa<llvm::ConstantInt>(outer->getOperand(0))
                               ? outer->getOperand(1)
                               : outer->getOperand(0);
  auto *zext = llvm::dyn_cast<llvm::ZExtInst>(maybeZext);
  return zext != nullptr && zext->getOperand(0) == &equalCmp &&
         hasOnlyUser(*zext, *outer) && hasOnlyUser(equalCmp, *zext);
}

bool pointerIsFailCallSetupValue(llvm::Value *value, llvm::BasicBlock &block) {
  for (llvm::Value *candidate : {value, value->stripPointerCasts()}) {
    if (llvm::isa<llvm::GlobalVariable>(candidate)) {
      continue;
    }
    auto *inst = llvm::dyn_cast<llvm::Instruction>(candidate);
    if (inst == nullptr || inst->getParent() != &block) {
      continue;
    }
    if (llvm::isa<llvm::IntToPtrInst>(inst) ||
        llvm::isa<llvm::GetElementPtrInst>(inst) ||
        llvm::isa<llvm::CastInst>(inst)) {
      return true;
    }
  }
  return false;
}

bool pureInstructionOnlyFeedsBlock(llvm::Instruction &inst,
                                   llvm::BasicBlock &block) {
  if (inst.mayHaveSideEffects()) {
    return false;
  }
  if (!llvm::isa<llvm::BinaryOperator>(inst) &&
      !llvm::isa<llvm::CastInst>(inst) &&
      !llvm::isa<llvm::GetElementPtrInst>(inst) &&
      !llvm::isa<llvm::PHINode>(inst)) {
    return false;
  }
  return llvm::all_of(inst.users(), [&](llvm::User *user) {
    auto *userInst = llvm::dyn_cast<llvm::Instruction>(user);
    return userInst != nullptr && userInst->getParent() == &block;
  });
}

bool storeIsFailCallSetup(llvm::StoreInst &store, llvm::BasicBlock &block) {
  return !store.isVolatile() && !store.isAtomic() &&
         llvm::isa<llvm::ConstantInt>(store.getValueOperand()) &&
         pointerIsFailCallSetupValue(store.getPointerOperand(), block);
}

bool isStackCheckFailCallee(llvm::Function *callee) {
  return callee != nullptr &&
         (callee->getName() == "__stack_chk_fail" ||
          callee->getName() == "__stack_smash_handler");
}

bool blockOnlyCallsStackCheckFail(llvm::BasicBlock &block) {
  llvm::CallInst *failCall = nullptr;
  bool sawFailCall = false;
  for (llvm::Instruction &inst : block) {
    if (!sawFailCall) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (!isStackCheckFailCallee(call->getCalledFunction())) {
          return false;
        }
        failCall = call;
        sawFailCall = true;
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        if (!storeIsFailCallSetup(*store, block)) {
          return false;
        }
        continue;
      }
      if (!pureInstructionOnlyFeedsBlock(inst, block)) {
        return false;
      }
      continue;
    }

    if (llvm::isa<llvm::UnreachableInst>(inst)) {
      continue;
    }
    return false;
  }
  return failCall != nullptr;
}

void eraseDeadLoadAndPointer(llvm::LoadInst &load) {
  if (load.getParent() == nullptr || !load.use_empty()) {
    return;
  }
  llvm::Value *pointer = load.getPointerOperand();
  load.eraseFromParent();
  llvm::RecursivelyDeleteTriviallyDeadInstructions(pointer);
}

bool eraseStackCanaryCheck(llvm::BranchInst &branch,
                           NativeStackCanaryCleanupSummary &summary) {
  if (!branch.isConditional()) {
    return false;
  }

  llvm::ICmpInst *equalCmp = nullptr;
  std::optional<bool> trueMeansEqual =
      conditionTrueMeansCanaryEqual(branch.getCondition(), &equalCmp);
  if (!trueMeansEqual || equalCmp == nullptr ||
      !conditionUseChainIsPrivate(branch, *equalCmp)) {
    return false;
  }

  llvm::BasicBlock *success =
      *trueMeansEqual ? branch.getSuccessor(0) : branch.getSuccessor(1);
  llvm::BasicBlock *fail =
      *trueMeansEqual ? branch.getSuccessor(1) : branch.getSuccessor(0);
  if (success == fail || !blockOnlyCallsStackCheckFail(*fail)) {
    return false;
  }

  auto *first = llvm::dyn_cast<llvm::LoadInst>(equalCmp->getOperand(0));
  auto *second = llvm::dyn_cast<llvm::LoadInst>(equalCmp->getOperand(1));
  if (first == nullptr || second == nullptr) {
    return false;
  }

  llvm::LoadInst *savedLoad = nullptr;
  llvm::LoadInst *fsCanaryLoad = nullptr;
  std::optional<FsCanaryAddress> fsAddress;
  if (loadIsSavedCanarySlot(*first)) {
    fsAddress = findFsCanaryAddress(*second);
    if (fsAddress) {
      savedLoad = first;
      fsCanaryLoad = second;
    }
  }
  if (savedLoad == nullptr && loadIsSavedCanarySlot(*second)) {
    fsAddress = findFsCanaryAddress(*first);
    if (fsAddress) {
      savedLoad = second;
      fsCanaryLoad = first;
    }
  }
  if (savedLoad == nullptr || fsCanaryLoad == nullptr || !fsAddress) {
    return false;
  }

  llvm::Function *function = branch.getFunction();
  if (function == nullptr) {
    return false;
  }

  llvm::BasicBlock *checkBlock = branch.getParent();
  llvm::Value *oldCondition = branch.getCondition();
  fail->removePredecessor(checkBlock);
  llvm::IRBuilder<> builder(&branch);
  builder.CreateBr(success);
  branch.eraseFromParent();

  llvm::RecursivelyDeleteTriviallyDeadInstructions(oldCondition);
  eraseDeadLoadAndPointer(*savedLoad);
  eraseDeadLoadAndPointer(*fsCanaryLoad);

  size_t blockCountBefore = function->size();
  if (llvm::removeUnreachableBlocks(*function) &&
      function->size() < blockCountBefore) {
    summary.FailBlocksRemoved += blockCountBefore - function->size();
  }
  ++summary.CanaryChecksRemoved;
  return true;
}

void eraseStackCanaryChecks(llvm::Function &function,
                            NativeStackCanaryCleanupSummary &summary) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (llvm::BasicBlock &block : llvm::make_early_inc_range(function)) {
      auto *branch =
          llvm::dyn_cast_or_null<llvm::BranchInst>(block.getTerminator());
      if (branch != nullptr && eraseStackCanaryCheck(*branch, summary)) {
        changed = true;
        break;
      }
    }
  }
}

} // namespace

NativeStackCanaryCleanupSummary runNativeStackCanaryCleanup(
    llvm::Module &module, const NativeStackCanaryCleanupOptions &options) {
  NativeStackCanaryCleanupSummary summary;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    ++summary.FunctionsSeen;
    eraseStackCanaryChecks(function, summary);
  }
  if (options.PrintSummary) {
    printNativeStackCanaryCleanupSummary(summary, llvm::errs());
  }
  return summary;
}

void printNativeStackCanaryCleanupSummary(
    const NativeStackCanaryCleanupSummary &summary, llvm::raw_ostream &os) {
  os << "Native stack canary cleanup: functions=" << summary.FunctionsSeen
     << " checks_removed=" << summary.CanaryChecksRemoved
     << " fail_blocks_removed=" << summary.FailBlocksRemoved << '\n';
}

} // namespace notdec::bin2llvm
