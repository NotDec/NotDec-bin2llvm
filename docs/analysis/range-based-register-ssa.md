# Range-based Register SSA

本文说明 `NativeRegisterSummarySSA` 当前的 range-aware register SSA 是怎么工作的。
写法按 Braun SSA 的概念来对照，但重点是当前实现，不是重新介绍论文。

相关文件：

- `docs/analysis/braun-ssa-construction.md`
- `lib/passes/summary/NativeRegisterSummarySSA.cpp`
- `logs/20260704-03-bin2llvm-complete-range-aware-summary-ssa-plan.md`

## 一句话

当前 register SSA 的核心仍是 Braun 风格：

```text
读寄存器 range 时，先看当前 block 的 currentDef；
没有就递归读 predecessor exit；
合流处按需建 PHI；
循环里先注册 PHI 打断递归；
最后补齐 pending PHI，并删除 trivial PHI。
```

和早期实现不同的是，Braun 里的 `variable` 现在不是整个寄存器，而是一个寄存器
bit range：

```text
Variable = RegisterRangeKey(global, bit_offset, bit_width)
```

所以 `RAX[0:32]`、`RAX[32:32]`、`ZMM0[0:64]` 可以分别有自己的 SSA value 和
自己的 PHI。完整寄存器读写只是这些 range 的拼接和拆分，不再是一条独立的
whole-register SSA 主路径。

## 为什么不能继续用 whole-register variable

Braun SSA 要求 variable 有稳定的一等身份。`readVariable` 和 `writeVariable` 必须
维护同一套 `currentDef`。

早期 whole-register 做法把 `RAX`、`ZMM0` 当 variable，partial read/write 只是旁路
特判。这会带来几个问题：

- 低位读写会拖入高位依赖。比如只读 `EAX`，却可能生成 `RAX` 的 `i64` PHI。
- SIMD 低 lane 会拖入整 `ZMM0`，PHI 类型容易变成 `i512`。
- partial write 如果没有写进同一套 `currentDef`，后续 range read 还要反向扫描补洞。
- call 参数、返回值、clobber、entry input 和 dead-store liveness 容易继续按整寄存器扩散。

range-based 的目标就是把“寄存器状态”统一成 range 变量，避免一部分逻辑走整寄存器，
另一部分逻辑走局部 matcher。

## 和 Braun SSA 的对应关系

```text
Braun variable
  -> RegisterRangeKey
     例如 RAX[0:32]、RAX[32:32]、ZMM0[0:64]

Braun writeVariable(var, block, value)
  -> writeSegment() / writeAccessRange()
     把某个 range 的新定义写入 CurrentDef[(block, range)]

Braun readVariable(var, block)
  -> readRangeBefore() / readRangeEntry() / readRangeExit()
     从 use 点向前找当前 range 的 reaching definition

Braun currentDef[var][block]
  -> CurrentDef[(block, RegisterRangeKey)] = RangedSSAValue
     block 内当前已知的 range 定义

Braun incomplete phi
  -> PendingRangePhi[(block, range)]
     递归或 pending 状态下先放进去，后面补 incoming

Braun seal/finalize
  -> finalizePendingPhis()
     当前 LLVM CFG 已经完整，不需要动态 seal block，但仍要补齐 PHI incoming

Braun trivial phi deletion
  -> simplifyRangePhi() + Replacement + DeadPhis
     self reference 不算真实不同值，只有一个真实 incoming 时删 PHI
```

## 总体流程

`FunctionBuilder::run()` 里和 range SSA 直接相关的主流程是：

```text
collectAccesses()
planRegisterRanges()

rewritePartialReads()
foldDuplicatePartialReadXors()
rewriteLoads()

addIndirectCallsiteShapes()
collectSignatureCallArgs()
collectFunctionReturnValues()
rewritePartialWrites()

finalizePendingPhis()
removeDeadStoresByLiveness()
removeDeadPartialReads()
removeDeadReplacedLoads()
removeDeadEntryReads()
eraseDeadPhis()
attachMetadata()
```

其中真正构造 range SSA 的主干是：

1. `planRegisterRanges()` 先决定每个 register global 要拆成哪些 canonical range。
2. `rewritePartialReads()` 和 `rewriteLoads()` 从 use 点触发 range read。
3. `readRangeBefore()` 在当前 block 内顺序推进 `CurrentDef`。
4. 当前 block 找不到定义时，`readRangeEntry()` / `readRangeExit()` 递归读 predecessor。
5. 合流和循环通过 `ensureRangePhi()` / `completeRangePhi()` 建 range PHI。
6. call 参数、函数返回、entry input 都通过 range read/materialize 接入同一套 SSA。
7. residue cleanup 再删掉已经被替换的 load、partial helper 和死 store。

## 核心状态

### RegisterRangeKey

`RegisterRangeKey` 是当前 SummarySSA 的 variable：

```cpp
struct RegisterRangeKey {
  llvm::GlobalVariable *Global;
  uint64_t BitOffset;
  uint32_t BitWidth;
};
```

它描述的是同一个 lifted register global 的某段 bit range。这个 range 不是任意临时访问；
它必须来自 `planRegisterRanges()` 规划出来的 canonical segment。

### RangedSSAValue

`CurrentDef` 里不只存裸 `llvm::Value *`，而是存：

```cpp
struct RangedSSAValue {
  llvm::Value *Value;
  RegisterRangeKey CoveredRange;
};
```

原因是一个 segment 的当前值可能来自更宽的 value。例如一次完整 `RAX[0:64]` 写入后，
`RAX[0:32]` 和 `RAX[32:32]` 都可以指向同一个 `%rax64`，但 `CoveredRange` 记录这个
value 覆盖的是 `[0:64]`。

读取单个 segment 时：

- 如果 `CoveredRange == range`，直接返回 `Value`。
- 如果 `CoveredRange` 覆盖当前 `range`，用 extract materialize 该 segment。
- 如果覆盖不了，说明当前定义不能服务这个 range，不能当成整寄存器 fallback。

### CurrentDef

```text
CurrentDef[(block, range)] = RangedSSAValue
```

这是 block-local 的当前定义缓存。它负责同一个基本块内的顺序语义：

```text
store RAX
read AL
partial_write EAX
read RAX
```

这些读写不应该每次都从 use 点重新反向扫描，而是由 `transferRangeBlockUntil()` 从 block
开头按顺序推进到读点。

### EntryRangeValue / ExitRangeValue

```text
EntryRangeValue[(block, range)] -> value at block entry
ExitRangeValue[(block, range)]  -> value at block exit
```

这两者对应 Braun 递归里的跨 block cache。`readRangeEntry()` 读 block 入口值；
`readRangeExit()` 读 predecessor exit 值。

### PendingRangePhi / ResolvingRangeEntry

`PendingRangePhi` 存按需创建但可能还没完整 incoming 的 range PHI。
`ResolvingRangeEntry` 是递归保护。

遇到循环时，`readRangeEntry(header, RAX[0:32])` 可能在递归中再次读到自己。
这时不能继续递归，而是返回已经创建的 PHI：

```text
if ResolvingRangeEntry contains (header, RAX[0:32]):
  return ensureRangePhi(header, RAX[0:32])
```

这就是 Braun 里“先创建 PHI 并写入 currentDef，再递归查 operands”的同一个点。

### UnknownCurrentDef

未知 call effect 会把某个 range 标成 unknown。
这和“没有当前定义”不同。

- 没有当前定义：可以继续读 block entry。
- 当前定义未知：不能静默回到 entry，否则会把 call clobber 后的值误认为 call 前的值。

所以 `readRangeBefore()` 在看到 `UnknownCurrentDef[(block, range)]` 时直接失败，让上层选择
unknown materialization 或跳过替换。

### CurrentDefPosition

`CurrentDefPosition[block]` 记录当前 block 的 range transfer 已经推进到哪条指令。

这样同一个 block 内多个读点不用每次从头扫描。若缓存位置已经被删，或者当前读点早于缓存
位置，就清空该 block 的 range state 并重建。

## range planner

range SSA 不能直接把每次访问的原始 bit range 都当 variable。原因是访问会互相包含：

```text
AL   = RAX[0:8]
AH   = RAX[8:8]
EAX  = RAX[0:32]
RAX  = RAX[0:64]
```

如果 `RAX[0:8]` 和 `RAX[0:32]` 同时都是 variable，那么一次 `EAX` 写入会“部分定义”
`AL`，这不符合 Braun 的简单 variable 语义。

当前做法是先收集 boundary，再把相邻 boundary 之间的区间作为 canonical segment。
`planRegisterRanges()` 的来源包括：

- 每个 register 的 `0` 和 full width。
- raw register load/store 的完整宽度。
- `partial_read` / `partial_write` helper 的访问范围。
- ABI input/output slot。
- ABI killed / unaffected register。
- summary facts 里的 `EntryDemandMask` / `ExitDemandMask` 连续区间。

规划后，`plannedRangesCovering(global, offset, width)` 必须能用若干相邻 segment 完整覆盖
一次访问。覆盖不了时返回空，调用方不能静默退回 whole-register SSA。

## block 内算法

Braun 论文通常是边生成 IR 边调用 `writeVariable` / `readVariable`。
这里是在已有 LLVM IR 上跑 pass，所以 block 内使用 forward transfer：

```text
readRangeBefore(block, range, before):
  transferRangeBlockUntil(block, before)

  if CurrentDef[(block, range)] exists:
    return currentSegment(block, range)

  if UnknownCurrentDef[(block, range)] exists:
    return null

  return readRangeEntry(block, range)
```

`transferRangeBlockUntil()` 从上次处理位置继续扫描到 `before`。每条相关指令由
`transferRangeInstruction()` 转成 range write 或 unknown：

### store

raw register store：

```text
store %v, @RAX
```

被看成：

```text
writeAccessRange(RAX, 0, fullWidth, %v)
```

如果 `%v` 宽度正好覆盖整个访问，`writeCoveredValue()` 会让覆盖范围内的每个 planned segment
都指向同一个 `%v`，并记录 `CoveredRange = RAX[0:fullWidth]`。

### partial_write

`partial_write` helper 被看成真实 range write：

```text
writeAccessRange(global, bitOffset, writeWidth, value)
```

它只定义覆盖到的 segment，不会杀掉其它 segment。未覆盖的高位或低位继续保留原来的
`CurrentDef`，后续 full read 需要时再拼接。

### call

call effect 分四类：

- `Preserve`：不改 `CurrentDef`。
- `ReturnValue`：每个受影响 range 写入 `callRangeValue(call, range, "return")`。
- `Clobber`：每个受影响 range 写入 `callRangeValue(call, range, "clobber")`。
- `Unknown`：把 range 放进 `UnknownCurrentDef`，禁止回退到 entry。

这部分是 Braun 论文没有的机器语义。实现上仍要落到 `writeVariable` 的等价动作：
call return/clobber 必须写入 range `CurrentDef`，不能旁路成整寄存器 helper。

## 跨 block 算法

当前 CFG 已经完整，所以所有 block 可以看成 sealed。
但 pending PHI 仍然需要，因为递归和循环会先创建 PHI，再补 incoming。

### readRangeEntry

简化流程：

```text
readRangeEntry(block, range):
  if EntryRangeValue has (block, range):
    return resolve(cached)

  if ResolvingRangeEntry has (block, range):
    return ensureRangePhi(block, range)

  mark ResolvingRangeEntry

  preds = predecessors(block)
  if preds.empty:
    value = entryRangeInput(range)
  else if preds.size == 1:
    value = readRangeExit(preds[0], range)
    if PendingRangePhi has (block, range):
      value = completeRangePhi(block, range)
  else:
    value = completeRangePhi(block, range)

  unmark ResolvingRangeEntry
  EntryRangeValue[(block, range)] = resolve(value)
  return EntryRangeValue[(block, range)]
```

这和 Braun 的 `readVariableRecursive` 是同一个结构：

- 没有 predecessor：读函数入口的初始值。
- 一个 predecessor：直接读 predecessor exit。
- 多个 predecessor：建 PHI。
- 递归回到同一个 `(block, range)`：先返回 PHI 打断循环。

### readRangeExit

`readRangeExit(block, range)` 读的是 block terminator 前的值：

```text
terminator = block.getTerminator()
value = readRangeBefore(block, range, terminator)
```

如果 value 支配 terminator，就缓存到 `ExitRangeValue`。这里比纯 Braun 多了 dominance
检查，因为当前 pass 在已有 LLVM IR 上插入和替换 value，不能留下不支配 use 的值。

### ensureRangePhi

`ensureRangePhi(block, range)` 会：

1. 在 block 的 PHI 区创建一个 LLVM PHI。
2. PHI 类型固定为 `rangeType(range)`，也就是 segment 宽度。
3. 写入 `PendingRangePhi[(block, range)]`。
4. 立刻写入 `EntryRangeValue[(block, range)]`。

第 4 步很关键。没有它，循环 back edge 递归回来时就看不到占位 PHI，会继续递归。

### completeRangePhi

`completeRangePhi(block, range)` 给 PHI 补 incoming：

```text
for pred in predecessors(block):
  incoming = readRangeExit(pred, range)
  incoming = rangeTypedValueOrUnknown(incoming, range)
  if incoming does not dominate pred terminator:
    incoming = unknownRangeBefore(pred terminator, range)
  phi.addIncoming(incoming, pred)

return simplifyRangePhi(phi, range)
```

这里有两个现实处理：

- 类型不对时 materialize range unknown，而不是生成 malformed PHI。
- incoming 必须支配 predecessor terminator；否则不能作为该 edge 的 incoming。

### finalizePendingPhis

虽然 CFG 已经完整，递归过程中仍可能留下未完整 PHI。
`finalizePendingPhis()` 会反复扫描 `PendingRangePhi`，对 incoming 数不足的 PHI 调
`completeRangePhi()`，直到不再变化。

最后保留下来的 PHI 必须满足：

```text
phi.getNumIncomingValues() == pred_size(phi.getParent())
```

## trivial PHI 删除

`simplifyRangePhi()` 只处理 complete PHI。
它按 Braun 的 trivial PHI 规则做：

- self reference 不算真实 incoming。
- 如果所有非 self incoming 都是同一个 value，就把 PHI 替换成这个 value。
- 如果只有 self，没有真实 incoming，就创建 range unknown。

替换后：

- `Replacement[phi] = same`
- `phi.replaceAllUsesWith(same)`
- `DeadPhis.insert(phi)`

后续所有读取 value 的地方都要走 `resolve()`，避免继续拿到已替换 PHI。
`eraseDeadPhis()` 最后物理删除 use 已清空的 PHI。

## access range 的读写

### 读一个 access

`readAccessRange(global, offset, width, before, name)` 负责读任意访问范围。

它先尝试 `tryCurrentCoveredAccess()`：

```text
如果当前 block 的每个 covered segment 都来自同一个更宽 RangedSSAValue，
直接从这个更宽 value extract 出目标 access。
```

这能避免完整写后马上读低位时生成一堆拆分/拼接。

如果不能走 covered fast path，就调用：

```text
plannedRangesCovering(global, offset, width)
assembleRangeRead(ranges, offset, width)
```

单 segment 访问直接走 `readRangeBefore()`。
多 segment 访问会逐段读，再按 bit offset 低到高插回目标整数。

### 写一个 access

`writeAccessRange()` 负责把一次写入拆到 planned ranges：

- 如果 source value 正好覆盖这次 access，用 `writeCoveredValue()` 让所有 segment 共享这个 value。
- 否则对每个 segment 从 source extract，再 `writeSegment()`。

这让 full store 和 partial write 都落到同一套 range `CurrentDef`。

## entry input

入口 block 没有 predecessor 时，`readRangeEntry()` 会调用 `entryRangeInput(range)`。

它的语义是“函数入口时这个 register range 的值”：

- 如果处于 post-signature cleanup，且 range 被新函数参数覆盖，直接从参数 extract。
- 如果 summary facts 认为这个 range 不可能来自 entry，返回 null。
- 如果 range 是完整寄存器，创建带 `notdec.register.summary_ssa.entry` metadata 的 entry load。
- 如果 range 是窄范围，创建带 `notdec.register.summary_ssa.range_entry` metadata 的 entry partial read。

这些 entry value 是 SSA 构造的边界输入，不是 whole-register SSA fallback。
签名重写后，`rewriteInternalFunctionBody()` 会根据 metadata 把 entry load / range entry 替换成
真实函数参数或参数子范围。

## call 参数和返回

call 参数读取不再从整寄存器 cache 取值。
`readSlotValueBefore()` 会按 ABI/signature slot 调：

```text
readSlotRangeBefore(before, slot)
  -> plannedRangesCovering(slot.Unit->Global, slot.OffsetBits, slot.SizeBits)
  -> assembleRangeReadIfDominating(...)
  -> cast to slot type
```

函数返回值收集也是同一条路线：

```text
collectFunctionReturnValues()
  -> readSlotValueBefore(ret, returnSlot)
```

call 自己对寄存器的定义由 `callRangeValue()` materialize：

- return range：`notdec.register.summary_return.<type>()`
- clobber range：`notdec.register.summary_clobber.<type>()`

helper 带 metadata：register name、kind、bit offset、bit width、call index。
签名重写后，如果 call 已经变成显式返回值，`callRangeValue()` 会优先从新 call result 里
extract 对应 range。

## range-aware liveness

死 store 删除也使用 `RegisterRangeKey`。

核心规则：

- raw full load 让该 global 的所有 planned ranges 变 live。
- partial read 只让对应 planned ranges 变 live。
- full store kill 该 global 的 live ranges。
- partial write 只 kill 写到的 ranges，没写到的 ranges 保持 live。
- call return/clobber kill 受影响 ranges。
- call read 按 ABI/signature 让输入 ranges 变 live。
- function exit 用 summary 的 `ExitDemandMask` 只加入实际 demanded ranges。

这和 SSA 主路径配套：低位需求不会再自动扩散成整个 register live。

## 和纯 Braun SSA 的不同

当前实现不是论文的最小样例，有这些差别：

1. CFG 已经存在。
   不需要边建 CFG 边 seal block，但仍需要 `PendingRangePhi` 和 `finalizePendingPhis()`。

2. block 内不是直接生成 SSA IR。
   代码先有 lifted LLVM IR，再由 `transferRangeBlockUntil()` 顺序解释 register event，维护
   `CurrentDef`。

3. variable 是 planned range，不是源语言变量。
   任意 access 必须先映射到 canonical segments。

4. 一个 `CurrentDef` 可以来自更宽 value。
   `RangedSSAValue::CoveredRange` 是为 full store / full return / common extract 减少无谓拆分。

5. 有 ABI 和 call effect。
   Preserve、return、clobber、unknown 都要映射成 range state 操作。

6. 有 unknown value。
   当 incoming 缺失、类型不对、call effect 不明时，宁可显式 unknown，也不能留下 malformed PHI
   或回到旧 entry value。

7. 有 dominance 检查。
   LLVM PHI incoming 和普通 use 都必须满足支配关系。

8. 有签名重写。
   entry range 和 call return range 可能先用 helper/metadata 表达，后续再替换成显式参数或
   显式返回值。

9. 只做 trivial PHI 删除。
   Braun 论文里 irreducible CFG 的 SCC 后处理当前不是主线；当前目标是 verifier 正确、
   incoming 完整、trivial PHI 不残留。

## 当前不应再有的路径

`NativeRegisterSummarySSA` 的 old whole-register SSA 主路径已经删除。
当前主实现里不应再出现独立的：

- `EntryValue`
- `ExitValue`
- whole-register `PendingPhi`
- whole-register `EntryInputs`
- whole-register `CallValues`
- `readBlockEntry()` / `readBlockExit()`
- `ensurePhi()` / `completePhi()` 这类整寄存器 PHI 路径

`readValueBefore()` 现在只是 full register read 的 wrapper：

```text
readValueBefore(unit)
  -> readFullRangeValueBefore(unit)
  -> readAccessRangeIfDominating(global, 0, fullWidth)
```

也就是说 full register 读取仍然走 range SSA，只是在 use 点把所有 needed segments 组装成
完整 register 类型。

## 实现时要守住的点

1. range variable 必须来自 `plannedRangesCovering()`，不要临时把任意 access 当 variable。
2. `writeSegment()` 写入的 value 类型必须等于 range 类型。
3. `writeCoveredValue()` 必须显式记录 `CoveredRange`，不能靠裸 value 猜完整寄存器。
4. `readRangeBefore()` 必须先推进 block-local transfer，再读 `CurrentDef`。
5. `UnknownCurrentDef` 不能当成 cache miss；它表示不能回退到 entry。
6. 多 predecessor 和递归回边必须先 `ensureRangePhi()`，再读 predecessor。
7. range PHI 类型必须是 segment 宽度，不是 whole register 宽度。
8. pending PHI 最终必须 incoming 完整。
9. trivial PHI 替换后，后续读取都要经过 `resolve()`。
10. dominance 失败不能用 whole-register fallback 掩盖。
11. entry/call helper 是边界 value 和签名重写桥接，不是第二套 SSA。
12. liveness、call 参数、函数返回都要继续按 range 做，不能把低位需求扩成整寄存器。

## 小例子

### 分支低位写入

```text
entry:
  if cond goto a else b

a:
  write RAX[0:32] = x
  goto merge

b:
  write RAX[0:32] = y
  goto merge

merge:
  read RAX[0:32]
```

当前 SSA 应建：

```text
merge:
  %rax_low = phi i32 [x, a], [y, b]
```

不应该建 `phi i64`，也不应该读取整 `RAX` 再 trunc。

### partial write 后 full read

假设 planner 把 `RAX` 切成：

```text
RAX[0:32]
RAX[32:32]
```

执行：

```text
write RAX[0:32] = x
read RAX[0:64]
```

full read 会读两个 segment：

```text
low  = read RAX[0:32]
high = read RAX[32:32]
rax  = insert low/high into i64
```

高 32 位来自之前 reaching definition、entry input、PHI 或 unknown。
低 32 位不会强迫高 32 位一起进入同一个 `i64` PHI。

### unknown call

```text
write RAX[0:64] = x
call unknown()
read RAX[0:64]
```

如果 call effect 对 `RAX` 是 unknown，transfer 会标记相关 ranges 为 `UnknownCurrentDef`。
后面的 read 不能回到 call 前的 `x`，也不能回到 function entry；只能失败并由上层选择
unknown materialization 或跳过替换。

## 读代码入口

建议按这个顺序读当前实现：

1. `FunctionBuilder::run()`：看整体 pass 顺序。
2. `planRegisterRanges()` / `plannedRangesCovering()`：看 variable 怎么定。
3. `readAccessRange()` / `assembleRangeRead()`：看 use 点怎么读任意访问。
4. `transferRangeBlockUntil()` / `transferRangeInstruction()`：看 block 内 writeVariable。
5. `readRangeBefore()`：看 block-local currentDef 和 entry recursion 的分界。
6. `readRangeEntry()` / `readRangeExit()`：看 Braun 递归。
7. `ensureRangePhi()` / `completeRangePhi()` / `simplifyRangePhi()`：看 PHI 生命周期。
8. `entryRangeInput()` / `callRangeValue()`：看入口和调用边界。
9. `removeDeadStoresByLiveness()`：看 range liveness 和 residue cleanup。
