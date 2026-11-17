#pragma once

#include "notdec-bin2llvm/core/NativePipelineSpec.h"
#include "notdec-bin2llvm/passes/register/NativeRegisterElimination.h"

#include <set>
#include <string>
#include <vector>

namespace llvm {
class Module;
} // namespace llvm

namespace notdec::bin2llvm {

class NativeProgramState;

// 一次 pipeline 运行的上下文。Module 是唯一被修改的对象；State 和
// FunctionIdentities 只在 binary 输入时提供，用于把 symbol/address 选择器
// 解析成 IR 函数名（.ll 输入只有 name/glob 选择器可用）。
struct NativePipelineContext {
  llvm::Module &Module;
  const NativeConfig &Config;
  const NativeProgramState *State = nullptr;
  std::vector<NativeFunctionIdentity> FunctionIdentities;
  // pin 的函数 + 地址被取/需要保活的函数。final-cleanup 前用 llvm.used 保活。
  std::set<std::string> PreservedFunctions;
  // 由 Config.Functions 里 pin 解析出的函数名，后续 stage 跳过它们。
  std::set<std::string> PinnedFunctions;
  // 非空时在每个 stage 前后写 <NN>-<name>-{before,after}.ll。
  std::string DumpStagesDir;
  // 校验失败时写 <OutputPath>.<NN>-<name>.bad.ll（OutputPath 非空时）。
  std::string OutputPath;
  bool PrintSummary = false;

  // 由 register-elimination stage 填充：tool 用它写 warning TSV 和末尾审计。
  NativeRegisterEliminationSummary EliminationSummary;
  bool AbiForAllFunctions = false;
};

// 按 Config.Pipeline 顺序执行，每个 stage 后 verify。任何失败都返回 false，
// Error 里带 stage 序号和名字。
bool runNativePipeline(NativePipelineContext &Context, std::string &Error);

} // namespace notdec::bin2llvm
