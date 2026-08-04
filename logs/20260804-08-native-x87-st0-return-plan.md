# 20260804-08 native x87 栈返回（SysV long double ST0）建模

## 原始 prompt

```text
再找下一个问题吧

SysV long double 返回（x87 ST0），那多个long double是不是会用到ST1-ST7？那有点麻烦了，怎么办合适呢

按这个试试吧
```

## 背景

wrk 的 stats 模块（`stats_mean`/`stats_stdev`/`stats_percentile`）真实返回 `long double`（SysV x86-64：结果留在 x87 栈 ST0，函数尾部 `fsqrt; ret` 之类）。x87 指令已折叠成 intrinsic 库（ST0..ST7 不建全局），但 summary 的返回槽模型只有 GPR/XMM/ZMM（`AbiFacts::IntegerOutputsInOrder/FloatOutputsInOrder`），没有 ST0 返回槽：

- 单函数提升：默认 seed RAX，`stats_stdev` 恢复成 `i64` 返回，尾部 `ret i64 %RAX.range_summary_ssa`（RAX 未定义，垃圾值）。
- 全量提升：调用点需求传播把返回槽猜成 ZMM0，恢复成 `double`，四条返回路径全 `ret double %ZMM0.return_unknown`（`return_binding_uses_clobber_value`）。
- 外部 libm：`sqrtl` 真实 `long double sqrtl(long double)`，IR 里 `declare i64 @sqrtl()`；`round` 真实 `double round(double)`，IR 里 `declare i64 @round()`。

多个 long double 的 struct 返回按 SysV 走 hidden sret（内存），`complex long double` 才用 ST0+ST1，wrk 里都没有，不用建模 ST1-ST7。

## 目标

- 内部函数以 x87 栈返回 long double 时，LLVM 函数签名不加返回槽（保持 void），消除错误返回类型和 unknown 返回值。
- 调用方通过 `notdec.x87.fstp.f80()` 从库栈取回返回值（lifting 已正确建模，保持不动）。
- wrk 全量：`stats_stdev` 不再 `ret double %ZMM0.return_unknown`，签名变 void；`llvm-as`+verify 通过；fortune i386/x86_64 与 `ctest -R native` 不回归。

## 技术路线

1. `lib/passes/summary/NativeRegisterSummarySSA.cpp`：
   - 新 helper 检测"x87 栈返回函数"：函数体内有 `notdec.x87.*` intrinsic 调用，且返回路径（ret 所在 block 回溯到最近一条非 phi 指令，必要时看前驱 terminator 前一条）最后指令是 x87 intrinsic。
   - `shapeForInternalFunction`：命中时清空 `shape.Returns`（不生成 LLVM 返回槽）。函数保持/改回 void 返回，调用点重写和 return binding 走现有 void 机制（`stats_mean` 已是 void 返回，机制现成）。
2. 不生成新的 intrinsic；ST0 值继续留在库内部状态，调用方 `fstp.f80()` 弹出。
3. 外部 long double 函数（`sqrtl`/`round`）签名、`complex long double`、多 long double struct 押后，不在此轮做。

## 风险与判断标准

- 检测信号可能误报（函数混合整数/XMM 返回与 x87 路径）或漏报（ret block 无 x87 且前驱是外部调用）。用 wrk 全量核对：只应命中 stats 类函数；误报函数在输出 IR 里逐个看。
- `sqrtl` tail-call 路径（0x851a）真实返回值是 sqrtl 的 ST0，但 sqrtl 声明还是 i64，该路径库状态不完全准——属外部原型问题，本轮不修，留注释。
- 判断标准：wrk 全量无 `stats_stdev` unknown 返回、签名 void、调用点 `fstp.f80()` 取回；`ctest -R native` 15/15；fortune i386/x86_64 CTest 通过。

## 不做什么

- 不建 ST 寄存器全局，不改 intrinsic 库接口。
- 不修外部 libm long double 签名（sqrtl/round）。

## 实现（已完成）

改动文件与函数：

- `lib/passes/summary/NativeRegisterSummarySSA.cpp`：
  - 新增 `isX87StackPreservingIntrinsic()`：`notdec.x87.*` 且返回 void（fstp.f32/f64/f80 这类导出 ST0 为 LLVM 值的不算）。
  - 新增 `functionReturnsX87StackValue()`：函数体内有保留 ST0 的 x87 intrinsic，且从 ret 沿 pred 回溯（跳过纯栈清理 block，深度 ≤8）存在返回路径，最后一条计算指令是保留 ST0 的 x87 intrinsic。
  - 辅助：`isStackPointerGlobalName()`/`valueDerivedFromStackPointer()`/`isAllocaDerived()`/`isStackCleanupInstruction()`/`blockLastComputationIsX87()`/`blockIsStackCleanup()`：区分 lifting 生成的 RSP/RBP 栈清理（含 alloca 访问）和真正计算指令。
  - `shapeForInternalFunction()`：命中 x87 栈返回时 `break` 掉返回槽循环，`shape.Returns` 为空，函数保持 void 返回，不生成 RAX/XMM 假返回槽。
- `tests/native_register_summary_ssa_test.cpp`：新增 `testX87StackReturnKeepsVoidReturnType()`：void (i64) 函数体最后是 `notdec.x87.fsqrt`，断言 summary 重写后仍 void（若检测缺失会被 seedAbiReturns 加 RAX 槽变 i64）。

验证：

- wrk 全量：`stats_stdev` 从 `double + ret %ZMM0.return_unknown` 变为 `void`，`notdec.unknown.double` 消失，`llvm-as`+`opt -passes=verify` 通过；diff 函数签名只有 stats_stdev double→void。
- wrk 调用点：`call void @stats_stdev` + 调用方 `fstp.f80()` 弹出返回值，链路完整。
- fortune x86_64 / ffi.so 全量：0 unknown、签名无变化；av_sscanf（libavutil 单函数，x87 计算 + XMM0 返回）：保持 `i64` 返回，无误报。
- `./build/bin/native_register_summary_ssa_test` 通过；`ctest -R native` 15/15。
- 计划中押后项不变：外部 libm long double 签名（sqrtl/round）、complex long double、多 long double struct。
