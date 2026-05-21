#pragma once

#include "notdec-bin2llvm/Pcode.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace llvm {
class LLVMContext;
class Module;
} // namespace llvm

namespace notdec::bin2llvm {

struct PcodeLoweringConfig {
  std::string ModuleName = "notdec.bin2llvm.pcode";
  std::string EntryFunctionName = "notdec_pcode";

  // Address-to-symbol table for already-confirmed functions in the same
  // generated module.  Raw Sleigh P-Code only gives CALL a target address, so
  // the module builder has to provide the symbol name when it knows one.
  std::unordered_map<uint64_t, std::string> DirectCallTargets;

  // Address-to-symbol table for dynamic-linker PLT stubs.  This is separate
  // from DirectCallTargets so a PLT call is not mistaken for a local function
  // even if the stub bytes were decoded earlier.
  std::unordered_map<uint64_t, std::string> ExternalCallTargets;

  // GOT-slot-to-symbol table for guarded indirect external calls such as
  // __gmon_start__.  The lowerer only uses this when a CALLIND input can be
  // traced back to a direct RAM varnode at that GOT address.
  std::unordered_map<uint64_t, std::string> IndirectExternalCallTargets;
};

std::unique_ptr<llvm::Module>
buildPcodeModule(llvm::LLVMContext &context, const PcodeProgram &program,
                 const PcodeLoweringConfig &config, std::string &errorMessage);

bool appendPcodeFunction(llvm::LLVMContext &context, llvm::Module &module,
                         const PcodeProgram &program,
                         const PcodeLoweringConfig &config,
                         std::string &errorMessage);

} // namespace notdec::bin2llvm
