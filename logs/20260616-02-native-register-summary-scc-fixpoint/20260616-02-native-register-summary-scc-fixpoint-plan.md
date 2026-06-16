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

## 背景

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
  entryDemand[R]: 函数入口 R 是否影响被观察结果，第一版只作为辅助信息,
  paramLower: ABI register 参数数量下界,
  paramUpper: ABI register 参数数量上界
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

顶层参数按 ABI 初始化参数边界：

```text
paramLower = max index of ABI input register with readEntry
paramUpper = ABI register argument count
```

这里的 `paramUpper` 很弱。
如果只从“外部可以传入任意 ABI 参数寄存器，但函数可以不用”这个黑盒角度看，顶层函数的上界几乎就是所有 ABI 参数寄存器。
所以上界本身意义不大，不能单独作为强参数证据。

top-down 的主收益仍然是 `exitDemand`，也就是确认哪些返回寄存器真的被使用。
参数部分只顺带维护上下界和 weak/strong 证据。

#### 8.2 从所有 caller 观察函数效果

internal function 的返回值需求来自所有 caller。
遍历 caller 时，对每个 direct callsite 临时计算：

```text
caller 在 call 后读取 R
callee summary 显示 R 可能是 non-entry
  -> callee.exitDemand[R] = true
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

#### 8.3 参数上下界

参数判断比返回值弱，因为“没被读到”不等于“外部没有传”。
第一版不要把 top-down 参数分析写成绝对精确，只维护上下界：

```text
paramLower:
  bottom-up readEntry 命中的最高 ABI 参数寄存器位置

paramUpper:
  从 caller 已知参数数量和 callsite 数据流能排除的最高位置
```

例子：

```text
wrapper_entry:
  call callee
```

如果 wrapper 入口立刻 call callee，所有 ABI 参数寄存器看起来都可能直接传给 callee。
这时只看 callsite 很难知道 callee 到底有几个参数。
但是如果 top-down 已经确认 wrapper 只有 2 个 ABI register 参数，那么从 wrapper entry 直接透传给 callee 的第 3、4、5、6 个 ABI 参数位置就不能作为强参数。

所以参数传播规则是：

```text
callee.readEntry[R_i] = true
callsite 的 R_i 来源是 caller entry R_i
caller.paramUpper < i
  -> callee 的第 i 个参数不能作为 strong 参数

callee.readEntry[R_i] = true
callsite 的 R_i 来源是 caller entry R_i
caller.paramLower >= i
  -> callee 的第 i 个参数有 caller 参数支持

callee.readEntry[R_i] = true
callsite 的 R_i 是 caller 本地计算出来的值
  -> callee 的第 i 个参数有 callsite 准备证据
```

这个规则只给约束，不强行精确恢复。
最终可以得到：

```text
paramLower <= 参数数量 <= paramUpper
```

顶层函数的 `paramUpper` 通常过大，所以主要靠 `readEntry` 给下界。
internal function 的 `paramUpper` 才可能被所有 caller 收紧。
只有明确来源的 callsite 才生成上界约束。
如果多个 caller 给出的约束冲突，例如 `paramLower > paramUpper`，说明至少有一个观察不可靠，consumer 应回退到 weak 参数结论，不用这个区间改 prototype。

#### 8.4 函数内 backward demand

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
但它不是 top-down 第一版的主要产物；参数恢复仍然优先用 `readEntry + paramLower/paramUpper + caller 约束` 综合判断。

#### 8.5 Call 指令的 demand 传播

call 是 top-down 的核心。
设 caller 在 call 后 demand `R`：

```text
callee exit mayEntry=true
  -> caller call 前 R 继续 demand

callee exit mayNonEntry=true
  -> callee.exitDemand[R] = true
```

参数方向则用 callee 的 `readEntry` 和当前 callsite 的来源关系生成约束：

```text
callee.readEntry[R_i] = true
  -> 分析 caller call 前 R_i 来源
  -> 更新 callee.paramLower / callee.paramUpper
```

如果后续 `entryDemand` 稳定，也可以作为参数置信度加分项，但第一版不把它作为唯一判据。

#### 8.6 Top-down SCC fixpoint

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
paramLower: 0 -> ABI register argument count
paramUpper: ABI register argument count -> 0
```

CFG backward 合流使用 OR。
`exitDemand/entryDemand` 只增不减，`paramLower` 单调增，`paramUpper` 单调减，最终会收敛。

#### 8.7 消费规则

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

参数用区间和证据综合判断：

```text
index(R) <= paramLower
EffectSummary.readEntry[R] = true
  -> 强输入参数候选

paramLower < index(R) <= paramUpper
EffectSummary.readEntry[R] = true
  -> weak 输入参数候选

index(R) > paramUpper
  -> 不作为参数
```

这里的 `paramUpper` 如果来自顶层 ABI 默认值，意义较弱。
只有被 caller 约束收紧后，才适合用来排除参数。

### 9. Prototype recovery 消费

新版链路不再需要 caller-side trial/use 作为主判据。

函数输入先来自 bottom-up 的 `readEntry`：

```text
callee summary: readEntry[RDI] = true
callsite: 当前 RDI 抽象状态
  -> RDI 是这个 callsite 的 weak 参数来源
```

top-down 后再看参数上下界：

```text
index(RDI) <= DemandSummary.paramLower
  -> strong 参数候选

DemandSummary.paramLower < index(RDI) <= DemandSummary.paramUpper
  -> weak 参数候选

index(RDI) > DemandSummary.paramUpper
  -> 不作为参数
```

`entryDemand` 只作为辅助置信度，不单独决定参数数量。

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

### 10. Register residue 删除

summary 可用于更系统地删除 residue：

- caller call 前写了 `RDI`，但 callee summary 不读 `RDI`：这类 store 可以进入删除候选。
- caller call 后读 `RAX`，且 callee summary 显示 `RAX` 是 ABI output 上的 changed exit value：接到 call result。
- caller call 后继续读 `RBX`，且 callee summary preserved `RBX`：继续使用 call 前 state。
- caller call 后读 caller-saved register，callee summary modified：不能沿用 call 前 state。

第一版先只做 summary 和 metadata / audit。
真正删除 residue 可以单独接在验证稳定之后。

## 不做什么

第一版不做：

- stack 参数恢复。
- 一般 memory alias。
- 栈保存/恢复的完整证明。
- partial register 精细合并。
- 条件执行路径标记。
- Ghidra trial/use 兼容层。

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
- 顶层 `paramUpper` 默认等于 ABI 参数寄存器数量，基本不能用来排除参数。
- internal function 的 `paramUpper` 只有在 callsite 来源明确且没有冲突时才可信。
- 只看 caller 读取寄存器时，未建模的内存/异常/间接调用可能隐藏真实观察点。

风险处理：

- 第一版宁可保守，不从混合状态生成 signature rewrite。
- top-down 结果只用于增强返回值/参数置信度，不删除 bottom-up effect summary。
- root/export 使用 ABI return register 作为 demand seed；不确定入口先保守保留 weak candidate。
- 参数上界只作为排除证据使用；遇到上下界冲突，回退到 weak 参数结论。
- summary 先写 metadata / 日志，不直接大规模删 IR。
- 删除 register residue 必须作为后续阶段，等 summary 稳定后再做。

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
- wrapper 入口直接 call callee，且 wrapper 参数上界已知为 2：callee 的第 3 个透传 ABI 参数不能作为 strong 参数。
- 顶层函数没有 caller 时，参数上界保持 ABI 默认值，不用它排除参数。
- 递归 SCC 内 demand 能收敛。

Bench2 判断标准：

- summary pass 能跑完当前 Bench2 native IR。
- 输出每个函数的 register read / preserved / modified / return 统计。
- 输出 demanded output / weak output 统计，检查多返回寄存器候选是否减少。
- 不引入 `llvm-as` / `opt verify` 回归。
- 在不开启 residue 删除时，IR 行为不变。
