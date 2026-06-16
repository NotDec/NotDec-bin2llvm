# Native register summary SCC fixpoint plan

用户原始要求：

> 外部函数/间接调用暂时就看ABI，是没问题的。stack 参数可能后续再拓展覆盖吧，初版先不考虑，不过确实是一个值得考虑的点。partial register感觉也没必要。就直接当做存放了整个寄存器吧。条件路径没必要啊，直接就当做两边路径都被改了就行了。内存和栈一样，都先不考虑。后期的话，也只是考虑占空间上的内存，其他地方的内存完全不考虑。未知clobber应该没啥吧。按这个写一个plan文档吧。文档可以明确说一下，这个是一个新版的单独链路，和之前的模仿Ghidra的做法完全独立，也不用参考那边

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
- 不把未知 clobber 作为核心状态；不能证明时退到 ABI fallback 或 conservative mixed。

## 目标

做一个新的 register summary 分析链路，目标是给每个函数产出稳定 summary：

```text
register -> {
  是否读取过函数入口值,
  正常返回时是否仍等于入口值,
  是否被本函数写成新值,
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

### 2. 抽象值

函数内部不修改 IR，只做只读数据流。

每个 register 当前值用少量抽象值表示：

```text
Entry(R)          函数入口时 R 的值
LocalDef(id)      本函数内普通定义
CallReturn(c, R)  call c 产生的返回寄存器值
CallClobber(c, R) call c 后该寄存器被改写
Mixed             多路径合并后不一致
Unknown           无法解释，保守
```

`Unknown` 只作为保守收口，不作为主要语义。
正常情况下，外部/间接 call 由 ABI fallback 直接产生 preserved / return / clobber 效果。

### 3. Basic block summary

先对每个 basic block 生成 transfer summary：

```text
block input register state
  -> block output register state
  -> block 内读取了哪些 Entry(R)
  -> block 内写了哪些 register
```

普通指令按寄存器读写更新：

- 读 register：
  - 读取当前抽象值。
  - 如果当前值是 `Entry(R)`，标记本函数 read entry `R`。
- 写 register：
  - 当前 register state 变成 `LocalDef(id)`。
- call：
  - direct internal call 用 callee summary。
  - external / indirect call 用 ABI fallback。

这里不判断 caller 有没有“准备参数”。
只要 callee summary 说它读取某个 register，当前 reaching value 就是这个 callsite 的参数来源。

### 4. CFG 合并

在 CFG join 点合并 register state：

```text
Entry(R) + Entry(R)        -> Entry(R)
LocalDef(a) + LocalDef(a)  -> LocalDef(a)
相同 CallReturn           -> CallReturn
其它不一致                -> Mixed
Unknown 参与              -> Unknown
```

条件路径不单独保留。
只要不同路径给同一个 register 产生不同值，就进入 `Mixed`，后续按保守效果处理。

函数 exit summary 从所有 return block 合并得到：

- 所有 return 都是 `Entry(R)`：`R` preserved。
- 任一路径是 `LocalDef` / `CallReturn` / `CallClobber` / `Mixed`：`R` modified。
- 如果 modified register 是 ABI return register，并且值不是 entry value，可作为 return 候选。

### 5. Function summary

每个函数对每个 register 输出：

```text
ReadEntry:
  no
  yes

ExitEffect:
  preserved
  modified
  return_candidate
  mixed

CallUse:
  这个函数是否会把入口 R 作为内部 call 参数继续传递
```

第一版先不区分 preserve-only read 和 semantic read。
原因是 stack/memory 保存恢复不在范围内；没有栈建模时，强行区分容易制造假精度。
如果后续加入 frame-slot 分析，再补这个维度。

### 6. Callsite 应用

分析 caller 的 call 指令时：

```text
callee reads R
  -> 当前 caller state[R] 是 callsite argument source

callee preserves R
  -> caller state[R] 不变

callee returns R
  -> caller state[R] = CallReturn(call, R)

callee modifies R
  -> caller state[R] = CallClobber(call, R)
```

external / indirect call 没有 callee summary，直接用 ABI：

- ABI unaffected：state 不变。
- ABI output：`CallReturn`。
- ABI killed-by-call：`CallClobber`。

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
summary lattice 必须有限，避免来回震荡：

```text
preserved -> modified -> mixed/unknown
no read   -> read
no return -> return_candidate -> mixed
```

一旦某个寄存器效果升级为更保守状态，不再降级。

### 8. Prototype recovery 消费

新版链路不再需要 caller-side trial/use 作为主判据。

函数输入来自 callee summary：

```text
callee summary: reads RDI
callsite: current state[RDI] = Entry(RDI) 或 LocalDef(...)
  -> RDI 是这个 callsite 的参数来源
```

函数返回来自 callee exit summary：

```text
callee summary: RAX return_candidate
caller call 后 state[RAX] = CallReturn(call, RAX)
  -> RAX 是返回值来源
```

如果 callee 没读某个 ABI input register，则 caller 对该 register 的写入不应该被当成参数证据。

### 9. Register residue 删除

summary 可用于更系统地删除 residue：

- caller call 前写了 `RDI`，但 callee summary 不读 `RDI`：这类 store 可以进入删除候选。
- caller call 后读 `RAX`，且 callee summary 返回 `RAX`：接到 call return value。
- caller call 后继续读 `RBX`，且 callee summary preserved `RBX`：继续使用 call 前 state。
- caller call 后读 caller-saved register，callee summary modified：使用 call clobber value。

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
- `Mixed` 状态如果消费侧处理不严，可能误当 precise value。

风险处理：

- 第一版宁可保守，不从 `Mixed/Unknown` 生成 signature rewrite。
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
- `Mixed` 不参与 prototype rewrite。

Bench2 判断标准：

- summary pass 能跑完当前 Bench2 native IR。
- 输出每个函数的 register read / preserved / modified / return 统计。
- 不引入 `llvm-as` / `opt verify` 回归。
- 在不开启 residue 删除时，IR 行为不变。
