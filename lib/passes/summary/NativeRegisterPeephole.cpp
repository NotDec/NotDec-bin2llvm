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

// 编译器把无符号除以常数 d 生成成 `(x * M) >> (64 + s)`（M 是 64 位魔数，
// s 由编译器按 d 选定）。lift 出来的形态是 `mul nuw i128 (zext x), M` +
// `lshr i128, K` + `trunc`；InstCombine 之后可能是 `lshr i128, K` 后直接
// trunc（K = 64 + s），也可能是 `lshr 64` + `trunc` 之后再来一次修正
// `lshr i64, s`。这里只做一件事：反解出 d 并把整条链换成 `udiv x, d`。
//
// 反解用 Granlund-Montgomery 的充分条件（Hacker's Delight 4-1）：
//   M == ceil(2^K / d) 且 (M*d - 2^K) * 2^L <= 2^K
// 满足时 `floor(x*M/2^K) == floor(x/d)` 对全部 x < 2^L 成立。这是证明而不是
// 抽样，所以命中即改写。L 取被除数本身的值上界：普通 i64 值 L = 64；被除数是
// `lshr i64 %y, k`（除法前先移位，如 x/100 = (x>>2)/25）时它的值恒小于
// 2^(64-k)，L 可以收到 64-k —— clang 正是按这个更窄的范围挑魔数的，d=25 那种
// 魔数只有把 L 收窄才成立。
//
// 不认的形态（整体跳过，不半改）：带 add 修正的魔数形态（clang -O2 的 x/7、
// x/4095 是 `mulhi; sub; shr; add; shr`）、有符号除法（sext/ashr 链）。
// 除数本身不设上界：反解是 O(1) 的，而且 x/10^9 这类大除数很常见。
constexpr unsigned kMinMagicShift = 64;
// 误差条件 (M*d - 2^K) * 2^L <= 2^K 里 e < d，d 最大到 2^64，所以 K 最大是
// 64 + 64；这里按 64 + L 留够就行，不额外收紧。
constexpr unsigned kMaxMagicShift = 128;

std::optional<uint64_t> solveUnsignedMagicDivisor(const llvm::APInt &magic,
                                                  unsigned shift,
                                                  unsigned inputBits) {
  if (magic.isZero() || magic.getActiveBits() > 64 ||
      shift < kMinMagicShift || shift > kMaxMagicShift || inputBits == 0 ||
      inputBits > 64) {
    return std::nullopt;
  }
  unsigned width = shift + 128;
  llvm::APInt powerOfTwo = llvm::APInt::getOneBitSet(width, shift);
  llvm::APInt wideMagic = magic.zext(width);
  // M == ceil(2^K / d) 等价于 d 落在 [2^K/M, 2^K/(M-1)) 里，而这个区间宽度只有
  // 约 d^2/2^K（远小于 1），所以候选除数就是 ceil(2^K/M)，直接算出来再验证，
  // 不用线性搜索。
  llvm::APInt divisor = powerOfTwo.udiv(wideMagic);
  if (!powerOfTwo.urem(wideMagic).isZero()) {
    divisor += 1;
  }
  if (divisor.ult(3) || divisor.getActiveBits() > 64) {
    return std::nullopt;
  }
  llvm::APInt product = wideMagic * divisor;
  if (product.ult(powerOfTwo)) {
    return std::nullopt; // M 比 ceil(2^K / d) 小。
  }
  llvm::APInt previous = (wideMagic - 1) * divisor;
  if (!previous.ult(powerOfTwo)) {
    return std::nullopt; // M 比 ceil(2^K / d) 大。
  }
  llvm::APInt error = product - powerOfTwo;
  if (error.shl(inputBits).ugt(powerOfTwo)) {
    return std::nullopt; // 误差超过这个被除数范围能容忍的大小。
  }
  return divisor.getZExtValue();
}

struct MagicMulChain {
  llvm::Value *Scaled = nullptr; // zext 的源，i64 被除数。
  llvm::Instruction *Root = nullptr; // 要替换掉的那个 i64 值。
  llvm::APInt Magic;
  unsigned Shift = 0; // 128 位乘积总共右移多少位。
  unsigned InputBits = 64; // 被除数的值上界（< 2^InputBits）。
};

// `lshr i64 %y, k` 的结果恒小于 2^(64-k)，反解可以用更窄的范围。
unsigned dividendBitBound(llvm::Value *dividend) {
  auto *shift = llvm::dyn_cast<llvm::BinaryOperator>(dividend);
  if (shift == nullptr || shift->getOpcode() != llvm::Instruction::LShr) {
    return 64;
  }
  auto *amount = llvm::dyn_cast<llvm::ConstantInt>(shift->getOperand(1));
  if (amount == nullptr || amount->getValue().getActiveBits() > 8) {
    return 64;
  }
  uint64_t bits = amount->getValue().getZExtValue();
  if (bits == 0 || bits >= 64) {
    return 64;
  }
  return (unsigned)(64 - bits);
}

// `%root = trunc i128 (lshr i128 %m, K)` 或
// `%root = lshr i64 (trunc i128 (lshr i128 %m, 64)), s`。
std::optional<MagicMulChain> matchMagicMulChain(llvm::Instruction &root) {
  llvm::Value *value = &root;
  unsigned extraShift = 0;
  if (root.getOpcode() == llvm::Instruction::LShr) {
    auto *amount = llvm::dyn_cast<llvm::ConstantInt>(root.getOperand(1));
    if (amount == nullptr ||
        root.getType() != llvm::Type::getInt64Ty(root.getContext())) {
      return std::nullopt;
    }
    extraShift = (unsigned)amount->getValue().getLimitedValue(64);
    if (extraShift == 0 || extraShift >= 64) {
      return std::nullopt;
    }
    value = root.getOperand(0);
  }
  auto *trunc = llvm::dyn_cast<llvm::TruncInst>(value);
  if (trunc == nullptr ||
      trunc->getType() != llvm::Type::getInt64Ty(root.getContext())) {
    return std::nullopt;
  }
  auto *high = llvm::dyn_cast<llvm::Instruction>(trunc->getOperand(0));
  if (high == nullptr || high->getOpcode() != llvm::Instruction::LShr) {
    return std::nullopt;
  }
  auto *amount = llvm::dyn_cast<llvm::ConstantInt>(high->getOperand(1));
  if (amount == nullptr) {
    return std::nullopt;
  }
  unsigned highShift = (unsigned)amount->getValue().getLimitedValue(128);
  if (highShift < kMinMagicShift) {
    return std::nullopt;
  }
  unsigned totalShift = highShift + extraShift;
  if (totalShift > kMaxMagicShift) {
    return std::nullopt;
  }
  auto *mul = llvm::dyn_cast<llvm::Instruction>(high->getOperand(0));
  if (mul == nullptr || mul->getOpcode() != llvm::Instruction::Mul) {
    return std::nullopt;
  }

  llvm::ConstantInt *magic = nullptr;
  llvm::Value *scaled = nullptr;
  for (llvm::Value *operand : mul->operands()) {
    if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(operand)) {
      magic = constant;
      continue;
    }
    if (scaled != nullptr) {
      return std::nullopt; // 两个非常量操作数：不是 `x * magic`。
    }
    scaled = operand;
  }
  if (magic == nullptr || scaled == nullptr) {
    return std::nullopt;
  }
  auto *extend = llvm::dyn_cast<llvm::ZExtInst>(scaled);
  if (extend == nullptr || !extend->getOperand(0)->getType()->isIntegerTy(64)) {
    return std::nullopt; // 只认 64 位无符号操作数经 zext 进 i128 的形态。
  }
  // 链上其它使用者不影响正确性：改写只换掉 root 的值，链上指令只有在
  // use_empty() 时才会被收掉（mulq 的低半段 trunc、CF/OF 的 icmp 常常还挂在
  // 乘法和 lshr 上，但都是死值）。这里不要求单使用，否则会漏掉一半的除法。

  MagicMulChain chain;
  chain.Scaled = extend->getOperand(0);
  chain.Root = &root;
  chain.Magic = magic->getValue();
  chain.Shift = totalShift;
  chain.InputBits = dividendBitBound(chain.Scaled);
  return chain;
}

// 只在根已经被删掉之后调用：从根原来的操作数出发，把只为这条链活着的
// zext/mul/lshr/trunc 收掉（每一步都要求 use_empty）。
void eraseDeadMagicMulChain(
    const llvm::SmallVectorImpl<llvm::Instruction *> &operands) {
  llvm::SmallVector<llvm::Instruction *, 4> worklist(operands.begin(),
                                                     operands.end());
  auto pushOperands = [&](llvm::Instruction &inst) {
    for (llvm::Value *operand : inst.operands()) {
      if (auto *operandInst = llvm::dyn_cast<llvm::Instruction>(operand)) {
        worklist.push_back(operandInst);
      }
    }
  };
  while (!worklist.empty()) {
    llvm::Instruction *inst = worklist.pop_back_val();
    if (inst->getParent() == nullptr || !inst->use_empty() ||
        inst->mayHaveSideEffects() || llvm::isa<llvm::PHINode>(inst)) {
      continue;
    }
    pushOperands(*inst);
    inst->eraseFromParent();
  }
}

bool rewriteConstantDivision(llvm::Instruction &root,
                             NativeRegisterPostRewritePeepholeSummary &summary) {
  std::optional<MagicMulChain> chain = matchMagicMulChain(root);
  if (!chain) {
    return false;
  }
  std::optional<uint64_t> divisor = solveUnsignedMagicDivisor(
      chain->Magic, chain->Shift, chain->InputBits);
  if (!divisor) {
    return false;
  }
  std::string name = root.getName().str();
  llvm::IRBuilder<> builder(&root);
  llvm::Value *quotient =
      builder.CreateUDiv(chain->Scaled,
                         llvm::ConstantInt::get(root.getType(), *divisor));
  quotient->takeName(&root);
  llvm::SmallVector<llvm::Instruction *, 4> chainOperands;
  for (llvm::Value *operand : root.operands()) {
    if (auto *operandInst = llvm::dyn_cast<llvm::Instruction>(operand)) {
      chainOperands.push_back(operandInst);
    }
  }
  root.replaceAllUsesWith(quotient);
  if (root.getParent() != nullptr && root.use_empty()) {
    root.eraseFromParent();
  }
  eraseDeadMagicMulChain(chainOperands);
  if (!name.empty() && quotient->getName().empty()) {
    quotient->setName(name);
  }
  ++summary.ConstantDivisionsRecognized;
  return true;
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
  // 常数除法识别先做：命中会删掉链上的 trunc/lshr，所以这里用 WeakVH 收集
  // 候选根，后面的 value-range 化简再重新收集一次 trunc。
  llvm::SmallVector<llvm::WeakVH, 64> divisionRoots;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      if (inst.getType()->isIntegerTy(64) &&
          (llvm::isa<llvm::TruncInst>(inst) ||
           inst.getOpcode() == llvm::Instruction::LShr)) {
        divisionRoots.emplace_back(&inst);
      }
    }
  }
  for (llvm::WeakVH &handle : divisionRoots) {
    auto *root = llvm::dyn_cast_or_null<llvm::Instruction>(handle);
    if (root != nullptr && root->getParent() != nullptr) {
      (void)rewriteConstantDivision(*root, summary);
    }
  }

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
     << summary.ValueRangeExtractsSimplified
     << " constant_divisions_recognized=" << summary.ConstantDivisionsRecognized
     << '\n';
}

} // namespace notdec::bin2llvm
