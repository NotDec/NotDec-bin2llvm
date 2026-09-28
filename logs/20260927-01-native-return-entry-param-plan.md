# 返回槽里“可能还是入口值”的位没有形参承载（读寄存器 global / partial_read 占位）

> **后续**：本文的“入口形参”改动已并入 `logs/20260927-02-native-register-value-source-plan.md`
> 的**阶段 1**（入口侧闭合）；本轮实验未落地，patch 在 `/tmp/fix-entry-param.patch`。

## 0. 用户原始 prompt

> 继续修复IR里面明显的问题吧。性能目前看还行。

（接着“继续”，即继续上一轮的 IR 问题排查。）

## 1. 现象：产物里读未定义的寄存器 global

扫 `/tmp/fix29/*.ll`（10 目标，性能优化后的产物）时发现两类“入口值”占位读：

| 目标 | `notdec.partial_read`（带 `summary_ssa.range_entry`） | 整寄存器 entry load（`load iN, ptr @REG`） |
|---|---|---|
| lighttpd | 76（71） | 54 |
| redis-cli | 97（94） | 45 |
| tmux | 272（267） | 473 |
| ssh | 81（75） | 16 |
| wolfssl | 1319（1311） | 138 |
| libuv | 17（14） | 6 |
| memcached | 13（9） | 11 |
| ffmpeg | 6（4） | 1 |
| vsftpd | 5（4） | 1 |
| wrk | 0 | 0 |

这些读的地址是寄存器 global，而产物里寄存器 global 全部是 `external global`
（`lib/RegisterStorage.cpp:158` 建的就是无初值 ExternalLinkage），没有任何 pass 定义或写入
它们：`@RDX = external global i64` 是唯一一条 `@RDX` 声明，全模块没有 `store ... @RDX`。

例子（lighttpd `fdevent_fdnode_event_set`，DWARF `void (fdevents *, fdnode *, int)`）：

```llvm
define internal i64 @fdevent_fdnode_event_set(i64 %RDI.arg, i64 %RSI.arg, i32 %RDX.arg) ...
entry:
  %RDX.range_entry68 = call i32 @notdec.partial_read.i64.i32(ptr nonnull @RDX, i64 32)
  ...
common.ret:
  ... ret i64 (执行到这里的 RDX)      ; 高 32 位来自上面那次读
```

## 2. 根因：出口 provenance 是 Mixed 的返回寄存器缺入口值

1. 这些函数的签名里带返回槽（`shape.Returns`）：summary 说该寄存器 `MayNonEntry && ExitDemand`，
   于是函数把它当“调用后寄存器值”返回给调用者（`NativeRegisterSummarySSA.cpp:1806`、
   `addExitLiveRegisters` 的出口活跃性 `:5627`，返回的位是 `ExitDemandMask & MayNonEntryMask`）。
2. 返回的位里有一部分在**某些路径上没被写过**（`MayEntryMask` 非 0，即出口 provenance 是 Mixed）。
   这些路径上出口值就是入口值，按位 SSA 必须在函数入口给出一个值（`entryRangeInput`）。
3. 入口值只有两个来源：形参（`entryRangeArgument`）或读寄存器 global（`entryRegisterLoad` /
   `entryPartialRead`）。形参由 `shapeForInternalFunction` 按 `ReadEntryMask` 建，
   `ReadEntryMask` 只记“入口处被语义读过”的位——本函数从没读过 RDX，所以没有 RDX 形参，
   于是退回读 global。

为什么读 global 是错的：callee 入口的寄存器值在真实机器里等于**调用点那一刻调用者的值**。
调用者的值在调用者的按位 SSA 里是已知的（可能是它自己的入口值、前面某次写的值），
但两边没有任何连接：callee 读的是 global，调用者也不会往 global 写。中间任何一次写
（例如 `mov %edx,%esi`）都被丢掉。

## 3. 方案

**最小改法（本轮采用）**：把“返回槽里可能还是入口值的位”也算进形参需求，即
`shapeForInternalFunction` 的 `addParamForUnit` 里

```c++
llvm::APInt returnEntryMask;             // 仅对 abi.InternalReturnRegisters 里的寄存器
if (internal return register) {
  returnEntryMask = ExitDemandMask & MayNonEntryMask & MayEntryMask;   // 按寄存器宽度对齐
}
if (!ReadEntry && returnEntryMask.isZero()) return;   // 原来的“没读过就不建形参”保留
...
paramMask |= returnEntryMask;
```

- 位宽对齐用 `zextOrTrunc`（mask 来自 hex 文本，前导零会丢），沿用 `addExitLiveRegisters` 的做法。
- 中段 mask（`countr_zero != 0`）的既有拒绝判据不动：这类仍按原设计退化成入口占位。
- 不放宽 `ReadEntryMask` 本身（它还被 `rangeMayComeFromEntry` 等消费），只在“建形参”这一处叠加。

**取舍**：
- 形参会变多（internal 函数的寄存器形参本来就不是 ABI 签名，DWARF 对照只看真形参是否还在）。
- 调用点必须给出这些位的值：调用者按位 SSA 里有就拿自己的值（正确），没有就可能多出
  unknown（`call_arg_rewrite_missing_binding` 一类的告警）。这是“把错误从 callee 的一次全局读
  变成调用点的显式未知”，语义上更诚实，但要用 unknown 计数核。
- 备选没做：
  (A) 把占位直接降级成 `notdec.unknown`（诚实但丢信息，unknown 计数会大涨）；
  (C) 调用点按 callee 的 `MayNonEntryMask` 用“自己的调用前值”补位（对 MayNonEntry 位仍是
      callee 的入口值，解决不了根因）；
  (D) 给寄存器 global 建定义（值仍是假的）。

## 4. 判断标准

- 10 目标 `llvm-as` + `opt -passes=verify` 全 OK；`ctest` 12/12。
- `partial_read`（尤其 `range_entry` 类）与整寄存器 entry load 明显下降。
- unknown 不涨（或涨幅能用“原来就对不上、现在显式化”解释）。
- lighttpd/redis-cli 等与 DWARF 对照：真形参（ABI 参数）不变，只是多出寄存器形参。
- 运行时间不劣化。

## 5. 实现记录

文件：`lib/passes/summary/NativeRegisterSummarySSA.cpp`（`shapeForInternalFunction` 的
`addParamForUnit`，约 `:1723-1760`，+23/-1 行）。

```bash
make -C build -j16 notdec-native-llvm native_register_summary_ssa_test native_register_summary_test
/tmp/fix30_run.sh        # 10 目标，-o /tmp/fix30/*.ll
```

## 6. 量测（未落地：unknown 计数代价见下）

### 6.1 全量版（不跳过浮点/变参）

`/tmp/fix30`（只跑了 tmux/tmux2/tmux3）与 `/tmp/fix29` 对比：

| 目标 | unknown | partial_read(entry) | regload |
|---|---|---|---|
| tmux | 25 → **505** | 272 → 315（267→309） | 473 → 455 |

505 的构成：`call_arg_rewrite_missing_binding:FUN_69630:{RAX=104, RDX=14, ZMM0=104}`（FUN_69630 是
变参函数，668 个调用点）+ 各 callee 的 `call_effect_mixed:*:ZMM0` 共 248（ZMM0 一项 343）。
结论：浮点/变参两类不能要 —— 浮点返回槽要整条 ZMM，而调用点实参绑定按 lane 记；
变参函数的寄存器保存区正好用 RDX/RCX/R8/R9/XMM*，把它们当定参会和变参值打架。

### 6.2 收窄版（跳过浮点返回寄存器 + 变参函数）

`/tmp/fix31` 与 `/tmp/fix29` 对比（收窄 = `!shape.VarArg && !isFloatAbiOutputUnit`）：

| 目标 | unknown | partial_read(entry) | regload | clobber | bytes | verify |
|---|---|---|---|---|---|---|
| tmux（3 次） | 25 → 30 / 30 / 30 | 272 → **230**（267→225） | 473 → 466 | 6832 → 6840/6844/6857 | 21.73M → 21.36M | OK |
| wolfssl | 556 → **689** | 1319 → **1240**（1311→1232） | 138 → 116 | 2361 → 3112 | 37.4M → 38.06M | OK |
| ssh | 65 → **294** | 81 → **67**（75→62） | 16 → 10 | 1255 → 2187 | 12.09M → 12.93M | OK |
| lighttpd | 26 → **84** | 76 → **43**（71→38） | 54 → 48 | 521 → 621 | 4.564M → 4.671M | OK |
| redis-cli | 7 → **13** | 97 → **86**（94→83） | 45 → 44 | 663 → 768 | 4.025M → 4.112M | OK |
| libuv | 8 → **11** | 17 → 13（14→10） | 6 → 5 | 15 | 2.765M → 2.772M | OK |
| ffmpeg | 5 → 5 | 6 → 6（4→4） | 1 → 1 | 133 | 4.694M | OK |
| vsftpd | 0 → 0 | 5 → 3（4→2） | 1 → 1 | 23 | 1.7088M | OK |
| memcached | 0 → 0 | 13 → 13（9→9） | 11 → 8 | 335 | 3.7996M | OK |
| wrk | 0 → 0 | 0 | 0 | 39 | 0.9299M | OK |

unknown 涨幅按目标：lighttpd +58、ssh +229、wolfssl +133、redis-cli +6、libuv +3、tmux +5；
其中 ssh/lighttpd/wolfssl 的涨幅几乎全在 @@call_effect_mixed@@（ssh +218、lighttpd +47、wolfssl 同型），
即"新形参在调用点要值 → 该值上一次定义是某个 Mixed 效果的调用 → 物化 unknown"。unchanged 的目标
（ffmpeg/vsftpd/memcached/wrk）说明这个机制只在部分调用链上被触发。运行时间都在噪声内
（tmux 274→278~286s，wolfssl 626→652s）。

修复本身按预期生效（lighttpd）：

- `fdevent_fdnode_event_set`：`i32 %RDX.arg` → `i64 %RDX.arg`，入口
  `notdec.partial_read.i64.i32(@RDX, 32)` 消失。
- `network_unregister_sock`：多出 `i64 %RDX.arg`，入口 `load i64, ptr @RDX` 消失。
- 它的调用者 `FUN_12cf6`（调用后读 RDX，是返回需求的来源）也多出 `i64 %RDX.arg`，
  值改由形参传递。

代价来自两处二阶效应：

1. 新形参在调用点要值，`call_arg_rewrite_missing_binding` 找不到绑定时物化 unknown
   （lighttpd 只 +5，tmux +3；大头是下面这条）。
2. 第二个 summary 轮次在**已改写过的 IR** 上算 demand，新调用点的实参被当成 observer
   （见 `docs/analysis/simple-efficient-ai-register-elimination.md` 的说明），于是部分 callee 的
   出口 provenance 从 Preserve/Clobber 翻成 Mixed → 调用点按既有规则物化
   `call_effect_mixed` unknown（lighttpd +47，其中 RDX 约 30、RAX 16；tmux 0）。

因此按本文件 §4 的判据（unknown 不涨）**没有落地**：改动存到 `/tmp/fix-entry-param.patch`，
工作区回到 HEAD。后续方向见 §7。

### 6.3 单测

`tests/native_register_summary_ssa_test.cpp` 新增
`testMixedExitReturnRegisterGetsEntryParam`（RDX 只在一条路径写、调用点调用后读 RDX →
出口 Mixed → 必须有 RDX 形参、且不能有入口 load）。有牙：把 `NativeRegisterSummarySSA.cpp`
stash 掉后该测试失败（`mixed exit register did not get an entry parameter`），恢复后通过。
随改动一起回退。

## 7. 后续可选方向

1. **调用点实参兜底**：`NativeRegisterSummarySSA.cpp:8891` 一带
   `binding == nullptr → notdec.unknown` 换成"取调用点该寄存器的按位 SSA 值 / 入口值"
   （只适用寄存器槽；栈槽不适用）。这样新形参不再按调用点各产生一个 unknown，而是每个
   函数一份共享的入口读，`call_arg_rewrite_missing_binding` 一类可能整体下降。
2. **demand 侧**：第二个 summary 轮次不要把"按位 SSA 生成的调用点实参"当 observer
   （或让 demand 跳过这些实参），避免出口 provenance 被翻成 Mixed。

方向 1 更小、收益更直接；方向 2 影响面大。两者都做完后再按 §4 的判据重跑 10 目标。

