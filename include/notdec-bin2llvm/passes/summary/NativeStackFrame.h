#pragma once

#include <cstdint>
#include <set>
#include <string>

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace notdec::bin2llvm {

struct NativeStackFrameRewriteOptions {
  bool PrintSummary = false;
};

struct NativeStackFrameRewriteSummary {
  uint64_t FunctionsSeen = 0;
  uint64_t FunctionsRewritten = 0;
  uint64_t AccessesRewritten = 0;
  uint64_t FramePointerLoadsReplaced = 0;
  std::set<std::string> IgnoredRegisters;
};

// Summary chain stack handling.  It only localizes fixed negative offsets from
// the ABI stack pointer.  It does not model caller stack arguments, aliases, or
// general memory.
NativeStackFrameRewriteSummary runNativeStackFrameRewrite(
    llvm::Module &module, const NativeStackFrameRewriteOptions &options = {});

void printNativeStackFrameRewriteSummary(
    const NativeStackFrameRewriteSummary &summary, llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
