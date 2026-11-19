#include "notdec-bin2llvm/passes/register/NativeRegisterFinalCleanup.h"

#include "notdec-bin2llvm/intrinsics/NativeRegisterPartialRead.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterPartialWrite.h"
#include "notdec-bin2llvm/intrinsics/NativeRegisterValueRange.h"

#include "core/NotDecIntrinsics.h"

#include "RegisterEliminationInternal.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/DCE.h"
#include "llvm/Transforms/Utils/Local.h"

#include "llvm/IR/Metadata.h"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace notdec::bin2llvm {
namespace {

constexpr std::array<llvm::StringRef, 6> FunctionMetadataKeys = {
    "notdec.register.summary",
    "notdec.register.summary.read_entry",
    "notdec.register.summary.preserves",
    "notdec.register.summary.modifies",
    "notdec.register.summary.demanded_returns",
    "notdec.register.summary_ssa",
};

constexpr std::array<llvm::StringRef, 6> InstructionMetadataKeys = {
    "notdec.register.summary_ssa.call_value",
    "notdec.register.summary_ssa.entry",
    "notdec.register.summary_ssa.phi",
    "notdec.register.summary_ssa.range_entry",
    "notdec.register.summary_ssa.replaced",
    "notdec.register.summary_ssa.zero_demand_operand",
};

bool isRegisterGlobal(const llvm::GlobalVariable &global) {
  return global.getMetadata("notdec.register") != nullptr;
}

bool isRegisterGlobalPointer(const llvm::Value *value) {
  auto *global =
      llvm::dyn_cast<llvm::GlobalVariable>(value->stripPointerCasts());
  return global != nullptr && isRegisterGlobal(*global);
}

bool isRegisterLoadOrStore(const llvm::Instruction &inst) {
  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
    return isRegisterGlobalPointer(load->getPointerOperand());
  }
  if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
    return isRegisterGlobalPointer(store->getPointerOperand());
  }
  return false;
}

bool isRegisterHelperCall(const llvm::Instruction &inst) {
  auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
  if (call == nullptr) {
    return false;
  }
  llvm::Function *callee = call->getCalledFunction();
  return (callee != nullptr &&
          callee->getName().starts_with("notdec.register.")) ||
         parseNativeRegisterValueExtract(*call).has_value() ||
         parseNativeRegisterValueInsert(*call).has_value() ||
         parseNativeRegisterPartialRead(*call).has_value() ||
         parseNativeRegisterPartialWrite(*call).has_value();
}

bool isVarArgUnknownHelperName(llvm::StringRef name) {
  return name.starts_with("notdec.register.vararg_unknown.");
}

bool isOpaqueUnknownHelperName(llvm::StringRef name) {
  return name.starts_with("notdec.unknown.");
}

llvm::APInt partialWriteMask(unsigned fullWidth, unsigned writeWidth,
                             uint64_t bitOffset) {
  if (fullWidth == 0 || writeWidth == 0 || bitOffset >= fullWidth ||
      bitOffset + writeWidth > fullWidth) {
    return llvm::APInt(fullWidth, 0);
  }
  return llvm::APInt::getLowBitsSet(fullWidth, writeWidth).shl(bitOffset);
}

llvm::Value *lowerValueExtract(llvm::CallBase &call,
                               const NativeRegisterValueExtractInfo &info) {
  auto *sourceType =
      llvm::dyn_cast<llvm::IntegerType>(info.FullValue->getType());
  auto *resultType = llvm::dyn_cast<llvm::IntegerType>(call.getType());
  if (sourceType == nullptr || resultType == nullptr ||
      sourceType->getBitWidth() != info.FullWidth ||
      resultType->getBitWidth() != info.ReadWidth) {
    return nullptr;
  }
  if (info.BitOffset == 0 && info.ReadWidth == info.FullWidth) {
    return info.FullValue;
  }
  llvm::IRBuilder<> builder(&call);
  llvm::Value *bits = info.FullValue;
  if (info.BitOffset != 0) {
    bits = builder.CreateLShr(
        bits, llvm::ConstantInt::get(sourceType, info.BitOffset),
        "notdec.reg.extract.lower.shift");
  }
  if (bits->getType() != resultType) {
    bits = builder.CreateTrunc(bits, resultType, "notdec.reg.extract.lower");
  }
  return bits;
}

llvm::Value *lowerValueInsert(llvm::CallBase &call,
                              const NativeRegisterValueInsertInfo &info) {
  auto *baseType = llvm::dyn_cast<llvm::IntegerType>(info.Base->getType());
  auto *valueType = llvm::dyn_cast<llvm::IntegerType>(info.Value->getType());
  if (baseType == nullptr || valueType == nullptr ||
      call.getType() != baseType || baseType->getBitWidth() != info.FullWidth ||
      valueType->getBitWidth() != info.WriteWidth) {
    return nullptr;
  }
  if (info.BitOffset == 0 && info.WriteWidth == info.FullWidth &&
      info.Value->getType() == baseType) {
    return info.Value;
  }
  llvm::APInt writeMask =
      partialWriteMask(info.FullWidth, info.WriteWidth, info.BitOffset);
  if (writeMask.isZero()) {
    return nullptr;
  }
  llvm::IRBuilder<> builder(&call);
  llvm::Value *wide = builder.CreateZExtOrTrunc(info.Value, baseType,
                                                "notdec.reg.insert.lower.wide");
  if (info.BitOffset != 0) {
    wide = builder.CreateShl(wide,
                             llvm::ConstantInt::get(baseType, info.BitOffset),
                             "notdec.reg.insert.lower.shift");
  }
  wide = builder.CreateAnd(wide, llvm::ConstantInt::get(baseType, writeMask),
                           "notdec.reg.insert.lower.bits");
  llvm::Value *kept =
      builder.CreateAnd(info.Base, llvm::ConstantInt::get(baseType, ~writeMask),
                        "notdec.reg.insert.lower.keep");
  return builder.CreateOr(kept, wide, "notdec.reg.insert.lower");
}

uint64_t lowerValueRangeHelpers(llvm::Module &module) {
  std::vector<llvm::CallBase *> calls;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call != nullptr && (parseNativeRegisterValueExtract(*call) ||
                              parseNativeRegisterValueInsert(*call))) {
        calls.push_back(call);
      }
    }
  }

  uint64_t lowered = 0;
  for (llvm::CallBase *call : calls) {
    if (call->getParent() == nullptr) {
      continue;
    }
    llvm::Value *replacement = nullptr;
    if (std::optional<NativeRegisterValueExtractInfo> extract =
            parseNativeRegisterValueExtract(*call)) {
      replacement = lowerValueExtract(*call, *extract);
    } else if (std::optional<NativeRegisterValueInsertInfo> insert =
                   parseNativeRegisterValueInsert(*call)) {
      replacement = lowerValueInsert(*call, *insert);
    }
    if (replacement == nullptr) {
      continue;
    }
    call->replaceAllUsesWith(replacement);
    call->eraseFromParent();
    ++lowered;
  }
  return lowered;
}

uint64_t lowerVarArgUnknownHelpers(llvm::Module &module) {
  std::vector<llvm::CallBase *> calls;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      llvm::Function *callee =
          call == nullptr ? nullptr : call->getCalledFunction();
      if (callee != nullptr && isVarArgUnknownHelperName(callee->getName())) {
        calls.push_back(call);
      }
    }
  }

  uint64_t lowered = 0;
  for (llvm::CallBase *call : calls) {
    if (call->getParent() == nullptr || call->getType()->isVoidTy()) {
      continue;
    }
    call->replaceAllUsesWith(llvm::PoisonValue::get(call->getType()));
    call->eraseFromParent();
    ++lowered;
  }
  return lowered;
}

uint64_t eraseDeadRegisterReads(llvm::Module &module) {
  std::vector<llvm::Instruction *> deadReads;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      if (!inst.use_empty()) {
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        if (!load->isVolatile() &&
            isRegisterGlobalPointer(load->getPointerOperand())) {
          deadReads.push_back(load);
        }
        continue;
      }
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call != nullptr && parseNativeRegisterPartialRead(*call)) {
        deadReads.push_back(call);
      }
    }
  }

  uint64_t removed = 0;
  for (llvm::Instruction *inst : deadReads) {
    if (inst->getParent() == nullptr || !inst->use_empty()) {
      continue;
    }
    inst->eraseFromParent();
    ++removed;
  }
  return removed;
}

bool isDeadRemovableValueHelperName(llvm::StringRef name) {
  // 这些 helper 是纯值占位/值重组：没有副作用，也没人读时可以直接删。
  // 注意不要包含 notdec.register.userop.*（未建模指令可能对应 trap/异常）。
  return name.starts_with("notdec.register.summary_") ||
         name.starts_with("notdec.unknown.") ||
         name.starts_with("notdec.reg.extract.") ||
         name.starts_with("notdec.reg.insert.") ||
         name.starts_with("notdec.partial_read.") ||
         isVarArgUnknownHelperName(name);
}

// 删除没人用的 notdec 值 helper，并递归清掉因此变死的操作数链。
// 普通 call 有副作用语义，DCE/ADCE 不会删这些占位；但占位只在"值被读到"
// 时才有意义，残留的死占位会让 clobber/unknown 审计计数随创建顺序漂移。
uint64_t eraseDeadNotDecValueHelperCallsImpl(llvm::Module &module) {
  std::vector<llvm::CallBase *> worklist;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr || !call->use_empty()) {
        continue;
      }
      llvm::Function *callee = call->getCalledFunction();
      if (callee != nullptr &&
          isDeadRemovableValueHelperName(callee->getName())) {
        worklist.push_back(call);
      }
    }
  }

  uint64_t removed = 0;
  while (!worklist.empty()) {
    llvm::CallBase *call = worklist.back();
    worklist.pop_back();
    if (call->getParent() == nullptr || !call->use_empty()) {
      continue;
    }
    llvm::SmallVector<llvm::Value *, 4> operands;
    for (llvm::Use &operand : call->operands()) {
      if (llvm::isa<llvm::Instruction>(operand.get())) {
        operands.push_back(operand.get());
      }
    }
    call->eraseFromParent();
    ++removed;
    for (llvm::Value *operand : operands) {
      auto *operandCall = llvm::dyn_cast<llvm::CallBase>(operand);
      llvm::RecursivelyDeleteTriviallyDeadInstructions(operand);
      // RecursivelyDeleteTriviallyDeadInstructions does not remove calls (they
      // are not trivially dead without attributes), so enqueue a helper call
      // that just lost its last user.
      if (operandCall != nullptr && operandCall->getParent() != nullptr &&
          operandCall->use_empty()) {
        llvm::Function *callee = operandCall->getCalledFunction();
        if (callee != nullptr &&
            isDeadRemovableValueHelperName(callee->getName())) {
          worklist.push_back(operandCall);
        }
      }
    }
  }
  return removed;
}

uint64_t eraseUnusedRegisterGlobals(llvm::Module &module) {
  std::vector<llvm::GlobalVariable *> deadGlobals;
  for (llvm::GlobalVariable &global : module.globals()) {
    if (isRegisterGlobal(global) && global.use_empty()) {
      deadGlobals.push_back(&global);
    }
  }

  for (llvm::GlobalVariable *global : deadGlobals) {
    global->eraseFromParent();
  }
  return deadGlobals.size();
}

uint64_t eraseUnusedRegisterHelperDeclarationsImpl(llvm::Module &module) {
  std::vector<llvm::Function *> deadHelpers;
  for (llvm::Function &function : module) {
    if (!function.isDeclaration() || !function.use_empty()) {
      continue;
    }
    llvm::StringRef name = function.getName();
    if (name.starts_with("notdec.register.summary_") ||
        // 未建模 Sleigh userop 的专用占位函数（PcodeToLLVM lowerCallOther）：
        // 调用点可能随 GlobalDCE 一起没了，声明别再留在产物里。
        name.starts_with("notdec.register.userop.") ||
        isVarArgUnknownHelperName(name) ||
        isOpaqueUnknownHelperName(name) ||
        isNativeRegisterValueRangeName(name) ||
        isNativeRegisterPartialReadName(name) ||
        isNativeRegisterPartialWriteName(name)) {
      deadHelpers.push_back(&function);
    }
  }

  for (llvm::Function *function : deadHelpers) {
    function->eraseFromParent();
  }
  return deadHelpers.size();
}

bool functionHasRegisterResidue(const llvm::Function &function) {
  for (const llvm::Instruction &inst : llvm::instructions(function)) {
    if (isRegisterLoadOrStore(inst) || isRegisterHelperCall(inst)) {
      return true;
    }
  }
  return false;
}

uint64_t clearFunctionMetadata(llvm::Function &function) {
  uint64_t cleared = 0;
  for (llvm::StringRef key : FunctionMetadataKeys) {
    if (function.getMetadata(key) == nullptr) {
      continue;
    }
    function.setMetadata(key, nullptr);
    ++cleared;
  }
  return cleared;
}

uint64_t clearInstructionMetadata(llvm::Function &function) {
  uint64_t cleared = 0;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    for (llvm::StringRef key : InstructionMetadataKeys) {
      if (inst.getMetadata(key) == nullptr) {
        continue;
      }
      inst.setMetadata(key, nullptr);
      ++cleared;
    }
  }
  return cleared;
}

uint64_t countRemainingRegisterAccesses(llvm::Module &module) {
  uint64_t count = 0;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      if (isRegisterLoadOrStore(inst) || isRegisterHelperCall(inst)) {
        ++count;
      }
    }
  }
  return count;
}

void runGlobalDCE(llvm::Module &module) {
  llvm::LoopAnalysisManager loopAnalysis;
  llvm::FunctionAnalysisManager functionAnalysis;
  llvm::CGSCCAnalysisManager cgsccAnalysis;
  llvm::ModuleAnalysisManager moduleAnalysis;

  llvm::PassBuilder builder;
  builder.registerModuleAnalyses(moduleAnalysis);
  builder.registerCGSCCAnalyses(cgsccAnalysis);
  builder.registerFunctionAnalyses(functionAnalysis);
  builder.registerLoopAnalyses(loopAnalysis);
  builder.crossRegisterProxies(loopAnalysis, functionAnalysis, cgsccAnalysis,
                               moduleAnalysis);

  llvm::ModulePassManager passes;
  passes.addPass(llvm::GlobalDCEPass());
  passes.run(module, moduleAnalysis);
}

void runLocalDeadCodeCleanup(llvm::Module &module) {
  llvm::LoopAnalysisManager loopAnalysis;
  llvm::FunctionAnalysisManager functionAnalysis;
  llvm::CGSCCAnalysisManager cgsccAnalysis;
  llvm::ModuleAnalysisManager moduleAnalysis;

  llvm::PassBuilder builder;
  builder.registerModuleAnalyses(moduleAnalysis);
  builder.registerCGSCCAnalyses(cgsccAnalysis);
  builder.registerFunctionAnalyses(functionAnalysis);
  builder.registerLoopAnalyses(loopAnalysis);
  builder.crossRegisterProxies(loopAnalysis, functionAnalysis, cgsccAnalysis,
                               moduleAnalysis);

  llvm::FunctionPassManager passes;
  passes.addPass(llvm::DCEPass());
  passes.addPass(llvm::ADCEPass());
  for (llvm::Function &function : module) {
    if (!function.isDeclaration()) {
      passes.run(function, functionAnalysis);
    }
  }
}

} // namespace

// `insertvalue <struct> poison, %v, i` 用来构造"只填部分字段"的结构体：未填
// 字段就是未知值。最终 IR 里不能留 poison（poison 会让使用者成为 UB，也带不了
// 归因），所以给其余每个字段补一个带归因的 notdec.unknown，再把原 insertvalue
// 的基座接到补齐后的聚合上。只处理单层索引的 struct（多字段结构体的常见构造）。
uint64_t replacePoisonAggregateBases(llvm::Module &module) {
  uint64_t replaced = 0;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    std::vector<llvm::InsertValueInst *> candidates;
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      auto *insert = llvm::dyn_cast<llvm::InsertValueInst>(&inst);
      if (insert == nullptr || insert->getNumIndices() != 1) {
        continue;
      }
      auto *poison =
          llvm::dyn_cast<llvm::PoisonValue>(insert->getAggregateOperand());
      if (poison == nullptr ||
          llvm::dyn_cast<llvm::StructType>(poison->getType()) == nullptr) {
        continue;
      }
      candidates.push_back(insert);
    }
    for (llvm::InsertValueInst *insert : candidates) {
      auto *structType =
          llvm::cast<llvm::StructType>(insert->getAggregateOperand()->getType());
      unsigned setIndex = insert->getIndices()[0];
      llvm::IRBuilder<> builder(insert);
      llvm::Value *current = llvm::PoisonValue::get(structType);
      for (unsigned index = 0; index < structType->getNumElements(); ++index) {
        if (index == setIndex) {
          continue;
        }
        llvm::Value *field = unknownValueAt(
            builder, structType->getElementType(index), "unknown.field");
        attachUnknownValueMetadata(field, "poison_aggregate_field",
                                   function.getName(), "", "", "insertvalue");
        current = builder.CreateInsertValue(current, field, index);
      }
      insert->setOperand(0, current);
      ++replaced;
    }
  }
  return replaced;
}


// phi 的 poison 入边会把 poison 带到 ret/br/store 等观察点（phi 本身不会被
// InstCombine 折掉），在最终 IR 里换成带归因的 notdec.unknown：未定义变成
// "某个未知值"，调用点/返回点不再是 UB。占位插在该入边对应的前驱块终止符前。
uint64_t replacePoisonPhiIncomings(llvm::Module &module) {
  uint64_t replaced = 0;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &inst : block) {
        auto *phi = llvm::dyn_cast<llvm::PHINode>(&inst);
        if (phi == nullptr) {
          continue;
        }
        for (unsigned index = 0; index < phi->getNumIncomingValues(); ++index) {
          llvm::Value *incoming = phi->getIncomingValue(index);
          if (incoming == nullptr ||
              !llvm::isa<llvm::PoisonValue>(incoming)) {
            continue;
          }
          llvm::BasicBlock *predecessor = phi->getIncomingBlock(index);
          llvm::Instruction *insertBefore = predecessor->getTerminator();
          if (insertBefore == nullptr) {
            continue;
          }
          llvm::IRBuilder<> builder(insertBefore);
          llvm::Value *unknown =
              unknownValueAt(builder, incoming->getType(), "unknown.phi");
          attachUnknownValueMetadata(unknown, "poison_phi_incoming",
                                     function.getName(), "", phi->getName(),
                                     "phi");
          phi->setIncomingValue(index, unknown);
          ++replaced;
        }
      }
    }
  }
  return replaced;
}


// 有函数体的 callee 是我们自己 lift 出来的函数：它的形参/实参是真实观察者，
// poison 实参会让调用点变成 UB。把这类实参换成带归因的 notdec.unknown。
// 外部/间接 callee 不动：它们的变参槽 poison 是 fortune x86_64 零-unknown
// 门槛的既定表示（vararg_unknown 降级路径），不能一起改。
uint64_t replacePoisonCallArguments(llvm::Module &module) {
  uint64_t replaced = 0;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr || call->arg_size() == 0) {
        continue;
      }
      llvm::Function *callee = call->getCalledFunction();
      if (callee == nullptr || callee->isDeclaration()) {
        continue;
      }
      llvm::IRBuilder<> builder(call);
      for (unsigned index = 0; index < call->arg_size(); ++index) {
        llvm::Value *argument = call->getArgOperand(index);
        if (argument == nullptr ||
            !llvm::isa<llvm::PoisonValue>(argument)) {
          continue;
        }
        llvm::Value *unknown =
            unknownValueAt(builder, argument->getType(), "unknown.arg");
        attachUnknownValueMetadata(unknown, "poison_call_argument",
                                   function.getName(), callee->getName(), "",
                                   "call_arg");
        call->setArgOperand(index, unknown);
        ++replaced;
      }
    }
  }
  return replaced;
}


// 终结指令上的 poison 一律换成带归因的 notdec.unknown：
// - br/switch 条件是 poison：整个函数 UB；
// - ret 的值是 poison：调用点拿到的返回值 UB。
// 这两处都只是"值未知"，用不透明调用表示；值层面的其它 poison 保持不动，
// 仍作为 demand 分析缺口的信号。
//
NativePoisonTerminatorSummary
replacePoisonTerminatorValues(llvm::Module &module) {
  NativePoisonTerminatorSummary summary;
  uint64_t &replacedConditions = summary.ConditionsReplaced;
  uint64_t &replacedReturns = summary.ReturnsReplaced;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      llvm::Value *poison = nullptr;
      if (auto *branch = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
        if (!branch->isConditional()) {
          continue;
        }
        poison = branch->getCondition();
      } else if (auto *switchInst = llvm::dyn_cast<llvm::SwitchInst>(&inst)) {
        poison = switchInst->getCondition();
      } else if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
        poison = ret->getReturnValue();
      } else {
        continue;
      }
      if (poison == nullptr || !llvm::isa<llvm::PoisonValue>(poison)) {
        continue;
      }
      llvm::IRBuilder<> builder(&inst);
      llvm::Value *unknown =
          unknownValueAt(builder, poison->getType(), "poison.value");
      bool isReturn = llvm::isa<llvm::ReturnInst>(&inst);
      attachUnknownValueMetadata(unknown, "poison_terminator_value",
                                 function.getName(), "", "value",
                                 isReturn ? "return" : "branch");
      if (auto *branch = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
        branch->setCondition(unknown);
        ++replacedConditions;
      } else if (auto *switchInst = llvm::dyn_cast<llvm::SwitchInst>(&inst)) {
        switchInst->setCondition(unknown);
        ++replacedConditions;
      } else {
        llvm::cast<llvm::ReturnInst>(&inst)->setOperand(0, unknown);
        ++replacedReturns;
      }
    }
  }
  return summary;
}


// 同 eraseDeadUnknownValueCalls：tool 的最后一次 InstCombine+SimplifyCFG 可能
// 才把死块里的调用删掉，于是 helper 声明在那之后才变成没人用。导出同一个清理，
// 让 tool 在最后再补一次。
uint64_t eraseUnusedRegisterHelperDeclarations(llvm::Module &module) {
  return eraseUnusedRegisterHelperDeclarationsImpl(module);
}

// notdec.unknown.* 是占位调用：重写和 DCE 之后使用者可能已经没了，但调用指令
// 本身不会被当死代码删掉（普通 call 有副作用语义）。最终 IR 之前要再扫一次，
// 因为 tool 在 final cleanup 之后还会跑一次 InstCombine，它可能删掉最后一个
// 使用者。
uint64_t eraseDeadNotDecValueHelperCalls(llvm::Module &module) {
  return eraseDeadNotDecValueHelperCallsImpl(module);
}

uint64_t eraseDeadUnknownValueCalls(llvm::Module &module) {
  std::vector<llvm::CallBase *> deadCalls;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr || !call->use_empty()) {
        continue;
      }
      llvm::Function *callee = call->getCalledFunction();
      if (callee != nullptr && isOpaqueUnknownHelperName(callee->getName())) {
        deadCalls.push_back(call);
      }
    }
  }

  uint64_t removed = 0;
  for (llvm::CallBase *call : deadCalls) {
    if (call->getParent() == nullptr || !call->use_empty()) {
      continue;
    }
    call->eraseFromParent();
    ++removed;
  }
  return removed;
}

std::map<std::string, uint64_t>
countRemainingUnknownValues(llvm::Module &module) {
  std::map<std::string, uint64_t> counts;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr) {
        continue;
      }
      llvm::Function *callee = call->getCalledFunction();
      if (callee == nullptr || !isOpaqueUnknownHelperName(callee->getName())) {
        continue;
      }
      // ABI 未定义位（!notdec.abi_undefined）不是建模缺口：它们记录的是"二进制
      // 里这几位本来就没有约定值"，和"我们不知道值"分开统计（见
      // logs/20261119-02-abi-undefined-bits-plan.md）。审计里由
      // NativeRegisterValueSourceAudit::AbiUndefinedValues 单独计数。
      if (call->getMetadata("notdec.abi_undefined") != nullptr) {
        continue;
      }
      std::string reason = "unknown";
      std::string calleeName;
      std::string registerName;
      if (llvm::MDNode *source = call->getMetadata("notdec.unknown.source")) {
        for (const llvm::MDOperand &operand : source->operands()) {
          auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
          if (text == nullptr) {
            continue;
          }
          llvm::StringRef value = text->getString();
          if (value.consume_front("reason=")) {
            reason = value.str();
          } else if (value.consume_front("callee=")) {
            calleeName = value.str();
          } else if (value.consume_front("register=")) {
            registerName = value.str();
          }
        }
      }
      std::string key = reason;
      if (!calleeName.empty()) {
        key += ":" + calleeName;
      }
      if (!registerName.empty()) {
        key += ":" + registerName;
      }
      ++counts[key];
    }
  }
  return counts;
}

NativeRegisterFinalCleanupSummary runNativeRegisterFinalCleanup(
    llvm::Module &module, const NativeRegisterFinalCleanupOptions &options) {
  NativeRegisterFinalCleanupSummary summary;

  if (options.RunGlobalDCE) {
    runGlobalDCE(module);
  }
  summary.ValueRangeHelpersLowered += lowerValueRangeHelpers(module);
  summary.VarArgUnknownHelpersLowered += lowerVarArgUnknownHelpers(module);
  summary.PoisonCallArgumentsReplaced += replacePoisonCallArguments(module);
  summary.PoisonPhiIncomingsReplaced += replacePoisonPhiIncomings(module);
  summary.PoisonAggregateBasesReplaced += replacePoisonAggregateBases(module);
  NativePoisonTerminatorSummary poisonTerminators =
      replacePoisonTerminatorValues(module);
  summary.PoisonControlFlowConditionsReplaced +=
      poisonTerminators.ConditionsReplaced;
  summary.PoisonReturnValuesReplaced += poisonTerminators.ReturnsReplaced;
  runLocalDeadCodeCleanup(module);
  summary.DeadUnknownValuesRemoved += eraseDeadUnknownValueCalls(module);
  summary.DeadRegisterReadsRemoved += eraseDeadRegisterReads(module);
  runLocalDeadCodeCleanup(module);
  eraseDeadNotDecValueHelperCallsImpl(module);
  summary.RegisterGlobalsRemoved += eraseUnusedRegisterGlobals(module);
  summary.HelperDeclarationsRemoved +=
      eraseUnusedRegisterHelperDeclarationsImpl(module);
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    ++summary.FunctionsSeen;
    if (functionHasRegisterResidue(function)) {
      continue;
    }
    ++summary.FunctionsWithoutRegisterResidue;
    summary.FunctionMetadataCleared += clearFunctionMetadata(function);
    summary.InstructionMetadataCleared += clearInstructionMetadata(function);
  }
  if (options.RunGlobalDCE) {
    runGlobalDCE(module);
  }
  summary.RegisterGlobalsRemoved += eraseUnusedRegisterGlobals(module);
  summary.HelperDeclarationsRemoved +=
      eraseUnusedRegisterHelperDeclarationsImpl(module);
  summary.RemainingRegisterAccesses = countRemainingRegisterAccesses(module);

  if (options.PrintSummary) {
    printNativeRegisterFinalCleanupSummary(summary, llvm::errs());
  }
  return summary;
}

void printNativeRegisterFinalCleanupSummary(
    const NativeRegisterFinalCleanupSummary &summary, llvm::raw_ostream &os) {
  os << "Native register final cleanup: functions=" << summary.FunctionsSeen
     << " functions_without_register_residue="
     << summary.FunctionsWithoutRegisterResidue
     << " dead_register_reads_removed=" << summary.DeadRegisterReadsRemoved
     << " dead_unknown_values_removed=" << summary.DeadUnknownValuesRemoved
     << " register_globals_removed=" << summary.RegisterGlobalsRemoved
     << " helper_declarations_removed=" << summary.HelperDeclarationsRemoved
     << " value_range_helpers_lowered=" << summary.ValueRangeHelpersLowered
     << " vararg_unknown_helpers_lowered="
     << summary.VarArgUnknownHelpersLowered
     << " poison_control_flow_conditions_replaced="
     << summary.PoisonControlFlowConditionsReplaced
     << " poison_return_values_replaced="
     << summary.PoisonReturnValuesReplaced
     << " poison_call_arguments_replaced="
     << summary.PoisonCallArgumentsReplaced
     << " poison_phi_incomings_replaced="
     << summary.PoisonPhiIncomingsReplaced
     << " poison_aggregate_bases_replaced="
     << summary.PoisonAggregateBasesReplaced
     << " function_metadata_cleared=" << summary.FunctionMetadataCleared
     << " instruction_metadata_cleared=" << summary.InstructionMetadataCleared
     << " remaining_register_accesses=" << summary.RemainingRegisterAccesses
     << '\n';
}

} // namespace notdec::bin2llvm
