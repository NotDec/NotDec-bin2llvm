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
