# native：x86_fp80 store 写 i80 寄存器 global 被 range 状态丢弃

## 原始 prompt

```text
按这个卡死推进修一下吧
按这个开始推进修一下吧
```

（上下文：之前把 wrk 剩下的 43 条 `notdec.unknown` 分了三族，本轮修的是其中
唯一"全部 live"的一族——x87 ST0/ST1 的 7 条。）

## 背景：三族 unknown 与为什么先修 x87

| 家族 | 数量 | reason | live / dead |
| --- | --- | --- | --- |
| indirect call 后的 RDX | 28 | `range_return_helper_clobbered_register` | 14 / 14 |
| x87 ST0/ST1 | 7 | `range_unknown` | **7 / 0** |
| memchr / luaL_checkudata / 内部函数后的 RDX | 6 | 同上 | 2 / 4 |
| `aprintf` 入口 RAX 低字节 | 2 | `range_unknown` | 0 / 2 |

- indirect RDX 是 `logs/20260912-01` 明确的设计取舍（"整型返回从 RAX 开始，RDX 默认
  排除"），要动先得改那条约定；
- x87 这 7 条全部 live：例如 `stats_stdev` 的 **返回值** 就是 ST0，其中一条路径上
  直接是 `notdec.unknown.i80`。

## 根因

以 `stats_stdev` 为例（final IR）：

```llvm
bb_8458:
  %2 = sitofp i64 %1 to x86_fp80
  %x87.fp80.bits3907 = load x86_fp80, ptr @ST1, align 4
  store x86_fp80 %2, ptr @ST0, align 4          ; ← 类型是 x86_fp80
  call void @notdec.x87.push(x86_fp80 %x87.fp80.bits3907)
  %ST0.range_unknown52 = call i80 @notdec.unknown.i80()   ; ← 这里
bb_8471:
  %ST0.range_summary_ssa473.in = phi x86_fp80 [ %18, %bb_84f0 ], [ %2, %bb_8458 ]  ; 算数路径有值
  %ST0.range_summary_ssa51 = phi i80 [ %ST0.range_unknown, ... ], [ %ST0.range_unknown52, ... ] ; range 路径没值
...
  %16 = bitcast i80 %ST0.range_summary_ssa51 to x86_fp80
  %ST0.range_summary_ssa = phi x86_fp80 [ %25, ... ], [ 0xK0, ... ], [ %16, ... ]
  ret x86_fp80 %ST0.range_summary_ssa
```

机制：

1. `@ST0` 是 `@ST0 = external global i80`，而 x87 窗口规范化（以及前一轮
   InstCombine 折叠它的 bitcast）写的是 `store x86_fp80 %2, ptr @ST0`；
2. `registerStore()` 里 `IsStorageValue = (store 值类型 == global 类型)` → false；
3. `transferRangeInstruction()` 的 store 分支要求 `IsStorageValue`，于是**这次写被
   静默丢掉**，range 状态里 ST0 一直是"没有定义"；
4. 读取路径（`currentSegment()`/`writeSegment()`）只接受 `rangeType()`（i80）类型的
   值，所以即使记进去也读不出来；
5. 结果：返回槽（i80 range）拿不到值 → `notdec.unknown.i80`；而 x86_fp80 那套
   表示（`.in` phi）由另一条路解析成功，于是出现"同一寄存器两套表示，一套有值
   一套 unknown"。

pre-SSA IR 里这类 store 是混合的（InstCombine 折叠了一部分 bitcast）：
`store i80 ...` 和 `store x86_fp80 ...` 都有，所以只有一部分 x87 路径中招。

## 路线（最小修改）

`lib/passes/summary/NativeRegisterSummarySSA.cpp`：

- 新增 `coerceStoreValueToRangeWidth()`：store 的值类型与 global 类型不同但
  **位宽相同**、且是标量整数/浮点（指针要先 ptrtoint、向量不在内）时，在写点插入
  一个 `bitcast` 到 `i{width}`；
- `transferRangeInstruction()` 的 store 分支：`IsStorageValue` 为真照旧；为假时先尝试
  coerce，coerce 不出来（真正的部分写、指针等）就保持原来的"忽略"行为。

不动 `IsStorageValue` 本身的语义（它在别处还有 12 个使用点）。

## 不做什么

- 不动 indirect call 的 RDX 保守返回（有 test 钉住，要改先讨论）；
- 不给 `notdec.unknown.*` 加 `memory(none)`（实测在 EarlyCSE 之后只省 0.4% 字节，
  而且会合并独立 unknown）。

## 实现记录

- `lib/passes/summary/NativeRegisterSummarySSA.cpp`：新增
  `coerceStoreValueToRangeWidth()`，改 `transferRangeInstruction()` store 分支；
- `tests/native_register_summary_ssa_test.cpp`：新增
  `testX87Fp80StoreStillFeedsRange()`（x86_fp80 store → 断言返回类型变成 x86_fp80
  且模块里没有用到的 `notdec.unknown.i80`）。

### 反向验证

把 coerce 分支改成 `nullptr`（等价于修复前）重跑：新测试报
`x86_fp80 store into the i80 ST0 global was dropped`（EXIT=1）；恢复后
`native_register_summary_ssa_test` 全绿。

## 验证（wrk，对照 = 43d75fc）

| 项 | 之前 | 本轮 |
| --- | --- | --- |
| 指令数 | 11,707 | 11,693 |
| IR 字节 | 1,017,586 | 1,015,679 |
| define 数 | 87 | 87 |
| `notdec.unknown` | 43 | **38** |
| — 其中 x87 ST0/ST1 | 7（ST0 4 + ST1 3） | **2**（ST0） |
| — RDX after call | 34 | 34 |
| — aprintf RAX | 2 | 2 |
| `summary_clobber` | 25 | 25 |
| warning TSV | 299 行数据 | 299（与之前 diff 0） |
| residue | 1 行 | 1 行 |
| ctest | 12/12 | 12/12（含 fortune x86_64/i386） |

剩下的 2 条 ST0 不是这次的类型不匹配机制：

- `FUN_81d0`：query 点落在本块 `store x86_fp80 %2, ptr @ST0` **之前**（unknown 被插在
  store 前面），拿到的是 store 之前的状态；
- `stats_mean`：该路径上 ST0 根本没有定义（ST0 不是入口输入），是诚实的 unknown。

时间这轮是 45.1s（同一份代码在本机 41–46s 之间抖），不作为判据；判据是指令数 /
unknown 数 / 警告行数。

## memcached 与 ctest

memcached：完全中性（同一份对照 `memcached.ecse2`）。

| 项 | 之前 | 本轮 |
| --- | --- | --- |
| 指令数 | 63,049 | 63,049 |
| IR 字节 | 5,652,860 | 5,652,964（只差编号位数） |
| define / unknown / clobber | 230 / 1,181 / 3,939 | 230 / 1,181 / 3,939 |
| warning TSV | 4,970 行数据 | 4,970（diff 0） |
| residue | 5 | 5 |
| `sasl_listmech` | 8 参 | 8 参 |

ctest：12/12（含 fortune x86_64/i386）。
