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
  std::string StackPointerRegister;
  // Only registers whose complete semantics are handled by the stack rewrite
  // belong here.  A frame-register load can be rewritten locally without
  // making that register ignorable in unrelated code.
  std::set<std::string> IgnoredRegisters;
};

struct NativeStackFrameCleanupOptions {
  bool PrintSummary = false;
  std::string StackPointerRegister;
  std::set<std::string> Registers;
};

struct NativeStackFrameCleanupSummary {
  uint64_t FunctionsSeen = 0;
  uint64_t AccessesRewritten = 0;
  uint64_t FramePointerLoadsReplaced = 0;
  uint64_t RegisterLoadsRemoved = 0;
  uint64_t RegisterStoresRemoved = 0;
  uint64_t StackAllocaLoadsRemoved = 0;
  uint64_t StackAllocaStoresRemoved = 0;
  uint64_t StackAllocasRemoved = 0;
};

// Summary chain stack handling.  It only localizes fixed negative offsets from
// the ABI stack pointer.  It does not model caller stack arguments, aliases, or
// general memory.
NativeStackFrameRewriteSummary runNativeStackFrameRewrite(
    llvm::Module &module, const NativeStackFrameRewriteOptions &options = {});

NativeStackFrameCleanupSummary runNativeStackFrameCleanup(
    llvm::Module &module, const NativeStackFrameCleanupOptions &options = {});

void printNativeStackFrameRewriteSummary(
    const NativeStackFrameRewriteSummary &summary, llvm::raw_ostream &os);

void printNativeStackFrameCleanupSummary(
    const NativeStackFrameCleanupSummary &summary, llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
