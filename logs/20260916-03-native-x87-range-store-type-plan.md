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

## 附：剩下 2 条 ST0 unknown 的详细定位

结论：这两条**不是**类型不匹配（那是已修的 5 条），而是 **x87 栈顶下面的空槽被
push/pop/xchg 旋转后浮到 ST0**，模型给 `notdec.unknown` 是诚实的；两条都不影响
函数算出来的结果。

### 证据 1：pre-SSA 的原始序列

FUN_81d0 entry（pre-SSA 25922-25928）：

```llvm
%arg  = load i80, ptr [RBP+16]     ; 入参 long double（stack+8.arg）
%old0 = load i80, ptr @ST0         ; ← 栈顶下面的槽
%old1 = load x86_fp80, ptr @ST1
store i80 %arg,  ptr @ST0          ; 压栈：新顶
store i80 %old0, ptr @ST1          ; 旧顶下移
call void @notdec.x87.push(%old1)
```

FUN_81d0 `bb_821c`（pre-SSA 25986-25989）——关键：

```llvm
%a = load i80, ptr @ST0
%b = load i80, ptr @ST1
store i80 %b, ptr @ST0             ; ST0 ← 旧 ST1（就是 entry 里那个未定义槽）
store i80 %a, ptr @ST1
```

即 bb_821c 把 ST1 里那个未定义槽**换回了 ST0**。SysV 下 ST0/ST1 在函数入口本来
就是未定义的（psABI 要求 x87 栈在调用边界为空，long double 参数走内存），所以这条
路径上 ST0 确实没有值 → unknown 忠实。

stats_mean（pre-SSA 26452-26455）是函数里**第一次**碰 ST0/ST1：

```llvm
%old0 = load i80, ptr @ST0
%old1 = load x86_fp80, ptr @ST1
store x86_fp80 <新值>, ptr @ST0
store i80 %old0, ptr @ST1
```

循环入口之前没有任何 ST0 写（pre-SSA 里第一个 ST0 store 就是这一次），所以循环头
携带的 ST0 初值本来就是未定义的。

### 证据 2：临时插桩 trace

（`NOTDEC_RANGE_TRACE=1` 只对这两个函数打印 rstore/rphi/rread，分析完已回退）

FUN_81d0：

```text
[rstore] entry   isStorage=1 source=v wrote=1
[rstore] bb_821c isStorage=1 source=v wrote=1      ← swap 那次写
[rphi]   bb_823b <- bb_821c exit=v typed=v dom=1   ← phi 拿到的就是 swap 后的未定义槽
```

stats_mean：

```text
[rread] bb_83cd cur=null unknownDef=0
[rread] bb_83bc cur=null unknownDef=0
[rread] entry   cur=null unknownDef=0
[rphi]  bb_83d0 <- bb_83cd exit=null typed=null dom=0   ← 循环头 ST0 入边确实无定义
```

即**不是丢写、也不是缓存顺序问题**：FUN_81d0 每条 ST0 写都 `wrote=1`；stats_mean
链路上 ST0 确实无定义。

### 为什么不影响结果 / 可选后续

- FUN_81d0 的 unknown 参与 `%x87.fp80.value245` 的 phi + `fcmp ugt`，最后进
  `store i80 %ST0..., ptr %notdec_stack.native`（`aprintf` 的出参槽）；
- stats_mean 的 unknown 是循环头 ST0 phi 的合法入边，被循环体第一次 shift-read 用掉；
- 但两者表示的都是 ABI 未定义的槽位，两个函数的返回值都来自显式 store 的值。

可选处理（都不建议单独开一轮）：

1. 保持现状：保守、忠实，只是 2 条 IR 噪音；
2. 把"下移槽的读"归入窗口移位内部动作（像 `isX87WindowShiftLoad` 那样不产生
   demand）：unknown 消失，但语义变成"ST0 保持旧值"——对未定义槽同样合法，只是不如
   unknown 诚实；
3. 用 `poison`/`undef` 表示死槽：能让 LLVM 折叠整条链，但 poison 会传播（可能被
   当 UB），不符合项目现在"unknown 用不透明调用"的约定。

## 更正（2026-09-17）

上面的结论“两条 ST0 unknown 都是 ABI 未定义的槽位、unknown 忠实”**不成立**：

- FUN_81d0 的 ST0 unknown 不是未定义槽。pre-SSA 里 `fldt` 的栈上 long double 参数先
  `store -> @ST0`，下一次 `fld` 又把它 `store -> @ST1`，`fxch` 再从 @ST1 读回来；summary 已经
  把窗口内的 store 收进 range 状态（IR 里不再写 @ST1），而这条窗口搬移读还在读 global，
  拿回的是没人再维护的旧值，于是 `i80 %"stack+8.arg"` 在最终 IR 里 0 使用。
- stats_mean 的那条同类（循环头的 ST0 值其实是本块 store 写的），修复后也变成已知值。
- 修复见 `logs/20260917-01-native-x87-window-shift-load-plan.md`：修好后 FUN_81d0 的参数恢复
  流动，wrk unknown 38 → 37，memcached 1181 不变。

