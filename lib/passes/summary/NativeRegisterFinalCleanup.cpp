#include "notdec-bin2llvm/passes/summary/NativeRegisterFinalCleanup.h"

#include "notdec-bin2llvm/NativeRegisterPartialRead.h"
#include "notdec-bin2llvm/NativeRegisterPartialWrite.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"

#include <array>

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

bool isRegisterGlobal(const llvm::GlobalVariable &global) {
  return global.getMetadata("notdec.register") != nullptr;
}

bool isRegisterGlobalPointer(const llvm::Value *value) {
  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(value->stripPointerCasts());
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
         parseNativeRegisterPartialRead(*call).has_value() ||
         parseNativeRegisterPartialWrite(*call).has_value();
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

} // namespace

NativeRegisterFinalCleanupSummary runNativeRegisterFinalCleanup(
    llvm::Module &module, const NativeRegisterFinalCleanupOptions &options) {
  NativeRegisterFinalCleanupSummary summary;

  if (options.RunGlobalDCE) {
    runGlobalDCE(module);
  }
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
  }
  if (options.RunGlobalDCE) {
    runGlobalDCE(module);
  }
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
     << " function_metadata_cleared=" << summary.FunctionMetadataCleared
     << " remaining_register_accesses=" << summary.RemainingRegisterAccesses
     << '\n';
}

} // namespace notdec::bin2llvm
