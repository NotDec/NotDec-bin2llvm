# 20260804-06 native x87 fnstcw/fldcw/fabs/fchs 折叠

## 原始 prompt

```text
不要匹配pcode，最好直接获取底层汇编指令，然后直接生成，跳过指令对应的pcode部分
```

## 背景

上一轮（fprem/fnstsw）后，Bench2 里还有几条 x87 指令没折叠：python 的 `fegetround`（0x608550）用 `fnstcw` 读舍入控制字、`fesetround`（0x608570）用 `fldcw` 写回；redis 的舍入切换序列 `fnstcw; or; fldcw; fistpll; fldcw`；redis fmod 里的 `fabs/fucomi`；php ffi.so 的 `fldt/fchs/fstpt`。未折叠时 `fldcw/fnstcw` 走 `@FPUControlWord` 全局，库内部永远不知道舍入模式被改，后续 `fistpll` 截断语义错。

用户同时提出方向性要求：识别不要匹配 pcode 形状（如找 `COPY(0x10a0)`、`FLOAT_ABS`），直接按底层汇编助记符 + 操作数分类，生成时跳过该指令对应的 pcode 部分。上一轮 FNSTSW 分支还残留 `COPY(FSW)` 形状确认，这一轮一并清掉。

## 目标

- `FNSTCW/FSTCW` → `notdec.x87.fnstcw()` 返回 i16，lowering 写到 pcode 目标（AX 或内存）。
- `FLDCW` → `notdec.x87.fldcw(i16)`，参数是内存 load 值。
- `FABS/FCHS` → `notdec.x87.fabs()` / `notdec.x87.fchs()`，无操作数。
- 顺带修两个 redis fmod 依赖的形态：`FISTP`（Ghidra 是 `round(ST0)->trunc` 而非直接 trunc，现有分支只认 FloatTrunc）和 `FSUBR/FDIVR` 反向寄存器算术（`ST0 = st(i) - ST0` 被误生成 `fsub`）。
- 所有分类只由 Mnemonic（+Body 操作数文本）驱动，pcode 只提供操作数值（内存地址、宽度、st(i) 下标）。

## 技术路线（lib/PcodeToLLVM.cpp）

1. `include/notdec-bin2llvm/Pcode.h`：`PcodeOpView` 加 `Body`（操作数文本，与 Mnemonic 合成完整汇编指令）。`lib/SleighLift.cpp`：`setInstructionMnemonic()` 改 `setInstructionAssembly()`，把 `SleighInstructionSummary` 的 Mnemonic 和 Body 一起挂到该指令产生的所有 op。
2. `isFpuStatusWordVarnode()`（0x10a2）泛化为 `isFpuWordVarnode()`（0x10a0 control word 或 0x10a2 status word，均 2 字节）。
3. `X87IntrinsicSpec`：删 `WritesStatusWord`（抑制改为无条件跳过 FPU 字 op，任何折叠组都不该有 FPU 字全局）。
4. `classifyX87ByMnemonic()`：
   - FNSTSW/FSTSW/FNSTCW/FSTCW 合并分支（在 `touchesX87Stack` 检查前）：Body `"AX"` → `ResultRegister`（从组内写寄存器的 COPY 输出取）；否则内存 → `StoreAddress`（从 STORE 取，宽 2 字节）。
   - `FLDCW`：组内 2 字节 LOAD 的输出作参数。
   - `FABS/FCHS`：直接按 mnemonic 生成，无操作数。
   - `FSTP/FISTP` 简化：mnemonic 决定 intrinsic，STORE 只提供目标地址和存储宽度；不再要求 producer 是 `FloatTrunc`/`FloatFloat2Float`/`Copy`（fistpll 的 `ROUND->TRUNC` 因此自动覆盖，舍入模式由库按 fldcw 设置的 control word 决定）。
   - FSUBR/FDIVR 加入寄存器/内存算术分支；`regArithName()` 加 input0 参数，反向（input0 是 st(i)）生成 `fsubr/fdivr`。
5. `lowerX87Group()`：抑制条件改为无条件跳过 ST 栈寄存器或 FPU 字（0x10a0/0x10a2）的输入/输出。
6. 单测：`testX87FnstcwFldcwFoldsToIntrinsicCalls`（python 风格 fnstcw→STORE + LOAD→fldcw）、`testX87FabsChsFoldsToIntrinsicCalls`、`testX87FsubrSt3FoldsToIntrinsicCall`；既有 fprem/fnstsw 测试的 FNSTSW op 补 `Body="AX"`。

## 判断标准

- python `fegetround` 提升 = `call i16 @notdec.x87.fnstcw()`，`fesetround` = `call void @notdec.x87.fldcw(i16)`。
- redis 舍入序列 = `fnstcw()` → 内存置位 → `fldcw()` → `fistp.i64()` → `fldcw()`，无 unknown、无告警。
- fmod 的 `fabs/fucomi/fsubr` 全部折叠；ffi.so `fchs` 序列折叠为 `fld.f80/fchs/fstp.f80`。
- wrk 全量无未折叠告警、verify 通过；fortune i386/x86_64 回归；`ctest -R native` 15/15。

## 风险与不做什么

- `fstenv`（libavutil×1）仍不折叠，会创建 `@FPUControlWord/@FPUStatusWord` 全局（现状不变，语义仍不可信），单独一轮处理。
- `finit/fclex/fcom` 等写 FPU 字的指令未出现在 manifest 目标里，不实现。
- heritage JSON 路径（`classifyX87ByShape`，无 mnemonic/Body）不动。
- 不实现 `notdec.x87` 库本身（舍入模式、fistp 截断语义由库按 fldcw 状态负责）。

## 实现（已完成）

改动文件与函数：

- `include/notdec-bin2llvm/Pcode.h`：`PcodeOpView` 加 `std::string Body`（约 112 行）。
- `lib/SleighLift.cpp`：`setInstructionAssembly()`（约 269 行）替代 `setInstructionMnemonic()`，`appendInstructionPcode()`（约 297 行）传入整个 summary。
- `lib/PcodeToLLVM.cpp`：
  - `isFpuWordVarnode()`（约 154 行）替代 `isFpuStatusWordVarnode()`，覆盖 0x10a0/0x10a2。
  - `X87IntrinsicSpec`（约 460 行）：删 `WritesStatusWord`，注释改为 fnstsw/fnstcw 共用 `ResultRegister`/`StoreAddress`。
  - `classifyX87ByMnemonic()`：FNSTSW/FSTSW/FNSTCW/FSTCW 合并分支（约 906 行）、FLDCW（约 940 行）、FABS/FCHS（约 961 行）、FSTP/FISTP 简化（约 1050 行）、FSUBR/FDIVR 加入算术分支（约 1213 行）。
  - `regArithName()`（约 244 行）加 input0 参数，反向生成 fsubr/fdivr。
  - `lowerX87Group()`（约 1490 行）：抑制条件无条件跳过 ST/FPU 字 op。
- `tests/pcode_to_llvm_test.cpp`：`addX87FnstcwFldcwOps()`/`testX87FnstcwFldcwFoldsToIntrinsicCalls`、`addX87FabsChsOps()`/`testX87FabsChsFoldsToIntrinsicCalls`、`addX87FsubrSt3Ops()`/`testX87FsubrSt3FoldsToIntrinsicCall`；fprem/fnstsw 测试 FNSTSW op 补 `Body="AX"`。

验证：

- `./build/bin/pcode_to_llvm_test` 通过。
- python3.12：`fegetround`（0x608550）= `call i16 @notdec.x87.fnstcw()`；`fesetround`（0x608570）= `call void @notdec.x87.fldcw(i16 %RDI.arg)`，无 `@FPUControlWord` 全局，verify 通过。
- redis-check-rdb 0xbf7e0：`fnstcw()` → `fldcw()` → `call i64 @notdec.x87.fistp.i64()` → `fldcw()`，替换掉原来的 `@llvm.round.f80` 垃圾；redis-server 0x955c0 fmod：`fabs/fucomi/fsubr.sti(3)/fstp.sti` 全折叠。
- ffi.so 0x1bec8：`fld.f80/fchs/fstp.f80` 折叠，verify 通过。
- wrk 全量：134 个 `notdec.x87.*` call、0 条未折叠告警、`llvm-as`+verify 通过。
- `ctest --test-dir build -R native` 15/15；fortune i386 回归脚本通过。

## 后续已知问题

- `fstenv`（libavutil av_assert0_fpu 附近）：需库支持把 FPU 环境（14 字节）写内存，未折叠。
- `finit/fninit`、`fclex/fnclex`、无后缀 `fcom/fucom` 等写 FPU 字的指令未出现，暂不处理。
