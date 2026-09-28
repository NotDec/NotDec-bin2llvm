#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace notdec::bin2llvm {

struct NativeRegisterSummarySSAOptions {
  bool EnableRewrite = true;
  bool EnableResidueRemoval = true;
  // Signature rewrite can expose new dead register stores only after LLVM has
  // folded the now-local dataflow.  Keep this behind an option so
  // --no-instcombine-pass can still disable all InstCombine work.
  bool EnablePostRewriteInstCombine = true;
  bool AttachMetadata = true;
  // 识别编译器生成的变参序言并恢复成 llvm.va_start + 变参签名（默认开）。
  bool EnableVarArgRecovery = true;
  bool PrintSummary = false;
  std::string ExternalPrototypeJsonPath;
  std::set<std::string> IgnoredRegisters;
};

struct NativeRegisterSummarySSAFunctionSummary {
  std::string FunctionName;
  uint64_t LoadsSeen = 0;
  uint64_t StoresSeen = 0;
  uint64_t LoadsReplaced = 0;
  uint64_t DeadLoadsRemoved = 0;
  uint64_t DeadStoresRemoved = 0;
  uint64_t PhisCreated = 0;
  uint64_t PhisSimplified = 0;
  uint64_t RangeEntryInputs = 0;
  uint64_t CallReturnValues = 0;
  uint64_t CallClobberValues = 0;
  uint64_t CallArgStoresMarked = 0;
  uint64_t CallsRewritten = 0;
  uint64_t FunctionsRewritten = 0;
  uint64_t PreservedCalls = 0;
  uint64_t UnknownCallEffects = 0;
  uint64_t StackFrameAccessesRewritten = 0;
  uint64_t StackFramePointerLoadsReplaced = 0;
  uint64_t StackFrameRegisterLoadsRemoved = 0;
  uint64_t StackFrameRegisterStoresRemoved = 0;
  uint64_t StackFrameAllocaLoadsRemoved = 0;
  uint64_t StackFrameAllocaStoresRemoved = 0;
  uint64_t StackFrameAllocasRemoved = 0;
  uint64_t PartialDemandCandidates = 0;
  uint64_t PartialDemandMatched = 0;
  uint64_t PartialDemandRejected = 0;
  uint64_t RangeRegistersPlanned = 0;
  uint64_t RangeSegmentsPlanned = 0;
  uint64_t RangeReadEvents = 0;
  uint64_t RangeWriteEvents = 0;
  uint64_t RangeClobberEvents = 0;
};

// Leftover call-value helpers are kept in the IR when SummarySSA cannot prove a
// concrete register value.  Emit them as diagnostics so missing prototypes or
// suspicious clobber uses are visible in batch runs.
struct NativeRegisterSummarySSAWarning {
  std::string FunctionName;
  std::string CalleeName;
  std::string RegisterName;
  uint64_t RangeBitOffset = 0;
  uint64_t RangeBitWidth = 0;
  std::string Kind;
  std::string Reason;
  uint64_t Uses = 0;
};

struct NativeRegisterSummarySSASummary {
  uint64_t FunctionsSeen = 0;
  uint64_t LoadsSeen = 0;
  uint64_t StoresSeen = 0;
  uint64_t LoadsReplaced = 0;
  uint64_t DeadLoadsRemoved = 0;
  uint64_t DeadStoresRemoved = 0;
  uint64_t PhisCreated = 0;
  uint64_t PhisSimplified = 0;
  uint64_t RangeEntryInputs = 0;
  uint64_t CallReturnValues = 0;
  uint64_t CallClobberValues = 0;
  uint64_t CallArgStoresMarked = 0;
  uint64_t CallsRewritten = 0;
  uint64_t FunctionsRewritten = 0;
  uint64_t PreservedCalls = 0;
  uint64_t UnknownCallEffects = 0;
  uint64_t StackFrameAccessesRewritten = 0;
  uint64_t StackFramePointerLoadsReplaced = 0;
  uint64_t StackFrameRegisterLoadsRemoved = 0;
  uint64_t StackFrameRegisterStoresRemoved = 0;
  uint64_t StackFrameAllocaLoadsRemoved = 0;
  uint64_t StackFrameAllocaStoresRemoved = 0;
  uint64_t StackFrameAllocasRemoved = 0;
  // NativeVarArgRewrite 的结果：识别到/改写成功的变参函数（签名带 ...）。
  uint64_t VarArgFunctionsDetected = 0;
  uint64_t VarArgFunctionsRewritten = 0;
  std::vector<std::string> VarArgFunctions;
  uint64_t StackCanaryChecksRemoved = 0;
  uint64_t StackCanaryFailBlocksRemoved = 0;
  uint64_t StackCanaryFailFunctionsRemoved = 0;
  uint64_t PartialDemandCandidates = 0;
  uint64_t PartialDemandMatched = 0;
  uint64_t PartialDemandRejected = 0;
  uint64_t RangeRegistersPlanned = 0;
  uint64_t RangeSegmentsPlanned = 0;
  uint64_t RangeReadEvents = 0;
  uint64_t RangeWriteEvents = 0;
  uint64_t RangeClobberEvents = 0;
  std::vector<NativeRegisterSummarySSAFunctionSummary> Functions;
  std::vector<NativeRegisterSummarySSAWarning> Warnings;
};

// 产物审计：把“寄存器值的来源”变成可数的量（见
// logs/20260927-02-native-register-value-source-plan.md）。
// “接口可改”指本地链接且函数地址没被取——这类函数的调用点全部由本 pass 改写，
// 所以它不该再读寄存器 global（不变量 I1）；其余函数是边界，读寄存器是允许的
// （I3），但要单独计数。UnknownValuesByReason 按 !notdec.unknown.source 的 reason
// 分类，用来盯“建模缺口型 unknown”（I2）。
struct NativeRegisterValueSourceAudit {
  uint64_t LocalRegisterReads = 0;
  uint64_t BoundaryRegisterReads = 0;
  uint64_t PartialWriteHelpers = 0;
  uint64_t ClobberPlaceholders = 0;
  std::map<std::string, uint64_t> UnknownValuesByReason;
};

NativeRegisterValueSourceAudit
auditNativeRegisterValueSources(llvm::Module &module);

NativeRegisterSummarySSASummary runNativeRegisterSummarySSA(
    llvm::Module &module, const NativeRegisterSummarySSAOptions &options = {});

void printNativeRegisterSummarySSASummary(
    const NativeRegisterSummarySSASummary &summary, llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
