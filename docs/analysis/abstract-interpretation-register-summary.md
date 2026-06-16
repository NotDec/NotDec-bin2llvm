# Abstract interpretation notes for register summaries

本文只总结 native register summary 这条链路需要的抽象解释基础。
目标不是完整介绍抽象解释，而是给后续实现里的 domain、transfer function、join、meet、fixpoint 一个清楚的说法。

## 基本模型

抽象解释把真实程序状态映射成更小的抽象状态。

对 register summary 来说，真实状态是：

```text
某个程序点上所有机器寄存器的真实 bit-level 值
```

这个太细，不适合跨函数 summary。
我们只保留和参数/返回/寄存器消除有关的信息：

```text
某个 register 的入口值是否被读过
某个 register 是否被改过
某个 register 在函数出口是否还等于入口值
```

也就是不要在 domain 里区分 `CallReturn`、`CallClobber` 这种来源名。
这些是调用点解释结果，不是核心抽象域。

## 抽象域

第一版可以把每个 register 的抽象 cell 写成：

```text
Cell = {
  evidence: bitset { Read, Modified },
  relation: bitset { SameAsEntry, DifferentFromEntry }
}
```

含义：

- `Read`：入口值被用过。
- `Modified`：这个 register 被写过，或者当前值可能已经不是入口值。
- `SameAsEntry`：当前值可能等于函数入口值。
- `DifferentFromEntry`：当前值可能不是函数入口值。

实现时用 map 存：

```text
Map<Register, Cell>
```

map 里没有某个 register，表示默认值：

```text
evidence = {}
relation = { SameAsEntry }
```

也就是 untouched。

常见状态可以读成：

```text
missing                 -> untouched
{Read}, Same            -> read-only
{Modified}, Different   -> modified
{Read, Modified}, ...   -> read and modified
Same at all exits       -> preserved
Same/Different mixed    -> conditional or unknown enough, consumer 保守处理
```

`preserved` 不是 domain 里的原子值。
它是函数出口 summary 的派生结果：所有 return path 上这个 register 的 relation 都只有 `SameAsEntry`。

## Join 和 meet

这个 domain 可以看成有限 powerset lattice 的 product。

对一个 register：

```text
join evidence  = bitset union
meet evidence  = bitset intersection
join relation  = bitset union
meet relation  = bitset intersection
```

对整个 map：

```text
join(m1, m2)[r] = join(m1.getOrDefault(r), m2.getOrDefault(r))
meet(m1, m2)[r] = meet(m1.getOrDefault(r), m2.getOrDefault(r))
```

union 和 intersection 都满足交换律：

```text
A ∪ B = B ∪ A
A ∩ B = B ∩ A
```

所以 pointwise join / meet 也满足交换律：

```text
join(m1, m2) = join(m2, m1)
meet(m1, m2) = meet(m2, m1)
```

它们也满足结合律和幂等律。
这对 CFG 合流很重要：basic block predecessor 的枚举顺序不应该影响结果。

本分析是 may-style forward analysis，CFG 合流点主要用 join。
meet 主要用于说明 lattice 完整性，或者后续如果需要“所有路径共同成立”的判定，可以在派生 summary 时使用。

## Transfer function

每条指令定义一个 transfer function：

```text
F_inst : State -> State
```

规则保持简单：

- 读 register `R`
  - 如果 `relation(R)` 包含 `SameAsEntry`，标记 `Read`。
  - 状态里的当前值 relation 不变。
- 写 register `R`
  - 标记 `Modified`。
  - `relation(R)` 变成 `{ DifferentFromEntry }`。
- 不考虑的 register
  - 直接跳过，不读不写。
- direct call
  - 把 callee summary 实例化到 caller 当前 state。
- external / indirect call
  - 用 ABI fallback 构造一个 synthetic summary，再按同样规则实例化。

call 的实例化规则：

```text
如果 callee reads R:
  当前 caller state[R] 是 callsite 参数来源
  如果 caller relation(R) 包含 SameAsEntry，标记 caller 的 Read

如果 callee exit relation(R) 包含 SameAsEntry:
  caller call 后 relation(R) 包含 call 前 relation(R)

如果 callee exit relation(R) 包含 DifferentFromEntry:
  caller call 后 relation(R) 包含 DifferentFromEntry
  caller evidence(R) 加 Modified
```

这样 `SameAsEntry` 始终是相对当前函数入口解释的。
callee 的 preserved register 套到 caller 后，就是保持 caller call 前的 relation。

transfer function 必须 monotone：

```text
s1 <= s2  =>  F(s1) <= F(s2)
```

这里的 `<=` 就是 pointwise subset。
因为 transfer 只会加 bit 或把 relation 往更保守的集合推进，所以满足 monotone。

## Fixpoint

对一个函数，按 CFG 做 forward fixpoint：

```text
in[entry] = default state
out[bb] = transfer_bb(in[bb])
in[bb] = join(out[pred1], out[pred2], ...)
重复直到不变
```

domain 有限，所以不需要 widening。
最坏情况只是每个 register 的几个 bit 从空集合逐步加到稳定。

对跨函数分析，call graph SCC 也是同一个思想：

```text
SCC 内函数 summary 初始化为保守默认
反复分析 SCC 内函数
直到所有 summary 不再变化
```

## 保存/恢复寄存器

保存/恢复 callee-saved register 不应该硬塞进核心 transfer function。

原因：

- 它通常依赖 stack/frame slot。
- 第一版不做 stack / memory alias。
- 如果强行在核心域里猜，会把抽象域搞复杂，也容易误判。

更好的处理是 postpass refinement：

```text
先用核心分析得到 summary
再由专门 pass 匹配 frame-local save/restore
如果证明 R 在所有 return path 恢复入口值
  -> 把 R 的 exit relation 精化为 SameAsEntry
  -> summary 标记 preserved
```

这一步可以只处理确定的 frame-local slot。
不要扩展到一般内存。

## 参考资料

- Patrick Cousot, Radhia Cousot, [Abstract Interpretation: A Unified Lattice Model for Static Analysis of Programs by Construction or Approximation of Fixpoints](https://www.di.ens.fr/~cousot/publications.www/CousotCousot-POPL-77-ACM-p238--252-1977.pdf), POPL 1977.
- Jonni Kanerva, lecture notes, [Abstract Interpretation](https://www.cs.cmu.edu/~aldrich/courses/17-355-18sp/notes/notes14-abstractinterpretation.pdf), CMU course material.
- J. B. Kam, J. D. Ullman, [Monotone Data Flow Analysis Frameworks](https://www.cs.utexas.edu/~pingali/CS380C/2023/papers/Monotone%20data%20flow%20analysis%20frameworks.pdf), Acta Informatica 1977.
