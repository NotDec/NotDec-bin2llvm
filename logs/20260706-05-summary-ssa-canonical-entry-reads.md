# SummarySSA canonical entry reads

## 背景

`NativeRegisterSummarySSA` 之前用 `freeze poison` 表示函数入口寄存器值。这个对普通未用 bit 还可以，但对 `RSP/RBP` 这类参与地址计算的寄存器不合适：后续优化可能把入口值折成 0，最后出现错误的 `inttoptr (i64 -N to ptr)`。

本次目标是：入口寄存器值不能再凭空变成 0；如果 stack 能恢复就折成 `alloca`，否则保留可追踪的入口寄存器读取。

## 修改

- `lib/passes/summary/NativeRegisterSummarySSA.cpp:435`：新增 `eraseDeadSummarySSAEntryReads()`，清掉后续签名重写后无用的 canonical entry read。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1814`：post-signature cleanup 末尾调用 `removeDeadEntryReads()`，避免签名替换后留下无用入口读取。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1858`：新增 `EntryRegisterLoads` 缓存，同一个函数里同一个完整寄存器入口值只生成一次。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1952`：把 `zeroDemandReplacement()` 改名为 `undemandedStoreOperandZero()`，明确它只处理寄存器 store 数据流里无人需求的 bit，不代表未知入口值。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:2570`：`collectAccesses()` 跳过带 `notdec.register.summary_ssa.range_entry` 的 canonical partial read，避免把入口读取当普通 partial read 再处理。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:3080`：新增函数级 `removeDeadEntryReads()`。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:4179`：新增 `entryRegisterLoad()`，完整入口 range 生成 canonical `load @REG`。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:4196`：新增 `entryPartialRead()`，小 range 入口值生成 `notdec.partial_read.iFULL.iREAD(@REG, offset)`，不主动用 `lshr/trunc` 从 full load 抽取。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:4221`：`entryRangeInput()` 从 `freeze poison` 改成按 range 懒生成 canonical entry read。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:5846`：SummarySSA 末尾补一轮 dead helper / entry read 清理。
- `include/notdec-bin2llvm/passes/summary/NativeRegisterFinalCleanup.h:21`：final cleanup summary 增加 dead read、register global、helper declaration 删除统计。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:72`：final cleanup 在判断函数是否还有 register residue 前，先删除无用 register load / `partial_read`。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:107`：删除已经没有 use 的 register global。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:121`：删除无用 summary / partial helper 声明。

## 验证

构建：

```bash
cmake --build external/NotDec-bin2llvm/build --target notdec-native-llvm -j4
```

fortune：

```text
OUT=/tmp/notdec-bin2llvm-fortune-entryload-finalcleanup-20260706190041
inttoptr_negative_count=0
entry_load_count=1
range_entry_partial_count=1
raw_RSP_refs=0
raw_RBP_refs=0
register_globals=2
summary_helpers=0
unused_range_entry_calls=0
elapsed=12.12
verify=ok
```

php sockets：

```text
OUT=/tmp/notdec-bin2llvm-php-sockets-entryload-finalcleanup-20260706190053
inttoptr_negative_count=0
entry_load_count=29
range_entry_partial_count=25
raw_RSP_refs=7
raw_RBP_refs=3
register_globals=16
summary_helpers=9
unused_range_entry_calls=0
elapsed=89.52
verify=ok
```

验证使用本地 LLVM 22：

```bash
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as native.ll -o native.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify native.bc -disable-output
```

## 评价

- 实现效果：8/10。`RSP/RBP` 不再被错误折成 0，fortune 的无用 entry read 清干净；sockets 仍有真实 register residue，但没有新增无用 entry read。
- 复杂度：6/10。入口值从 poison 改成真实 read 后逻辑更容易解释，但 cleanup 需要覆盖 SummarySSA 后和 final cleanup 两个位置。
- 维护成本：6/10。新增逻辑集中在 SummarySSA 和 final cleanup，没有扩散到 lifting；后续如果 partial read/write intrinsic 继续扩展，需要同步维护 final cleanup 的 helper 判定。

