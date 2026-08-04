# 20260804-05 native x87 fprem/fnstsw 折叠

## 原始 prompt

```text
接下来看看下一个问题吧
（分析后）按这个改一下吧
```

## 背景

Bench2 真实库（libpython3.12、libavutil/libavformat/libavfilter、libicuuc/libicui18n）的 fmod 实现都用 `fprem; fnstsw %ax; test $0x4,%ah; jne` 部分余数重试循环。之前这两条指令没折叠：FPREM 走普通 p-code，ST0/ST1 是 x87 栈寄存器（已从 RegisterStorage 过滤）→ 余数计算全丢；FNSTSW 读 FSW（0x10a2）→ `@FPUStatusWord` 未知 → AX 未知 → 循环条件在未知值上，提升出的 IR 要么无限循环要么随机退出。实测 `uprv_fmod`（libicuuc 0xaf970）整函数垃圾，还有 "x87 instruction FPREM ... was not folded" 告警。

Ghidra 侧事实：FPREM 的 p-code 是单次精确余数（`FLOAT_DIV→trunc→FLOAT_MULT→FLOAT_SUB`），不写 FSW C2；FNSTSW 的 p-code 只有 `AX = COPY(FSW 0x10a2)`，不碰 x87 栈（所以 `touchesX87Stack` 入口判断会放行）。

## 目标

- `FPREM` → `notdec.x87.fprem()`（void）：库内部一次算出精确余数（ST0 = ST0 mod ST1），FSW C2 保持 0。库不能模拟"部分余数 C2=1"，否则提升出来的 `jne` 重试循环会永远转下去；一次算完，循环正好跑一次退出，结果正确。
- `FNSTSW/FSTSW` → `notdec.x87.fnstsw()` 返回 `i16` 状态字：lowering 把结果写到 p-code 目标（`fnstsw %ax` → AX 寄存器；内存形式复用 fstp 的 StoreAddress 机制）。
- `uprv_fmod` 提升后循环是 `fprem()` call → `fnstsw()` call → `AH&4` 分支，无 unknown、无告警。

## 技术路线（lib/PcodeToLLVM.cpp）

1. 新增 `isFpuStatusWordVarnode()`（0x10a2,2 判定）。
2. `X87IntrinsicSpec` 加 `WritesStatusWord`（抑制组内 FSW COPY）和 `ResultRegister`（结果写回寄存器）。
3. `classifyX87ByMnemonic()`：FNSTSW/FSTSW 分支放在 `touchesX87Stack` 检查之前（该指令不碰栈）；找 `COPY(FSW)`，输出是寄存器 → `ResultRegister`，输出是 unique → 找消费它的 STORE 设 `StoreAddress`。FPREM 分支加在 FCOMI 之后：验证 ST0 有 FLOAT_SUB 写入即可，fold 成 void intrinsic。
4. `lowerX87Group()`：抑制循环加 FSW 输入跳过条件；call 后若 `ResultRegister` 则 `write()` 写回。
5. 单测 `testX87FpremFnstswFoldsToIntrinsicCall`：FPREM+FNSTSW AX+TEST AH+JNE 回边，断言 fprem/fnstsw call、AX 有写入、无 FSW/ST0 全局、verifier 通过。

## 判断标准

- `uprv_fmod` 提升无告警、无 unknown，循环形状为 `fprem(); fnstsw(); lshr/and 4; br`。
- FNSTSW 三种形式（AX、mem、mem+offset）都折叠成 `call i16 @notdec.x87.fnstsw()`。
- 单测、`ctest -R native` 15/15、fortune i386 回归、wrk 全量 verify 全绿。

## 风险与不做什么

- 风险：fprem 的 C2 语义由库决定（本实现要求库一次算完）；`fprem1`（IEEE 余数）未出现，暂不补。
- 不做什么：不实现 `notdec.x87` 库本身；heritage JSON 形状分类（`classifyX87ByShape`）不加 fprem/fnstsw（无助记符，现有目标不走这条路）。

## 实现（已完成）

改动文件与函数（`lib/PcodeToLLVM.cpp`）：

- `isFpuStatusWordVarnode()`（约 150 行）：FSW 寄存器（0x10a2,2）判定，用于 FNSTSW 分类和 lowering 抑制。
- `X87IntrinsicSpec`（约 460 行）：加 `WritesStatusWord`、`ResultRegister`。
- `classifyX87ByMnemonic()`：`spec` 声明提前到函数顶；FNSTSW/FSTSW 分支（约 895 行）在 `touchesX87Stack` 检查前，`COPY(FSW)` 输出为寄存器 → `ResultRegister`，为 unique → 找 STORE 设 `StoreAddress`；FPREM 分支（约 1105 行）在 FCOMI 分支后，验证 `FloatSub` 写 ST0 后 fold 成 `notdec.x87.fprem`。
- `lowerX87Group()`（约 1460 行）：抑制循环加 `WritesStatusWord && isFpuStatusWordVarnode(input)` 跳过；call 后 `ResultRegister` 存在则 `write()` 写回（约 1510 行）。
- `tests/pcode_to_llvm_test.cpp`：`addX87FpremFnstswOps()`（按真实 p-code 手写 FPREM 三段 + FNSTSW AX + TEST AH + JNE 回边）、`testX87FpremFnstswFoldsToIntrinsicCall()`，断言 fprem/fnstsw call、AX 全局写入、无 FSW/ST0 全局、verifier 通过。

验证：

- `./build/bin/pcode_to_llvm_test` 通过。
- `uprv_fmod`（libicuuc 0xaf970）：无告警、无 unknown；循环为 `call fprem()` → `call i16 @fnstsw()` → `lshr 8/trunc/and 4` → `br` 回边，`llvm-as`+verify 通过。
- FNSTSW 三形式（/tmp/fsw 测试 ELF）：AX 形式写回 EAX、`fstsw (%esp)`/`fnstsw 0x10(%esp)` 内存形式走 StoreAddress，均为 `call i16 @notdec.x87.fnstsw()` + store。
- `ctest --test-dir build -R native` 15/15 通过；fortune i386 回归通过；wrk 全量提升无告警、verify 通过。
