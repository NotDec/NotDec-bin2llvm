#include "notdec-bin2llvm/passes/summary/NativeRegisterFinalCleanup.h"

#include "notdec-bin2llvm/NativeRegisterPartialRead.h"
#include "notdec-bin2llvm/NativeRegisterPartialWrite.h"
#include "notdec-bin2llvm/NativeRegisterValueRange.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringRef.h"
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

#include <array>
#include <optional>
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

bool rangesOverlap(uint64_t lhsOffset, uint64_t lhsWidth, uint64_t rhsOffset,
                   uint64_t rhsWidth) {
  uint64_t lhsEnd = lhsOffset + lhsWidth;
  uint64_t rhsEnd = rhsOffset + rhsWidth;
  return lhsOffset < rhsEnd && rhsOffset < lhsEnd;
}

std::optional<uint64_t> constantShiftAmount(llvm::Value *value) {
  auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value);
  if (constant == nullptr || constant->getValue().getActiveBits() > 64) {
    return std::nullopt;
  }
  return constant->getZExtValue();
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

uint64_t simplifyInsertedValueExtracts(llvm::Module &module) {
  std::vector<llvm::TruncInst *> truncs;
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

  uint64_t simplified = 0;
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
    ++simplified;
  }
  return simplified;
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

uint64_t eraseUnusedRegisterHelperDeclarations(llvm::Module &module) {
  std::vector<llvm::Function *> deadHelpers;
  for (llvm::Function &function : module) {
    if (!function.isDeclaration() || !function.use_empty()) {
      continue;
    }
    llvm::StringRef name = function.getName();
    if (name.starts_with("notdec.register.summary_") ||
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

NativeRegisterFinalCleanupSummary runNativeRegisterFinalCleanup(
    llvm::Module &module, const NativeRegisterFinalCleanupOptions &options) {
  NativeRegisterFinalCleanupSummary summary;

  if (options.RunGlobalDCE) {
    runGlobalDCE(module);
  }
  summary.ValueRangeExtractsSimplified += simplifyInsertedValueExtracts(module);
  summary.ValueRangeHelpersLowered += lowerValueRangeHelpers(module);
  runLocalDeadCodeCleanup(module);
  summary.DeadRegisterReadsRemoved += eraseDeadRegisterReads(module);
  runLocalDeadCodeCleanup(module);
  summary.RegisterGlobalsRemoved += eraseUnusedRegisterGlobals(module);
  summary.HelperDeclarationsRemoved +=
      eraseUnusedRegisterHelperDeclarations(module);
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
      eraseUnusedRegisterHelperDeclarations(module);
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
     << " register_globals_removed=" << summary.RegisterGlobalsRemoved
     << " helper_declarations_removed=" << summary.HelperDeclarationsRemoved
     << " value_range_extracts_simplified="
     << summary.ValueRangeExtractsSimplified
     << " value_range_helpers_lowered=" << summary.ValueRangeHelpersLowered
     << " function_metadata_cleared=" << summary.FunctionMetadataCleared
     << " instruction_metadata_cleared=" << summary.InstructionMetadataCleared
     << " remaining_register_accesses=" << summary.RemainingRegisterAccesses
     << '\n';
}

} // namespace notdec::bin2llvm
