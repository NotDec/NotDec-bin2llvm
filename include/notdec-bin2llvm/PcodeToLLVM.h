#pragma once

#include "notdec-bin2llvm/Pcode.h"

#include "llvm/IR/GlobalValue.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace llvm {
class LLVMContext;
class Module;
} // namespace llvm

namespace notdec::bin2llvm {

enum class PcodeMemoryModel {
  GlobalArray,
  IntToPtr,
};

struct PcodeLoweringConfig {
  std::string ModuleName = "notdec.bin2llvm.pcode";
  std::string EntryFunctionName = "notdec_pcode";
  std::optional<uint64_t> EntryAddress;
  llvm::GlobalValue::LinkageTypes EntryFunctionLinkage =
      llvm::GlobalValue::ExternalLinkage;

  // GlobalArray keeps the old synthetic @notdec_ram object.  IntToPtr maps
  // P-Code RAM addresses to real LLVM pointers, which is better for native ELF
  // modules whose addresses should line up with debug info and relocations.
  PcodeMemoryModel MemoryModel = PcodeMemoryModel::GlobalArray;

  // Address-to-symbol table for already-confirmed functions in the same
  // generated module.  Raw Sleigh P-Code only gives CALL a target address, so
  // the module builder has to provide the symbol name when it knows one.
  std::unordered_map<uint64_t, std::string> DirectCallTargets;

  // Address-to-symbol table for dynamic-linker PLT stubs.  This is separate
  // from DirectCallTargets so a PLT call is not mistaken for a local function
  // even if the stub bytes were decoded earlier.
  std::unordered_map<uint64_t, std::string> ExternalCallTargets;

  // GOT-slot-to-symbol table for guarded indirect external calls or tail jumps
  // such as __gmon_start__ and _ITM_deregisterTMCloneTable.  The lowerer only
  // uses this when a CALLIND/BRANCHIND input can be traced back to a direct RAM
  // varnode at that GOT address.
  std::unordered_map<uint64_t, std::string> IndirectExternalCallTargets;

  // Optional native block facts keyed by block start.  Native sparse functions
  // can have cold blocks far away from the entry, so contiguous P-Code order is
  // not always a valid fallthrough relation.  Ranges name the accepted block
  // body; successors name the accepted CFG edges.
  std::unordered_map<uint64_t, uint64_t> BlockRanges;
  std::unordered_map<uint64_t, std::vector<uint64_t>> BlockSuccessors;
};

std::unique_ptr<llvm::Module>
buildPcodeModule(llvm::LLVMContext &context, const PcodeProgram &program,
                 const PcodeLoweringConfig &config, std::string &errorMessage);

bool appendPcodeFunction(llvm::LLVMContext &context, llvm::Module &module,
                         const PcodeProgram &program,
                         const PcodeLoweringConfig &config,
                         std::string &errorMessage);

} // namespace notdec::bin2llvm
