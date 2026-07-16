#include "notdec-bin2llvm/passes/summary/NativeRegisterPeephole.h"

#include "notdec-bin2llvm/NativeRegisterPartialRead.h"
#include "notdec-bin2llvm/NativeRegisterValueRange.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>

namespace notdec::bin2llvm {
namespace {

struct NarrowUse {
  llvm::Instruction *Root = nullptr;
  llvm::Instruction *Middle = nullptr;
  uint64_t BitOffset = 0;
};

llvm::GlobalVariable *fullRegisterLoadGlobal(llvm::LoadInst &load) {
  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      load.getPointerOperand()->stripPointerCasts());
  if (global == nullptr || global->getMetadata("notdec.register") == nullptr ||
      load.getMetadata("notdec.register.access") == nullptr) {
    return nullptr;
  }
  auto *loadType = llvm::dyn_cast<llvm::IntegerType>(load.getType());
  auto *globalType = llvm::dyn_cast<llvm::IntegerType>(global->getValueType());
  if (loadType == nullptr || globalType == nullptr || loadType != globalType) {
    return nullptr;
  }
  return global;
}

std::optional<uint64_t> constantShiftAmount(llvm::Value *value) {
  auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value);
  if (constant == nullptr || constant->getValue().getActiveBits() > 64) {
    return std::nullopt;
  }
  return constant->getZExtValue();
}

std::optional<NarrowUse> narrowUseFromFullLoad(llvm::LoadInst &load,
                                               unsigned fullWidth) {
  if (!load.hasOneUse()) {
    return std::nullopt;
  }

  auto *user = llvm::dyn_cast<llvm::Instruction>(*load.user_begin());
  if (user == nullptr) {
    return std::nullopt;
  }

  if (auto *trunc = llvm::dyn_cast<llvm::TruncInst>(user)) {
    unsigned readWidth = trunc->getType()->getIntegerBitWidth();
    if (readWidth < fullWidth) {
      return NarrowUse{trunc, nullptr, 0};
    }
    return std::nullopt;
  }

  if (user->getOpcode() != llvm::Instruction::LShr || !user->hasOneUse()) {
    return std::nullopt;
  }
  std::optional<uint64_t> bitOffset = constantShiftAmount(user->getOperand(1));
  if (!bitOffset || *bitOffset >= fullWidth) {
    return std::nullopt;
  }
  auto *trunc = llvm::dyn_cast<llvm::TruncInst>(*user->user_begin());
  if (trunc == nullptr) {
    return std::nullopt;
  }
  unsigned readWidth = trunc->getType()->getIntegerBitWidth();
  if (readWidth == 0 || readWidth > fullWidth - *bitOffset) {
    return std::nullopt;
  }
  return NarrowUse{trunc, user, *bitOffset};
}

llvm::CallInst *createPartialRead(llvm::LoadInst &load,
                                  llvm::GlobalVariable &global,
                                  llvm::Instruction &insertBefore,
                                  uint64_t bitOffset) {
  auto *fullType = llvm::cast<llvm::IntegerType>(load.getType());
  auto *readType = llvm::cast<llvm::IntegerType>(insertBefore.getType());
  llvm::Module *module = load.getModule();
  llvm::IRBuilder<> builder(&insertBefore);
  llvm::Function *partialRead = getOrInsertNativeRegisterPartialRead(
      *module, global.getType(), readType, fullType->getBitWidth(),
      readType->getBitWidth());
  llvm::CallInst *call = builder.CreateCall(
      partialRead,
      {&global, llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(load.getContext()), bitOffset)},
      insertBefore.getName());
  if (llvm::MDNode *metadata = load.getMetadata("notdec.register.access")) {
    call->setMetadata("notdec.register.access", metadata);
  }
  return call;
}

bool rewriteLoad(llvm::LoadInst &load,
                 NativeRegisterPreSummaryPeepholeSummary &summary) {
  llvm::GlobalVariable *global = fullRegisterLoadGlobal(load);
  if (global == nullptr) {
    return false;
  }
  ++summary.FullRegisterLoadsSeen;
  unsigned fullWidth = load.getType()->getIntegerBitWidth();
  if (fullWidth > 64) {
    return false;
  }
  if (!load.hasOneUse()) {
    ++summary.MultiUseLoadsSkipped;
    return false;
  }

  std::optional<NarrowUse> use = narrowUseFromFullLoad(load, fullWidth);
  if (!use) {
    return false;
  }

  llvm::CallInst *partialRead =
      createPartialRead(load, *global, *use->Root, use->BitOffset);
  use->Root->replaceAllUsesWith(partialRead);
  use->Root->eraseFromParent();
  if (use->Middle != nullptr && use->Middle->use_empty()) {
    use->Middle->eraseFromParent();
  }
  if (load.use_empty()) {
    load.eraseFromParent();
  }

  ++summary.Rewrites;
  if (use->BitOffset == 0 && use->Middle == nullptr) {
    ++summary.DirectTruncRewrites;
  } else {
    ++summary.ShiftTruncRewrites;
  }
  return true;
}

bool rangesOverlap(uint64_t lhsOffset, uint64_t lhsWidth, uint64_t rhsOffset,
                   uint64_t rhsWidth) {
  uint64_t lhsEnd = lhsOffset + lhsWidth;
  uint64_t rhsEnd = rhsOffset + rhsWidth;
  return lhsOffset < rhsEnd && rhsOffset < lhsEnd;
}

llvm::Value *
findInsertedRangePiece(llvm::Value *value, unsigned fullWidth,
                       uint64_t bitOffset, unsigned readWidth,
                       llvm::Type *readType,
                       llvm::SmallPtrSetImpl<llvm::Value *> &seen) {
  if (value == nullptr || !seen.insert(value).second) {
    return nullptr;
  }

  if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value)) {
    if (constant->isZero() && constant->getBitWidth() == fullWidth) {
      return llvm::ConstantInt::get(readType, 0);
    }
    return nullptr;
  }

  auto *call = llvm::dyn_cast<llvm::CallBase>(value);
  std::optional<NativeRegisterValueInsertInfo> insert =
      call == nullptr ? std::nullopt : parseNativeRegisterValueInsert(*call);
  if (!insert || insert->FullWidth != fullWidth) {
    return nullptr;
  }

  uint64_t insertOffset = insert->BitOffset;
  uint64_t insertWidth = insert->WriteWidth;
  if (bitOffset >= insertOffset &&
      bitOffset + readWidth <= insertOffset + insertWidth) {
    if (bitOffset == insertOffset && readWidth == insertWidth &&
        insert->Value->getType() == readType) {
      return insert->Value;
    }
    return nullptr;
  }

  if (rangesOverlap(bitOffset, readWidth, insertOffset, insertWidth)) {
    return nullptr;
  }
  return findInsertedRangePiece(insert->Base, fullWidth, bitOffset, readWidth,
                                readType, seen);
}

llvm::Value *simplifyInsertedValueExtract(llvm::TruncInst &trunc) {
  if (trunc.use_empty()) {
    return nullptr;
  }
  auto *sourceType =
      llvm::dyn_cast<llvm::IntegerType>(trunc.getOperand(0)->getType());
  auto *readType = llvm::dyn_cast<llvm::IntegerType>(trunc.getType());
  if (sourceType == nullptr || readType == nullptr) {
    return nullptr;
  }

  llvm::Value *fullValue = trunc.getOperand(0);
  uint64_t bitOffset = 0;
  if (auto *shift = llvm::dyn_cast<llvm::BinaryOperator>(fullValue)) {
    if (shift->getOpcode() != llvm::Instruction::LShr) {
      return nullptr;
    }
    std::optional<uint64_t> amount = constantShiftAmount(shift->getOperand(1));
    if (!amount) {
      return nullptr;
    }
    bitOffset = *amount;
    fullValue = shift->getOperand(0);
    sourceType = llvm::dyn_cast<llvm::IntegerType>(fullValue->getType());
    if (sourceType == nullptr) {
      return nullptr;
    }
  }

  unsigned fullWidth = sourceType->getBitWidth();
  unsigned readWidth = readType->getBitWidth();
  if (readWidth == 0 || readWidth >= fullWidth || bitOffset >= fullWidth ||
      readWidth > fullWidth - bitOffset) {
    return nullptr;
  }

  llvm::SmallPtrSet<llvm::Value *, 8> seen;
  return findInsertedRangePiece(fullValue, fullWidth, bitOffset, readWidth,
                                readType, seen);
}

} // namespace

NativeRegisterPreSummaryPeepholeSummary
runNativeRegisterPreSummaryPeephole(llvm::Module &module) {
  NativeRegisterPreSummaryPeepholeSummary summary;
  llvm::SmallVector<llvm::LoadInst *, 64> loads;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        loads.push_back(load);
      }
    }
  }

  for (llvm::LoadInst *load : loads) {
    if (load->getParent() != nullptr) {
      (void)rewriteLoad(*load, summary);
    }
  }
  return summary;
}

NativeRegisterPostRewritePeepholeSummary
runNativeRegisterPostRewritePeephole(llvm::Module &module) {
  NativeRegisterPostRewritePeepholeSummary summary;
  llvm::SmallVector<llvm::TruncInst *, 64> truncs;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      if (auto *trunc = llvm::dyn_cast<llvm::TruncInst>(&inst)) {
        truncs.push_back(trunc);
      }
    }
  }

  for (llvm::TruncInst *trunc : truncs) {
    if (trunc->getParent() == nullptr) {
      continue;
    }
    llvm::Instruction *middle =
        llvm::dyn_cast<llvm::Instruction>(trunc->getOperand(0));
    llvm::Value *replacement = simplifyInsertedValueExtract(*trunc);
    if (replacement == nullptr) {
      continue;
    }
    trunc->replaceAllUsesWith(replacement);
    trunc->eraseFromParent();
    if (middle != nullptr && middle->use_empty()) {
      middle->eraseFromParent();
    }
    ++summary.ValueRangeExtractsSimplified;
  }
  return summary;
}

void printNativeRegisterPreSummaryPeepholeSummary(
    const NativeRegisterPreSummaryPeepholeSummary &summary,
    llvm::raw_ostream &os) {
  os << "Native register pre-summary peephole: full_loads="
     << summary.FullRegisterLoadsSeen << " rewrites=" << summary.Rewrites
     << " direct_trunc=" << summary.DirectTruncRewrites
     << " shift_trunc=" << summary.ShiftTruncRewrites
     << " multi_use_skipped=" << summary.MultiUseLoadsSkipped << '\n';
}

void printNativeRegisterPostRewritePeepholeSummary(
    const NativeRegisterPostRewritePeepholeSummary &summary,
    llvm::raw_ostream &os) {
  os << "Native register post-rewrite peephole: value_range_extracts_simplified="
     << summary.ValueRangeExtractsSimplified << '\n';
}

} // namespace notdec::bin2llvm
