#pragma once

#include "notdec-bin2llvm/core/NativePipelineSpec.h"

#include "llvm/ADT/ArrayRef.h"

#include <string>

namespace llvm {
class Module;
} // namespace llvm

namespace notdec::bin2llvm {

// 用外部 .ll/.bc 文件里的入口函数体替换目标函数的函数体。
//
// - 目标由 Options.Select 解析（为空时要求入口函数名与模块里的函数同名）。
// - 入口函数签名必须与目标完全一致。
// - 文件里除入口外的函数/全局会按名字导入：目标里已存在则复用（不替换其
//   函数体），不存在则按 allow_undefined 决定补声明还是报错。
// - 每个被替换的函数挂 !notdec.override（文件路径 + sha256）。
bool applyNativeIROverride(
    llvm::Module &Module, const NativeIROverrideOptions &Options,
    llvm::ArrayRef<NativeFunctionIdentity> Identities, std::string &Error);

} // namespace notdec::bin2llvm
