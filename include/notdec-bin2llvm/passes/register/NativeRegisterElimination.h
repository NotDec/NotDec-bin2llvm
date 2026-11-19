#pragma once

#include "notdec-bin2llvm/abi/NativeExternalPrototype.h"

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

struct NativeRegisterEliminationOptions {
  bool EnableRewrite = true;
  bool EnableResidueRemoval = true;
  // Signature rewrite can expose new dead register stores only after LLVM has
  // folded the now-local dataflow.  Keep this behind an option so
  // --no-instcombine-pass can still disable all InstCombine work.
  bool EnablePostRewriteInstCombine = true;
  bool AttachMetadata = true;
  // 识别编译器生成的变参序言并恢复成 llvm.va_start + 变参签名（默认开）。
  bool EnableVarArgRecovery = true;
  // 寄存器感知 DCE（默认关）：需求定点后把"写了没人读"的寄存器写的值换成 0，
  // 删掉死掉的计算链，再重跑一遍 summary。
  // 关掉的原因：需求的死写判定只对"签名要不要这几位"负责，还没证明对"删除值"
  // 是 sound 的（hexx64/fortune 上已经抓到 noreturn 调用的实参被误判成死写，
  // 修掉之后仍有 callee-saved 返回槽上的差异）。要打开需要先把需求分析做成
  // 严格的上界，见 logs/20261120-01-native-demand-dce-plan.md。
  bool EnableValueDce = false;
  // 是否要求所有函数都遵守外部 ABI（参数和返回都用 ABI 寄存器）。
  // 默认 false：本地链接、地址没被取的函数属于"接口可改"，参数和返回都可以用自定义
  // 约定（例如把 caller-saved 的 RCX/R8 也当返回通道）。某些编译配置下所有函数都遵守
  // ABI，这时打开该开关，产物形状与打开前一致。
  bool AbiForAllFunctions = false;
  // 按函数名强制调用约定（用户 JSON `functions[].abi`）：external = 参数/返回只
  // 用外部 ABI 寄存器（不再假设所有寄存器都能当返回通道）；internal = 允许自定义
  // 约定。集合里的名字优先于 AbiForAllFunctions。
  std::set<std::string> ForcedExternalAbiFunctions;
  std::set<std::string> ForcedInternalAbiFunctions;
  // 按函数名钉死的输入/输出寄存器（用户 JSON `functions[].signature`）。
  std::map<std::string, NativeSignaturePin> PinnedSignatures;
  bool PrintSummary = false;
  std::string ExternalPrototypeJsonPath;
  std::set<std::string> IgnoredRegisters;
};

struct NativeRegisterEliminationFunctionSummary {
  std::string FunctionName;
  uint64_t LoadsSeen = 0;
  uint64_t StoresSeen = 0;
  uint64_t LoadsReplaced = 0;
  uint64_t DeadLoadsRemoved = 0;
  uint64_t DeadStoresRemoved = 0;
  uint64_t PhisCreated = 0;
  uint64_t PhisSimplified = 0;
  uint64_t RangeEntryInputs = 0;
  uint64_t EntryRangeRejectedReads = 0;
  uint64_t CallReturnValues = 0;
  uint64_t CallClobberValues = 0;
  // 调用点补的"ABI 未定义位"值（!notdec.abi_undefined）。和 unknown 分开计，
  // 它们不是建模缺口，而是二进制里本来就没有约定的位。
  uint64_t AbiUndefinedReturnValues = 0;
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
struct NativeRegisterEliminationWarning {
  std::string FunctionName;
  std::string CalleeName;
  std::string RegisterName;
  uint64_t RangeBitOffset = 0;
  uint64_t RangeBitWidth = 0;
  std::string Kind;
  std::string Reason;
  uint64_t Uses = 0;
};

struct NativeRegisterEliminationSummary {
  uint64_t FunctionsSeen = 0;
  uint64_t LoadsSeen = 0;
  uint64_t StoresSeen = 0;
  uint64_t LoadsReplaced = 0;
  uint64_t DeadLoadsRemoved = 0;
  uint64_t DeadStoresRemoved = 0;
  uint64_t PhisCreated = 0;
  uint64_t PhisSimplified = 0;
  uint64_t RangeEntryInputs = 0;
  uint64_t EntryRangeRejectedReads = 0;
  uint64_t CallReturnValues = 0;
  uint64_t CallClobberValues = 0;
  // 调用点补的"ABI 未定义位"值（!notdec.abi_undefined）。和 unknown 分开计，
  // 它们不是建模缺口，而是二进制里本来就没有约定的位。
  uint64_t AbiUndefinedReturnValues = 0;
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
  std::vector<NativeRegisterEliminationFunctionSummary> Functions;
  std::vector<NativeRegisterEliminationWarning> Warnings;
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
  // ABI 未定义位（!notdec.abi_undefined）单独计数：它们不是"建模缺口"，而是
  // 二进制里本来就没有约定值的位（例如 double 返回后 XMM0 的高半）。
  // UnknownValuesByReason 只统计真正的未知，方便按 I2 不变量收敛。
  uint64_t AbiUndefinedValues = 0;
  std::map<std::string, uint64_t> AbiUndefinedByCallee;
  std::map<std::string, uint64_t> UnknownValuesByReason;
};

NativeRegisterValueSourceAudit
auditNativeRegisterValueSources(llvm::Module &module,
                                bool abiForAllFunctions = false);

NativeRegisterEliminationSummary runNativeRegisterElimination(
    llvm::Module &module, const NativeRegisterEliminationOptions &options = {});

void printNativeRegisterEliminationSummary(
    const NativeRegisterEliminationSummary &summary, llvm::raw_ostream &os);

} // namespace notdec::bin2llvm
