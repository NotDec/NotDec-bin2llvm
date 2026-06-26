# Partial register bit/lane liveness plan

用户原始要求：

> 详细写个文档，先复盘一下当前的算法，然后规划一下怎么改进成支持这种partial write的bit-liveness / lane-liveness模式

## 背景

当前 summary native 链路已经能用 `gtirb` 默认前端跑完整 native IR。`fortune`、`wrk`、`memcached`、`redis-cli` 都已经验证过：IR 能生成，LLVM 22 `llvm-as` 和 `opt -passes=verify` 能通过。

现在剩下的问题不是 verifier 错误，而是还有一批寄存器访问没消掉。`fortune`、`wrk` 主要只剩 entry load；`memcached`、`redis-cli` 还会看到非 entry 的 `@RCX`、`@RDX` 和不少 `ZMM*` load/store。

典型残留长这样：

```llvm
%old = load i64, ptr @RDX
%keep = and i64 %old, -256
%new = or i64 %keep, %low8
store i64 %new, ptr @RDX
```

含义是 partial write：保留旧寄存器的高位，只替换低位。这个模式不能简单当成普通死 store 删除，因为如果后面有人读取完整 `RDX`，高位确实来自旧值。

## 当前算法复盘

### 1. 前置 summary

`NativeRegisterSummary` 先做函数级 summary。它大致回答这些问题：

- 哪些 entry register 可能被读取。
- 哪些 register 退出时仍可能来自 entry。
- 哪些 register 被函数修改。
- 哪些 call 的返回 register 被 caller 真正使用。

这个结果供 `NativeRegisterSummarySSA` 使用。它不是 Ghidra trial/use 链路，而是 summary 链路自己的 bottom-up / top-down 结果。

### 2. SummarySSA 的值化

`NativeRegisterSummarySSA` 负责把 register global 的 load 尽量替换成 SSA value。

核心接口是：

- `readValueBefore(block, unit, beforeInst)`
- `readBlockEntry(block, unit)`
- `readBlockExit(block, unit)`

它按 CFG 追踪某个 register 在某个位置前的当前值：

- 如果前面有 store，就用 store value。
- 如果来自前驱块，就构造 PHI。
- 如果来自函数入口，就生成 entry load。
- 如果来自 call，就按 summary / ABI 判断 preserve、return、clobber、unknown。

这一步会替换普通 register load，并把替换过的 load 标记成：

```text
notdec.register.summary_ssa.replaced
```

### 3. Signature rewrite

当前 `NativeRegisterSummarySSA` 同时做函数签名改写。

它根据 summary 和 ABI：

- 把 external call 的寄存器参数改成 LLVM call operand。
- 把 internal function 的 entry register load 改成 LLVM function parameter。
- 把 demanded return register 改成 LLVM return value。
- 删除已经被签名消费的参数 store。

这一步已经让大量 `store @RDI; call @foo()` 变成 `call @foo(arg)`。

### 4. Residue cleanup

当前还有一个保守的 liveness 清理：

- `removeDeadReplacedLoads()` 删除已经替换且没 use 的 load。
- `removeDeadStoresByLiveness()` 做 register-global 级别的反向 liveness。
- `transferLoadLiveness()` 看到 raw register load，就把这个 global 标记为 live。
- `transferStoreLiveness()` 看到 store，就 kill 这个 global 的 live。
- `transferCallLiveness()` 按 call effect kill 或 read register。

这套 liveness 的粒度是“整个 register global”。也就是说，`@RDX` 要么 live，要么不 live。它不区分低 8 位和高 56 位。

### 5. 已有 partial 处理

当前已有两个很有限的 partial 相关规则：

- lifting 阶段已经尽量只生成最大 backing register，例如 `XMM0/YMM0/ZMM0` 统一到 `ZMM0`。
- `isKeepHighPartialLoadUse()` 能识别简单的 keep-high partial write：

```text
load @REG -> and mask -> or low -> store @REG
```

在 liveness 里，如果某个 load 只服务于这种 keep-high 表达式，就不把它当成真实 read。

这能解决一部分误判，但还不是完整算法。真实 IR 里仍然会残留更多变体，比如：

- partial write 之后又有控制流 merge。
- store value 还被其他指令间接使用。
- SIMD/ZMM 的 lane 组合更复杂。
- 某些路径读取低位，另一些路径读取完整寄存器。

## 当前算法的问题

核心问题是：当前 SummarySSA 只知道“整个寄存器值”，不知道“哪些 bit/lane 被需要”。

例如：

```llvm
%old = load i64, ptr @RDX
%new = (%old & -256) | %low8
store i64 %new, ptr @RDX
...
%use = trunc i64 %new to i8
```

如果后面只用低 8 位，那么 `%old & -256` 没意义，`load @RDX` 可以删。

但如果后面是：

```llvm
%use = add i64 %new, 1
```

那完整 64 位被观察，高 56 位真的依赖旧 `RDX`，不能删。

当前 global-level liveness 分不清这两种情况，所以只能保守保留。

## 改进目标

新增一套 partial register 的 bit/lane demand 分析，目标是回答：

> 某个 register value 的哪些 bit 或 lane 会被后续真实观察？

然后用这个结果消除不必要的 keep-high 依赖。

第一版不要追求完全通用，只解决当前真实问题：

- 整数寄存器：按 bit range，比如 `[0, 8)`、`[0, 32)`、`[0, 64)`。
- SIMD 寄存器：按 lane / byte range，比如 `ZMM0[0:64]`、`ZMM0[64:512]`。
- 不做一般 memory alias。
- 不分析 stack 参数。
- 不改变 lifting 的语义，只在 SummarySSA 层消费已生成的完整寄存器表达式。

## 抽象域

对每个 register global 维护一个 demand mask。

整数寄存器可以用 bit range set：

```text
RDX: { [0,8), [32,64) }
```

SIMD 可以先用 lane/byte range set：

```text
ZMM0: { [0,64), [128,256) }
```

第一版可以统一叫 `DemandMask`：

```text
empty      没有任何 bit/lane 被需要
full       整个寄存器被需要
ranges     若干半开区间
```

join 用 union：

```text
join({[0,8)}, {[8,16)}) = {[0,16)} 或 {[0,8), [8,16)}
```

控制流合并点用 OR 语义：只要任一路径需要某些 bit，合并后就需要。

## Transfer 规则

这是反向分析，从 use 往 def 走。

### 1. 完整读取

如果指令读取完整 register value，比如：

```llvm
add i64 %reg, 1
store i64 %reg, ptr %mem
call foo(%reg)
```

则 demand 是 full。

### 2. 截断读取

```llvm
%lo = trunc i64 %reg to i8
```

只需要 `[0,8)`。

### 3. and mask

```llvm
%x = and i64 %reg, C
```

只需要 mask 中可能影响后续 demand 的 bit。

如果后续只 demand `%x` 的 `[0,8)`，且 `C` 在高位为 1、低位为 0，那么对 `%reg` 的 demand 可能是 empty。

### 4. or

```llvm
%new = or %keep, %low
```

把 downstream demand 分配给两个 operand：

- 对 `keep`：只需求由 `keep` 贡献的 bit。
- 对 `low`：只需求由 `low` 贡献的 bit。

如果能识别 `keep = old & highMask`、`low = value & lowMask`，就可以更精确。

### 5. partial write

识别：

```text
old = load @REG
keep = old & keepMask
new = keep | lowBits
store new, @REG
```

如果 store 后对 `REG` 的 demand 和 `keepMask` 没交集，则 old load 可以删。

如果有交集，则这些高位确实需要 old load，不能删。

### 6. PHI

PHI 的 demand 分发到每个 incoming value：

```text
需求(PHI) = D
需求(incoming_i) += D
```

这是普通 SSA demand 规则。控制流层面仍然是 union。

### 7. call

call 的处理保持保守：

- 如果 register 是 ABI input，按 full demand。
- 如果 external call clobber 某 register，call 前的旧值不需要被 call 后 demand 继承。
- 如果 call 返回某 register，返回值定义覆盖这个 register 的 demanded bits。
- unknown call 第一版按 full clobber 或现有 ABI summary 处理，不新增复杂规则。

## Rewrite 策略

分析得到 demand 后，再做 rewrite。

### 情况 1：keep-high 完全不需要

```llvm
%old = load i64, ptr @RDX
%keep = and i64 %old, -256
%new = or i64 %keep, %low8
store i64 %new, ptr @RDX
```

如果高位没有 demand，可以改成：

```llvm
%new = zext/trunc %low8 to i64
store i64 %new, ptr @RDX
```

后续 dead code 会删掉 `%old` 和 `%keep`。

这里用 0 填高位是否安全，需要按语义区分：

- 如果后续高位无 demand，填 0、poison、undef 在可观察行为上都不应影响。
- 为了 IR 稳定和避免 poison 扩散，第一版建议填 0。

### 情况 2：keep-high 部分需要

如果只需要部分高位，则保留对应 mask：

```text
neededOldMask = demand & keepMask
```

把 `old & keepMask` 改成 `old & neededOldMask`。

这个可以进一步减少依赖，但第一版可以先不做，只做“高位全不需要”的情况。

### 情况 3：完整需要

如果 demand 覆盖 keepMask，则不改。

## 放在 pipeline 的位置

建议放在 `NativeRegisterSummarySSA` 内部，但作为独立阶段：

```text
1. runNativeStackFrameRewrite()
2. runNativeRegisterSummary()
3. SummarySSA rewriteLoads()
4. Signature rewrite planning
5. bit/lane demand analysis
6. partial write rewrite
7. removeDeadStoresByLiveness()
8. rewriteSignatureShapes()
9. post-signature cleanup
```

原因：

- 太早做，没有 SSA value 和 call effect 信息，容易误判。
- 太晚做，signature rewrite 已经删了一些 store/call 信息，分析来源不完整。
- 放在 SummarySSA 值化之后，可以直接利用 `readValueBefore()`、PHI、call effect。

## 实现阶段

### 阶段 1：只做观测和统计

新增 debug 统计：

- 找到多少 keep-high partial write。
- 其中多少 old bits 没有后续 demand。
- 按 register 分类：`RDX/RCX/ZMM0/...`。
- 不改 IR。

判断标准：

- `fortune/wrk/memcached/redis-cli` 能稳定跑。
- 统计能解释当前残留 register access。

### 阶段 2：整数寄存器最小 rewrite

只处理 `i64` register。

只支持这种模式：

```text
load @REG -> and constant keepMask -> or lowValue -> store @REG
```

只在 `demand & keepMask == 0` 时改写。

判断标准：

- `RDX/RCX` 这类整数残留减少。
- 四个 Bench2 目标通过 `llvm-as` 和 verifier。
- 不增加 summary_return helper。

### 阶段 3：跨 basic block demand

把 demand 做成真正 CFG 反向 fixpoint。

- 每个 block 有 `liveInDemand` / `liveOutDemand`。
- join 是 range union。
- 对 PHI 按 incoming edge 分发 demand。

判断标准：

- 不能只在单 basic block 内有效。
- `memcached/redis-cli` 的跨 block partial write 能解释。

### 阶段 4：SIMD/ZMM lane demand

扩展到 `i512` / `ZMM*`。

第一版按 byte range，不理解浮点 lane 类型。

支持：

- `trunc`
- `zext/sext`
- `shl/lshr/ashr` 常量移位
- `and/or/xor` 常量 mask
- `extractvalue` / `insertelement` 如果 IR 里出现

判断标准：

- `ZMM*` raw load/store 明显下降。
- 不把 SIMD 返回签名改成 `i512`。
- verifier 通过。

### 阶段 5：清理和开关

加一个选项，例如：

```text
EnablePartialRegisterDemandRewrite
```

默认是否开启要等真实样例稳定后决定。第一版可以默认关，Bench2 验证稳定后再打开。

## 风险

### 1. 误删旧高位

最大风险是误判高位没人看，实际后面完整寄存器被观察。解决方式：第一版只在明确 downstream demand 不覆盖 high mask 时改。

### 2. poison/undef 语义

如果用 poison 填未需求 bit，后续优化可能把 poison 扩散。第一版建议用 0 填不被 demand 的 bit，避免 IR 语义过激。

### 3. SIMD lane 复杂

ZMM 的真实语义比整数复杂，特别是浮点、向量 lane、mask register。第一版只按 bit/byte demand，不恢复高层 SIMD 类型。

### 4. 和 signature rewrite 顺序耦合

signature rewrite 会删除 call arg store，也会重建函数体。partial rewrite 要在信息还完整的时候做，不能太晚。

## 不做什么

第一版不做：

- 一般 memory alias。
- stack 参数恢复。
- SIMD 类型恢复。
- 任意 LLVM IR 的位级 symbolic execution。
- 旧 heritage 链路支持。
- Ghidra trial/use 风格回退。

## 判断标准

完成第一版后至少要满足：

- `native_register_summary_ssa_test` 通过。
- `fortune`、`wrk`、`memcached`、`redis-cli` 生成 IR 成功。
- 四个目标通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
- `memcached/redis-cli` 的非 entry `@RCX/@RDX` raw access 明显下降。
- 没有新增 RAUW 类型错误、dominance 错误、summary_return helper 回退。

## 结论

partial-register 不是没有算法，而是当前 SummarySSA 的寄存器粒度太粗。

继续补局部 liveness 规则会越来越像启发式。更稳的路线是引入 bit/lane demand：先回答“哪些 bit/lane 真被观察”，再删除不必要的 keep-high old value。

建议先做整数寄存器的最小版本，再扩展到 ZMM lane。不要一开始就试图完整覆盖所有 LLVM 位运算和 SIMD 指令。

## 实现记录

这次先落了最小可工作的第一版，不再把 `isKeepHighPartialLoadUse()` 当最终逻辑，而是在 `NativeRegisterSummarySSA` 里直接加了一层 demand 计算和 rewrite。

- `lib/passes/summary/NativeRegisterSummarySSA.cpp:92-157` 新增 `PartialDemandState`，存每个 value 的 bit demand。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1245-1637` 在 `FunctionBuilder::run()` 里先做 `rewriteLoads()`，再做 `rewritePartialWrites()`，然后继续走原来的 call/signature/liveness 流程。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1406-1577` 新增 `computePartialDemands()`，先从 return、call、branch、switch、icmp/fcmp 这些真正观察点反推 demand，再沿 `trunc/zext/sext/bitcast/phi/select/and/or/xor/shl/lshr/ptrtoint/inttoptr` 往回传播。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1580-1637` 新增 `rewritePartialWrites()`，先匹配 `load @REG -> and mask -> or low -> store @REG`，如果 `keep` 部分没有 downstream demand，就直接把 store 改成只写 `low`，再删掉变成死链的 `or/load`。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:2410-2427, 2918-2968` 把 `PartialDemandCandidates/Matched/Rejected` 汇总并打印到 summary。
- `tests/native_register_summary_ssa_test.cpp:1973-2016` 新增 `testPartialKeepHighStoreIsDemandRewritten()`，确认 `RDX` 的 keep-high 模式在没有高位需求时会被切掉。

验证结果：

- `cmake --build build --target native_register_summary_ssa_test -j2`
- `./build/bin/native_register_summary_ssa_test`
- `fortune` 目标重新跑过，IR 和 verifier 都能过；当前输出里仍然有 `RDX.entry`，但这次新增的 keep-high old load 路径已经能被这层 demand rewrite 覆盖。

后续又补了 `or` / `sext` / `add-sub-mul` 的 demand 传播，并加了一个最小 `ZMM0` keep-high 回归，证明整数和最小 ZMM keep-high 都能切掉。
但真实 `memcached` / `redis-cli` 里还残着不少 `ZMM*`，而且它们多半不是单条 keep-high，而是更长的向量链：

- `load i512 @ZMM*`
- `and i512 ...`
- `or disjoint i512 ...`
- `trunc i512 -> i128/i64`
- `lshr/shl i512`
- 再回写 `store i512 @ZMM*`

这说明当前 plan 里写的 bit/lane demand 骨架已经能跑，但还没有把 ZMM 的 lane 来源/去向按这种组合链完整建模。继续往下做，已经不是“补一个局部规则”了，而是要单独定向量 lane 的语义边界。

### 2026-06-26 更新：推进 ZMM lane demand

这轮继续按 bit/lane demand 路线推进，没有回到旧的 keep-high 局部 liveness 规则。

代码改动：

- `lib/passes/summary/NativeRegisterSummarySSA.cpp:92` 给 `PartialDemandState` 补注释，明确它是反向 bit demand，不把 register store 当真实观察点。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1299` 新增 `lshrSourceDemand()`，修正 `trunc(lshr x, N)` 反推 demand 时的方向：结果低位对应 source 的 `[N, N+width)`，不是 source 的低位。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1307` 新增 `isDisjointOr()`，只在 LLVM 标记为 `or disjoint` 时做精确 operand demand 拆分。普通 `or/xor` 仍保守传给两边。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1381` 新增 `rewriteDeadKeepHighParts()`，在 store value 的 `or disjoint` 组合链里，把没有 downstream demand 的 `load @REG -> and keepMask` 子表达式替换成 0，再交给 DCE 删除旧 load 链。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1567` 把普通内存 store value 作为真实观察点；register global store 仍不作为观察点，因为是否需要由后续 load / call / return 决定。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1662` 调整 `or` transfer：只有 `or disjoint` 使用已知贡献 mask 拆 demand，避免普通 `or` 误删。
- `tests/native_register_summary_ssa_test.cpp:2062` 新增 `testPartialZmmDisjointLaneChainIsDemandRewritten()`，覆盖 `and keep-high -> or disjoint -> shl/or disjoint -> store -> lshr/trunc use` 这种更接近真实 ZMM lane 的链。

验证：

- `cmake --build build --target native_register_summary_ssa_test -j2`
- `./build/bin/native_register_summary_ssa_test`
- `cmake --build build --target notdec-native-llvm -j2`
- `build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached --all-confirmed --summary-json-out /tmp/memcached-summary-bitlane-new.json -o /tmp/memcached-bitlane-new.ll`
- `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/memcached-bitlane-new.ll -o /tmp/memcached-bitlane-new.bc`
- `/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/memcached-bitlane-new.bc -o /tmp/memcached-bitlane-new.verified.bc`
- `/usr/bin/time -f 'elapsed=%e user=%U sys=%S maxrss=%M' build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --summary-json-out /tmp/fortune-bitlane-summary.json -o /tmp/fortune-bitlane.ll`
- `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/fortune-bitlane.ll -o /tmp/fortune-bitlane.bc`
- `/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/fortune-bitlane.bc -o /tmp/fortune-bitlane.verified.bc`
- `build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk --all-confirmed --summary-json-out /tmp/wrk-bitlane-summary.json -o /tmp/wrk-bitlane.ll`
- `build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/redis-cli --all-confirmed --summary-json-out /tmp/redis-cli-bitlane-summary.json -o /tmp/redis-cli-bitlane.ll`
- `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/wrk-bitlane.ll -o /tmp/wrk-bitlane.bc`
- `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/redis-cli-bitlane.ll -o /tmp/redis-cli-bitlane.bc`
- `/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/wrk-bitlane.bc -o /tmp/wrk-bitlane.verified.bc`
- `/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/redis-cli-bitlane.bc -o /tmp/redis-cli-bitlane.verified.bc`

效果：

| 文件 | ZMM load | ZMM store | RDX load | RDX store | RCX load | RCX store |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 旧 memcached `module-all.ll` | 98 | 93 | 71 | 864 | 50 | 726 |
| 新 `/tmp/memcached-bitlane-new.ll` | 34 | 26 | 2 | 0 | 1 | 5 |
| 新 `/tmp/fortune-bitlane.ll` | 0 | 0 | 1 | 0 | 0 | 0 |
| 新 `/tmp/wrk-bitlane.ll` | 0 | 0 | 1 | 0 | 0 | 0 |
| 新 `/tmp/redis-cli-bitlane.ll` | 12 | 10 | 4 | 3 | 3 | 2 |

fortune 本轮耗时：`elapsed=8.59 user=8.56 sys=0.03 maxrss=170008`。

剩余问题：

- memcached 里还剩 34 个 ZMM load 和 26 个 ZMM store。抽样看，有些是 entry load，属于真实输入候选；有些 store 后低 128 位又被写回内存，不能按死 keep-high 删除。
- 这轮只把 `or disjoint` 组合链纳入正式 demand transfer。更复杂的 SIMD 语义，比如向量 shuffle、浮点 lane、mask register，还没有建模。
- 四个当前关注目标 `fortune`、`wrk`、`memcached`、`redis-cli` 都已重新生成并通过 LLVM 22 verifier。
- `redis-cli` 还剩少量 RCX/RDX/ZMM 残留。抽样看包含 entry load、真实 register store、以及 store 后仍被观察的 ZMM 值；下一步应逐个分类，而不是继续扩大 keep-high 删除。
- `redis-cli` 的 `powerLawRand` 里还看到 `pow` 相关 ZMM 残留：当前 IR 仍是 `call void @pow(i64 0, ...)`，前后用 `store @ZMM3` 表达浮点参数/返回。这不是 bit/lane demand 能单独解决的问题，而是 external FP/SIMD ABI signature rewrite 没覆盖。是否把 `double pow(double,double)` 这类 libc/libm 原型和 XMM/ZMM 参数返回纳入 summary signature rewrite，需要单独决策。

复杂度评分：

- 实现效果：8/10。真实 memcached 残留明显下降，且 verifier 通过。
- 理解成本：5/10。新增的是反向 demand transfer，局部集中在 `NativeRegisterSummarySSA`，但 bit mask 方向需要小心。
- 维护成本：5/10。规则仍基于 LLVM integer IR，后续 SIMD 专用指令会继续扩展 transfer。

更好的方案：

长期看，最好把 `DemandMask` 从单个 `APInt` 抽成小类，统一处理 bit range、lane range、mask 截断和 shift。当前先用 `APInt` 是为了少改代码，并且已经能覆盖整数和一批 ZMM lane 组合。
