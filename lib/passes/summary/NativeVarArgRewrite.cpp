#include "notdec-bin2llvm/passes/summary/NativeVarArgRewrite.h"

#include "notdec-bin2llvm/NativeRegisterPartialRead.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace notdec::bin2llvm {
namespace {

// x86-64 SysV 的整数/向量参数寄存器顺序。lift 里向量寄存器建模成 ZMM（i512），
// 变参保存区里存的是低 128 位（XMM）。
constexpr std::array<llvm::StringRef, 6> GpArgOrder = {"RDI", "RSI", "RDX",
                                                       "RCX", "R8",  "R9"};
constexpr std::array<llvm::StringRef, 8> FloatArgOrder = {
    "ZMM0", "ZMM1", "ZMM2", "ZMM3", "ZMM4", "ZMM5", "ZMM6", "ZMM7"};

constexpr uint64_t GpSaveAreaSize = 48;
constexpr uint64_t FpSaveAreaSize = 128;
constexpr uint64_t VaListSize = 24;
constexpr uint64_t FpOffsetBase = 48;

constexpr llvm::StringRef VarArgMetadataName = "notdec.vararg";

const std::array<llvm::StringRef, 6> FlagRegisterNames = {"SF", "ZF", "PF",
                                                          "CF", "OF", "AF"};

int gpArgIndex(llvm::StringRef name) {
  for (size_t index = 0; index < GpArgOrder.size(); ++index) {
    if (GpArgOrder[index] == name) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

int floatArgIndex(llvm::StringRef name) {
  for (size_t index = 0; index < FloatArgOrder.size(); ++index) {
    if (FloatArgOrder[index] == name) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

bool isRegisterGlobal(const llvm::Value *value) {
  const auto *global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(
      value->stripPointerCasts());
  return global != nullptr && global->hasMetadata("notdec.register");
}

bool isFlagRegisterGlobal(const llvm::Value *value) {
  const auto *global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(
      value->stripPointerCasts());
  if (global == nullptr) {
    return false;
  }
  for (llvm::StringRef name : FlagRegisterNames) {
    if (global->getName() == name) {
      return true;
    }
  }
  return false;
}

// 把帧指针解析成 (alloca, 常量字节偏移)。只接受 alloca 本身和单下标 i8 GEP：
// 这正是 NativeStackFrameRewrite 之后帧访问的形态。
std::optional<std::pair<llvm::AllocaInst *, uint64_t>>
frameSlot(llvm::Value *pointer) {
  pointer = pointer->stripPointerCasts();
  if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(pointer)) {
    return std::make_pair(alloca, uint64_t{0});
  }
  auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(pointer);
  if (gep == nullptr || gep->getNumOperands() != 2) {
    return std::nullopt;
  }
  auto *base = llvm::dyn_cast<llvm::AllocaInst>(
      gep->getPointerOperand()->stripPointerCasts());
  auto *index = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1));
  if (base == nullptr || index == nullptr) {
    return std::nullopt;
  }
  return std::make_pair(base, index->getZExtValue());
}

// 值经过 bitcast/trunc 之后是不是"读某个寄存器 global"（load 或 partial_read）。
llvm::Value *stripValueCasts(llvm::Value *value) {
  while (true) {
    if (auto *cast = llvm::dyn_cast<llvm::CastInst>(value)) {
      value = cast->getOperand(0);
      continue;
    }
    return value;
  }
}

// AL 测试的根：partial_read.i64.i8(@RAX, 0)（编译器 test al,al 的 lift 形态）。
bool isAlTestRoot(const llvm::Instruction &inst) {
  const auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
  if (call == nullptr) {
    return false;
  }
  std::optional<NativeRegisterPartialReadInfo> read =
      parseNativeRegisterPartialRead(*call);
  return read.has_value() && read->Global != nullptr &&
         read->Global->getName() == "RAX" && read->ReadWidth == 8;
}

struct SaveStore {
  int Index = -1;
  llvm::AllocaInst *Frame = nullptr;
  uint64_t Offset = 0;
  llvm::Instruction *Store = nullptr;
  llvm::Instruction *Source = nullptr;
};

struct I32Store {
  llvm::AllocaInst *Frame = nullptr;
  uint64_t Offset = 0;
  uint64_t Value = 0;
  llvm::Instruction *Store = nullptr;
};

struct FunctionPlan {
  llvm::Function *Function = nullptr;
  NativeVarArgInfo Info;
  llvm::AllocaInst *Frame = nullptr;
  std::vector<SaveStore> GpSaves;
  std::vector<SaveStore> XmmSaves;
  std::vector<llvm::Instruction *> VaListFieldStores;
  llvm::BasicBlock *VaListInitBlock = nullptr;
  std::vector<llvm::Instruction *> AlTestInstructions;
};

std::optional<FunctionPlan> analyzeFunction(llvm::Function &function) {
  FunctionPlan plan;
  plan.Function = &function;

  // 1) GP 保存区：load @REG + store 到帧槽，值只能有这一个使用者。
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    if (store == nullptr ||
        !store->getValueOperand()->getType()->isIntegerTy(64)) {
      continue;
    }
    auto *load = llvm::dyn_cast<llvm::LoadInst>(
        stripValueCasts(store->getValueOperand()));
    if (load == nullptr || !load->hasOneUse() ||
        !isRegisterGlobal(load->getPointerOperand())) {
      continue;
    }
    const auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
        load->getPointerOperand()->stripPointerCasts());
    int index = gpArgIndex(global->getName());
    if (index < 0) {
      continue;
    }
    std::optional<std::pair<llvm::AllocaInst *, uint64_t>> slot =
        frameSlot(store->getPointerOperand());
    if (!slot) {
      continue;
    }
    plan.GpSaves.push_back(
        SaveStore{index, slot->first, slot->second, store, load});
  }
  {
    std::set<int> savedRegisters;
    for (const SaveStore &save : plan.GpSaves) {
      savedRegisters.insert(save.Index);
    }
    if (savedRegisters.size() < 2) {
      return std::nullopt;
    }
    // 保存槽必须落在同一个基址上：offset = base + 8*regIndex。
    int64_t base = -1;
    for (const SaveStore &save : plan.GpSaves) {
      int64_t candidate =
          static_cast<int64_t>(save.Offset) - 8 * save.Index;
      if (base < 0) {
        base = candidate;
      } else if (base != candidate) {
        return std::nullopt;
      }
    }
    if (base < 0) {
      return std::nullopt;
    }
    plan.Info.SaveAreaOffset = static_cast<uint64_t>(base);
    plan.Frame = plan.GpSaves.front().Frame;
    std::string saved;
    for (const SaveStore &save : plan.GpSaves) {
      if (!saved.empty()) {
        saved += ",";
      }
      saved += GpArgOrder[save.Index].str();
    }
    plan.Info.SavedRegisters = saved;
  }

  // 2) va_list 初始化：同一帧上相邻 4 字节的 i32 对（gp_offset=8k, fp_offset=48）。
  std::vector<I32Store> i32Stores;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    if (store == nullptr || !store->getValueOperand()->getType()->isIntegerTy(32)) {
      continue;
    }
    auto *constant =
        llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
    if (constant == nullptr) {
      continue;
    }
    std::optional<std::pair<llvm::AllocaInst *, uint64_t>> slot =
        frameSlot(store->getPointerOperand());
    if (!slot || slot->first != plan.Frame) {
      continue;
    }
    i32Stores.push_back(I32Store{slot->first, slot->second,
                                 constant->getZExtValue(), store});
  }
  std::optional<uint64_t> fixedGp;
  uint64_t vaListOffset = 0;
  llvm::BasicBlock *vaListInitBlock = nullptr;
  for (const I32Store &fp : i32Stores) {
    if (fp.Value != FpOffsetBase) {
      continue;
    }
    for (const I32Store &gp : i32Stores) {
      if (gp.Offset + 4 != fp.Offset || gp.Value % 8 != 0 ||
          gp.Value > GpSaveAreaSize) {
        continue;
      }
      uint64_t candidate = gp.Value / 8;
      if (fixedGp.has_value() && *fixedGp != candidate) {
        return std::nullopt;  // 多个互相矛盾的 va_list，保守跳过
      }
      fixedGp = candidate;
      vaListOffset = gp.Offset;
      vaListInitBlock = gp.Store->getParent();
    }
  }
  if (!fixedGp.has_value()) {
    return std::nullopt;
  }
  plan.Info.FixedGp = *fixedGp;
  plan.Info.VaListOffset = vaListOffset;
  // 保存的寄存器必须都是变参寄存器（固定参数的槽不该出现在保存区里；
  // -O0 那种保存全部 6 个的形态本轮保守跳过）。
  for (const SaveStore &save : plan.GpSaves) {
    if (static_cast<uint64_t>(save.Index) < plan.Info.FixedGp) {
      return std::nullopt;
    }
  }

  // 3) AL 测试（编译器 test al,al，决定是否保存 XMM）：从 partial_read.i64.i8(@RAX)
  //    出发算传递闭包。闭包内的使用者只能是标志寄存器 store、进一步的标志运算，
  //    或依赖它的分支；出现别的使用者就放弃改写（保守）。
  std::vector<llvm::Instruction *> alRoots;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    if (isAlTestRoot(inst)) {
      alRoots.push_back(&inst);
    }
  }
  if (alRoots.empty()) {
    return std::nullopt;
  }
  std::set<llvm::Instruction *> alTest;
  llvm::SmallVector<llvm::Instruction *, 16> worklist(alRoots.begin(),
                                                     alRoots.end());
  while (!worklist.empty()) {
    llvm::Instruction *current = worklist.pop_back_val();
    if (!alTest.insert(current).second) {
      continue;
    }
    for (llvm::User *user : current->users()) {
      auto *userInst = llvm::dyn_cast<llvm::Instruction>(user);
      if (userInst == nullptr) {
        return std::nullopt;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(userInst)) {
        if (!isFlagRegisterGlobal(store->getPointerOperand())) {
          return std::nullopt;
        }
        continue;
      }
      if (llvm::isa<llvm::BinaryOperator>(userInst) ||
          llvm::isa<llvm::ICmpInst>(userInst) ||
          llvm::isa<llvm::CastInst>(userInst)) {
        worklist.push_back(userInst);
        continue;
      }
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(userInst)) {
        // 标志位计算里还会用 llvm.ctpop（PF）这类纯 intrinsic：它们也是 AL 测试
        // 链的一部分，跟着一起当死代码清理。
        llvm::Function *callee = call->getCalledFunction();
        if (callee != nullptr && callee->isIntrinsic() &&
            !call->mayHaveSideEffects()) {
          worklist.push_back(userInst);
          continue;
        }
      }
      if (llvm::isa<llvm::BranchInst>(userInst)) {
        continue;
      }
      return std::nullopt;
    }
  }
  plan.AlTestInstructions.assign(alTest.begin(), alTest.end());

  // 4) FP 保存区：区间内每个 store 都必须是可删的 XMM 保存（否则放弃改写，
  //    避免留下读 ZMM 的入口值）。
  uint64_t fpBase = plan.Info.SaveAreaOffset + GpSaveAreaSize;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    if (store == nullptr) {
      continue;
    }
    std::optional<std::pair<llvm::AllocaInst *, uint64_t>> slot =
        frameSlot(store->getPointerOperand());
    if (!slot || slot->first != plan.Frame || slot->second < fpBase ||
        slot->second >= fpBase + FpSaveAreaSize) {
      continue;
    }
    auto *read = llvm::dyn_cast<llvm::CallBase>(
        stripValueCasts(store->getValueOperand()));
    std::optional<NativeRegisterPartialReadInfo> partial =
        read == nullptr ? std::nullopt
                        : parseNativeRegisterPartialRead(*read);
    if (!partial || partial->Global == nullptr || partial->ReadWidth != 128) {
      return std::nullopt;
    }
    int index = floatArgIndex(partial->Global->getName());
    if (index < 0 || !store->getValueOperand()->hasOneUse()) {
      return std::nullopt;
    }
    if (slot->second != fpBase + 16 * static_cast<uint64_t>(index)) {
      return std::nullopt;
    }
    plan.XmmSaves.push_back(
        SaveStore{index, slot->first, slot->second, store, read});
  }

  // 5) va_list 的 4 个字段 store（指针字段允许缺失，值可能经过寄存器中转）。
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
    if (store == nullptr) {
      continue;
    }
    std::optional<std::pair<llvm::AllocaInst *, uint64_t>> slot =
        frameSlot(store->getPointerOperand());
    if (!slot || slot->first != plan.Frame) {
      continue;
    }
    uint64_t offset = slot->second;
    if (offset < vaListOffset || offset >= vaListOffset + VaListSize) {
      continue;
    }
    // 只认和 gp_offset/fp_offset 同一个块里的 store：那一段 24 字节槽就是
    // va_list 本身；别的块写同一段槽是栈槽复用出去的其它变量，跳过。
    if (store->getParent() != vaListInitBlock) {
      continue;
    }
    plan.VaListFieldStores.push_back(store);
  }
  if (plan.VaListFieldStores.size() < 2) {
    return std::nullopt;
  }

  return plan;
}

void eraseIfDead(llvm::Instruction *inst) {
  if (inst != nullptr && inst->getParent() != nullptr && inst->use_empty()) {
    inst->eraseFromParent();
  }
}

void applyPlan(FunctionPlan &plan, NativeVarArgRewriteSummary &summary) {
  llvm::Function &function = *plan.Function;

  // 1) 保存区 store（GP/XMM）及其只喂给它的读。
  for (const SaveStore &save : plan.GpSaves) {
    save.Store->eraseFromParent();
    ++summary.RegisterSavesRemoved;
  }
  for (const SaveStore &save : plan.GpSaves) {
    eraseIfDead(save.Source);
  }
  for (const SaveStore &save : plan.XmmSaves) {
    save.Store->eraseFromParent();
    ++summary.RegisterSavesRemoved;
  }
  for (const SaveStore &save : plan.XmmSaves) {
    eraseIfDead(save.Source);
  }

  // 2) 空块折叠：删掉 XMM 保存之后，AL 守卫的那条分支一侧变空。把
  //    "br i1 %c, %empty, %other"（%empty 只有一条到 %other 的跳转）折成直接
  //    跳到 %other 并删掉空块。空块折叠本身语义等价，标志/条件的死代码由后面
  //    的 InstCombine + 死 store 清理负责。
  {
    std::vector<llvm::BranchInst *> branches;
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      if (auto *branch = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
        if (branch->isConditional()) {
          branches.push_back(branch);
        }
      }
    }
    for (llvm::BranchInst *branch : branches) {
      if (branch->getParent() == nullptr || !branch->isConditional()) {
        continue;
      }
      for (unsigned index = 0; index < 2; ++index) {
        llvm::BasicBlock *side = branch->getSuccessor(index);
        llvm::BasicBlock *other = branch->getSuccessor(1 - index);
        if (side == other || side->getSinglePredecessor() != branch->getParent()) {
          continue;
        }
        if (!side->getFirstNonPHIOrDbgOrAlloca()->isTerminator()) {
          continue;
        }
        auto *sideBranch = llvm::dyn_cast<llvm::BranchInst>(side->getTerminator());
        if (sideBranch == nullptr || sideBranch->isConditional() ||
            sideBranch->getSuccessor(0) == side) {
          continue;
        }
        llvm::IRBuilder<> builder(branch);
        builder.CreateBr(sideBranch->getSuccessor(0));
        branch->eraseFromParent();
        side->eraseFromParent();
        break;
      }
    }
  }

  // 3) 手写 va_list 初始化 → llvm.va_start。
  llvm::Value *vaListPointer = nullptr;
  for (llvm::Instruction *store : plan.VaListFieldStores) {
    if (auto *storeInst = llvm::dyn_cast<llvm::StoreInst>(store)) {
      std::optional<std::pair<llvm::AllocaInst *, uint64_t>> slot =
          frameSlot(storeInst->getPointerOperand());
      if (slot && slot->first == plan.Frame &&
          slot->second == plan.Info.VaListOffset) {
        vaListPointer = storeInst->getPointerOperand();
        break;
      }
    }
  }
  llvm::Instruction *insertBefore = plan.VaListFieldStores.front();
  if (vaListPointer == nullptr) {
    llvm::IRBuilder<> entryBuilder(
        &*function.getEntryBlock().getFirstInsertionPt());
    vaListPointer = entryBuilder.CreateGEP(
        llvm::Type::getInt8Ty(function.getContext()), plan.Frame,
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(function.getContext()),
                              plan.Info.VaListOffset),
        "notdec.vararg.ap");
  }
  {
    llvm::IRBuilder<> builder(insertBefore);
    // llvm.va_start 是 llvm_anyptr_ty 重载 intrinsic：必须给出指针类型，
    // 否则 getOrInsertDeclaration 解不出重载会崩。
    llvm::Function *vaStart = llvm::Intrinsic::getOrInsertDeclaration(
        function.getParent(), llvm::Intrinsic::vastart,
        {llvm::PointerType::get(function.getContext(), 0)});
    builder.CreateCall(vaStart, {vaListPointer});
    ++summary.VaStartCallsInserted;
  }
  for (llvm::Instruction *store : plan.VaListFieldStores) {
    if (store->getParent() != nullptr) {
      store->eraseFromParent();
      ++summary.VaListFieldStoresRemoved;
    }
  }

  // 4) AL 测试链的清理由后续管线负责：标志寄存器 store 会被 SSA 阶段的死 store
  //    清理删掉，随后 partial_read(.i8 @RAX) 变成没人用的死读，被
  //    removeDeadPartialReads / eraseDeadRegisterReads 收掉。这里不再自己扫，
  //    避免和"空块折叠已经删掉整块指令"相互踩到悬空指针。

  // 5) metadata：签名推断（shapeForInternalFunction）只认这个标记。
  {
    llvm::LLVMContext &context = function.getContext();
    std::vector<llvm::Metadata *> fields{
        llvm::MDString::get(
            context, "fixed_gp=" + std::to_string(plan.Info.FixedGp)),
        llvm::MDString::get(
            context, "fixed_fp=" + std::to_string(plan.Info.FixedFp)),
        llvm::MDString::get(
            context,
            "save_area=" + std::to_string(plan.Info.SaveAreaOffset)),
        llvm::MDString::get(
            context, "va_list=" + std::to_string(plan.Info.VaListOffset)),
        llvm::MDString::get(context, "saved=" + plan.Info.SavedRegisters),
        llvm::MDString::get(context, "source=x86_64_sysv_pattern"),
    };
    function.setMetadata(VarArgMetadataName, llvm::MDNode::get(context, fields));
  }
  ++summary.FunctionsRewritten;
  summary.RewrittenFunctions.push_back(function.getName().str());
}

} // namespace

bool isNativeVarArgFunction(const llvm::Function &function) {
  return function.getMetadata(VarArgMetadataName) != nullptr;
}

std::optional<NativeVarArgInfo> nativeVarArgInfo(const llvm::Function &function) {
  llvm::MDNode *node = function.getMetadata(VarArgMetadataName);
  if (node == nullptr) {
    return std::nullopt;
  }
  NativeVarArgInfo info;
  for (const llvm::MDOperand &operand : node->operands()) {
    auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
    if (text == nullptr) {
      continue;
    }
    llvm::StringRef value = text->getString();
    if (value.consume_front("fixed_gp=")) {
      info.FixedGp = std::stoull(value.str());
    } else if (value.consume_front("fixed_fp=")) {
      info.FixedFp = std::stoull(value.str());
    } else if (value.consume_front("save_area=")) {
      info.SaveAreaOffset = std::stoull(value.str());
    } else if (value.consume_front("va_list=")) {
      info.VaListOffset = std::stoull(value.str());
    } else if (value.consume_front("saved=")) {
      info.SavedRegisters = value.str();
    }
  }
  return info;
}

NativeVarArgRewriteSummary
runNativeVarArgRewrite(llvm::Module &module,
                       const NativeVarArgRewriteOptions &options) {
  NativeVarArgRewriteSummary summary;
  for (llvm::Function &function : module) {
    if (function.isDeclaration() ||
        function.getName().starts_with("notdec.") ||
        function.getName().starts_with("llvm.")) {
      continue;
    }
    ++summary.FunctionsSeen;
    std::optional<FunctionPlan> plan = analyzeFunction(function);
    if (!plan.has_value()) {
      continue;
    }
    ++summary.FunctionsDetected;
    summary.DetectedFunctions.push_back(function.getName().str());
    if (!options.RewriteVarArgs) {
      if (options.AttachMetadata) {
        llvm::LLVMContext &context = function.getContext();
        std::vector<llvm::Metadata *> fields{
            llvm::MDString::get(
                context, "fixed_gp=" + std::to_string(plan->Info.FixedGp)),
            llvm::MDString::get(
                context, "fixed_fp=" + std::to_string(plan->Info.FixedFp)),
            llvm::MDString::get(
                context,
                "save_area=" + std::to_string(plan->Info.SaveAreaOffset)),
            llvm::MDString::get(
                context, "va_list=" + std::to_string(plan->Info.VaListOffset)),
            llvm::MDString::get(context, "saved=" + plan->Info.SavedRegisters),
            llvm::MDString::get(context, "source=x86_64_sysv_pattern"),
        };
        function.setMetadata(VarArgMetadataName,
                             llvm::MDNode::get(context, fields));
      }
      continue;
    }
    applyPlan(*plan, summary);
  }
  return summary;
}

void printNativeVarArgRewriteSummary(const NativeVarArgRewriteSummary &summary,
                                     llvm::raw_ostream &os) {
  os << "Native vararg rewrite: functions=" << summary.FunctionsSeen
     << " detected=" << summary.FunctionsDetected
     << " rewritten=" << summary.FunctionsRewritten
     << " register_saves_removed=" << summary.RegisterSavesRemoved
     << " va_list_fields_removed=" << summary.VaListFieldStoresRemoved
     << " va_start_inserted=" << summary.VaStartCallsInserted;
  if (!summary.DetectedFunctions.empty()) {
    os << " [";
    for (size_t index = 0; index < summary.DetectedFunctions.size(); ++index) {
      if (index != 0) {
        os << ",";
      }
      os << summary.DetectedFunctions[index];
    }
    os << "]";
  }
  os << "\n";
}

} // namespace notdec::bin2llvm
