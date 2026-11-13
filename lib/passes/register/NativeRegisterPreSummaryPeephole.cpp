#include "notdec-bin2llvm/passes/register/NativeRegisterPreSummaryPeephole.h"

#include "notdec-bin2llvm/intrinsics/NativeRegisterPartialRead.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterPartialWrite.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterValueRange.h"

#include "PeepholeUtils.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
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

// ===== 按"观察宽度"收窄寄存器读：GCC 常拿宽指令算窄值 =====
//
// GCC 对窄值（char、16 位值）经常用 32 位指令算、只观察低位，常见三种形态：
//   - switch 跳转表下标：`leal -lo(%rdx), %ecx; cmpb $range, %cl; movzbl %cl, %ecx`
//     （gcc/expr.cc 的 try_tablejump / do_tablejump；clang 不生成这个形状）；
//   - 字节自增自减：`movb X, %dl; ...; decl %edx; ...; movb %dl, Y`；
//   - 寄存器搬移：`movzbl X, %esi; ...; mov ebx, esi; ...; movb %bl, Y`。
// 共同点：写进寄存器的值最后只被低 K 位观察。提升后每次读都是 partial_read，
// 宽的那次"高位没有人看"，却会被记成入口依赖并沿调用图放大。
//
// 规则（要么整条链都成立、要么整体跳过）：
//   1. 锚点：一次寄存器写 partial_write.i{writeWidth}(@R2, %v, 0) + 写回读 / zext / 整存；
//   2. 观察宽度：从这个写走到 @R2 下一次写，收集每次读覆盖到的最高位，取最大值 K；
//      这段里出现比本次写更宽的观察、整寄存器 load、真实 call、ret 逃逸就跳过；
//   3. 反向 use-def：从 %v 出发，只走低位封闭的运算（加/减/乘/与/或/异或宽度不变，
//      常量左移减 c、常量右移加 c），遇到 partial_read.iW 且 K < W 就记成收窄目标；
//      load 结果、调用结果、参数、除法、比较、变量移位都是边界，不再往回走；
//   4. 使用者闭包：被改动过的值，使用者必须都还在这条链上（或就是那次写）。
// 只收窄读：运算和写都保持原类型——收窄写会让寄存器高位不再被覆盖，在按位 may
// 记账里反而制造更多入口读。
//
// 低位等价：加/减/乘/与/或/异或的结果低 K 位只取决于操作数的低 K 位（进位只往上走）；
// 左移 c 位的结果低 K 位只取决于低 K-c 位；右移 c 位取决于低 K+c 位。所以把读收窄到
// K 位再 zext 回去，低 K 位逐位不变，高位按第 2、4 条没有观察者。

bool isRegisterGlobalPointer(llvm::Value *value, llvm::GlobalVariable *global) {
  return value != nullptr && value->stripPointerCasts() == global;
}

// 从 start 走到 @global 下一次写为止，返回这段里被观察到的最高位 K。
// 返回 false 表示这个写入值不能安全收窄。
bool observedWidthUntilNextWrite(llvm::Instruction *start,
                                 llvm::GlobalVariable *global,
                                 unsigned writeWidth, unsigned &observedWidth) {
  llvm::SmallPtrSet<llvm::BasicBlock *, 16> visited;
  llvm::SmallVector<std::pair<llvm::BasicBlock *, llvm::Instruction *>, 16>
      worklist;
  worklist.push_back({start->getParent(), start});
  while (!worklist.empty()) {
    llvm::BasicBlock *block = worklist.back().first;
    llvm::Instruction *inst = worklist.back().second;
    worklist.pop_back();
    if (!visited.insert(block).second) {
      continue;
    }
    while (inst != nullptr) {
      llvm::Instruction *next = inst->getNextNode();
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(inst)) {
        if (std::optional<NativeRegisterPartialReadInfo> read =
                parseNativeRegisterPartialRead(*call)) {
          if (read->Global == global) {
            unsigned upper = static_cast<unsigned>(read->BitOffset) +
                             static_cast<unsigned>(read->ReadWidth);
            if (upper > writeWidth) {
              return false; // 观察到了本次写之外的位（整寄存器读一类）
            }
            observedWidth = std::max(observedWidth, upper);
          }
          inst = next;
          continue;
        }
        if (std::optional<NativeRegisterPartialWriteInfo> write =
                parseNativeRegisterPartialWrite(*call)) {
          if (write->Global == global) {
            break; // 值被下一次写覆盖，观察区间结束
          }
          inst = next;
          continue;
        }
        llvm::Function *callee = call->getCalledFunction();
        bool harmless =
            callee != nullptr &&
            (callee->isIntrinsic() ||
             callee->getName().starts_with("notdec."));
        if (!harmless) {
          return false; // 未知调用可能把该寄存器当参数读走
        }
        inst = next;
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
        if (isRegisterGlobalPointer(store->getPointerOperand(), global)) {
          break; // 整寄存器写，观察区间结束
        }
        inst = next;
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
        if (isRegisterGlobalPointer(load->getPointerOperand(), global)) {
          return false; // 整寄存器 load
        }
        inst = next;
        continue;
      }
      if (inst->isTerminator()) {
        if (llvm::isa<llvm::ReturnInst>(inst) ||
            llvm::isa<llvm::UnreachableInst>(inst) ||
            llvm::isa<llvm::ResumeInst>(inst)) {
          return false; // 值还活着就离开函数，算逃逸
        }
        for (llvm::BasicBlock *successor : llvm::successors(block)) {
          worklist.push_back({successor, &successor->front()});
        }
        break;
      }
      for (llvm::Use &operand : inst->operands()) {
        if (isRegisterGlobalPointer(operand.get(), global)) {
          return false; // 该寄存器以别的形式参与运算
        }
      }
      inst = next;
    }
  }
  return true;
}

// 支持的收窄宽度：只认 8/16/32，别的宽度交给更靠后的分析，这里不发明新读法。
bool isSupportedReadWidth(unsigned width) {
  return width == 8 || width == 16 || width == 32;
}

// 反向 use-def：needed 表示"这个值的低 needed 位必须正确"。
// traversed 收集走过的所有指令（含边界），targets 收集要收窄的寄存器读。
bool collectNarrowableReads(
    llvm::Value *root, unsigned neededWidth, llvm::Instruction *anchorWrite,
    llvm::SmallPtrSetImpl<llvm::Instruction *> &traversed,
    llvm::SmallVectorImpl<std::pair<llvm::CallInst *, unsigned>> &targets) {
  llvm::SmallVector<std::pair<llvm::Value *, unsigned>, 16> worklist;
  worklist.push_back({root, neededWidth});
  while (!worklist.empty()) {
    llvm::Value *value = worklist.back().first;
    unsigned needed = worklist.back().second;
    worklist.pop_back();
    auto *inst = llvm::dyn_cast<llvm::Instruction>(value);
    if (inst == nullptr || needed == 0) {
      continue; // 常量、参数，或者不需要任何位
    }
    if (!traversed.insert(inst).second) {
      continue; // 已经走过：第一次的宽度更保守，够用
    }
    auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(inst);
    if (auto *call = llvm::dyn_cast<llvm::CallInst>(inst)) {
      std::optional<NativeRegisterPartialReadInfo> read =
          parseNativeRegisterPartialRead(*call);
      if (read) {
        if (needed < read->ReadWidth) {
          targets.push_back({call, needed});
        }
        continue; // 链底就是它，继续往回只是寄存器历史，这里不做
      }
      continue; // 调用结果：边界
    }
    if (binary == nullptr) {
      continue; // load、比较、select、类型转换以外的指令：边界
    }
    unsigned resultWidth = binary->getType()->isIntegerTy()
                               ? binary->getType()->getIntegerBitWidth()
                               : 0;
    if (resultWidth == 0) {
      continue;
    }
    needed = std::min(needed, resultWidth);
    auto pushOperand = [&](llvm::Value *operand, unsigned operandNeeded) {
      unsigned operandWidth =
          operand->getType()->isIntegerTy()
              ? operand->getType()->getIntegerBitWidth()
              : 0;
      if (operandWidth == 0) {
        return;
      }
      worklist.push_back({operand, std::min(operandNeeded, operandWidth)});
    };
    switch (binary->getOpcode()) {
    case llvm::Instruction::Add:
    case llvm::Instruction::Sub:
    case llvm::Instruction::Mul:
    case llvm::Instruction::And:
    case llvm::Instruction::Or:
    case llvm::Instruction::Xor:
      for (llvm::Value *operand : binary->operands()) {
        pushOperand(operand, needed);
      }
      break;
    case llvm::Instruction::Shl: {
      auto *amount = llvm::dyn_cast<llvm::ConstantInt>(binary->getOperand(1));
      if (amount == nullptr || amount->getValue().uge(64)) {
        break; // 变量移位：边界
      }
      unsigned shift = static_cast<unsigned>(amount->getZExtValue());
      pushOperand(binary->getOperand(0), shift < needed ? needed - shift : 0);
      break;
    }
    case llvm::Instruction::LShr:
    case llvm::Instruction::AShr: {
      auto *amount = llvm::dyn_cast<llvm::ConstantInt>(binary->getOperand(1));
      if (amount == nullptr || amount->getValue().uge(64)) {
        break; // 变量移位：边界
      }
      unsigned shift = static_cast<unsigned>(amount->getZExtValue());
      pushOperand(binary->getOperand(0), needed + shift);
      break;
    }
    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt:
    case llvm::Instruction::Trunc:
      pushOperand(binary->getOperand(0), needed);
      break;
    default:
      break; // 除法、取模等：低 K 位不封闭，边界
    }
  }
  (void)anchorWrite;
  return true;
}

// 命中就按观察宽度收窄链上的寄存器读；任一条件不成立整体跳过。
bool narrowObservedReads(llvm::CallBase &valueWrite,
                         NativeRegisterPreSummaryPeepholeSummary &summary) {
  std::optional<NativeRegisterPartialWriteInfo> write =
      parseNativeRegisterPartialWrite(valueWrite);
  if (!write || write->BitOffset != 0 || write->WriteWidth < 8 ||
      write->WriteWidth > 32) {
    return false;
  }
  llvm::GlobalVariable *destination = write->Global;
  unsigned writeWidth = static_cast<unsigned>(write->WriteWidth);
  // 写回读 -> zext -> 整存：低位写零扩展的固定物化。
  auto *backRead =
      llvm::dyn_cast_or_null<llvm::CallInst>(valueWrite.getNextNode());
  auto *extend = llvm::dyn_cast_or_null<llvm::ZExtInst>(
      backRead != nullptr ? backRead->getNextNode() : nullptr);
  auto *materialize = llvm::dyn_cast_or_null<llvm::StoreInst>(
      extend != nullptr ? extend->getNextNode() : nullptr);
  if (backRead == nullptr || extend == nullptr || materialize == nullptr) {
    return false;
  }
  std::optional<NativeRegisterPartialReadInfo> back =
      parseNativeRegisterPartialRead(*backRead);
  if (!back || back->Global != destination || back->BitOffset != 0 ||
      back->ReadWidth != writeWidth) {
    return false;
  }
  if (extend->getOperand(0) != backRead ||
      materialize->getValueOperand() != extend ||
      !isRegisterGlobalPointer(materialize->getPointerOperand(), destination) ||
      !backRead->hasOneUse() || !extend->hasOneUse()) {
    return false;
  }
  llvm::Instruction *afterMaterialize = materialize->getNextNode();
  if (afterMaterialize == nullptr) {
    return false;
  }
  unsigned observedWidth = 0;
  if (!observedWidthUntilNextWrite(afterMaterialize, destination, writeWidth,
                                   observedWidth)) {
    return false;
  }
  if (observedWidth == 0 || observedWidth >= writeWidth) {
    return false; // 没人读，或者整个宽度都被观察，没什么可收窄
  }
  llvm::SmallPtrSet<llvm::Instruction *, 16> traversed;
  llvm::SmallVector<std::pair<llvm::CallInst *, unsigned>, 4> targets;
  if (!collectNarrowableReads(write->Value, observedWidth, &valueWrite,
                              traversed, targets)) {
    return false;
  }
  llvm::SmallVector<std::pair<llvm::CallInst *, unsigned>, 4> usable;
  for (auto &target : targets) {
    if (isSupportedReadWidth(target.second)) {
      usable.push_back(target);
    }
  }
  if (usable.empty()) {
    return false;
  }
  // 使用者闭包：被改动的值不能有链外的使用者。
  llvm::SmallPtrSet<llvm::Instruction *, 16> changed;
  llvm::SmallVector<llvm::Instruction *, 8> queue;
  for (auto &target : usable) {
    queue.push_back(target.first);
  }
  while (!queue.empty()) {
    llvm::Instruction *inst = queue.pop_back_val();
    if (!changed.insert(inst).second) {
      continue;
    }
    for (llvm::User *user : inst->users()) {
      if (user == &valueWrite) {
        continue;
      }
      auto *userInst = llvm::dyn_cast<llvm::Instruction>(user);
      if (userInst == nullptr || !traversed.contains(userInst)) {
        return false; // 链外还有人看这些值的高位
      }
      queue.push_back(userInst);
    }
  }
  // 改写：partial_read.iW -> partial_read.i{needed} + zext i{needed}->iW。
  llvm::Module *module = valueWrite.getModule();
  for (auto &target : usable) {
    llvm::CallInst *read = target.first;
    if (read->getParent() == nullptr) {
      continue;
    }
    std::optional<NativeRegisterPartialReadInfo> info =
        parseNativeRegisterPartialRead(*read);
    if (!info) {
      continue;
    }
    llvm::Function *narrowRead = getOrInsertNativeRegisterPartialRead(
        *module, info->Global->getType(),
        llvm::Type::getIntNTy(module->getContext(), target.second),
        info->FullWidth, target.second);
    llvm::IRBuilder<> builder(read);
    llvm::CallInst *narrow = builder.CreateCall(
        narrowRead,
        {info->Global,
         llvm::ConstantInt::get(llvm::Type::getInt64Ty(module->getContext()),
                                info->BitOffset)});
    if (llvm::MDNode *metadata = read->getMetadata("notdec.register.access")) {
      narrow->setMetadata("notdec.register.access", metadata);
    }
    llvm::Value *extended = builder.CreateZExt(narrow, read->getType());
    std::string name = read->getName().str();
    read->replaceAllUsesWith(extended);
    read->eraseFromParent();
    if (!name.empty()) {
      extended->setName(name);
    }
    ++summary.ByteIndexRewrites;
  }
  return true;
}

// ===== 死标志位物化消除（lifter 自己发的标志位，不是编译器模式） =====
//
// 提升器给每条算术操作都发 CF/OF/SF/ZF/PF 物化（p-code 的 flag 输出），但机器码里
// 很多标志位根本没被读过（GCC 不会为死标志位生成 setcc/分支）。这些计算会连带读
// 寄存器，把"读了但没人看"的位记进入口依赖。这里只认这一条逻辑：
//   flag store 到下一次同 flag 的写、调用或函数出口之间没有任何读 → 死，
//   连同它现在没人用的计算链（含链底读寄存器的那次 partial_read）一起删掉。
// 活的标志位（后面有分支读它）原样保留。

bool isArithmeticFlagGlobal(llvm::GlobalVariable *global) {
  if (global == nullptr) {
    return false;
  }
  llvm::StringRef name = global->getName();
  return name == "CF" || name == "OF" || name == "SF" || name == "ZF" ||
         name == "PF";
}

// 这个 flag store 之后有没有人读（有读就是活的）。
bool flagValueIsRead(llvm::StoreInst &store, llvm::GlobalVariable *global) {
  llvm::SmallPtrSet<llvm::BasicBlock *, 16> visited;
  llvm::SmallVector<std::pair<llvm::BasicBlock *, llvm::Instruction *>, 16>
      worklist;
  worklist.push_back({store.getParent(), store.getNextNode()});
  while (!worklist.empty()) {
    llvm::BasicBlock *block = worklist.back().first;
    llvm::Instruction *inst = worklist.back().second;
    worklist.pop_back();
    if (!visited.insert(block).second) {
      continue;
    }
    while (inst != nullptr) {
      llvm::Instruction *next = inst->getNextNode();
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
        if (isRegisterGlobalPointer(load->getPointerOperand(), global)) {
          return true;  // 有人读这个标志位
        }
      } else if (auto *other = llvm::dyn_cast<llvm::StoreInst>(inst)) {
        if (isRegisterGlobalPointer(other->getPointerOperand(), global)) {
          break;  // 被重新定义，这条路径结束
        }
      } else if (auto *call = llvm::dyn_cast<llvm::CallBase>(inst)) {
        llvm::Function *callee = call->getCalledFunction();
        bool keepsFlag =
            callee != nullptr &&
            (callee->isIntrinsic() || callee->getName().starts_with("notdec."));
        if (!keepsFlag) {
          break;  // 真实调用按 ABI 清标志位，之后与本 store 无关
        }
      }
      if (inst->isTerminator()) {
        if (llvm::isa<llvm::ReturnInst>(inst) ||
            llvm::isa<llvm::UnreachableInst>(inst) ||
            llvm::isa<llvm::ResumeInst>(inst)) {
          break;  // 走到函数出口还没有读，这条路径结束
        }
        for (llvm::BasicBlock *successor : llvm::successors(block)) {
          worklist.push_back({successor, &successor->front()});
        }
        break;
      }
      inst = next;
    }
  }
  return false;
}

// 链路节点能不能跟着删：读写寄存器的 helper、纯运算和 intrinsic 可以；
// store / 真实调用 / partial_write / 终结指令不行。
bool isRemovableFlagChainNode(llvm::Instruction &inst) {
  if (inst.isTerminator() || llvm::isa<llvm::StoreInst>(inst)) {
    return false;
  }
  auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
  if (call == nullptr) {
    return true;
  }
  llvm::Function *callee = call->getCalledFunction();
  if (callee == nullptr) {
    return false;
  }
  if (callee->isIntrinsic()) {
    return true;
  }
  llvm::StringRef name = callee->getName();
  if (!name.starts_with("notdec.")) {
    return false;
  }
  return !name.starts_with("notdec.partial_write.");
}

// 删掉死 store 和它现在没人用的计算链；链上还有别的使用者的节点保留。
void eraseDeadFlagValue(llvm::Instruction *root,
                        NativeRegisterPreSummaryPeepholeSummary &summary) {
  llvm::SmallVector<llvm::Instruction *, 16> pending;
  pending.push_back(root);
  while (!pending.empty()) {
    llvm::Instruction *inst = pending.pop_back_val();
    if (inst->getParent() == nullptr || !inst->use_empty()) {
      continue;
    }
    for (llvm::Use &operand : inst->operands()) {
      auto *operandInst = llvm::dyn_cast<llvm::Instruction>(operand.get());
      if (operandInst != nullptr && isRemovableFlagChainNode(*operandInst)) {
        pending.push_back(operandInst);
      }
    }
    inst->eraseFromParent();
    ++summary.DeadFlagValuesRemoved;
  }
}

} // namespace

NativeRegisterPreSummaryPeepholeSummary
runNativeRegisterPreSummaryPeephole(llvm::Module &module) {
  NativeRegisterPreSummaryPeepholeSummary summary;

  // 死标志位物化先做：后面下标收窄的区间检查才看得到干净的值图。反复扫到不动为止——
  // 删掉一条链后，前面那条 flag store 可能也变成死的。
  for (unsigned iteration = 0; iteration < 4; ++iteration) {
    llvm::SmallVector<llvm::WeakVH, 64> flagStores;
    for (llvm::Function &function : module) {
      if (function.isDeclaration()) {
        continue;
      }
      for (llvm::Instruction &inst : llvm::instructions(function)) {
        auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
        if (store == nullptr) {
          continue;
        }
        auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
            store->getPointerOperand()->stripPointerCasts());
        if (isArithmeticFlagGlobal(global)) {
          flagStores.emplace_back(store);
        }
      }
    }
    if (iteration == 0) {
      summary.FlagStoresSeen = flagStores.size();
    }
    unsigned removedThisRound = 0;
    for (llvm::WeakVH &handle : flagStores) {
      auto *store = llvm::dyn_cast_or_null<llvm::StoreInst>(handle);
      if (store == nullptr || store->getParent() == nullptr) {
        continue;
      }
      auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
          store->getPointerOperand()->stripPointerCasts());
      if (global == nullptr || flagValueIsRead(*store, global)) {
        continue;
      }
      eraseDeadFlagValue(store, summary);
      ++summary.DeadFlagStoresRemoved;
      ++removedThisRound;
    }
    if (removedThisRound == 0) {
      break;
    }
  }
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

  // 按观察宽度收窄寄存器读（见 narrowObservedReads 的注释）：switch 下标、字节自增自减、寄存器搬移。
  // 放在 rewriteLoad 之后：整寄存器 load + trunc 这时已经变成 partial_read。
  llvm::SmallVector<llvm::WeakVH, 64> indexWrites;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr) {
        continue;
      }
      std::optional<NativeRegisterPartialWriteInfo> write =
          parseNativeRegisterPartialWrite(*call);
      if (write && write->BitOffset == 0 && write->WriteWidth >= 8 &&
          write->WriteWidth <= 32) {
        indexWrites.emplace_back(call);
      }
    }
  }
  for (llvm::WeakVH &handle : indexWrites) {
    auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(handle);
    if (call == nullptr || call->getParent() == nullptr) {
      continue;
    }
    ++summary.ByteIndexCandidates;
    if (!narrowObservedReads(*call, summary)) {
      ++summary.ByteIndexSkipped;
    }
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
     << " multi_use_skipped=" << summary.MultiUseLoadsSkipped
     << " byte_index_candidates=" << summary.ByteIndexCandidates
     << " byte_index_rewrites=" << summary.ByteIndexRewrites
     << " byte_index_skipped=" << summary.ByteIndexSkipped
     << " flag_stores=" << summary.FlagStoresSeen
     << " dead_flag_stores=" << summary.DeadFlagStoresRemoved
     << " dead_flag_values=" << summary.DeadFlagValuesRemoved << '\n';
}

} // namespace notdec::bin2llvm
