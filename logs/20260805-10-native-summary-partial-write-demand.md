# 20260805-10 native summary：partial write 只擦除写入位，不再反向加 demand

## 原始 prompt

```text
再找下一个问题吧
```

## 背景

上一轮（20260805-09）后 wrk 还剩 3 条 ST0 warning，其中 `stats_stdev` 的返回槽是
`{ x86_fp80, double }`，double 槽是多余的：调用点之后的传参写把 ZMM0 demand 误传给了
更早的 `call stats_stdev`，给不返回 double 的函数误加 ZMM0 返回槽。进一步查发现大量
`range_return_helper_rewrite_missing_value`（FUN_8530/FUN_88c0/FUN_9ea0/FUN_a030/
aeResizeSetSize 等）都是同一根因。

## 根因

`NativeRegisterSummary.cpp` 的 `liveBeforeBlock`（约 2264 行）对
`notdec.partial_write` 调用执行 `addDemand(live, global, valueDemand(partial->Value))`，
把写入值自身的位需求反加到 global 的 live demand 上。这条 demand 沿调用点向后传播，
使更早的 callee 被误认为返回该寄存器位段（如 `lua_pushnumber` 的 double 参数经
`partial_write @ZMM0`，把 ZMM0 demand 反向传给 `call stats_stdev`，函数被加上
ZMM0/double 返回槽）。

## 修复

`liveBeforeBlock` 的 `parseNativeRegisterPartialWrite` 分支（else 分支）：

- partial write 定义写入位：`eraseDemand(live, global, writeMask)`，写覆盖旧需求；
  未写位保留（读改写需要旧值）。
- 写入值自己的位需求由 value 链上的显式 load 追踪，不再反加到 global。

改动点：`lib/passes/summary/NativeRegisterSummary.cpp` 的 `liveBeforeBlock`
（`parseNativeRegisterPartialWrite` 分支，约 2264-2275 行）。

## 验证

- wrk 全量 rc=0，`llvm-as` + `opt -passes=verify` 通过。
- `stats_stdev` 返回恢复为 `x86_fp80`，double 槽消失；FUN_8530/FUN_88c0/FUN_9ea0/
  FUN_a030/aeResizeSetSize 等 ZMM0 `range_return_helper_rewrite_missing_value` 误报消除。
- warning 总数 1075→1043；新增 41 条逐条核对均为语义正确/既有局限：
  - main 的 ZMM0-6 `remaining_summary_clobber_value`：调用后向量寄存器被 clobber，
    IR 保留 unknown 值，与既有 ~1000 条同类 warning 一致。
  - FUN_ae30 的 R8/RSI clobber / `call_arg_uses_clobber_value`：反汇编确认调用
    gettimeofday/script_response/aeCreateFileEvent 前 R8/RSI 确实未设置，绑定到
    clobber 值正确；替换掉了旧的 `call_arg_rewrite_missing_binding`。
  - format_time_s 的 `stack+8` binding missing：tail call 栈参透传的既有局限，
    取代旧的假 ZMM0 binding missing。
  - http_parser_execute/parse_url 的 RDX return range [0,16)：http_parser 辅助函数
    真实写入 EDX（如 `mov $0x1,%edx`），调用点后 RDX [0,16) 本应 unknown，新 warning
    反映更准确的 clobber/return 分析，非回归。
- `pcode_to_llvm_test`、`native_register_summary_ssa_test`、`native_register_summary_test`
  通过；fortune i386/x86_64 回归脚本通过；`ctest -R native` 15/15。
