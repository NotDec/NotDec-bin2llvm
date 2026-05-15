#pragma once

#include "notdec-bin2llvm/HeritagePcode.h"

#include <memory>
#include <string>

namespace llvm {
class LLVMContext;
class Module;
} // namespace llvm

namespace notdec::bin2llvm {

struct HeritageLoweringConfig {
  std::string ModuleName = "notdec.bin2llvm.heritage";
};

std::unique_ptr<llvm::Module>
buildHeritageModule(llvm::LLVMContext &context, const HeritageProgram &program,
                    const HeritageLoweringConfig &config,
                    std::string &errorMessage);

std::unique_ptr<llvm::Module> buildHeritageDeclarationModule(
    llvm::LLVMContext &context, const HeritageModule &module,
    const HeritageLoweringConfig &config, std::string &errorMessage);

} // namespace notdec::bin2llvm
