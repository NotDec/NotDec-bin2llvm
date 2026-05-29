#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/passes/NativePrototypeRecovery.h"
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

notdec::bin2llvm::NativeAbiParamEntry registerParamEntry(
    const std::string &name) {
  notdec::bin2llvm::NativeAbiParamEntry entry;
  entry.MinSize = 1;
  entry.MaxSize = 8;
  entry.Align = 8;
  entry.Storage.Kind = notdec::bin2llvm::NativeAbiStorageKind::Register;
  entry.Storage.Name = name;
  return entry;
}

void attachPrototypeTestAbi(llvm::Module &module) {
  notdec::bin2llvm::NativeAbiSpec abi;
  abi.PrototypeName = "__stdcall";
  abi.Inputs.push_back(registerParamEntry("RDI"));
  abi.Outputs.push_back(registerParamEntry("RAX"));
  notdec::bin2llvm::attachNativeAbiMetadata(module, abi);
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

std::unique_ptr<llvm::Module> createPrototypeCandidateModule(
    llvm::LLVMContext &context) {
  auto module =
      std::make_unique<llvm::Module>("instcombine-prototype-test", context);
  llvm::GlobalVariable *rdi = createRegisterGlobal(*module, "RDI");
  llvm::GlobalVariable *rax = createRegisterGlobal(*module, "RAX");
  attachPrototypeTestAbi(*module);

  auto *funcType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "prototype_rdi_to_rax", *module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::LoadInst *input = builder.CreateLoad(rdi->getValueType(), rdi, "rdi");
  input->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, "RDI"));
  llvm::Value *result =
      builder.CreateAdd(input, llvm::ConstantInt::get(input->getType(), 1));
  llvm::StoreInst *store = builder.CreateStore(result, rax);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, "RAX"));
  builder.CreateRetVoid();
  return module;
}

std::unique_ptr<llvm::Module> createMultireturnPrototypeModule(
    llvm::LLVMContext &context) {
  auto module =
      std::make_unique<llvm::Module>("instcombine-multireturn-test", context);
  llvm::GlobalVariable *rax = createRegisterGlobal(*module, "RAX");
  attachPrototypeTestAbi(*module);

  auto *funcType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                              {llvm::Type::getInt1Ty(context)}, {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "multireturn_rax", *module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right = llvm::BasicBlock::Create(context, "right", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(function->getArg(0), left, right);

  builder.SetInsertPoint(left);
  llvm::StoreInst *leftStore = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 0x1111), rax);
  leftStore->setMetadata("notdec.register.access",
                         registerAccessMetadata(context, "RAX"));
  builder.CreateRetVoid();

  builder.SetInsertPoint(right);
  llvm::StoreInst *rightStore = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 0x1111), rax);
  rightStore->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, "RAX"));
  builder.CreateRetVoid();
  return module;
}

std::unique_ptr<llvm::Module> createConflictingReturnPrototypeModule(
    llvm::LLVMContext &context) {
  auto module =
      std::make_unique<llvm::Module>("instcombine-conflict-return-test",
                                     context);
  llvm::GlobalVariable *rax = createRegisterGlobal(*module, "RAX");
  attachPrototypeTestAbi(*module);

  auto *funcType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                              {llvm::Type::getInt1Ty(context)}, {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "conflicting_return_rax", *module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::BasicBlock *left = llvm::BasicBlock::Create(context, "left", function);
  llvm::BasicBlock *right = llvm::BasicBlock::Create(context, "right", function);

  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(function->getArg(0), left, right);

  builder.SetInsertPoint(left);
  llvm::StoreInst *leftStore = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 0x1111), rax);
  leftStore->setMetadata("notdec.register.access",
                         registerAccessMetadata(context, "RAX"));
  builder.CreateRetVoid();

  builder.SetInsertPoint(right);
  llvm::StoreInst *rightStore = builder.CreateStore(
      llvm::ConstantInt::get(rax->getValueType(), 0x2222), rax);
  rightStore->setMetadata("notdec.register.access",
                          registerAccessMetadata(context, "RAX"));
  builder.CreateRetVoid();
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

void attachPreservesMetadata(llvm::Function &function,
                             llvm::GlobalVariable *global,
                             const std::string &name) {
  llvm::LLVMContext &context = function.getContext();
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "name=" + name),
      llvm::ValueAsMetadata::get(global),
  };
  llvm::Metadata *entries[] = {llvm::MDNode::get(context, fields)};
  function.setMetadata("notdec.register.preserves",
                       llvm::MDNode::get(context, entries));
}

void attachClobbersMetadata(llvm::Function &function,
                            llvm::GlobalVariable *global,
                            const std::string &name) {
  llvm::LLVMContext &context = function.getContext();
  llvm::Metadata *fields[] = {
      llvm::MDString::get(context, "name=" + name),
      llvm::ValueAsMetadata::get(global),
  };
  llvm::Metadata *entries[] = {llvm::MDNode::get(context, fields)};
  function.setMetadata("notdec.register.clobbers",
                       llvm::MDNode::get(context, entries));
}

std::unique_ptr<llvm::Module> createDirectPreserveModule(
    llvm::LLVMContext &context) {
  auto module = std::make_unique<llvm::Module>("instcombine-direct-test",
                                               context);
  llvm::GlobalVariable *rdi = createRegisterGlobal(*module, "RDI");
  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "preserves_rdi", *module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  calleeBuilder.CreateRetVoid();
  attachPreservesMetadata(*callee, rdi, "RDI");

  auto *funcType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "direct_then_load_rdi", *module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(rdi->getValueType(), 0x5678), rdi);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, "RDI"));
  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *load =
      builder.CreateLoad(rdi->getValueType(), rdi, "rdi_after_direct");
  load->setMetadata("notdec.register.access",
                    registerAccessMetadata(context, "RDI"));
  builder.CreateRet(load);
  return module;
}

std::unique_ptr<llvm::Module> createDirectClobberModule(
    llvm::LLVMContext &context) {
  auto module = std::make_unique<llvm::Module>("instcombine-direct-clobber-test",
                                               context);
  llvm::GlobalVariable *rdi = createRegisterGlobal(*module, "RDI");
  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  llvm::Function *callee =
      llvm::Function::Create(calleeType, llvm::GlobalValue::ExternalLinkage,
                             "clobbers_rdi", *module);
  llvm::BasicBlock *calleeEntry =
      llvm::BasicBlock::Create(context, "entry", callee);
  llvm::IRBuilder<> calleeBuilder(calleeEntry);
  calleeBuilder.CreateRetVoid();
  attachClobbersMetadata(*callee, rdi, "RDI");

  auto *funcType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "direct_clobber_then_load_rdi", *module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(rdi->getValueType(), 0x9abc), rdi);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, "RDI"));
  builder.CreateCall(calleeType, callee);
  llvm::LoadInst *load =
      builder.CreateLoad(rdi->getValueType(), rdi, "rdi_after_clobber");
  load->setMetadata("notdec.register.access",
                    registerAccessMetadata(context, "RDI"));
  builder.CreateRet(load);
  return module;
}

std::unique_ptr<llvm::Module> createIndirectCallModule(
    llvm::LLVMContext &context) {
  auto module = std::make_unique<llvm::Module>("instcombine-indirect-test",
                                               context);
  llvm::GlobalVariable *rdi = createRegisterGlobal(*module, "RDI");
  auto *calleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {});
  auto *calleePtrType = llvm::PointerType::getUnqual(context);
  auto *funcType = llvm::FunctionType::get(
      llvm::Type::getInt64Ty(context), {calleePtrType}, {});
  llvm::Function *function =
      llvm::Function::Create(funcType, llvm::GlobalValue::ExternalLinkage,
                             "indirect_then_load_rdi", *module);
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  llvm::StoreInst *store = builder.CreateStore(
      llvm::ConstantInt::get(rdi->getValueType(), 0xbeef), rdi);
  store->setMetadata("notdec.register.access",
                     registerAccessMetadata(context, "RDI"));
  builder.CreateCall(calleeType, function->getArg(0));
  llvm::LoadInst *load =
      builder.CreateLoad(rdi->getValueType(), rdi, "rdi_after_indirect");
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

bool functionMetadataHasRegister(const llvm::Function &function,
                                 llvm::StringRef kind,
                                 llvm::StringRef name) {
  llvm::MDNode *node = function.getMetadata(kind);
  if (node == nullptr) {
    return false;
  }
  std::string expected = ("name=" + name).str();
  for (const llvm::MDOperand &operand : node->operands()) {
    auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(operand.get());
    if (entry == nullptr) {
      continue;
    }
    for (const llvm::MDOperand &fieldOperand : entry->operands()) {
      auto *field = llvm::dyn_cast_or_null<llvm::MDString>(fieldOperand.get());
      if (field != nullptr && field->getString() == expected) {
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

  llvm::LLVMContext directBaselineContext;
  std::unique_ptr<llvm::Module> directBaselineModule =
      createDirectPreserveModule(directBaselineContext);
  notdec::bin2llvm::NativeRegisterSSASummary directBaseline =
      notdec::bin2llvm::runNativeRegisterSSA(*directBaselineModule, options);
  if (llvm::verifyModule(*directBaselineModule, &llvm::errs())) {
    std::cerr << "direct baseline module verification failed\n";
    return EXIT_FAILURE;
  }

  llvm::LLVMContext directCombinedContext;
  std::unique_ptr<llvm::Module> directCombinedModule =
      createDirectPreserveModule(directCombinedContext);
  llvm::Function *directCombinedCallee =
      directCombinedModule->getFunction("preserves_rdi");
  llvm::Function *directCombinedFunction =
      directCombinedModule->getFunction("direct_then_load_rdi");
  if (directCombinedCallee == nullptr || directCombinedFunction == nullptr) {
    std::cerr << "direct test function missing\n";
    return EXIT_FAILURE;
  }
  runInstCombine(*directCombinedCallee);
  runInstCombine(*directCombinedFunction);
  ok &= expect(functionMetadataHasRegister(*directCombinedCallee,
                                           "notdec.register.preserves", "RDI"),
               "instcombine dropped direct callee preserves metadata");

  notdec::bin2llvm::NativeRegisterSSASummary directCombined =
      notdec::bin2llvm::runNativeRegisterSSA(*directCombinedModule, options);
  if (llvm::verifyModule(*directCombinedModule, &llvm::errs())) {
    std::cerr << "direct combined module verification failed\n";
    return EXIT_FAILURE;
  }

  ok &= expect(directCombined.CallsSeen >= directBaseline.CallsSeen,
               "direct call count dropped after instcombine");
  ok &= expect(directCombined.LoadsReplaced >= directBaseline.LoadsReplaced,
               "direct preserving call load replacement dropped");

  llvm::LLVMContext clobberBaselineContext;
  std::unique_ptr<llvm::Module> clobberBaselineModule =
      createDirectClobberModule(clobberBaselineContext);
  notdec::bin2llvm::NativeRegisterSSASummary clobberBaseline =
      notdec::bin2llvm::runNativeRegisterSSA(*clobberBaselineModule, options);
  if (llvm::verifyModule(*clobberBaselineModule, &llvm::errs())) {
    std::cerr << "clobber baseline module verification failed\n";
    return EXIT_FAILURE;
  }

  llvm::LLVMContext clobberCombinedContext;
  std::unique_ptr<llvm::Module> clobberCombinedModule =
      createDirectClobberModule(clobberCombinedContext);
  llvm::Function *clobberCombinedCallee =
      clobberCombinedModule->getFunction("clobbers_rdi");
  llvm::Function *clobberCombinedFunction =
      clobberCombinedModule->getFunction("direct_clobber_then_load_rdi");
  if (clobberCombinedCallee == nullptr || clobberCombinedFunction == nullptr) {
    std::cerr << "clobber test function missing\n";
    return EXIT_FAILURE;
  }
  runInstCombine(*clobberCombinedCallee);
  runInstCombine(*clobberCombinedFunction);
  ok &= expect(functionMetadataHasRegister(*clobberCombinedCallee,
                                           "notdec.register.clobbers", "RDI"),
               "instcombine dropped direct callee clobbers metadata");

  notdec::bin2llvm::NativeRegisterSSASummary clobberCombined =
      notdec::bin2llvm::runNativeRegisterSSA(*clobberCombinedModule, options);
  if (llvm::verifyModule(*clobberCombinedModule, &llvm::errs())) {
    std::cerr << "clobber combined module verification failed\n";
    return EXIT_FAILURE;
  }

  ok &= expect(clobberCombined.CallsSeen >= clobberBaseline.CallsSeen,
               "direct clobber call count dropped after instcombine");
  ok &= expect(clobberCombined.LoadsReplaced <= clobberBaseline.LoadsReplaced,
               "direct clobber call load was incorrectly replaced");

  llvm::LLVMContext indirectBaselineContext;
  std::unique_ptr<llvm::Module> indirectBaselineModule =
      createIndirectCallModule(indirectBaselineContext);
  notdec::bin2llvm::NativeRegisterSSASummary indirectBaseline =
      notdec::bin2llvm::runNativeRegisterSSA(*indirectBaselineModule, options);
  if (llvm::verifyModule(*indirectBaselineModule, &llvm::errs())) {
    std::cerr << "indirect baseline module verification failed\n";
    return EXIT_FAILURE;
  }

  llvm::LLVMContext indirectCombinedContext;
  std::unique_ptr<llvm::Module> indirectCombinedModule =
      createIndirectCallModule(indirectCombinedContext);
  llvm::Function *indirectCombinedFunction =
      indirectCombinedModule->getFunction("indirect_then_load_rdi");
  if (indirectCombinedFunction == nullptr) {
    std::cerr << "indirect test function missing\n";
    return EXIT_FAILURE;
  }
  runInstCombine(*indirectCombinedFunction);
  notdec::bin2llvm::NativeRegisterSSASummary indirectCombined =
      notdec::bin2llvm::runNativeRegisterSSA(*indirectCombinedModule, options);
  if (llvm::verifyModule(*indirectCombinedModule, &llvm::errs())) {
    std::cerr << "indirect combined module verification failed\n";
    return EXIT_FAILURE;
  }

  ok &= expect(indirectCombined.CallsSeen >= indirectBaseline.CallsSeen,
               "indirect call count dropped after instcombine");
  ok &= expect(indirectCombined.LoadsReplaced <= indirectBaseline.LoadsReplaced,
               "indirect call barrier load was incorrectly replaced");

  llvm::LLVMContext prototypeContext;
  std::unique_ptr<llvm::Module> prototypeModule =
      createPrototypeCandidateModule(prototypeContext);
  llvm::Function *prototypeFunction =
      prototypeModule->getFunction("prototype_rdi_to_rax");
  if (prototypeFunction == nullptr) {
    std::cerr << "prototype test function missing\n";
    return EXIT_FAILURE;
  }
  runInstCombine(*prototypeFunction);
  notdec::bin2llvm::runNativeRegisterSSA(*prototypeModule, options);
  notdec::bin2llvm::NativePrototypeRecoveryOptions prototypeOptions;
  notdec::bin2llvm::NativePrototypeRecoverySummary prototypeSummary =
      notdec::bin2llvm::runNativePrototypeRecovery(*prototypeModule,
                                                   prototypeOptions);
  if (llvm::verifyModule(*prototypeModule, &llvm::errs())) {
    std::cerr << "prototype module verification failed\n";
    return EXIT_FAILURE;
  }

  ok &= expect(prototypeSummary.InputCandidates >= 1,
               "prototype input candidate count dropped after instcombine");
  ok &= expect(prototypeSummary.ReturnCandidates >= 1,
               "prototype return candidate count dropped after instcombine");
  ok &= expect(functionMetadataHasRegister(*prototypeFunction,
                                           "notdec.prototype.input_candidates",
                                           "RDI"),
               "RDI input candidate was missing after instcombine");
  ok &= expect(functionMetadataHasRegister(*prototypeFunction,
                                           "notdec.prototype.return_candidates",
                                           "RAX"),
               "RAX return candidate was missing after instcombine");

  llvm::LLVMContext multireturnContext;
  std::unique_ptr<llvm::Module> multireturnModule =
      createMultireturnPrototypeModule(multireturnContext);
  llvm::Function *multireturnFunction =
      multireturnModule->getFunction("multireturn_rax");
  if (multireturnFunction == nullptr) {
    std::cerr << "multireturn test function missing\n";
    return EXIT_FAILURE;
  }
  runInstCombine(*multireturnFunction);
  notdec::bin2llvm::runNativeRegisterSSA(*multireturnModule, options);
  notdec::bin2llvm::NativePrototypeRecoveryOptions multireturnOptions;
  notdec::bin2llvm::NativePrototypeRecoverySummary multireturnSummary =
      notdec::bin2llvm::runNativePrototypeRecovery(*multireturnModule,
                                                   multireturnOptions);
  if (llvm::verifyModule(*multireturnModule, &llvm::errs())) {
    std::cerr << "multireturn module verification failed\n";
    return EXIT_FAILURE;
  }

  ok &= expect(multireturnSummary.ReturnCandidates >= 1,
               "multireturn return candidate count dropped after instcombine");
  ok &= expect(functionMetadataHasRegister(*multireturnFunction,
                                           "notdec.prototype.return_candidates",
                                           "RAX"),
               "multireturn RAX return candidate was missing after instcombine");

  llvm::LLVMContext conflictContext;
  std::unique_ptr<llvm::Module> conflictModule =
      createConflictingReturnPrototypeModule(conflictContext);
  llvm::Function *conflictFunction =
      conflictModule->getFunction("conflicting_return_rax");
  if (conflictFunction == nullptr) {
    std::cerr << "conflicting return test function missing\n";
    return EXIT_FAILURE;
  }
  runInstCombine(*conflictFunction);
  notdec::bin2llvm::runNativeRegisterSSA(*conflictModule, options);
  notdec::bin2llvm::NativePrototypeRecoveryOptions conflictOptions;
  notdec::bin2llvm::NativePrototypeRecoverySummary conflictSummary =
      notdec::bin2llvm::runNativePrototypeRecovery(*conflictModule,
                                                   conflictOptions);
  if (llvm::verifyModule(*conflictModule, &llvm::errs())) {
    std::cerr << "conflicting return module verification failed\n";
    return EXIT_FAILURE;
  }

  ok &= expect(conflictSummary.ReturnCandidates == 0,
               "conflicting return candidate was marked after instcombine");
  ok &= expect(!functionMetadataHasRegister(
                   *conflictFunction, "notdec.prototype.return_candidates",
                   "RAX"),
               "conflicting RAX return candidate was present after instcombine");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
