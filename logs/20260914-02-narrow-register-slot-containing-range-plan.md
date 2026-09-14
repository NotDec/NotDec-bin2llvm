# Narrow register slot reads from a containing planned range

## 背景

`format_time_us` / `format_binary` 调用 `FUN_81d0` 时，RSI 参数一直缺 binding：

```llvm
%RSI.arg_unknown = call i32 @notdec.unknown.i32()
tail call i64 @FUN_81d0(..., i32 %RSI.arg_unknown, ...)
```

PcodeToLLVM 里明明有：

```llvm
store i64 2, ptr @RSI
tail call void @FUN_81d0()
```

## 根因

诊断 `callArgStoreBindings()` 后确认：

- RDI slot 能读到值；
- RSI slot 的 `readSlotValueBefore()` 直接返回 `nullptr`；
- 进一步确认 `RSI` slot 是窄区间：

```text
offset=0 size=32
```

但 `planRegisterRanges()` 为 RSI 规划的 range 是整寄存器 / 更宽的 range。

`plannedRangesCovering()` 当前要求所有 planned range **完全落在请求区间内**：

```cpp
if (range.BitOffset > cursor || range.BitOffset >= end) break;
if (range.BitOffset + range.BitWidth > end) break;
```

所以请求 `RSI[0:32]`、planned range 是 `RSI[0:64]` 时：

- `ranges` 返回空；
- `readSlotRangeBefore()` 直接失败；
- callsite binding 阶段提前 `break`，后面的 stack 参数也不再收集。

## 修改

在 `readSlotRangeBefore()` 中增加 containing-range fallback：

1. 先按原逻辑尝试精确覆盖；
2. 如果精确覆盖为空，则在 `PlannedRanges` 中找一个完整包含请求区间的 range；
3. 读取该 range；
4. 用 `extractBitsFromIntegerValue()` 从完整值中截出请求的 bit 段。

这样窄 slot 可以安全地从更宽的 planned range 中取值。

## 验证

- `native_register_summary_ssa_test` 通过。
- 全量 ctest：12/12 通过。
- wrk 重新生成并通过 `llvm-as` + `opt -passes=verify`。

关键 IR 结果：

```llvm
tail call i64 @FUN_81d0(i64 81952, i32 2, i80 %"stack+8.arg")
tail call i64 @FUN_81d0(i64 82000, i32 2, i80 %"stack+8.arg")
tail call i64 @FUN_81d0(i64 82112, i32 2, i80 %"stack+8.arg")
```

原来 `format_time_us` / `format_binary` / `format_metric` 的
`FUN_81d0 RSI call_arg_rewrite_missing_binding` 全部消失。

wrk 总 warning：

```text
342 -> 322
```

## 剩余

`main -> format_time_s` / `format_time_us` 的 `stack+8` 仍是
`unknown/clobber`。这个问题不同：binding 已找到，但值链里含
`notdec.unknown.i80`，根因在 x87 `ST0 -> fstpt -> outgoing stack` 的值传播。
