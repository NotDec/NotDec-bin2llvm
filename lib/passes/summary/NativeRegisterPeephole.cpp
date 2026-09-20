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

// 编译器把无符号除以常数 d 生成成 `(x * M) >> (W + s)`（W 是被除数位宽，
// M 是 W 位魔数，s 由编译器按 d 选定）。lift 出来的形态是
// `mul i(2W) (zext x), M` + `lshr i(2W), K` + `trunc`（x86-64 的 mulq 是
// W = 64 → i128，i386 的 mull 是 W = 32 → i64）；InstCombine 之后可能是
// `lshr, K` 后直接 trunc（K = W + s），也可能是 `lshr W` + `trunc` 之后再来
// 一次修正 `lshr iW, s`。这里只做一件事：反解出 d 并把整条链换成 `udiv x, d`。
//
// 反解用 Granlund-Montgomery 的充分条件（Hacker's Delight 4-1）：
//   M == ceil(2^K / d) 且 (M*d - 2^K) * 2^L <= 2^K
// 满足时 `floor(x*M/2^K) == floor(x/d)` 对全部 x < 2^L 成立。这是证明而不是
// 抽样，所以命中即改写。L 取被除数本身的值上界：普通 i64 值 L = 64；被除数是
// `lshr i64 %y, k`（除法前先移位，如 x/100 = (x>>2)/25）时它的值恒小于
// 2^(W-k)，L 可以收到 W-k —— clang 正是按这个更窄的范围挑魔数的，d=25 那种
// 魔数只有把 L 收窄才成立。
//
// 有符号除法（`int x/d`）的 lift 形态是
// `%h = lshr i(2W) (mul nsw (sext x), M), W`、`%q = ashr iW (trunc %h), s`、
// `%sign = lshr iW (trunc %h), W-1`、`add iW %q, %sign`（s = 0 时 %q 就是那个
// trunc）。它算的是 `floor_s(x*M/2^(W+s)) + [x<0]`：x >= 0 时就是 GM 的
// `floor(x/d)`；x < 0 时逐项展开可以证明同一个条件
// `e * 2^(W-1) <= 2^K`（e = M*d - 2^K）就够，所以两种除法共用一套反解，
// 只是把 L 从 W 换成 W-1。反过来，`x + mulhi`（负魔数）那种形态不认。
//
// 不认的形态（整体跳过，不半改）：带 add 修正的无符号魔数形态（clang -O2 的
// x/7、x/4095 是 `mulhi; sub; shr; add; shr`）、有符号的负魔数 `+x` 形态。
// 除数本身不设上界：反解是 O(1) 的，而且 x/10^9 这类大除数很常见。

std::optional<uint64_t> solveMagicDivisor(const llvm::APInt &magic,
                                           unsigned shift,
                                           unsigned inputBits,
                                           unsigned valueWidth,
                                           bool allowShortShift = false) {
  // 2W 位乘积取高半段时移位一定 >= W；窄乘积形态（16 位除法提升到 32 位）
  // 在 W 位内就算完了，移位可以更小，靠 inputBits 限住输入范围。
  unsigned minShift = allowShortShift ? 1 : valueWidth;
  if (magic.isZero() || valueWidth == 0 || valueWidth > 64 ||
      magic.getActiveBits() > valueWidth || inputBits == 0 ||
      inputBits > valueWidth || shift < minShift ||
      shift > 2 * valueWidth) {
    return std::nullopt;
  }
  unsigned width = shift + 2 * valueWidth;
  llvm::APInt powerOfTwo = llvm::APInt::getOneBitSet(width, shift);
  llvm::APInt wideMagic = magic.zext(width);
  // M == ceil(2^K / d) 等价于 d 落在 [2^K/M, 2^K/(M-1)) 里，而这个区间宽度只有
  // 约 d^2/2^K（远小于 1），所以候选除数就是 ceil(2^K/M)，直接算出来再验证，
  // 不用线性搜索。
  llvm::APInt divisor = powerOfTwo.udiv(wideMagic);
  if (!powerOfTwo.urem(wideMagic).isZero()) {
    divisor += 1;
  }
  if (divisor.ult(3) || divisor.getActiveBits() > valueWidth) {
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

// 无符号 add 修正形态的等价性：`h = floor(x*M/2^N)`、`e = M*d - 2^N`，
// 代入 `x = q*d + r` 得到
//   q_out = q + floor((r + floor((q*e + r*M)/2^N)) / 2^extra)，extra = s + 1，
// 所以只要 `E(q,r) = r + floor((q*e + r*M)/2^N) < 2^extra` 对全部可行 (q,r)
// 成立就够了。E 对 q、r 都单调，而可行域是 `q*d + r <= 2^inputBits - 1` 的阶梯，
// 最大值只可能在两个角点 (q_max, r_max) 和 (q_max-1, d-1) 取到 —— 两次 APInt
// 检查就是完整证明，不需要抽样。
std::optional<uint64_t>
solveUnsignedAddMagicDivisor(const llvm::APInt &magic, unsigned highShift,
                             unsigned extraShift, unsigned inputBits,
                             unsigned valueWidth) {
  if (magic.isZero() || valueWidth == 0 || valueWidth > 64 ||
      magic.getActiveBits() > valueWidth || inputBits == 0 ||
      inputBits > valueWidth || highShift < valueWidth ||
      highShift > 2 * valueWidth || extraShift == 0 ||
      extraShift > valueWidth) {
    return std::nullopt;
  }
  unsigned width = 2 * valueWidth + inputBits + extraShift + 8;
  llvm::APInt powerOfTwo = llvm::APInt::getOneBitSet(width, highShift);
  llvm::APInt wideMagic = magic.zext(width);
  llvm::APInt divisor = powerOfTwo.udiv(wideMagic);
  if (!powerOfTwo.urem(wideMagic).isZero()) {
    divisor += 1;
  }
  if (divisor.ult(3) || divisor.getActiveBits() > valueWidth) {
    return std::nullopt;
  }
  llvm::APInt product = wideMagic * divisor;
  llvm::APInt previous = (wideMagic - 1) * divisor;
  if (product.ult(powerOfTwo) || !previous.ult(powerOfTwo)) {
    return std::nullopt; // M != ceil(2^N / d)。
  }
  llvm::APInt error = product - powerOfTwo;
  llvm::APInt limit = llvm::APInt::getOneBitSet(width, extraShift);
  llvm::APInt maxX = llvm::APInt::getLowBitsSet(width, inputBits);
  llvm::APInt qMax = (maxX - 1).udiv(divisor);
  llvm::APInt rMax = (maxX - 1) - qMax * divisor;
  auto worstError = [&](const llvm::APInt &q, const llvm::APInt &r) {
    return r + (q * error + r * wideMagic).lshr(highShift);
  };
  llvm::APInt worst = worstError(qMax, rMax);
  if (!qMax.isZero()) {
    llvm::APInt other = worstError(qMax - 1, divisor - 1);
    if (worst.ult(other)) {
      worst = other;
    }
  }
  if (!worst.ult(limit)) {
    return std::nullopt;
  }
  return divisor.getZExtValue();
}

struct MagicMulChain {
  llvm::Value *Scaled = nullptr; // 被除数（无符号是 zext 的源，有符号是 sext 的源）。
  llvm::Instruction *Root = nullptr; // 要替换掉的那个 iW 值。
  llvm::APInt Magic;
  unsigned Shift = 0; // 2W 位乘积总共右移多少位。
  unsigned InputBits = 0; // 被除数的值上界（< 2^InputBits）。
  unsigned Width = 0; // W：被除数位宽，也是 udiv/sdiv 的位宽。
  bool Signed = false;
  // 无符号 add 修正形态：`h = mulhi_u(x, M)` 之后是
  // `((x - h) >> 1 + h) >> s`，等价于 `floor((x + h) / 2^(s+1))`。
  // Shift 仍是取 h 的移位 N（一般为 W），ExtraShift = s + 1。
  bool AddCorrection = false;
  unsigned ExtraShift = 0;
  // 窄乘积形态：`mul iW` 自身宽（16 位除法被提升到 32 位），移位可以 < W。
  bool NarrowProduct = false;
};

// `%m = mul i(2W) (zext|sext iW %x), <常量>`：两种除法共用的乘积解析。
struct MagicProduct {
  llvm::Value *Scaled = nullptr; // 扩展前的值（可能比 W 窄）。
  llvm::ConstantInt *Magic = nullptr;
  bool Signed = false;
  unsigned SourceBits = 0; // Scaled 的位宽：比 W 窄时是"窄源加宽"形态。
};

std::optional<MagicProduct> parseMagicProduct(llvm::Instruction &mul,
                                              unsigned width) {
  auto *mulType = llvm::dyn_cast<llvm::IntegerType>(mul.getType());
  if (mul.getOpcode() != llvm::Instruction::Mul || mulType == nullptr ||
      mulType->getBitWidth() != 2 * width) {
    return std::nullopt;
  }
  MagicProduct product;
  for (llvm::Value *operand : mul.operands()) {
    if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(operand)) {
      if (product.Magic != nullptr) {
        return std::nullopt;
      }
      product.Magic = constant;
      continue;
    }
    if (product.Scaled != nullptr) {
      return std::nullopt;
    }
    product.Scaled = operand;
  }
  if (product.Magic == nullptr || product.Scaled == nullptr) {
    return std::nullopt;
  }
  if (auto *extend = llvm::dyn_cast<llvm::ZExtInst>(product.Scaled)) {
    product.Signed = false;
    product.Scaled = extend->getOperand(0);
  } else if (auto *extend = llvm::dyn_cast<llvm::SExtInst>(product.Scaled)) {
    product.Signed = true;
    product.Scaled = extend->getOperand(0);
  } else {
    return std::nullopt;
  }
  auto *sourceType =
      llvm::dyn_cast<llvm::IntegerType>(product.Scaled->getType());
  if (sourceType == nullptr || sourceType->getBitWidth() > width) {
    return std::nullopt; // 源比 W 宽就说明乘的不是被除数本身，不认。
  }
  product.SourceBits = sourceType->getBitWidth();
  return product;
}

// `%h = trunc i(2W) (lshr i(2W) %m, N) to iW`：取乘积高半段。
std::optional<std::pair<llvm::Instruction *, unsigned>>
parseHighHalf(llvm::Instruction &trunc, unsigned width) {
  if (!llvm::isa<llvm::TruncInst>(trunc) ||
      !trunc.getType()->isIntegerTy(width)) {
    return std::nullopt;
  }
  auto *high = llvm::dyn_cast<llvm::Instruction>(trunc.getOperand(0));
  if (high == nullptr || high->getOpcode() != llvm::Instruction::LShr) {
    return std::nullopt;
  }
  auto *amount = llvm::dyn_cast<llvm::ConstantInt>(high->getOperand(1));
  if (amount == nullptr) {
    return std::nullopt;
  }
  unsigned highShift = (unsigned)amount->getValue().getLimitedValue(2 * width);
  if (highShift < width || highShift > 2 * width) {
    return std::nullopt;
  }
  auto *mul = llvm::dyn_cast<llvm::Instruction>(high->getOperand(0));
  if (mul == nullptr || mul->getOpcode() != llvm::Instruction::Mul) {
    return std::nullopt;
  }
  return std::make_pair(mul, highShift);
}

// `lshr iW %y, k` 的结果恒小于 2^(W-k)，反解可以用更窄的范围。
unsigned dividendBitBound(llvm::Value *dividend, unsigned width) {
  auto *shift = llvm::dyn_cast<llvm::BinaryOperator>(dividend);
  if (shift == nullptr || shift->getOpcode() != llvm::Instruction::LShr) {
    return width;
  }
  auto *amount = llvm::dyn_cast<llvm::ConstantInt>(shift->getOperand(1));
  if (amount == nullptr || amount->getValue().getActiveBits() > 8) {
    return width;
  }
  uint64_t bits = amount->getValue().getZExtValue();
  if (bits == 0 || bits >= width) {
    return width;
  }
  return (unsigned)(width - bits);
}

// `%root = trunc i(2W) (lshr i(2W) %m, K)` 或
// `%root = lshr iW (trunc i(2W) (lshr i(2W) %m, W)), s`。
// W 取自乘积的一半位宽，所以 x86-64 的 mulq（i64 → i128）和 i386 的
// mull（i32 → i64）走同一套匹配。
std::optional<MagicMulChain> matchMagicMulChain(llvm::Instruction &root) {
  auto *rootType = llvm::dyn_cast<llvm::IntegerType>(root.getType());
  if (rootType == nullptr || rootType->getBitWidth() > 64) {
    return std::nullopt;
  }
  unsigned width = rootType->getBitWidth();
  llvm::Value *value = &root;
  unsigned extraShift = 0;
  if (root.getOpcode() == llvm::Instruction::LShr) {
    auto *amount = llvm::dyn_cast<llvm::ConstantInt>(root.getOperand(1));
    if (amount == nullptr) {
      return std::nullopt;
    }
    extraShift = (unsigned)amount->getValue().getLimitedValue(2 * width);
    if (extraShift == 0 || extraShift >= width) {
      return std::nullopt;
    }
    value = root.getOperand(0);
  }
  auto *trunc = llvm::dyn_cast<llvm::TruncInst>(value);
  if (trunc == nullptr || trunc->getType() != rootType) {
    return std::nullopt;
  }
  // trunc 必须来自 `lshr, K`（取乘积高半段）；裸的 `trunc(mul)` 是低半段，
  // 不是除法。
  auto *high = llvm::dyn_cast<llvm::Instruction>(trunc->getOperand(0));
  if (high == nullptr || high->getOpcode() != llvm::Instruction::LShr) {
    return std::nullopt;
  }
  auto *amount = llvm::dyn_cast<llvm::ConstantInt>(high->getOperand(1));
  if (amount == nullptr) {
    return std::nullopt;
  }
  unsigned highShift = (unsigned)amount->getValue().getLimitedValue(2 * width);
  if (highShift < width) {
    return std::nullopt;
  }
  unsigned totalShift = highShift + extraShift;
  if (totalShift > 2 * width) {
    return std::nullopt;
  }
  auto *mul = llvm::dyn_cast<llvm::Instruction>(high->getOperand(0));
  if (mul == nullptr || mul->getOpcode() != llvm::Instruction::Mul) {
    return std::nullopt;
  }
  auto *mulType = llvm::dyn_cast<llvm::IntegerType>(mul->getType());
  if (mulType == nullptr || mulType->getBitWidth() != 2 * width) {
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
  if (extend == nullptr ||
      !extend->getOperand(0)->getType()->isIntegerTy(width)) {
    return std::nullopt; // 只认 W 位无符号操作数经 zext 进 2W 位的形态。
  }
  // 链上其它使用者不影响正确性：改写只换掉 root 的值，链上指令只有在
  // use_empty() 时才会被收掉（mulq 的低半段 trunc、CF/OF 的 icmp 常常还挂在
  // 乘法和 lshr 上，但都是死值）。这里不要求单使用，否则会漏掉一半的除法。

  MagicMulChain chain;
  chain.Scaled = extend->getOperand(0);
  chain.Root = &root;
  chain.Magic = magic->getValue();
  chain.Shift = totalShift;
  chain.Width = width;
  chain.InputBits = dividendBitBound(chain.Scaled, width);
  chain.Signed = false;
  return chain;
}

// `%root = add iW %shifted, %sign`（顺序任意）：
//   %shifted = ashr iW %truncated, s（s >= 1）或直接就是 %truncated（s == 0）
//   %sign    = lshr iW <同一个 trunc 或被除数>, W-1
//   %truncated = trunc i(2W) (lshr i(2W) %m, W) to iW
//   %m = mul i(2W) (sext iW %x), M
std::optional<MagicMulChain> matchSignedMagicMulChain(llvm::Instruction &root) {
  if (root.getOpcode() != llvm::Instruction::Add) {
    return std::nullopt;
  }
  auto *rootType = llvm::dyn_cast<llvm::IntegerType>(root.getType());
  if (rootType == nullptr || rootType->getBitWidth() > 64) {
    return std::nullopt;
  }
  unsigned width = rootType->getBitWidth();
  if (width < 2) {
    return std::nullopt;
  }

  llvm::Value *shifted = nullptr;
  llvm::Instruction *sign = nullptr;
  for (llvm::Value *operand : root.operands()) {
    auto *inst = llvm::dyn_cast<llvm::Instruction>(operand);
    if (inst == nullptr) {
      return std::nullopt;
    }
    if (inst->getOpcode() == llvm::Instruction::LShr) {
      auto *amount = llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1));
      if (amount == nullptr ||
          amount->getValue() != llvm::APInt(width, width - 1)) {
        return std::nullopt; // 符号位必须是 >> (W-1)。
      }
      sign = inst;
      continue;
    }
    if (inst->getOpcode() == llvm::Instruction::AShr ||
        llvm::isa<llvm::TruncInst>(inst)) {
      if (shifted != nullptr) {
        return std::nullopt;
      }
      shifted = operand;
      continue;
    }
    return std::nullopt;
  }
  if (shifted == nullptr || sign == nullptr) {
    return std::nullopt;
  }

  unsigned extraShift = 0;
  llvm::Value *shiftedBase = shifted;
  if (auto *ashr = llvm::dyn_cast<llvm::BinaryOperator>(shifted)) {
    auto *amount = llvm::dyn_cast<llvm::ConstantInt>(ashr->getOperand(1));
    if (amount == nullptr) {
      return std::nullopt;
    }
    extraShift = (unsigned)amount->getValue().getLimitedValue(width);
    if (extraShift == 0 || extraShift >= width) {
      return std::nullopt;
    }
    shiftedBase = ashr->getOperand(0);
  }
  llvm::APInt magicValue;
  llvm::Value *scaled = nullptr;
  unsigned inputBits = width - 1; // 有符号默认 |x| < 2^(W-1)。
  if (auto *trunc = llvm::dyn_cast<llvm::TruncInst>(shiftedBase)) {
    // 简单形态：%shifted 直接来自 `trunc(lshr(mul, W))`。
    if (trunc->getType() != rootType) {
      return std::nullopt;
    }
    std::optional<std::pair<llvm::Instruction *, unsigned>> highHalf =
        parseHighHalf(*trunc, width);
    if (!highHalf || highHalf->second != width) {
      return std::nullopt; // 只认 `>> W` 取高半段。
    }
    std::optional<MagicProduct> product =
        parseMagicProduct(*highHalf->first, width);
    if (!product || !product->Signed ||
        product->Magic->getValue().isNegative()) {
      return std::nullopt; // 负魔数走下面的 `+x` 形态。
    }
    scaled = product->Scaled;
    magicValue = product->Magic->getValue();
    llvm::Value *signSource = sign->getOperand(0);
    if (product->SourceBits == width) {
      // 符号位来自高半段（同一个 trunc）或被除数本身，两者都等于 [x < 0]。
      if (signSource != trunc && signSource != scaled) {
        return std::nullopt;
      }
    } else {
      // 窄源加宽：乘的是 `sext iK %v`（K < W），而被除数是同一个 %v 的 iW 扩展
      // （符号位就取自它）。按 iK 的范围验证，改写用 iW 那个值。
      auto *wideDividend = llvm::dyn_cast<llvm::Instruction>(signSource);
      if (wideDividend == nullptr ||
          wideDividend->getOpcode() != llvm::Instruction::SExt ||
          wideDividend->getOperand(0) != scaled ||
          !wideDividend->getType()->isIntegerTy(width)) {
        return std::nullopt;
      }
      scaled = wideDividend;
      inputBits = product->SourceBits - 1;
    }
  } else {
    // `+x` 形态：`%t = add iW %x, %h`（注意是加不是减），符号位取自同一个 %t。
    // 机器里乘的是负常量 M_neg；因为 `floor(x*M_neg/2^W) = floor(x*M_u/2^W) - x`
    // （M_u = M_neg + 2^W），外面再 `+ x` 正好把它还原成 M_u 的简单形态，
    // 所以取常量的低 W 位当 M_u，照原来的有符号条件验证。
    auto *sum = llvm::dyn_cast<llvm::Instruction>(shiftedBase);
    if (sum == nullptr || sum->getOpcode() != llvm::Instruction::Add ||
        sign->getOperand(0) != sum) {
      return std::nullopt;
    }
    llvm::Instruction *highValue = nullptr;
    llvm::Value *dividend = nullptr;
    for (llvm::Value *operand : sum->operands()) {
      auto *inst = llvm::dyn_cast<llvm::Instruction>(operand);
      if (inst != nullptr && llvm::isa<llvm::TruncInst>(inst)) {
        if (highValue != nullptr) {
          return std::nullopt;
        }
        highValue = inst;
        continue;
      }
      if (dividend != nullptr) {
        return std::nullopt;
      }
      dividend = operand;
    }
    if (highValue == nullptr || dividend == nullptr) {
      return std::nullopt;
    }
    std::optional<std::pair<llvm::Instruction *, unsigned>> highHalf =
        parseHighHalf(*highValue, width);
    if (!highHalf || highHalf->second != width) {
      return std::nullopt;
    }
    std::optional<MagicProduct> product =
        parseMagicProduct(*highHalf->first, width);
    if (!product || !product->Signed || product->Scaled != dividend ||
        !product->Magic->getValue().isNegative()) {
      return std::nullopt;
    }
    scaled = dividend;
    magicValue = product->Magic->getValue().trunc(width);
  }

  MagicMulChain chain;
  chain.Scaled = scaled;
  chain.Root = &root;
  chain.Magic = magicValue;
  chain.Shift = width + extraShift;
  chain.Width = width;
  chain.InputBits = inputBits;
  chain.Signed = true;
  return chain;
}

// 无符号 add 修正形态：
//   %h = trunc i(2W) (lshr i(2W) (mul i(2W) (zext iW %x), M), N)
//   %d = sub iW %x, %h
//   %s1 = lshr iW %d, 1
//   %a = add iW %s1, %h
//   %root = lshr iW %a, s
std::optional<MagicMulChain>
matchUnsignedAddMagicMulChain(llvm::Instruction &root) {
  if (root.getOpcode() != llvm::Instruction::LShr) {
    return std::nullopt;
  }
  auto *rootType = llvm::dyn_cast<llvm::IntegerType>(root.getType());
  if (rootType == nullptr || rootType->getBitWidth() > 64) {
    return std::nullopt;
  }
  unsigned width = rootType->getBitWidth();
  auto *finalAmount = llvm::dyn_cast<llvm::ConstantInt>(root.getOperand(1));
  if (finalAmount == nullptr) {
    return std::nullopt;
  }
  unsigned finalShift = (unsigned)finalAmount->getValue().getLimitedValue(width);
  if (finalShift == 0 || finalShift >= width) {
    return std::nullopt;
  }
  auto *sum = llvm::dyn_cast<llvm::BinaryOperator>(root.getOperand(0));
  if (sum == nullptr || sum->getOpcode() != llvm::Instruction::Add) {
    return std::nullopt;
  }
  llvm::Instruction *highValue = nullptr;
  llvm::Instruction *halved = nullptr;
  for (llvm::Value *operand : sum->operands()) {
    auto *inst = llvm::dyn_cast<llvm::Instruction>(operand);
    if (inst == nullptr) {
      return std::nullopt;
    }
    if (llvm::isa<llvm::TruncInst>(inst)) {
      highValue = inst;
      continue;
    }
    if (inst->getOpcode() == llvm::Instruction::LShr) {
      halved = inst;
      continue;
    }
    return std::nullopt;
  }
  if (highValue == nullptr || halved == nullptr) {
    return std::nullopt;
  }
  auto *halvedAmount = llvm::dyn_cast<llvm::ConstantInt>(halved->getOperand(1));
  if (halvedAmount == nullptr || !halvedAmount->getValue().isOne()) {
    return std::nullopt; // 只认 `>> 1` 那一步。
  }
  auto *difference =
      llvm::dyn_cast<llvm::BinaryOperator>(halved->getOperand(0));
  if (difference == nullptr ||
      difference->getOpcode() != llvm::Instruction::Sub) {
    return std::nullopt;
  }
  llvm::Value *dividend = nullptr;
  bool subtractedHigh = false;
  for (llvm::Value *operand : difference->operands()) {
    if (operand == highValue) {
      subtractedHigh = true;
      continue;
    }
    if (dividend != nullptr) {
      return std::nullopt;
    }
    dividend = operand;
  }
  if (!subtractedHigh || dividend == nullptr) {
    return std::nullopt;
  }

  std::optional<std::pair<llvm::Instruction *, unsigned>> highHalf =
      parseHighHalf(*highValue, width);
  if (!highHalf) {
    return std::nullopt;
  }
  std::optional<MagicProduct> product =
      parseMagicProduct(*highHalf->first, width);
  if (!product || product->Signed || product->Scaled != dividend) {
    return std::nullopt;
  }

  MagicMulChain chain;
  chain.Scaled = dividend;
  chain.Root = &root;
  chain.Magic = product->Magic->getValue();
  chain.Shift = highHalf->second;
  chain.Width = width;
  chain.InputBits = dividendBitBound(dividend, width);
  chain.Signed = false;
  chain.AddCorrection = true;
  chain.ExtraShift = finalShift + 1;
  return chain;
}

// 窄乘积形态（\u4f8b\u5982 16 \u4f4d\u9664\u6cd5\u88ab\u63d0\u5347\u5230 32 \u4f4d\uff09\uff1a\u4e58\u79ef\u5c31\u5728 W \u4f4d\u5185\u7b97\u5b8c\uff0c
// \u6ca1\u6709 2W \u7684\u9ad8\u534a\u6bb5\u3002
//   unsigned: %v = zext iK %x; %p = mul iW %v, M; %q = lshr iW %p, N
//   signed:   %v = sext iK %x; %p = mul iW %v, M;
//             %q = add iW (lshr iW %p, N), (lshr iW %p, W-1)
std::optional<MagicMulChain>
matchNarrowProductMagicChain(llvm::Instruction &root) {
  auto *rootType = llvm::dyn_cast<llvm::IntegerType>(root.getType());
  if (rootType == nullptr || rootType->getBitWidth() > 64) {
    return std::nullopt;
  }
  unsigned width = rootType->getBitWidth();
  llvm::Instruction *product = nullptr;
  unsigned quotientShift = 0;
  bool signedForm = false;
  if (root.getOpcode() == llvm::Instruction::LShr) {
    product = llvm::dyn_cast<llvm::Instruction>(root.getOperand(0));
    auto *amount = llvm::dyn_cast<llvm::ConstantInt>(root.getOperand(1));
    if (product == nullptr || amount == nullptr) {
      return std::nullopt;
    }
    quotientShift = (unsigned)amount->getValue().getLimitedValue(2 * width);
  } else if (root.getOpcode() == llvm::Instruction::Add) {
    signedForm = true;
    llvm::Instruction *shifted = nullptr;
    llvm::Instruction *sign = nullptr;
    for (llvm::Value *operand : root.operands()) {
      auto *inst = llvm::dyn_cast<llvm::Instruction>(operand);
      if (inst == nullptr || inst->getOpcode() != llvm::Instruction::LShr) {
        return std::nullopt;
      }
      auto *amount = llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1));
      if (amount == nullptr) {
        return std::nullopt;
      }
      if (amount->getValue() == llvm::APInt(width, width - 1)) {
        sign = inst;
      } else {
        shifted = inst;
        quotientShift = (unsigned)amount->getValue().getLimitedValue(2 * width);
      }
    }
    if (shifted == nullptr || sign == nullptr ||
        shifted->getOperand(0) != sign->getOperand(0)) {
      return std::nullopt;
    }
    product = llvm::dyn_cast<llvm::Instruction>(shifted->getOperand(0));
  } else {
    return std::nullopt;
  }
  if (product == nullptr || product->getOpcode() != llvm::Instruction::Mul ||
      product->getType() != rootType || quotientShift == 0 ||
      quotientShift >= width) {
    return std::nullopt;
  }

  llvm::ConstantInt *magic = nullptr;
  llvm::Value *scaled = nullptr;
  for (llvm::Value *operand : product->operands()) {
    if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(operand)) {
      if (magic != nullptr) {
        return std::nullopt;
      }
      magic = constant;
      continue;
    }
    if (scaled != nullptr) {
      return std::nullopt;
    }
    scaled = operand;
  }
  if (magic == nullptr || scaled == nullptr) {
    return std::nullopt;
  }
  llvm::Value *source = nullptr;
  if (auto *extend = llvm::dyn_cast<llvm::ZExtInst>(scaled)) {
    if (signedForm) {
      return std::nullopt;
    }
    source = extend->getOperand(0);
  } else if (auto *extend = llvm::dyn_cast<llvm::SExtInst>(scaled)) {
    if (!signedForm) {
      return std::nullopt;
    }
    source = extend->getOperand(0);
  } else {
    return std::nullopt;
  }
  auto *sourceType = llvm::dyn_cast<llvm::IntegerType>(source->getType());
  if (sourceType == nullptr || sourceType->getBitWidth() >= width) {
    return std::nullopt; // 只认比 W 窄的源（提升过的窄除法）。
  }
  unsigned sourceBits = sourceType->getBitWidth();
  unsigned inputBits = signedForm ? sourceBits - 1 : sourceBits;

  // 乘积必须真的在 W 位内算得下，否则机器码是回绕后的值，公式不成立。
  unsigned checkWidth = width + sourceBits + 8;
  llvm::APInt maxMagnitude =
      signedForm ? llvm::APInt::getOneBitSet(checkWidth, sourceBits - 1)
                 : llvm::APInt::getLowBitsSet(checkWidth, sourceBits);
  llvm::APInt productMax = maxMagnitude * magic->getValue().zext(checkWidth);
  unsigned productBits = signedForm ? width - 1 : width;
  if (productMax.getActiveBits() > productBits) {
    return std::nullopt;
  }

  MagicMulChain chain;
  chain.Scaled = scaled; // 已经是 iW 的值（zext/sext 的结果）。
  chain.Root = &root;
  chain.Magic = magic->getValue();
  chain.Shift = quotientShift;
  chain.Width = width;
  chain.InputBits = inputBits;
  chain.Signed = signedForm;
  chain.NarrowProduct = true;
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
  std::optional<MagicMulChain> chain = matchSignedMagicMulChain(root);
  if (!chain) {
    chain = matchUnsignedAddMagicMulChain(root);
  }
  if (!chain) {
    chain = matchMagicMulChain(root);
  }
  if (!chain) {
    chain = matchNarrowProductMagicChain(root);
  }
  if (!chain) {
    return false;
  }
  std::optional<uint64_t> divisor =
      chain->AddCorrection
          ? solveUnsignedAddMagicDivisor(chain->Magic, chain->Shift,
                                         chain->ExtraShift, chain->InputBits,
                                         chain->Width)
          : solveMagicDivisor(chain->Magic, chain->Shift, chain->InputBits,
                              chain->Width, chain->NarrowProduct);
  if (!divisor) {
    return false;
  }
  std::string name = root.getName().str();
  llvm::IRBuilder<> builder(&root);
  llvm::Value *denominator =
      llvm::ConstantInt::get(root.getType(), *divisor);
  llvm::Value *quotient = chain->Signed
                              ? builder.CreateSDiv(chain->Scaled, denominator)
                              : builder.CreateUDiv(chain->Scaled, denominator);
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
  if (chain->Signed) {
    ++summary.SignedConstantDivisionsRecognized;
  } else {
    ++summary.ConstantDivisionsRecognized;
  }
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
      // W = 32（i386 的 mull）和 W = 64（x86-64 的 mulq）都要扫；有符号除法的
      // 根是最后那个 `add iW %q, %sign`，无符号是 trunc 或修正用的 lshr。
      if ((inst.getType()->isIntegerTy(32) ||
           inst.getType()->isIntegerTy(64)) &&
          (llvm::isa<llvm::TruncInst>(inst) ||
           inst.getOpcode() == llvm::Instruction::LShr ||
           inst.getOpcode() == llvm::Instruction::Add)) {
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
     << " signed_constant_divisions_recognized="
     << summary.SignedConstantDivisionsRecognized << '\n';
}

} // namespace notdec::bin2llvm
