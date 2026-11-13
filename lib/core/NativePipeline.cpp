#include "notdec-bin2llvm/core/NativePipeline.h"

#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"

#include <iostream>
#include <vector>

namespace notdec::bin2llvm {

NativeRegisterEliminationStageResult
runNativeRegisterEliminationStage(llvm::Module &module,
                                  const NativePipelineOptions &options) {
  NativeRegisterEliminationStageResult result;
  if (options.DisableRegisterElimination) {
    return result;
  }

  NativeRegisterEliminationOptions passOptions;
  passOptions.EnableRewrite = true;
  passOptions.EnableResidueRemoval = !options.DisableResidueRemoval;
  passOptions.EnablePostRewriteInstCombine = !options.DisableInstCombine;
  passOptions.EnableVarArgRecovery = !options.DisableVarArgRecovery;
  passOptions.AbiForAllFunctions = options.AbiForAllFunctions;
  passOptions.PrintSummary = options.PrintRegisterSummary;
  passOptions.ExternalPrototypeJsonPath = options.ExternalPrototypeJsonPath;
  result.Summary = runNativeRegisterElimination(module, passOptions);
  if (llvm::verifyModule(module, &llvm::errs())) {
    std::cerr << "module verification failed after summary register SSA pass\n";
    result.Ok = false;
  }
  return result;
}

bool runNativeInstCombineStage(llvm::Module &module,
                               const NativePipelineOptions &options,
                               bool runEarlyCSEFirst) {
  if (options.DisableInstCombine) {
    return true;
  }

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

  llvm::FunctionPassManager functionPasses;
  if (runEarlyCSEFirst) {
    functionPasses.addPass(llvm::EarlyCSEPass());
  }
  functionPasses.addPass(llvm::InstCombinePass());
  functionPasses.addPass(llvm::SimplifyCFGPass());
  for (llvm::Function &function : module) {
    if (!function.isDeclaration()) {
      functionPasses.run(function, functionAnalysis);
    }
  }

  if (llvm::verifyModule(module, &llvm::errs())) {
    std::cerr << "module verification failed after instcombine pass\n";
    return false;
  }
  return true;
}

bool runNativePostRewritePeepholeStage(llvm::Module &module,
                                       const NativePipelineOptions &options) {
  NativeRegisterPostRewritePeepholeSummary summary =
      runNativeRegisterPostRewritePeephole(module);
  if (options.PrintRegisterSummary) {
    printNativeRegisterPostRewritePeepholeSummary(summary, llvm::errs());
  }
  if (llvm::verifyModule(module, &llvm::errs())) {
    std::cerr << "module verification failed after post-rewrite peephole pass\n";
    return false;
  }
  return true;
}

namespace {

bool hasExternallyVisibleFunctionDefinition(const llvm::Module &module) {
  for (const llvm::Function &function : module) {
    if (!function.isDeclaration() && !function.hasLocalLinkage()) {
      return true;
    }
  }
  return false;
}

void preserveAddressTakenFunctions(
    llvm::Module &module, const std::set<std::string> &functionNames) {
  if (functionNames.empty()) {
    return;
  }

  llvm::LLVMContext &context = module.getContext();
  llvm::Type *pointerType = llvm::PointerType::get(context, 0);
  std::vector<llvm::Constant *> functions;
  for (const std::string &name : functionNames) {
    llvm::Function *function = module.getFunction(name);
    if (function == nullptr || function->isDeclaration()) {
      continue;
    }
    functions.push_back(function);
  }
  if (functions.empty()) {
    return;
  }

  // Keep address-taken internal bodies alive through the final GlobalDCE.
  // Without this, only direct-call reachability is represented in the module
  // even though the original code can enter these functions through a function
  // pointer stored in code or relocated data.
  auto *arrayType = llvm::ArrayType::get(pointerType, functions.size());
  auto *initializer = llvm::ConstantArray::get(arrayType, functions);
  auto *used = new llvm::GlobalVariable(
      module, arrayType, /*isConstant=*/false,
      llvm::GlobalValue::AppendingLinkage, initializer, "llvm.used");
  used->setSection("llvm.metadata");
}

} // namespace

bool runNativeFinalCleanupStage(
    llvm::Module &module, const std::set<std::string> &preservedFunctions) {
  preserveAddressTakenFunctions(module, preservedFunctions);
  NativeRegisterFinalCleanupOptions passOptions;
  // If entry-point recovery has not found a public root yet, GlobalDCE would
  // delete every internal native function and hide the actual frontend state.
  passOptions.RunGlobalDCE = hasExternallyVisibleFunctionDefinition(module);
  runNativeRegisterFinalCleanup(module, passOptions);
  if (llvm::verifyModule(module, &llvm::errs())) {
    std::cerr << "module verification failed after final cleanup pass\n";
    return false;
  }
  return true;
}

} // namespace notdec::bin2llvm
