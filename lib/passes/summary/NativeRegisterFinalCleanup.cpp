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

} // namespace

NativeRegisterFinalCleanupSummary runNativeRegisterFinalCleanup(
    llvm::Module &module, const NativeRegisterFinalCleanupOptions &options) {
  NativeRegisterFinalCleanupSummary summary;

  if (options.RunGlobalDCE) {
    runGlobalDCE(module);
  }
  summary.DeadRegisterReadsRemoved += eraseDeadRegisterReads(module);
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
     << " dead_register_reads_removed="
     << summary.DeadRegisterReadsRemoved
     << " register_globals_removed=" << summary.RegisterGlobalsRemoved
     << " helper_declarations_removed=" << summary.HelperDeclarationsRemoved
     << " function_metadata_cleared=" << summary.FunctionMetadataCleared
     << " instruction_metadata_cleared=" << summary.InstructionMetadataCleared
     << " remaining_register_accesses=" << summary.RemainingRegisterAccesses
     << '\n';
}

} // namespace notdec::bin2llvm
