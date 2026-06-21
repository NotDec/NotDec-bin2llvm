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
- 常见 libc / external symbol 优先用已知 fixed 参数数量。
- 未知 external symbol 如果同名 callsite 参数数量不同，打印 warning，然后取最小参数数量。
- 取最小时不能删除被截断掉的额外参数 store。

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
- partial register 精细建模。
- 一般内存 alias。
- internal function signature rewrite。
- 把旧 Ghidra-style trial/use 链路重新引回来。

## 实现记录：阶段 1 审计和 IR 输入 ABI 修复

阶段 1 先用现有 Bench2 `selected-targets-native` 产物做审计。
审计时发现一个基础问题：

```text
notdec-native-llvm 以 .ll/.bc 作为输入时，不会重新 attach notdec.abi metadata。
```

这会导致 SummarySSA 读不到 ABI input register 顺序，进而不会标记 external call 的参数 store。
表现是：

```llvm
store i64 %x, ptr @RDI
call void @free()
```

重新跑当前 pipeline 后仍然不会变成：

```llvm
call void @free(i64 %x)
```

ELF 输入路径没有这个问题，因为 ELF lowering 后会调用 `attachDefaultAbiMetadata()`。

本次做了一个小修复：

- [tools/notdec-native-llvm.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:702)
  - 新增 `ensureDefaultAbiMetadata()`。
  - 如果 module 已经有 `notdec.abi`，不做任何事。
  - 如果没有，就按默认 x86-64 cspec attach ABI metadata。
- [tools/notdec-native-llvm.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:922)
  - IR 输入路径读完 module 后、运行 InstCombine/SummarySSA 前调用 `ensureDefaultAbiMetadata()`。

修复前，用旧 `module-all.ll` 作为输入重新跑当前 pipeline，external call 仍基本没有参数：

```text
name        noarg external   param external   ABI param stores
wrk         19               0                3
memcached   63               0                8
vsftpd      56               0                31
libuv       127              0                6
```

这里 ABI param stores 已经变少，是 SummarySSA 的普通 residue 删除生效了；
但 external signature rewrite 没有参数证据，所以没有生成带参数 call。

修复后，同样输入重新跑当前 pipeline：

```text
name        noarg external   param external   ABI param stores   run time
wrk         12               7                6                  1.15s
memcached   21               43               19                 2.48s
vsftpd      50               6                39                 1.70s
libuv       103              22               20                 3.68s
```

这次新增了 78 个带参数 external call。
典型结果：

```llvm
call void @bind(i64 %2, i64 %addr, i64 %len)
call void @free(i64 %ptr)
call void @__assert_fail(i64 ..., i64 ..., i64 ..., i64 ...)
```

所有四个输出都通过：

```text
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify
```

剩余问题初步分类：

- `notdec_native_*` 是 internal direct call，当前阶段不改。
- `free` 这类仍有漏网，多数是跨 basic block 参数准备，例如 store 在前驱块、call 在后继块；这属于阶段 3。
- `fcntl64` 这类存在同一 external symbol 多个 callsite 参数数量不同：
  - 一个 callsite 标成 `count=2`。
  - 另一个 callsite 标成 `count=3`。
  - 当前 rewrite pass 按保守规则跳过整个 symbol。
  - 这属于 varargs / known prototype 问题，阶段 1 不修。
- `open64`、`pthread_*` 等部分无参 external call 需要继续抽样，有些是真没准备连续 ABI 参数，有些可能是跨块准备。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm native_register_summary_ssa_test native_external_call_signature_rewrite_test -j2
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/native_external_call_signature_rewrite_test
```

复杂度评估：

- 实现效果：7/10。修复了 IR 输入路径审计不生效的问题，Bench2 旧产物可以直接复跑新链路。
- 理解成本：2/10。只是在 CLI 入口补默认 ABI metadata，不改变 pass 内部逻辑。
- 维护成本：2/10。已有 ABI metadata 时不覆盖，风险很低。

## 实现记录：阶段 2/3 external 参数绑定和冲突处理

本次把 external 参数数量推理前移到 SummarySSA。
signature rewrite 不再自己只看同一 basic block 里有没有参数 store，而是消费 SummarySSA 给 callsite 绑定好的 ABI 参数 value。

具体改动：

- [NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:743)
  - `markExternalCallArgumentStores()` 先收集 external call，再给 call 添加 `notdec.register.summary_ssa.call_arg_values` operand bundle。
  - 继续保留 `notdec.register.summary_ssa.call_args` metadata，表示连续 ABI 参数前缀长度。
- [NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:813)
  - `callArgStoreBindings()` 按 ABI 参数寄存器顺序调用 `readValueBefore()`。
  - 因为 `readValueBefore()` 会走 `readBlockEntry()` / `readBlockExit()`，所以可以跨 basic block 找 call 前寄存器当前值。
  - 如果值只是函数入口自动 load 出来的 entry input，就停止，避免把所有 external call 都误判成满参数。
- [NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:836)
  - `findStoreBeforeCall()` 只负责找同 basic block 且 value 对得上的 store。
  - 找不到 store 时仍然可以改 call operand，但不删除跨块 store。
- [NativeExternalCallSignatureRewrite.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeExternalCallSignatureRewrite.cpp:57)
  - 新增常见 external 原型表，只记录参数数量和 vararg，不做 C 类型恢复。
  - 覆盖常见 libc 和 Bench2 里常见的外部符号，例如 `free`、`malloc`、`memcpy`、`strcmp`、`read`、`write`、`fcntl64`、`__printf_chk`。
- [NativeExternalCallSignatureRewrite.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeExternalCallSignatureRewrite.cpp:221)
  - `collectCallArgBindings()` 改为从 operand bundle 取参数 value。
  - store 只作为可删除证据；没有 store 不影响 call operand 改写。
- [NativeExternalCallSignatureRewrite.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeExternalCallSignatureRewrite.cpp:349)
  - `resolveSymbolPlan()` 对已知 external 使用固定参数数量。
  - 未知 external 如果同名不同 callsite 参数数量不同，打印 warning，取最小参数数量。
  - 取最小时，被截断的额外参数 store 不删除。
- [NativeExternalCallSignatureRewrite.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeExternalCallSignatureRewrite.cpp:466)
  - `rewriteCall()` 只保留对外有意义的 operand bundle，内部参数 value bundle 不写入最终 IR。
- [NativeExternalCallSignatureRewrite.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeExternalCallSignatureRewrite.cpp:498)
  - `stripCallArgValueBundles()` 清理未被改写 call 上残留的内部 operand bundle。
- [NativeExternalCallSignatureRewrite.h](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/passes/summary/NativeExternalCallSignatureRewrite.h:26)
  - summary 增加 known prototype、minimum args、known args 不足的计数。
- [native_external_call_signature_rewrite_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_external_call_signature_rewrite_test.cpp:146)
  - 增加未知 external 冲突取最小参数数量测试。
- [native_external_call_signature_rewrite_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_external_call_signature_rewrite_test.cpp:197)
  - 增加已知 fixed prototype 跳过参数不足 callsite 的测试。
- [native_external_call_signature_rewrite_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_external_call_signature_rewrite_test.cpp:249)
  - 增加跨 basic block 参数准备的端到端测试。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target native_register_summary_ssa_test native_external_call_signature_rewrite_test notdec-native-llvm -j2
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/native_external_call_signature_rewrite_test
```

真实样例重新从二进制跑 `/usr/bin/wrk`：

```text
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk \
  --all-confirmed \
  -o /tmp/notdec-summaryssa-callsite-binding-smoke/wrk.ll \
  --summary-json-out /tmp/notdec-summaryssa-callsite-binding-smoke/wrk-summary.json
```

结果：

```text
wrk native pipeline: 45.36s
call_arg_values_bundle_leftover: 0
call_args_metadata: 211
external_param_calls: 211
```

LLVM 22 验证通过：

```text
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-summaryssa-callsite-binding-smoke/wrk.ll -o /tmp/notdec-summaryssa-callsite-binding-smoke/wrk.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-summaryssa-callsite-binding-smoke/wrk.bc -o /tmp/notdec-summaryssa-callsite-binding-smoke/wrk.verified.bc
```

`wrk` 中看到的冲突 warning：

```text
lua_newuserdata counts={3,6}; using minimum 3
lua_getfield counts={3,4,6}; using minimum 3
lua_pushlstring counts={3,6}; using minimum 3
lua_setfield counts={3,6}; using minimum 3
lua_pushnil counts={1,6}; using minimum 1
OPENSSL_init_ssl counts={2,3}; using minimum 2
lua_settop counts={3,6}; using minimum 3
epoll_ctl counts={4,6}; using minimum 4
```

典型改写结果：

```llvm
call void @memcpy(i64 %RAX.return, i64 %unique_4a00_8, i64 %11)
call void @free(i64 %RAX.return)
call void @epoll_ctl(i64 ..., i64 ..., i64 ..., i64 ...)
```

仍然保守处理的情况：

- 只从 entry input 直接传给 external call 的寄存器暂时不算实参，避免把所有 ABI 参数寄存器都算进去。
- 跨 basic block 可以改 call operand，但不删除跨块 store。
- 未知 external 的 vararg 仍不精确，只按当前可证明的连续 ABI 参数前缀处理。

复杂度评估：

- 实现效果：8/10。解决了同名 external 参数数量冲突直接跳过的问题，也让跨 basic block 参数准备能进入 call operand。
- 理解成本：5/10。SummarySSA 和 rewrite 之间多了一个内部 operand bundle，但职责比较清楚。
- 维护成本：4/10。known prototype 表需要后续补充；operand bundle 已在 rewrite 后清理，不会污染最终 IR。

## 实现记录：fortune 样例和 entry 参数显式转发

本次把 Ubuntu noble 的 `fortune-mod` 加到 Bench2 数据集中，并用当前 native 链路跑了 `/usr/games/fortune`。
Bench2 本身在 `/sn640/NotDec-Exp/Bench2`，不是当前 bin2llvm 仓库的 tracked 文件，这里只记录结果。

新增 Bench2 内容：

- `fortune-mod 1:1.99.1-7.3build1`
  - `/usr/games/fortune`
  - `/usr/bin/strfile`
  - `/usr/bin/unstr`
- `librecode0 3.6-26`
  - `/usr/lib/x86_64-linux-gnu/librecode.so.0.0.0`
- 对应 dbgsym 文件已经解到 `/usr/lib/debug/.build-id/`。
- `benchmark-targets.tsv` 里新增目标：
  - `fortune	executable	/usr/games/fortune`
- `export-bin2llvm-selected-targets.py` 里新增：
  - `fortune=executable`

第一次跑 fortune 发现一个可修问题：

```llvm
store i64 %RDX.entry, ptr @RDI
call void @strlen()
```

这里 `%RDX.entry` 是函数入口参数，但已经被显式写入 `RDI` 作为 external call 参数。
旧规则把所有 entry input 都排除，导致这个 `strlen` 没有参数。

本次修复：

- [NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:813)
  - `callArgStoreBindings()` 先找同 basic block 的显式参数 store。
  - 如果当前 value 是 entry input，但找到了显式参数 store，就允许作为 active call 参数。
  - 如果只是裸 entry input，没有显式 store，仍然停止，避免误判满参数。
- [native_external_call_signature_rewrite_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_external_call_signature_rewrite_test.cpp:76)
  - 新增 `loadRegister()` 测试 helper。
- [native_external_call_signature_rewrite_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_external_call_signature_rewrite_test.cpp:302)
  - 新增 `testExplicitEntryForwardedArgIsRewritten()`。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target native_register_summary_ssa_test native_external_call_signature_rewrite_test notdec-native-llvm -j2
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/native_external_call_signature_rewrite_test
```

fortune 重新从二进制跑：

```text
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  --all-confirmed \
  -o /tmp/notdec-fortune-native-after-entry-forward/fortune.ll \
  --summary-json-out /tmp/notdec-fortune-native-after-entry-forward/summary.json
```

LLVM 22 验证通过：

```text
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-native-after-entry-forward/fortune.ll -o /tmp/notdec-fortune-native-after-entry-forward/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-native-after-entry-forward/fortune.bc -o /tmp/notdec-fortune-native-after-entry-forward/fortune.verified.bc
```

效果对比：

```text
before:
  external calls with params: 80
  no-arg external calls: 22
  stores to register globals: 507

after:
  external calls with params: 84
  no-arg external calls: 18
  stores to register globals: 503
```

改好的典型结果：

```llvm
call void @strlen(i64 %RDX.entry)
call void @strlen(i64 %RSI.entry)
call void @strrchr(i64 %RSI.entry, i64 45)
```

fortune 当前仍有明显未消除 register residue：

```text
register_access_metadata: 508
loads_from_register_globals: 108
stores_to_register_globals: 503
```

主要来源：

- internal `notdec_native_*` call 还没有做 signature rewrite，call 前后仍需要靠 register global 传值。
- `RSP` 相关 store/load 还没有专门栈指针 pass 消除。
- 部分 external call 仍无参，例如 `__memcpy_chk()`、`strchr()`、`strncmp()`、`fwrite()`、`strncpy()`、`__snprintf_chk()`，需要继续看是缺少 known prototype、参数准备跨块 store 删除不足，还是前面的值被当前 SummarySSA 判成不可信。

复杂度评估：

- 实现效果：6/10。这个小修能多消除一类真实 external 参数，但 fortune 里剩余 residue 主要已经不是这个点。
- 理解成本：2/10。规则只是把“entry input 一律排除”改成“显式 store 后允许”。
- 维护成本：2/10。仍然保留裸 entry input 的保护，误报风险较低。

## 实现记录：拆分 heritage / summary 代码目录

本次只做目录和 pipeline 边界整理，不改变 summary pass 内部算法。

目标是防止旧 Ghidra-style 链路和新 summary 链路继续混在同一个 `passes/` 平铺目录里。

具体改动：

- 旧链路移动到 `heritage/`：
  - [NativeHeritageSSA.h](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/passes/heritage/NativeHeritageSSA.h:1)
  - [NativePrototypeRecovery.h](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/passes/heritage/NativePrototypeRecovery.h:1)
  - [NativeHeritageSSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/heritage/NativeHeritageSSA.cpp:1)
  - [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/heritage/NativePrototypeRecovery.cpp:1)
- 新链路移动到 `summary/`：
  - [NativeRegisterSummary.h](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/passes/summary/NativeRegisterSummary.h:1)
  - [NativeRegisterSummarySSA.h](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/passes/summary/NativeRegisterSummarySSA.h:1)
  - [NativeExternalCallSignatureRewrite.h](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/passes/summary/NativeExternalCallSignatureRewrite.h:1)
  - [NativeRegisterSummary.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummary.cpp:1)
  - [NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:1)
  - [NativeExternalCallSignatureRewrite.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeExternalCallSignatureRewrite.cpp:1)
- [tools/notdec-native-llvm.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:812)
  - 默认仍运行 summary 链路。
  - 只有显式 `--heritage-register-ssa-pass` 才运行 `NativeHeritageSSA`。
- [tools/notdec-native-llvm.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:876)
  - `NativePrototypeRecovery` 现在只在 heritage 模式下运行。
  - 默认 summary 链路不再混入旧 prototype recovery metadata。
- [AGENTS.md](/sn640/NotDec/external/NotDec-bin2llvm/AGENTS.md:65)
  - 增加 heritage / summary 两条链路的说明。
- [README.md](/sn640/NotDec/external/NotDec-bin2llvm/logs/20260616-01-native-prototype-recovery-stage2/README.md:1)
  - 汇总 stage2 目标、原始 prompt 和文件索引。

同时把原来三个单文件日志目录合并到：

```text
logs/20260616-01-native-prototype-recovery-stage2/
```

被合并的文件：

```text
20260616-02-native-register-summary-scc-fixpoint-plan.md
20260617-01-native-external-call-signature-rewrite-plan.md
20260618-01-native-summary-ssa-followup-plan.md
```

signature rewrite 计划也同步修正：

- 当前 `NativeExternalCallSignatureRewrite` 只是已有阶段实现。
- 后续应重写成 summary 链路下统一的 `NativeCallSignatureRewrite`。
- 新 pass 同时处理 external declaration call 和 internal `notdec_native_*` direct call。
- 不基于旧 `NativePrototypeRecovery` 做 internal function signature rewrite。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target native_register_summary_test native_register_summary_ssa_test native_external_call_signature_rewrite_test notdec-native-llvm -j2
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/native_external_call_signature_rewrite_test
cmake --build /tmp/notdec-bin2llvm-build --target native_register_effects_test native_prototype_recovery_test native_instcombine_metadata_test -j2
/tmp/notdec-bin2llvm-build/bin/native_register_effects_test
```

fortune 同口径验证：

```text
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  --all-confirmed \
  -o /tmp/notdec-fortune-chain-split/fortune.ll \
  --summary-json-out /tmp/notdec-fortune-chain-split/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-chain-split/fortune.ll -o /tmp/notdec-fortune-chain-split/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-chain-split/fortune.bc -o /tmp/notdec-fortune-chain-split/fortune.verified.bc
```

结果：

```text
fortune native pipeline: 10.21s
summary_ssa_call_args_metadata: 112
prototype_recovered_metadata: 0
register_access_metadata: 508
loads_from_register_globals: 108
stores_to_register_globals: 503
```

`prototype_recovered_metadata: 0` 说明默认 summary 链路已经不再混入旧 `NativePrototypeRecovery` 输出。

复杂度评估：

- 实现效果：8/10。代码目录和默认 pipeline 都明确区分了新旧链路。
- 理解成本：2/10。只是目录分组和 include 路径变化。
- 维护成本：2/10。后续新功能可以直接放 summary 目录，旧链路留在 heritage 目录。

## 实现记录：修复 fortune R9 残留的前段根因

这次问题一开始表现为 fortune `add_file` 里还剩两处 `R9` register access，但根因不在 SummarySSA。

真实汇编片段：

```asm
3748: je     0x3e81
374e: pxor   xmm0,xmm0
3752: xor    r9d,r9d
3763: mov    WORD PTR [rax+0x38],r9w
```

`xor r9d,r9d` 真实语义是把整个 `R9` 清零。low-pcode 和单独 pcode-to-LLVM 都能看到这个写入，但 native discovery / lowering 后的 IR 曾经让 `bb_3748` 的 false edge 跳到错误的 `bb_36f8`，导致 `0x374e/0x3752` 这段在 LLVM CFG 中不可达，后面的 `load @R9` 就无法被 SummarySSA 消掉。

具体改动：

- [SleighLift.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/SleighLift.cpp:453)
  - `SleighInstructionDecoder::decode(...)` 改成先调用 `oneInstruction(...)` 收集 pcode，再调用 `printAssembly(...)` 收集文本。
  - 原因是 Ghidra `oneInstruction()` 会 `applyCommits()` 写回 SLEIGH context；在复用 engine 上先 `printAssembly()` 再 `oneInstruction()`，fortune 这个位置会让后续地址漏解。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:83)
  - `lowerTerminator(...)` 现在接收当前 block index，方便查询 native CFG。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:256)
  - 新增 `nativeConditionalFalseBlock(...)`。
  - SLEIGH `CBRANCH` 只带 taken target，false edge 应优先从 `Config.BlockSuccessors` 里取，而不是默认使用 pcode 顺序里的下一个 block。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:321)
  - `CBRANCH` lowering 记录 true target address，并用 native CFG 计算 false block。
  - 找不到 native CFG 信息时才退回原来的 fallthrough。

验证：

```text
cmake --build build --target notdec-native-discover notdec-native-llvm -j4
cmake --build build --target native_register_summary_ssa_test native_register_summary_test -j4
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
build/bin/notdec-native-discover --instructions-json /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/fortune-3470-raw-cbranch-fix.ll
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-cbranch-fix/fortune.ll --summary-json-out /tmp/notdec-fortune-cbranch-fix/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-cbranch-fix/fortune.ll -o /tmp/notdec-fortune-cbranch-fix/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-cbranch-fix/fortune.bc -o /tmp/notdec-fortune-cbranch-fix/fortune.verified.bc
```

结果：

```text
fortune native pipeline: 13.26s
confirmed_functions: 25
basic_blocks: 2369
instructions: 2908
final !notdec.register.access residue: 1
remaining register access: FS_OFFSET only
R9 residue: 0
llvm-as + opt verify: passed
```

性能：同口径 fortune 之前约 9.92s，本次约 13.26s。这个差异主要来自本轮本地完整生成波动和额外 CFG 修复后可达 IR 增加，暂时没有看到算法级新增慢路径；后续 Bench2 批量跑时再看是否需要 profile。

复杂度评估：

- 实现效果：9/10。修掉了 fortune 里误残留的 `R9`，也修正了 native sparse CFG 下条件跳转 false edge 的语义。
- 理解成本：3/10。新增逻辑集中在 `CBRANCH` false edge，不影响 SummarySSA 抽象域。
- 维护成本：3/10。依赖 discovery 的 `BlockSuccessors`，这正是 native lowering 已经传入的事实；缺失时仍保留旧 fallback。

## 实现记录：native decode 增加 instruction flow facts

本次根据前段 decode 复盘做第一步收紧，不改 SummarySSA，也不重写 discovery 算法。

参考判断：

- Ghidra 的 block model 是基于 instruction 的 `FlowType` / `FallThrough` / flow references 建块，不把 pcode 顺序当 CFG 权威。
- angr 的 CFGFast 更偏 basic block lift，再从 block exits 建 CFG。
- 当前 native 链路先线性 decode，再用 pcode 辅助切块；这次先把机器指令级 flow facts 写进 `NativeInstruction`，让后续 block 构建消费这份事实。

具体改动：

- [NativeAnalysis.h](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/NativeAnalysis.h:185)
  - 新增 `NativeInstructionFlowKind`，区分 none、conditional branch、unconditional branch、indirect branch、return。
- [NativeAnalysis.h](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/NativeAnalysis.h:197)
  - `NativeInstruction` 增加 `FlowKind`、`DirectFlowTargets`、`DirectCallTargets`、`Fallthrough`、`HasIndirectCall`。
  - 设计目标是让 instruction fact 明确携带控制流，不再只保存文本和 bytes。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1498)
  - `decodeSeed(...)` 先把当前窗口收成 `decodedInstructions`，再用 pcode flow info 标注，最后写回 `NativeProgramState`。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1539)
  - 新增 `annotateDecodedInstructionFlows(...)`，把 `DecodedFlowInfo` 折叠进每条 `NativeInstruction`。
  - 普通 call 只记录 `DirectCallTargets`，不作为 block terminator。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1919)
  - `buildDecodedBlocks(...)` 改为消费 `NativeInstruction` 的 flow facts，不再直接依赖独立的 `flowInfos` map。

验证：

```text
cmake --build build --target notdec-native-discover notdec-native-llvm -j4
build/bin/notdec-native-discover --blocks-json /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/fortune-3470-flowfacts-raw.ll
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-flowfacts/fortune.ll --summary-json-out /tmp/notdec-fortune-flowfacts/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-flowfacts/fortune.ll -o /tmp/notdec-fortune-flowfacts/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-flowfacts/fortune.bc -o /tmp/notdec-fortune-flowfacts/fortune.verified.bc
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
```

结果：

```text
fortune native pipeline: 13.09s
confirmed_functions: 25
basic_blocks: 2369
instructions: 2908
0x3470 / 0x3748 block successors: 0x3e81, 0x374e
final !notdec.register.access residue: 1
remaining register access: FS_OFFSET only
llvm-as + opt verify: passed
```

性能：同口径上次 fortune 约 13.26s，本次 13.09s，没有看到性能下降。

复杂度评估：

- 实现效果：7/10。CFG 行为不变，但 instruction facts 更清晰，后续可以继续让 lowering 更少依赖 pcode 顺序。
- 理解成本：3/10。新增几个字段，但含义直接，对照 Ghidra instruction flow/fallthrough 模型更容易解释。
- 维护成本：3/10。当前字段只在 discovery 内使用，后续如果输出 JSON 或给 lowering 直接用，需要保持语义一致。

## 实现记录：native lowering 直接消费 native CFG

这次在前一轮 instruction flow facts 的基础上，继续收紧 lower 层对 pcode 顺序的依赖。

核心判断：

- 当 `PcodeLoweringConfig.BlockSuccessors` 非空时，说明这次是 native 链路的 lowering，CFG 应该优先信任 native discovery 的 block facts。
- 只有在没有 native CFG 的通用 pcode 路径里，才保留旧的顺序 fallback。

具体改动：

- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:112)
  - 增加 `usesNativeCfg()`，把 native lowering 和通用 pcode lowering 分开。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:219)
  - `buildBasicBlocks(...)` 在 native 模式下不再因为 terminator 自动把 `index + 1` 加进 block starts，避免继续用 pcode 顺序猜块边界。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:243)
  - `nativeFallthroughBlock(...)` 在 native 模式下只认 `Config.BlockSuccessors`，没有对应信息就返回空，不回退到顺序 next block。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:256)
  - `nativeConditionalFalseBlock(...)` 在 native 模式下只从 `Config.BlockSuccessors` 里找 false edge，不再猜顺序 fallthrough。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:376)
  - `CBRANCH` 的 false edge 如果 native CFG 找不到，就落到 `notdec_exit`，不再悄悄接到顺序块。

验证：

```text
cmake --build build --target notdec-native-llvm native_register_summary_test native_register_summary_ssa_test -j4
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/fortune-nativecfg-raw.ll
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-nativecfg/fortune.ll --summary-json-out /tmp/notdec-fortune-nativecfg/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-nativecfg/fortune.ll -o /tmp/notdec-fortune-nativecfg/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-nativecfg/fortune.bc -o /tmp/notdec-fortune-nativecfg/fortune.verified.bc
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
```

结果：

```text
fortune native pipeline: 12.71s
confirmed_functions: 25
basic_blocks: 2369
instructions: 2908
fortune still passes llvm-as + opt -passes=verify
final !notdec.register.access residue: 1
remaining register access: FS_OFFSET only
```

复杂度评估：

- 实现效果：8/10。native CFG 现在更明确地作为 lowering 权威，减少了 pcode 顺序带来的误判机会。
- 理解成本：3/10。只是把 native / generic 两条路径分开了。
- 维护成本：3/10。fallback 还保留在非 native 路径里，后续如果要彻底统一，还可以再把 block facts 往前收。

## 实现记录：native block starts 不再从 pcode branch targets 派生

这次继续收紧 native lowering 的 CFG 来源。

前一轮已经让 native 模式下的 fallthrough / conditional false edge 优先使用 `Config.BlockSuccessors`。但 `buildBasicBlocks(...)` 仍然会扫描 pcode branch target，把这些 target 作为 block starts。这样虽然通常正确，但语义上仍然是在 lower 层从 pcode 反推 native CFG。

具体改动：

- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:206)
  - `buildBasicBlocks(...)` 在 `usesNativeCfg()` 为 true 时，只使用 native `BlockSuccessors` 的 block start 和 successor 作为 block starts。
  - 从 pcode branch target 和 terminator 后 `index + 1` 推 block start 的逻辑只保留给非 native / generic pcode 路径。

验证：

```text
cmake --build build --target notdec-native-llvm native_register_summary_test native_register_summary_ssa_test -j4
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/fortune-native-blockstarts-raw.ll
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-native-blockstarts/fortune.ll --summary-json-out /tmp/notdec-fortune-native-blockstarts/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-native-blockstarts/fortune.ll -o /tmp/notdec-fortune-native-blockstarts/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-native-blockstarts/fortune.bc -o /tmp/notdec-fortune-native-blockstarts/fortune.verified.bc
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
```

结果：

```text
fortune native pipeline: 12.72s
confirmed_functions: 25
basic_blocks: 2369
instructions: 2908
0x3470 raw IR still has bb_3748 false edge to bb_374e
final !notdec.register.access residue: 1
remaining register access: FS_OFFSET only
llvm-as + opt verify: passed
```

复杂度评估：

- 实现效果：8/10。native lowering 的 block starts 现在来自 discovery block facts，而不是 pcode branch scan。
- 理解成本：2/10。只是把 native / generic pcode 分支放得更清楚。
- 维护成本：2/10。非 native fallback 不变，native 路径更少隐式猜测。

## 实现记录：把 native block ranges 传入 lowering config

这次把 native block facts 从只有 successors 扩成 ranges + successors。

目标是让 lower 层明确知道 native discovery 接受的 block body 和 CFG edges，而不是只靠 successors 推断 native 模式。

具体改动：

- [PcodeToLLVM.h](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/PcodeToLLVM.h:48)
  - `PcodeLoweringConfig` 新增 `BlockRanges`，类型是 `block start -> block end`。
  - 注释明确 `BlockRanges` 表示已接受的 block body，`BlockSuccessors` 表示 CFG edges。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:112)
  - `usesNativeCfg()` 改为看 `BlockRanges` 或 `BlockSuccessors`。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:202)
  - native 模式下，`buildBasicBlocks(...)` 会先把 `BlockRanges` 的 block starts 加入 lowering block starts。
- [notdec-native-llvm.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:545)
  - 新增 `blockRangesByStart(...)`，从 `NativeFunction.Blocks` 或 CLI resolved block ranges 构造 range map。
- [notdec-native-llvm.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:763)
  - `--all-confirmed` 路径把 `NativeFunction.Blocks` 的 ranges 传给 `PcodeLoweringConfig`。
- [notdec-native-llvm.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:1026)
  - `-f` / `-n` 路径把 resolved `FunctionBlockRanges` 也传给 lowering。

当前没有做的事：

- lower 层暂时还没有用 `BlockRanges` 的 end 来截断块内 pcode op 范围。
- 这一步需要把 block start/end 都映射到 op index，再替代现在的 `next BlockStart` 推 end，单独做风险更低。

验证：

```text
cmake --build build --target notdec-native-llvm native_register_summary_test native_register_summary_ssa_test -j4
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/fortune-blockranges-raw.ll
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-blockranges/fortune.ll --summary-json-out /tmp/notdec-fortune-blockranges/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-blockranges/fortune.ll -o /tmp/notdec-fortune-blockranges/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-blockranges/fortune.bc -o /tmp/notdec-fortune-blockranges/fortune.verified.bc
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
```

结果：

```text
fortune native pipeline: 12.86s
confirmed_functions: 25
basic_blocks: 2369
instructions: 2908
0x3470 raw IR still has bb_3748 false edge to bb_374e
final !notdec.register.access residue: 1
remaining register access: FS_OFFSET only
llvm-as + opt verify: passed
```

复杂度评估：

- 实现效果：7/10。block range facts 已经进入 lowering config，为后续按 range 切 pcode block 做准备。
- 理解成本：2/10。新增字段含义直接，和 `NativeBasicBlock` 一致。
- 维护成本：2/10。目前只是传递事实和 native-mode 判断，行为风险低。

## 实现记录：native lowering 用 BlockRanges 截断块内 pcode

这次让 `PcodeToLLVM` 真正使用上一轮传入的 `BlockRanges`。

之前 lower 层虽然拿到了 native block starts，但每个 LLVM basic block 的 pcode op end 仍按下一个 `BlockStart` 推导。这样仍然隐含了 pcode vector 顺序。现在 native 模式下会按 native block 的 `[start, end)` 地址范围截取块内 pcode op。

具体改动：

- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:70)
  - lower loop 改为使用 `BlockEnds[blockIndex]`，不再现场用下一个 `BlockStart` 算 end。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:236)
  - `buildBasicBlocks(...)` 在创建 block 时同步计算并保存 `BlockEnds`。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:248)
  - 新增 `blockEndForStart(...)`。
  - generic pcode 路径保持旧逻辑。
  - native 路径优先读取 `Config.BlockRanges[startAddress]`，从 start op 向后扫描，直到 op 的 instruction address 离开 `[blockStart, blockEnd)`。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:1383)
  - 新增 `BlockEnds`，和 `BlockStarts` 并行保存。

验证：

```text
cmake --build build --target notdec-native-llvm native_register_summary_test native_register_summary_ssa_test -j4
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/fortune-blockends-raw.ll
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-blockends/fortune.ll --summary-json-out /tmp/notdec-fortune-blockends/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-blockends/fortune.ll -o /tmp/notdec-fortune-blockends/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-blockends/fortune.bc -o /tmp/notdec-fortune-blockends/fortune.verified.bc
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
```

结果：

```text
fortune native pipeline: 12.71s
confirmed_functions: 25
basic_blocks: 2369
instructions: 2908
0x3470 raw IR still has bb_3748 false edge to bb_374e
final !notdec.register.access residue: 1
remaining register access: FS_OFFSET only
llvm-as + opt verify: passed
```

复杂度评估：

- 实现效果：8/10。native lowering 的 block body 现在由 native block range 决定，进一步减少了 pcode 顺序假设。
- 理解成本：3/10。新增 `BlockEnds`，但和 `BlockStarts` 并行，含义清楚。
- 维护成本：3/10。generic pcode 路径保持旧逻辑；native 路径依赖 discovery range facts。

## 实现记录：native terminator lowering 去掉顺序 nextBlock fallback

这次继续收紧 native lowering 里残留的顺序依赖。

之前虽然 native mode 已经优先使用 `BlockRanges` / `BlockSuccessors`，但 `lower(...)` 仍然会先计算 `nextBlock(blockIndex)`，再把它作为 `lowerTerminator(...)` 的 fallback 传下去。这个 fallback 只有在 generic pcode 路径下才应该存在。

具体改动：

- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:76)
  - `lower(...)` 在 native 模式下不再提前计算顺序 `nextBlock(blockIndex)` 作为 fallback。
  - native terminator lowering 收到的 fallback 直接是 `nullptr`。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:280)
  - `nativeFallthroughBlock(...)` / `nativeConditionalFalseBlock(...)` 继续只认 native CFG facts；找不到就返回空或 `notdec_exit`，不再回退到顺序 next block。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm native_register_summary_test native_register_summary_ssa_test -j2
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/notdec-fortune-no-pcode-fallback/fortune-3470-raw.ll
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-no-pcode-fallback-full/fortune.ll --summary-json-out /tmp/notdec-fortune-no-pcode-fallback-full/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-no-pcode-fallback/fortune-3470-raw.ll -o /tmp/notdec-fortune-no-pcode-fallback/fortune-3470-raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-no-pcode-fallback/fortune-3470-raw.bc -o /tmp/notdec-fortune-no-pcode-fallback/fortune-3470-raw.verified.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-no-pcode-fallback-full/fortune.ll -o /tmp/notdec-fortune-no-pcode-fallback-full/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-no-pcode-fallback-full/fortune.bc -o /tmp/notdec-fortune-no-pcode-fallback-full/fortune.verified.bc
```

结果：

```text
confirmed_functions: 25
basic_blocks: 2369
instructions: 2908
0x3470 raw IR still has bb_3748 false edge to bb_374e
final !notdec.register.access residue: 1
remaining register access: FS_OFFSET only
stores to register globals: 0
llvm-as + opt verify: passed
```

复杂度评估：

- 实现效果：8/10。native lowering 的 terminator fallback 现在只剩 native CFG facts，没有顺序块兜底。
- 理解成本：2/10。只是把 native / generic fallback 边界收紧。
- 维护成本：2/10。不会影响 generic pcode 路径，也不会影响现有 summary 结果。

## 实现记录：seed decode 改成本地可达 block facts

这次继续减少 native decode 对线性 pcode 顺序的依赖。

之前 seed decode 会一次线性取一个小窗口。窗口里如果包含 `return` / unconditional branch 后面的字节，后续 block 构建仍可能把这些字节放进同一个函数事实里。第一版尝试过直接在第一个 no-fallthrough terminator 处截断窗口，但 fortune 里会丢掉同一窗口内仍可由本地 branch target 到达的块，所以没有采用。

最终做法是：先给窗口内 instruction 标注 flow facts，再从 seed entry 在这个小窗口内跑一次本地 CFG 可达性，只把本地可达 instruction 变成 block facts。

具体改动：

- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1526)
  - `decodeSeed(...)` 在 `annotateDecodedInstructionFlows(...)` 后调用 `reachableInstructionStarts(...)`。
  - `state.addInstruction(...)` 和 `addDecodedFunctionBlocks(...)` 只消费本地可达的 instruction。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1555)
  - 新增 `reachableInstructionStarts(...)`。
  - 在当前 decode 窗口内，从 seed entry 沿 `DirectFlowTargets` 和 `Fallthrough` 走本地 CFG。
  - 不靠“下一个线性地址一定可达”来跨过 no-fallthrough terminator。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:199)
  - `buildBasicBlocks(...)` 改为返回 `bool`，native 模式下要求 `BlockRanges` 覆盖所有 pcode op。
  - native lowering 不再从 `BlockSuccessors` 反推缺失的 block start；缺 range 直接报错。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:316)
  - 新增 `blockAddressForStart(...)` / `blockAddressForIndex(...)`，让 LLVM block 名和 successor 查询使用 native block start，而不是 pcode op 的地址。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:133)
  - 增加 `testNativeBlockRangeIsRequired()`，覆盖 native range 缺失时失败的情况。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target pcode_to_llvm_test notdec-native-llvm native_register_summary_test native_register_summary_ssa_test -j2
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-local-reach/fortune.ll --summary-json-out /tmp/notdec-fortune-local-reach/summary.json
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/notdec-fortune-local-reach-raw/fortune-3470-raw.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-local-reach/fortune.ll -o /tmp/notdec-fortune-local-reach/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-local-reach/fortune.bc -o /tmp/notdec-fortune-local-reach/fortune.verified.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-local-reach-raw/fortune-3470-raw.ll -o /tmp/notdec-fortune-local-reach-raw/fortune-3470-raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-local-reach-raw/fortune-3470-raw.bc -o /tmp/notdec-fortune-local-reach-raw/fortune-3470-raw.verified.bc
```

结果：

```text
fortune native pipeline: about 9.31s
confirmed_functions: 25
basic_blocks: 1022
instructions: 2574
sleigh-direct-call seeds: 130
final !notdec.register.access residue: 1
remaining register access: FS_OFFSET only
stores to register globals: 0
llvm-as + opt verify: passed
```

额外检查：

- discovery 里 `0x3748` 仍然是独立 block。
- `0x3748` successors 是 `0x3e81` 和 `0x374e`。
- full summary 链路没有再出现 `R9` register access residue。

复杂度评估：

- 实现效果：8/10。seed 内 block facts 现在来自 instruction flow 的本地可达性，明显减少线性窗口带来的不可达块。
- 理解成本：4/10。新增一个小的本地 CFG walk；比直接截断窗口更准确。
- 维护成本：3/10。逻辑只在 native discovery/lowering 边界，generic pcode 路径不受影响。

## 实现记录：call/xref 也按本地可达 instruction 提交

上一节只过滤了进入 block facts 的 instruction，但 `addDirectControlFlow(...)` 仍然会在过滤前把线性窗口里的 call / flow / data xref 和 unresolved indirect flow 写进全局 state。这样不可达字节虽然不进 block，仍可能生成函数 seed 或 xref。

这次把这部分也收紧：先收集 pending facts，等 seed 内本地可达性算完后，只提交可达 instruction 地址上的 facts。

具体改动：

- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1539)
  - `decodeSeed(...)` 在 `reachableInstructionStarts(...)` 之后才提交 call targets、xref、unresolved flow。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1597)
  - 新增 `reachableCallTargets(...)` / `reachableXrefs(...)` / `reachableUnresolvedFlows(...)`。
  - 这些函数都按 reachable instruction address 过滤。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1747)
  - `collectDirectControlFlow(...)` 不再直接写 `NativeProgramState`。
  - direct internal call target 和 instruction call target 分开记录，避免 PLT/external call 被错误加入 native function seed。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1971)
  - data/string xref 也改成 pending xref，统一在可达过滤后提交。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm native_register_summary_test native_register_summary_ssa_test pcode_to_llvm_test -j2
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-reachable-xrefs3/fortune.ll --summary-json-out /tmp/notdec-fortune-reachable-xrefs3/summary.json
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/notdec-fortune-reachable-xrefs3-raw/fortune-3470-raw.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-reachable-xrefs3/fortune.ll -o /tmp/notdec-fortune-reachable-xrefs3/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-reachable-xrefs3/fortune.bc -o /tmp/notdec-fortune-reachable-xrefs3/fortune.verified.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-reachable-xrefs3-raw/fortune-3470-raw.ll -o /tmp/notdec-fortune-reachable-xrefs3-raw/fortune-3470-raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-reachable-xrefs3-raw/fortune-3470-raw.bc -o /tmp/notdec-fortune-reachable-xrefs3-raw/fortune-3470-raw.verified.bc
```

结果：

```text
fortune native pipeline: about 9.38s
confirmed_functions: 25
basic_blocks: 1022
instructions: 2574
sleigh-direct-call seeds: 109
xrefs: total=1830 flow=977 call=580 data=268 string=5
final !notdec.register.access residue: 1
remaining register access: FS_OFFSET only
stores to register globals: 0
llvm-as + opt verify: passed
```

复杂度评估：

- 实现效果：8/10。block facts、call seeds、xref、unresolved flow 都统一受本地可达性约束。
- 理解成本：4/10。多了 pending fact 提交流程，但比直接写 state 更符合当前分层。
- 维护成本：3/10。逻辑仍限制在 seed decode 内，summary 链路和 generic pcode lowering 不受影响。

## 实现记录：instruction JSON 输出 flow facts

当前 native decode 已经把机器指令级 flow facts 存在 `NativeInstruction` 里，但 `notdec-native-discover --instructions-*` 之前只输出 address、bytes、text、source。这样排查 block facts 时还要回头看代码或 CFG 输出，不够直接。

这次只暴露已有事实，不改变 discovery / lowering 行为。

具体改动：

- [NativeAnalysis.h](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/NativeAnalysis.h:193)
  - 声明 `toString(NativeInstructionFlowKind)`。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:2421)
  - 实现 `NativeInstructionFlowKind` 的字符串输出。
- [notdec-native-discover.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-discover.cpp:1204)
  - `printInstructionObject(...)` 增加：
    - `flow_kind`
    - `direct_flow_targets`
    - `direct_call_targets`
    - `fallthrough`
    - `has_indirect_call`
- [native_analysis_facts_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_analysis_facts_test.cpp:19)
  - 增加 `NativeInstructionFlowKind` 字符串输出测试。
- [CMakeLists.txt](/sn640/NotDec/external/NotDec-bin2llvm/CMakeLists.txt:246)
  - 在 native 依赖启用时注册 `notdec.native_analysis.facts`。

抽样验证：

```text
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --instructions-range-json 0x3740 0x3758 /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
```

关键输出：

```json
{
  "address": "0x3748",
  "text": "JZ 0x3e81",
  "flow_kind": "conditional branch",
  "direct_flow_targets": ["0x3e81"],
  "fallthrough": "0x374e"
}
```

完整验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target native_analysis_facts_test notdec-native-discover notdec-native-llvm native_register_summary_test native_register_summary_ssa_test pcode_to_llvm_test -j2
/tmp/notdec-bin2llvm-build/bin/native_analysis_facts_test
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-flow-json/fortune.ll --summary-json-out /tmp/notdec-fortune-flow-json/summary.json
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/notdec-fortune-flow-json-raw/fortune-3470-raw.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-flow-json/fortune.ll -o /tmp/notdec-fortune-flow-json/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-flow-json/fortune.bc -o /tmp/notdec-fortune-flow-json/fortune.verified.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-flow-json-raw/fortune-3470-raw.ll -o /tmp/notdec-fortune-flow-json-raw/fortune-3470-raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-flow-json-raw/fortune-3470-raw.bc -o /tmp/notdec-fortune-flow-json-raw/fortune-3470-raw.verified.bc
```

结果：

```text
confirmed_functions: 25
basic_blocks: 1022
instructions: 2574
sleigh-direct-call seeds: 109
final !notdec.register.access residue: 1
remaining register access: FS_OFFSET only
stores to register globals: 0
llvm-as + opt verify: passed
```

复杂度评估：

- 实现效果：6/10。没有改变语义，但让 instruction facts 可以直接审计。
- 理解成本：1/10。只是 JSON 字段和枚举字符串。
- 维护成本：1/10。输出字段来自已有结构体，不新增分析状态。

## 实现记录：native lowering 使用显式函数入口块

上一轮已经让 native block facts 进入 `PcodeToLLVM`，但 LLVM `entry` 仍然跳到 `BlockStarts.front()`。
这个值来自 p-code op 的顺序，不一定等于 native 函数入口。native decode 后续要减少对 p-code 顺序推 CFG 的依赖，所以这里改成显式入口地址驱动。

具体改动：

- [PcodeToLLVM.h](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/PcodeToLLVM.h:27)
  - `PcodeLoweringConfig` 增加 `EntryAddress`。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:71)
  - `PcodeLowerer::lower(...)` 不再直接用 `BlockStarts.front()` 作为入口。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:125)
  - 新增 `entryBlockForProgram(...)`。
  - native CFG 下按 `EntryAddress` 查找覆盖该地址的 `BlockRanges`。
  - 普通 p-code lowering 没有设置 `EntryAddress` 时保持旧行为。
- [notdec-native-llvm.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:758)
  - `--all-confirmed` 每个函数 lowering 时传入 `function.Entry`。
- [notdec-native-llvm.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:1021)
  - 单函数 `-f` lowering 时传入选中的函数入口地址。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:155)
  - 增加 `testNativeEntryAddressChoosesEntryBlock()`。
  - 测试里 p-code op 顺序故意是 `0x2000` 在前、`0x1000` 在后，确认 LLVM `entry` 跳到 `bb_1000`。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target pcode_to_llvm_test notdec-native-llvm native_register_summary_test native_register_summary_ssa_test native_analysis_facts_test -j2
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_analysis_facts_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-entry-address/fortune.ll --summary-json-out /tmp/notdec-fortune-entry-address/summary.json
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/notdec-fortune-entry-address-raw/fortune-3470-raw.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-entry-address/fortune.ll -o /tmp/notdec-fortune-entry-address/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-entry-address/fortune.bc -o /tmp/notdec-fortune-entry-address/fortune.verify.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-entry-address-raw/fortune-3470-raw.ll -o /tmp/notdec-fortune-entry-address-raw/fortune-3470-raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-entry-address-raw/fortune-3470-raw.bc -o /tmp/notdec-fortune-entry-address-raw/fortune-3470-raw.verify.bc
```

结果：

```text
fortune native pipeline: about 9.56s
confirmed_functions: 25
basic_blocks: 1022
instructions: 2574
sleigh-direct-call seeds: 109
final !notdec.register.access residue: 1
remaining !notdec.register.access: FS_OFFSET only
stores to register globals: 0
llvm-as + opt verify: passed
```

注意：最终 IR 里还有少量 `summary_ssa.entry` load，例如 bootstrap / 顶层函数里的 `RBX`、`RSP`、`RAX`。这些不是原始 `!notdec.register.access` store/load 残留；后续要继续区分“真实入口环境输入”和“可消除寄存器访问”。

复杂度评估：

- 实现效果：7/10。修掉了一个明确的 p-code 顺序依赖点，native lowering 入口现在来自 function fact。
- 理解成本：2/10。只给 lowering config 多传一个入口地址。
- 维护成本：2/10。普通 p-code 路径保持旧行为，native 路径失败时给出明确错误。

## 实现记录：native range decode 固定按地址顺序

`collectSleighPcodeRanges(...)` 之前按调用方传入的 range 顺序拼接 p-code。native block facts 本身有明确地址，p-code 拼接顺序不应该取决于 discovery / vector 的偶然顺序。

具体改动：

- [SleighLift.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/SleighLift.cpp:6)
  - 增加 `<algorithm>`。
- [SleighLift.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/SleighLift.cpp:538)
  - `collectSleighPcodeRanges(...)` 先复制并按 range start 排序，再逐个 range 调 Sleigh。
  - 这不改变单个 range 内的指令顺序，只固定多个 native block range 的拼接顺序。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target pcode_to_llvm_test notdec-native-llvm native_register_summary_test native_register_summary_ssa_test native_analysis_facts_test -j2
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_analysis_facts_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-range-sort/fortune.ll --summary-json-out /tmp/notdec-fortune-range-sort/summary.json
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/notdec-fortune-range-sort-raw/fortune-3470-raw.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-range-sort/fortune.ll -o /tmp/notdec-fortune-range-sort/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-range-sort/fortune.bc -o /tmp/notdec-fortune-range-sort/fortune.verify.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-range-sort-raw/fortune-3470-raw.ll -o /tmp/notdec-fortune-range-sort-raw/fortune-3470-raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-range-sort-raw/fortune-3470-raw.bc -o /tmp/notdec-fortune-range-sort-raw/fortune-3470-raw.verify.bc
```

结果：

```text
fortune native pipeline: about 8.67s
confirmed_functions: 25
basic_blocks: 1022
instructions: 2574
sleigh-direct-call seeds: 109
final !notdec.register.access residue: 1
remaining !notdec.register.access: FS_OFFSET only
stores to register globals: 0
llvm-as + opt verify: passed
```

复杂度评估：

- 实现效果：5/10。只是稳定 p-code range 拼接顺序，但方向上减少了调用方顺序对 lowering 的影响。
- 理解成本：1/10。局部排序，行为直观。
- 维护成本：1/10。没有新增状态。

## 实现记录：native lowering 不再把缺失目标静默变成 ret block

继续收紧 `PcodeToLLVM` 对 native block facts 的使用。之前 `blockForTarget(...)` 在目标地址没有本地 block 时会直接创建一个空 LLVM block，最后补 `ret void`。这对普通 p-code 还可以作为兜底，但 native CFG 下会掩盖两类真实情况：

- NOP / ENDBR 等机器 block 没有任何 SLEIGH p-code，但它仍是 native CFG 的合法 block。
- direct branch 跳出当前函数 block ranges，可能是 internal/external tail jump，或者单函数 raw 模式下还没发现的跨 chunk 目标。

这次改成：

- 当前函数 range 内的 native target 必须有 block fact；缺失就报错。
- 当前函数 range 外的 direct target 不再造 `ret void` block，改成 tail call。
- 没有 p-code 的 native block 也创建 LLVM block，并按 `BlockSuccessors` 接到下一个 block。

具体改动：

- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:115)
  - `PcodeLowerer::lower(...)` 在普通 p-code block lowering 后，额外 lower `EmptyNativeBlockAddresses`。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:148)
  - 新增 `sortedNativeRanges()`，native range 遍历固定按地址顺序。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:161)
  - 新增 `nativeRangesCoverAddress(...)`，用来区分当前函数内缺 block 和跳出当前 range。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:279)
  - `buildBasicBlocks(...)` 记录没有 p-code op 的 native block。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:422)
  - `nativeFallthroughBlock(...)` 改成能返回错误，不再用 `blockForTarget(...)` 静默造块。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:441)
  - `nativeConditionalFalseBlock(...)` 同样改成 strict native target。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:466)
  - 新增 `nativeEmptyBlockSuccessor(...)`，给 empty native block 生成 successor branch 或 `ret void`。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:499)
  - 新增 `blockForNativeTarget(...)`，native CFG 下缺当前 range 内 block 时明确报错。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:513)
  - 新增 `tailJumpBlockForKnownFunction(...)`，支持 conditional branch 跳到 known/synthetic function 时生成 tail-call block。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:571)
  - direct `BRANCH` 目标优先匹配本地 block、external target、internal target。
  - 如果目标不在当前 native ranges 内，降成 `notdec_native_<addr>` tail call。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:606)
  - direct `CBRANCH` 目标也按同样规则处理；跳出当前 ranges 时生成 conditional tail-call block。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:1634)
  - 增加 `TailJumpBlockForAddress` 和 `EmptyNativeBlockAddresses`。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:35)
  - 增加 `uniqueVarnode(...)`、`copyOp(...)`、`cbranchOp(...)` 测试 helper。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:166)
  - 增加 internal unconditional tail branch 测试。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:207)
  - 增加 internal conditional tail branch 测试。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:310)
  - 增加当前 ranges 外 direct branch 降成 synthetic tail call 的测试。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:351)
  - 增加 branch 目标是 empty native block 的测试。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:384)
  - 增加 native successor 缺 block 时失败的测试。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target pcode_to_llvm_test notdec-native-llvm native_register_summary_test native_register_summary_ssa_test native_analysis_facts_test -j2
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_analysis_facts_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-empty-native-block/fortune.ll --summary-json-out /tmp/notdec-fortune-empty-native-block/summary.json
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/notdec-fortune-empty-native-block-raw/fortune-3470-raw.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-empty-native-block/fortune.ll -o /tmp/notdec-fortune-empty-native-block/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-empty-native-block/fortune.bc -o /tmp/notdec-fortune-empty-native-block/fortune.verify.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-empty-native-block-raw/fortune-3470-raw.ll -o /tmp/notdec-fortune-empty-native-block-raw/fortune-3470-raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-empty-native-block-raw/fortune-3470-raw.bc -o /tmp/notdec-fortune-empty-native-block-raw/fortune-3470-raw.verify.bc
```

结果：

```text
fortune native pipeline: about 9.37s
confirmed_functions: 25
basic_blocks: 1022
instructions: 2574
sleigh-direct-call seeds: 109
final !notdec.register.access residue: 1
remaining !notdec.register.access: FS_OFFSET only
stores to register globals: 0
llvm-as + opt verify: passed
```

实现中发现的真实样例：

- `0x367c` 是 `fortune` 里 `0x3470` 函数的 native block，机器指令是 NOP，SLEIGH 没有生成 p-code。现在会生成 `bb_367c` 并接到 successor。
- `0x3470` 的 p-code 里有 `CBRANCH 0x27b2`。`0x27b2` 是另一个 confirmed function / chunk 的入口，不属于当前函数 ranges。现在会生成 conditional tail-call block，而不是空 `ret void` block。

复杂度评估：

- 实现效果：8/10。消除了 native CFG 下最危险的静默 ret block 兜底，同时覆盖了真实 NOP block 和跨 chunk branch。
- 理解成本：5/10。`PcodeToLLVM` 多了 empty block 和 tail-call block 两个概念，但都直接对应 native CFG 事实。
- 维护成本：4/10。后续如果要更精确处理共享 chunk，可能还要把跨 range branch 从 synthetic call 升级成显式 chunk/function 关系。

## 实现记录：EntryAddress 支持 empty native block

上一段已经让没有 p-code 的 native block 也能被 lower 成 LLVM block，但 `entryBlockForProgram(...)` 仍只从有 p-code 的 `BlockStarts` 里找入口。如果函数入口正好落在 NOP / ENDBR 这种 empty native block，LLVM `entry` 会找不到真实 native entry block。

这次把 native 模式入口选择改成按 `BlockRanges` 查找入口地址所属 block，再从 `BlockForAddress` 取 LLVM block。这样有 p-code 的 block 和 empty block 走同一套 native block fact。

具体改动：

- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:170)
  - `entryBlockForProgram(...)` 在 native CFG 模式下改为遍历 `sortedNativeRanges()`。
  - 找到覆盖 `EntryAddress` 的 block range 后，用 block start 查 `BlockForAddress`。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:384)
  - 增加 `testNativeEntryAddressCanTargetEmptyBlock()`。
  - 测试构造 `0x1000` empty entry block，successor 是 `0x2000` 有 p-code block，确认 LLVM `entry` 跳到 `bb_1000`。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target pcode_to_llvm_test notdec-native-llvm native_register_summary_test native_register_summary_ssa_test native_analysis_facts_test -j2
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_analysis_facts_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-empty-entry/fortune.ll --summary-json-out /tmp/notdec-fortune-empty-entry/summary.json
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/notdec-fortune-empty-entry-raw/fortune-3470-raw.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-empty-entry/fortune.ll -o /tmp/notdec-fortune-empty-entry/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-empty-entry/fortune.bc -o /tmp/notdec-fortune-empty-entry/fortune.verify.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-empty-entry-raw/fortune-3470-raw.ll -o /tmp/notdec-fortune-empty-entry-raw/fortune-3470-raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-empty-entry-raw/fortune-3470-raw.bc -o /tmp/notdec-fortune-empty-entry-raw/fortune-3470-raw.verify.bc
```

结果：

```text
fortune native pipeline: about 9.38s
confirmed_functions: 25
basic_blocks: 1022
instructions: 2574
sleigh-direct-call seeds: 109
final !notdec.register.access residue: 1
remaining !notdec.register.access: FS_OFFSET only
stores to register globals: 0
llvm-as + opt verify: passed
```

复杂度评估：

- 实现效果：6/10。补上 empty block 支持后的入口选择缺口。
- 理解成本：1/10。入口选择和 native block fact 对齐。
- 维护成本：1/10。没有新增状态。

## 实现记录：native 多 successor block 必须有 terminator

native lowering 里还有一个会隐藏 CFG 问题的点：一个 block 的 p-code 没有 terminator 时，会走 `nativeFallthroughBlock(...)`。如果 native `BlockSuccessors` 有多个 successor，之前会当成没有 successor，最后生成 `ret void`。

这次只收紧多个 successor 的情况：

- 0 successor：仍允许，常见于 no-return call 或函数出口。
- 1 successor：正常 fallthrough 到这个 native successor。
- 大于 1 successor：必须由 p-code terminator 处理；如果走到这里就报错。

具体改动：

- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:424)
  - `nativeFallthroughBlock(...)` 对 `BlockSuccessors.size() > 1` 报错：`successors but no p-code terminator`。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:443)
  - 增加 `testNativeMultipleSuccessorsRequireTerminator()`。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target pcode_to_llvm_test notdec-native-llvm native_register_summary_test native_register_summary_ssa_test native_analysis_facts_test -j2
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_analysis_facts_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-strict-successors/fortune.ll --summary-json-out /tmp/notdec-fortune-strict-successors/summary.json
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/notdec-fortune-strict-successors-raw/fortune-3470-raw.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-strict-successors/fortune.ll -o /tmp/notdec-fortune-strict-successors/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-strict-successors/fortune.bc -o /tmp/notdec-fortune-strict-successors/fortune.verify.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-strict-successors-raw/fortune-3470-raw.ll -o /tmp/notdec-fortune-strict-successors-raw/fortune-3470-raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-strict-successors-raw/fortune-3470-raw.bc -o /tmp/notdec-fortune-strict-successors-raw/fortune-3470-raw.verify.bc
```

结果：

```text
fortune native pipeline: about 9.51s
confirmed_functions: 25
basic_blocks: 1022
instructions: 2574
sleigh-direct-call seeds: 109
final !notdec.register.access residue: 1
remaining !notdec.register.access: FS_OFFSET only
stores to register globals: 0
llvm-as + opt verify: passed
```

复杂度评估：

- 实现效果：5/10。只收紧一个错误兜底，但能防止多分支 CFG 被误降成函数出口。
- 理解成本：1/10。规则直接对应 native successor 数量。
- 维护成本：1/10。没有新增状态。

## 实现记录：校验 native conditional successor facts

`CBRANCH` 的 p-code 只给 taken target，false edge 要靠 native `BlockSuccessors`。之前 `nativeConditionalFalseBlock(...)` 会从 successor 列表里拿第一个不等于 true target 的地址。如果 block facts 里漏了 true target，或者有多个 false 候选，lowering 会悄悄选错。

这次把规则收紧：

- true target 在当前函数 ranges 内时，`BlockSuccessors` 必须包含它。
- true target 跳出当前函数 ranges 时，允许 `BlockSuccessors` 只记录当前函数内 false edge；`fortune` 里 `0x3470` 的 `0x3930 -> 0x27b2` 就是这种跨 chunk conditional tail path。
- false successor 最多只能有一个。

具体改动：

- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:448)
  - `nativeConditionalFalseBlock(...)` 校验 true target 是否在 successor facts 中。
  - 对当前 ranges 外 true target 放宽，因为它会被 lower 成 tail-call block。
  - 多个 false successor 直接报错。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:469)
  - 增加 true successor 缺失时报错测试。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:493)
  - 增加 true target 跳出当前 ranges 时允许只有 false successor 的测试。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:539)
  - 增加多个 false successor 报错测试。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target pcode_to_llvm_test notdec-native-llvm native_register_summary_test native_register_summary_ssa_test native_analysis_facts_test -j2
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_analysis_facts_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-strict-cbranch/fortune.ll --summary-json-out /tmp/notdec-fortune-strict-cbranch/summary.json
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/notdec-fortune-strict-cbranch-raw/fortune-3470-raw.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-strict-cbranch/fortune.ll -o /tmp/notdec-fortune-strict-cbranch/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-strict-cbranch/fortune.bc -o /tmp/notdec-fortune-strict-cbranch/fortune.verify.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-strict-cbranch-raw/fortune-3470-raw.ll -o /tmp/notdec-fortune-strict-cbranch-raw/fortune-3470-raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-strict-cbranch-raw/fortune-3470-raw.bc -o /tmp/notdec-fortune-strict-cbranch-raw/fortune-3470-raw.verify.bc
```

结果：

```text
fortune native pipeline: about 9.40s
confirmed_functions: 25
basic_blocks: 1022
instructions: 2574
sleigh-direct-call seeds: 109
final !notdec.register.access residue: 1
remaining !notdec.register.access: FS_OFFSET only
stores to register globals: 0
llvm-as + opt verify: passed
```

复杂度评估：

- 实现效果：6/10。减少了 conditional false edge 的猜测，避免 successor facts 不一致时静默错连。
- 理解成本：2/10。多了一个跨 ranges true target 例外，但它对应当前 tail-call lowering 规则。
- 维护成本：2/10。仍局限在 PcodeToLLVM native lowering 内。

## 实现记录：native CBRANCH 必须有 successor facts

上一段已经校验 `CBRANCH` 的 true / false successor 内容，但如果当前 block 完全没有 `BlockSuccessors` entry，之前仍会落到旧 fallback。native 工具链正常会为每个 block 写入 successor entry，即使 successor 为空，所以缺 entry 本身就是 fact 缺失。

这次只收紧 direct `CBRANCH`：

- native CFG 模式下，当前 conditional block 没有 successor facts 时直接报错。
- 如果确实没有当前函数内 false edge，应显式传空 successor list。

具体改动：

- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:448)
  - `nativeConditionalFalseBlock(...)` 在 native CFG 模式下缺 `BlockSuccessors` entry 时报 `missing successor facts`。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:207)
  - 旧的 conditional tail branch 测试补显式空 successor list。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:494)
  - 增加 `testNativeConditionalRequiresSuccessorFacts()`。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target pcode_to_llvm_test notdec-native-llvm native_register_summary_test native_register_summary_ssa_test native_analysis_facts_test -j2
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_analysis_facts_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-missing-cbranch-successors/fortune.ll --summary-json-out /tmp/notdec-fortune-missing-cbranch-successors/summary.json
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/notdec-fortune-missing-cbranch-successors-raw/fortune-3470-raw.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-missing-cbranch-successors/fortune.ll -o /tmp/notdec-fortune-missing-cbranch-successors/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-missing-cbranch-successors/fortune.bc -o /tmp/notdec-fortune-missing-cbranch-successors/fortune.verify.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-missing-cbranch-successors-raw/fortune-3470-raw.ll -o /tmp/notdec-fortune-missing-cbranch-successors-raw/fortune-3470-raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-missing-cbranch-successors-raw/fortune-3470-raw.bc -o /tmp/notdec-fortune-missing-cbranch-successors-raw/fortune-3470-raw.verify.bc
```

结果：

```text
fortune native pipeline: about 9.40s
confirmed_functions: 25
basic_blocks: 1022
instructions: 2574
sleigh-direct-call seeds: 109
final !notdec.register.access residue: 1
remaining !notdec.register.access: FS_OFFSET only
stores to register globals: 0
llvm-as + opt verify: passed
```

复杂度评估：

- 实现效果：4/10。补齐上一轮 conditional successor 校验的 missing-entry 情况。
- 理解成本：1/10。缺 successor facts 直接报错。
- 维护成本：1/10。没有新增状态。

## 实现记录：BRANCHIND 消费 native successor facts

上一轮之后，direct branch / conditional branch 已经基本按 `BlockSuccessors` lowering，但 `BRANCHIND` 还会在找不到已知 GOT external tail jump 时直接接到 `notdec_exit`。这会无视 decode 层后续可能提供的 indirect branch successor facts。

这次只做保守收紧：

- 已知 GOT/PLT external tail jump 仍走原来的 tail call 逻辑。
- native CFG 模式下，`BRANCHIND` 必须有当前 block 的 successor entry。
- successor 为空时，表示 decode 层没有解析出目标，仍接 `notdec_exit`。
- successor 正好一个时，按这个 fact 生成普通 `br`。
- successor 多个时先报错，不用 `indirectbr` 硬凑。当前还没有 jump table 目标表达式和 LLVM label address 的可靠对应关系。

具体改动：

- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:522)
  - 新增 `nativeIndirectBranchSuccessor(...)`，统一检查 `BRANCHIND` 的 native successor facts。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:744)
  - `lowerTerminator(...)` 的 `BranchInd` 分支在 native CFG 模式下消费唯一 successor；缺 facts 或多 successor 报错。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:89)
  - 新增 `branchIndOp(...)` 测试构造器。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:598)
  - 增加唯一 successor 能降低为 direct branch 的测试。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:644)
  - 增加缺 successor facts 报错测试。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:665)
  - 增加多个 indirect successor 暂不支持的报错测试。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target pcode_to_llvm_test notdec-native-llvm native_register_summary_test native_register_summary_ssa_test native_analysis_facts_test -j2
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_analysis_facts_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-branchind-successors/fortune.ll --summary-json-out /tmp/notdec-fortune-branchind-successors/summary.json
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/notdec-fortune-branchind-successors-raw/fortune-3470-raw.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-branchind-successors/fortune.ll -o /tmp/notdec-fortune-branchind-successors/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-branchind-successors/fortune.bc -o /tmp/notdec-fortune-branchind-successors/fortune.verify.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-branchind-successors-raw/fortune-3470-raw.ll -o /tmp/notdec-fortune-branchind-successors-raw/fortune-3470-raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-branchind-successors-raw/fortune-3470-raw.bc -o /tmp/notdec-fortune-branchind-successors-raw/fortune-3470-raw.verify.bc
```

结果：

```text
fortune native pipeline: about 9.36s
confirmed_functions: 25
basic_blocks: 1022
instructions: 2574
sleigh-direct-call seeds: 109
unresolved_indirect_flows.total: 1
unresolved_indirect_flows.indirect branch: 1
final !notdec.register.access residue: 1
remaining !notdec.register.access: FS_OFFSET only
stores to register globals: 0
llvm-as + opt verify: passed
```

复杂度评估：

- 实现效果：5/10。不会解决 fortune 里 `0x2879` 的未知 indirect branch，但以后 decode 层如果给出唯一后继，lowering 不再丢掉它。
- 理解成本：2/10。规则和 direct/conditional 分支一致：native facts 是事实来源；多目标 indirect branch 明确留给 jump table 设计。
- 维护成本：2/10。只新增一个局部 helper 和三条 synthetic 测试。

## 实现记录：无 terminator / 空 native block 也必须有 successor facts

`CBRANCH` 和 `BRANCHIND` 已经要求 native successor facts，但还有两个旧兜底会生成隐式 `ret void`：

- 有 p-code、但没有 p-code terminator 的 native block。
- 没有 p-code 的空 native block。

在 native CFG 模式下，这两种 block 的控制流也应该由 decode/block facts 决定。确实无后继时应显式传空 successor list；缺 entry 代表 facts 缺失。

这次收紧：

- `nativeFallthroughBlock(...)` 在 native CFG 模式下缺 `BlockSuccessors` entry 时报错。
- `nativeEmptyBlockSuccessor(...)` 在 native CFG 模式下缺 `BlockSuccessors` entry 时报错。
- 已有空 block 正常测试补显式空 successor list。

具体改动：

- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:424)
  - `nativeFallthroughBlock(...)` 缺 successor facts 时不再隐式接 `ret void`。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:509)
  - `nativeEmptyBlockSuccessor(...)` 缺 successor facts 时不再隐式接 `ret void`。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:361)
  - `testNativeDirectBranchCanTargetEmptyBlock()` 补显式空 successor list。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:480)
  - 增加无 terminator block 缺 successor facts 报错测试。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:501)
  - 增加空 native block 缺 successor facts 报错测试。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:568)
  - conditional tail 测试里的空 false block 补显式空 successor list。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target pcode_to_llvm_test notdec-native-llvm native_register_summary_test native_register_summary_ssa_test native_analysis_facts_test -j2
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_analysis_facts_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-missing-fallthrough-successors/fortune.ll --summary-json-out /tmp/notdec-fortune-missing-fallthrough-successors/summary.json
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -f 0x3470 --no-register-ssa-pass --no-prototype-recovery-pass --no-instcombine-pass -o /tmp/notdec-fortune-missing-fallthrough-successors-raw/fortune-3470-raw.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-missing-fallthrough-successors/fortune.ll -o /tmp/notdec-fortune-missing-fallthrough-successors/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-missing-fallthrough-successors/fortune.bc -o /tmp/notdec-fortune-missing-fallthrough-successors/fortune.verify.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-missing-fallthrough-successors-raw/fortune-3470-raw.ll -o /tmp/notdec-fortune-missing-fallthrough-successors-raw/fortune-3470-raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-missing-fallthrough-successors-raw/fortune-3470-raw.bc -o /tmp/notdec-fortune-missing-fallthrough-successors-raw/fortune-3470-raw.verify.bc
```

结果：

```text
fortune native pipeline: about 9.35s
confirmed_functions: 25
basic_blocks: 1022
instructions: 2574
unresolved_indirect_flows.total: 1
unresolved_indirect_flows.indirect branch: 1
final !notdec.register.access residue: 1
remaining !notdec.register.access: FS_OFFSET only
stores to register globals: 0
llvm-as + opt verify: passed
```

复杂度评估：

- 实现效果：5/10。继续消除 lowering 层的隐式 `ret void` 兜底，让缺 facts 更早暴露。
- 理解成本：1/10。规则简单：native CFG 模式下，每个需要 lower 控制流的 block 都必须有 successor entry。
- 维护成本：1/10。没有新增状态，只收紧两个已有 helper。

## 已完成：多目标 BRANCHIND lowering

当前这一步只处理 lowering 侧：如果 block facts 已经明确给出 `BRANCHIND`
的多个后继，`PcodeToLLVM` 可以把它表达成 LLVM `switch`。

具体改动：

- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:2063)
  - `buildDecodedBlocks(...)` 对 `NativeInstructionFlowKind::IndirectBranch`
    使用 `DirectFlowTargets` 作为 block successors。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:535)
  - `nativeIndirectBranchSuccessor(...)` 从单个 block 改为返回多个目标 block。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:759)
  - `lowerTerminator(...)` 在 `BRANCHIND` 有多个 native successors 时生成
    `switch target, default notdec_exit`，每个 case 是一个目标地址。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:707)
  - `testNativeIndirectBranchLowersMultipleSuccessorsAsSwitch()` 覆盖多目标
    indirect branch lowering。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target pcode_to_llvm_test native_analysis_facts_test notdec-native-llvm -j2
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_analysis_facts_test
cmake --build /tmp/notdec-bin2llvm-build --target native_register_summary_test native_register_summary_ssa_test -j2
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/usr/bin/time -f 'elapsed=%e' /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-indirect-switch/fortune.ll --summary-json-out /tmp/notdec-fortune-indirect-switch/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-indirect-switch/fortune.ll -o /tmp/notdec-fortune-indirect-switch/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-indirect-switch/fortune.bc -o /tmp/notdec-fortune-indirect-switch/fortune.verify.bc
```

结果：

```text
pcode_to_llvm_test: passed
native_analysis_facts_test: passed
native_register_summary_test: passed
native_register_summary_ssa_test: passed
fortune native pipeline: 9.48s
llvm-as + opt verify: passed
```

复杂度评估：

- 实现效果：4/10。lowering 侧已经能消费多目标 indirect branch facts，但
  fortune 的 jump table facts 还没有恢复出来。
- 理解成本：1/10。已有 `BlockSuccessors` 继续作为唯一输入；多个目标用
  `switch` 表达。
- 维护成本：1/10。没有引入新的 CFG 状态。

## 当前边界：jump table facts 还没恢复

fortune 当前仍有一个 unresolved indirect branch：

```text
0x2879: notrack jmp rax
0x2872 block successors: []
unresolved indirect flows: [{ address: "0x2879", kind: "indirect branch" }]
```

对应汇编是 GCC switch table：

```text
2840: lea rbx,[0x63d8]
2866: sub eax,0x61
2869: cmp eax,0x16
286c: ja 0x2efe
2872: movsxd rax,DWORD PTR [rbx+rax*4]
2876: add rax,rbx
2879: notrack jmp rax
```

尝试过把 x86 jump table 识别塞进 `collectDirectControlFlow(...)` 的单次
p-code 扫描，但没有保留：这个位置只能看到一个小 seed 的 p-code，`RBX`
table base 来自前面 block 的 `LEA`，继续在这里补跨 seed 回溯会把 decode
阶段做成半截数据流分析。

下一步更合适的做法：

- 先让 instruction/block facts 稳定下来。
- 单独做一个 jump table facts pass，从已确认函数的 instruction facts、
  block facts 和必要的短距离数据流里恢复 `{branch, tableBase, targets}`。
- 再把恢复出的 targets 写回 block successors。
- `PcodeToLLVM` 不再需要新增特殊逻辑，直接消费多个 successors 并 lower 成
  `switch`。

## 已完成：条件分支窗口 false 边和后置 jump table facts

这一步修了两个 facts 层问题。

第一个问题是 0x2866 这种窗口末尾的条件跳转：

```text
2866: sub eax,0x61
2869: cmp eax,0x16
286c: ja 0x2efe
2872: movsxd rax,DWORD PTR [rbx+rax*4]
```

之前 block `0x2866..0x2872` 只有 true successor `0x2efe`，没有 false
successor `0x2872`。现在最后一条 instruction 如果是 conditional branch，
即使窗口里没有下一条 instruction，也把 `instruction.end()` 记录成
fallthrough。

第二个问题是 jump table 不应该塞进单个 seed 的 p-code 扫描。现在加了一个
后置 `X86JumpTableAnalyzer`，它在 Sleigh decode 之后读稳定的 instruction /
block facts，先只匹配 fortune 里的 x86-64 PIC 32-bit offset table 形态：

```text
LEA RBX,[0x63d8]
CMP EAX,0x16
MOVSXD RAX,dword ptr [RBX + RAX*0x4]
ADD RAX,RBX
JMP RAX
```

具体改动：

- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1664)
  - `annotateDecodedInstructionFlows(...)` 给窗口末尾的条件跳转补
    `instruction.end()` fallthrough。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:2720)
  - `NativeProgramState::addBasicBlock(...)` 对同 start/end 的重复 block
    合并 successors，不再直接丢掉新 facts。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:2763)
  - 新增 `addBasicBlockSuccessors(...)`，让后置 analyzer 可以只补边。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:2815)
  - 新增 `removeUnresolvedFlow(...)`，但当前只有完整恢复时才移除 unresolved。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:2411)
  - 新增 `X86JumpTableAnalyzer`。当前只把已经有 native block 的 jump table
    target 写回 successors；缺 block 的目标先不写，避免 lowering 跳过函数。
- [NativeAnalysis.h](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/NativeAnalysis.h:275)
  - 暴露上述两个状态更新方法和 `createX86JumpTableAnalyzer()`。
- [notdec-native-discover.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-discover.cpp:1378)
  - 在 Sleigh decode 后运行 `X86JumpTableAnalyzer`。
- [notdec-native-llvm.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:386)
  - native LLVM 主链路也运行该 analyzer。

当前 fortune 结果：

```text
block 0x2866 successors: ["0x2efe", "0x2872"]
block 0x2872 successors: ["0x2efe"]
unresolved indirect flows: still 1 at 0x2879
```

这里 unresolved 仍保留是有意的。表里其他 target，比如 `0x2970`，当前还没有
native block；如果直接写进 successors，`PcodeToLLVM` 会报
`native branch target 0x2970 is missing a native block` 并跳过 `0x2820`。
所以当前只补“已经确认有 block 的目标”，完整解决需要下一步让 jump table
facts 反向驱动二次 decode，把所有 table target 先 decode 成 block，再移除
unresolved。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm native_analysis_facts_test pcode_to_llvm_test native_register_summary_test native_register_summary_ssa_test -j2
/tmp/notdec-bin2llvm-build/bin/native_analysis_facts_test
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --block-json 0x2866 /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --block-json 0x2872 /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --unresolved-json /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
/usr/bin/time -f 'elapsed=%e' /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-jumptable-facts/fortune.ll --summary-json-out /tmp/notdec-fortune-jumptable-facts/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-jumptable-facts/fortune.ll -o /tmp/notdec-fortune-jumptable-facts/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-jumptable-facts/fortune.bc -o /tmp/notdec-fortune-jumptable-facts/fortune.verify.bc
```

结果：

```text
all native tests above: passed
fortune native pipeline: 9.83s
notdec_native_2820 is generated
llvm-as + opt verify: passed
```

复杂度评估：

- 实现效果：5/10。修复了条件 false 边，并把 jump table 恢复放到了正确阶段；
  但还没有二次 decode，所以不是完整 jump table 支持。
- 理解成本：3/10。新增一个 analyzer，但它只消费已有 facts，入口清晰。
- 维护成本：3/10。当前匹配规则偏窄，后续应扩展为“恢复 table targets 后驱动
  decode”的完整流程。

## 已完成：jump table targets 反向驱动 Sleigh decode

上一节的问题是后置 `X86JumpTableAnalyzer` 只能补已经存在的 block target。
如果 table 里有 `0x2970` 这种还没 decode 成 native block 的目标，直接写
successor 会让 `PcodeToLLVM` 报 missing native block，所以只能保留
unresolved。

这次改成两步：

- Sleigh seed decode 每处理完一个 seed，就查看当前函数里的 unresolved
  indirect branch。
- 如果能匹配 x86-64 PIC i32 offset jump table，就读取完整 table targets，
  把这些 target 作为同一函数的新 seed 入队。
- Sleigh 队列跑完后，后置 `X86JumpTableAnalyzer` 再写完整 successors；
  此时 target block 已经存在，可以安全清掉 unresolved。

具体改动：

- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:97)
  - 新增文件级 `parseHex(...)`、`readSigned32(...)` 和 `addUniqueAddress(...)`。
    这些 helper 现在同时服务 Sleigh decode 队列和后置 jump table analyzer。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1336)
  - 新增 `X86PicI32JumpDispatch` 和 x86 PIC i32 offset jump table 匹配 helper。
    匹配仍然只覆盖 fortune 当前形态：
    `LEA RBX,[table]`、`CMP EAX,max`、`MOVSXD RAX,[RBX+RAX*4]`、
    `ADD RAX,RBX`、`JMP RAX`。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1539)
  - `SleighSeedInstructionAnalyzer::run(...)` 在每个 seed decode 后调用
    `enqueueRecoveredJumpTableTargets(...)`。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1665)
  - 新增 `enqueueRecoveredJumpTableTargets(...)`，把恢复出的 table target
    按同一 `functionEntry` 入队继续 decode。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:2620)
  - `X86JumpTableAnalyzer` 删除重复匹配代码，改为复用同一组 helper。

当前 fortune 结果：

```text
block 0x2872 successors:
  ["0x2970", "0x2efe", "0x2963", "0x2956", "0x2949", "0x2940",
   "0x292a", "0x2913", "0x28f2", "0x28e5", "0x28cf", "0x28c5",
   "0x2886", "0x287c"]
unresolved indirect flows: 0
```

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm native_analysis_facts_test pcode_to_llvm_test native_register_summary_test native_register_summary_ssa_test -j2
/tmp/notdec-bin2llvm-build/bin/native_analysis_facts_test
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --unresolved-json /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
/usr/bin/time -f 'elapsed=%e' /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-jumptable-decode/fortune.ll --summary-json-out /tmp/notdec-fortune-jumptable-decode/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-jumptable-decode/fortune.ll -o /tmp/notdec-fortune-jumptable-decode/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-jumptable-decode/fortune.bc -o /tmp/notdec-fortune-jumptable-decode/fortune.verify.bc
```

结果：

```text
all native tests above: passed
fortune unresolved flow count: 0
fortune native pipeline: 11.59s
llvm-as + opt verify: passed
latest IR: /tmp/notdec-fortune-jumptable-decode/fortune.ll
```

性能说明：上一版 fortune 同口径是 9.83s，这次是 11.59s。变慢主要来自
jump table target 被实际 decode 成 block，属于预期成本；后续如果 Bench2
整体变慢明显，再考虑缓存 jump table 匹配结果或减少重复扫描 unresolved flows。

复杂度评估：

- 实现效果：7/10。fortune 这个 jump table 的 CFG 已经闭合，unresolved 为 0。
- 理解成本：4/10。Sleigh decode 多了一次“恢复 target 后入队”的反馈，但 lowering
  仍只消费已确认 CFG，没有新增特殊路径。
- 维护成本：4/10。当前 matcher 仍偏窄，只覆盖 fortune 里这类 GCC PIC table；
  后续扩展其他 table 形态时应继续复用同一组 helper。

## 已修正：重叠函数下 jump table successors 写错函数

继续检查 raw IR 时发现一个问题：`0x2820` 的 CFG facts 里
`0x2872` 已经有 14 个 jump table successors，但 raw lowering 里的
`notdec_native_2820` 仍然从 `bb_2872` 跳到 `notdec_exit`，没有生成
`switch`。

调试后确认根因不是 P-Code 丢失：

```text
0x2879: BRANCHIND (register,0x0,8)
```

真正的问题是 fortune 里当前还有重叠确认函数，比如 `0x27b2`、`0x2820`、
`0x3470` 的范围互相覆盖。旧的 `X86JumpTableAnalyzer` 用
`functionContaining(branchAddress)` 按 range 找第一个函数，所以 `0x2879`
先命中了 `0x27b2`，jump table successors 写到了 `notdec_native_27b2`；
真正需要的 `notdec_native_2820` 仍然是空 successors。

修正：

- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:2614)
  - `X86JumpTableAnalyzer::recoverJumpTableAt(...)` 不再只用
    `functionContaining(...)` 找一个函数。
  - 改为遍历所有 confirmed functions，只处理实际有 basic block 覆盖
    `branchAddress` 的函数。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:2630)
  - 新增 `recoverJumpTableInFunction(...)`，把 jump table successors 写回对应
    `function.Entry`。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:2665)
  - 新增 `functionHasBlockContaining(...)`，避免重叠 range 误命中。

当前结果：

```text
raw IR:
  define void @notdec_native_2820()
  bb_2872:
    switch i64 %RAX83, label %notdec_exit [...]

summary IR:
  define void @notdec_native_2820(...)
  bb_2872:
    switch i32 %unique_de00_4, label %notdec_exit [...]
```

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm native_analysis_facts_test pcode_to_llvm_test native_register_summary_test native_register_summary_ssa_test -j2
/tmp/notdec-bin2llvm-build/bin/native_analysis_facts_test
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --unresolved-json /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --no-register-ssa-pass --no-instcombine-pass -o /tmp/notdec-fortune-raw-jumptable/fortune.raw.ll --summary-json-out /tmp/notdec-fortune-raw-jumptable/summary.json
/usr/bin/time -f 'elapsed=%e' /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-jumptable-decode/fortune.ll --summary-json-out /tmp/notdec-fortune-jumptable-decode/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-jumptable-decode/fortune.ll -o /tmp/notdec-fortune-jumptable-decode/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-jumptable-decode/fortune.bc -o /tmp/notdec-fortune-jumptable-decode/fortune.verify.bc
```

结果：

```text
native tests: passed
fortune unresolved flow count: 0
fortune native pipeline: 11.55s
llvm-as + opt verify: passed
latest summary IR: /tmp/notdec-fortune-jumptable-decode/fortune.ll
latest raw IR: /tmp/notdec-fortune-raw-jumptable/fortune.raw.ll
```

复杂度评估：

- 实现效果：8/10。`notdec_native_2820` 的 jump table 现在真正进入 lowering，
  raw 和 summary IR 都有 switch。
- 理解成本：4/10。逻辑仍在 facts 层，只是从 range 命中改为 block 覆盖命中。
- 维护成本：4/10。重叠函数本身后续还要治理；这次修正避免 jump table 边写错
  函数，但不解决重叠函数来源问题。

## 已完成：同函数 decode 尊重已知函数范围

继续看 fortune 的重叠函数后，确认 `0x27b2`、`0x2820`、`0x3470`
这些入口的 seed 主要来自 `.eh_frame`。`.eh_frame` 给出的原始范围并不重叠：

```text
0x27b2: [0x27b2, 0x27f0)
0x2820: [0x2820, 0x31e3)
0x3470: [0x3470, 0x3eb0)
```

重叠是在 Sleigh seed decode 队列里产生的。旧逻辑只用 known range 限制
一次 seed decode 的字节数，但 direct branch / fallthrough / jump table target
仍然会作为同函数 seed 入队。如果 target 落在 `.eh_frame` range 外，下一轮
decode 会退回 `capBytesAtNextFunctionSeed(...)`，于是函数被拖到别的范围里。

这次改成：如果某个 function seed 有明确 `RangeStart/RangeEnd`，那么同函数
decode 只允许 range 内的 target 入队，block successors 也只保留 range 内
target。没有明确范围的 seed 暂时保持原行为，比如 fortune 的 `0x32d0` 来自
init-array / relocation，入口直接跳 `0x3250`，不能用这条规则收紧。

具体改动：

- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1529)
  - `SleighSeedInstructionAnalyzer::run(...)` 在入队 direct branch target 和
    fallthrough target 前调用 `targetBelongsToFunctionRange(...)`。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1697)
  - jump table target 反向驱动 decode 时也使用同一个 range 检查。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1939)
  - `addDecodedFunctionBlocks(...)` 在写 block successors 前调用
    `eraseOutOfRangeFunctionSuccessors(...)`，避免 lowering 看到没有 native block
    的 range 边界 target。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:2319)
  - 新增 `eraseOutOfRangeFunctionSuccessors(...)`。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:2369)
  - 新增 `targetBelongsToFunctionRange(...)`，只有 seed 有明确 range 时才生效。

当前 fortune 函数范围：

```text
0x27b2: range [0x27b2, 0x27c0), block_count 1
0x2820: range [0x2820, 0x31e3), block_count 234
0x3470: range [0x3470, 0x3eb0), block_count 199
unresolved indirect flows: 0
```

`0x2820` 的 jump table 仍然正确 lower：

```text
raw IR:
  define void @notdec_native_2820()
  bb_2872:
    switch i64 %RAX83, label %notdec_exit [...]

summary IR:
  define void @notdec_native_2820(...)
  bb_2872:
    switch i32 %unique_de00_4, label %notdec_exit [...]
```

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm native_analysis_facts_test pcode_to_llvm_test native_register_summary_test native_register_summary_ssa_test -j2
/tmp/notdec-bin2llvm-build/bin/native_analysis_facts_test
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --functions-json /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --cfg-json 0x2820 /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --unresolved-json /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --no-register-ssa-pass --no-instcombine-pass -o /tmp/notdec-fortune-raw-range-bounded/fortune.raw.ll --summary-json-out /tmp/notdec-fortune-raw-range-bounded/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-raw-range-bounded/fortune.raw.ll -o /tmp/notdec-fortune-raw-range-bounded/fortune.raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-raw-range-bounded/fortune.raw.bc -o /tmp/notdec-fortune-raw-range-bounded/fortune.raw.verify.bc
/usr/bin/time -f 'elapsed=%e' /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-range-bounded/fortune.ll --summary-json-out /tmp/notdec-fortune-range-bounded/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-range-bounded/fortune.ll -o /tmp/notdec-fortune-range-bounded/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-range-bounded/fortune.bc -o /tmp/notdec-fortune-range-bounded/fortune.verify.bc
```

结果：

```text
native tests: passed
fortune unresolved flow count: 0
fortune native pipeline: 10.13s
raw IR verify: passed
summary IR verify: passed
latest raw IR: /tmp/notdec-fortune-raw-range-bounded/fortune.raw.ll
latest summary IR: /tmp/notdec-fortune-range-bounded/fortune.ll
```

性能说明：上一版修 jump table overlap 后 fortune 是 11.55s，这次降到 10.13s。
主要原因是少 decode 了 `.eh_frame` range 外的错误同函数 block。

复杂度评估：

- 实现效果：8/10。`.eh_frame` 等强范围 seed 不再越界扩张，`0x2820` 的 switch
  仍然保留。
- 理解成本：4/10。新增的是单一范围检查，位置在 seed 入队和 successor 写入。
- 维护成本：4/10。无范围 seed 仍保留旧行为，后续需要单独处理 init-array /
  relocation 这类入口的函数边界。

## 已完成：Native xref facts 去重

继续看无明确 range 的 seed 时，发现另一个 facts 层噪声：同一条 xref 会因为
重复 decode 或多个入口覆盖被反复加入。比如 fortune 的 `function-xrefs-json`
里，同一个 `{from, to, kind, source}` 会重复出现很多次。这会污染 callgraph、
incoming/outgoing 统计，也让后续基于 xref 的判断更难看。

这次只在 `NativeProgramState::addXref(...)` 做完全相同 xref 去重：

- 比较字段：`From`、`To`、`Kind`、`Source`。
- 不合并不同 source 的 xref，保留不同 analyzer 的证据。
- 不改 decode、block、lowering 逻辑。

具体改动：

- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:3095)
  - `NativeProgramState::addXref(...)` 插入前先查 `XrefsByFrom[xref.From]`，
    已有完全相同 xref 时直接返回。

当前 fortune 结果：

```text
before:
  xrefs total: 1838
  flow: 985
  call: 565
  data: 283
  string: 5

after:
  xrefs total: 868
  flow: 438
  call: 279
  data: 146
  string: 5
```

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm native_analysis_facts_test pcode_to_llvm_test native_register_summary_test native_register_summary_ssa_test -j2
/tmp/notdec-bin2llvm-build/bin/native_analysis_facts_test
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --no-register-ssa-pass --no-instcombine-pass -o /tmp/notdec-fortune-raw-xref-dedup/fortune.raw.ll --summary-json-out /tmp/notdec-fortune-raw-xref-dedup/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-raw-xref-dedup/fortune.raw.ll -o /tmp/notdec-fortune-raw-xref-dedup/fortune.raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-raw-xref-dedup/fortune.raw.bc -o /tmp/notdec-fortune-raw-xref-dedup/fortune.raw.verify.bc
/usr/bin/time -f 'elapsed=%e' /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-xref-dedup/fortune.ll --summary-json-out /tmp/notdec-fortune-xref-dedup/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-xref-dedup/fortune.ll -o /tmp/notdec-fortune-xref-dedup/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-xref-dedup/fortune.bc -o /tmp/notdec-fortune-xref-dedup/fortune.verify.bc
```

结果：

```text
native tests: passed
fortune unresolved flow count: 0
fortune native pipeline: 10.15s
raw IR verify: passed
summary IR verify: passed
latest raw IR: /tmp/notdec-fortune-raw-xref-dedup/fortune.raw.ll
latest summary IR: /tmp/notdec-fortune-xref-dedup/fortune.ll
```

复杂度评估：

- 实现效果：7/10。xref 噪声明显下降，callgraph / xref 查询更接近 facts 本身。
- 理解成本：2/10。只是插入前去重。
- 维护成本：2/10。保留 source 维度，不会把不同来源的证据合并掉。

## 已完成：direct CALL xref source 与 flow 区分

xref 去重后继续看 `callgraph-json`，发现直接 CALL 的 xref kind 已经是
`call`，但 source 仍写成 `sleigh-pcode-direct-flow`。这会让调用边和普通
BRANCH/CBRANCH 边在 source 上混在一起，不利于后续基于 facts 的判断。

这次只改 direct CALL 的 source 名：

- direct CALL: `sleigh-pcode-direct-call`
- direct BRANCH/CBRANCH: 继续使用 `sleigh-pcode-direct-flow`
- PLT call、GOT indirect call 的 source 不变。

具体改动：

- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:2000)
  - `collectDirectControlFlow(...)` 在 `PcodeOpcode::Call` 且非 PLT 的路径里，
    把 xref source 从 `sleigh-pcode-direct-flow` 改为
    `sleigh-pcode-direct-call`。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm native_analysis_facts_test pcode_to_llvm_test native_register_summary_test native_register_summary_ssa_test -j2
/tmp/notdec-bin2llvm-build/bin/native_analysis_facts_test
/tmp/notdec-bin2llvm-build/bin/pcode_to_llvm_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-kind-json call /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --no-register-ssa-pass --no-instcombine-pass -o /tmp/notdec-fortune-raw-direct-call-source/fortune.raw.ll --summary-json-out /tmp/notdec-fortune-raw-direct-call-source/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-raw-direct-call-source/fortune.raw.ll -o /tmp/notdec-fortune-raw-direct-call-source/fortune.raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-raw-direct-call-source/fortune.raw.bc -o /tmp/notdec-fortune-raw-direct-call-source/fortune.raw.verify.bc
/usr/bin/time -f 'elapsed=%e' /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed -o /tmp/notdec-fortune-direct-call-source/fortune.ll --summary-json-out /tmp/notdec-fortune-direct-call-source/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-direct-call-source/fortune.ll -o /tmp/notdec-fortune-direct-call-source/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-direct-call-source/fortune.bc -o /tmp/notdec-fortune-direct-call-source/fortune.verify.bc
```

结果：

```text
native tests: passed
fortune unresolved flow count: 0
fortune native pipeline: 10.10s
raw IR verify: passed
summary IR verify: passed
latest raw IR: /tmp/notdec-fortune-raw-direct-call-source/fortune.raw.ll
latest summary IR: /tmp/notdec-fortune-direct-call-source/fortune.ll
```

复杂度评估：

- 实现效果：5/10。行为不变，但 call/flow facts 更清楚。
- 理解成本：1/10。只改 source 字符串。
- 维护成本：1/10。source 语义更明确。

## 已完成：动态数组入口 thunk 的 tail branch 建模

继续看 fortune 的 `0x32d0` 时，发现它来自 `.init_array`：

```text
0x32d0: endbr64
0x32d4: jmp 0x3250
```

之前 decode 会把 `0x3250` 的主体块并进 `notdec_native_32d0`，导致
`notdec_native_32d0` 的函数范围变成 `[0x3250, 0x32e0)`。这不是简单的空块问题，
而是动态数组入口 thunk 被当成普通同函数跳转处理了。

这次只对很窄的场景做处理：

- seed 没有 `.eh_frame` / symbol 之类明确 range。
- seed 来源包含 `dt-init-array` 或 `dt-fini-array`。
- 当前指令是 direct unconditional branch。
- branch target 小于当前 function entry。

满足这些条件时，把目标记录为 `sleigh-tail-branch` seed，并从当前函数的
`DirectFlowTargets` 中移走。这样当前函数不再吸收目标函数体，lowering 仍能用已有
tail-call 表达。

具体改动：

- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1531)
  - `SleighSeedInstructionAnalyzer::run(...)` 消费 `DecodeSeedResult::TailBranchTargets`，
    为目标添加 `sleigh-tail-branch` function seed 并入队 decode。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1581)
  - `DecodeSeedResult` 增加 `TailBranchTargets`。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1757)
  - `decodeSeed(...)` 在 reachable 计算前调用 `collectTailBranchTargets(...)`，
    先把 tail branch 从本函数 CFG 边里切出去。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1798)
  - 新增 `collectTailBranchTargets(...)`、`isTailBranchTarget(...)`、
    `isDynamicArrayThunkSeed(...)`。

fortune 结果：

```text
function_seeds: 26
confirmed_functions: 26
basic_blocks: 1013
instructions: 2602
xrefs total: 868
unresolved_indirect_flows: 0
sleigh-tail-branch: 1
```

`0x32d0` 现在是独立 thunk：

```text
notdec_native_32d0 range: [0x32d0, 0x32d9)
notdec_native_3250 range: [0x3250, 0x3289)
```

raw IR 中对应为：

```llvm
define void @notdec_native_32d0() {
bb_32d0:
  tail call void @notdec_native_3250()
  ret void
}
```

summary IR 也生成 tail call。当前 signature rewrite 会给部分 internal tail call
补 `undef` 参数，例如 `notdec_native_32d0 -> notdec_native_3250`。这次抽样中
`notdec_native_3250` 的这些参数没有实际使用，所以不影响当前语义；但这属于
signature rewrite 后续需要清理的点。

验证：

```text
cmake --build build -j$(nproc)
build/bin/native_analysis_facts_test
build/bin/pcode_to_llvm_test
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --no-register-ssa-pass --no-instcombine-pass --summary-json-out /tmp/notdec-fortune-tail-thunk-raw/summary.json -o /tmp/notdec-fortune-tail-thunk-raw/fortune.raw.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-tail-thunk-raw/fortune.raw.ll -o /tmp/notdec-fortune-tail-thunk-raw/fortune.raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-tail-thunk-raw/fortune.raw.bc -o /tmp/notdec-fortune-tail-thunk-raw/fortune.raw.verified.bc
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --summary-json-out /sn640/NotDec-Exp/Bench2/bin2llvm-native-projects/selected-targets-native/fortune-executable/summary.json -o /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/fortune/executable/module-all.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/fortune/executable/module-all.ll -o /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/fortune/executable/module-all.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/fortune/executable/module-all.bc -o /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/fortune/executable/module-all.verified.bc
```

结果：

```text
native tests: passed
raw IR verify: passed
summary IR verify: passed
fortune native pipeline: 10.16s
latest raw IR: /tmp/notdec-fortune-tail-thunk-raw/fortune.raw.ll
latest summary IR: /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/fortune/executable/module-all.ll
```

性能说明：上一版 fortune 是 10.10s 左右，这次是 10.16s，同口径没有明显退化。

复杂度评估：

- 实现效果：7/10。修掉了 `.init_array` 入口 thunk 吸收目标函数体的问题，同时没有
  增加基本块数量。
- 理解成本：4/10。新增了一类 tail branch seed，但只在 decode facts 层内部使用。
- 维护成本：4/10。规则刻意保守，后续如果遇到更多 thunk 形态，再扩展匹配条件。

## 已完成：全空 native block 也按 block facts lowering

继续检查 native lowering 对 block facts 的依赖时，发现还有一个旧兜底：

- `PcodeLowerer::lower(...)` 遇到 `program.Ops.empty()` 直接返回。
- `buildBasicBlocks(...)` 在 native mode 下要求至少有一个 p-code op 被 block range
  覆盖。
- `notdec-native-llvm` 遇到 empty p-code 会跳过 confirmed function。

这和当前方向不一致。native 前端已经有明确的 `NativeBasicBlock` facts，即使某个
block 没有 SLEIGH p-code，也应该能生成对应 LLVM block，并按 `BlockSuccessors`
决定是跳转还是 `ret void`。之前只支持“部分 block 没有 p-code”，这次补上
“整个函数都没有 p-code”的情况。

具体改动：

- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:63)
  - `PcodeLowerer::lower(...)` 只在非 native CFG 模式下对空 p-code 直接返回。
    native CFG 模式继续进入 `buildBasicBlocks(...)`，让 block facts 驱动 lowering。
- [PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:310)
  - `buildBasicBlocks(...)` 允许 `starts` 为空，只要已经收集到
    `EmptyNativeBlockAddresses`。
- [notdec-native-llvm.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:750)
  - `--all-confirmed` 模式下，只有 confirmed function 没有 blocks 时才因为 empty
    p-code 跳过。
- [notdec-native-llvm.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:1009)
  - 单函数 lowering 下，只有没有 native block ranges 时才把 empty p-code 当失败。
- [pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:432)
  - 增加 `testAllEmptyNativeBlockCanLower()`，验证没有任何 p-code op 时，native
    block facts 仍能生成 `bb_1000` 并通过 LLVM verifier。

验证：

```text
cmake --build build -j$(nproc)
build/bin/native_analysis_facts_test
build/bin/pcode_to_llvm_test
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --no-register-ssa-pass --no-instcombine-pass --summary-json-out /tmp/notdec-fortune-all-empty-native-block/raw-summary.json -o /tmp/notdec-fortune-all-empty-native-block/fortune.raw.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-all-empty-native-block/fortune.raw.ll -o /tmp/notdec-fortune-all-empty-native-block/fortune.raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-all-empty-native-block/fortune.raw.bc -o /tmp/notdec-fortune-all-empty-native-block/fortune.raw.verified.bc
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --summary-json-out /tmp/notdec-fortune-all-empty-native-block/summary.json -o /tmp/notdec-fortune-all-empty-native-block/fortune.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-all-empty-native-block/fortune.ll -o /tmp/notdec-fortune-all-empty-native-block/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-all-empty-native-block/fortune.bc -o /tmp/notdec-fortune-all-empty-native-block/fortune.verified.bc
```

fortune 结果：

```text
function_seeds: 26
confirmed_functions: 26
basic_blocks: 1013
instructions: 2602
xrefs total: 868
unresolved_indirect_flows: 0
native pipeline: 10.06s
```

当前 fortune 没有触发全空函数行为，所以 facts 数量不变。这个改动主要是去掉一个
lowering 层对 p-code 是否存在的硬依赖。

复杂度评估：

- 实现效果：6/10。补齐了 native block facts 驱动 lowering 的一个边界情况。
- 理解成本：2/10。沿用已有 empty native block 机制，没有引入新 CFG 规则。
- 维护成本：2/10。逻辑更一致，后续遇到无 p-code 的真实 block 不会被跳过。

## 已完成：tail branch 指令 fact 也单独保留

上一轮把 `.init_array` thunk 的 `JMP 0x3250` 从当前函数 CFG successor 里切掉了，
这样 block facts 是对的，但 `--instructions-json` 里这条指令会显得像“没有目标”。
这会让 instruction facts 不完整。

这次只补一个很小的事实字段：

- `NativeInstruction` 新增 `TailFlowTargets`。
- 当 direct branch 被判成 tail branch 时，目标地址同时保留在
  `tail_flow_targets`，但不再进入 `direct_flow_targets`。
- block 构建仍然只看 `direct_flow_targets`，所以 CFG 不受影响。
- `notdec-native-discover --instructions-*` 现在能直接看出这条机器指令的真实 tail
  target。

具体改动：

- [NativeAnalysis.h](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/NativeAnalysis.h:205)
  - `NativeInstruction` 增加 `TailFlowTargets`。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1802)
  - `collectTailBranchTargets(...)` 在切出 tail branch 的同时，把目标写入
    `instruction.TailFlowTargets`。
- [notdec-native-discover.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-discover.cpp:1220)
  - instruction JSON 输出新增 `tail_flow_targets`。

验证：

```text
build/bin/native_analysis_facts_test
build/bin/pcode_to_llvm_test
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
build/bin/notdec-native-discover --instructions-function-json 0x32d0 /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --no-register-ssa-pass --no-instcombine-pass --summary-json-out /tmp/notdec-fortune-tail-flow-facts/raw-summary.json -o /tmp/notdec-fortune-tail-flow-facts/fortune.raw.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-tail-flow-facts/fortune.raw.ll -o /tmp/notdec-fortune-tail-flow-facts/fortune.raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-tail-flow-facts/fortune.raw.bc -o /tmp/notdec-fortune-tail-flow-facts/fortune.raw.verified.bc
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --summary-json-out /tmp/notdec-fortune-tail-flow-facts/summary.json -o /tmp/notdec-fortune-tail-flow-facts/fortune.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-tail-flow-facts/fortune.ll -o /tmp/notdec-fortune-tail-flow-facts/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-tail-flow-facts/fortune.bc -o /tmp/notdec-fortune-tail-flow-facts/fortune.verified.bc
```

fortune 的 instruction facts 现在是：

```text
0x32d4:
  flow_kind: unconditional branch
  direct_flow_targets: []
  tail_flow_targets: ["0x3250"]
```

而 summary / verifier 都仍然通过，fortune 运行时间仍在 10 秒左右。

复杂度评估：

- 实现效果：6/10。保持 CFG 语义不变，同时把 instruction facts 补完整。
- 理解成本：2/10。字段名直接，和 CFG successor 分开。
- 维护成本：2/10。后续碰到更多 tail thunk 形态时，还是同一套字段。

## 已完成：jump table 目标同步回 instruction facts

继续检查 `notdec_native_2820` 的 switch 时，发现后置 jump table analyzer 已经把
`0x2879` 的 14 个 targets 写进 block successors，也加了 `x86-jump-table` xrefs，
但 `--instructions-function-json 0x2820` 里 `0x2879` 这条 `JMP RAX` 的
`direct_flow_targets` 仍然是空的。

这会造成 instruction facts 和 block facts 不一致。当前目标是
instruction facts -> block facts -> lowering，所以后置恢复出的控制流也要回填到
对应 instruction fact。

具体改动：

- [NativeAnalysis.h](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/NativeAnalysis.h:282)
  - `NativeProgramState` 增加 `addInstructionDirectFlowTargets(...)`。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:2745)
  - `X86JumpTableAnalyzer::recoverJumpTableInFunction(...)` 在更新 block successors 后，
    同步更新 `branchAddress` 对应 instruction 的 `DirectFlowTargets`。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:3152)
  - `NativeProgramState::addInstructionDirectFlowTargets(...)` 只追加去重 targets，
    不覆盖原始 decode 得到的 flow targets。

验证：

```text
build/bin/notdec-native-discover --instructions-function-json 0x2820 /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
build/bin/native_analysis_facts_test
build/bin/pcode_to_llvm_test
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --no-register-ssa-pass --no-instcombine-pass --summary-json-out /tmp/notdec-fortune-jumptable-instruction-facts/raw-summary.json -o /tmp/notdec-fortune-jumptable-instruction-facts/fortune.raw.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-jumptable-instruction-facts/fortune.raw.ll -o /tmp/notdec-fortune-jumptable-instruction-facts/fortune.raw.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-jumptable-instruction-facts/fortune.raw.bc -o /tmp/notdec-fortune-jumptable-instruction-facts/fortune.raw.verified.bc
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --summary-json-out /tmp/notdec-fortune-jumptable-instruction-facts/summary.json -o /tmp/notdec-fortune-jumptable-instruction-facts/fortune.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-jumptable-instruction-facts/fortune.ll -o /tmp/notdec-fortune-jumptable-instruction-facts/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-jumptable-instruction-facts/fortune.bc -o /tmp/notdec-fortune-jumptable-instruction-facts/fortune.verified.bc
```

fortune 的 `0x2879` instruction fact 现在是：

```text
flow_kind: indirect branch
direct_flow_targets:
  0x2970, 0x2efe, 0x2963, 0x2956, 0x2949, 0x2940, 0x292a,
  0x2913, 0x28f2, 0x28e5, 0x28cf, 0x28c5, 0x2886, 0x287c
```

fortune 结果：

```text
function_seeds: 26
confirmed_functions: 26
basic_blocks: 1013
instructions: 2602
xrefs total: 868
unresolved_indirect_flows: 0
native pipeline: 10.20s
```

复杂度评估：

- 实现效果：7/10。jump table 恢复结果现在同时体现在 instruction facts 和 block
  facts 中。
- 理解成本：2/10。只是把后置恢复出的 targets 回填到已有字段。
- 维护成本：2/10。后续其他后置 flow analyzer 也可以复用同一个 helper。

## 已完成：direct tail branch 写入 instruction facts

继续检查 native decode 到 lowering 的边界时，发现 direct branch 到 PLT 时只生成了
flow xref，没有写入 `DecodedFlowInfo::BranchTargets`。lowering 还能靠
`ExternalCallTargets` 生成 tail call，但 instruction facts 自己不完整。

这次保持 block CFG 的语义不变：block successor 仍只保留函数内边；跳到 PLT 或已知其他
函数入口的无条件 direct branch 只标到 instruction 的 `tail_flow_targets`。

具体改动：

- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1823)
  - `SleighSeedInstructionAnalyzer::isTailBranchTarget(...)` 增加两类 tail
    目标：PLT external target、已知其他函数入口。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:2097)
  - `collectDirectControlFlow(...)` 遇到 direct branch 到 PLT 时，也把 target 写入
    `DecodedFlowInfo::BranchTargets`，后续再由 `collectTailBranchTargets(...)`
    挪到 `TailFlowTargets`。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:2412)
  - `isKnownOtherFunctionEntry(...)` 改为接收 const state；函数本身不修改 state。

验证：

```text
cmake --build build -j$(nproc)
build/bin/native_analysis_facts_test
build/bin/pcode_to_llvm_test
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --summary-json-out /tmp/notdec-fortune-tail-facts/summary.json -o /tmp/notdec-fortune-tail-facts/fortune.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-tail-facts/fortune.ll -o /tmp/notdec-fortune-tail-facts/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-tail-facts/fortune.bc -o /tmp/notdec-fortune-tail-facts/fortune.verified.bc
```

fortune 结果：

```text
function_seeds: 26
confirmed_functions: 26
basic_blocks: 1013
instructions: 2602
xrefs total: 868
unresolved_indirect_flows: 0
native pipeline: 10.11s
```

抽查：

```text
0x32d4: JMP 0x3250
  direct_flow_targets: []
  tail_flow_targets: ["0x3250"]

0x5258: JMP 0x280c
  direct_flow_targets: []
  tail_flow_targets: ["0x280c"]
```

复杂度评估：

- 实现效果：6/10。tail branch 的 instruction facts 更完整，但 lowering 仍通过
  `PcodeLoweringConfig` 消费这些事实。
- 理解成本：2/10。规则和现有 dynamic array thunk tail branch 复用同一字段。
- 维护成本：2/10。后续如果更多 tail 形态被识别，也只需要扩展同一个判断。

## 已完成：block successor 不再凭 p-code 位置猜边

继续对比 fortune 的 `instructions-json` 和 `blocks-json` 时，发现 198 个 block
successor 不能从最后一条 instruction fact 推出来。大部分是 seed 窗口被截断后继续解码的
fallthrough：block 已经连到下一个 seed，但最后一条 instruction 没有记录这个
fallthrough。

还发现一个真正的 CFG 错边：`0x3250` 里的 guarded indirect tail jump：

```text
0x327e: JZ 0x3288
0x3280: JMP RAX
0x3288: RET
```

旧逻辑把 `0x3280` block 的 successor 设成 `0x3288`。这是错的，`JMP RAX` 没有
fallthrough，后面的 `RET` 只是 guard 失败路径。根因是 `buildDecodedBlocks(...)` 和
`NativeProgramState::addBasicBlock(...)` 在拆块时只看到“下一条指令是 block 起点”，就补了一条
successor，没有确认 instruction fact 里真的有 fallthrough。

具体改动：

- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1786)
  - `SleighSeedInstructionAnalyzer::decodeSeed(...)` 先计算 seed 末尾
    `FallthroughTarget`，再把它写回 `decodedInstructions.back().Fallthrough`，最后再
    `state.addInstruction(...)`。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:2366)
  - `buildDecodedBlocks(...)` 只有普通指令被切块时才自动补下一块 successor；terminator
    不再因为 `successors.empty()` 被接到下一块。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:3093)
  - `NativeProgramState::addBasicBlock(...)` 拆已有 block 时，只有 instruction facts 里存在
    `Fallthrough == splitAddress`，才补 split successor。空 successor 也可能表示 return 或
    indirect tail exit，不能直接解释成“继续执行下一块”。

验证：

```text
cmake --build build -j$(nproc)
build/bin/native_analysis_facts_test
build/bin/pcode_to_llvm_test
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --summary-json-out /tmp/notdec-fortune-flow-consistency/summary.json -o /tmp/notdec-fortune-flow-consistency/fortune.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-flow-consistency/fortune.ll -o /tmp/notdec-fortune-flow-consistency/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-flow-consistency/fortune.bc -o /tmp/notdec-fortune-flow-consistency/fortune.verified.bc
```

fortune 结果：

```text
function_seeds: 26
confirmed_functions: 26
basic_blocks: 1013
instructions: 2602
xrefs total: 868
unresolved_indirect_flows: 0
native pipeline: 10.23s
```

额外一致性检查：从 `instructions-json` 的最后一条 instruction fact 推导 block
successor，fortune 上 `missing_from_instruction_facts` 从 198 降到 0。

试错结论：不能简单把“跳出当前函数范围的无条件跳转”全部标为 tail branch。这个规则会让
fortune 从 26 个函数变成 29 个函数，并出现 `native conditional block 0x3884 is missing
true successor 0x38cd`。tail branch 仍需要 PLT、已知函数入口、thunk 形态等强证据。

复杂度评估：

- 实现效果：7/10。block successor 不再从 p-code 位置硬猜，和 instruction facts 的关系更清楚。
- 理解成本：3/10。`addBasicBlock(...)` 多了一个小 helper，但它直接表达“拆块必须有
  fallthrough 证据”。
- 维护成本：2/10。后续新增 flow analyzer 仍然只需要维护 instruction facts，block facts 从这些事实导出。

## 已完成：已知函数末尾不再生成续解码 fallthrough

上一轮 consistency 检查后还剩 15 个 extra instruction edges。抽查其中一个：

```text
0x2815: MOV RDI,qword ptr [RBX + 0x20]
0x2819: CALL 0x27a0
```

`0x2815` 的 eh-frame seed 范围正好到 `0x281e`。旧逻辑看到 decode 撞到 byte limit，
就把 `0x281e` 当成下一段 fallthrough 写进 instruction fact；但 `0x281e` 已经是已知
函数末尾，不是可执行 successor，block CFG 后面又把它按范围过滤掉了。

具体改动：

- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:1984)
  - `fallthroughTargetForDecodedWindow(...)` 增加 seed 边界检查：如果 `rangeEnd` 等于已知
    seed 的 `RangeEnd`，不生成续解码 fallthrough。

验证：

```text
cmake --build build -j$(nproc)
build/bin/native_analysis_facts_test
build/bin/pcode_to_llvm_test
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --summary-json-out /tmp/notdec-fortune-seed-end-fallthrough/summary.json -o /tmp/notdec-fortune-seed-end-fallthrough/fortune.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-seed-end-fallthrough/fortune.ll -o /tmp/notdec-fortune-seed-end-fallthrough/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-seed-end-fallthrough/fortune.bc -o /tmp/notdec-fortune-seed-end-fallthrough/fortune.verified.bc
```

fortune 结果：

```text
function_seeds: 26
confirmed_functions: 26
basic_blocks: 1013
instructions: 2602
xrefs total: 868
unresolved_indirect_flows: 0
native pipeline: 10.16s
```

一致性检查：

```text
missing_from_instruction_facts: 0
extra_instruction_edges: 10
```

剩余 10 个 extra 主要是跨函数或跨范围的 instruction edge，例如 `0x27bb: JMP 0x38cd`。
这类不是 seed 末尾 fallthrough 问题，后续应该在函数确认后做一轮 post-analysis facts
归一化，不能在 seed decode 阶段只靠当前已知函数表判断。

复杂度评估：

- 实现效果：6/10。消除了函数末尾假 fallthrough，fortune 指标稳定。
- 理解成本：1/10。只是在已有 fallthrough 判断里加已知 seed 末尾检查。
- 维护成本：1/10。规则直接对应 eh-frame/symbol range 的边界语义。

## 已完成：flow facts 后置归一化和 block 覆盖补洞

继续检查剩余 extra instruction edges，发现还有两类问题：

- 条件或无条件 direct branch 的目标是真实机器目标，但不是当前函数 CFG successor。例如
  `0x3950: JZ 0x27b2`，true 分支跳到另一个函数入口，false 分支继续当前函数。
- `0x2aa1..0x2aa6` 有 decoded instructions，但没有任何 basic block 覆盖，导致
  `0x2a9f` 的 fallthrough 指到一个不存在的 block。

这类信息必须等函数、jump table 和 block 都确认后才能判断，所以加了一个后置 analyzer，
放在 `X86JumpTableAnalyzer` 之后。

具体改动：

- [NativeAnalysis.h](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/NativeAnalysis.h:284)
  - `NativeProgramState` 增加 `markInstructionTailFlowTarget(...)`，只把已有 direct target
    移到 tail target，不新增控制流。
- [NativeAnalysis.h](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/NativeAnalysis.h:357)
  - 增加 `createFlowFactNormalizer()` factory。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:2789)
  - 新增 `FlowFactNormalizer`。
  - 第一阶段补 block 覆盖洞：找函数范围内已解码但未被 block 覆盖的连续指令，生成
    `NativeBasicBlock`。
  - 第二阶段归一化 successor：普通指令 fallthrough 指向同函数已有 block 时补 successor；
    block 末尾 instruction 的 direct target 如果不在 block successor 中，则移动到
    `TailFlowTargets`。
- [NativeAnalysis.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/NativeAnalysis.cpp:3359)
  - 实现 `NativeProgramState::markInstructionTailFlowTarget(...)`。
- [notdec-native-discover.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-discover.cpp:1382)
  - native discovery pipeline 在 `X86JumpTableAnalyzer` 后运行 `FlowFactNormalizer`。
- [notdec-native-llvm.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:387)
  - native LLVM pipeline 同步运行 `FlowFactNormalizer`。
- [native_analysis_facts_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_analysis_facts_test.cpp:81)
  - 增加 `testFlowNormalizerMovesNonCfgTargetToTail(...)`，覆盖非当前 CFG successor 的 direct
    target 被转成 tail target。
- [native_analysis_facts_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_analysis_facts_test.cpp:128)
  - 增加 `testFlowNormalizerFillsDecodedBlockHole(...)`，覆盖已解码但未被 block 覆盖的指令段会补成 block。

验证：

```text
cmake --build build -j$(nproc)
build/bin/native_analysis_facts_test
build/bin/pcode_to_llvm_test
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --summary-json-out /tmp/notdec-fortune-flow-normalizer-test/summary.json -o /tmp/notdec-fortune-flow-normalizer-test/fortune.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-flow-normalizer-test/fortune.ll -o /tmp/notdec-fortune-flow-normalizer-test/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-flow-normalizer-test/fortune.bc -o /tmp/notdec-fortune-flow-normalizer-test/fortune.verified.bc
```

fortune 结果：

```text
function_seeds: 26
confirmed_functions: 26
basic_blocks: 1016
instructions: 2602
xrefs total: 868
unresolved_indirect_flows: 0
native pipeline: 10.23s / 10.69s / 10.89s / 10.61s observed
```

一致性检查：

```text
uncovered_instructions: 0
missing_from_instruction_facts: 0
extra_instruction_edges: 0
```

复杂度评估：

- 实现效果：8/10。fortune 上 decoded instruction、block coverage、flow facts 三者一致。
- 理解成本：4/10。多了一个后置 analyzer，但职责清楚：不 decode，只整理 facts。
- 维护成本：3/10。当前 coverage 查询是线性扫描，真实大二进制上后续可能需要区间索引优化。

## 计划：native lowering 支持指令内部 p-code 控制流

继续看 fortune 的 lowered IR，发现 `0x2aad: CMOVNZ R8,RAX` 被降成：

```text
bb_2aad:
  br i1 poison, label %notdec_exit, label %bb_2ab1
```

低层 p-code 是：

```text
CBRANCH (ram,0x2ab1,8) <cond>
(register,0x80,8) = COPY <old RAX>
```

这不是机器级条件跳转，而是单条 `CMOVNZ` 指令内部的“条件跳过 COPY”。当前
`PcodeToLLVM` 把所有 p-code `CBRANCH/BRANCH` 都当成 LLVM basic block terminator，导致
`COPY` 被丢掉，语义错误。

目标：

- 机器级 CFG 继续只来自 native instruction/block facts。
- `PcodeToLLVM` 在 native block 内允许 p-code 内部控制流，不能把这类 `CBRANCH` 解释成函数
  CFG 边。
- 对 `CMOV` 这类条件执行指令，至少不能把条件写错成无条件或直接跳 exit。

技术路线：

- 在 native lowering 的 `buildBasicBlocks(...)` 里，保留 native block start/end，同时为
  native block 内部的 p-code branch target 和 fallthrough continuation 增加内部 block start。
- `CBRANCH` 如果目标地址是当前 native block end 或当前 native block 内地址，false 分支应指向
  下一条 p-code op 的内部 block，而不是 `exitBlock()`。
- `BRANCH` 如果目标是当前 native block 内部地址，降成内部 block 跳转；如果目标是 native block
  外部，仍按现有机器 CFG/tail call 规则处理。
- native block 的最终 successor 仍通过 `BlockSuccessors` 校验，不能重新退回 p-code 顺序猜函数 CFG。

风险：

- 这个改动会让一个 native basic block 对应多个 LLVM basic block，后续 summary SSA 要继续依赖
  LLVM CFG 正常工作。
- 需要避免把真正的机器条件分支也当成指令内部控制流；判断边界必须基于 native block range 和
  successor facts，而不是单看 p-code opcode。

判断标准：

- fortune 的 `bb_2aad` 不再跳到 `notdec_exit`，且 `CMOVNZ` 的 COPY 路径保留下来。
- `pcode_to_llvm_test` 增加覆盖 native block 内部 CBRANCH 的用例。
- fortune 仍能 `llvm-as` 和 `opt -passes=verify`，summary 链路仍通过核心测试。

实现记录：

- [lib/PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:81)
  在 native mode 下区分 native block 和 instruction-internal p-code block。内部 p-code
  block 不清空 `Values`，否则同一条机器指令前面算出的 `unique` 临时值会在后续内部 block
  里变成 poison。
- [lib/PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:294)
  增加 `addNativeBlockStart(...)`，记录 LLVM block 对应的显示地址、父 native block 地址、
  native block end，以及是否是内部 p-code block。
- [lib/PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:341)
  在 `buildBasicBlocks(...)` 中为 native block 内 terminator 的 continuation 和同块 target
  增加内部 block start。机器级 CFG 仍来自 native block facts。
- [lib/PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:455)
  `blockEndForStart(...)` 对内部 block 使用父 native block 的地址范围，修复同一条机器指令里
  `CBRANCH` 后续 p-code 仍是同地址时被误判为空 block 的问题。
- [lib/PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:506)
  增加 `internalPcodeContinuation(...)`，`CBRANCH` 的 false path 优先跳到同 native block 内的
  p-code continuation。
- [tests/pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:665)
  增加 `testNativeInternalConditionalKeepsSkippedPcode()`，覆盖 `CBRANCH` 跳过同一 native block
  内 p-code 的情况，确保内部 block 不是空跳转。

验证：

```text
cmake --build build -j$(nproc)
build/bin/pcode_to_llvm_test
build/bin/native_analysis_facts_test
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  --all-confirmed --summary-json-out /tmp/notdec-fortune-internal-pcode/summary.json \
  -o /tmp/notdec-fortune-internal-pcode/fortune.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-internal-pcode/fortune.ll \
  -o /tmp/notdec-fortune-internal-pcode/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-fortune-internal-pcode/fortune.bc \
  -o /tmp/notdec-fortune-internal-pcode/fortune.verified.bc
```

结果：

- 核心测试通过。
- fortune 默认 summary 链路通过 `llvm-as` 和 verifier，耗时 `10.62 sec`。
- 用 `--no-register-ssa-pass --no-instcombine-pass` 看原始 lowering，`0x2aad: CMOVNZ R8,RAX`
  已经变成：

```llvm
bb_2aad:
  br i1 %816, label %bb_2ab1, label %bb_2aad2

bb_2aad2:
  store i64 %unique_39b00_8401, ptr @R8, align 4, !notdec.register.access !106
  br label %bb_2ab1
```

后续观察：

- 最终 summary SSA 后 `bb_2aad2` 变空，是因为 R8 的 store 被寄存器消除链路判断为无后续使用。
- 最终 IR 里还有部分条件分支变成 `poison`，这是 summary/flag 处理的后续问题，不是这次
  p-code 内部控制流切块的问题。

评分：

- 实现效果：8/10。原始 lowering 语义修正，fortune 和测试通过。
- 理解成本：5/10。`PcodeToLLVM` 现在允许一个 native block 拆成多个 LLVM block，需要记住
  “父 native block”和“内部 p-code block”的区别。
- 维护成本：4/10。逻辑仍集中在 lowering 层，后续如果支持更复杂的 p-code 内部 merge，可能需要
  给 `unique` 临时值补 PHI。

## 实现记录：SummarySSA unknown 值和 CFG 清理

背景：

- 修完 native lowering 的内部 p-code 控制流后，fortune 默认输出仍有大量 `br i1 poison`。
- 对比 `--no-register-ssa-pass` 的原始 lowering，`0x2aad: CMOVNZ R8,RAX` 的条件和值都正常；
  问题主要来自 summary SSA 用裸 `undef` 表示 unknown register value，后续 instcombine 会把
  相关条件折成 `poison`。
- 修掉裸 `undef` 后，剩余少量 `br i1 poison` 位于常量分支保留下来的死边/死块里，单独跑
  `simplifycfg` 可以清掉。

实现：

- [lib/passes/summary/NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:119)
  增加 `frozenPoisonBefore(...)` 和 `frozenPoisonAt(...)`，统一用 `freeze poison` 表示
  summary SSA 自己制造的 unknown 值，避免裸 `undef` 直接进入 branch/call/return。
- [lib/passes/summary/NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:1159)
  `completePhi(...)` 在某个 predecessor 的寄存器值无法确定时，在该 predecessor terminator
  前插入 `freeze poison`，再作为 PHI incoming。
- [lib/passes/summary/NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:1438)
  `callValue(...)` 对非 ABI return register 的 fallback 改成 frozen unknown，不再直接返回
  `undef`。
- [lib/passes/summary/NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:1528)
  `collectFunctionReturnValues(...)` 在函数出口无法读到返回寄存器值时使用 frozen unknown。
- [lib/passes/summary/NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:1629)
  `buildReturnValue(...)` 对缺失返回值字段使用 frozen unknown。多返回值的 aggregate seed 仍用
  `undef`，因为后续字段会被 `insertvalue` 覆盖；缺失字段本身不再是裸 `undef`。
- [lib/passes/summary/NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:1807)
  signature rewrite 过程中，如果 call 参数或 helper return 无法匹配到真实值，使用 frozen unknown。
- [tools/notdec-native-llvm.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:883)
  默认优化函数在 `InstCombinePass` 后追加 `SimplifyCFGPass`，清理常量分支留下来的死边和死块。
- [tests/native_register_summary_ssa_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_register_summary_ssa_test.cpp:293)
  增加 `testUnknownPhiIncomingUsesFrozenPoison()`，覆盖一条路径写寄存器、另一条路径经过 unknown
  external call、汇合后读寄存器的情况，要求 PHI incoming 使用 `FreezeInst` 而不是裸 `undef`。

验证：

```text
cmake --build build -j$(nproc)
build/bin/pcode_to_llvm_test
build/bin/native_analysis_facts_test
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  --all-confirmed --summary-json-out /tmp/notdec-fortune-frozen-summary/summary.json \
  -o /tmp/notdec-fortune-frozen-summary/fortune.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-frozen-summary/fortune.ll \
  -o /tmp/notdec-fortune-frozen-summary/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-fortune-frozen-summary/fortune.bc \
  -o /tmp/notdec-fortune-frozen-summary/fortune.verified.bc
```

结果：

- 核心测试通过。
- fortune 默认 summary 链路通过 `llvm-as` 和 verifier，耗时 `10.49 sec`。
- fortune 默认输出中：
  - `br i1 poison`: 0
  - `ret ... poison`: 0
  - `store ... ptr poison`: 0

已知后续：

- `--no-instcombine-pass` 曾暴露 summary SSA 对前置 cleanup 的依赖，后续已继续收敛，见下一节。

评分：

- 实现效果：8/10。默认 fortune 输出不再出现 branch/return/store pointer 直接使用 poison。
- 理解成本：3/10。unknown 值表达统一为 `freeze poison`，符合 lowering 层已有做法。
- 维护成本：3/10。`SimplifyCFGPass` 是 LLVM 标准清理，风险低；`freeze poison` 插入点需要后续
  继续注意 dominance。

## 实现记录：修复 signature rewrite 后跨函数 argument 泄漏

背景：

- 继续跑 fortune 的 `--no-instcombine-pass` 调试模式时，summary SSA 不再是 verifier 报
  `br poison`，而是在 signature rewrite 阶段暴露出更具体的问题：
  - 先出现 `insertvalue` 字段类型不匹配的 assert。
  - 用 gdb 复现后，稳定看到 verifier 报 `Referring to an argument in another function!`，
    例如新函数返回值里还引用旧函数的 `%R8.arg`。
- 根因是 `rewriteInternalFunctionBody(...)` 用 `splice` 把旧函数 body 移到新函数后，只替换了
  summary SSA entry load 到新参数的映射，没有处理旧函数已有 argument 在 moved body 和
  `FunctionReturns` 缓存中的引用。

实现：

- [lib/passes/summary/NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:1629)
  `buildReturnValue(...)` 对单返回值和多返回值都增加类型兜底。缓存值类型和目标返回字段不一致时，
  使用 frozen unknown，而不是继续构造非法 `ret` 或 `insertvalue`。
- [lib/passes/summary/NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:1672)
  增加 `foreignArgumentReplacement(...)`。如果 moved body 里还引用别的函数的 argument：
  - 优先按同名同类型映射到新函数参数。
  - 找不到就用新函数 entry 里的 frozen unknown 替代。
- [lib/passes/summary/NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:1694)
  增加 `replaceForeignArgumentsInBody(...)`，扫描新函数体所有 instruction operand，消除跨函数
  argument 引用。
- [lib/passes/summary/NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:1760)
  `rewriteInternalFunctionBody(...)` 在 splice 和参数 entry-load 替换后调用
  `replaceForeignArgumentsInBody(...)`。
- [lib/passes/summary/NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:1776)
  对 `FunctionReturns` 缓存中的返回值也做同样的 foreign argument remap。
- [tests/native_register_summary_ssa_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_register_summary_ssa_test.cpp:673)
  增加 `testForeignArgumentInMovedBodyIsReplaced()`，构造旧函数已有 `%R8.arg`，但新 summary
  signature 不保留该参数的场景，验证 rewrite 后没有跨函数 argument operand。

验证：

```text
cmake --build build -j$(nproc)
build/bin/native_register_summary_ssa_test
build/bin/pcode_to_llvm_test
build/bin/native_analysis_facts_test
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  --all-confirmed --no-instcombine-pass \
  --summary-json-out /tmp/notdec-fortune-noinst-debug/summary.json \
  -o /tmp/notdec-fortune-noinst-debug/fortune.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-noinst-debug/fortune.ll \
  -o /tmp/notdec-fortune-noinst-debug/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-fortune-noinst-debug/fortune.bc \
  -o /tmp/notdec-fortune-noinst-debug/fortune.verified.bc
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  --all-confirmed --summary-json-out /tmp/notdec-fortune-default-post-foreign/summary.json \
  -o /tmp/notdec-fortune-default-post-foreign/fortune.ll
```

结果：

- 核心 native 测试通过。
- fortune `--no-instcombine-pass` 通过 `llvm-as` 和 verifier，耗时 `9.16 sec`。
- fortune 默认链路通过 `llvm-as` 和 verifier，耗时 `10.42 sec`。
- 默认输出中 `br i1 poison`、`ret ... poison`、`store ... ptr poison` 仍为 0。

评分：

- 实现效果：8/10。no-instcombine 调试模式不再因为 signature rewrite 的跨函数 argument 泄漏失败。
- 理解成本：4/10。多了一个 moved body 修复步骤，但位置集中在 internal signature rewrite。
- 维护成本：3/10。逻辑保守：同名同类型才复用新参数，否则降级为 frozen unknown。

## 实现记录：native direct branch 必须匹配 block successor facts

背景：

- 继续检查 `PcodeToLLVM` 后，发现 native mode 下 direct `BRANCH` 还有一处隐含的 p-code CFG
  依赖：只要 p-code target 已经有 LLVM block，就直接生成 `br`，没有确认这个 target 是否是
  当前 native block 的 `BlockSuccessors`。
- 这和当前路线不一致。机器级 CFG 边应该来自 instruction/block facts；p-code target 可以帮助
  lower 指令语义，但不能单独把两个 native block 连成 CFG。

实现：

- [lib/PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:708)
  增加 `isInternalPcodeTarget(...)`。指令内部 p-code block 仍允许直接跳，因为它属于同一个
  native block，不是机器级 CFG 边。
- [lib/PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:724)
  增加 `nativeDirectBranchTarget(...)`。native mode 下 direct branch 目标如果不是内部 p-code
  target，就必须出现在当前 block 的 `BlockSuccessors` 中；缺少 successor facts 或缺少该 successor
  都直接报错。
- [lib/PcodeToLLVM.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:809)
  direct `BRANCH` lowering 在生成 `br` 前调用 `nativeDirectBranchTarget(...)`。
- [tests/pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:371)
  `testNativeDirectBranchCanTargetEmptyBlock()` 明确补上 `0x1000 -> 0x2000` successor fact。
- [tests/pcode_to_llvm_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/pcode_to_llvm_test.cpp:403)
  增加 `testNativeDirectBranchRequiresSuccessorFact()`，验证 direct branch 不能只凭 p-code target
  连接到已有 native block。

验证：

```text
cmake --build build -j$(nproc)
build/bin/pcode_to_llvm_test
build/bin/native_analysis_facts_test
build/bin/native_register_summary_test
build/bin/native_register_summary_ssa_test
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  --all-confirmed --summary-json-out /tmp/notdec-fortune-strict-branch/summary.json \
  -o /tmp/notdec-fortune-strict-branch/fortune.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-strict-branch/fortune.ll \
  -o /tmp/notdec-fortune-strict-branch/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-fortune-strict-branch/fortune.bc \
  -o /tmp/notdec-fortune-strict-branch/fortune.verified.bc
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  --all-confirmed --no-instcombine-pass \
  --summary-json-out /tmp/notdec-fortune-strict-branch-noinst/summary.json \
  -o /tmp/notdec-fortune-strict-branch-noinst/fortune.ll
```

结果：

- 核心 native 测试通过。
- fortune 默认链路通过 `llvm-as` 和 verifier，耗时 `10.43 sec`。
- fortune `--no-instcombine-pass` 通过 `llvm-as` 和 verifier，耗时 `9.03 sec`。
- 默认输出中 `br i1 poison`、`ret ... poison`、`store ... ptr poison` 仍为 0。

评分：

- 实现效果：8/10。direct branch 不再靠 p-code target 偷连 native CFG。
- 理解成本：3/10。逻辑和 conditional/indirect branch 的 successor fact 校验一致。
- 维护成本：3/10。指令内部 p-code target 有专门豁免，避免影响 CMOV 这类单指令内部控制流。
