# Native register summary SCC fixpoint plan

用户原始要求：

> 外部函数/间接调用暂时就看ABI，是没问题的。stack 参数可能后续再拓展覆盖吧，初版先不考虑，不过确实是一个值得考虑的点。partial register感觉也没必要。就直接当做存放了整个寄存器吧。条件路径没必要啊，直接就当做两边路径都被改了就行了。内存和栈一样，都先不考虑。后期的话，也只是考虑占空间上的内存，其他地方的内存完全不考虑。未知clobber应该没啥吧。按这个写一个plan文档吧。文档可以明确说一下，这个是一个新版的单独链路，和之前的模仿Ghidra的做法完全独立，也不用参考那边

用户补充要求：

> 每个 register 当前值用少量抽象值表示，这一块感觉不太系统。初始值就是entry，如果改动了就变成改动了的，为什么还要单独分什么Call Clobber，Call Return。对某个函数的调用，目标函数的所有基本块在transfer function的合并下得到的结果就是函数的结果，根据函数的结果去分析call 的效应就可以了，没必要分什么clobber return。栈指针相关可以单独由专门的pass匹配和处理，当前这个pass可以不考虑，搞一个不考虑的寄存器集合作为pass的参数。另外3-5节重写一下按照transfer function的思路去描述，定义抽象域，join函数，meet函数，然后证明join和meet操作满足交换律，然后函数的summary就直接定义为整个函数CFG的整体效应即可。最好先网上搜一下且复习一下相关的抽象解释框架，将相关基础知识也总结到一个docs/下合适位置的文档，然后再去改。抽象域这一块稍微处理一下，比如用一个什么map去存，如果map里面不存在的值就是默认值，表示untouched。如果仅读取了就是read，被改过就是modified？但是，函数的summary可以在计算完毕后，单独去匹配保存和恢复callee saved register的模式，然后将对应的值从modified改成别的值？这一块深入思考一下

用户进一步澄清：

> write比较明确，即关系寄存器是否被改动。函数识别参数（read）的话，其实主要关心函数开头的值是否可能被读取，可能存在use，所以read这部分是Or运算。但是，一旦某个值已经被覆盖，类似被killed，则此时后续指令再读取就不管了。这个逻辑可以表示吗？
>
> 如果转换函数用了if规则表达了这种情况的话，它是否还满足交换律？
>
> 你这个说的完全不对，再去复习一下docs/analysis/abstract-interpretation-register-summary.md文档，或者搜一下网上关于抽象解释这方面的介绍再补充进去，不可能提到的是指令前后的读写交换，而是指控制流那边。如果join和meet运算符之间满足交换律，就意味着所有可能路径组合后再合并，等价于在每个控制流汇聚的地方提前合并，极大减少算力消耗。

用户继续补充：

> summary 算完后，可以考虑再top-down进一步确认一些不确定的寄存器信息？比如，部分函数在结尾修改了多个寄存器，但是真正的返回值可能只有RAX，可以通过分析所有的caller看修改的寄存器真正被用的有多少。因为顶层的寄存器调用情况是已知的，即假设需要遵守调用约定，所以从顶层不断确定下来。按照这个思路进一步拓展想想
>
> 是的，按照这个思路改一下，在logs/20260616-02-native-register-summary-scc-fixpoint/20260616-02-native-register-summary-scc-fixpoint-plan.md计划的技术路线后面补充top-down部分

用户最新补充：

> 第八节开头强调一下，是对每个函数，从所有的caller角度观察call的效果中，真正起作用的有哪些。
>
> 每个callsite的感觉不一定要保存，就top-down遍历的过程中，对每个函数的所有caller（callsite）分析就可以了。顶层就按照ABI调用约定来就行。直接根据作为参数的寄存器中，之前bottom up的结果中，使用了entry进来的值的情况来确定真正的参数数量。确定之后就可以top-down一路传播下来。比如说，如果entry恰好有call，则几乎所有的参数寄存器都传递给callee了，此时难以确定到底传了几个参数。根据callee的使用可以分析得到一定存在的参数，根据top-down分析，此时caller的参数数量已经确定了，则不存在的参数（那些调用约定里面可以是参数，但是不一定有参数的地方）一定没有被传递给callee。此时也算是函数的参数数量有个上界和下界的约束，函数的参数数量在这两个之间。这个思路的问题是，顶层如果按照上界的思路（即仅从所有caller的角度黑盒观察目标函数，允许出现那种完全没有被用到的函数）那其实上界就是当做所有寄存器都存了参数，只不过没用上。上界的意义可能不大？（即entryDemand信息）
>
> exitDemand是主要的，分析返回值真正被use的有多少。这个肯定非常有意义，top-down主要计算这个，同时可以顺带根据所有信息综合确定每个函数的参数和返回值情况

用户再次补充：

> 参数那一块还是简化一点吧，先不搞8.3函数上下界吧，然后后面也不用有那个“weak 输入参数候选”。然后第10点那里，标记出来之后就考虑做register SSA，让那边基于当前这个结果去做类型恢复。现在的RegisterSSA可能和之前模仿Ghidra链路模仿得比较深，如果改起来比较复杂，可以考虑把当前的RegisterSSA重命名为HeritageSSA，然后写一个新的SSA（必须要参考logs/20260529-01-native-prototype-recovery-pass/08-register-elimination/20260610-02-braun-ssa-reference.md，使用Simple and Efficient Construction of Static Single Assignment Form）

## 背景

实现前要复习：docs/analysis/abstract-interpretation-register-summary.md

这是 native prototype recovery / register elimination 的新版独立链路。
它不沿用之前模仿 Ghidra 的 trial/use 做法，也不以 caller 侧有没有“参数准备指令”作为核心判断。

新版思路是：

```text
函数内做只读的寄存器数据流分析
  -> 产出每个函数对每个 register 的 summary
  -> 按 call graph SCC 做自底向上 fixpoint
  -> callsite 直接套用 callee summary
  -> 决定哪些 register 是 input / preserved / return / clobbered
```

关键变化是：如果某个 register 的值从函数入口一路传到 callsite，且 callee summary 说明它会读这个 register，那么这个 entry value 也应该是参数证据。
不需要额外寻找 `mov rdi, ...` 这类本地准备动作。

第一版范围收窄：

- 外部函数和间接调用只用 ABI fallback。
- 不恢复 stack 参数。
- 不做一般内存/栈 alias 分析。
- 不做 partial register 精细建模，`AL/EAX/RAX` 先按整个 backing register 处理。
- 条件路径不单独保留条件语义；路径合并后只做保守 summary。
- 不把 `CallReturn` / `CallClobber` 放进核心抽象域；这些是 callsite 消费 summary 后得到的解释。
- pass 接收一个忽略 register 集合，栈指针、需要专门 pass 处理的 frame register 先从这条链路排除。

## 目标

做一个新的 register summary 分析链路，目标是给每个函数产出稳定 summary：

```text
register -> {
  是否读取过函数入口值,
  正常返回时是否仍可能等于入口值,
  正常返回时是否可能是非入口值,
  是否作为返回值,
  遇到 call 时如何传递和更新
}
```

这个 summary 之后用于：

- internal direct call 的 register effect 精化。
- register input / return recovery。
- 删除不再需要的 register residue。
- 避免把 preserved register 错当 clobber。
- 避免把没有被 callee 读取的 caller register store 当参数。

## 技术路线

### 1. Register unit

分析单元使用现有 backing register unit。
第一版不做 partial register 精度：

```text
写 AL / AX / EAX / RAX
  -> 都当成写整个 RAX backing register
读 AL / AX / EAX / RAX
  -> 都当成读整个 RAX backing register
```

这会损失精度，但能先把跨函数 summary 跑通。

### 2. 抽象域

函数内部不修改 IR，只做只读数据流。
抽象解释基础见：

- [abstract-interpretation-register-summary.md](/sn640/NotDec/external/NotDec-bin2llvm/docs/analysis/abstract-interpretation-register-summary.md)

第一版不把 register 当前值分成 `CallReturn` / `CallClobber`。
核心状态只描述两件事：

- 这个 register 的入口值是否被读过。
- 这个 register 当前是否还可能等于入口值。

每个 register 的 cell：

```text
Cell = {
  mayEntry: bool,
  mayNonEntry: bool,
  readEntry: bool
}
```

实现上用 map 存：

```text
Map<Register, Cell>
```

map 里不存在某个 register，表示默认 untouched：

```text
mayEntry = true
mayNonEntry = false
readEntry = false
```

这个默认值只适用于已经可达的 block 状态。
worklist 需要单独记录 block 是否可达：

```text
unreachable
reachable(Map<Register, Cell>)
```

未访问 block 是 `unreachable`，不是“所有 register 都 missing”。

常见解释：

- missing：没有读，也没有 caller 可见的改动。
- `readEntry=true`：函数入口值可能被读过，可作为参数证据。
- `mayEntry=false, mayNonEntry=true`：入口值在当前路径已经被 killed。
- `mayEntry=true, mayNonEntry=true`：路径混合，可能没变也可能变了。
- 函数出口 `mayEntry=true, mayNonEntry=false`：函数 preserved 该 register。
- 函数出口 `mayNonEntry=true`：函数可能 changed 该 register，consumer 保守处理。

`preserved` 是从函数出口状态派生出来的，不是核心域里的原子值。
`writeEvent` 不放在核心判断里。
如果后续为了调试想记录“函数体里是否出现过写寄存器指令”，可以加 audit bit，但它不能用于判断 caller 可见 clobber。

### 3. Transfer function

每条指令定义一个 transfer function：

```text
F_inst : State -> State
```

普通指令：

- 读 register `R`
  - 如果 `R` 不在 ignored set，执行 `readEntry[R] |= mayEntry[R]`。
  - 不改变 `mayEntry/mayNonEntry`。
- 写 register `R`
  - 如果 `R` 不在 ignored set，执行 `mayEntry[R] = false, mayNonEntry[R] = true`。
- 读写同一 register
  - 先按读处理，再按写处理。

这能表达 killed 后不再算入口读取：

```asm
mov rdi, 0
use rdi
```

`mov` 后 `mayEntry=false`，所以后面的 `use rdi` 不会设置 `readEntry`。

call 指令：

- direct internal call 用 callee summary 转换 caller state。
- external / indirect call 用 ABI fallback 转换 caller state。
- callsite 参数证据来自 callee summary 的 `readEntry`，不是来自 caller 侧有没有准备指令。

callee summary 套到 caller state 时：

```text
if callee.readEntry[R]:
  当前 caller state[R] 是 callsite 参数来源
  caller.readEntry[R] |= caller.mayEntry[R]

post.mayEntry[R] =
  callee.mayEntry[R] ? pre.mayEntry[R] : false

post.mayNonEntry[R] =
  (callee.mayEntry[R] ? pre.mayNonEntry[R] : false)
  OR callee.mayNonEntry[R]
```

如果 callee summary 说它读取 `RDI`，那么 caller 在 callsite 处的当前 `RDI` 值就是参数来源。
这个值可以来自本地定义，也可以直接来自 caller 函数入口。

### 4. Join / meet

CFG 合流使用 join。
对单个 register：

```text
join mayEntry    = OR
join mayNonEntry = OR
join readEntry   = OR
```

meet 是对应的交集：

```text
meet mayEntry    = AND
meet mayNonEntry = AND
meet readEntry   = AND
```

对 map 做 pointwise join / meet。
missing register 使用默认 untouched cell。

对 block state：

```text
join(unreachable, s) = s
join(s, unreachable) = s
join(unreachable, unreachable) = unreachable
```

两个 reachable state 再做 pointwise join / meet。

交换律来自集合 union / intersection：

```text
A ∪ B = B ∪ A
A ∩ B = B ∩ A
```

所以：

```text
join(s1, s2) = join(s2, s1)
meet(s1, s2) = meet(s2, s1)
```

这保证 predecessor 枚举顺序不影响 CFG 合流结果。
本分析是 may-style forward analysis，fixpoint 里主要用 join。

更关键的是，提前在 CFG 合流点 join 之后继续分析，应该等价于枚举所有路径再 join。
这个等价不只靠交换律，还要求当前 transfer function 对 join 可分配：

```text
F(join(a, b)) = join(F(a), F(b))
```

当前读规则满足这个条件：

```text
F_read(s).readEntry = s.readEntry OR s.mayEntry

F_read(join(a, b)).readEntry
= (a.readEntry OR b.readEntry) OR (a.mayEntry OR b.mayEntry)
= (a.readEntry OR a.mayEntry) OR (b.readEntry OR b.mayEntry)
= join(F_read(a), F_read(b)).readEntry
```

写规则也满足：

```text
F_write(s).mayEntry = false
F_write(s).mayNonEntry = true
F_write(s).readEntry = s.readEntry
```

所以：

```text
F_write(join(a, b)) = join(F_write(a), F_write(b))
```

block transfer 是指令 transfer 的组合。
每条指令都对 join 可分配时，block transfer 也可分配。
这就是不枚举所有控制流路径也能得到同样 summary 的原因。

### 5. Function summary

一个函数的 summary 定义为整个 CFG transfer function 的 fixpoint 结果。

计算方式：

```text
in[entry] = reachable(default state)
其他 block 初始为 unreachable
out[bb] = F_block(in[bb])
in[bb] = join(out[pred1], out[pred2], ...)
重复直到不变
```

然后合并所有 return block 的 `out` state，得到函数出口状态。

对每个 register：

```text
readEntry=false, mayEntry=true, mayNonEntry=false
  -> untouched

readEntry=true, mayEntry=true, mayNonEntry=false
  -> read + preserved

mayNonEntry=true
  -> modified

mayEntry=true, mayNonEntry=true
  -> mixed，consumer 保守处理
```

如果 register 是 ABI return register，且函数出口 `mayNonEntry=true`，可以派生为 return candidate。
这仍然不是核心域里的 `CallReturn` 值，只是 summary 消费阶段的解释。

### 5b. 保存/恢复精化

第一版 core pass 不处理 stack/memory。
保存/恢复 callee-saved register 后续用单独 postpass 做。

流程：

```text
先得到普通 register summary
再运行 frame-local save/restore matcher
如果证明 R 在所有 return path 恢复入口值
  -> 将 R 的出口精化为 mayEntry=true, mayNonEntry=false
  -> 不把纯保存用途的读计入 readEntry
  -> summary 标记 preserved
```

这一步只能处理确定的 frame-local slot。
不要扩展到一般内存 alias。
它也不改变 core domain 的 join/meet 设计。

### 6. Callsite 应用

分析 caller 的 call 指令时：

```text
callee reads R
  -> 当前 caller state[R] 是 callsite argument source

callee preserves R
  -> caller call 后 mayEntry/mayNonEntry 继承 call 前状态

callee modifies R
  -> caller call 后 mayNonEntry=true
```

external / indirect call 没有 callee summary，直接用 ABI：

- ABI unaffected：state 不变。
- ABI output / killed-by-call：`mayEntry=false, mayNonEntry=true`。

### 7. Call graph SCC fixpoint

构建 direct call graph。
external 和 indirect call 不进图，只按 ABI 处理。

流程：

```text
构建 call graph
压缩 SCC
按 SCC DAG 自底向上遍历

for each SCC:
  初始化 SCC 内函数 summary
  repeat:
    用当前 summary 分析 SCC 内每个函数
    更新函数 summary
  until summary 不变
```

递归和互递归只在 SCC 内 fixpoint。
summary lattice 必须有限，避免来回震荡。
每个 block state 只有两类：

```text
unreachable
reachable(register map)
```

每个 register cell 只有 3 个 bit：

```text
mayEntry: bool
mayNonEntry: bool
readEntry: bool
```

`mayEntry` 在写入时会从 true 变 false，但合流时仍用 OR。
transfer function 仍然是 monotone，worklist 里的 in-state 通过 join 增长；domain 有限，所以仍然收敛。
保存/恢复这类精化放在 fixpoint 后的 postpass，避免破坏单调迭代。

### 8. Top-down demand analysis

bottom-up summary 解决“函数本身可能做什么”：

```text
readEntry
exit mayEntry / mayNonEntry
callee 对 caller 的 register effect
```

top-down demand analysis 解决另一个问题：对每个函数，从它所有 caller 的 call 效果里看，哪些寄存器效果真正起作用。
最重要的是返回值：如果某个函数同时修改 `RAX/RDX/RCX`，bottom-up 只能说这些 register 都可能是 non-entry。
但如果所有 caller 在 call 后只读取 `RAX`，那么 `RAX` 才是强返回值证据，`RDX/RCX` 更像 dead changed register。

因此在 bottom-up SCC fixpoint 收敛后，再加一个 top-down demand analysis。
它不修改 `EffectSummary`，只产出第二套结果：

```text
DemandSummary = {
  exitDemand[R]: 函数返回时 R 是否被 caller / 顶层 ABI 观察,
  entryDemand[R]: 函数入口 R 是否影响被观察结果，第一版只作为辅助信息
}
```

`callsite` 不一定需要保存成长期 metadata。
top-down 遍历 caller 时，可以临时分析每个 callsite 的 call 前后状态，把结果直接合并到 callee 的 `DemandSummary`。
后续如果要调试，再选择性输出 callsite audit。

#### 8.1 Demand 种子

顶层函数、exported function、外部可调用入口按 ABI 加初始返回值需求：

```text
ABI return registers -> exitDemand
```

第一版可以先只放常规返回寄存器，例如 x86-64 SysV 下的 `RAX`。
callee-saved register 不是返回值 demand，它是 ABI preservation obligation，仍由 bottom-up preserved 判断处理。

top-down 的主收益仍然是 `exitDemand`，也就是确认哪些返回寄存器真的被使用。
参数第一版不做 top-down 精化，只使用 bottom-up `readEntry` 作为输入参数证据。

#### 8.2 从所有 caller 观察函数效果

internal function 的返回值需求来自所有 caller。
遍历 caller 时，对每个 direct callsite 重新跑一次局部需求分析。
分析对象是 callee 可能修改的寄存器：

```text
callee exit mayNonEntry[R] = true
```

对每个这样的 `R`，检查 call 后从该 callsite 出发，`R` 的新值是否在被覆盖前被读取。
多个 caller、多个 callsite 的结果用 OR 聚合：

```text
callee.exitDemand[R] =
  OR(callsiteDemand(callsite, R) for all direct callsites to callee)
  OR root/export ABI seed
```

其中：

```text
caller 在 call 后读取 R
callee summary 显示 R 可能是 non-entry
  -> callsiteDemand(callsite, R) = true
```

如果 callee 的出口是混合状态：

```text
mayEntry=true, mayNonEntry=true
```

则 call 后对 `R` 的需求要拆成两部分：

```text
callee.exitDemand[R] = true
caller call 前 R 继续 demand
```

如果某个 changed register 从来没有被 caller 读取，也不是顶层 ABI return register，它就只是 dead changed register，不应作为强返回值证据。

#### 8.3 函数内 backward demand

top-down 进入某个函数后，用 `exitDemand` 在函数 CFG 上做 backward analysis。
这个阶段仍然只追 register，不引入 stack/memory alias。

普通指令规则：

```text
如果指令写 R，且 R 在 demand_after 中:
  指令读取的 register 加入 demand_before

如果指令写 R，但 R 不在 demand_after 中:
  这个写出的 register 值对当前需求无贡献

如果指令只读 R，且这个 read 本身是控制流或可观察副作用需要:
  R 加入 demand_before
```

对当前第一版，分支条件读取的 register 应该保守加入 demand。
一般内存/栈暂不追；后续如果加内存输出，只处理明确的占空间 frame-local 内存，不做全局 alias。

函数入口处可以得到辅助信息：

```text
entryDemand[R]
```

它表示入口 register `R` 的值可能影响被观察结果。
但它不是 top-down 第一版的主要产物；参数恢复仍然优先用 bottom-up `readEntry` 判断。

#### 8.4 Call 指令的 demand 传播

call 是 top-down 的核心。
设 caller 在 call 后 demand `R`：

```text
callee exit mayEntry=true
  -> caller call 前 R 继续 demand

callee exit mayNonEntry=true
  -> callee.exitDemand[R] = true
```

参数方向第一版不在 top-down 中精化。
callee 的输入参数仍由 bottom-up `readEntry` 给出。
如果后续 `entryDemand` 稳定，可以作为审计信息输出，但不用于直接改 prototype。

#### 8.5 Top-down SCC fixpoint

demand 沿 call graph 从 caller 传向 callee。
递归和互递归仍然需要 SCC fixpoint：

```text
先跑 bottom-up SCC，得到稳定 EffectSummary
再按 call graph 从 roots / exports 向下传播 demand

for each SCC:
  初始化 SCC 内函数 DemandSummary
  repeat:
    用当前 demand 分析 SCC 内每个函数
    遍历这些函数内的 callsite，临时计算 observation
    把 observation 合并到 callee DemandSummary
  until DemandSummary 不变
```

这个 fixpoint 的 lattice 是有限的：

```text
exitDemand[R]: false -> true
entryDemand[R]: false -> true
```

CFG backward 合流使用 OR。
`exitDemand/entryDemand` 只增不减，最终会收敛。

#### 8.6 消费规则

最终 prototype / residue 删除不只看 bottom-up changed，还看 top-down demand。
返回值优先级最高：

```text
EffectSummary.exit mayNonEntry[R] = true
DemandSummary.exitDemand[R] = true
R 属于 ABI return register
  -> 强返回值候选
```

如果：

```text
EffectSummary.exit mayNonEntry[R] = true
DemandSummary.exitDemand[R] = false
```

则 `R` 只是函数可能写过的 dead changed register。
它可以用于 clobber/residue 判断，但不应直接当返回值。

参数第一版只看 bottom-up `readEntry`：

```text
EffectSummary.readEntry[R] = true
  -> 参数候选
```

### 9. Prototype recovery 消费

新版链路不再需要 caller-side trial/use 作为主判据。

函数输入先来自 bottom-up 的 `readEntry`：

```text
callee summary: readEntry[RDI] = true
callsite: 当前 RDI 抽象状态
  -> RDI 是参数候选
```

第一版不再维护参数上下界，也不区分参数强弱。
`entryDemand` 只作为审计信息，不直接决定参数数量。

函数返回也分两层。
bottom-up 只产出 weak return candidate：

```text
callee summary: RAX 是 ABI output register
callee exit mayNonEntry[RAX] = true
  -> RAX 是 weak 返回值候选
```

top-down 确认 caller / root 真的观察这个寄存器后，再升级：

```text
DemandSummary.exitDemand[RAX] = true
  -> RAX 是 strong 返回值候选
```

如果 callee 没读某个 ABI input register，则 caller 对该 register 的写入不应该被当成参数证据。

### 10. Register SSA 与寄存器消除

summary / demand 标记完成后，下一步不直接大规模删除 register residue。
更稳的路线是先构建 register SSA，再基于 SSA def-use 删除确认无用的 register residue：

```text
register summary / demand
  -> 标记函数 input / demanded return / preserved / clobbered
  -> register SSA
  -> 基于 SSA def-use 做 register residue 消除
  -> 后续如果需要，再让其他 pass 消费这些结果
```

现有 `RegisterSSA` 如果和之前模仿 Ghidra heritage 的链路耦合太深，不要继续在里面硬塞新逻辑。
可以把当前实现重命名为 `HeritageSSA`，保留给旧链路使用。
然后新写一个独立 `RegisterSSA`，只面向当前 summary/demand 链路。

新的 register SSA 必须参考：

- [20260610-02-braun-ssa-reference.md](/sn640/NotDec/external/NotDec-bin2llvm/logs/20260529-01-native-prototype-recovery-pass/08-register-elimination/20260610-02-braun-ssa-reference.md)
- Braun et al., `Simple and Efficient Construction of Static Single Assignment Form`

第一版设计：

```text
variable:
  backing register unit

readVariable(register, block):
  查询当前 block 的本地定义
  没有则递归 predecessor
  join block 需要 PHI

writeVariable(register, block, value):
  记录当前 block 的 register SSA definition

call transfer:
  按 EffectSummary / DemandSummary 解释 preserved、demanded return、clobber
```

实现上要保留 Braun 算法的关键机制：

- `incompletePhis[block][register]`：递归遇到未完成 join 时先放临时 PHI。
- `sealed block` / finalize：当前 LLVM CFG 已经完整，可以把 pass 结束当 sealed point，但必须统一补齐 PHI operands。
- `addPhiOperands`：对每个 predecessor 读取对应 register value。
- `tryRemoveTrivialPhi`：删除只合并同一个值的 PHI，并更新缓存。

这几个机制是硬要求。
不能留下 operandless PHI，也不能只靠 `replaceAllUsesWith` 删除 PHI 而不更新 SSA 缓存。

寄存器消除消费 SSA 时：

- `readEntry=true` 的 register entry value 不能当成无用 residue 删除。
- `exitDemand=true` 且 `mayNonEntry=true` 的 ABI return register 不能当成无用 residue 删除。
- preserved register 在 call 前后的 SSA value 应保持同一条 def-use 链。
- clobbered register 不应沿用 call 前 SSA value。
- 没有被 SSA def-use 消费、也不属于 entry input / demanded return / preserved obligation 的 register load/store，才进入删除候选。

真正删除 residue 放在 SSA 验证稳定之后。

## 不做什么

第一版不做：

- stack 参数恢复。
- 一般 memory alias。
- 栈保存/恢复的完整证明。
- partial register 精细合并。
- 条件执行路径标记。
- Ghidra trial/use 兼容层。
- 在 register SSA 稳定前直接删除 residue。

保存/恢复 callee-saved register 如果依赖 stack slot，第一版可以先不证明。
后续如果要补，只做 frame-local slot，不做全局内存 alias。

## 风险

主要风险：

- 不建模 stack/memory 时，保存恢复寄存器只能靠 ABI 或后续 frame-slot 分析，第一版会少恢复。
- whole-register 粗粒度会把 partial write 放大成 whole write，可能让 summary 偏保守。
- direct call graph 不完整时，部分 internal call 会退到 ABI。
- SCC fixpoint 如果 lattice 设计太细，容易震荡或实现复杂。
- `mayEntry=true, mayNonEntry=true` 的混合状态如果消费侧处理不严，可能误当 precise value。
- top-down demand 如果 root/export 的 ABI seed 不准，会把真实返回值误判为未使用。
- 只看 caller 读取寄存器时，未建模的内存/异常/间接调用可能隐藏真实观察点。
- 现有 `RegisterSSA` 如果和 heritage 旧链路耦合太深，直接改可能影响旧路径。
- 新 register SSA 如果没有完整处理 incomplete PHI / finalize，容易重新出现 PHI incoming 不完整的 verifier 错误。

风险处理：

- 第一版宁可保守，不从混合状态生成 signature rewrite。
- top-down 结果主要用于增强返回值置信度，不删除 bottom-up effect summary。
- root/export 使用 ABI return register 作为 demand seed；不确定入口先保守保留 weak return candidate。
- 旧 `RegisterSSA` 难改时先重命名为 `HeritageSSA`，新链路使用独立 `RegisterSSA`。
- 新 SSA 必须按 Braun 风格实现 incomplete PHI / finalize / trivial PHI 删除。
- summary 先写 metadata / 日志，不直接大规模删 IR。
- 删除 register residue 必须作为后续阶段，等 SSA 稳定后再做。

## 判断标准

最小判断标准：

- 空函数 summary：所有 register preserved / no read。
- 只读 `RDI` 的函数：summary 标记 reads `RDI`。
- 写 `RAX` 并返回的函数：summary 标记 `RAX` return candidate。
- direct caller 调用该函数时，当前 `RDI` state 被记录为 callsite 参数来源，即使它来自 caller entry。
- external / indirect call 使用 ABI fallback。
- 简单递归 SCC 能收敛。
- `mayEntry=true, mayNonEntry=true` 不参与 prototype rewrite。
- 修改 `RAX/RDX` 但 caller 只读取 `RAX` 的函数：top-down 标记 `RAX` 为 demanded output，`RDX` 不作为强返回值。
- caller call 后读取 preserved register：demand 继续传到 call 前，不误传成 callee return。
- 递归 SCC 内 demand 能收敛。
- register SSA 里保留下来的每个 PHI incoming 数量等于 predecessor 数量。
- trivial PHI 能被删除或替换，缓存不保留已删除 PHI。
- preserved register 在 call 前后保持同一条 SSA def-use 链。
- clobbered register 不沿用 call 前 SSA value。

Bench2 判断标准：

- summary pass 能跑完当前 Bench2 native IR。
- 输出每个函数的 register read / preserved / modified / return 统计。
- 输出 demanded output / weak return 统计，检查多返回寄存器候选是否减少。
- 不引入 `llvm-as` / `opt verify` 回归。
- 在不开启 residue 删除时，IR 行为不变；开启 register SSA 后也必须通过 verifier。

## 实现记录：第一版 summary pass

本次先实现独立 summary 分析，不接入默认 native pipeline，也不改旧 `RegisterSSA`。

改动文件：

- `include/notdec-bin2llvm/passes/NativeRegisterSummary.h`
  - 第 15-60 行新增 `NativeRegisterSummaryOptions`、`NativeRegisterSummaryRegister`、`NativeRegisterSummaryFunction`、`NativeRegisterSummary`。
  - 第 62-67 行新增 `runNativeRegisterSummary()` 和 `printNativeRegisterSummary()`。
- `lib/passes/NativeRegisterSummary.cpp`
  - 第 28-88 行新增 register unit、`Cell { mayEntry, mayNonEntry, readEntry }`、`State`、`FunctionEffect`、`FunctionDemand`、ABI fallback 数据。
  - 第 90-224 行新增 register metadata 识别和 ABI metadata 读取。
  - 第 247-287 行实现 `joinCell()` / `joinState()` / register read-write transfer，其中 `joinState()` 处理稀疏 map 的默认 untouched 路径。
  - 第 329-444 行实现 direct call graph、SCC 和 bottom-up fixpoint。
  - 第 446-540 行 `Analyzer::analyzeFunction()` / `Analyzer::applyFunctionEffect()` 实现 CFG forward fixpoint 和 callee summary 应用。
  - 第 560-686 行 `Analyzer::runTopDownDemand()` / `Analyzer::applyBackwardCallDemand()` 实现 caller 视角的返回值 demand 聚合。
  - 第 714-755 行输出 metadata：
    - `notdec.register.summary`
    - `notdec.register.summary.read_entry`
    - `notdec.register.summary.preserves`
    - `notdec.register.summary.modifies`
    - `notdec.register.summary.demanded_returns`
  - 第 758-858 行生成公开 summary 和打印统计。
- `lib/CMakeLists.txt`
  - 第 10 行把 `passes/NativeRegisterSummary.cpp` 加入 `notdec-bin2llvm-core`。
- `CMakeLists.txt`
  - 第 201-214 行新增 `native_register_summary_test` 和 `notdec.native_register_summary.fixpoint`。
- `tests/native_register_summary_test.cpp`
  - 第 130-154 行覆盖 killed 后读取不算入口参数。
  - 第 156-187 行覆盖 callee `readEntry` 传播到 caller entry。
  - 第 189-231 行覆盖稀疏 map 合流时 untouched 路径不能被 predecessor 顺序丢掉。
  - 第 233-275 行覆盖 caller 只读取 `RAX` 时，callee 的 `RDX` 不作为 demanded return。

实现时确认的一点：

- 顶层/root 的 ABI demand seed 第一版只取 ABI outputs 中的第一个 register。否则 `RAX/RDX` 同时作为 ABI output 时，会把没有被 caller 使用的 `RDX` 也误标成 demanded return。这个和计划中“第一版先只放常规返回寄存器”一致。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target native_register_summary_test native_register_effects_test -j2
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_effects_test
git diff --check
```

复杂度评估：

- 实现效果：7/10。bottom-up summary 和 top-down exit demand 已经能跑通核心用例，但还没有接入 pipeline，也没有处理 stack 保存/恢复。
- 理解成本：6/10。新增了一条独立分析链路，代码集中在一个文件里，暂时没有影响旧链路。
- 维护成本：6/10。后续主要风险在 demand seed、间接调用和 frame-local 保存/恢复；当前 metadata 输出先作为审计入口。

## 实现记录：summary-based Register SSA 第一版

本次继续实现独立的新 SSA pass，没有改旧 `NativeRegisterSSA`，也没有接入默认 `notdec-native-llvm` pipeline。
旧 `NativeRegisterSSA` 仍负责当前默认链路里的 Ghidra-style trial/use 和旧 metadata。

改动文件：

- `include/notdec-bin2llvm/passes/NativeRegisterSummarySSA.h`
  - 第 14-18 行新增 `NativeRegisterSummarySSAOptions`。
  - 第 20-47 行新增函数级和模块级 summary 计数。
  - 第 49-53 行新增 `runNativeRegisterSummarySSA()` 和打印接口。
- `lib/passes/NativeRegisterSummarySSA.cpp`
  - 第 30-67 行新增 register unit、summary fact、ABI fact、call effect 和 SSA key 数据结构。
  - 第 69-210 行新增 metadata 读取、register load/store 识别、ABI fallback 读取、summary fact 映射。
  - 第 212-615 行新增 `FunctionBuilder`，实现只面向完整 backing register 的 SSA 构建。
  - 第 278-331 行 `rewriteLoads()` / `readValueBefore()` 替换 load，并按 summary 解释 call 后寄存器值。
  - 第 334-487 行 `readBlockEntry()` / `readBlockExit()` / `ensurePhi()` / `completePhi()` / `finalizePendingPhis()` 实现 Braun 风格 lazy SSA 和 PHI 收尾。
  - 第 489-563 行按 callee summary / ABI 判断 preserved、demanded return、clobber、unknown call effect。
  - 第 633-661 行 `runNativeRegisterSummarySSA()` 先运行 `runNativeRegisterSummary()`，再消费 summary 构建 SSA。
- `lib/CMakeLists.txt`
  - 第 11 行把 `passes/NativeRegisterSummarySSA.cpp` 加入 `notdec-bin2llvm-core`。
- `CMakeLists.txt`
  - 第 216-229 行新增 `native_register_summary_ssa_test` 和 `notdec.native_register_summary.ssa`。
- `tests/native_register_summary_ssa_test.cpp`
  - 第 120-154 行覆盖 join block PHI incoming 数量等于 predecessor 数量，并通过 verifier。
  - 第 156-187 行覆盖 preserved call 后的 load 沿用 call 前 value。
  - 第 189-221 行覆盖 demanded return 生成 `notdec.register.summary_return.*` helper，并替换 call 后 load。

这一步只做 SSA 的最小可验证链路：

- 只处理完整 backing register load/store。
- 不处理 partial register 精度。
- 不删除 register residue。
- 不接入 prototype/type recovery。
- 不改默认 CLI pipeline。
- 遇到 unknown call effect 时保守不替换。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target native_register_summary_ssa_test native_register_summary_test native_register_effects_test -j2
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_effects_test
git diff --check
```

复杂度评估：

- 实现效果：6/10。已经能消费 summary/demand、构建 PHI、处理 preserved call 和 demanded return，但还没做 residue 删除。
- 理解成本：5/10。新 pass 独立，代码比旧 `NativeRegisterSSA` 窄很多；代价是短期存在新旧两套 SSA。
- 维护成本：5/10。后续需要把 residue 删除和 pipeline 消费点接到新 metadata/helper 上，再决定旧 SSA 是否重命名为 `HeritageSSA`。

## 实现记录：summary SSA 局部 residue 删除

本次只在 `NativeRegisterSummarySSA` 内加最小安全删除，不改默认 pipeline。
删除范围保持很窄：

- 删除已经被 summary SSA 替换、且已经没有 use 的 register load。
- 删除同一 basic block 内被后续同 register store 覆盖、期间没有 register load 或普通 call 的完整 register store。
- 不跨 CFG 删除。
- 不删除 partial register access。
- 遇到普通 call 清空局部 store 追踪，避免跨未知副作用删除。

改动文件：

- `include/notdec-bin2llvm/passes/NativeRegisterSummarySSA.h`
  - 第 14-19 行新增 `EnableResidueRemoval` 开关。
  - 第 21-35 行、37-51 行新增 `DeadLoadsRemoved` / `DeadStoresRemoved` 计数。
- `lib/passes/NativeRegisterSummarySSA.cpp`
  - 第 223-234 行在 SSA rewrite 后、删 PHI 前调用 residue 删除。
  - 第 247-256 行新增 `ReplacedLoads` 缓存。
  - 第 283-301 行 `rewriteLoads()` 记录已替换 load。
  - 第 304-312 行新增 `removeDeadReplacedLoads()`。
  - 第 314-357 行新增 `removeLocalDeadStores()`。
  - 第 678-691 行汇总新的删除计数。
- `tests/native_register_summary_ssa_test.cpp`
  - 第 156-187 行、190-223 行确认已替换 load 会被删除。
  - 第 225-251 行新增 overwritten store 删除用例。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target native_register_summary_ssa_test -j2
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_effects_test
git diff --check
```

复杂度评估：

- 实现效果：6/10。已经能删掉最明显的 replaced load 和局部 overwritten store，但还没有跨 block residue 删除。
- 理解成本：4/10。删除规则集中在新 SSA pass 内，且条件比较保守。
- 维护成本：4/10。后续要接 pipeline/Bench2 后，再根据 audit 结果扩展更强的删除规则。

## 实现记录：summary SSA pipeline opt-in 和 Bench2 小用例验证

本次把 summary-based register SSA 接入 `notdec-native-llvm`，但只做显式 opt-in，不改变默认旧链路。

改动文件：

- `tools/notdec-native-llvm.cpp`
  - 第 63-64 行新增 `UseSummaryRegisterSSAPass` CLI 状态。
  - 第 77-80 行 usage 增加 `--summary-register-ssa-pass`。
  - 第 140-146 行解析 `--summary-register-ssa-pass`。
  - 第 786-803 行 `runRegisterSSAPassIfEnabled()` 在 opt-in 时运行 `runNativeRegisterSummarySSA()`，否则继续运行旧 `runNativeRegisterSSA()`。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm native_register_summary_ssa_test native_register_summary_test native_register_effects_test -j2
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_effects_test

/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /tmp/notdec-summary-ssa-cli.ll \
  -o /tmp/notdec-summary-ssa-cli.noic.out.ll \
  --no-instcombine-pass --summary-register-ssa-pass \
  --register-ssa-summary --no-prototype-recovery-pass
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-summary-ssa-cli.noic.out.ll \
  -o /tmp/notdec-summary-ssa-cli.noic.out.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-summary-ssa-cli.noic.out.bc \
  -o /tmp/notdec-summary-ssa-cli.noic.verified.bc
```

小 IR 结果：

```text
loads=1 stores=2 loads_replaced=1 dead_loads_removed=1 dead_stores_removed=1
```

Bench2 小用例：

```text
/sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk -f 0x8300
```

同口径关闭 prototype recovery：

```text
old register SSA:     0.46s, output 159 lines
summary register SSA: 0.43s, output 296 lines
```

summary SSA 输出：

```text
functions=1 loads=11 stores=25 loads_replaced=7 dead_loads_removed=7
dead_stores_removed=1 phis_created=0 phis_simplified=0 entry_inputs=2
call_returns=2 call_clobbers=0 preserved_calls=1 unknown_call_effects=4
```

完整默认后续 pipeline 验证：

```text
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk \
  -f 0x8300 -o /tmp/wrk-8300-summary-default.ll \
  --summary-register-ssa-pass --register-ssa-summary
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/wrk-8300-summary-default.ll \
  -o /tmp/wrk-8300-summary-default.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/wrk-8300-summary-default.bc \
  -o /tmp/wrk-8300-summary-default.verified.bc
```

结果：通过，时间 `0.46s`。

复杂度评估：

- 实现效果：7/10。summary SSA 已可通过 CLI 显式启用，并在 Bench2 小函数上通过 verifier。
- 理解成本：3/10。只增加一个 opt-in 开关，默认路径不变。
- 维护成本：4/10。后续如果要替换默认旧 SSA，需要更大范围 Bench2 audit 后再决定。

## 实现记录：修复 summary SSA PHI incoming 和 Bench2 三目标验证

扩大到 Bench2 `--all-confirmed` 后，`vsftpd` 暴露了 summary SSA 的 PHI incoming bug：

```text
PHINode should have one entry for each predecessor of its parent basic block
module verification failed after summary register SSA pass
```

原因是 `completePhi()` 之前按 predecessor basic block 去重。
但 LLVM PHI 是按 CFG edge 计数的，`switch` 可以从同一个 predecessor block 向同一个目标 block 产生多条边。
这种情况下 PHI 需要多个 incoming，不能只保留一个。

改动文件：

- `lib/passes/NativeRegisterSummarySSA.cpp`
  - 第 453-471 行 `completePhi()` 改成按 predecessor block 的出现次数补 incoming，允许同一 block 多条边。
  - 第 476-508 行 `simplifyPhi()` 只在 PHI incoming 数量等于 `pred_size(parent)` 后才删除 trivial PHI。
  - 第 510-528 行 `finalizePendingPhis()` 改成迭代补齐未完成 PHI，避免补一个 PHI 时新建的 pending PHI 被漏掉。
- `tests/native_register_summary_ssa_test.cpp`
  - 第 120-130 行新增 `hasPhiIncomingCount()`。
  - 第 168-205 行新增 `testDuplicatePredecessorEdgesKeepPhiComplete()`，构造 `switch` 的重复 predecessor edge，要求 PHI 有 3 个 incoming 并通过 verifier。
  - 第 310 行把新测试接入 main。

验证：

```text
git diff --check
cmake --build /tmp/notdec-bin2llvm-build --target native_register_summary_ssa_test notdec-native-llvm -j2
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_effects_test
```

Bench2 summary SSA all-confirmed：

```text
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd \
  --all-confirmed -o /tmp/notdec-summary-audit-vsftpd-summary-fix.ll \
  --no-prototype-recovery-pass --summary-register-ssa-pass --register-ssa-summary
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/notdec-summary-audit-vsftpd-summary-fix.ll \
  -o /tmp/notdec-summary-audit-vsftpd-summary-fix.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-summary-audit-vsftpd-summary-fix.bc \
  -o /tmp/notdec-summary-audit-vsftpd-summary-fix.verified.bc
```

结果：

```text
vsftpd summary SSA:   87.59s, 126067 lines, llvm-as/opt verify passed
libuv summary SSA:   141.73s, 167152 lines, llvm-as/opt verify passed
memcached summary SSA: 179.46s, 218409 lines, llvm-as/opt verify passed
```

补充：

- 同口径 `vsftpd --all-confirmed` 旧 `NativeRegisterSSA` 当前触发 PHI 类型断言，不能作为全量性能 baseline。
- 之前单函数 `wrk -f 0x8300` 已记录旧链路 `0.46s`、summary 链路 `0.43s`。

复杂度评估：

- 实现效果：8/10。修掉真实 CFG 上的 PHI incoming verifier blocker，并通过三个 Bench2 all-confirmed summary SSA gate。
- 理解成本：4/10。修复仍局限在 Braun SSA PHI 完成逻辑里，没有扩散到 summary 分析。
- 维护成本：4/10。后续要继续扩大 Bench2 audit，但当前 opt-in 链路已有比单函数更强的 verifier 证据。

## 实现记录：CFG 级 register store liveness 删除

这次把 summary SSA 里的 residue 删除从“同一基本块内的覆盖 store”扩到“CFG 级 backward liveness”。

核心还是保守版，只处理完整 backing register：

- `store R*`：如果从后继块和后续指令回看，`R*` 不再活跃，就删掉。
- `load R*`：把 `R*` 标成活跃。
- `call`：
  - 内部 direct call 按 callee summary 看它到底读哪些 input register。
  - 外部 / 间接 call 按 ABI `Inputs` 处理。
  - `ReturnValue` / `Clobber` 会杀掉 call 前旧值的活跃性。
  - `Unknown` 保守保留旧值。
- 函数出口：只把 summary 里 `ExitDemand=true && MayNonEntry=true` 的 ABI return register 作为 live seed。

改动文件：

- `lib/passes/NativeRegisterSummarySSA.cpp`
  - 第 52-57 行 `AbiFacts` 增加 `Inputs`。
  - 第 159-170 行 `collectAbiFacts()` 读取 ABI input registers。
  - 第 233-239 行 `run()` 改成调用新的 `removeDeadStoresByLiveness()`。
  - 第 321-459 行新增 CFG backward liveness 删除，包含 `transferBlockLiveness()`、`transferCallLiveness()`、`addExitLiveRegisters()`。
  - 第 667-715 行新增 `callReadsRegister()`，用 callee summary 或 ABI input 判断 call 是否读某个 register。
- `tests/native_register_summary_ssa_test.cpp`
  - 第 47-73 行 `attachTestAbi()` 增加 `RDI` 输入。
  - 第 312-340 行新增跨 block dead store 测试。
  - 第 343-370 行新增 call 前 ABI input store 保留测试。
  - 第 381-382 行把两个新测试接入 main。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target native_register_summary_ssa_test notdec-native-llvm native_register_summary_test native_register_effects_test -j2
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_effects_test
```

Bench2 结果：

```text
wrk -f 0x8300, summary SSA, no prototype recovery:
  dead_stores_removed=23
  line count 296 -> 267
  TIME 0.50s

wrk -f 0x8300, old NativeRegisterSSA, no prototype recovery:
  line count 159
  TIME 0.45s
  llvm-as / opt -passes=verify passed

wrk -f 0x8300, default后续 pipeline:
  dead_stores_removed=23
  line count 268
  TIME 0.44s

vsftpd --all-confirmed, summary SSA, no prototype recovery:
  line count 126067 -> 113980
  TIME 97.38s
  llvm-as / opt -passes=verify passed
```

复杂度评估：

- 实现效果：8/10。比前一版多删了一批真正跨 block 的 register residue，而且没有碰 partial / stack / memory。
- 理解成本：5/10。新增了一个很小的 backward liveness，但规则还算直接。
- 维护成本：4/10。后面如果要继续扩大，只需要沿着 register liveness 再补更细的 call/memory 约束，不用重做 SSA。

## 实现记录：summary SSA residue removal CLI gate

之前 `NativeRegisterSummarySSAOptions` 已有 `EnableResidueRemoval`，但 `notdec-native-llvm` 没有 CLI 入口。
这会让 plan 里的“不开启 residue 删除时也能 verify”只能从单测覆盖，不能直接用 native pipeline 验证。

本次新增：

```text
--no-summary-register-residue-removal
```

它只允许和 `--summary-register-ssa-pass` 一起使用。
默认行为不变，summary SSA 仍默认开启 residue removal。

改动文件：

- `tools/notdec-native-llvm.cpp`
  - 第 63-66 行新增 `DisableSummaryRegisterResidueRemoval`。
  - 第 79-82 行 usage 增加 `--no-summary-register-residue-removal`。
  - 第 146-152 行解析该 flag。
  - 第 258-266 行校验该 flag 必须搭配 `--summary-register-ssa-pass`。
  - 第 803-809 行把该 flag 映射到 `NativeRegisterSummarySSAOptions::EnableResidueRemoval`。

验证：

```text
git diff --check
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm -j2
```

Bench2 `wrk -f 0x8300` 对照：

```text
summary SSA, no residue removal:
  dead_loads_removed=0
  dead_stores_removed=0
  lines=305
  TIME 0.46s
  llvm-as / opt -passes=verify passed

summary SSA, residue removal enabled:
  dead_loads_removed=7
  dead_stores_removed=23
  lines=267
  TIME 0.46s
  llvm-as / opt -passes=verify passed
```

复杂度评估：

- 实现效果：7/10。补齐了 native CLI 层的 residue removal 对照 gate。
- 理解成本：2/10。只是一个显式开关，默认路径不变。
- 维护成本：2/10。后续 Bench2 audit 可以直接用这个开关区分 SSA rewrite 和 residue 删除效果。

## 实现记录：summary SSA Bench2 audit 脚本

之前 Bench2 结果主要靠手写命令。
这次新增一个 register-SSA 专用 audit 脚本，避免继续复用 prototype recovery audit 时混入签名恢复逻辑。

新增文件：

- `scripts/bench2-native-summary-ssa-audit.sh`
  - 第 4-11 行定义默认 build / Bench2 / LLVM 路径和参数状态。
  - 第 13-26 行 usage，说明这是 register SSA 专用 audit，prototype recovery 默认关闭。
  - 第 28-79 行解析 `--target`、`--mode`、`--decode-seed-limit` 等参数；默认跑 `summary-no-residue` 和 `summary-residue`。
  - 第 103-118 行复用 manifest 的 `PROJECT:ROLE` 查找方式。
  - 第 120-142 行定义三种 mode：`old`、`summary-no-residue`、`summary-residue`。
  - 第 144-161 行从 `--register-ssa-summary` stderr 里解析计数。
  - 第 171-225 行逐目标生成 IR、用 LLVM 22 `llvm-as` / `opt -passes=verify` 验证，并写 `metrics.tsv`。

验证：

```text
chmod +x scripts/bench2-native-summary-ssa-audit.sh
bash -n scripts/bench2-native-summary-ssa-audit.sh
git diff --check
```

Bench2 `wrk` 全量：

```text
scripts/bench2-native-summary-ssa-audit.sh \
  --target wrk:executable \
  --out-dir /tmp/notdec-bin2llvm-summary-ssa-audit-wrk
```

结果：

```text
wrk:executable summary-no-residue: 47s, 61037 lines, loads_replaced=5307, dead_loads_removed=0, dead_stores_removed=0, phis_created=4894, phis_simplified=1922
wrk:executable summary-residue:    49s, 47672 lines, loads_replaced=5307, dead_loads_removed=5306, dead_stores_removed=8032, phis_created=4894, phis_simplified=1922
```

Bench2 三目标 seed50：

```text
scripts/bench2-native-summary-ssa-audit.sh \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable \
  --decode-seed-limit 50 \
  --out-dir /tmp/notdec-bin2llvm-summary-ssa-audit-seed50
```

结果：

```text
vsftpd summary-no-residue:   11s, 3084 lines, loads_replaced=430, dead_loads_removed=0, dead_stores_removed=0
vsftpd summary-residue:      11s, 2168 lines, loads_replaced=430, dead_loads_removed=430, dead_stores_removed=473
libuv summary-no-residue:    11s, 2950 lines, loads_replaced=332, dead_loads_removed=0, dead_stores_removed=0
libuv summary-residue:       11s, 2147 lines, loads_replaced=332, dead_loads_removed=332, dead_stores_removed=447
memcached summary-no-residue: 10s, 2867 lines, loads_replaced=294, dead_loads_removed=0, dead_stores_removed=0
memcached summary-residue:    10s, 2146 lines, loads_replaced=294, dead_loads_removed=294, dead_stores_removed=409
```

所有输出都通过 LLVM 22 `llvm-as` / `opt -passes=verify`。

复杂度评估：

- 实现效果：8/10。Bench2 audit 从手写命令变成可重复脚本，并能直接比较 no-residue / residue。
- 理解成本：3/10。脚本只关心 register SSA，不和 prototype audit 混在一起。
- 维护成本：3/10。后续扩大目标或 seed limit 只需要追加 `--target` / `--decode-seed-limit`。

## 阶段审计：当前 goal 覆盖情况

对照本 plan 的技术路线和判断标准，当前新版链路已经形成闭环：

```text
NativeRegisterSummary
  -> bottom-up SCC summary
  -> top-down exit demand
  -> summary-based Register SSA
  -> CFG 级 register residue 删除
  -> notdec-native-llvm opt-in pipeline
  -> Bench2 audit script
```

已覆盖的判断标准：

- 空函数 / untouched register：summary 默认 missing cell 表示 preserved / no read。
- killed 后读取不算入口参数：`native_register_summary_test.cpp::testKilledReadDoesNotBecomeInput()`。
- callee 读 entry register 可以传回 caller entry：`testCalleeReadPropagatesToCallerEntry()`。
- sparse join 不丢 untouched path：`testSparseJoinKeepsUntouchedPath()`。
- `RAX/RDX` 都被写但 caller 只读 `RAX`：`testTopDownDemandKeepsOnlyUsedReturn()`。
- external / indirect call ABI fallback：summary 和 summary SSA 都读取 ABI input/output/effect metadata，Bench2 native pipeline 使用默认 x86-64 cspec metadata。
- SCC fixpoint：`NativeRegisterSummary.cpp` 已用 direct call graph SCC 做 bottom-up summary 和 top-down demand fixpoint；递归 SCC 走同一套有限 bit lattice。
- PHI incoming 数量：`native_register_summary_ssa_test.cpp::testPhiIncomingMatchesPredecessors()` 和 `testDuplicatePredecessorEdgesKeepPhiComplete()`。
- trivial PHI：`NativeRegisterSummarySSA.cpp::simplifyPhi()` 只在 PHI 完整后替换，并更新 `Replacement` 缓存。
- preserved call：`testPreservedCallKeepsPreviousValue()`。
- demanded return / clobber 不沿用 call 前 value：`testDemandedReturnCreatesCallValue()` 覆盖 demanded return helper，`callEffect()` 对 clobber 会生成独立 helper。
- residue 删除：`testOverwrittenStoreIsRemoved()`、`testCrossBlockDeadStoreIsRemoved()`、`testAbiInputStoreBeforeCallIsKept()`。
- 不开启 residue 删除也能 verify：`--no-summary-register-residue-removal` 已接入 CLI，并在 `wrk -f 0x8300` 验证。
- Bench2 verify：`wrk` 全量、`vsftpd/libuv/memcached` seed50、以及 `vsftpd/libuv/memcached` all-confirmed summary SSA 都通过 LLVM 22 assemble/verify。

当前明确不做的点仍按第一版边界处理：

- stack 参数恢复。
- 一般 memory alias。
- frame-local 保存/恢复 callee-saved register 的完整证明。
- partial register 精细合并。
- 条件执行路径标记。
- Ghidra trial/use 兼容层。

关于旧 `NativeRegisterSSA`：

- 当前没有重命名为 `HeritageSSA`。
- 原因是新链路已经独立放在 `NativeRegisterSummarySSA`，并通过 `--summary-register-ssa-pass` 显式 opt-in。
- 旧 `NativeRegisterSSA` 仍服务默认旧 pipeline，避免影响已有 prototype recovery 结果。
- 因此“拆分/重命名旧 Heritage 链路”在当前阶段不是必须代码动作；真正需要决定的是后续是否把 summary SSA 替换成默认链路。

当前阶段结论：

- 计划里的新版独立链路已经实现并有可重复 Bench2 audit。
- summary SSA 的 residue 删除已经从局部规则扩展到 CFG 级 register liveness。
- 默认 pipeline 仍保留旧 `NativeRegisterSSA`；是否切默认需要更大 Bench2 audit 后单独决策。

## 后续实现：旧链路改名 HeritageSSA，SummarySSA 切为默认

本节覆盖上一节末尾的旧结论。当前实现已经把旧链路改成 `HeritageSSA`，并把新版
`NativeRegisterSummarySSA` 设为 `notdec-native-llvm` 默认寄存器消除链路。

改动文件：

- `include/notdec-bin2llvm/passes/NativeHeritageSSA.h:14` 定义旧链路的
  `NativeHeritageSSAOptions`；第 19 行和第 64 行定义旧链路 summary 结构；第
  110-117 行说明该 pass 是旧 Ghidra-style fallback，并导出
  `runNativeHeritageSSA()` / `printNativeHeritageSSASummary()`。
- `lib/passes/NativeHeritageSSA.cpp:1` 改为包含 `NativeHeritageSSA.h`；第
  2790-2818 行把旧入口改名为 `runNativeHeritageSSA()`；第 2821-2823 行把
  summary 标题改成 `native heritage ssa summary`。
- `lib/CMakeLists.txt:12` 把旧源文件从 `passes/NativeRegisterSSA.cpp` 改为
  `passes/NativeHeritageSSA.cpp`。
- `tools/notdec-native-llvm.cpp:63-65` 把 CLI 状态改成
  `UseHeritageRegisterSSAPass`；第 80-82 行 usage 增加
  `--heritage-register-ssa-pass`，保留 `--summary-register-ssa-pass` 作为兼容空
  开关；第 147-152 行解析这两个参数；第 262-270 行校验 Heritage 与
  `--no-register-ssa-pass` / `--no-summary-register-residue-removal` 的冲突；第
  802-826 行默认运行 `runNativeRegisterSummarySSA()`，只有显式
  `--heritage-register-ssa-pass` 时才运行 `runNativeHeritageSSA()`。
- `tests/native_register_effects_test.cpp:2`、第 1855-1858 行、第 2233-2234 行改
  用 Heritage 名字，继续覆盖旧链路行为。
- `tests/native_instcombine_metadata_test.cpp:3`、第 436-461 行、第 628 行等直接调
  旧 SSA 的位置改用 Heritage 名字，继续覆盖 metadata/prototype 相关旧链路回归。
- `scripts/bench2-native-summary-ssa-audit.sh:16` 增加 `heritage` mode；第 123-135
  行让 `heritage|old` 显式加 `--heritage-register-ssa-pass`，summary mode 直接使用
  默认 SummarySSA，不再依赖 `--summary-register-ssa-pass`。

验证命令：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm \
  native_register_summary_ssa_test native_register_summary_test \
  native_register_effects_test native_instcombine_metadata_test \
  native_prototype_recovery_test -j2

/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_effects_test
/tmp/notdec-bin2llvm-build/bin/native_instcombine_metadata_test
/tmp/notdec-bin2llvm-build/bin/native_prototype_recovery_test
```

全部通过。

CLI 同口径验证：

```text
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk -f 0x8300 \
  -o /tmp/wrk-default-summary.ll --register-ssa-summary \
  --no-prototype-recovery-pass

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/wrk-default-summary.ll \
  -o /tmp/wrk-default-summary.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/wrk-default-summary.bc -o /tmp/wrk-default-summary.verified.bc
```

默认路径输出 `Native register summary SSA`，说明已经走 SummarySSA。结果：267 行。

显式 Heritage 验证：

```text
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk -f 0x8300 \
  -o /tmp/wrk-heritage.ll --heritage-register-ssa-pass \
  --register-ssa-summary --no-prototype-recovery-pass
```

输出 `native heritage ssa summary`，并通过 LLVM 22 assemble/verify。结果：159 行。

Bench2 小样本审计：

```text
scripts/bench2-native-summary-ssa-audit.sh --target wrk:executable \
  --decode-seed-limit 50 \
  --out-dir /tmp/notdec-summary-default-switch-wrk50
```

结果：

```text
summary-no-residue: 11s, 3225 lines
summary-residue:    11s, 2339 lines
```

Heritage mode 验证：

```text
scripts/bench2-native-summary-ssa-audit.sh --target wrk:executable \
  --mode heritage --decode-seed-limit 50 \
  --out-dir /tmp/notdec-summary-default-switch-wrk50-heritage
```

结果：10s，1867 lines。

复杂度评估：

- 实现效果：8/10。默认链路已经切到新版 SummarySSA，旧链路仍可显式回退。
- 理解成本：4/10。代码里不再有“默认 register SSA”与旧 Ghidra-style 实现同名的问题。
- 维护成本：3/10。旧链路只保留 fallback 入口，后续主要修 SummarySSA。

## 后续修复：SummarySSA 跳过 LLVM intrinsic call

问题：

`/tmp/wrk-default-summary.ll` 里 `@notdec.register.summary_return.i64()` 出现在
`llvm.ctpop`、`llvm.ssub.with.overflow` 这类 LLVM intrinsic 后面。这是错误的。
intrinsic 是 lifted IR 内部计算，不是底层二进制里的函数调用，不应该按 ABI 读写寄存器。

原因：

`NativeRegisterSummary.cpp` 的 bottom-up summary 已经跳过 intrinsic，但
`NativeRegisterSummarySSA.cpp` 在 `readValueBefore()` 和 store liveness 里只跳过
`notdec.register.*` helper，没有跳过 LLVM intrinsic。结果是 SummarySSA 在重写寄存器
load 时，把 intrinsic 当成 external ABI call，并为 ABI output `RAX` 生成了假的
`summary_return.i64`。

改动文件：

- `lib/passes/NativeRegisterSummarySSA.cpp:159-165` 新增 `isAnalyzableCall()`，统一跳过
  `notdec.register.*` helper 和 LLVM intrinsic。
- `lib/passes/NativeRegisterSummarySSA.cpp:435-439` 在 register store liveness 里使用
  `isAnalyzableCall()`，intrinsic 不再 kill ABI output，也不读取 ABI input。
- `lib/passes/NativeRegisterSummarySSA.cpp:483-487` 在 `readValueBefore()` 里使用
  `isAnalyzableCall()`，intrinsic 不再触发 `callEffect()`，也不会生成
  `summary_return` / `summary_clobber` helper。
- `tests/native_register_summary_ssa_test.cpp:8` 引入 LLVM intrinsic 头文件。
- `tests/native_register_summary_ssa_test.cpp:285-311` 增加
  `testIntrinsicDoesNotCreateCallValue()`：在 store `RAX`、调用 `llvm.ctpop`、再读 `RAX`
  的场景下，要求 load 能被替换，并且 `CallReturnValues == 0`。
- `tests/native_register_summary_ssa_test.cpp:410` 接入新测试。

验证命令：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm \
  native_register_summary_ssa_test native_register_summary_test -j2

/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
```

全部通过。

`wrk -f 0x8300` 默认 SummarySSA 重新验证：

```text
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk -f 0x8300 \
  -o /tmp/wrk-default-summary-fixed.ll --register-ssa-summary \
  --no-prototype-recovery-pass

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/wrk-default-summary-fixed.ll \
  -o /tmp/wrk-default-summary-fixed.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/wrk-default-summary-fixed.bc -o /tmp/wrk-default-summary-fixed.verified.bc
```

结果：

```text
255 lines
loads_replaced=11
dead_loads_removed=11
dead_stores_removed=24
call_returns=0
unknown_call_effects=0
```

修复前同一函数是 267 lines，`call_returns=2`，并且有假的
`@notdec.register.summary_return.i64()`。修复后 `rg summary_return` 没有命中。

Bench2 小样本：

```text
scripts/bench2-native-summary-ssa-audit.sh --target wrk:executable \
  --decode-seed-limit 50 \
  --out-dir /tmp/notdec-summary-intrinsic-fix-wrk50
```

结果：

```text
summary-no-residue: 10s, 3213 lines
summary-residue:    10s, 2241 lines
```

同一 seed50 修复前 residue 结果是 2339 lines。这说明错误的 intrinsic call effect
消掉后，新链路 IR 变短，并且仍通过 LLVM 22 assemble/verify。

复杂度评估：

- 实现效果：9/10。修掉了一个明确语义错误，intrinsic 不再伪造 ABI 返回寄存器。
- 理解成本：2/10。和 bottom-up summary 的 call 判定保持一致。
- 维护成本：2/10。后续如果还有其它“非二进制 call”，可以集中扩展 `isAnalyzableCall()`。

## 后续修复：Pcode lowering 后清理不可达基本块

问题：

`/tmp/wrk-default-summary-fixed.ll` 里有很多 `; No predecessors!` 的空基本块，例如
`bb_5456`、`bb_5460`、`bb_5466`。这些块不是 SummarySSA 新引入的控制流，而是
Pcode lowering 阶段把函数范围内的所有 block 都建出来后，留下的不可达块。SummarySSA
删除寄存器 load/store 后，IR 更短，这些块更显眼。

处理：

这类块应该在 lifting 输出阶段清理，而不是放到寄存器 SSA pass 里。处理位置改到
`PcodeLowerer::lower()` 末尾：所有 pcode block 和外部跳转目标都 lower 完成后，调用
LLVM 的 unreachable block 清理。这样即使禁用寄存器 SSA，lift 出来的 IR 也不会带这些
无前驱块。

改动文件：

- `lib/PcodeToLLVM.cpp:17` 引入 `llvm/Transforms/Utils/BasicBlockUtils.h`。
- `lib/PcodeToLLVM.cpp:105-110` 在 `PcodeLowerer::lower()` 末尾处理完
  `ExternalTargetBlocks` 后调用 `llvm::EliminateUnreachableBlocks(Function)`。
- `tests/pcode_to_llvm_test.cpp:41-75` 增加
  `testUnreachablePcodeBlocksAreRemoved()`：构造两个 pcode return block，其中第二个无前驱，
  要求 lower 后 `bb_2000` 不存在。
- `CMakeLists.txt:231-244` 增加 `pcode_to_llvm_test` 和 `notdec.pcode_to_llvm.cfg`。

验证命令：

```text
cmake --build /tmp/notdec-bin2llvm-build --target pcode_to_llvm_test \
  native_register_summary_ssa_test notdec-native-llvm -j2

/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
```

通过。

`wrk -f 0x8300` 默认 SummarySSA 重新验证：

```text
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk -f 0x8300 \
  -o /tmp/wrk-default-summary-lower-clean.ll --register-ssa-summary \
  --no-prototype-recovery-pass

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/wrk-default-summary-lower-clean.ll \
  -o /tmp/wrk-default-summary-lower-clean.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/wrk-default-summary-lower-clean.bc -o /tmp/wrk-default-summary-lower-clean.verified.bc
```

结果：

```text
255 lines -> 219 lines
No predecessors 块全部消失
loads_replaced=11
dead_loads_removed=11
dead_stores_removed=24
call_returns=0
```

同时验证禁用寄存器 SSA 后的 lowerer 输出：

```text
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk -f 0x8300 \
  -o /tmp/wrk-no-regssa-lower-clean.ll --no-register-ssa-pass \
  --no-prototype-recovery-pass
```

结果：237 lines，`No predecessors` 没有命中，并通过 LLVM 22 assemble/verify。这说明清理
已经在 lifting 输出层完成，不依赖 SummarySSA。

Bench2 小样本：

```text
scripts/bench2-native-summary-ssa-audit.sh --target wrk:executable \
  --decode-seed-limit 50 \
  --out-dir /tmp/notdec-lower-unreachable-clean-wrk50
```

结果：

```text
summary-no-residue: 11s, 3210 lines
summary-residue:    11s, 2238 lines
```

复杂度评估：

- 实现效果：8/10。IR 更适合人工检查，也减少无用块。
- 理解成本：1/10。直接使用 LLVM 现成 CFG 清理。
- 维护成本：1/10。该清理和 SummarySSA 主逻辑解耦。

## 后续修复：PLT tail branch 不合并进 caller CFG

问题：

`stats_free` 里有一条真实指令：

```text
831d: jmp 0x5450 <free@plt>
```

`0x5450` 属于 `.plt`，不是 `.text`：

```text
.plt  0x5020 - 0x56f0
.text 0x5700 - 0xec7a
```

对应 PLT 代码：

```text
5450 <free@plt>: jmpq *GOT
5456:            pushq $0x42
545b:            jmp 0x5020 <.plt>
```

之前 discovery 把 `CALL free@plt` 识别成 external call，但把 `BRANCH free@plt`
当普通控制流，所以会把 `free@plt` body 拉进 `stats_free`。PLT stub 的第一条
`jmp *GOT` 又被 lowerer 建模成 `call @free(); ret void`，所以 IR 里出现很多看起来像
`ret void` 的 PLT 块。相邻的 `exit@plt`、`lua_pushfstring@plt` 等块也是同一个 PLT
线性 decode 窗口里被带进来的，不应该属于 `stats_free`。

处理：

discovery 层 direct `BRANCH` 到已知 PLT entry 时，记录 flow xref，但不把 PLT entry 加入
当前函数的 branch target worklist。这样 caller 不再合并 PLT body。

这里暂时不在 `PcodeToLLVM` 里特殊处理 direct `BRANCH -> ExternalCallTargets`。去掉 PLT
body 后，如果仍直接 lower 这种函数外 branch，当前 lowerer 会生成一个外部目标空 block。
这说明外部 tail branch 的 IR 表达还需要单独设计，不能用 lowering special case 掩盖
discovery 的 CFG 归属问题。

改动文件：

- `lib/NativeAnalysis.cpp:1661-1666`：direct branch 目标是 `lookupPltExternal()` 时，记录
  `sleigh-pcode-plt-tail-branch` xref 并 `continue`，不加入 `info.BranchTargets`。

验证命令：

```text
cmake --build /tmp/notdec-bin2llvm-build --target pcode_to_llvm_test \
  notdec-native-llvm -j2

/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
```

通过。

`wrk -f 0x8300` 验证：

```text
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk -f 0x8300 \
  -o /tmp/wrk-tailcall-summary.ll --register-ssa-summary \
  --no-prototype-recovery-pass

/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk -f 0x8300 \
  -o /tmp/wrk-tailcall-no-regssa.ll --no-register-ssa-pass \
  --no-prototype-recovery-pass
```

两者都通过 LLVM 22 assemble/verify。结果：

```text
summary:      204 lines
basic_blocks: 3
instructions: 9
bb_5450:      有一个外部目标空 block
PLT 相邻块:   不再出现
summary_return: 无命中
```

`--summary-json-out` 同口径显示 `stats_free` 现在是 3 个 basic blocks、9 条 instruction，
不再包含 PLT 相邻块。剩下的 `bb_5450: ret void` 是 lowerer 对函数外 direct branch 的
当前表达问题，后续需要用明确的 external tail branch IR 方案处理。

Bench2 小样本：

```text
scripts/bench2-native-summary-ssa-audit.sh --target wrk:executable \
  --decode-seed-limit 50 \
  --out-dir /tmp/notdec-tailcall-plt-wrk50
```

结果：

```text
summary-no-residue: 待重新跑更大样本
summary-residue:    待重新跑更大样本
```

复杂度评估：

- 实现效果：7/10。`stats_free -> free@plt` 这类 external tail jump 不再污染 caller CFG；
  external tail branch 的最终 IR 表达还没完成。
- 理解成本：3/10。需要知道 PLT entry 是 external target，不是普通函数内 flow。
- 维护成本：2/10。只改 discovery 的 target 归属，不改 lowerer 语义。

## 后续修复：Register SSA 后再跑一次 InstCombine

问题：

SummarySSA 删除寄存器 load/store 后，会留下很多只为寄存器副作用服务的死计算。例如
`stats_free` 里删除 `RDI` store 后，围绕 flags、`ctpop`、overflow 的计算已经没有用户，
但 pipeline 原来只在 RegisterSSA 前跑一次 InstCombine，后面不会再清理这些新死代码。

处理：

默认 pipeline 改成：

```text
InstCombine -> RegisterSSA -> InstCombine -> PrototypeRecovery
```

这样 RegisterSSA 删除寄存器访问后，后置 InstCombine 负责清掉新出现的死计算和可简化 CFG。

改动文件：

- `tools/notdec-native-llvm.cpp:901-910`：IR 输入路径在 `runRegisterSSAPassIfEnabled()` 后再调用一次
  `runInstCombinePassIfEnabled()`。
- `tools/notdec-native-llvm.cpp:1019-1026`：ELF native lowering 路径同样在 RegisterSSA 后再调用一次
  `runInstCombinePassIfEnabled()`。

验证命令：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm -j2

/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
```

全部通过。

`wrk -f 0x8300` 默认 SummarySSA：

```text
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk -f 0x8300 \
  -o /tmp/wrk-post-ssa-instcombine.ll --register-ssa-summary \
  --no-prototype-recovery-pass
```

结果：

```text
204 lines -> 142 lines
dead_loads_removed=11
dead_stores_removed=25
summary_return=0
剩余寄存器访问只剩 RDI.entry
```

Bench2 `wrk` seed50：

```text
scripts/bench2-native-summary-ssa-audit.sh --target wrk:executable \
  --decode-seed-limit 50 \
  --out-dir /tmp/notdec-post-ssa-instcombine-wrk50
```

结果：

```text
summary-no-residue: 11s, 2784 lines
summary-residue:    11s, 1478 lines
```

对比前一轮 `summary-residue` 约 2242 lines，后置 InstCombine 明显减少了 register SSA 后的死代码。

复杂度评估：

- 实现效果：8/10。明显减少 SummarySSA 后死代码，不改 SummarySSA 核心算法。
- 理解成本：2/10。pipeline 多跑一次已有清理 pass。
- 维护成本：2/10。复用现有 `runInstCombinePassIfEnabled()`，受同一个 `--no-instcombine-pass` 控制。
