#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace notdec::bin2llvm {

struct NativeRegisterSSAOptions {
  bool EnableRewrite = true;
  bool PrintSummary = false;
};

struct NativeRegisterSSAFunctionSummary {
  std::string FunctionName;
  uint64_t LoadsSeen = 0;
  uint64_t StoresSeen = 0;
  uint64_t LoadsReplaced = 0;
  uint64_t PhisCreated = 0;
  uint64_t PhisSimplified = 0;
  uint64_t ExternalInputs = 0;
  uint64_t CallsSeen = 0;
  uint64_t PreservedRegisters = 0;
  uint64_t ClobberedRegisters = 0;
};

struct NativeRegisterSSASummary {
  uint64_t FunctionsSeen = 0;
  uint64_t LoadsSeen = 0;
  uint64_t StoresSeen = 0;
  uint64_t LoadsReplaced = 0;
  uint64_t PhisCreated = 0;
  uint64_t PhisSimplified = 0;
  uint64_t ExternalInputs = 0;
  uint64_t CallsSeen = 0;
  uint64_t PreservedRegisters = 0;
  uint64_t ClobberedRegisters = 0;
  std::vector<NativeRegisterSSAFunctionSummary> Functions;
};

// This pass only promotes full-width register globals tagged by RegisterStorage.
// Partial register aliases stay as memory operations until alias semantics are
// modeled explicitly.
NativeRegisterSSASummary
runNativeRegisterSSA(llvm::Module &module,
                     const NativeRegisterSSAOptions &options = {});

void printNativeRegisterSSASummary(const NativeRegisterSSASummary &summary,
                                   llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
