#include "notdec-bin2llvm/ModuleBuilder.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

namespace notdec::bin2llvm {

std::unique_ptr<llvm::Module> buildDemoModule(
    llvm::LLVMContext &context, const BuildConfig &config) {
  auto module = std::make_unique<llvm::Module>(config.ModuleName, context);

  auto *functionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
  auto *function = llvm::Function::Create(
      functionType, llvm::GlobalValue::ExternalLinkage,
      config.EntryFunctionName, module.get());

  auto *entryBlock = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entryBlock);
  builder.CreateRetVoid();

  return module;
}

}  // namespace notdec::bin2llvm
