#pragma once

#include "notdec-bin2llvm/Pcode.h"

#include <memory>
#include <string>

namespace llvm {
class LLVMContext;
class Module;
} // namespace llvm

namespace notdec::bin2llvm {

struct PcodeLoweringConfig {
  std::string ModuleName = "notdec.bin2llvm.pcode";
  std::string EntryFunctionName = "notdec_pcode";
};

std::unique_ptr<llvm::Module>
buildPcodeModule(llvm::LLVMContext &context, const PcodeProgram &program,
                 const PcodeLoweringConfig &config, std::string &errorMessage);

bool appendPcodeFunction(llvm::LLVMContext &context, llvm::Module &module,
                         const PcodeProgram &program,
                         const PcodeLoweringConfig &config,
                         std::string &errorMessage);

} // namespace notdec::bin2llvm
