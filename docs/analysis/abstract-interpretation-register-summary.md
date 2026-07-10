# Abstract interpretation notes for register summaries

本文只总结 native register summary 这条链路需要的抽象解释基础。
目标不是完整介绍抽象解释，而是把 domain、transfer function、join、meet、fixpoint 讲清楚。

## 基本模型

抽象解释把真实程序状态映射成更小的抽象状态。

对 register summary 来说，真实状态是：

```text
某个程序点上所有机器寄存器的真实 bit-level 值
```

这个太细，不适合跨函数 summary。
这条链路只关心三件事：

```text
当前 register 的值是否可能还是函数入口值
当前 register 的值是否可能是函数内部产生的新值
函数入口值是否可能被真正读取过
```

`CallReturn`、`CallClobber`、`ABIKilled` 这类名字不放进核心抽象域。
它们是调用点消费 summary 后得到的解释，不是 register 当前值本身。

## 抽象域

每个 register 的 cell：

```text
Cell = {
  mayEntry: bool,
  mayNonEntry: bool,
  readEntry: bool
}
```

含义：

- `mayEntry`：当前值可能还是函数入口时的值。
- `mayNonEntry`：当前值可能是函数内部写出来的值，或者被调用函数写出来的值。
- `readEntry`：函数入口值可能被真正读取过。

实现时用 map 存：

```text
Map<Register, Cell>
```

map 里没有某个 register，表示默认 untouched：

```text
mayEntry = true
mayNonEntry = false
readEntry = false
```

这个默认值只适用于已经可达的程序点。
CFG worklist 还需要一个单独的 block 状态：

```text
unreachable
reachable(Map<Register, Cell>)
```

未访问 block 是 `unreachable`，不是“所有 register 都 missing”。
否则稀疏 map 的默认 untouched 会把未到达路径误当成真实入口值路径。

常见状态：

```text
missing / default                         -> untouched
mayEntry=true, readEntry=true             -> read-only input
mayEntry=false, mayNonEntry=true          -> entry value killed
mayEntry=true, mayNonEntry=true           -> path mixed，可能没变也可能变了
函数出口 mayEntry=true, mayNonEntry=false -> caller 可见 preserved
函数出口 mayNonEntry=true                 -> caller 可见可能 changed
```

`writeEvent` 不放在核心域里。
如果后续为了日志想记录“函数体里是否出现过写寄存器指令”，可以加一个 audit bit，例如 `writtenSeen`。
但它不能用于判断 caller 可见 clobber。
callee-saved register 的典型情况是“函数内部写过，但出口恢复了”，caller 只应该看到 preserved。

## Join 和 meet

这个 domain 可以看成几个 bool bit 的 product lattice。

对一个 register：

```text
join mayEntry    = OR
join mayNonEntry = OR
join readEntry   = OR

meet mayEntry    = AND
meet mayNonEntry = AND
meet readEntry   = AND
```

对整个 map：

```text
join(m1, m2)[r] = join(m1.getOrDefault(r), m2.getOrDefault(r))
meet(m1, m2)[r] = meet(m1.getOrDefault(r), m2.getOrDefault(r))
```

对 block state：

```text
join(unreachable, s) = s
join(s, unreachable) = s
join(unreachable, unreachable) = unreachable
```

两个 reachable state 再做上面的 pointwise join。

`join` 和 `meet` 各自满足交换律、结合律、幂等律：

```text
a join b = b join a
(a join b) join c = a join (b join c)
a join a = a
```

`meet` 同理。

这保证 CFG 合流时 predecessor 枚举顺序不影响结果。
但“提前在控制流汇聚点合并”和“枚举所有路径后再合并”等价，不只依赖交换律。
还需要相关 transfer function 对 join 可分配：

```text
F(a join b) = F(a) join F(b)
```

如果只有 monotone，没有 distributive，迭代算法仍然能得到安全 fixpoint，但不一定等于 meet-over-all-paths / join-over-all-paths 的精确结果。

本分析使用 may-style forward analysis，CFG 合流点用 `join`。
`meet` 主要用于说明 lattice 完整性，或者以后派生 must 结论。

## Transfer function

每条指令定义一个 transfer function：

```text
F_inst : State -> State
```

读 register `R`：

```text
readEntry[R] = readEntry[R] OR mayEntry[R]
```

也就是：只有当前值仍可能来自函数入口时，这次 read 才算“可能读取入口值”。
如果入口值已经被覆盖，后续再读这个 register，不会再增加参数证据。

写 register `R`：

```text
mayEntry[R] = false
mayNonEntry[R] = true
```

读写同一个 register 的指令，按机器语义先读后写。

例子：

```asm
mov rdi, 0
use rdi
```

`mov` 之后：

```text
mayEntry=false
mayNonEntry=true
readEntry=false
```

所以后面的 `use rdi` 不会把 `rdi` 标成入口参数。

分支例子：

```asm
if cond:
  mov rdi, 0

use rdi
```

合流点：

```text
mayEntry = true OR false = true
mayNonEntry = false OR true = true
```

后面的 `use rdi` 会设置：

```text
readEntry = true
```

这是对的，因为没走 `if` 的路径上确实读了入口 `rdi`。

## 为什么提前合并是对的

对当前 domain，读写 transfer 都对 `join` 可分配。

读规则：

```text
F_read(s).readEntry = s.readEntry OR s.mayEntry
```

所以：

```text
F_read(a join b).readEntry
= (a.readEntry OR b.readEntry) OR (a.mayEntry OR b.mayEntry)
= (a.readEntry OR a.mayEntry) OR (b.readEntry OR b.mayEntry)
= join(F_read(a), F_read(b)).readEntry
```

其他 bit 不变，也满足同样关系。

写规则：

```text
F_write(s).mayEntry = false
F_write(s).mayNonEntry = true
F_write(s).readEntry = s.readEntry
```

所以：

```text
F_write(a join b)
= F_write(a) join F_write(b)
```

block transfer 是多条指令 transfer 的组合。
如果每条指令都对 join 可分配，组合后仍然可分配。

因此，这个初版 register summary 可以在每个 CFG 合流点提前 join，而不用枚举所有路径。
这正是它能比显式路径枚举便宜的原因。

## Call transfer

direct internal call 使用 callee summary。
known external 使用已经映射到 ABI register range 的 prototype。
unknown external 的输入读取由分析选项决定，但 caller-saved clobber 始终按 ABI 处理。

设 callee 对 register `R` 的出口 summary 是：

```text
callee.mayEntry
callee.mayNonEntry
callee.readEntry
```

套到 caller 的 callsite：

```text
if callee.readEntry:
  caller.readEntry[R] = caller.readEntry[R] OR caller.mayEntry[R]

post.mayEntry[R] =
  callee.mayEntry ? pre.mayEntry[R] : false

post.mayNonEntry[R] =
  (callee.mayEntry ? pre.mayNonEntry[R] : false)
  OR callee.mayNonEntry
```

含义：

- 如果 callee 会读入口 `R`，那么 callsite 当前 `R` 就是参数来源。
- 如果 callee 可能 preserved `R`，caller call 后仍保留 call 前来源。
- 如果 callee 可能写出新 `R`，caller call 后 `R` 可能是 non-entry。

这个 call transfer 也是用 OR、条件选择和常量组成的。
在 callee summary 固定时，它对 caller state 的 `join` 仍然可分配。

### 外部 call 的输入与 clobber 分开处理

外部函数有两类信息：

```text
输入：call 是否读取某个 ABI 参数 register range
输出：call 是否覆盖 caller-saved register range
```

这两类信息不能绑在一起。
unknown external 在第一遍可以假设零输入，但不能因此假设它不 clobber RAX、RDX 等
caller-saved register。

known external 的输入由 prototype 精确给出：

```text
fixed integer 参数 -> 对应 GPR slot
float/double 参数   -> 对应 XMM/ZMM backing range
vararg              -> fixed 参数后按 MaxArgs 限制 ABI fallback
```

`NativeRegisterSummary` 不重新解析 C 类型。
prototype 到 register slot 的映射由 SummarySSA 侧统一完成，再以
`NativeExternalCallShape` 传给 summary。
这样 summary 和最终 LLVM signature 使用同一套 ABI 映射。

unknown external 默认仍保留旧的 `AbiInputs` 策略，保证单独调用
`runNativeRegisterSummary()` 的代码不静默改变。
两遍 SummarySSA 链路显式选择 `NoInputs`。

## 未知外部函数的两遍分析

unknown external 没有函数体，也没有可靠 prototype。
如果第一遍直接假设它读取全部 ABI 参数，caller 的 `ReadEntry` 会被放大。
当前链路改成固定两遍：

```text
第一遍 bottom-up summary，unknown external 零输入
-> 收集 callsite 参数来源
-> 聚合临时 external prototype
-> 第二遍完整 summary
-> SummarySSA 和 signature rewrite
```

### 第一遍

第一遍只需要稳定的 forward effect：

- known external 按 prototype 读取输入。
- unknown external 不读取 ABI 输入。
- 所有 external 仍应用 ABI clobber。
- 不跑 top-down demand。
- 不附最终 metadata。

forward solver 同时维护按 bit 的来源状态：

```text
Entry         来自函数入口
Local         本函数明确写入
CallProduced  前一个 call 的返回值或 clobber
```

一个 ABI slot 的全部 bit 都来自同一来源时，分别映射为：

```text
ForwardedEntry
LocalDefinition
CallProduced
```

不同来源混合时是 `Mixed`，无法判断时是 `Unknown`。
这套来源信息不改变 `Cell` 抽象域，只服务于 callsite 证据。

SCC bottom-up 最后一轮已经在稳定 callee effect 下得到每个 block 的 `In/Out`。
实现直接保存这轮状态并 replay 指令，不再额外调用一次 CFG solver。

### 聚合 callsite

对每个 unknown external callsite，从 arg0 开始统计连续的
`LocalDefinition` 前缀：

```text
Local, Local, Entry -> 2
Local, CallProduced -> 1
Entry, Local        -> 0
```

同一 external 的最终参数数量取所有 callsite 前缀的最大值。
这样不会因为某个条件分支少设置一个可选参数而取到过小结果。

`ForwardedEntry`、`Mixed`、`CallProduced` 和 `Unknown` 不作为强证据。
callsite 不一致会输出 warning；所有 callsite 都是零前缀时保持零参数假设并输出
unresolved warning。
内置和用户 JSON prototype 始终优先，不参与自动推断。

### 第二遍

第二遍使用 known + inferred prototype：

- 重新做 bottom-up effect。
- 做 top-down entry/exit demand。
- 生成最终 metadata。
- 未推断成功的 external 仍按零输入。

第二遍结果是后续 internal signature shape、range planning 和 SummarySSA 的唯一 facts。
参数数量在进入 `FunctionBuilder` 前已经确定，后置流程不再修改 direct external arity。

## Fixpoint

对一个函数，按 CFG 做 forward fixpoint：

```text
in[entry] = reachable(default state)
其他 block 初始为 unreachable
out[bb] = transfer_bb(in[bb])
in[bb] = join(out[pred1], out[pred2], ...)
重复直到不变
```

domain 有限，所以不需要 widening。
每个 register 只有 3 个 bit，block 还有一个 `unreachable/reachable` 标记，状态空间有限。
`mayEntry` 在某条路径上会被写操作置 false，但 transfer function 仍然是 monotone。
worklist 里的 in-state 通过 join 增长，最终会稳定。

对跨函数分析，call graph SCC 也是同一个思想：

```text
SCC 内函数 summary 初始化为当前可用值
反复分析 SCC 内函数
直到所有 summary 不再变化
```

## 保存/恢复寄存器

保存/恢复 callee-saved register 不应该硬塞进核心 transfer function。

原因：

- 它通常依赖 stack/frame slot。
- 第一版不做 stack / memory alias。
- 如果强行在核心域里猜，会影响 `readEntry`，把保存动作误当参数读取。

更好的处理是单独 refinement：

```text
先用核心分析得到 summary
再由专门 pass 匹配 frame-local save/restore
如果证明 R 在所有 return path 恢复入口值
  -> 把出口精化为 mayEntry=true, mayNonEntry=false
  -> 不把纯保存用途的读计入 readEntry
```

这一步只处理确定的 frame-local slot。
不要扩展到一般内存。

如果为了审计保留 `writtenSeen`，callee-saved register 可以是：

```text
writtenSeen=true
mayEntry=true
mayNonEntry=false
```

但 caller 可见效果只看 `mayEntry/mayNonEntry`，不是看 `writtenSeen`。

## 参考资料

- Patrick Cousot, Radhia Cousot, [Abstract Interpretation: A Unified Lattice Model for Static Analysis of Programs by Construction or Approximation of Fixpoints](https://www.di.ens.fr/~cousot/publications.www/CousotCousot-POPL-77-ACM-p238--252-1977.pdf), POPL 1977.
- Gary A. Kildall, [A Unified Approach to Global Program Optimization](https://haoxintu.github.io/files/1-A%20Unified%20Approach%20to%20Global%20Program%20Optimization.pdf), POPL 1973.
- J. B. Kam, J. D. Ullman, [Monotone Data Flow Analysis Frameworks](https://link.springer.com/article/10.1007/BF00290339), Acta Informatica 1977.
