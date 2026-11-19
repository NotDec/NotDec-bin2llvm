#pragma once

#include "notdec-bin2llvm/abi/NativeExternalPrototype.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace notdec::bin2llvm {

// --- 函数选择器 -------------------------------------------------------------
//
// JSON 里多个字段同时出现表示 AND；空选择器非法（由解析器拒绝）。匹配按最终
// LLVM 函数名进行，`symbol` / `address` 只是 binary 输入下的等价入口。
struct NativeFunctionSelector {
  std::optional<std::string> Name;
  std::optional<std::string> Glob;
  std::optional<std::string> Symbol;
  std::optional<uint64_t> Address;
  // 匹配不到任何函数时降级为 warning，而不是报错。
  bool Optional = false;

  bool empty() const {
    return !Name && !Glob && !Symbol && !Address;
  }
};

// IR 函数与二进制事实的对应关系（binary 输入时由 tool 填充）。选择器里的
// symbol / address 通过它解析；.ll 输入只有 name/glob 可用。
struct NativeFunctionIdentity {
  std::string Name;
  std::string Symbol;
  uint64_t Address = 0;
};

// 函数级属性声明，全部 optional：没写的保持原样。
struct NativeFunctionAttributes {
  std::optional<bool> NoReturn;
  std::optional<bool> NoUnwind;
  std::optional<bool> WillReturn;
  std::optional<bool> NoInline;
  std::optional<bool> AlwaysInline;
  std::optional<bool> Cold;
  // none | read | write | readwrite | argmem | inaccessiblemem
  std::optional<std::string> Memory;

  bool empty() const {
    return !NoReturn && !NoUnwind && !WillReturn && !NoInline &&
           !AlwaysInline && !Cold && !Memory;
  }
};

struct NativeFunctionConfig {
  NativeFunctionSelector Select;
  bool Pin = false;
  NativeFunctionAttributes Attributes;
  // 可选：强制这批函数的调用约定。external = 参数/返回只用外部 ABI 寄存器
  // （不再假设 R8/R9/R10/R11 等也能当返回通道）；internal = 允许自定义约定。
  enum class Abi { Unset, External, Internal };
  Abi AbiConvention = Abi::Unset;
  // 可选：直接钉死输入/输出寄存器，跳过寄存器 SSA 的形状推断。
  std::optional<NativeSignaturePin> Signature;
};

// --- 各 stage 的 options ----------------------------------------------------

struct NativeIROverrideOptions {
  // 配置文件里写的原始路径，用于错误信息和 --print-config 回写。
  std::string File;
  // 解析后的绝对路径与内容 sha256（load 阶段填充）。
  std::string ResolvedFile;
  std::string Sha256;
  std::vector<NativeFunctionSelector> Select;
  std::string Entry;
  // 默认允许导入 override 文件里的新声明/定义（helper、libc 声明等）；
  // 置 false 时要求所有符号在目标 module 里已存在。
  bool AllowUndefined = true;
  bool StripDebug = true;
  bool AttributesFromOverride = false;
};

struct NativeInstCombineOptions {
  bool EarlyCSE = false;
  std::vector<NativeFunctionSelector> Exclude;
};

struct NativeRegisterEliminationConfig {
  bool Rewrite = true;
  bool ResidueRemoval = true;
  bool PostRewriteInstCombine = true;
  bool VarArgRecovery = true;
  bool ValueDce = false;
  bool AbiForAllFunctions = false;
  bool AttachMetadata = true;
  // 原始路径 + 解析后的绝对路径（可能是同一个）。
  std::string ExternalPrototypes;
  std::string ResolvedExternalPrototypes;
  std::string ExternalPrototypesSha256;
  std::vector<std::string> IgnoredRegisters;
};

struct NativeFinalCleanupOptions {
  enum class GlobalDceMode { Auto, Enabled, Disabled };
  GlobalDceMode GlobalDce = GlobalDceMode::Auto;
  bool PreserveAddressTaken = true;
};

enum class NativeStageKind {
  InstCombine,
  RegisterElimination,
  FunctionPointerPromotion,
  PostRewritePeephole,
  IROverride,
  FinalCleanup,
  EraseDeadHelpers,
};

// 一个 stage 的完整类型化描述。所有 kind 的 options 平铺在这里：数量固定、
// 每个 stage 只用其中一组，换取 C++17 下无需 variant/type-erasure 的简单性。
struct NativeStageConfig {
  NativeStageKind Kind = NativeStageKind::InstCombine;
  // 配置里写的名字，用于诊断和 dump 文件名。
  std::string Name;
  NativeInstCombineOptions InstCombine;
  NativeRegisterEliminationConfig RegisterElimination;
  NativeIROverrideOptions IROverride;
  NativeFinalCleanupOptions FinalCleanup;
};

enum class NativePipelineInput { Binary, IR };

struct NativeConfig {
  unsigned SchemaVersion = 1;
  // 配置文件绝对路径；默认配置为空。
  std::string SourcePath;
  // 相对路径的解析基准，始终是绝对路径。
  std::string RootPath;
  std::vector<NativeStageConfig> Pipeline;
  std::vector<NativeFunctionConfig> Functions;
};

// 默认 pipeline（与重构前的硬编码序列一致）。binary 输入包含
// function-pointer-promotion，.ll 输入不包含。
NativeConfig defaultNativeConfig(NativePipelineInput Input);

// 解析并校验配置文件。成功时所有外部文件路径都已解析为绝对路径、算好 sha256。
// 失败时 Error 带 JSON 指针，例如
// "pipeline[1].options.residue_removal: expected bool, got string"。
std::optional<NativeConfig> loadNativeConfig(llvm::StringRef Path,
                                             NativePipelineInput Input,
                                             std::string &Error);

// 解析内存里的配置文本（单测用）。ConfigPath 只用于定位相对路径的基准目录
// 和错误信息，不需要真实存在。
std::optional<NativeConfig> parseNativeConfig(llvm::StringRef Text,
                                              llvm::StringRef ConfigPath,
                                              NativePipelineInput Input,
                                              std::string &Error);

// 打印规范化 JSON（所有字段都写全，可直接回灌给 --config）。
void printNativeConfig(const NativeConfig &Config, llvm::raw_ostream &OS);

// --- stage 注册表 -----------------------------------------------------------

struct NativeStageDescriptor {
  llvm::StringRef Name;
  NativeStageKind Kind;
  bool Repeatable = false;
  bool Filterable = false;
  // 只解析 options 对象，不改 module。校验失败时 Error 带 JSON 指针。
  bool (*ParseOptions)(const llvm::json::Object &Options,
                       llvm::StringRef Path, NativeStageConfig &Stage,
                       std::string &Error);
};

llvm::ArrayRef<NativeStageDescriptor> nativeStageDescriptors();

const NativeStageDescriptor *findNativeStageDescriptor(llvm::StringRef Name);

const NativeStageDescriptor *findNativeStageDescriptor(NativeStageKind Kind);

// 把某个 kind 的默认 options 打印成 JSON 对象（--list-stages 用）。
llvm::json::Object nativeStageOptionsToJson(const NativeStageConfig &Stage);

// 打印某个 stage 的 options（--print-config 用，字段顺序固定）。
void printNativeStageOptions(const NativeStageConfig &Stage,
                             llvm::raw_ostream &OS);

// 规范化配置的 sha256（对 printNativeConfig 的输出取哈希）。
std::string nativeConfigSha256(const NativeConfig &Config);

// 来源清单：{schema_version, sha256, files: [{role, path, sha256}]}。
llvm::json::Object nativeConfigManifest(const NativeConfig &Config);

// 把配置来源挂到 module 的 !notdec.config 上。默认配置（SourcePath 为空）
// 不挂，保证不带 --config 的产物与以前一致。
void attachNativeConfigMetadata(llvm::Module &Module,
                                const NativeConfig &Config);

// --- 选择器匹配 -------------------------------------------------------------

// 多个字段同时出现视为 AND。Identity 为 nullptr 时 symbol/address 条件不匹配。
bool nativeSelectorMatches(const NativeFunctionSelector &Selector,
                           llvm::StringRef FunctionName,
                           const NativeFunctionIdentity *Identity);

// 把选择器解析成 Module 里的函数名集合。IncludeDeclarations 为 false 时只匹配
// 有函数体的定义。任一必需选择器匹配为空时返回 false（Optional 选择器只告警）。
bool resolveNativeFunctionSelectors(
    llvm::Module &Module, llvm::ArrayRef<NativeFunctionIdentity> Identities,
    llvm::ArrayRef<NativeFunctionSelector> Selectors, llvm::StringRef Path,
    bool IncludeDeclarations, std::set<std::string> &Out, std::string &Error);

} // namespace notdec::bin2llvm
