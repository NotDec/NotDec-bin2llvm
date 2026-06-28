# Braun SSA 构建算法笔记

本文整理 Braun et al. 的 SSA 构建算法基础知识和总体流程。
目标是讲清楚算法怎么工作，以及实现时哪些状态不能省。

主要参考：

- Matthias Braun 等，`Simple and Efficient Construction of Static Single Assignment Form`
  - PDF: https://c9x.me/compile/bib/braun13cc.pdf
  - 项目页: https://compilers.cs.uni-saarland.de/projects/ssaconstr/
- SPIRV-Tools `source/opt/ssa_rewrite_pass.cpp`
  - https://github.com/KhronosGroup/SPIRV-Tools/blob/main/source/opt/ssa_rewrite_pass.cpp
- Cranelift frontend `ssa.rs`
  - https://github.com/bytecodealliance/wasmtime/blob/main/cranelift/frontend/src/ssa.rs
- 本仓库已有记录：
  - `logs/20260529-01-native-prototype-recovery-pass/08-register-elimination/20260610-02-braun-ssa-reference.md`

## 要解决的问题

SSA 的要求很简单：每个变量只能定义一次，每个 use 都直接指向一个明确的 SSA value。
普通命令式代码里，一个变量可以被反复赋值：

```text
x = 1
if cond:
  x = 2
use(x)
```

到合流点时，`use(x)` 可能看到 `1`，也可能看到 `2`。
SSA 用 PHI 表达这个合流：

```text
x0 = 1
x1 = 2
x2 = phi(x0, x1)
use(x2)
```

传统 Cytron 算法通常先有一个非 SSA CFG，再算 dominance frontier，批量插 PHI，最后 rename。
Braun 算法换了方向：它不先全局算 PHI 位置，而是在读变量时往前找 reaching definition。
找的过程中遇到 CFG 合流点，再按需插 PHI。

所以它的核心特点是：

- lazy：只有变量被读到时才构造它需要的 SSA value。
- backwards：从 use 往 predecessor 方向找定义。
- memoized：找过的 `(variable, block)` 会缓存，避免重复递归。
- no dominance frontier：基础算法不需要先算支配树和 dominance frontier。
- on-the-fly：可以边生成 CFG 边生成 SSA，不一定要先生成非 SSA IR。

## 核心概念

### variable

算法里的 `variable` 是源语言里的可变变量。
它不是 SSA value，而是“还没有改写前”的那个变量名。

在 native register SSA 场景里，可以把一个 register unit 看成一个 variable。
比如 `RAX` 对应一个 variable，算法负责把多次读写 `RAX` 改成 SSA value 流。

### SSA value

SSA value 是一次定义的结果。
赋值、表达式、PHI 都可以产生 SSA value。

算法做的事就是把：

```text
read variable V at block B
```

变成：

```text
use SSA value X
```

### currentDef

`currentDef[variable][block]` 记录这个 block 内当前已知的变量定义。

它有两个作用：

1. block 内顺序处理语句时，记录最近一次写入。
2. 跨 block 查找时，缓存 `(variable, block)` 的结果。

没有这个缓存，循环 CFG 会递归不止，普通 CFG 也会重复查很多次。

### filled block

一个 block 是 filled，意思是它里面的指令已经生成完了。
Braun 论文要求：只有 filled block 才能作为别的 block 的 predecessor。

直觉上，predecessor 如果还没填完，就不能保证它已经记录了变量的最新定义。

### sealed block

一个 block 是 sealed，意思是它的 predecessor 列表已经完整，不会再加新的 incoming edge。

这是处理“边生成 CFG 边生成 SSA”的关键。
如果 block 还没 sealed，就不能按当前 predecessor 列表补 PHI，因为后面可能还会多出一个 predecessor。

### incomplete phi

如果在 unsealed block 里读一个还没有 currentDef 的变量，算法会先创建一个没有 operands 的 PHI。
这个 PHI 是临时代理，记录到 `incompletePhis[block][variable]`。

它不能长期留在最终 IR 里。
等 block sealed 后，必须按完整 predecessor 列表补 operands，然后再尝试删除 trivial PHI。

### trivial phi

trivial PHI 是没有真正合并多个不同值的 PHI。
典型例子：

```text
phi(x, x)
phi(self, x)
phi(self, x, x)
```

这些 PHI 可以替换成 `x`。
如果一个 PHI 除了 self reference 没有别的真实 operand，一般替换成 `undef` 或实现里定义的 unknown value。

删除 trivial PHI 后，使用它的其它 PHI 可能也变成 trivial。
所以需要递归检查 users，或者有一个等价的后处理机制。

## 基本数据结构

一个最小实现通常需要这些状态：

```text
currentDef:       Map<Variable, Map<Block, Value>>
sealedBlocks:     Set<Block>
incompletePhis:   Map<Block, Map<Variable, Phi>>
```

如果 IR 没有 value users 列表，还需要额外维护 PHI users 或者做后处理。
如果是在已有完整 CFG 上跑 pass，所有 block 一开始都可以视为 sealed。
但这不代表可以省掉 PHI 完整性检查：任何临时 PHI 最后都必须补齐 incoming。

## 局部流程：一个 block 内

同一个 block 内没有控制流合并，处理方式就是“最近一次定义”。

```text
writeVariable(var, block, value):
  currentDef[var][block] = value

readVariable(var, block):
  if currentDef[var] has block:
    return currentDef[var][block]
  return readVariableRecursive(var, block)
```

例子：

```text
x = 1
y = x
x = 2
z = x
```

处理后：

```text
x0 = 1
y0 = x0
x1 = 2
z0 = x1
```

这里不需要 PHI。
PHI 只在变量定义可能从不同 predecessor 流入时才需要。

## 全局流程：从 use 往前找定义

当 `readVariable` 在当前 block 找不到 `currentDef`，就进入递归查找。

简化伪代码如下：

```text
readVariableRecursive(var, block):
  if block is not sealed:
    phi = new empty phi in block
    currentDef[var][block] = phi
    incompletePhis[block][var] = phi
    return phi

  preds = predecessors(block)

  if preds is empty:
    value = initialValue(var)
    currentDef[var][block] = value
    return value

  if preds has one block:
    value = readVariable(var, preds[0])
    currentDef[var][block] = value
    return value

  phi = new empty phi in block
  currentDef[var][block] = phi

  for pred in preds:
    phi.addIncoming(readVariable(var, pred), pred)

  value = simplifyTrivialPhi(phi)
  currentDef[var][block] = value
  return value
```

这里最重要的一步是：多 predecessor 时，先创建空 PHI，并立刻写入 `currentDef`，再递归查 operands。

这是为了处理循环。
如果递归沿着 back edge 又回到同一个 block，`currentDef` 里已经有这个 PHI，可以直接返回，递归就停住了。

## if 合流例子

源程序：

```text
entry:
  x = 1
  if cond goto then else goto merge

then:
  x = 2
  goto merge

merge:
  use(x)
```

在 `merge` 里读 `x`：

1. `merge` 没有 `currentDef[x]`。
2. `merge` 有两个 predecessor：`entry` 和 `then`。
3. 先在 `merge` 创建 `phi`，写入 `currentDef[x][merge]`。
4. 查 `entry` 的 `x`，得到 `1`。
5. 查 `then` 的 `x`，得到 `2`。
6. `phi(1, 2)` 不是 trivial，保留。
7. `use(x)` 改成 `use(phi(1, 2))`。

## loop 例子

源程序：

```text
preheader:
  x = 0
  goto header

header:
  use(x)
  if cond goto body else goto exit

body:
  x = x + 1
  goto header
```

`header` 有两个 predecessor：`preheader` 和 `body`。
其中 `body -> header` 是 back edge。

如果 CFG 已经完整，读 `header` 的 `x` 时：

1. `header` 没有 `currentDef[x]`。
2. `header` 有两个 predecessor，所以创建空 PHI，并先写入 `currentDef[x][header]`。
3. 查 `preheader`，得到 `0`。
4. 查 `body`，`body` 里的 `x + 1` 又会读 `header` 的 `x`。
5. 这时 `currentDef[x][header]` 已经是刚才的 PHI，递归停住。
6. 最后得到类似：

```text
x_header = phi(0, x_body)
x_body = x_header + 1
```

如果 CFG 是边生成边处理，`header` 一开始可能还不知道 `body` 这个 predecessor。
这时 `header` 不能 sealed。
在 unsealed `header` 里读 `x`，算法只能先放 incomplete PHI。
等 back edge 加完，再 seal `header`，补上 `preheader` 和 `body` 两条 incoming。

## sealBlock 流程

`sealBlock(block)` 的职责只有一个：这个 block 的 predecessor 已经完整了，把之前欠着的 incomplete PHI 补完。

简化伪代码：

```text
sealBlock(block):
  for each (var, phi) in incompletePhis[block]:
    for pred in predecessors(block):
      phi.addIncoming(readVariable(var, pred), pred)
    value = simplifyTrivialPhi(phi)
    currentDef[var][block] = value

  mark block sealed
```

实现时常见顺序是先补 incomplete PHI，再把 block 标成 sealed，或者先标记再补。
关键不是顺序名字，而是递归查 predecessor 时必须看到一致的状态，不能让 operandless PHI 漏到最终 IR。

## trivial PHI 删除

删除 trivial PHI 的判断可以这样理解：

```text
real = none

for operand in phi.operands:
  if operand == phi:
    continue
  if real is none:
    real = operand
    continue
  if operand != real:
    return phi

if real is none:
  real = undef

replace phi by real
recheck phi users
return real
```

这里要注意三点：

1. self reference 不算一个真实不同值。
2. `phi(x, x)` 和 `phi(self, x)` 都可以替换成 `x`。
3. 替换后要更新缓存和 users，否则后续可能还会拿到已经删除的 PHI。

## 完整流程

如果是从 AST 或字节码直接构造 SSA，典型流程是：

1. 创建 block。
2. 按顺序生成 block 内指令。
3. 遇到变量写入，调用 `writeVariable`。
4. 遇到变量读取，调用 `readVariable`，必要时递归向 predecessor 查找。
5. 生成控制流 edge。
6. 当一个 block 的 predecessor 列表确定后，调用 `sealBlock`。
7. 所有 block 处理完后，确认没有 incomplete PHI 遗留。
8. 对 PHI 做 trivial 删除，必要时对 irreducible CFG 做 SCC 级别清理。

如果 CFG 已经完整，流程更像一个 SSA rewrite pass：

1. 把所有 block 当成 sealed。
2. 从 load/use 出发调用 `readVariable`。
3. 递归查 predecessor，必要时插 PHI。
4. 用得到的 SSA value 替换原来的 load/use。
5. pass 结束前检查所有 pending PHI。
6. 每个保留 PHI 的 incoming 数必须等于所在 block 的 predecessor 数。

## 算法性质

### 为什么是 pruned SSA

PHI 是按需创建的。
只有某个变量真的被读到，算法才会为这个 use 往前找定义，并在必要时插 PHI。

因此它天然避免了大量死 PHI。
论文把这个性质描述为会构造 pruned SSA：保留下来的 PHI 至少是为了某个 use 链服务的。

### reducible CFG 上能得到 minimal SSA

对 reducible CFG，配合 trivial PHI 删除，Braun 基础算法能得到 minimal SSA。

直观理解：

- 只有不同定义真的在合流点被同一个 use 需要时，PHI 才会产生。
- 如果递归过程中为了打断循环临时放了多余 PHI，trivial 删除会把它消掉。

### irreducible CFG 需要额外清理

对 irreducible CFG，基础算法可能留下非 trivial 但仍冗余的 PHI 组。
论文给了一个 SCC 后处理：把 PHI 之间形成的数据流环找出来，如果整个环外面只流入一个真实值，就把这组 PHI 替换成那个值。

这不是理解基础流程的第一步，但实现真实编译器时要知道这个边界。

## 常见实现方式

### 直接 PHI

LLVM IR、SPIR-V 这类 IR 可以直接插 PHI。
SPIRV-Tools 的 `SSARewriter` 就是这种风格：从 load 查 reaching definition，沿 CFG 往前找 store，在 join 点创建 PHI candidate，并有 finalize 逻辑处理 incomplete PHI。

### block parameter

Cranelift 使用 block parameter 表达 PHI。
变量在 block 入口处需要合流时，不一定创建显式 PHI 指令，而是在 block 上加参数，并在 predecessor terminator 上传入对应参数。

这只是 IR 表达不同。
核心还是 Braun 思路：

- `def_var` 对应 `writeVariable`。
- `use_var` 对应 `readVariable`。
- `seal_block` 对应 incomplete PHI 收尾。

## 在 NotDec-bin2llvm 里的理解方式

本仓库日志里的问题背景是 native register SSA。
这里不要把算法里的 variable 理解成 C 局部变量，而要理解成一个待消除的寄存器单元。

大致对应关系：

```text
Braun variable        -> register unit
writeVariable         -> 某条指令定义/写寄存器
readVariable          -> 某条指令读取寄存器值
currentDef            -> block 内或 block 边界处缓存的寄存器 SSA value
PHI                   -> 多个 predecessor 流入的寄存器值合流
incomplete phi        -> 递归或 CFG 未 sealed 时的临时占位
seal/finalize         -> 给临时 PHI 补齐 incoming
```

native register SSA 还多了 Braun 论文没有的机器语义：

- 一个寄存器可能有 partial read/write。
- call 可能 clobber caller-saved register。
- 函数入口寄存器值可能要表示为 external input。
- 某条路径上的寄存器值可能未知。

这些语义可以扩展在 `readVariable` 周围，但不能破坏 SSA 基本规则。
最重要的底线是：

- 不能让 operandless PHI 留在最终 IR。
- block sealed 或 pass finalize 后，每个 PHI 都必须有完整 incoming。
- 遇到 call clobber 或未知值，要明确选择 `undef`、unknown value 或跳过替换，不能靠 malformed PHI 表达。
- trivial PHI 删除后，要同步更新 `currentDef`、替换表和其它 PHI operands。

## 实现检查清单

写 Braun 风格 SSA 构建时，可以按下面检查：

1. 每次 `readVariable(var, block)` 是否先查 `currentDef`。
2. 多 predecessor block 是否先创建 PHI 并写入 `currentDef`，再递归查 operands。
3. unsealed block 是否把临时 PHI 放进 `incompletePhis`。
4. `sealBlock` 或 finalize 是否补齐了所有 incomplete PHI。
5. 保留在 IR 里的 PHI incoming 数是否等于 predecessor 数。
6. trivial PHI 删除是否处理 self reference。
7. trivial PHI 删除后是否递归影响 users。
8. 替换 PHI 后，缓存里是否还指向旧 PHI。
9. 已有完整 CFG 的 pass 是否仍有最终 pending PHI 检查。
10. 对 irreducible CFG 是否接受非 minimal 结果，还是需要 SCC 清理。

## 一句话总结

Braun SSA 构建算法就是：变量被读到时再往前找定义，找不到就跨 predecessor 递归；合流处按需建 PHI；循环用先注册的空 PHI 打断递归；CFG 未完整时用 incomplete PHI 暂存；block sealed 后必须补 operands；最后删掉没有实际合流意义的 trivial PHI。
