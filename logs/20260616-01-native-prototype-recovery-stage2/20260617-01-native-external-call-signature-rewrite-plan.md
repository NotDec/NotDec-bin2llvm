# Native call signature rewrite plan

用户原始要求：

> 感觉既然之前都做register消除了，那是不是应该同步也做函数签名的修改。详细规划一下，是合并到前面的registerSSA，还是说怎么做？

## 背景

当前 native 新链路已经把寄存器 SSA / register residue 清理推进到 SummarySSA 这一层。
例如外部 tail call 现在可以生成：

```llvm
%RDI.entry = load i64, ptr @RDI
%0 = add i64 %RDI.entry, -8
store i64 %0, ptr @RDI
tail call void @free()
ret void
```

这里 `store @RDI` 不是普通残留。
它表示 x86-64 SysV ABI 下传给 `free` 的第一个参数。
SummarySSA 只知道 register 当前值和 call effect，不应该把这个 store 直接删掉。

下一步需要把这种 ABI register passing 改成 LLVM call operand：

```llvm
%0 = add i64 %RDI.entry, -8
tail call void @free(i64 %0)
ret void
```

这一步不是类型恢复。
第一版只把“寄存器传参”机械改成“LLVM call 参数”，参数类型先用 register 宽度对应的整数类型。

## 目标

新增一个独立 pass，第一版曾暂名：

```text
NativeExternalCallSignatureRewrite
```

它放在 SummarySSA 之后运行，负责处理 external direct call / external tail call 的寄存器参数。

第一版只做这些事：

- 识别 external callee。
- 按 ABI 参数寄存器顺序读取 call 前的 register store。
- 把这些值改写成 LLVM call 参数。
- 必要时替换 external function declaration 的函数类型。
- 删除已经被消费的参数 register store。

第一版明确不做：

- 不处理 stack 参数。
- 不处理返回值。
- 不处理 internal function signature。
- 不做真实 C 类型恢复。
- 不根据 printf 这类库函数知识做特殊签名。
- 不把这个逻辑合进 SummarySSA。

## 当前修订：重写成统一 call signature rewrite

当前 `NativeExternalCallSignatureRewrite` 已经能处理一批 external call，但设计上偏窄：

- 名字限定 external，但新链路真正需要同时处理 external call 和 internal `notdec_native_*` call。
- internal function signature rewrite 不应该回到旧 `NativePrototypeRecovery`。
- 继续把 internal 改写塞进现有 external-only 实现，会让 pass 职责变乱。

后续应当在 summary 链路里重写一个统一 pass，建议命名：

```text
NativeCallSignatureRewrite
```

代码位置：

```text
include/notdec-bin2llvm/passes/summary/
lib/passes/summary/
```

它消费 `NativeRegisterSummarySSA` 产出的信息，统一负责：

- external declaration call：
  - 把 ABI register 参数改成 LLVM call operand。
  - 必要时重建 declaration。
- internal direct call：
  - 把 callsite 的 ABI register 参数改成 LLVM call operand。
  - 重建 callee function type。
  - 用新 LLVM argument 替换 callee entry register load。
  - 删除确认只服务于当前 call 的参数 store。

第一版 internal 范围保持保守：

- 只处理 direct call。
- 只处理 `notdec_native_*` 这类当前 module 内定义的函数。
- 只处理 register 参数连续前缀。
- 所有 callsite 都能改，才改 callee。
- 遇到 address-taken、递归 SCC、indirect call、stack 参数、多返回，先跳过。
- 返回值 rewrite 可以作为第二步做，不和第一版参数 rewrite 混在一起。

这仍属于本 stage2 计划的一部分，不另开新链路。

## 为什么不合进 SummarySSA

SummarySSA 的职责是寄存器值分析和寄存器消除。
它回答的是：

```text
这个 register 的值来自哪里？
callee 会不会读它？
callee 会不会改它？
这个 register residue 能不能删？
```

函数签名改写回答的是另一个问题：

```text
LLVM call 指令应该带哪些 operand？
callee declaration 的 FunctionType 应该是什么？
```

这两个问题依赖关系很清楚：

```text
SummarySSA 先把 register 值整理清楚
  -> signature rewrite 再把 ABI register 参数搬到 call operand
  -> 后置 InstCombine / DCE 清掉无用 residue
```

如果把签名改写塞进 SummarySSA，pass 会同时负责数据流分析、IR value 替换、function type 替换。
后面调 bug 时很难判断是 summary 错了，还是 call 改写错了。
所以第一版保持成单独 pass。

## 技术路线

### 1. 运行位置

建议 pipeline：

```text
InstCombine
  -> NativeRegisterSummarySSA
  -> NativeCallSignatureRewrite
  -> InstCombine
```

`NativePrototypeRecovery` 属于 heritage 链路。summary 默认链路里不应继续依赖它做 internal signature rewrite。

### 2. 识别 external call

external 部分只处理 direct call：

```llvm
call void @free()
tail call void @free()
```

callee 必须是 declaration，或者能明确来自 PLT / external relocation 的符号。
旧 external-only 第一版不处理 internal function。
统一 rewrite pass 中，internal direct call 应作为同一个 pass 的另一类输入。

遇到 indirect call：

```llvm
call void %fp()
```

先跳过。

### 3. 参数来源

从 module ABI 信息拿寄存器参数顺序。
以 x86-64 SysV 为例：

```text
RDI, RSI, RDX, RCX, R8, R9
```

对每个 external call，在同一个 basic block 内从 call 往前扫描。
查找最近的完整 register store：

```llvm
store i64 %v, ptr @RDI
```

如果找到 `RDI`，它就是第一个 call 参数。
继续找 `RSI`，它就是第二个 call 参数。

第一版只接受连续前缀参数。
也就是找到 `RDI` 和 `RSI` 可以改成两个参数；
如果没找到 `RDI`，就不因为找到了 `RSI` 而生成第二个参数。

### 4. 扫描边界

旧 external-only 第一版只做同 basic block 反向扫描。
当前 SummarySSA 已经会在 callsite 上记录 ABI 参数 value，后续统一 pass 应直接消费 SummarySSA 的 value binding，不在 rewrite pass 里重新写跨 basic block 数据流。

扫描遇到这些情况停止：

- 另一个 non-intrinsic call。
- terminator。
- 无法理解的 register memory alias。

intrinsic call 可以跳过。
例如 flags 计算可能留下过 `llvm.ctpop`，这不是 ABI 调用。

这个策略会漏掉跨 basic block 准备参数的情况，但不会乱改。
后续如果需要，可以基于 dominator 或 SummarySSA 产出的 value binding 扩展。

### 5. callee 类型改写

LLVM 不能直接修改 `FunctionType`。
如果要把：

```llvm
declare void @free()
```

改成：

```llvm
declare void @free(i64)
```

需要新建函数声明，然后替换 call 的 callee。
实现前要参考 LLVM 自己的函数复制和函数类型改写代码，避免漏掉属性和 metadata。
重点源码：

- `/sn640/NotDec/llvm-source/llvm/lib/Transforms/Utils/CloneModule.cpp`
  - `CloneModule` 创建新 `Function` 后调用 `copyAttributesFrom`。
  - 对 function declaration 还会 `getAllMetadata` 后逐个复制 metadata。
- `/sn640/NotDec/llvm-source/llvm/lib/Transforms/Utils/CloneFunction.cpp`
  - `CloneFunctionAttributesInto` 先用 `copyAttributesFrom` 复制 global/function 级信息，再重建 `AttributeList`。
  - `CloneFunctionMetadataInto` 用 `getAllMetadata` / `MapMetadata` 复制 function metadata。
- `/sn640/NotDec/llvm-source/llvm/lib/Transforms/IPO/DeadArgumentElimination.cpp`
  - 改函数类型时新建 `Function`，复制 attributes、comdat，再重建所有 callsite。
  - 重建 callsite 时保留 calling convention、callsite attributes、operand bundles、tail call kind、`prof/dbg` metadata。
- `/sn640/NotDec/llvm-source/llvm/lib/Transforms/IPO/ArgumentPromotion.cpp`
  - 同样展示了新建函数、复制 metadata、重建 callsite、保留 tail call kind 的做法。

第一版采用保守规则：

- 同一个 external callee 的所有可改 callsite 参数个数必须一致。
- 参数类型全部用 register 宽度对应的 integer type。
- 返回类型保持原样。
- calling convention、attributes、linkage、name 需要保留。
- function declaration 的 metadata 需要完整保留。
- callsite 的 operand bundle、tail call kind、calling convention、attributes、`prof/dbg` metadata 需要保留。

如果同一个 external symbol 在不同 callsite 推出不同参数个数，先跳过这个 symbol。
这会放过 `printf` 这类变参函数，但第一版不靠库函数知识处理它们。

实现上不能只写一个裸的：

```cpp
Function::Create(NewTy, ExternalLinkage, Name, M)
```

然后替换 callee。
这很容易丢掉 DLL storage class、visibility、unnamed addr、section、partition、comdat、GC、prefix/prologue/personality、function attrs、return attrs、metadata 等信息。
即使当前 native IR 大概率没有完整 debug info，也应该按 LLVM 现有做法复制，避免后续接真实 bitcode 或更多 metadata 时出问题。

### 6. tail call

如果原 call 是：

```llvm
tail call void @free()
```

改写后仍保留 `tail`：

```llvm
tail call void @free(i64 %arg)
```

不要使用 `musttail`。
`musttail` 对 caller / callee signature 有额外限制，当前 IR 不需要它。

### 7. 删除参数 store

被成功搬进 call operand 的 register store 可以删除：

```llvm
store i64 %arg, ptr @RDI
tail call void @free(i64 %arg)
```

这里是分 pass 后最需要小心的地方。
如果 signature rewrite 和 SummarySSA 合在一起做，SummarySSA 在构造 register value 时天然知道：

```text
这个 call 读取 RDI
call 前 RDI 当前值来自哪个 store / phi / entry load
这个 store 后面是否还会被别的 register use 读到
```

分成独立 pass 后，signature rewrite 只能看到改写后的 IR。
当前 SummarySSA 公开出来的信息还不够完整：

- `notdec.register.summary_ssa.entry` 标记 entry load。
- `notdec.register.summary_ssa.replaced` 标记被替换过的 load。
- `notdec.register.summary_ssa.phi` 标记新建 phi。
- `notdec.register.summary_ssa.call_value` 标记 call return/clobber helper value。
- `notdec.register.summary_ssa.call_args` / `notdec.register.summary_ssa.call_arg_store`
  标记 external call 参数和对应 store。
- 函数级 `notdec.register.summary_ssa` 只记录统计信息。

这些信息没有直接表达：

```text
callsite C 的 ABI 参数寄存器 RDI 消费了 store S
store S 是否只服务于 callsite C
```

所以第一版实现时可以先保守，但方向不能保守到放弃优化。
signature rewrite 应该尽量知道 store 的完整用途。
前面的 pass 也要尽量把“这个 store 最终只是 ABI 参数准备”这件事标出来。
可接受的最小规则是：

```text
只删除本 pass 在同一个 basic block 内反向扫描直接选中的 store；
store 到 call 之间不能出现任何可能读取该 register global 的指令；
store 到 call 之间不能出现另一个 analyzable call；
store 到 call 之间不能出现可能 alias register global 的未知内存操作；
call 改写成功后再删除这些被消费的 store。
```

如果不能满足这些条件，只改写 call operand，不删除 store。
这会留下少量 residue，但比误删安全。

如果 store 后到 call 前又出现同 register 的覆盖，扫描本来就应该取最近的一次。
较早的 store 留给后置 InstCombine / DCE 或后续 pass 处理。

后续如果这个 pass 需要跨 block 或更激进地删除 store，应该让 SummarySSA 显式产出 call-arg binding metadata，而不是在 signature rewrite 里重新猜。
可以考虑给 call 加类似：

```text
notdec.register.summary_ssa.call_arg = { register=RDI, value/store id=... }
```

但第一版先不引入这个机制，避免把两个 pass 重新耦合得太深。

更准确地说，pass 边界只是实现分层，不是优化边界。
最终目标还是：

```text
尽可能把 ABI register 上的准备 store
  -> 变成 call operand
  -> 再删掉原 store
```

所以如果后面发现某类 store 其实能安全删，只要前面的 pass 能稳定标出来，就应该补上，不必因为它现在落在 signature rewrite 之后就放弃。

## 实现记录

已完成这条独立链路的最小实现：

- `include/notdec-bin2llvm/passes/summary/NativeCallSignatureRewrite.h`
- `lib/passes/summary/NativeCallSignatureRewrite.cpp`
- `include/notdec-bin2llvm/passes/summary/NativeRegisterSummarySSA.h`
- `lib/passes/summary/NativeRegisterSummarySSA.cpp`
- `tools/notdec-native-llvm.cpp`
- `lib/CMakeLists.txt`
- `CMakeLists.txt`
- `tests/native_register_summary_ssa_test.cpp`
- `tests/native_external_call_signature_rewrite_test.cpp`

现在的效果是：

- SummarySSA 会给 external ABI 参数 call 标出 `notdec.register.summary_ssa.call_args`。
- 对应参数准备 store 会标成 `notdec.register.summary_ssa.call_arg_store`。
- `NativeCallSignatureRewrite` 会把它们改成 LLVM call operand。
- external declaration 会改成带参数签名。
- 已消费的 register store 会删掉。

真实样例 `/usr/bin/wrk` 的 `0x8300` 入口已经验证过：

```llvm
tail call void @free(i64 %0)
```

`store @RDI` 不再保留。
生成的 IR 还能通过 `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as` 和 `opt -passes=verify`。

### 8. 和 SummarySSA 的关系

SummarySSA 继续负责这些：

- internal direct call 的 register effect。
- external / indirect call 的 ABI fallback。
- 删除普通 register residue。
- 处理 PLT tail branch lower 成 external tail call 之后的寄存器状态。

Signature rewrite 不重新做 SCC summary。
它只消费 SummarySSA 整理后的 IR。

当前重点用例是：

```llvm
store i64 %x, ptr @RDI
tail call void @free()
```

改成：

```llvm
tail call void @free(i64 %x)
```

这能把外部函数调用从“全局寄存器模拟”推进到更正常的 LLVM IR。

## 风险

1. 参数个数误判。

   同 basic block 反向扫描只能看到局部准备动作。
   跨 block 准备参数会漏掉。
   第一版宁可漏掉，不跨 CFG 猜。

2. external symbol 多签名冲突。

   同一个 declaration 在不同 callsite 可能推到不同参数个数。
   第一版遇到冲突就跳过该 symbol。

3. 变参函数。

   `printf`、`fprintf` 这类函数可能天然有不同参数个数。
   第一版不做 varargs 推断。
   之后可以用 dynamic symbol 名字和 ABI 规则单独处理。

4. 参数类型粗糙。

   第一版用 `i64` 这类整数类型，不判断 pointer。
   这不是类型恢复，只是把寄存器传参转成 call operand。

5. store 删除过早。

   如果某个 store 同时被后续别的逻辑读取，删掉会错。
   所以第一版只删本 pass 能证明被当前 call 消费的 store。

## 判断标准

最小验收用例：

```llvm
store i64 %x, ptr @RDI
tail call void @free()
ret void
```

改写后：

```llvm
tail call void @free(i64 %x)
ret void
```

并且没有残留 `store @RDI`。

还需要覆盖：

- 普通 external call。
- external tail call。
- 同一个 external callee 多个 callsite 参数个数一致。
- 同一个 external callee 参数个数冲突时跳过。
- intrinsic call 不参与参数判断。

验证时至少检查：

- 新增单元测试。
- `notdec-native-llvm` 对当前 `wrk -f 0x8300` 的 IR 效果。
- 使用 `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as` 和 `/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify` 验证生成 IR。
- 对 Bench2 当前关注目标跑同口径 seed audit，记录行数和耗时，确认没有明显性能退化。

本次验证结果：

- `tests/native_register_summary_ssa_test.cpp` 通过，确认 SummarySSA 会标注 external call 参数 store。
- `tests/native_external_call_signature_rewrite_test.cpp` 通过，确认 signature rewrite 会重建 declaration 并删掉已消费 store。
- `/sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk` 的 `0x8300` 入口已验证：
  `tail call void @free(i64 %0)` 出现，`store @RDI` 消失，LLVM 22 `llvm-as` / `opt -passes=verify` 通过。
