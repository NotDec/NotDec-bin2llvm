#include "notdec-bin2llvm/passes/summary/NativeStackCanaryCleanup.h"

#include "notdec-bin2llvm/NativeAbi.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
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
#include <vector>

namespace notdec::bin2llvm {
namespace {

struct FsCanaryAddress {
  llvm::LoadInst *BaseLoad = nullptr;
  std::set<llvm::Instruction *> AddressChain;
};

struct CanaryConditionMatch {
  bool TrueMeansEqual = false;
  llvm::ICmpInst *EqualCmp = nullptr;
  llvm::StoreInst *FlagStore = nullptr;
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
  if (accessMatchesRegister(
          load.getMetadata("notdec.register.summary_ssa.entry"),
          wantedRegister)) {
    return true;
  }
  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      load.getPointerOperand()->stripPointerCasts());
  return global != nullptr &&
         global->getMetadata("notdec.register") != nullptr &&
         registerName(*global) == wantedRegister;
}

bool storeWritesRegister(const llvm::StoreInst &store,
                         llvm::StringRef wantedRegister) {
  if (accessMatchesRegister(store.getMetadata("notdec.register.access"),
                            wantedRegister)) {
    return true;
  }
  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      store.getPointerOperand()->stripPointerCasts());
  return global != nullptr &&
         global->getMetadata("notdec.register") != nullptr &&
         registerName(*global) == wantedRegister;
}

std::optional<int64_t> signedConstantValue(llvm::Value *value) {
  auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value);
  if (constant == nullptr || constant->getBitWidth() > 64) {
    return std::nullopt;
  }
  return constant->getSExtValue();
}

bool unknownValue(llvm::Value *value) {
  value = value->stripPointerCasts();
  if (llvm::isa<llvm::UndefValue>(value) ||
      llvm::isa<llvm::PoisonValue>(value)) {
    return true;
  }
  auto *freeze = llvm::dyn_cast<llvm::FreezeInst>(value);
  return freeze != nullptr &&
         (llvm::isa<llvm::UndefValue>(freeze->getOperand(0)) ||
          llvm::isa<llvm::PoisonValue>(freeze->getOperand(0)));
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
    if (auto lhs =
            integerOffsetFromBase(op->getOperand(0), base, seen, chain)) {
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
    if (auto lhs =
            integerOffsetFromBase(op->getOperand(0), base, seen, chain)) {
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

  // Unknown FS inputs are allowed, but concrete constants are not: treating
  // zero as a TLS base would invent a real address such as inttoptr(40).
  bool sawFsInput = false;
  for (llvm::Value *incoming : phi->incoming_values()) {
    if (unknownValue(incoming)) {
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

std::optional<int64_t>
integerOffsetFromFsBase(llvm::Value *value, std::set<llvm::Value *> &visiting,
                        std::map<llvm::Value *, std::optional<int64_t>> &cache,
                        std::set<llvm::Instruction *> &chain,
                        llvm::LoadInst **baseLoad) {
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

  if (auto *phi = llvm::dyn_cast<llvm::PHINode>(value)) {
    bool sawInput = false;
    for (llvm::Value *incoming : phi->incoming_values()) {
      if (unknownValue(incoming)) {
        continue;
      }
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
    if (auto lhs = integerOffsetFromFsBase(op->getOperand(0), visiting, cache,
                                           chain, baseLoad)) {
      if (auto rhs = signedConstantValue(op->getOperand(1))) {
        result = *lhs + *rhs;
      }
    }
    if (!result) {
      if (auto rhs = integerOffsetFromFsBase(op->getOperand(1), visiting, cache,
                                             chain, baseLoad)) {
        if (auto lhs = signedConstantValue(op->getOperand(0))) {
          result = *lhs + *rhs;
        }
      }
    }
  } else if (op->getOpcode() == llvm::Instruction::Sub) {
    if (auto lhs = integerOffsetFromFsBase(op->getOperand(0), visiting, cache,
                                           chain, baseLoad)) {
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

std::optional<int64_t>
pointerOffsetFromIntegerBase(llvm::Value *pointer, llvm::Value *base,
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
findFsCanaryIntegerAddress(llvm::LoadInst &load) {
  std::set<llvm::Instruction *> addressChain;
  if (auto *inst =
          llvm::dyn_cast<llvm::Instruction>(load.getPointerOperand())) {
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
  std::optional<int64_t> offset = integerOffsetFromFsBase(
      integer, visiting, cache, address.AddressChain, &address.BaseLoad);
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
  if (load != nullptr &&
      (loadReadsRegister(*load, "RBP") || loadReadsRegister(*load, "EBP") ||
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

std::optional<int64_t> offsetFromRegister(llvm::Value *value,
                                          llvm::StringRef registerName,
                                          std::set<llvm::Value *> &seen) {
  value = value->stripPointerCasts();
  if (!seen.insert(value).second) {
    return std::nullopt;
  }

  auto *load = llvm::dyn_cast<llvm::LoadInst>(value);
  if (load != nullptr && loadReadsRegister(*load, registerName)) {
    return 0;
  }

  auto *op = llvm::dyn_cast<llvm::Operator>(value);
  if (op == nullptr) {
    return std::nullopt;
  }
  if (op->getOpcode() == llvm::Instruction::Add) {
    if (auto lhs = offsetFromRegister(op->getOperand(0), registerName, seen)) {
      if (auto rhs = signedConstantValue(op->getOperand(1))) {
        return *lhs + *rhs;
      }
    }
    if (auto rhs = offsetFromRegister(op->getOperand(1), registerName, seen)) {
      if (auto lhs = signedConstantValue(op->getOperand(0))) {
        return *lhs + *rhs;
      }
    }
  }
  if (op->getOpcode() == llvm::Instruction::Sub) {
    if (auto lhs = offsetFromRegister(op->getOperand(0), registerName, seen)) {
      if (auto rhs = signedConstantValue(op->getOperand(1))) {
        return *lhs - *rhs;
      }
    }
  }
  return std::nullopt;
}

bool stackPointerSavedCanaryPointer(llvm::Value *pointer,
                                    llvm::StringRef stackPointerRegister) {
  if (stackPointerRegister.empty()) {
    return false;
  }
  llvm::Value *integer = intToPtrIntegerOperand(pointer);
  if (integer == nullptr) {
    return false;
  }
  std::set<llvm::Value *> seen;
  std::optional<int64_t> offset =
      offsetFromRegister(integer, stackPointerRegister, seen);
  // Raw lifted x64 can still address the saved canary from the current RSP
  // before stack-frame localization, so the offset may be positive or negative.
  return offset && *offset != 0;
}

bool loadIsSavedCanarySlot(llvm::LoadInst &load,
                           llvm::StringRef stackPointerRegister) {
  return !load.isVolatile() && !load.isAtomic() &&
         (stackAllocaPointer(load.getPointerOperand()) ||
          framePointerSavedCanaryPointer(load.getPointerOperand()) ||
          stackPointerSavedCanaryPointer(load.getPointerOperand(),
                                         stackPointerRegister));
}

llvm::LoadInst *savedCanaryLoadFromCompareOperand(llvm::Value *value) {
  if (auto *load = llvm::dyn_cast_or_null<llvm::LoadInst>(value)) {
    return load;
  }

  auto *andOp = llvm::dyn_cast_or_null<llvm::BinaryOperator>(value);
  if (andOp == nullptr || andOp->getOpcode() != llvm::Instruction::And) {
    return nullptr;
  }

  llvm::LoadInst *load = llvm::dyn_cast<llvm::LoadInst>(andOp->getOperand(0));
  auto *mask = llvm::dyn_cast<llvm::ConstantInt>(andOp->getOperand(1));
  if (load == nullptr || mask == nullptr) {
    load = llvm::dyn_cast<llvm::LoadInst>(andOp->getOperand(1));
    mask = llvm::dyn_cast<llvm::ConstantInt>(andOp->getOperand(0));
  }
  if (load == nullptr || mask == nullptr) {
    return nullptr;
  }

  auto *type = llvm::dyn_cast<llvm::IntegerType>(load->getType());
  if (type == nullptr || type->getBitWidth() <= 32) {
    return nullptr;
  }
  llvm::APInt low32Mask = llvm::APInt::getLowBitsSet(type->getBitWidth(), 32);
  return mask->getValue() == low32Mask ? load : nullptr;
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
    bool innerTrueMeansEqual = inner->getPredicate() == llvm::ICmpInst::ICMP_EQ;
    *equalCmp = inner;
    return outerTrueMeansInnerTrue ? innerTrueMeansEqual : !innerTrueMeansEqual;
  }

  *equalCmp = cmp;
  return cmp->getPredicate() == llvm::ICmpInst::ICMP_EQ;
}

std::optional<bool>
zfConditionTrueMeansCanaryEqual(llvm::Value *condition,
                                llvm::ICmpInst **equalCmp,
                                llvm::StoreInst **flagStore) {
  auto *cmp = llvm::dyn_cast_or_null<llvm::ICmpInst>(condition);
  if (cmp == nullptr || (cmp->getPredicate() != llvm::ICmpInst::ICMP_EQ &&
                         cmp->getPredicate() != llvm::ICmpInst::ICMP_NE)) {
    return std::nullopt;
  }

  llvm::Value *maybeZero = nullptr;
  llvm::Value *maybeFlag = nullptr;
  if (llvm::isa<llvm::ConstantInt>(cmp->getOperand(0))) {
    maybeZero = cmp->getOperand(0);
    maybeFlag = cmp->getOperand(1);
  } else if (llvm::isa<llvm::ConstantInt>(cmp->getOperand(1))) {
    maybeZero = cmp->getOperand(1);
    maybeFlag = cmp->getOperand(0);
  }
  auto *zero = llvm::dyn_cast_or_null<llvm::ConstantInt>(maybeZero);
  auto *flagLoad = llvm::dyn_cast_or_null<llvm::LoadInst>(maybeFlag);
  if (zero == nullptr || !zero->isZero() || flagLoad == nullptr ||
      !loadReadsRegister(*flagLoad, "ZF")) {
    return std::nullopt;
  }

  llvm::BasicBlock *block = flagLoad->getParent();
  if (block == nullptr) {
    return std::nullopt;
  }
  for (auto it = flagLoad->getIterator(); it != block->begin();) {
    --it;
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&*it);
    if (store == nullptr || !storeWritesRegister(*store, "ZF")) {
      continue;
    }
    auto *zext = llvm::dyn_cast<llvm::ZExtInst>(store->getValueOperand());
    auto *inner = zext == nullptr
                      ? llvm::dyn_cast<llvm::ICmpInst>(store->getValueOperand())
                      : llvm::dyn_cast<llvm::ICmpInst>(zext->getOperand(0));
    if (inner == nullptr ||
        (inner->getPredicate() != llvm::ICmpInst::ICMP_EQ &&
         inner->getPredicate() != llvm::ICmpInst::ICMP_NE)) {
      return std::nullopt;
    }
    bool trueMeansFlagOne = cmp->getPredicate() == llvm::ICmpInst::ICMP_NE;
    bool innerTrueMeansEqual = inner->getPredicate() == llvm::ICmpInst::ICMP_EQ;
    *equalCmp = inner;
    *flagStore = store;
    return trueMeansFlagOne ? innerTrueMeansEqual : !innerTrueMeansEqual;
  }
  return std::nullopt;
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

bool zfConditionUseChainIsPrivate(llvm::BranchInst &branch,
                                  llvm::ICmpInst &equalCmp,
                                  llvm::StoreInst &flagStore) {
  auto *outer = llvm::dyn_cast<llvm::ICmpInst>(branch.getCondition());
  if (outer == nullptr || !hasOnlyUser(*outer, branch)) {
    return false;
  }
  llvm::Value *maybeFlag = llvm::isa<llvm::ConstantInt>(outer->getOperand(0))
                               ? outer->getOperand(1)
                               : outer->getOperand(0);
  auto *flagLoad = llvm::dyn_cast<llvm::LoadInst>(maybeFlag);
  if (flagLoad == nullptr || !hasOnlyUser(*flagLoad, *outer)) {
    return false;
  }
  auto *zext = llvm::dyn_cast<llvm::ZExtInst>(flagStore.getValueOperand());
  if (zext != nullptr) {
    return zext->getOperand(0) == &equalCmp && hasOnlyUser(*zext, flagStore) &&
           hasOnlyUser(equalCmp, *zext);
  }
  return flagStore.getValueOperand() == &equalCmp &&
         hasOnlyUser(equalCmp, flagStore);
}

std::optional<CanaryConditionMatch>
matchCanaryCondition(llvm::BranchInst &branch) {
  CanaryConditionMatch zfMatch;
  // Lifted x64 checks often materialize the canary comparison into ZF and then
  // branch on ZF == 0.  Match that first so the outer flag compare is not
  // mistaken for the canary equality itself.
  if (std::optional<bool> trueMeansEqual = zfConditionTrueMeansCanaryEqual(
          branch.getCondition(), &zfMatch.EqualCmp, &zfMatch.FlagStore)) {
    if (zfMatch.EqualCmp != nullptr && zfMatch.FlagStore != nullptr &&
        zfConditionUseChainIsPrivate(branch, *zfMatch.EqualCmp,
                                     *zfMatch.FlagStore)) {
      zfMatch.TrueMeansEqual = *trueMeansEqual;
      return zfMatch;
    }
  }

  CanaryConditionMatch match;
  if (std::optional<bool> trueMeansEqual = conditionTrueMeansCanaryEqual(
          branch.getCondition(), &match.EqualCmp)) {
    if (match.EqualCmp != nullptr &&
        conditionUseChainIsPrivate(branch, *match.EqualCmp)) {
      match.TrueMeansEqual = *trueMeansEqual;
      return match;
    }
  }
  return std::nullopt;
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
  return callee != nullptr && (callee->getName() == "__stack_chk_fail" ||
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

void eraseDeadFlagStoreAndValue(llvm::StoreInst &store) {
  if (store.getParent() == nullptr) {
    return;
  }
  llvm::Value *value = store.getValueOperand();
  store.eraseFromParent();
  llvm::RecursivelyDeleteTriviallyDeadInstructions(value);
}

bool eraseStackCanaryPredecessor(llvm::BranchInst &branch,
                                 llvm::BasicBlock &fail,
                                 llvm::StringRef stackPointerRegister,
                                 NativeStackCanaryCleanupSummary &summary) {
  if (!branch.isConditional()) {
    return false;
  }

  std::optional<CanaryConditionMatch> condition = matchCanaryCondition(branch);
  if (!condition || condition->EqualCmp == nullptr) {
    return false;
  }
  llvm::ICmpInst *equalCmp = condition->EqualCmp;

  llvm::BasicBlock *success = condition->TrueMeansEqual
                                  ? branch.getSuccessor(0)
                                  : branch.getSuccessor(1);
  llvm::BasicBlock *matchedFail = condition->TrueMeansEqual
                                      ? branch.getSuccessor(1)
                                      : branch.getSuccessor(0);
  if (success == matchedFail || matchedFail != &fail) {
    return false;
  }

  llvm::LoadInst *firstSaved =
      savedCanaryLoadFromCompareOperand(equalCmp->getOperand(0));
  llvm::LoadInst *secondSaved =
      savedCanaryLoadFromCompareOperand(equalCmp->getOperand(1));
  auto *firstRawLoad = llvm::dyn_cast<llvm::LoadInst>(equalCmp->getOperand(0));
  auto *secondRawLoad = llvm::dyn_cast<llvm::LoadInst>(equalCmp->getOperand(1));
  if ((firstSaved == nullptr && firstRawLoad == nullptr) ||
      (secondSaved == nullptr && secondRawLoad == nullptr)) {
    return false;
  }

  llvm::LoadInst *savedLoad = nullptr;
  llvm::LoadInst *fsCanaryLoad = nullptr;
  std::optional<FsCanaryAddress> fsAddress;
  if (firstSaved != nullptr &&
      loadIsSavedCanarySlot(*firstSaved, stackPointerRegister) &&
      secondRawLoad != nullptr) {
    fsAddress = findFsCanaryAddress(*secondRawLoad);
    if (fsAddress) {
      savedLoad = firstSaved;
      fsCanaryLoad = secondRawLoad;
    }
  }
  if (savedLoad == nullptr && secondSaved != nullptr &&
      loadIsSavedCanarySlot(*secondSaved, stackPointerRegister) &&
      firstRawLoad != nullptr) {
    fsAddress = findFsCanaryAddress(*firstRawLoad);
    if (fsAddress) {
      savedLoad = secondSaved;
      fsCanaryLoad = firstRawLoad;
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
  fail.removePredecessor(checkBlock);
  llvm::IRBuilder<> builder(&branch);
  builder.CreateBr(success);
  branch.eraseFromParent();

  llvm::RecursivelyDeleteTriviallyDeadInstructions(oldCondition);
  if (condition->FlagStore != nullptr) {
    eraseDeadFlagStoreAndValue(*condition->FlagStore);
  }
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
                            llvm::StringRef stackPointerRegister,
                            NativeStackCanaryCleanupSummary &summary) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (llvm::BasicBlock &block : llvm::make_early_inc_range(function)) {
      if (!blockOnlyCallsStackCheckFail(block)) {
        continue;
      }

      // Treat the fail call as the anchor, then validate each incoming edge as
      // a real canary compare.  This keeps shared fail blocks natural: removing
      // one matching predecessor must not force unrelated predecessors to
      // match.
      std::vector<llvm::BranchInst *> predecessors;
      for (llvm::BasicBlock *predecessor : llvm::predecessors(&block)) {
        auto *branch = llvm::dyn_cast_or_null<llvm::BranchInst>(
            predecessor->getTerminator());
        if (branch != nullptr) {
          predecessors.push_back(branch);
        }
      }
      for (llvm::BranchInst *branch : predecessors) {
        if (eraseStackCanaryPredecessor(*branch, block, stackPointerRegister,
                                        summary)) {
          changed = true;
          break;
        }
      }
      if (changed) {
        break;
      }
    }
  }
}

} // namespace

NativeStackCanaryCleanupSummary
runNativeStackCanaryCleanup(llvm::Module &module,
                            const NativeStackCanaryCleanupOptions &options) {
  NativeStackCanaryCleanupSummary summary;
  std::string stackPointerRegister;
  if (std::optional<NativeAbiSpec> abi = readNativeAbiMetadata(module)) {
    stackPointerRegister = abi->StackPointerRegister;
  }
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    ++summary.FunctionsSeen;
    eraseStackCanaryChecks(function, stackPointerRegister, summary);
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
