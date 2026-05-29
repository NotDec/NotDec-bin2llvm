#include "notdec-bin2llvm/passes/NativeRegisterSSA.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

llvm::MDNode *registerAccessMetadata(llvm::LLVMContext &context,
                                     const std::string &name) {
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "base=" + name),
      llvm::MDString::get(context, "space=register"),
      llvm::MDString::get(context, "offset=0"),
      llvm::MDString::get(context, "size=8"),
      llvm::MDString::get(context, "name=" + name),
  };
  return llvm::MDNode::get(context, fields);
}

llvm::GlobalVariable *createRegisterGlobal(llvm::Module &module,
                                           const std::string &name) {
  llvm::LLVMContext &context = module.getContext();
  auto *type = llvm::Type::getInt64Ty(context);
  auto *global = new llvm::GlobalVariable(
      module, type, false, llvm::GlobalValue::ExternalLinkage, nullptr, name);
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "space=register"),
      llvm::MDString::get(context, "offset=0"),
      llvm::MDString::get(context, "size=8"),
      llvm::MDString::get(context, "name=" + name),
  };
  global->setMetadata("notdec.register", llvm::MDNode::get(context, fields));
  return global;
}

std::unique_ptr<llvm::Module> createModule(llvm::LLVMContext &context) {
  auto module = std::make_unique<llvm::Module>("instcombine-metadata-test",
                                               context);
  llvm::GlobalVariable *rdi = createRegisterGlobal(*module, "RDI");
  auto *funcType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "uses_rdi", *module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *load = builder.CreateLoad(rdi->getValueType(), rdi, "rdi");
  load->setMetadata("notdec.register.access",
                    registerAccessMetadata(context, "RDI"));
  llvm::Value *sum =
      builder.CreateAdd(load, llvm::ConstantInt::get(load->getType(), 1));
  llvm::StoreInst *store = builder.CreateStore(sum, rdi);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, "RDI"));
  builder.CreateRet(sum);
  return module;
}

std::unique_ptr<llvm::Module> createCallBarrierModule(
    llvm::LLVMContext &context) {
  auto module = std::make_unique<llvm::Module>("instcombine-call-test",
                                               context);
  llvm::GlobalVariable *rdi = createRegisterGlobal(*module, "RDI");
  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "external_call", *module);

  auto *funcType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "call_then_load_rdi", *module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(rdi->getValueType(), 0x1234), rdi);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, "RDI"));
  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *load =
      builder.CreateLoad(rdi->getValueType(), rdi, "rdi_after_call");
  load->setMetadata("notdec.register.access",
                    registerAccessMetadata(context, "RDI"));
  builder.CreateRet(load);
  return module;
}

void runInstCombine(llvm::Function &function) {
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
  functionPasses.addPass(llvm::InstCombinePass());
  functionPasses.run(function, functionAnalysis);
}

bool hasRegisterAccessLoad(const llvm::Function &function) {
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &instruction : block) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (load == nullptr) {
        continue;
      }
      if (load->getMetadata("notdec.register.access") != nullptr) {
        return true;
      }
    }
  }
  return false;
}

bool hasRegisterAccessStore(const llvm::Function &function) {
  for (const llvm::BasicBlock &block : function) {
    for (const llvm::Instruction &instruction : block) {
      auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
      if (store == nullptr) {
        continue;
      }
      if (store->getMetadata("notdec.register.access") != nullptr) {
        return true;
      }
    }
  }
  return false;
}

bool expect(bool condition, const std::string &message) {
  if (condition) {
    return true;
  }
  std::cerr << message << '\n';
  return false;
}

} // namespace

int main() {
  llvm::LLVMContext baselineContext;
  std::unique_ptr<llvm::Module> baselineModule = createModule(baselineContext);
  notdec::bin2llvm::NativeRegisterSSAOptions options;
  options.EnableRewrite = true;
  notdec::bin2llvm::NativeRegisterSSASummary baseline =
      notdec::bin2llvm::runNativeRegisterSSA(*baselineModule, options);
  if (llvm::verifyModule(*baselineModule, &llvm::errs())) {
    std::cerr << "baseline module verification failed\n";
    return EXIT_FAILURE;
  }

  llvm::LLVMContext combinedContext;
  std::unique_ptr<llvm::Module> combinedModule = createModule(combinedContext);
  llvm::Function *combinedFunction = combinedModule->getFunction("uses_rdi");
  if (combinedFunction == nullptr) {
    std::cerr << "test function missing\n";
    return EXIT_FAILURE;
  }
  runInstCombine(*combinedFunction);

  bool ok = true;
  ok &= expect(hasRegisterAccessLoad(*combinedFunction),
               "instcombine dropped register access metadata");
  ok &= expect(hasRegisterAccessStore(*combinedFunction),
               "instcombine dropped register store metadata");

  notdec::bin2llvm::NativeRegisterSSASummary combined =
      notdec::bin2llvm::runNativeRegisterSSA(*combinedModule, options);
  if (llvm::verifyModule(*combinedModule, &llvm::errs())) {
    std::cerr << "combined module verification failed\n";
    return EXIT_FAILURE;
  }

  ok &= expect(combined.LoadsSeen >= baseline.LoadsSeen,
               "load count dropped after instcombine");
  ok &= expect(combined.StoresSeen >= baseline.StoresSeen,
               "store count dropped after instcombine");
  ok &= expect(combined.ExternalInputs >= baseline.ExternalInputs,
               "external input count dropped after instcombine");
  ok &= expect(combined.LoadsReplaced >= baseline.LoadsReplaced,
               "replaced load count dropped after instcombine");

  llvm::LLVMContext callBaselineContext;
  std::unique_ptr<llvm::Module> callBaselineModule =
      createCallBarrierModule(callBaselineContext);
  notdec::bin2llvm::NativeRegisterSSASummary callBaseline =
      notdec::bin2llvm::runNativeRegisterSSA(*callBaselineModule, options);
  if (llvm::verifyModule(*callBaselineModule, &llvm::errs())) {
    std::cerr << "call baseline module verification failed\n";
    return EXIT_FAILURE;
  }

  llvm::LLVMContext callCombinedContext;
  std::unique_ptr<llvm::Module> callCombinedModule =
      createCallBarrierModule(callCombinedContext);
  llvm::Function *callCombinedFunction =
      callCombinedModule->getFunction("call_then_load_rdi");
  if (callCombinedFunction == nullptr) {
    std::cerr << "call test function missing\n";
    return EXIT_FAILURE;
  }
  runInstCombine(*callCombinedFunction);
  notdec::bin2llvm::NativeRegisterSSASummary callCombined =
      notdec::bin2llvm::runNativeRegisterSSA(*callCombinedModule, options);
  if (llvm::verifyModule(*callCombinedModule, &llvm::errs())) {
    std::cerr << "call combined module verification failed\n";
    return EXIT_FAILURE;
  }

  ok &= expect(callCombined.CallsSeen >= callBaseline.CallsSeen,
               "call count dropped after instcombine");
  ok &= expect(callCombined.LoadsReplaced <= callBaseline.LoadsReplaced,
               "call barrier load was incorrectly replaced after instcombine");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
