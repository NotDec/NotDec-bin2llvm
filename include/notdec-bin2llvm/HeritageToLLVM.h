#pragma once

#include "notdec-bin2llvm/HeritagePcode.h"

#include <memory>
#include <string>
#include <vector>

namespace llvm {
class LLVMContext;
class Module;
} // namespace llvm

namespace notdec::bin2llvm {

struct HeritageLoweringConfig {
  std::string ModuleName = "notdec.bin2llvm.heritage";
};

struct HeritageModuleLoweringFailure {
  std::string FunctionName;
  std::string Entry;
  std::string Message;
};

struct HeritageModuleLoweringStats {
  unsigned DeclaredInternalFunctions = 0;
  unsigned DeclaredExternalFunctions = 0;
  unsigned LoweredFunctions = 0;
  std::vector<HeritageModuleLoweringFailure> Failures;
};

std::unique_ptr<llvm::Module>
buildHeritageModule(llvm::LLVMContext &context, const HeritageProgram &program,
                    const HeritageLoweringConfig &config,
                    std::string &errorMessage);

std::unique_ptr<llvm::Module> buildHeritageDeclarationModule(
    llvm::LLVMContext &context, const HeritageModule &module,
    const HeritageLoweringConfig &config, std::string &errorMessage);

std::unique_ptr<llvm::Module> buildHeritageModuleWithBodies(
    llvm::LLVMContext &context, const HeritageModule &module,
    const HeritageLoweringConfig &config, HeritageModuleLoweringStats &stats,
    std::string &errorMessage);

} // namespace notdec::bin2llvm
