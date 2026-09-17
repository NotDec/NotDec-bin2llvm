# native x87 窗口搬移读跟 range 状态（plan + 记录）

## 用户原始 prompt

> 这里的 unknown 是否被使用了？难道说其实是没有被有意义地使用？
> 按照这个继续推进试试吧

（上下文：上一轮 `logs/20260916-03-native-x87-range-store-type-plan.md` 分析完还剩两条
ST0 unknown，一条在 FUN_81d0、一条在 stats_mean。本轮先确认它们是否被有意义地使用，
再实现修复。）

## 已有 native 状态

- 产物目录 `/sn640/NotDec-Exp/Bench2/bin2llvm-ir/`，本轮对照二进制 `rootfs/usr/bin/wrk`、
  `rootfs/usr/bin/memcached`。
- 管线（`tools/notdec-native-llvm.cpp`）：InstCombine → `NativeRegisterSummarySSA` →
  EarlyCSE+InstCombine → 提升/peephole → final cleanup → EarlyCSE+InstCombine → 写 IR。
- HEAD = `84e3323`；上一个功能提交 `77eaa9d`（x86_fp80 store 进 range 状态）。
  wrk 基线：defines 87 / unknown 38 / warning 299 / IR 1,015,851 B；
  memcached 基线：defines 230 / unknown 1181 / warning 4970 / IR 5,652,931 B。
- x87 模型（`x87_fp80.md`）：`@ST0`/`@ST1` 是 `external global i80` 的显式窗口，ST2..ST7 走
  `notdec.x87.push/pop/peek/poke`。lifter 的 `x87WindowPush` 展开成
  `store v -> @ST0; store 旧 ST0 -> @ST1; call @notdec.x87.push(旧 ST1)`；窗口搬移读来自
  `readX87ST(1, true)`（`load i80, ptr @ST1` + bitcast，带 `notdec.x87.window.shift` 标记或
  由 use 形态识别）。
- summary 侧 `registerLoad()` 默认把窗口搬移读当内部搬移跳过（不产生 demand）；range 重写
  侧此前也跟着跳过，于是这类 load 一律保持成读 global。

## 先回答：这两条 unknown 是被有意义地使用吗

**FUN_81d0 的那条不是“未定义槽”，是丢了真实值。** pre-SSA（`/tmp/nb-frame/pre-ssa.ll`，
`define internal void @FUN_7300()` 这种没有 `notdec.register.summary_ssa` metadata 的原始
lift 产物）里：

```llvm
%ST0237238 = load i80, ptr @ST0            ; 入口旧 ST0
%ST1239    = load x86_fp80, ptr @ST1       ; 入口旧 ST1（搬移读）
store x86_fp80 %29, ptr @ST0               ; fldt 的栈上 long double 参数
store i80 %ST0237238, ptr @ST1
call void @notdec.x87.push(x86_fp80 %ST1239)
...
store x86_fp80 %30, ptr @ST0
store i80 %ST036240241, ptr @ST1           ; 参数被搬到 ST1
call void @notdec.x87.push(x86_fp80 %ST138242)
...
; fxch %st(1)（bb_821c）：ST0 <- 旧 ST1，ST1 <- 旧 ST0，从 @ST1 把参数读回来
```

summary 把窗口内的 `store ... -> @ST1` 收进 range 状态后不再写 global，但 `fxch` 的搬移读
还在读 @ST1，读到的是没人维护的旧值；最终 IR 里 `i80 %"stack+8.arg"` 0 使用、ST0 变成
`notdec.unknown`。所以答案是：**值一直在，是我们在搬移读上把它丢了**。stats_mean 的那条
同类（循环头的 ST0 值其实是本块 store 写的），修好后也变成已知值。

## 目标与判断标准

1. FUN_81d0 的 `fldt` 栈参数在最终 IR 里恢复流动（`stack+8.arg` 有真实 use）；
2. 不增加 `notdec.unknown`：wrk 38 → ≤38，memcached 1181 不变；
3. LLVM 22 `opt -passes=verify` 通过；
4. 单测覆盖：不落这个修复时必须失败。

## 路线与取舍

- 方案 A（先量后否决）：所有窗口搬移读都走完整 range 读
  （`readFullRangeValueBefore(..., allowUnknownSegments=false)`）。FUN_81d0 修好了，但
  查询本身会触发 entry/phi 解析，生成的 unknown 又被同一 range 的其它读取复用，wrk
  unknown 38 → 45（main +3、stats_mean +2、FUN_8530 +2、FUN_7300 +1、FUN_81d0 −1）。
  其中 main 的 3 条是外部调用后 ST1 clobber 的诚实表示，但不符合“不增加 unknown”。
- 方案 B（采纳）：只认“本块里前面刚写过”的值——`transferRangeBlockUntil` + `currentSegment`
  单条完整 range，同类型才替换；拿不到就保持原样。搬移读总是紧跟在写之后，这个范围覆盖了
  FUN_81d0 / stats_mean / FUN_88c0 / main 的实际形态，且不触发 entry/phi 解析。
- 不动：`registerLoad()` 默认语义（搬移读仍然不产生 demand）、`NativeRegisterSummary` 需求
  分析、x87 库（push/pop/peek/poke）语义。

## 实现

| 文件 | 位置 | 内容 |
|---|---|---|
| `lib/passes/summary/NativeRegisterSummarySSA.cpp` | `registerLoad()`（~819） | 新增 `includeX87WindowShift` 参数，默认仍跳过搬移读 |
| 同上 | `collectAccesses()`（~4302） | 用 `includeX87WindowShift=true` 收集 `Loads`，让搬移读进入 range 重写 |
| 同上 | `rewriteLoads()`（~4916） | 搬移读改走 `readBlockLocalValueBefore`，普通读不变 |
| 同上 | 新 `readBlockLocalValueBefore()`（~6079） | 本块 range transfer + 单 range `currentSegment`，同类型才替换 |
| `tests/native_register_summary_ssa_test.cpp` | `testX87WindowShiftLoadFollowsRangeState()`（~1778） | 三次 push 的窗口搬移链（i80 形态 + fp80 形态），断言后两次 push 的参数不再回读 @ST1 |

窗口内的访问在 `runNativeRegisterSummarySSA()` 开头由 `canonicalizeX87WindowAccesses()`
（同文件 ~961）统一成 `load i80 + bitcast`，所以 `readBlockLocalValueBefore` 只需要同类型的
range 值，不需要额外补 bitcast（这一点用单测里的 x86_fp80 load 形态验证过：规范化先跑，
helper 看到的仍是 i80 load）。

## 验证

```bash
cmake --build build -j 8 --target notdec-native-llvm native_register_summary_ssa_test
build/bin/native_register_summary_ssa_test
build/bin/notdec-native-llvm <wrk|memcached> --all-confirmed --register-ssa-summary \
  --register-ssa-warning-out <tsv> -o <ll>
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify -S <ll> -o /dev/null
```

A/B：同一棵树、同一组命令，只差这一处改动（`/tmp/nb-frame/base2.*` 与 `final.*`）：

| 指标 | HEAD `84e3323` | 修复后 |
|---|---|---|
| wrk unknown | 38 | **37** |
| wrk IR bytes | 1,015,851 | 1,017,022 |
| wrk warning 行 | 299 | 305 |
| wrk defines | 87 | 87 |
| memcached unknown | 1181 | 1181 |
| memcached IR bytes | 5,652,931 | 5,652,970 |
| memcached warning 行 | 4970 | 4970 |
| memcached defines | 230 | 230 |
| verify（LLVM 22） | OK | OK |

按函数的实际改动（wrk；其余 69 个函数只有 metadata 编号差异）：

| 函数 | unknown | clobber | `load ... @ST1` | 说明 |
|---|---|---|---|---|
| FUN_81d0 | 1 → 0 | 0 → 0 | 3 → 2 | `fldt` 栈参数恢复流动（`stack+8.arg` 由 0 处使用变 2 处） |
| stats_mean | 1 → 0 | 0 → 0 | 4 → 3 | 循环头的 ST0 值来自本块 store |
| stats_within_stdev | 0 → 0 | 0 → 0 | 4 → 3 | 同类搬移读解析成已知值 |
| FUN_88c0 | 0 → 0 | 0 → 2 | 2 → 0 | 调用点后的 ST1 用 clobber 值代替读 global |
| main | 0 → 0 | 0 → 3 | 10 → 6 | 同上（sigaction / stats_percentile / __printf_chk 之后） |
| FUN_7300 | 0 → 1 | 0 → 1 | 2 → 1 | 第 1、2 次 push 的值对了；第 3 次仍跨块回读（见遗留） |

fortune 对比（`scripts/native-fortune-{i386,x86_64}-regression.sh`，同参数、同 LLVM 22）：

| 用例 | 指标 | HEAD | 修复后 |
|---|---|---|---|
| fortune i386 | unknown / warning / defines | 1 / 8 / 41 | 1 / 8 / 41 |
| fortune i386 | IR bytes | 247,592 | 247,731 |
| fortune x86_64 | unknown / warning / defines | 0 / 34 / 13 | 0 / 34 / 13 |
| fortune x86_64 | IR bytes | 293,801 | 293,800 |

两个 fortune 回归脚本的所有不变量检查（`llvm-as`、`opt -passes=verify`、ESP ABI、无
ST2..ST7 global、无 ST0/ST1 residue）都通过。

wrk 整体 `load x86_fp80, ptr @ST1` 39 → 30（少 9 条读 global 的搬移读）；
`load x86_fp80, ptr @ST0` 13 → 16（入口 ST0 值不再作为死代码被删）。新增的 6 行 warning 全部
是 `ST1 clobber remaining_summary_clobber_value`，对应上面这些调用点：以前读 global，现在显式
物化成 clobber 值。

单测回归验证：把 lib 文件 `git checkout` 回 HEAD 重建后 `native_register_summary_ssa_test`
退出码 1，报 `x87 window shift read still re-reads the @ST1 global`；恢复修复后退出码 0。

## 效果、理解与维护成本

- 效果：修掉一处真实值丢失（FUN_81d0 的栈上 long double 参数）和一处循环头值
  （stats_mean），9 条搬移读不再读 summary 已不维护的 global。代价是 wrk +1.1 KB IR、
  +6 行 warning，memcached 基本不变。
- 理解成本：`registerLoad()` 多一个 `includeX87WindowShift` 开关，需要知道“需求分析跳过
  搬移读、range 重写要看见搬移读”这个分工。
- 维护成本：`readBlockLocalValueBefore` 只认本块、单 range、同类型三个条件，比较保守；
  跨块搬移读仍然读 global（见下）。

## 遗留 / 不做什么

- **跨块搬移读仍读 global**：FUN_7300 第 3 次 push 的旧 ST1 是第 2 次 push 写在 bb_732f 的
  值，而读发生在 bb_7359（跨块），本块查不到定义 → 保持原样。该值实际是已知的（第一次
  `fld` 的值 A），所以这是同类问题的未修分支；要修就得走完整 phi/entry 解析，实测会额外
  引入 unknown（方案 A 的 45 条），留待单独一轮。
- 没有改 `registerLoad()` 的默认行为，没有动 x87 库语义：搬移读依然不产生 demand。
- 没有引入 poison/undef 表示死槽（沿用项目“unknown 用不透明调用”的约定）。
