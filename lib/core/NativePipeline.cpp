#include "notdec-bin2llvm/core/NativePipeline.h"

#include "notdec-bin2llvm/core/NativeIROverride.h"
#include "notdec-bin2llvm/passes/function-pointer/NativeFunctionPointerPromotion.h"
#include "notdec-bin2llvm/passes/register/NativeRegisterFinalCleanup.h"
#include "notdec-bin2llvm/passes/register/NativeRegisterPostRewritePeephole.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"

#include <filesystem>
#include <system_error>
#include <utility>

namespace notdec::bin2llvm {
namespace {

namespace fs = std::filesystem;
using llvm::StringRef;

// --- 函数级属性 -------------------------------------------------------------

void applyBoolAttribute(llvm::Function &Function, llvm::Attribute::AttrKind Kind,
                        bool Value) {
  if (Value) {
    Function.addFnAttr(Kind);
  } else {
    Function.removeFnAttr(Kind);
  }
}

void applyFunctionAttributes(llvm::Function &Function,
                             const NativeFunctionAttributes &Attributes) {
  if (Attributes.NoReturn) {
    applyBoolAttribute(Function, llvm::Attribute::NoReturn,
                       *Attributes.NoReturn);
  }
  if (Attributes.NoUnwind) {
    applyBoolAttribute(Function, llvm::Attribute::NoUnwind,
                       *Attributes.NoUnwind);
  }
  if (Attributes.WillReturn) {
    applyBoolAttribute(Function, llvm::Attribute::WillReturn,
                       *Attributes.WillReturn);
  }
  if (Attributes.NoInline) {
    applyBoolAttribute(Function, llvm::Attribute::NoInline,
                       *Attributes.NoInline);
  }
  if (Attributes.AlwaysInline) {
    applyBoolAttribute(Function, llvm::Attribute::AlwaysInline,
                       *Attributes.AlwaysInline);
  }
  if (Attributes.Cold) {
    applyBoolAttribute(Function, llvm::Attribute::Cold, *Attributes.Cold);
  }
  if (Attributes.Memory) {
    const std::string &Memory = *Attributes.Memory;
    if (Memory == "none") {
      Function.setMemoryEffects(llvm::MemoryEffects::none());
    } else if (Memory == "read") {
      Function.setMemoryEffects(llvm::MemoryEffects::readOnly());
    } else if (Memory == "write") {
      Function.setMemoryEffects(llvm::MemoryEffects::writeOnly());
    } else if (Memory == "readwrite") {
      Function.setMemoryEffects(llvm::MemoryEffects::unknown());
    } else if (Memory == "argmem") {
      Function.setMemoryEffects(llvm::MemoryEffects::argMemOnly());
    } else if (Memory == "inaccessiblemem") {
      Function.setMemoryEffects(llvm::MemoryEffects::inaccessibleMemOnly());
    }
  }
}

bool applyFunctionClaims(NativePipelineContext &Context, std::string &Error) {
  for (size_t I = 0; I < Context.Config.Functions.size(); ++I) {
    const NativeFunctionConfig &Claim = Context.Config.Functions[I];
    std::string Path =
        "functions[" + std::to_string(I) + "].select";
    std::set<std::string> Matched;
    if (!resolveNativeFunctionSelectors(
            Context.Module, Context.FunctionIdentities,
            llvm::ArrayRef<NativeFunctionSelector>(Claim.Select), Path,
            /*IncludeDeclarations=*/true, Matched, Error)) {
      return false;
    }
    for (const std::string &Name : Matched) {
      llvm::Function *Function = Context.Module.getFunction(Name);
      if (Function == nullptr) {
        continue;
      }
      applyFunctionAttributes(*Function, Claim.Attributes);
      if (Claim.Pin) {
        Context.PinnedFunctions.insert(Name);
        Context.PreservedFunctions.insert(Name);
      }
    }
  }
  return true;
}

// --- 输出与校验 -------------------------------------------------------------

std::string stageDumpPath(StringRef Directory, size_t Index, StringRef Name,
                          StringRef Suffix) {
  std::string FileName =
      llvm::formatv("{0:D3}-{1}-{2}.ll", Index, Name, Suffix).str();
  return (fs::path(Directory.str()) / FileName).string();
}

bool writeModuleToPath(llvm::Module &Module, const std::string &Path) {
  std::error_code EC;
  llvm::raw_fd_ostream Output(Path, EC);
  if (EC) {
    llvm::errs() << "failed to write " << Path << ": " << EC.message() << '\n';
    return false;
  }
  Module.print(Output, nullptr);
  return true;
}

bool verifyStageOutput(NativePipelineContext &Context, size_t Index,
                       StringRef StageName, std::string &Error) {
  std::string Message;
  llvm::raw_string_ostream Stream(Message);
  if (!llvm::verifyModule(Context.Module, &Stream)) {
    return true;
  }
  Stream.flush();
  if (!Context.OutputPath.empty()) {
    std::string BadPath = Context.OutputPath + "." +
                          llvm::formatv("{0:D3}", Index).str() + "-" +
                          StageName.str() + ".bad.ll";
    writeModuleToPath(Context.Module, BadPath);
  }
  Error = "module verification failed: " + Message;
  return false;
}

// --- 各 stage 的执行 --------------------------------------------------------

bool runInstCombineStage(const NativeStageConfig &Stage,
                         NativePipelineContext &Context, StringRef Path,
                         std::string &Error) {
  std::set<std::string> Excluded;
  std::string OptionsPath = (Path + ".options").str();
  if (!resolveNativeFunctionSelectors(Context.Module,
                                      Context.FunctionIdentities,
                                      Stage.InstCombine.Exclude,
                                      OptionsPath + ".exclude",
                                      /*IncludeDeclarations=*/false, Excluded,
                                      Error)) {
    return false;
  }
  llvm::LoopAnalysisManager LoopAnalysis;
  llvm::FunctionAnalysisManager FunctionAnalysis;
  llvm::CGSCCAnalysisManager CGSCCAnalysis;
  llvm::ModuleAnalysisManager ModuleAnalysis;
  llvm::PassBuilder Builder;
  Builder.registerModuleAnalyses(ModuleAnalysis);
  Builder.registerCGSCCAnalyses(CGSCCAnalysis);
  Builder.registerFunctionAnalyses(FunctionAnalysis);
  Builder.registerLoopAnalyses(LoopAnalysis);
  Builder.crossRegisterProxies(LoopAnalysis, FunctionAnalysis, CGSCCAnalysis,
                               ModuleAnalysis);

  llvm::FunctionPassManager FunctionPasses;
  if (Stage.InstCombine.EarlyCSE) {
    FunctionPasses.addPass(llvm::EarlyCSEPass());
  }
  FunctionPasses.addPass(llvm::InstCombinePass());
  FunctionPasses.addPass(llvm::SimplifyCFGPass());
  for (llvm::Function &Function : Context.Module) {
    if (Function.isDeclaration()) {
      continue;
    }
    std::string Name = Function.getName().str();
    if (Context.PinnedFunctions.count(Name) != 0 ||
        Excluded.count(Name) != 0) {
      continue;
    }
    FunctionPasses.run(Function, FunctionAnalysis);
  }
  return true;
}

bool runRegisterEliminationStage(const NativeStageConfig &Stage,
                                 NativePipelineContext &Context, StringRef Path,
                                 std::string &Error) {
  (void)Path;
  (void)Error;
  NativeRegisterEliminationOptions Options;
  const NativeRegisterEliminationConfig &Config = Stage.RegisterElimination;
  Options.EnableRewrite = Config.Rewrite;
  Options.EnableResidueRemoval = Config.ResidueRemoval;
  Options.EnablePostRewriteInstCombine = Config.PostRewriteInstCombine;
  Options.EnableVarArgRecovery = Config.VarArgRecovery;
  Options.AbiForAllFunctions = Config.AbiForAllFunctions;
  Options.AttachMetadata = Config.AttachMetadata;
  Options.PrintSummary = Context.PrintSummary;
  Options.ExternalPrototypeJsonPath = Config.ResolvedExternalPrototypes;
  Options.IgnoredRegisters.insert(Config.IgnoredRegisters.begin(),
                                  Config.IgnoredRegisters.end());
  Context.EliminationSummary =
      runNativeRegisterElimination(Context.Module, Options);
  Context.AbiForAllFunctions = Options.AbiForAllFunctions;
  return true;
}

bool runFunctionPointerPromotionStage(const NativeStageConfig &Stage,
                                      NativePipelineContext &Context,
                                      StringRef Path, std::string &Error) {
  (void)Stage;
  (void)Path;
  (void)Error;
  NativeFunctionPointerPromotionSummary Summary =
      runNativeFunctionPointerPromotion(Context.Module);
  if (Context.PrintSummary) {
    llvm::errs() << "Native function pointer promotion: seen="
                 << Summary.IndirectCallsSeen
                 << " promoted=" << Summary.IndirectCallsPromoted
                 << " slots=" << Summary.SlotsWithKnownSingleTarget
                 << " unknown_writes=" << Summary.SlotsWithUnknownWrites
                 << " escaped=" << Summary.SlotsWithEscapedAddress
                 << " multiple_targets=" << Summary.SlotsWithMultipleTargets
                 << '\n';
  }
  return true;
}

bool runPostRewritePeepholeStage(const NativeStageConfig &Stage,
                                 NativePipelineContext &Context, StringRef Path,
                                 std::string &Error) {
  (void)Stage;
  (void)Path;
  (void)Error;
  NativeRegisterPostRewritePeepholeSummary Summary =
      runNativeRegisterPostRewritePeephole(Context.Module);
  if (Context.PrintSummary) {
    printNativeRegisterPostRewritePeepholeSummary(Summary, llvm::errs());
  }
  return true;
}

bool runIROverrideStage(const NativeStageConfig &Stage,
                        NativePipelineContext &Context, StringRef Path,
                        std::string &Error) {
  return applyNativeIROverride(Context.Module, Stage.IROverride,
                               Context.FunctionIdentities, Error);
}

bool hasExternallyVisibleFunctionDefinition(const llvm::Module &Module) {
  for (const llvm::Function &Function : Module) {
    if (!Function.isDeclaration() && !Function.hasLocalLinkage()) {
      return true;
    }
  }
  return false;
}

void preserveAddressTakenFunctions(llvm::Module &Module,
                                   const std::set<std::string> &FunctionNames) {
  if (FunctionNames.empty()) {
    return;
  }
  llvm::LLVMContext &Context = Module.getContext();
  llvm::Type *PointerType = llvm::PointerType::get(Context, 0);
  std::vector<llvm::Constant *> Functions;
  for (const std::string &Name : FunctionNames) {
    llvm::Function *Function = Module.getFunction(Name);
    if (Function == nullptr || Function->isDeclaration()) {
      continue;
    }
    Functions.push_back(Function);
  }
  if (Functions.empty()) {
    return;
  }
  auto *ArrayType = llvm::ArrayType::get(PointerType, Functions.size());
  auto *Initializer = llvm::ConstantArray::get(ArrayType, Functions);
  auto *Used = new llvm::GlobalVariable(
      Module, ArrayType, /*isConstant=*/false,
      llvm::GlobalValue::AppendingLinkage, Initializer, "llvm.used");
  Used->setSection("llvm.metadata");
}

bool runFinalCleanupStage(const NativeStageConfig &Stage,
                          NativePipelineContext &Context, StringRef Path,
                          std::string &Error) {
  (void)Path;
  (void)Error;
  const NativeFinalCleanupOptions &Config = Stage.FinalCleanup;
  if (Config.PreserveAddressTaken) {
    preserveAddressTakenFunctions(Context.Module,
                                  Context.PreservedFunctions);
  }
  NativeRegisterFinalCleanupOptions Options;
  switch (Config.GlobalDce) {
  case NativeFinalCleanupOptions::GlobalDceMode::Auto:
    Options.RunGlobalDCE =
        hasExternallyVisibleFunctionDefinition(Context.Module);
    break;
  case NativeFinalCleanupOptions::GlobalDceMode::Enabled:
    Options.RunGlobalDCE = true;
    break;
  case NativeFinalCleanupOptions::GlobalDceMode::Disabled:
    Options.RunGlobalDCE = false;
    break;
  }
  NativeRegisterFinalCleanupSummary CleanupSummary =
      runNativeRegisterFinalCleanup(Context.Module, Options);
  if (CleanupSummary.PoisonControlFlowConditionsReplaced != 0 ||
      CleanupSummary.PoisonReturnValuesReplaced != 0) {
    llvm::errs() << "Native final cleanup: poison terminators replaced="
                 << CleanupSummary.PoisonControlFlowConditionsReplaced +
                        CleanupSummary.PoisonReturnValuesReplaced
                 << " (conditions="
                 << CleanupSummary.PoisonControlFlowConditionsReplaced
                 << ", returns=" << CleanupSummary.PoisonReturnValuesReplaced
                 << ")\n";
  }
  return true;
}

bool runEraseDeadHelpersStage(const NativeStageConfig &Stage,
                              NativePipelineContext &Context, StringRef Path,
                              std::string &Error) {
  (void)Stage;
  (void)Path;
  (void)Error;
  eraseDeadNotDecValueHelperCalls(Context.Module);
  eraseUnusedRegisterHelperDeclarations(Context.Module);
  // 最后一次 InstCombine 可能才把某个 phi/select 折成 poison；这里补做一次
  // 终结指令的兜底，保证产物里没有 poison 条件/返回值。
  replacePoisonCallArguments(Context.Module);
  replacePoisonPhiIncomings(Context.Module);
  NativePoisonTerminatorSummary poisonTerminators =
      replacePoisonTerminatorValues(Context.Module);
  if (poisonTerminators.ConditionsReplaced != 0 ||
      poisonTerminators.ReturnsReplaced != 0) {
    llvm::errs() << "Native erase dead helpers: poison terminators replaced="
                 << poisonTerminators.ConditionsReplaced +
                        poisonTerminators.ReturnsReplaced
                 << " (conditions=" << poisonTerminators.ConditionsReplaced
                 << ", returns=" << poisonTerminators.ReturnsReplaced << ")\n";
  }
  return true;
}

bool runNativeStage(const NativeStageConfig &Stage,
                    NativePipelineContext &Context, StringRef Path,
                    std::string &Error) {
  switch (Stage.Kind) {
  case NativeStageKind::InstCombine:
    return runInstCombineStage(Stage, Context, Path, Error);
  case NativeStageKind::RegisterElimination:
    return runRegisterEliminationStage(Stage, Context, Path, Error);
  case NativeStageKind::FunctionPointerPromotion:
    return runFunctionPointerPromotionStage(Stage, Context, Path, Error);
  case NativeStageKind::PostRewritePeephole:
    return runPostRewritePeepholeStage(Stage, Context, Path, Error);
  case NativeStageKind::IROverride:
    return runIROverrideStage(Stage, Context, Path, Error);
  case NativeStageKind::FinalCleanup:
    return runFinalCleanupStage(Stage, Context, Path, Error);
  case NativeStageKind::EraseDeadHelpers:
    return runEraseDeadHelpersStage(Stage, Context, Path, Error);
  }
  Error = "unhandled stage kind";
  return false;
}

} // namespace

bool runNativePipeline(NativePipelineContext &Context, std::string &Error) {
  if (!applyFunctionClaims(Context, Error)) {
    return false;
  }
  if (!Context.DumpStagesDir.empty()) {
    std::error_code EC;
    fs::create_directories(Context.DumpStagesDir, EC);
    if (EC) {
      Error = "failed to create dump directory " + Context.DumpStagesDir +
              ": " + EC.message();
      return false;
    }
  }
  for (size_t I = 0; I < Context.Config.Pipeline.size(); ++I) {
    const NativeStageConfig &Stage = Context.Config.Pipeline[I];
    std::string Path = "pipeline[" + std::to_string(I) + "]";
    bool Dump = !Context.DumpStagesDir.empty();
    if (Dump) {
      writeModuleToPath(Context.Module,
                        stageDumpPath(Context.DumpStagesDir, I, Stage.Name,
                                      "before"));
    }
    if (!runNativeStage(Stage, Context, Path, Error)) {
      Error = Path + " (" + Stage.Name + "): " + Error;
      return false;
    }
    if (Dump) {
      writeModuleToPath(Context.Module,
                        stageDumpPath(Context.DumpStagesDir, I, Stage.Name,
                                      "after"));
    }
    if (!verifyStageOutput(Context, I, Stage.Name, Error)) {
      Error = Path + " (" + Stage.Name + "): " + Error;
      return false;
    }
  }
  attachNativeConfigMetadata(Context.Module, Context.Config);
  return true;
}

} // namespace notdec::bin2llvm
