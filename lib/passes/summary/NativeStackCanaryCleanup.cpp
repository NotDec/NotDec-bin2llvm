#include "notdec-bin2llvm/passes/summary/NativeStackCanaryCleanup.h"

#include "notdec-bin2llvm/NativeAbi.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/ValueHandle.h"
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

// Compiler stack canaries are loaded from ABI-specific TLS slots.  Keep the
// base register and offset together so the cleanup stays narrow: these loads
// are only removed after the surrounding __stack_chk_fail edge also matches.
struct TlsCanarySpec {
  const char *BaseRegister = "";
  int64_t Offset = 0;
};

constexpr TlsCanarySpec TlsCanarySpecs[] = {
    {"FS_OFFSET", 40}, // x86-64 glibc
    {"GS_OFFSET", 20}, // i386 glibc
};

struct TlsCanaryAddress {
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

bool phiNamesRegister(const llvm::PHINode &phi, llvm::StringRef registerName) {
  return accessMatchesRegister(phi.getMetadata("notdec.register.summary_ssa.phi"),
                               registerName);
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
  } else if (op->getOpcode() == llvm::Instruction::ZExt) {
    result = integerOffsetFromBase(op->getOperand(0), base, seen, chain);
  }

  if (result) {
    if (auto *instruction = llvm::dyn_cast<llvm::Instruction>(value)) {
      chain.insert(instruction);
    }
  }
  return result;
}

bool valueIsTlsBase(llvm::Value *value, const TlsCanarySpec &spec,
                    std::set<llvm::Value *> &visiting,
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
  if (load != nullptr && loadReadsRegister(*load, spec.BaseRegister)) {
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

  // Unknown TLS inputs are allowed, but concrete constants are not: treating
  // zero as a TLS base would invent a real canary address such as inttoptr(40).
  bool sawTlsInput = false;
  for (llvm::Value *incoming : phi->incoming_values()) {
    if (unknownValue(incoming)) {
      continue;
    }
    if (visiting.count(incoming->stripPointerCasts()) != 0) {
      continue;
    }
    if (!valueIsTlsBase(incoming, spec, visiting, cache, chain, baseLoad)) {
      return finish(false);
    }
    sawTlsInput = true;
  }
  if (!sawTlsInput) {
    return finish(false);
  }
  chain.insert(phi);
  return finish(true);
}

std::optional<int64_t>
integerOffsetFromTlsBase(llvm::Value *value, const TlsCanarySpec &spec,
                         std::set<llvm::Value *> &visiting,
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
  if (valueIsTlsBase(value, spec, baseVisiting, baseCache, chain, baseLoad)) {
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
      std::optional<int64_t> incomingOffset = integerOffsetFromTlsBase(
          incoming, spec, visiting, cache, chain, baseLoad);
      if (!incomingOffset) {
        return finish(std::nullopt);
      }
      if (*incomingOffset != spec.Offset) {
        return finish(std::nullopt);
      }
      sawInput = true;
    }
    if (!sawInput) {
      return finish(std::nullopt);
    }
    chain.insert(phi);
    return finish(spec.Offset);
  }

  auto *op = llvm::dyn_cast<llvm::Operator>(value);
  if (op == nullptr) {
    return finish(std::nullopt);
  }

  std::optional<int64_t> result;
  if (op->getOpcode() == llvm::Instruction::Add) {
    if (auto lhs = integerOffsetFromTlsBase(op->getOperand(0), spec, visiting,
                                           cache, chain, baseLoad)) {
      if (auto rhs = signedConstantValue(op->getOperand(1))) {
        result = *lhs + *rhs;
      }
    }
    if (!result) {
      if (auto rhs = integerOffsetFromTlsBase(op->getOperand(1), spec, visiting,
                                             cache, chain, baseLoad)) {
        if (auto lhs = signedConstantValue(op->getOperand(0))) {
          result = *lhs + *rhs;
        }
      }
    }
  } else if (op->getOpcode() == llvm::Instruction::Sub) {
    if (auto lhs = integerOffsetFromTlsBase(op->getOperand(0), spec, visiting,
                                           cache, chain, baseLoad)) {
      if (auto rhs = signedConstantValue(op->getOperand(1))) {
        result = *lhs - *rhs;
      }
    }
  } else if (op->getOpcode() == llvm::Instruction::ZExt) {
    result = integerOffsetFromTlsBase(op->getOperand(0), spec, visiting, cache,
                                      chain, baseLoad);
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

std::optional<TlsCanaryAddress>
findTlsCanaryIntegerAddress(llvm::LoadInst &load,
                            const TlsCanarySpec &spec) {
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
  TlsCanaryAddress address;
  address.AddressChain = std::move(addressChain);
  std::optional<int64_t> offset = integerOffsetFromTlsBase(
      integer, spec, visiting, cache, address.AddressChain, &address.BaseLoad);
  if (offset && *offset == spec.Offset) {
    return address;
  }
  return std::nullopt;
}

bool stackAllocaPointer(llvm::Value *value, std::set<llvm::Value *> &seen) {
  value = value->stripPointerCasts();
  if (!seen.insert(value).second) {
    return false;
  }

  auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(value);
  if (alloca != nullptr) {
    return alloca->hasName() &&
           alloca->getName().starts_with("notdec_stack.native");
  }
  if (auto *gep = llvm::dyn_cast<llvm::GEPOperator>(value)) {
    return stackAllocaPointer(gep->getPointerOperand(), seen);
  }

  auto *op = llvm::dyn_cast<llvm::Operator>(value);
  if (op == nullptr) {
    return false;
  }
  if (op->getOpcode() != llvm::Instruction::IntToPtr &&
      op->getOpcode() != llvm::Instruction::PtrToInt &&
      op->getOpcode() != llvm::Instruction::ZExt &&
      op->getOpcode() != llvm::Instruction::And) {
    return false;
  }
  return llvm::any_of(op->operands(), [&](llvm::Use &operand) {
    return stackAllocaPointer(operand.get(), seen);
  });
}

bool stackAllocaPointer(llvm::Value *value) {
  std::set<llvm::Value *> seen;
  return stackAllocaPointer(value, seen);
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
  auto *phi = llvm::dyn_cast<llvm::PHINode>(value);
  if (phi != nullptr && phiNamesRegister(*phi, registerName)) {
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

std::optional<TlsCanaryAddress> findTlsCanaryAddress(llvm::LoadInst &load) {
  if (load.isVolatile() || load.isAtomic()) {
    return std::nullopt;
  }
  llvm::Function *function = load.getFunction();
  if (function == nullptr) {
    return std::nullopt;
  }
  for (const TlsCanarySpec &spec : TlsCanarySpecs) {
    for (llvm::BasicBlock &block : *function) {
      for (llvm::Instruction &inst : block) {
        auto *baseLoad = llvm::dyn_cast<llvm::LoadInst>(&inst);
        if (baseLoad == nullptr ||
            !loadReadsRegister(*baseLoad, spec.BaseRegister)) {
          continue;
        }
        TlsCanaryAddress address;
        address.BaseLoad = baseLoad;
        std::optional<int64_t> offset = pointerOffsetFromIntegerBase(
            load.getPointerOperand(), baseLoad, address.AddressChain);
        if (offset && *offset == spec.Offset) {
          return address;
        }
      }
    }
    for (llvm::BasicBlock &block : *function) {
      for (llvm::Instruction &inst : block) {
        std::set<llvm::Value *> visiting;
        std::map<llvm::Value *, bool> cache;
        TlsCanaryAddress address;
        if (!valueIsTlsBase(&inst, spec, visiting, cache,
                            address.AddressChain, &address.BaseLoad)) {
          continue;
        }
        std::optional<int64_t> offset = pointerOffsetFromIntegerBase(
            load.getPointerOperand(), &inst, address.AddressChain);
        if (offset && *offset == spec.Offset) {
          return address;
        }
      }
    }
    if (std::optional<TlsCanaryAddress> address =
            findTlsCanaryIntegerAddress(load, spec)) {
      return address;
    }
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

bool valueDependsOnLocalAlloca(llvm::Value *value, llvm::BasicBlock &block,
                               std::set<llvm::Value *> &seen) {
  value = value->stripPointerCasts();
  if (!seen.insert(value).second) {
    return false;
  }

  auto *inst = llvm::dyn_cast<llvm::Instruction>(value);
  if (inst == nullptr || inst->getParent() != &block) {
    return false;
  }
  if (llvm::isa<llvm::AllocaInst>(inst)) {
    return true;
  }
  if (!llvm::isa<llvm::BinaryOperator>(inst) && !llvm::isa<llvm::CastInst>(inst) &&
      !llvm::isa<llvm::GetElementPtrInst>(inst)) {
    return false;
  }
  return llvm::any_of(inst->operands(), [&](llvm::Use &operand) {
    return valueDependsOnLocalAlloca(operand.get(), block, seen);
  });
}

bool pointerDependsOnLocalAlloca(llvm::Value *pointer,
                                 llvm::BasicBlock &block) {
  std::set<llvm::Value *> seen;
  return valueDependsOnLocalAlloca(pointer, block, seen);
}

bool pureInstructionOnlyFeedsBlock(llvm::Instruction &inst,
                                   llvm::BasicBlock &block) {
  if (llvm::isa<llvm::AllocaInst>(inst)) {
    return llvm::all_of(inst.users(), [&](llvm::User *user) {
      auto *userInst = llvm::dyn_cast<llvm::Instruction>(user);
      return userInst != nullptr && userInst->getParent() == &block;
    });
  }
  if (inst.mayHaveSideEffects()) {
    return false;
  }
  if (!llvm::isa<llvm::BinaryOperator>(inst) &&
      !llvm::isa<llvm::CastInst>(inst) &&
      !llvm::isa<llvm::GetElementPtrInst>(inst) &&
      !llvm::isa<llvm::LoadInst>(inst) &&
      !llvm::isa<llvm::PHINode>(inst)) {
    return false;
  }
  return llvm::all_of(inst.users(), [&](llvm::User *user) {
    auto *userInst = llvm::dyn_cast<llvm::Instruction>(user);
    return userInst != nullptr && userInst->getParent() == &block;
  });
}

bool storeIsFailCallSetup(llvm::StoreInst &store, llvm::BasicBlock &block) {
  if (store.isVolatile() || store.isAtomic()) {
    return false;
  }
  if (llvm::isa<llvm::ConstantInt>(store.getValueOperand())) {
    return pointerIsFailCallSetupValue(store.getPointerOperand(), block);
  }
  return pointerDependsOnLocalAlloca(store.getPointerOperand(), block);
}

bool isDirectStackCheckFailCallee(llvm::Function *callee) {
  return callee != nullptr && (callee->getName() == "__stack_chk_fail" ||
                               callee->getName() == "__stack_smash_handler");
}

bool functionOnlyCallsStackCheckFail(
    llvm::Function &function, std::set<llvm::Function *> &visiting,
    std::map<llvm::Function *, bool> &cache,
    std::set<llvm::Function *> *failOnlyFunctions = nullptr);

bool isStackCheckFailCallee(llvm::Function *callee,
                            std::set<llvm::Function *> &visiting,
                            std::map<llvm::Function *, bool> &cache,
                            std::set<llvm::Function *> *failOnlyFunctions) {
  if (isDirectStackCheckFailCallee(callee)) {
    return true;
  }
  if (callee == nullptr || callee->isDeclaration()) {
    return false;
  }
  return functionOnlyCallsStackCheckFail(*callee, visiting, cache,
                                         failOnlyFunctions);
}

bool isSkippedNativeSetupCall(llvm::CallInst &call) {
  llvm::Function *callee = call.getCalledFunction();
  return callee != nullptr && callee->isDeclaration() && call.use_empty() &&
         callee->getName().starts_with("notdec_native_");
}

bool isDiscardedSummarySetupCall(llvm::CallInst &call) {
  llvm::Function *callee = call.getCalledFunction();
  return callee != nullptr && callee->isDeclaration() && call.use_empty() &&
         callee->getName().starts_with("notdec.register.summary_");
}

bool blockOnlyCallsStackCheckFail(llvm::BasicBlock &block,
                                  std::set<llvm::Function *> &visiting,
                                  std::map<llvm::Function *, bool> &cache,
                                  std::set<llvm::Function *>
                                      *failOnlyFunctions) {
  llvm::CallInst *failCall = nullptr;
  bool sawFailCall = false;
  for (llvm::Instruction &inst : block) {
    if (!sawFailCall) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (isStackCheckFailCallee(call->getCalledFunction(), visiting, cache,
                                   failOnlyFunctions)) {
          failCall = call;
          sawFailCall = true;
          continue;
        }
        if (isSkippedNativeSetupCall(*call)) {
          continue;
        }
        if (isDiscardedSummarySetupCall(*call)) {
          continue;
        }
        return false;
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

bool functionOnlyCallsStackCheckFail(
    llvm::Function &function, std::set<llvm::Function *> &visiting,
    std::map<llvm::Function *, bool> &cache,
    std::set<llvm::Function *> *failOnlyFunctions) {
  auto cached = cache.find(&function);
  if (cached != cache.end()) {
    if (cached->second && failOnlyFunctions != nullptr) {
      failOnlyFunctions->insert(&function);
    }
    return cached->second;
  }
  if (!visiting.insert(&function).second) {
    return false;
  }

  bool result = false;
  if (!function.empty()) {
    result = true;
    for (llvm::BasicBlock &block : function) {
      if (!blockOnlyCallsStackCheckFail(block, visiting, cache,
                                        failOnlyFunctions)) {
        result = false;
        break;
      }
    }
  }

  visiting.erase(&function);
  cache[&function] = result;
  if (result && failOnlyFunctions != nullptr) {
    failOnlyFunctions->insert(&function);
  }
  return result;
}

void eraseDeadLoadAndPointer(llvm::LoadInst &load) {
  if (load.getParent() == nullptr || !load.use_empty()) {
    return;
  }
  llvm::Value *pointer = load.getPointerOperand();
  load.eraseFromParent();
  llvm::RecursivelyDeleteTriviallyDeadInstructions(pointer);
}

bool sameAddressExpression(llvm::Value *lhs, llvm::Value *rhs,
                           std::set<std::pair<llvm::Value *, llvm::Value *>>
                               &seen) {
  lhs = lhs->stripPointerCasts();
  rhs = rhs->stripPointerCasts();
  if (lhs == rhs) {
    return true;
  }
  if (!seen.insert({lhs, rhs}).second) {
    return true;
  }

  auto *lhsConst = llvm::dyn_cast<llvm::ConstantInt>(lhs);
  auto *rhsConst = llvm::dyn_cast<llvm::ConstantInt>(rhs);
  if (lhsConst != nullptr || rhsConst != nullptr) {
    return lhsConst != nullptr && rhsConst != nullptr &&
           lhsConst->getValue() == rhsConst->getValue();
  }

  auto *lhsOp = llvm::dyn_cast<llvm::Operator>(lhs);
  auto *rhsOp = llvm::dyn_cast<llvm::Operator>(rhs);
  if (lhsOp == nullptr || rhsOp == nullptr ||
      lhsOp->getOpcode() != rhsOp->getOpcode() ||
      lhsOp->getNumOperands() != rhsOp->getNumOperands()) {
    return false;
  }

  unsigned opcode = lhsOp->getOpcode();
  if (opcode != llvm::Instruction::IntToPtr &&
      opcode != llvm::Instruction::PtrToInt &&
      opcode != llvm::Instruction::ZExt &&
      opcode != llvm::Instruction::GetElementPtr &&
      opcode != llvm::Instruction::And && opcode != llvm::Instruction::Add &&
      opcode != llvm::Instruction::Sub) {
    return false;
  }

  auto sameOperandsInOrder = [&]() {
    for (unsigned index = 0; index < lhsOp->getNumOperands(); ++index) {
      if (!sameAddressExpression(lhsOp->getOperand(index),
                                 rhsOp->getOperand(index), seen)) {
        return false;
      }
    }
    return true;
  };
  if (sameOperandsInOrder()) {
    return true;
  }

  if ((opcode == llvm::Instruction::And || opcode == llvm::Instruction::Add) &&
      lhsOp->getNumOperands() == 2) {
    return sameAddressExpression(lhsOp->getOperand(0), rhsOp->getOperand(1),
                                 seen) &&
           sameAddressExpression(lhsOp->getOperand(1), rhsOp->getOperand(0),
                                 seen);
  }
  return false;
}

bool samePointerValue(llvm::Value *lhs, llvm::Value *rhs) {
  if (lhs == nullptr || rhs == nullptr) {
    return false;
  }
  std::set<std::pair<llvm::Value *, llvm::Value *>> seen;
  return sameAddressExpression(lhs, rhs, seen);
}

llvm::StoreInst *findSavedCanaryStore(llvm::LoadInst &savedLoad) {
  if (savedLoad.getParent() == nullptr) {
    return nullptr;
  }
  llvm::Function *function = savedLoad.getFunction();
  if (function == nullptr) {
    return nullptr;
  }

  llvm::DominatorTree dominators(*function);
  llvm::StoreInst *match = nullptr;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    if (store == nullptr || store->isVolatile() || store->isAtomic()) {
      continue;
    }
    if (!samePointerValue(store->getPointerOperand(),
                          savedLoad.getPointerOperand())) {
      continue;
    }
    if (!dominators.dominates(store, &savedLoad)) {
      continue;
    }
    auto *storedLoad = llvm::dyn_cast<llvm::LoadInst>(store->getValueOperand());
    if (storedLoad == nullptr || !findTlsCanaryAddress(*storedLoad)) {
      continue;
    }
    if (match != nullptr) {
      return nullptr;
    }
    match = store;
  }
  return match;
}

void eraseSavedCanaryStore(llvm::LoadInst &savedLoad) {
  llvm::StoreInst *store = findSavedCanaryStore(savedLoad);
  if (store == nullptr || store->getParent() == nullptr) {
    return;
  }
  auto *storedLoad = llvm::dyn_cast<llvm::LoadInst>(store->getValueOperand());
  store->eraseFromParent();
  if (storedLoad != nullptr) {
    eraseDeadLoadAndPointer(*storedLoad);
  }
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
  llvm::LoadInst *tlsCanaryLoad = nullptr;
  std::optional<TlsCanaryAddress> tlsAddress;
  if (firstSaved != nullptr &&
      loadIsSavedCanarySlot(*firstSaved, stackPointerRegister) &&
      secondRawLoad != nullptr) {
    tlsAddress = findTlsCanaryAddress(*secondRawLoad);
    if (tlsAddress) {
      savedLoad = firstSaved;
      tlsCanaryLoad = secondRawLoad;
    }
  }
  if (savedLoad == nullptr && secondSaved != nullptr &&
      loadIsSavedCanarySlot(*secondSaved, stackPointerRegister) &&
      firstRawLoad != nullptr) {
    tlsAddress = findTlsCanaryAddress(*firstRawLoad);
    if (tlsAddress) {
      savedLoad = secondSaved;
      tlsCanaryLoad = firstRawLoad;
    }
  }
  llvm::Function *function = branch.getFunction();
  if (function == nullptr) {
    return false;
  }
  if (savedLoad == nullptr || tlsCanaryLoad == nullptr || !tlsAddress) {
    return false;
  }

  llvm::BasicBlock *checkBlock = branch.getParent();
  llvm::Value *oldCondition = branch.getCondition();
  llvm::WeakTrackingVH savedLoadHandle(savedLoad);
  llvm::WeakTrackingVH tlsCanaryLoadHandle(tlsCanaryLoad);
  eraseSavedCanaryStore(*savedLoad);
  fail.removePredecessor(checkBlock);
  llvm::IRBuilder<> builder(&branch);
  builder.CreateBr(success);
  branch.eraseFromParent();

  llvm::RecursivelyDeleteTriviallyDeadInstructions(oldCondition);
  if (condition->FlagStore != nullptr) {
    eraseDeadFlagStoreAndValue(*condition->FlagStore);
  }
  if (auto *remainingSavedLoad =
          llvm::dyn_cast_or_null<llvm::LoadInst>(savedLoadHandle)) {
    eraseDeadLoadAndPointer(*remainingSavedLoad);
  }
  if (auto *remainingTlsCanaryLoad =
          llvm::dyn_cast_or_null<llvm::LoadInst>(tlsCanaryLoadHandle)) {
    eraseDeadLoadAndPointer(*remainingTlsCanaryLoad);
  }

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
                            NativeStackCanaryCleanupSummary &summary,
                            std::set<llvm::Function *> &failOnlyFunctions) {
  std::set<llvm::Function *> visitingFailSinks;
  std::map<llvm::Function *, bool> failSinkCache;
  bool changed = true;
  while (changed) {
    changed = false;
    for (llvm::BasicBlock &block : llvm::make_early_inc_range(function)) {
      bool failSink = blockOnlyCallsStackCheckFail(
          block, visitingFailSinks, failSinkCache, &failOnlyFunctions);
      if (!failSink) {
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

void eraseDeadFailOnlyFunctions(
    std::set<llvm::Function *> &failOnlyFunctions,
    NativeStackCanaryCleanupSummary &summary) {
  std::set<llvm::Function *> visiting;
  std::map<llvm::Function *, bool> cache;
  for (llvm::Function *function :
       llvm::make_early_inc_range(failOnlyFunctions)) {
    if (function == nullptr || function->getParent() == nullptr ||
        function->isDeclaration() || !function->hasLocalLinkage() ||
        !function->use_empty()) {
      continue;
    }
    if (!functionOnlyCallsStackCheckFail(*function, visiting, cache)) {
      continue;
    }
    function->eraseFromParent();
    ++summary.FailFunctionsRemoved;
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
  std::set<llvm::Function *> failOnlyFunctions;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    ++summary.FunctionsSeen;
    eraseStackCanaryChecks(function, stackPointerRegister, summary,
                           failOnlyFunctions);
  }
  eraseDeadFailOnlyFunctions(failOnlyFunctions, summary);
  if (options.PrintSummary) {
    printNativeStackCanaryCleanupSummary(summary, llvm::errs());
  }
  return summary;
}

void printNativeStackCanaryCleanupSummary(
    const NativeStackCanaryCleanupSummary &summary, llvm::raw_ostream &os) {
  os << "Native stack canary cleanup: functions=" << summary.FunctionsSeen
     << " checks_removed=" << summary.CanaryChecksRemoved
     << " fail_blocks_removed=" << summary.FailBlocksRemoved
     << " fail_functions_removed=" << summary.FailFunctionsRemoved << '\n';
}

} // namespace notdec::bin2llvm
