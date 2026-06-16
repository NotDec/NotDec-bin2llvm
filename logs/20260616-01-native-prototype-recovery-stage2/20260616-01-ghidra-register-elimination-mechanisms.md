# Ghidra register elimination mechanisms

用户原始要求：

> 写一个文档，详细分析一下Ghidra的这些所有的机制，以及他们是顺序运行的几个阶段呢？还是说在heritage阶段按需调用的分析？

## 结论

Ghidra 不是一个单独的“寄存器消除 pass”。
它更像一套围绕 `heritage`、`ParamTrial`、`HighVariable`、`Cover`、`symbol`、`type` 的分析链。

其中有两种关系：

1. 有明确阶段顺序的东西。
2. 在这些阶段内部按需调用、互相反馈的东西。

所以答案不是“纯顺序流水线”，也不是“全部塞进 heritage 里一次做完”，而是两者都有。

## 总体顺序

可以把主流程理解成这样：

1. 先有 raw p-code / varnode / callsite。
2. `heritage` 先把寄存器和内存上的值变成 SSA。
3. `guardCalls()` 先把 call 对 storage 的影响显式化。
4. `ParamTrial` / `FuncCallSpecs` 再判断某个值是不是参数、返回值、还是无效 use。
5. `HighVariable` 把多个相关 varnode 合成一个高层变量。
6. `Cover` 检查这些合并是否安全。
7. `type` 和 `symbol` 在这个基础上再往上收。

这几个步骤不是一次性单向跑完，而是会回头影响前面的结果。

## 1. heritage 是核心，但不是全部

heritage 的作用是建 SSA。

关键点在：

- 寄存器值不是直接“消失”。
- call 不只是断点。
- `MULTIEQUAL` / `INDIRECT` / `COPY` 会被插进数据流里，帮助说明值从哪里来。

对应源码：

- [heritage.cc](</sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc:1443>) `Heritage::guardCalls()`
- [heritage.cc](</sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc:2663>) `Heritage::heritage()`

`guardCalls()` 先看每个 call 对某个 storage 是：

- `unaffected`
- `killedbycall`
- `return_address`
- `unknown_effect`

然后决定是直接穿过去，还是插 `INDIRECT` / `newIndirectCreation(...)`。

所以 heritage 不是“只做 renaming”，它先把 call 语义变成 SSA 可见的东西。

一个简单例子：

```text
RAX = 1
RBX = 2
call foo
RCX = RAX
RDX = RBX
```

如果直接“消除寄存器”，很容易写成：

```text
rax0 = 1
rbx0 = 2
call foo
rcx0 = rax0
rdx0 = rbx0
```

这就是错的。`call foo` 之后的 `RAX` 不一定还是 `1`。
在 x86-64 SysV 里，`RAX` 通常是返回寄存器，也是 caller-saved。
但 `RBX` 是 callee-saved，默认应该认为 call 后还保持旧值。

Ghidra 的处理更接近这样：

```text
rax0 = 1
rbx0 = 2
call foo
rax1 = INDIRECT_CREATE(call foo, RAX)   ; call 创建的新值，可能是返回值或 clobber
rcx0 = rax1
rdx0 = rbx0                             ; RBX unaffected，继续用 call 前的值
```

如果 call effect 更明确，比如 `RAX` 是返回值，可以理解成：

```text
rax1 = CALL_RETURN(call foo, RAX)
```

如果只是 killed-by-call，不能当返回值，就只能理解成：

```text
rax1 = CALL_CLOBBER_UNKNOWN(call foo, RAX)
```

重点是：Ghidra 不会直接把 call 后的 `RAX` 接到 call 前的 `rax0`。
它会先插一个解释 call effect 的数据流节点，再让 SSA 去连接后面的 use。
这样后面的参数恢复、返回值恢复、dead code 和 type recovery 才知道这个值到底是“call 返回值”、“call 杀掉后的未知值”，还是“preserved register 的旧值”。

## 2. trial/use 不是独立前处理，是 heritage 结果上的判定层

`ParamTrial` / `FuncCallSpecs` 这块是判断：

- 一个 call input 候选是不是参数。
- 一个 output 候选是不是返回值。
- 一个值是不是只被当前 call 用。
- 一个值是不是还有别的 use，不能当参数。

关键源码：

- [fspec.cc](</sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc:5567>) `FuncCallSpecs::checkInputTrialUse()`
- [funcdata_varnode.cc](</sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/funcdata_varnode.cc:1756>) `Funcdata::checkCallDoubleUse()`
- [funcdata_varnode.cc](</sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/funcdata_varnode.cc:1805>) `Funcdata::onlyOpUse()`

这里不是简单查“前面有没有 store”。
它会沿 use 链往前追，判断：

- 常量
- entry input
- local def
- COPY / CAST / PHI / PIECE / SUBPIECE
- 另一个 call
- 普通副作用 use

这说明 trial/use 是建立在 heritage 之后，但它本身也是一套递归分析。

这里 `Trial` 这个词很重要。
它不是“已经确定的参数”，而是“疑似参数位置”。
Ghidra 源码注释里说它是 putative parameter passing storage location，也就是暂时登记一个 storage：

```text
这个 call 可能把 RDI 当第 1 个参数。
这个 call 可能把 RSI 当第 2 个参数。
这个 call 后的 RAX 可能是返回值。
```

先登记 trial，而不是马上改 prototype，是因为 ABI 只能说明“这里可以传参”，不能说明“这次 call 真的用了它”。
比如：

```text
RDI = old_live_value
call foo
```

`RDI` 是 ABI input register，但如果它只是从函数入口一路活下来，没有本地准备动作，就不能马上断定 `foo(RDI)`。
所以 Ghidra 先把它放进 trial，后面再检查 use 链和 ancestor。

`ParamTrial` 和 `FuncCallSpecs` 的关系可以简单理解成：

```text
一个 CALL/CALLIND
  -> 一个 FuncCallSpecs
    -> activeinput: 一组输入 ParamTrial
    -> activeoutput: 一组输出 ParamTrial
```

`FuncCallSpecs` 是 callsite 级别的对象。
它知道这个 call 的 callee、prototype model、active input/output trial、call effect。
`ParamTrial` 是其中一个候选 storage，比如某个 register 或 stack slot。

`ParamTrial` 里会记录：

- storage 地址和大小。
- 当前 slot。
- 是否 checked。
- 是否 active。
- 是否 used。
- 是否 definitely not used。
- 是否 killed-by-call。
- 是否受 conditional execution 影响。

这些状态是逐步推进的：

```text
registerTrial()
  -> checkInputTrialUse()
  -> markActive / markInactive / markNoUse
  -> deriveInputMap()
  -> buildInputFromTrials()
```

结果会影响 p-code。
这点很关键。

在 `guardCalls()` 里，如果某个 storage 可能是 input，Ghidra 会：

```text
active->registerTrial(...)
vn = newVarnode(...)
opInsertInput(CALL, vn, CALL.numInput())
```

也就是先真的给 CALL p-code 加一个 input varnode。
这使得后续 heritage/SSA 可以把这个参数候选当成普通 use 来 rename。

后面如果 trial 被判定不是参数，`checkInputTrialUse()` 可能会把 CALL 对应 input 改成常量 0，释放这条数据流。
如果最终只有部分 trial 被确认使用，`buildInputFromTrials()` 会重建 CALL input 列表，只保留 `isUsed()` 的 trial。

所以 trial 不是只存在于旁路表里。
它一开始就会通过 `opInsertInput()` 影响 P-Code。
最终 `buildInputFromTrials()` 又会通过 `opSetAllInput()` 把 CALL 的输入列表收成最终结果。
这也是 Ghidra 这套方案比“额外 metadata 记录一下候选”更稳的地方：候选直接进入 SSA/use 链，后续删除、保留、改 prototype 都有真实数据流依据。

## 3. copy propagation 是配套机制，不是单独一层

Ghidra 里很多判断都默认 copy propagation 已经参与了。

原因很直接：

- call 参数值可能被 COPY 过。
- PHI 之间也可能只是值传递。
- 参数候选不能因为中间多了一层拷贝就失效。

`onlyOpUse()` 和 `ancestorOpUse()` 都是在这种前提下设计的：
它们允许穿过一部分“透明”操作，再决定这个值是否真的只服务当前 call。

所以 copy propagation 不是单独排在 heritage 前后某一步结束的东西。
它更像 heritage / trial/use / merge 过程中不断被利用的规则。

## 4. HighVariable 是更高层的变量视图

`HighVariable` 不是 SSA 的起点。
它是 SSA 之后把一组相关 varnode 收成一个逻辑变量的东西。

它主要负责：

- 把多个实例合成一个高层变量。
- 维护成员间关系。
- 提供类型、名字、symbol 的挂接点。

相关源码：

- [variable.hh](</sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/variable.hh:112>)
- [variable.cc](</sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/variable.cc:220>)

它和寄存器消除的关系是：

- SSA 先把“每个值是谁定义的”弄清楚。
- HighVariable 再把“这些值是不是同一个逻辑东西”弄清楚。

这一步常常要看 cover、type、symbol，不能只看 def-use。

## 5. Cover 是合并安全性的约束

Cover 解决的是“这个高层变量在代码里到底覆盖了哪些位置”。

它的用途不是装饰。
它是防止错误合并的硬约束。

比如：

- 两个 varnode 虽然名字像。
- 但它们在控制流上活跃区间重叠。
- 那就不能乱合。

相关位置：

- [variable.hh](</sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/variable.hh:150>)
- [variable.cc](</sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/variable.cc:324>)

`Cover` 会跟着 `HighVariable` 的成员变化而更新。
`HighIntersectTest` 还会缓存两两 intersection。

所以 cover 不是一遍算完就不动，它是会在 merge、split、type 推进后重新参与判断的。

可以把 `Cover` 先理解成一个 SSA varnode 的活跃范围：

```text
从这个 varnode 被定义的位置开始，
到所有使用它的位置结束，
中间经过哪些 basic block / p-code op。
```

Ghidra 里一个 `Varnode` 是 SSA 值，一个 `HighVariable` 是更高层的“源码变量”。
一个源码变量通常会有多个 SSA 实例。
比如：

```text
x0 = 1
use x0
x1 = 2
use x1
```

`x0` 和 `x1` 的活跃范围不重叠。
这时把它们合成同一个 HighVariable 是合理的：

```c
x = 1;
use(x);
x = 2;
use(x);
```

如果不合并，反编译结果可能变成两个没必要的临时变量：

```c
x_1 = 1;
use(x_1);
x_2 = 2;
use(x_2);
```

但如果两个 SSA 值的活跃范围重叠，就不能直接合并。
例如：

```text
a0 = load A
b0 = load B
use a0
use b0
```

在 `use a0` 到 `use b0` 之间，`a0` 和 `b0` 都还活着。
如果强行把它们合成一个 HighVariable，就等价于说同一个源码变量在同一个程序点同时有两个不同值。
这会造成错误：

```c
v = load_A();
v = load_B();
use(v);   // 本来应该用 A，现在可能被改成 B
use(v);
```

所以 Cover 的核心问题是：

```text
如果把这两个 varnode 当成同一个变量，会不会在某个程序点需要同时保存两个不同值？
```

如果会，就不能合并。
如果不会，就可以合并，反编译结果更像人写的代码。

控制流上也一样。
比如两个分支各自定义一个值，然后在合流点用 PHI：

```text
if cond:
  x1 = 1
else:
  x2 = 2
x3 = PHI(x1, x2)
use x3
```

`x1` 和 `x2` 分别只活在各自分支。
它们的 cover 不会在同一条实际路径上冲突，所以可以属于同一个 HighVariable。
这就是源码里的：

```c
if (cond)
  x = 1;
else
  x = 2;
use(x);
```

但下面这种不行：

```text
x1 = 1
x2 = 2
use x1
use x2
```

这两个值在同一条直线路径上同时活着。
把它们合并会丢掉一个值。

Ghidra 做 Cover 检查，就是为了在这两类情况中间划线：

- 不重叠：可以合并，变量更自然。
- 重叠：不能合并，否则语义错。
- 部分重叠或 piece/subpiece 重叠：需要更细的 `VariablePiece` / `HighIntersectTest` 判断。

这和寄存器消除也有关。
寄存器经常被复用：

```text
RAX = value_for_call
call foo(RAX)
RAX = return_value
use RAX
```

这两个 `RAX` 不是同一个源码变量。
第一个是参数准备值，第二个是返回值。
它们 storage 一样，但 cover 和定义来源不同。
如果只按寄存器名合并，就会把参数和返回值混成一个变量。

所以 Ghidra 不能只看 storage 名字，也不能只看类型名字。
它必须看 SSA def-use 和 cover，判断这些值能不能真的作为同一个 HighVariable。

## 6. symbol 和 use 是结果层，不是起点

`symbol` 是把 SSA / HighVariable 和“函数里有名字的东西”连起来。

`use` 则是在确认：

- 这个值是不是还真被当成参数。
- 这个值是不是只在一个语义位置出现。
- 这个值是不是被别的地方消耗了。

这两者都不是 heritage 之前的输入条件。
它们是 heritage 之后逐步收口的输出。

相关位置：

- [coreaction.cc](</sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc:2978>) `ActionNameVars::apply()`
- [funcdata_varnode.cc](</sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/funcdata_varnode.cc:311>) `Funcdata::findHigh()`

## 7. type recovery 也是反馈回路

type recovery 不是独立于 trial/use 的。

它会利用已经判断出来的参数/返回值位置、HighVariable、Symbol、Cover。
反过来，类型锁定又会影响后续的 use 判断和 merge 结果。

所以这里不是：

```text
SSA -> trial -> High -> type -> symbol
```

更像是：

```text
SSA -> trial/use -> High/Cover -> type/symbol -> 再修正 trial/use 结果
```

这就是为什么 Ghidra 不是一次直线流程。

## 8. 这些机制到底是顺序阶段，还是 heritage 内按需调用

答案是：

- 主骨架是顺序的。
- 具体判断是按需调用的。

更直白一点：

1. heritage 先把值关系建出来。
2. trial/use 在这个基础上按需追祖先、追后继、看 double use。
3. HighVariable / Cover / type / symbol 再对这些结果做收口。
4. 如果后面的结果变了，前面的判断也可能要重算。

所以 Ghidra 不是“heritage 一个函数里把所有事做完”。
它是“heritage 提供骨架，后面的分析按需走骨架，并把结果反过来修正骨架上的语义”。

## 9. 对第二阶段 native 的直接启发

对我们现在的第二阶段，最该学的不是某个单独函数，而是这个分层：

- 先把 call effect 显式放进 SSA。
- 再做 call input / output trial。
- 再做 High / cover / type / symbol 的收口。
- 不要把“暂时找不到值”写成最终语义。

最关键的一点是：

- `heritage` 负责把值流接起来。
- `trial/use` 负责判断这个值能不能拿去当参数。
- `HighVariable/Cover/type/symbol` 负责把值收成更高层的函数语义。

这三个层次不要混。
