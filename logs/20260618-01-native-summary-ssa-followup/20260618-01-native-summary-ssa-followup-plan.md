# Native summary SSA follow-up plan

用户原始要求：

> 把这几个点规划一下

## 当前状态

当前 native 新链路已经有三块基础能力：

1. `NativeRegisterSummary`：
   - 做 bottom-up register effect summary。
   - 做 top-down demanded return 分析。

2. `NativeRegisterSummarySSA`：
   - 基于 summary 做新的 register SSA。
   - 替换部分 register load。
   - 标记 external call 的 ABI 参数 store。
   - 做很保守的局部 register residue 删除。

3. `NativeExternalCallSignatureRewrite`：
   - 消费 SummarySSA 的参数标记。
   - 把 external direct call / tail call 的寄存器传参改成 LLVM call operand。
   - 重建 external declaration。
   - 删除已经被消费的参数 store。

最近验证过的真实点是 `/usr/bin/wrk` 的 `0x8300`，`free` 已经能从：

```llvm
store i64 %0, ptr @RDI
tail call void @free()
```

变成：

```llvm
tail call void @free(i64 %0)
```

并且通过 LLVM 22 的 `llvm-as` / `opt -passes=verify`。

后续重点不是继续模仿 Ghidra trial/use，而是把这条 SummarySSA 新链路在真实 Bench2 IR 上跑稳。

## 阶段 1：真实样例审计

目标：先知道当前新链路在真实 IR 上到底改得怎么样。

优先看这些目标：

- `wrk`
- `memcached`
- `libuv`
- `vsftpd`

每个目标至少记录：

- native lift 是否完成。
- SummarySSA 是否跑完。
- signature rewrite 改写了多少 external call。
- 删除了多少参数 store。
- 是否还有明显的 register residue。
- 是否通过 LLVM 22 `llvm-as` / `opt -passes=verify`。
- 同口径运行时间是否明显退化。

判断标准：

- 不只看 IR 能不能过 verifier。
- 要抽样看几处真实 external call，确认 call operand 和原寄存器 store 对得上。
- 如果出现 verifier 错误，先修语义问题，不用后置清理掩盖。

这一阶段的产出应该是一个 audit 表，而不是立刻加很多规则。

## 阶段 2：多参数 external call

当前机制已经按 ABI 参数寄存器顺序设计，但真实验证主要集中在单参数 `free`。
下一步要把多参数情况跑出来。

x86-64 SysV 第一版只看：

```text
RDI, RSI, RDX, RCX, R8, R9
```

规则保持简单：

- 只接受连续前缀参数。
- `RDI` 找不到，就不因为找到 `RSI` 而生成第二个参数。
- 同一个 external symbol 的可改 callsite 参数个数和类型必须一致。
- 冲突时跳过该 symbol，不做猜测。

需要补的测试：

- `call @foo` 前准备 `RDI/RSI`，改成 `@foo(i64, i64)`。
- `RDI/RSI/RDX` 三参数。
- 只找到 `RSI` 不改写。
- 同 symbol 两个 callsite 参数个数一致时一起改。
- 同 symbol 参数个数冲突时不改。

真实样例里优先找：

- `memcpy(dst, src, n)`
- `write(fd, buf, n)`
- `connect(fd, addr, len)`
- `setsockopt(...)`

判断标准：

- call operand 顺序必须和 ABI 一致。
- 被消费的 store 必须消失。
- 没有被证明只服务于当前 call 的 store 不能删。

## 阶段 3：跨 basic block 参数准备

现在 signature rewrite 主要依赖同 basic block 反向扫描。
这个规则安全，但会漏掉这类情况：

```llvm
bb1:
  store i64 %x, ptr @RDI
  br label %call

call:
  call void @foo()
```

短期不建议在 signature rewrite 里重新做一套数据流。
更合适的路线是让 SummarySSA 标清楚 call 参数来源：

```text
callsite C 的第 N 个 ABI 参数
  来自 register R
  当前 SSA value 是 V
  如果有唯一参数 store，则 store 是 S
```

signature rewrite 只消费这份标记：

- 有 value 就能改 call operand。
- 有唯一 store 且确认只服务于这个 call，才能删 store。
- 没有唯一 store 时，只改 call，不删 store。

这一阶段先不做跨 block store 删除。
原因是跨 CFG 删除需要支配关系和后续 use 检查，容易误删。

判断标准：

- 跨 block 参数能进入 call operand。
- 不因为跨 block 就冒险删除 store。
- metadata 足够支持后续调试。

## 阶段 4：清理临时 metadata

当前 IR 里可能保留：

```text
notdec.register.summary_ssa.call_args
notdec.register.summary_ssa.call_arg_store
notdec.register.summary_ssa.replaced
notdec.register.summary_ssa.phi
```

这些 metadata 分两类：

1. 需要给后续 pass 消费的：
   - `call_args`
   - `call_arg_store`

2. 只用于审计或调试的：
   - `replaced`
   - `phi`
   - `call_value`

第一版可以先保留全部 metadata，方便审计。
等 Bench2 稳定后，再加一个开关：

```text
--strip-native-summary-ssa-metadata
```

默认是否清理要看后续 pipeline：

- 如果后续 pass 还要消费，就不能默认删。
- 如果 signature rewrite 已经是最后一个消费者，可以在 rewrite 成功后清掉对应 call/store metadata。

判断标准：

- metadata 清理不能影响优化结果。
- 清理前后 verifier 都必须通过。
- 调试版本仍能保留 metadata。

## 阶段 5：返回值处理

这一阶段先不要急着改 external return signature。

原因：

- 参数改写只需要把 call 前 register value 搬到 operand，风险较低。
- 返回值改写需要处理 call 后 register load、helper value、可能的多返回候选。
- 当前 top-down demand 还需要更多真实样例确认。

建议顺序：

1. 先审计 `notdec.register.summary_return.*` helper 在真实 IR 里的使用。
2. 确认 demanded return 能减少误报的返回寄存器。
3. 再决定是否把 external call 的返回值改成真实 LLVM call result。

第一版只考虑 ABI 主返回寄存器，例如 x86-64 SysV 的 `RAX`。
`RDX:RAX` 这种双寄存器返回先不做。

判断标准：

- call 后读取 `RAX` 的位置能稳定替换成 call result。
- call 后不读取 `RAX` 时，不强行生成返回值。
- 不能把 dead changed register 当返回值。

## 阶段 6：internal function signature

internal function signature 改写比 external call 更晚做。

原因：

- internal function 需要同时改 callee function type、所有 callsite、entry register loads、return register stores。
- 还要考虑递归 SCC。
- 一旦改错，会影响函数体内部 SSA，不只是 callsite。

建议先不进入实现，只保留方向：

```text
bottom-up readEntry -> internal 参数候选
top-down exitDemand -> internal 返回值候选
SummarySSA value binding -> callsite operand
```

等 external call 参数和返回值都稳定后，再单独写 internal signature rewrite plan。

判断标准：

- 不在当前阶段改 internal function type。
- 不因为 internal signature 没改，就阻塞 external call 的优化。

## 阶段 7：当前优先级

推荐顺序：

1. Bench2 seed audit。
2. 多参数 external call 单测和真实样例。
3. SummarySSA 输出更完整的 call-arg binding。
4. 跨 basic block 参数进入 call operand，但先不跨 block 删除 store。
5. metadata 清理开关。
6. demanded return 审计。
7. external return rewrite。
8. internal function signature rewrite 另开计划。

短期最值得做的是前 3 点。
它们直接决定新链路能不能在真实项目里稳定替代旧 register residue 处理。

## 不做什么

这一轮不做：

- stack 参数。
- varargs 精确恢复。
- libc known prototype 特判。
- partial register 精细建模。
- 一般内存 alias。
- internal function signature rewrite。
- 把旧 Ghidra-style trial/use 链路重新引回来。

