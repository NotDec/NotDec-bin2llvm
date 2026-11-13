#include "NotDecIntrinsics.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/raw_ostream.h"

#include <utility>

namespace notdec::bin2llvm {

std::string llvmTypeName(llvm::Type *type) {
  std::string result;
  llvm::raw_string_ostream os(result);
  if (type != nullptr) {
    type->print(os);
  } else {
    os << "<null>";
  }
  return os.str();
}

llvm::Function *getOrInsertUnknownValueHelper(llvm::Module &module,
                                              llvm::Type *type) {
  std::string name = "notdec.unknown." + llvmTypeName(type);
  llvm::FunctionType *functionType = llvm::FunctionType::get(type, {}, false);
  if (auto *function = module.getFunction(name)) {
    if (function->getFunctionType() == functionType) {
      return function;
    }
  }
  std::string prefix = name + ".";
  for (llvm::Function &function : module.functions()) {
    if (function.getName().starts_with(prefix) &&
        function.getFunctionType() == functionType) {
      return &function;
    }
  }
  if (module.getNamedValue(name) != nullptr) {
    name += ".typed";
  }
  return llvm::Function::Create(functionType, llvm::GlobalValue::ExternalLinkage,
                                name, module);
}

llvm::Value *unknownValueAt(llvm::IRBuilderBase &builder, llvm::Type *type,
                            llvm::Twine name) {
  llvm::BasicBlock *block = builder.GetInsertBlock();
  if (block == nullptr || block->getModule() == nullptr) {
    return llvm::PoisonValue::get(type);
  }
  return unknownValueAt(builder, *block->getModule(), type, std::move(name));
}

llvm::Value *unknownValueAt(llvm::IRBuilderBase &builder, llvm::Module &module,
                            llvm::Type *type, llvm::Twine name) {
  // NOTE: freeze(poison) is *not* usable here.  LLVM is allowed to refine an
  // unconstrained freeze to any concrete value (InstCombine turns a returned
  // freeze(poison) into 0), which would silently replace "unknown register
  // value" with a specific value and drop !notdec.unknown.source.  The opaque
  // helper call is the only representation the optimizer will not fold.
  return builder.CreateCall(getOrInsertUnknownValueHelper(module, type), {},
                            name);
}

} // namespace notdec::bin2llvm
