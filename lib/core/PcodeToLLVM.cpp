#include "notdec-bin2llvm/core/PcodeToLLVM.h"
#include "notdec-bin2llvm/intrinsics/NativeX87Intrinsic.h"
#include "notdec-bin2llvm/core/RegisterStorage.h"

#include "core/NotDecIntrinsics.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "PcodeToLLVMInternal.h"

namespace notdec::bin2llvm {

bool appendPcodeFunction(llvm::LLVMContext &context, llvm::Module &module,
                         const PcodeProgram &program,
                         const PcodeLoweringConfig &config,
                         std::string &errorMessage) {
  if (auto pointerByteSize =
          inferPointerByteSizeFromRegisters(program.Registers)) {
    setModuleDataLayoutFromPointerSize(module, *pointerByteSize,
                                       program.IsBigEndian);
  }

  auto *functionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
  llvm::Function *function = module.getFunction(config.EntryFunctionName);
  if (function != nullptr) {
    if (!function->empty()) {
      errorMessage = "duplicate function name: " + config.EntryFunctionName;
      return false;
    }
    if (function->getFunctionType() != functionType) {
      errorMessage =
          "function declaration type mismatch: " + config.EntryFunctionName;
      return false;
    }
    function->setLinkage(config.EntryFunctionLinkage);
  } else {
    function = llvm::Function::Create(functionType,
                                      config.EntryFunctionLinkage,
                                      config.EntryFunctionName, &module);
  }

  auto *entryBlock = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entryBlock);
  PcodeLowerer lowerer(context, module, *function, builder, config);
  if (!lowerer.lower(program, errorMessage)) {
    function->eraseFromParent();
    return false;
  }
  return true;
}

std::unique_ptr<llvm::Module>
buildPcodeModule(llvm::LLVMContext &context, const PcodeProgram &program,
                 const PcodeLoweringConfig &config, std::string &errorMessage) {
  auto module = std::make_unique<llvm::Module>(config.ModuleName, context);
  if (!appendPcodeFunction(context, *module, program, config, errorMessage)) {
    return nullptr;
  }
  return module;
}

} // namespace notdec::bin2llvm
