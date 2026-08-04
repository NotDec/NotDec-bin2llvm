# 20260804-07 native x87 fstenv/fldenv 折叠

## 原始 prompt

```text
详细解释一下这个后续项
（上一轮的解释确认后）按这个做一下吧
```

## 背景

上一轮（06，fnstcw/fldcw/fabs/fchs）之后，`fstenv/fldenv` 是"FPU 状态泄漏到 LLVM 层"剩下的唯一口子：fldcw/fnstsw/fcomi 折叠后，control/status word 的读写全进库，LLVM 层的 `@FPUControlWord/@FPUStatusWord/@FPUTagWord` 全局从此没人写。libavutil 的 `av_assert0_fpu`（0xa29d8）用 `fstenv (%rsp)` 后读 `[rsp+8]` 做分支，提升出的 IR 是入口读未初始化全局（`%FPUTagWord.entry = load i16, ptr @FPUTagWord`），分支在随机值上。

## 目标

- `FSTENV/FNSTENV` → `notdec.x87.fstenv(ptr)`，void：库把内部 FPU 环境按真实 x86 布局写 `ptr`，组内 6 个按字段的 STORE 全部抑制。
- `FLDENV` → `notdec.x87.fldenv(ptr)`，void：库从 `ptr` 读环境更新内部状态（恢复 control word 会决定后续 fistp 的舍入模式），组内 6 个 LOAD 和寄存器写全部抑制。
- libavutil 提升后 `av_assert0_fpu` 不再出现任何 FPU 环境全局。

## 关键事实（实测）

64 位模式下 `fstenv` 写 28 字节，真实布局：control(+0)、status(+2)、tag(+4)、IP offset(+6, 4B)、IP selector(+10)、data offset(+12, 4B)、data selector(+16)、opcode(+18)、reserved(+20)。本机实测 `IP offset = 0xffffffff`（64 位模式 CPU 不记录有效 IP），即 **+8 = 0xffff**。`av_assert0_fpu` 读 +8 取反后 `test $0x3`，只有 +8 == 0xffff 才走正常返回路径——库写 IP offset 时必须写 `0xffffffff`，写 0 会让该函数走 abort 分支。Ghidra 的模拟布局（status +4、tag +8、8 字节指针）与真实布局错位，folding 后以真实布局为准，不能照抄 Ghidra 偏移。

## 技术路线

1. `lib/NativeX87Intrinsic.cpp/.h`：`getOrInsertNativeX87Intrinsic()` 加 `bool accessesMemory` 参数。默认仍 `inaccessibleMemOnly(ModRef)`（库不碰普通内存），fstenv/fldenv 传 true 时 `MemoryEffects::unknown()`（任意内存读+写），否则 call 会被当成不碰内存，库写/读 ptr 是 UB。
2. `lib/PcodeToLLVM.cpp`：
   - `isFpuWordVarnode()`（0x10a0/0x10a2）泛化为 `isFpuEnvVarnode()`，覆盖整个 FPU 环境寄存器区 0x10a0..0x10b8（control/status/tag/opcode 各 2 字节 + data/instruction pointer 各 8 字节）。
   - `X87IntrinsicSpec` 加 `SuppressLoads`（fldenv 抑制组内 LOAD）和 `AccessesMemory`。
   - `toIntrinsicArg()` 加 `IntToPtr`（地址 i64 → ptr 参数）。
   - `classifyX87ByMnemonic()` 加 FSTENV/FNSTENV/FLDENV 分支（在 `touchesX87Stack` 检查前）：基准地址 = 组内第一个"地址不是组内任何 op 产出"的 STORE（fstenv）/LOAD（fldenv）的地址，即 +0 字段的地址表达式（实测就是 RSP 本身），其余 5 个偏移都是它派生。intrinsic 参数就是这个地址，类型 `ptr`。
   - `lowerX87Group()`：`SuppressLoads` 时跳过 `Load`；call 声明传 `AccessesMemory`。
3. 单测：`testX87FstenvFoldsToIntrinsicCall`（6 个 STORE + RSP/INT_ADD 地址）、`testX87FldenvFoldsToIntrinsicCall`（6 个 LOAD + 寄存器写），断言 fold 成 call、无 FPU 环境全局、verifier 通过。

## 判断标准

- libavutil 0xa29c0：`call void @notdec.x87.fstenv(ptr ...)`，无 `@FPUControlWord/@FPUStatusWord/@FPUTagWord` 全局，分支从栈 +8 读库写的值，`llvm-as`+verify 通过。
- 单测、`ctest -R native` 15/15、fortune i386 回归、wrk 全量无告警。

## 风险与不做什么

- 库契约：fstenv 写 control(+0)/status(+2)/tag(+4)（库内部状态）+ IP offset `0xffffffff`(+6)/selector `0xffff`(+10)，其余 data offset/selector/opcode/reserved 写 0。IP/DP/opcode 无法精确模拟（真实 CPU 记录最后一条 x87 指令地址），正常代码不用它们，但 `av_assert0_fpu` 依赖 +8==0xffff。
- `fsave/fnsave/frstor/finit` 涉及 ST 寄存器整组进出内存和复位，押后。
- heritage JSON 路径（无 mnemonic/Body）不动。

## 实现（已完成）

改动文件与函数：

- `lib/NativeX87Intrinsic.cpp/.h`：`getOrInsertNativeX87Intrinsic()` 加 `bool accessesMemory = false`；true 时 `setMemoryEffects(MemoryEffects::unknown())`，注释说明 fstenv/fldenv 传指针给库。
- `lib/PcodeToLLVM.cpp`：
  - `isFpuEnvVarnode()`（约 154 行）：替代 `isFpuWordVarnode()`，范围 0x10a0..0x10b8。
  - `X87IntrinsicSpec`（约 470 行）：加 `SuppressLoads`、`AccessesMemory`。
  - `classifyX87ByMnemonic()`：FSTENV/FNSTENV 分支（约 975 行）和 FLDENV 分支（约 1000 行），基准地址取组内地址非组内产出的 STORE/LOAD。
  - `toIntrinsicArg()`（约 1530 行）：`targetType->isPointerTy()` 时 `CreateIntToPtr`。
  - `lowerX87Group()`（约 1540 行）：`SuppressLoads` 跳过 `Load`；`getOrInsertNativeX87Intrinsic` 传 `AccessesMemory`。
- `tests/pcode_to_llvm_test.cpp`：`addX87FstenvOps()`/`testX87FstenvFoldsToIntrinsicCall`、`addX87FldenvOps()`/`testX87FldenvFoldsToIntrinsicCall`（6 个 STORE/LOAD，偏移 0/4/8/0xc/0x12/0x14，第一个地址直接是 RSP）。

验证：

- libavutil 0xa29c0（`av_assert0_fpu`）：`call void @notdec.x87.fstenv(ptr nonnull %notdec_stack.native)`，`load i16, ptr %stack+8` + `xor/and/icmp` 分支基于库写值；无 `@FPUControlWord/@FPUStatusWord/@FPUTagWord`；`llvm-as`+verify 通过。
- `./build/bin/pcode_to_llvm_test` 通过（新增 fstenv/fldenv 两条）。
- `ctest --test-dir build -R native` 15/15；wrk 全量 0 未折叠告警、134 个 `notdec.x87.*` call、verify 通过。
