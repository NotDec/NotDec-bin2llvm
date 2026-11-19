#include "notdec-bin2llvm/passes/register/NativeRegisterValueDce.h"

#include "notdec-bin2llvm/intrinsics/NativeRegisterPartialWrite.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterValueRange.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <vector>

namespace notdec::bin2llvm {

namespace {

// 这个指令只产生值、没有可观察副作用，值没人用就可以删。
// 保守：store / 真实调用 / 终结指令 / alloca / 其它 notdec.register.* 都不删。
bool isRemovableValueNode(llvm::Instruction &inst) {
  if (inst.isTerminator() || llvm::isa<llvm::StoreInst>(inst) ||
      llvm::isa<llvm::AllocaInst>(inst) || llvm::isa<llvm::FenceInst>(inst) ||
      llvm::isa<llvm::AtomicRMWInst>(inst) ||
      llvm::isa<llvm::AtomicCmpXchgInst>(inst)) {
    return false;
  }
  if (llvm::isa<llvm::PHINode>(inst) || llvm::isa<llvm::SelectInst>(inst) ||
      llvm::isa<llvm::ICmpInst>(inst) || llvm::isa<llvm::FCmpInst>(inst) ||
      llvm::isa<llvm::CastInst>(inst) || llvm::isa<llvm::BinaryOperator>(inst) ||
      llvm::isa<llvm::GetElementPtrInst>(inst) ||
      llvm::isa<llvm::ExtractValueInst>(inst) ||
      llvm::isa<llvm::InsertValueInst>(inst) ||
      llvm::isa<llvm::ExtractElementInst>(inst) ||
      llvm::isa<llvm::InsertElementInst>(inst) ||
      llvm::isa<llvm::ShuffleVectorInst>(inst)) {
    return true;
  }
  auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
  if (call == nullptr) {
    return false;
  }
  llvm::Function *callee = call->getCalledFunction();
  if (callee == nullptr) {
    return false;
  }
  if (callee->isIntrinsic()) {
    // 只删"算值"的 intrinsic；assume / lifetime / vaarg 等有语义的保留。
    llvm::StringRef name = callee->getName();
    return name.starts_with("llvm.ctpop") ||
           name.starts_with("llvm.ctlz") || name.starts_with("llvm.cttz") ||
           name.starts_with("llvm.bswap") || name.starts_with("llvm.bitreverse") ||
           name.starts_with("llvm.abs.") || name.starts_with("llvm.smax") ||
           name.starts_with("llvm.smin") || name.starts_with("llvm.umax") ||
           name.starts_with("llvm.umin") || name.starts_with("llvm.fshl") ||
           name.starts_with("llvm.fshr") ||
           name.contains(".with.overflow.") ||
           name.starts_with("llvm.smul.fix") ||
           name.starts_with("llvm.umul.fix") ||
           name.starts_with("llvm.expect");
  }
  llvm::StringRef name = callee->getName();
  // 寄存器模型里的纯值 helper：读寄存器 global / 位段抽取插入 / unknown 占位。
  // partial_write 不在此列（写有效果，值换成 0 后保留写本身）。
  return name.starts_with("notdec.partial_read.") ||
         name.starts_with("notdec.unknown.") ||
         isNativeRegisterValueRangeName(name);
}

void enqueueOperandValues(llvm::Instruction &inst,
                          std::vector<llvm::Instruction *> &worklist) {
  for (llvm::Value *operand : inst.operand_values()) {
    if (auto *operandInst = llvm::dyn_cast<llvm::Instruction>(operand)) {
      worklist.push_back(operandInst);
    }
  }
}

} // namespace

NativeRegisterValueDceSummary
pruneDeadRegisterValues(llvm::Module &module,
                        const std::vector<llvm::Instruction *> &deadWrites) {
  NativeRegisterValueDceSummary summary;
  summary.DeadWrites = deadWrites.size();
  std::vector<llvm::Instruction *> worklist;
  worklist.reserve(deadWrites.size());

  for (llvm::Instruction *inst : deadWrites) {
    if (inst == nullptr || inst->getParent() == nullptr) {
      continue;
    }
    llvm::Value *value = nullptr;
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
      if (store->isVolatile()) {
        continue;
      }
      value = store->getValueOperand();
    } else if (auto *call = llvm::dyn_cast<llvm::CallBase>(inst)) {
      std::optional<NativeRegisterPartialWriteInfo> write =
          parseNativeRegisterPartialWrite(*call);
      if (!write) {
        continue;
      }
      value = write->Value;
    }
    if (value == nullptr) {
      continue;
    }
    auto *integer = llvm::dyn_cast<llvm::IntegerType>(value->getType());
    if (integer == nullptr) {
      // 浮点/向量寄存器写的死值先不动：0.0 不是位模式 0，换值可能改变
      // NaN 相关的观察；死值本来没人读，但保守起见留给后续。
      continue;
    }
    llvm::Constant *zero = llvm::ConstantInt::get(integer, 0);
    if (value == zero) {
      continue;
    }
    // 收集旧值的操作数：换值之后它们可能没人用了。
    if (auto *valueInst = llvm::dyn_cast<llvm::Instruction>(value)) {
      worklist.push_back(valueInst);
    }
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
      store->setOperand(0, zero);
    } else if (auto *call = llvm::dyn_cast<llvm::CallBase>(inst)) {
      // partial_write(ptr, value, offset)：value 是第 1 个实参。
      call->setArgOperand(1, zero);
    }
    ++summary.ValuesCleared;
  }

  // 递归删除没人用的纯值链。用 push/pop 的 worklist，重复项靠 use_empty 过滤。
  while (!worklist.empty()) {
    llvm::Instruction *inst = worklist.back();
    worklist.pop_back();
    if (inst == nullptr || inst->getParent() == nullptr ||
        !inst->use_empty() || !isRemovableValueNode(*inst)) {
      continue;
    }
    const bool registerRead = [&]() {
      auto *call = llvm::dyn_cast<llvm::CallBase>(inst);
      if (call == nullptr) {
        return false;
      }
      llvm::Function *callee = call->getCalledFunction();
      return callee != nullptr &&
             callee->getName().starts_with("notdec.partial_read.");
    }();
    enqueueOperandValues(*inst, worklist);
    inst->eraseFromParent();
    ++summary.InstructionsRemoved;
    if (registerRead) {
      ++summary.RegisterReadsRemoved;
    }
  }
  (void)module;
  return summary;
}

void printNativeRegisterValueDceSummary(
    const NativeRegisterValueDceSummary &summary, llvm::raw_ostream &os) {
  os << "Native register value dce: dead_writes=" << summary.DeadWrites
     << " values_cleared=" << summary.ValuesCleared
     << " instructions_removed=" << summary.InstructionsRemoved
     << " register_reads_removed=" << summary.RegisterReadsRemoved << '\n';
}

} // namespace notdec::bin2llvm
