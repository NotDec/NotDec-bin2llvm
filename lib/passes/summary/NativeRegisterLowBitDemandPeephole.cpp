#include "notdec-bin2llvm/passes/summary/NativeRegisterLowBitDemandPeephole.h"

#include "notdec-bin2llvm/NativeRegisterPartialRead.h"

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
                 NativeRegisterLowBitDemandPeepholeSummary &summary) {
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

} // namespace

NativeRegisterLowBitDemandPeepholeSummary
runNativeRegisterLowBitDemandPeephole(llvm::Module &module) {
  NativeRegisterLowBitDemandPeepholeSummary summary;
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

void printNativeRegisterLowBitDemandPeepholeSummary(
    const NativeRegisterLowBitDemandPeepholeSummary &summary,
    llvm::raw_ostream &os) {
  os << "Native register low-bit demand peephole: full_loads="
     << summary.FullRegisterLoadsSeen << " rewrites=" << summary.Rewrites
     << " direct_trunc=" << summary.DirectTruncRewrites
     << " shift_trunc=" << summary.ShiftTruncRewrites
     << " multi_use_skipped=" << summary.MultiUseLoadsSkipped << '\n';
}

} // namespace notdec::bin2llvm
