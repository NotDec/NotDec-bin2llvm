# 20260804-01 i386 DF 调用后定值 0

## 原始 prompt

```text
按照bin2llvm的分析链路顺序，先修靠前的问题。为什么fortune里面有x87的ST寄存器？感觉不应该啊
先看一下DF的问题吧，它是什么情况
对，按照B这个弄吧。除了DF，其他的flag有被ABI规定吗，是不是也可以一样处理
```

## 背景

fortune i386 里 8 处 `repz cmpsb`（0x154b、0x20e1/0x20f2、0x271a、0x2ac9/0x2b9e/0x2bba/0x2bcd）被 Ghidra 展开成循环，指针增量方向由 DF 决定（`ptr + 1 - DF*2`）。IR 里 DF 被外部调用点（readdir、strrchr、__ctype_b_loc）的 clobber 值接管，方向变成未知，对应 4 条 DF clobber 告警（2010/2170/2a50 三个函数）。

根因不是解码错误，是 ABI 建模缺口：

- i386 SysV ABI 规定 DF 在函数入口、返回以及外部调用返回后都必须为 0；fortune 无 std/cld，DF 实际恒为 0。
- 本地实际用的 cspec（/sn640/ghidra 的 x86gcc.cspec）default_proto 的 `<unaffected>` 只有 ESP/EBP/ESI/EDI/EBX，不含 DF（有版本差异，sleigh-config 那份就含 DF），通用调用点 clobber 逻辑把 DF 当未知值。
- 其他 flag（CF/PF/AF/ZF/SF/OF）是 caller-saved，调用后状态未定义，没有类似 DF 的"必须为 0"约定，不能一样处理。

## 目标

DF 在调用点（外部 clobber 或内部函数返回）都定值为常量 0，让 cmpsb 展开循环方向正确，同时不误把 DF 当入口参数，不破坏 std/cld 局部写。

## 技术路线

方案 B（代码层建模，不动 cspec）：

- 加 `isPostCallZeroRegister()`：DF。
- `callRangeValue()`（SummarySSA 生成调用点寄存器值处）对 DF 直接返回常量 0，覆盖 clobber 和 return 两条路径（return 路径原本生成 `range_return_unknown`）。
- 第一遍 register summary（NativeRegisterSummary.cpp）不改：Cell 域只区分 Entry/NonEntry，clobber 与"调用点写 0"域等价，backward demand 在调用点截断逻辑不变。

## 风险与判断标准

- 风险：std 后调外部函数再 cmpsb 的场景，调用点 DF 定值 0 与 ABI 一致（被调用方保证返回时 DF=0）；手写汇编违反 ABI 的场景不保证。
- 判断标准：4 条 DF clobber 告警消失；三个函数的 cmpsb 循环变成纯 `+1` 前向；x64 fortune 与 native 单测无回归。

## 实现

- `lib/passes/summary/NativeRegisterSummarySSA.cpp` 约 890 行：新增文件作用域 `isPostCallZeroRegister()`，注释说明 DF ABI 语义和为什么不用 cspec unaffected。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp` `SummarySSA::callRangeValue()`（约 6255 行）：在 return 检查前，对 `isPostCallZeroRegister(unit->Name)` 直接 `ConstantInt::get(rangeType(range), 0)` 并缓存，不再生成 `summary_clobber` / `range_return_unknown` 占位。
- 顺带收尾：删除工作区里排查 3930 EAX 返回值遗留的 DBG 打印（`NativeRegisterSummary.cpp` applyBackwardCallDemand、`NativeRegisterSummarySSA.cpp` buildInitialSignatureShapes）；保留 functional 的 intrinsic 跳过改动（applyBackwardCallDemand 跳过 intrinsic call）。

## 验证

- i386 fortune：DF clobber 告警 4 条 → 0；IR 里 DF 参与运算的 phi/zext/sub 全部消失，2010/2170/2a50 的 cmpsb 循环变成 `add ptr, 1` 纯前向。剩余告警 1 条：19b0 __fprintf_chk ST6 clobber（x87，下一个问题）。
- x64 fortune：无回归（call_arg 仍 0）。
- ctest：`-R native` 15/15 通过，native register/external/analysis 相关 8/8 通过。

## 下一步

19b0 的 x87 ST6 clobber 参与 `fdiv`（`%ST1569604.cast = bitcast i80 %ST6.clobber710 to x86_fp80`），是 x87 寄存器栈在 SummarySSA 的定值/use 链断点，见前一轮 IR 分析。
