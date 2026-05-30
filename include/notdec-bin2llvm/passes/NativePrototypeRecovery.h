#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class Function;
class FunctionType;
class LLVMContext;
class LoadInst;
class Module;
class raw_ostream;
class StoreInst;
class Value;
} // namespace llvm

namespace notdec::bin2llvm {

struct NativePrototypeRecoveryOptions {
  bool PrintSummary = false;
  bool RewriteSignatures = false;
};

// Native copy of Ghidra ParamTrial for the first input-recovery step.  It only
// models register inputs for now, but keeps ABI slot and active state so later
// work can add no-use, joining, and return candidates without changing the
// metadata shape.
struct NativeParamTrial {
  std::string RegisterName;
  uint64_t Slot = 0;
  // Optional simple value identity used by return recovery.  It is intentionally
  // small: unknown values are left unset so this first pass stays conservative.
  std::optional<std::string> ValueKey;
  // In-memory SSA value identity for return recovery.  This is not serialized
  // into metadata; it only lets one recovery run compare PHI/MULTIEQUAL values.
  llvm::Value *Value = nullptr;
  // Store that defined Value when the trial comes from an output register write.
  // It is kept in memory so rewrite binding can delete only real return stores.
  llvm::StoreInst *Store = nullptr;
  bool Active = false;
};

// Small container matching Ghidra ParamActive's role: keep the trials currently
// being considered for one function.
struct NativeParamActive {
  std::vector<NativeParamTrial> Trials;
};

// Minimal native copy of Ghidra FuncProto's recovered storage list.  This does
// not rewrite LLVM function types yet; it records the ordered storage chosen
// from input/output trials so the later signature rewrite has one stable source.
struct NativeRecoveredPrototypeParam {
  std::string RegisterName;
  uint64_t Slot = 0;
};

struct NativeRecoveredPrototype {
  std::string ModelName;
  std::vector<NativeRecoveredPrototypeParam> Inputs;
  std::vector<NativeRecoveredPrototypeParam> Returns;
};

// Gate before the later IR rewrite step.  It keeps the reason next to the
// decision so tests and CLI reporting can distinguish "no prototype" from
// "prototype exists but this native subset cannot rewrite it yet".
struct NativePrototypeRewriteEligibility {
  bool Eligible = false;
  bool NeedsRewrite = false;
  std::string Reason;
  llvm::FunctionType *RecoveredType = nullptr;
};

// Read-only bridge from recovered input storage to the SSA load that currently
// represents the function-entry register value.  Later signature rewriting can
// replace the load uses with the matching LLVM argument.
struct NativePrototypeInputBinding {
  NativeRecoveredPrototypeParam Param;
  llvm::LoadInst *ExternalInputLoad = nullptr;
};

// Read-only bridge from recovered return storage to the register store that
// currently carries the return value.  Later signature rewriting can move the
// store value into LLVM ret instructions.
struct NativePrototypeReturnBinding {
  NativeRecoveredPrototypeParam Param;
  llvm::StoreInst *ReturnStore = nullptr;
  std::vector<llvm::StoreInst *> ReturnStores;
  llvm::Value *ReturnValue = nullptr;
};

struct NativePrototypeRewriteResult {
  bool Rewritten = false;
  std::string Reason;
  llvm::Function *Function = nullptr;
};

// Module-level rewrite statistics.  Skip reasons make the conservative rewrite
// boundary visible while callsite rewrite is still not implemented.
struct NativePrototypeModuleRewriteSummary {
  uint64_t FunctionsSeen = 0;
  uint64_t FunctionsRewritten = 0;
  uint64_t FunctionsSkipped = 0;
  std::map<std::string, uint64_t> SkippedByReason;
};

struct NativePrototypeRecoveryFunctionSummary {
  std::string FunctionName;
  uint64_t ExternalInputsSeen = 0;
  uint64_t InputCandidates = 0;
  uint64_t ReturnCandidates = 0;
  bool RewriteEligible = false;
  bool NeedsSignatureRewrite = false;
};

struct NativePrototypeRecoverySummary {
  uint64_t FunctionsSeen = 0;
  uint64_t ExternalInputsSeen = 0;
  uint64_t InputCandidates = 0;
  uint64_t ReturnCandidates = 0;
  uint64_t RewriteEligibleFunctions = 0;
  uint64_t SignatureRewriteNeededFunctions = 0;
  uint64_t SignatureRewriteFunctionsSeen = 0;
  uint64_t SignatureRewriteFunctionsRewritten = 0;
  uint64_t SignatureRewriteFunctionsSkipped = 0;
  std::map<std::string, uint64_t> SignatureRewriteSkippedByReason;
  std::vector<NativePrototypeRecoveryFunctionSummary> Functions;
};

NativePrototypeRecoverySummary
runNativePrototypeRecovery(llvm::Module &module,
                           const NativePrototypeRecoveryOptions &options = {});

std::optional<NativeRecoveredPrototype>
readNativeRecoveredPrototypeMetadata(const llvm::Function &function);

std::optional<llvm::FunctionType *> buildNativeRecoveredPrototypeFunctionType(
    llvm::LLVMContext &context, const NativeRecoveredPrototype &prototype);

NativePrototypeRewriteEligibility
getNativePrototypeRewriteEligibility(const llvm::Function &function);

std::optional<std::vector<NativePrototypeInputBinding>>
getNativePrototypeInputBindings(llvm::Function &function);

std::optional<std::vector<NativePrototypeReturnBinding>>
getNativePrototypeReturnBindings(llvm::Function &function);

NativePrototypeRewriteResult
rewriteNativeRecoveredPrototypeReturnOnly(llvm::Function &function);

NativePrototypeRewriteResult
rewriteNativeRecoveredPrototypeInputOnly(llvm::Function &function);

NativePrototypeRewriteResult
rewriteNativeRecoveredPrototypeInputReturn(llvm::Function &function);

NativePrototypeRewriteResult
rewriteNativeRecoveredPrototype(llvm::Function &function);

NativePrototypeModuleRewriteSummary
rewriteNativeRecoveredPrototypes(llvm::Module &module);

void printNativePrototypeRecoverySummary(
    const NativePrototypeRecoverySummary &summary, llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
