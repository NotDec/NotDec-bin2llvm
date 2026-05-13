#pragma once

#include <memory>
#include <string>

namespace llvm {
class LLVMContext;
class Module;
}  // namespace llvm

namespace notdec::bin2llvm {

// First-stage lowering stays intentionally small: one config object, one entry
// point, and no fake abstraction for p-code yet.
struct BuildConfig {
  std::string ModuleName = "notdec.bin2llvm";
  std::string EntryFunctionName = "notdec_stub";
};

std::unique_ptr<llvm::Module> buildDemoModule(
    llvm::LLVMContext &context, const BuildConfig &config);

}  // namespace notdec::bin2llvm
