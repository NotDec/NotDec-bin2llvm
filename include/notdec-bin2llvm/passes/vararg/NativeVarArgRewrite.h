#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class Function;
class Module;
class raw_ostream;
} // namespace llvm

namespace notdec::bin2llvm {

struct NativeVarArgRewriteOptions {
  // false = 只识别 + 挂 metadata（用于诊断/单测），不做任何 IR 改写。
  bool RewriteVarArgs = true;
  bool AttachMetadata = true;
};

// 编译器生成的 x86-64 SysV 变参序言布局。语义见
// logs/20260919-01-native-vararg-recovery-plan.md：
//   - GP/FP 保存区：GP 6*8=48B（RDI..R9 顺序），FP 8*16=128B；
//   - __va_list_tag：i32 gp_offset、i32 fp_offset、ptr overflow_arg_area、
//     ptr reg_save_area；
//   - gp_offset = 8*固定 GP 参数个数；fp_offset = 48 + 16*固定 FP 参数个数。
struct NativeVarArgInfo {
  uint64_t FixedGp = 0;
  uint64_t FixedFp = 0;
  uint64_t SaveAreaOffset = 0;
  uint64_t VaListOffset = 0;
  // "R8,R9" 这类，便于日志/测试直接比对。
  std::string SavedRegisters;
};

struct NativeVarArgRewriteSummary {
  uint64_t FunctionsSeen = 0;
  uint64_t FunctionsDetected = 0;
  uint64_t FunctionsRewritten = 0;
  uint64_t RegisterSavesRemoved = 0;
  uint64_t VaListFieldStoresRemoved = 0;
  uint64_t VaStartCallsInserted = 0;
  // 命中的函数名（探测顺序），供 CLI/单测核对。
  std::vector<std::string> DetectedFunctions;
  std::vector<std::string> RewrittenFunctions;
  // 识别到但放弃改写的原因（保守跳过时记录，便于审计）。
  std::vector<std::string> SkippedFunctions;
};

// 识别 lift 里编译器生成的变参序言；识别成功后（RewriteVarArgs=true）把
// 手写保存区/手写 va_list 初始化换成 llvm.va_start，并挂 !notdec.vararg
// 让签名推断知道这个函数是变参的（固定参数 = 仍然被直接读的参数寄存器）。
NativeVarArgRewriteSummary
runNativeVarArgRewrite(llvm::Module &module,
                       const NativeVarArgRewriteOptions &options = {});

// 签名推断只看这个 metadata：存在即说明该函数应当重写成变参签名 (...)。
bool isNativeVarArgFunction(const llvm::Function &function);
std::optional<NativeVarArgInfo> nativeVarArgInfo(const llvm::Function &function);

void printNativeVarArgRewriteSummary(const NativeVarArgRewriteSummary &summary,
                                     llvm::raw_ostream &os);

// Stage 2：把"自己取变参"的手写菱形（gp_offset/fp_offset 比较 + 保存区/溢出区
// 双路径 + 偏移推进，见 X86ISelLowering.cpp LowerVAARG）折成 va_arg 指令。
// 纯局部变换，不改签名/调用点；匹配不上就跳过。返回折掉的站点数。
// 放在 summary SSA 与签名重写之后调用：那时 lift 的寄存器中转已经被展开，
// 菱形是干净的 SSA 形态。
uint64_t foldManualVaArgSequences(llvm::Module &module);

} // namespace notdec::bin2llvm
