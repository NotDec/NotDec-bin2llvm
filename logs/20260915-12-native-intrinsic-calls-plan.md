# 原始 prompt

```text
怎么感觉用call好一点。在那边识别一下这种调用吧，知道它不影响任何寄存器。特别是这种
"notdec."开头的可以都保留下来作为内置intrinsic，都假设它不影响任何寄存器，方便以后
增加intrinsic。其他的方面就按照当前计划修复试试吧
```

# 背景

修 FUN_9990/FUN_99d0（栈上的回调表被删）时需要一个"帧保活"手段。call 形式
（`call void @notdec.native_frame.keep(ptr %frame)`）第一版把 summary/签名重写都
带崩了：warning 353 -> 795、unknown_call_effects 72 -> 30、模块验证失败。
原因不是 call 本身，而是 `notdec.*` helper 的识别散落在多个名单里，新增一个名字
就得补一处。

# 本轮改动：notdec.* 一律视为内置 intrinsic

三处名单统一成 `name.starts_with("notdec.")`：

1. `lib/passes/summary/NativeRegisterSummary.cpp` `isNotDecRegisterHelperCall()`：
   原来的 `notdec.register.*` / `notdec.unknown.*` / x87 / value-range 名单；
2. `lib/passes/summary/NativeRegisterSummarySSA.cpp` `isUnknownExternalFunction()`：
   原来同样的名单；
3. `lib/passes/summary/NativeRegisterSummarySSA.cpp`
   `buildInitialSignatureShapes()` 的函数过滤：原来会跳过
   `notdec.register.*`/unknown/value-range/partial read/write，现在跳过所有
   `notdec.*`（这一处决定调用点会不会生成 `summary_clobber` 占位）。

约定写进注释：`notdec.*` 的寄存器影响一律由它们自己的语义（指针参数、返回值）
表达，调用点不参与外部参数推断、ABI clobber 推导和签名重写。以后新增
`notdec.*` intrinsic 不需要再改名单。

# 验证（wrk，仅这三处改动）

| 项 | 基线 | 本轮 |
| --- | --- | --- |
| define 数 | 85 | 85 |
| `ptrtoint (ptr @...)` 使用数 | 21 | 21 |
| `notdec.unknown.iN` 调用数 | 69 | 69 |
| warning TSV | 348 行 | 348 行，diff 0 |
| residue | 1 行 | 1 行 |
| IR 体积 | 1,862,315 B | 1,862,341 B |
| 时间 | 41.3s | 41.1s |
| ctest | 12/12 | 12/12 |

即三处统一本身是中性改动（ir 只差 26 B），可以先落地。

# 还没完成：call 形式保活仍会被 range 模型当 clobber

在同一个分支上试过 `attachNativeFrameKeepAlive()`（入口插
`call void @notdec.native_frame.keep(ptr %frame)`，每个帧一次，wrk 共 113 个）+
"非 spill 帧 store 一律保留"，结果：

| 项 | 基线 | call 保活 + 三处统一 |
| --- | --- | --- |
| warning TSV | 348 行 | 813 行（其中 217 条直接来自 keep-alive 调用：`remaining_summary_clobber_value`） |
| `notdec.unknown` 调用 | 69 | 95 |
| residue | 1 行 | 2 行 |
| IR 体积 | 1,862,315 B | 1,968,002 B（+5.7%） |
| define 数 | 85 | 85（FUN_9990/99d0 仍未回来） |

说明 `FunctionBuilder` 的 range/clobber 模型在"调用点"还有一条**不查名字**的
clobber 路径（这三处名单之外），只要遇到 call 就会生成
`notdec.register.summary_clobber.*`。要按 call 方案修，下一步是找到并放开那条
路径（grep `summaryClobber`/`range_return_helper` 在 `NativeRegisterSummarySSA.cpp`
里的判定）；否则改用 store 形式的标记
（`store ptr %frame, ptr @notdec.native_frame.escaped`，不是 call，绕开 range 模型）。

另外，"非 spill 帧 store 一律保留"这条实测很便宜：wrk 上 IR +663 B，时间/warning/
residue 都不变，可以和第 1 层一起落地。
