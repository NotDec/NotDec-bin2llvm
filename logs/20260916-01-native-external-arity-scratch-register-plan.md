# native：外部调用实参推理把"已被消费的临时寄存器"当参数

## 原始 prompt

```text
体积不敏感，没关系的下一步修什么
```

（给了三个候选后）

```text
按照推荐的这个试试吧
```

## 背景：收口后的现状（wrk final IR）

- define 87（`FUN_9990`/`FUN_99d0` 已回来）、warning 348 行、residue 1 行；
- 还剩 65 个 `notdec.unknown`，分布：`http_parser_execute` 33、
  `http_parser_parse_url` 21、其余 11；
- 11 号 log 记的"settings 指针被重建成 summary_clobber"那条线，本轮追到具体形态：
  `main` 传给 `script_verify_request` 的 RCX 是 unknown，因为
  `script_verify_request`/`script_request` 各多出一个假的 `i64 %RCX.arg` 形参，
  而它们来自 `lua_getfield` 被推断成 **4 个参数**。

## 根因

以 `script_init`（0x8bd0）里 8c30 的 `call lua_getfield` 为例：

```asm
8c16: call lua_newuserdata@plt
8c1b: lea 0x6af9(%rip),%rcx   # f71b
8c2d: mov %rcx,%rdx           # RDX = 真正的第 3 个实参
8c30: call lua_getfield@plt   # 真实只有 RDI/RSI/RDX 三个参数
```

lifter 产出的是裸调用 `call void @lua_getfield()`，实参完全由 callsite 证据推断：

1. `NativeRegisterSummary.cpp callsiteOrigin()` 只看寄存器当前值的**来源**
   （Local / Entry / CallClobber）；
2. `localDefinitionPrefix()` 取"连续前缀"（前导 Entry + 紧随的 Local）作为实参个数；
3. 同名 unknown external 取**所有调用点的最大 arity** 作为模块级签名
   （`NativeRegisterSummarySSA.cpp inferExternalCallShapes()`）。

真正把 arity 顶到 4 的调用点在 `script_create`（0xe952，实测确认）：

```asm
e952: lea 0x1003(%rip),%rcx   # f95c   ← 用 RCX 当临时寄存器
e959: mov $0xffffd8ee,%esi
e95e: mov %r13,%rdi
e965: mov %rcx,-0xa0(%rbp)    # 存进 frame（回调表初始化）
e96c: lea -0x7cb3(%rip),%rcx  # 6cc0
e973: lea 0xe14(%rip),%rdx    # f78e   ← 真正的第 3 个实参
e97a: mov %rcx,-0x90(%rbp)
...  还有若干 frame store
e9d4: call lua_getfield@plt   # 只有 RDI/RSI/RDX 是实参，RCX 是残留
```

RCX 在这里被反复用来给 frame store 传值，到调用时已经是个"用过的"值，但证据规则
只看它的**来源**（本地写 → LocalDefinition），于是被算成第 4 个实参 →
`lua_getfield` 全模块变 4 参数 → 其余 26 个调用点被迫用 `%RCX.arg`/`%RCX.clobber`/
unknown 去填第 4 个参数 → 反向传播成上层函数的假形参和 unknown。

## 目标与判断标准

- `lua_getfield` 回到 3 参数；`script_request`/`script_verify_request` 去掉假 RCX 形参；
- `notdec.unknown` 数明显下降；
- define ≥ 87、residue ≤ 1、warning 不出现新 kind、ctest 12/12、fortune 不劣化；
- IR 体积本轮不敏感，时间不劣化。

## 路线（最终版：只在"连续证据尾部"排除已被消费的值）

1. 证据里新增标记 `ConsumedLocal`（`NativeRegisterCallsiteSlotEvidence`）：
   LocalDefinition 的值在**本块内**最后一次写之后、调用点之前被对同一寄存器的
   load 读过，就打上。
2. `localDefinitionPrefix()` 里 `ConsumedLocal` 仍然算连续证据（不打断前缀），
   只在**连续证据的尾部**把它减掉（尾部的连续 `ConsumedLocal` 整段减掉）。

两个实测案例决定了"只在尾部"这个形状：

- `script_create`（0xe952）：RCX 是**尾部**槽（后面没有别的实参证据），值已被
  frame store 消费 → 减掉 → `lua_getfield` 回到 3 参数 ✓
- memcached `sasl_listmech`（0x26d65）是反例，不能"读过就截断"：

```asm
26d69: lea 0xb121(%rip),%r8    # suffix
26d80: lea 0xb7c3(%rip),%rcx
26d94: mov %r8,%rdx            # prefix = R8（同一个值）
26d97: push $0x0
26d99: push %rax               # 第 7 个参数
26d9d: call sasl_listmech
```

R8 被读过，但它和 RDX 都是实参（后面还有 R9/栈证据），"读过就截断"会把 arity 从
真实 7 砍到 4。只在尾部减既不碰 R8，又能修 `script_create` 的 RCX。

（中途试过再加一条"只有读的结果写进另一个实参寄存器才算消费"的窄条件：它在
`script_create` 上失效——那里的读是喂给 frame store 的，结果 arity 又回到 4，
memcached 也没变好，所以整条撤回，只保留尾部规则。）

只对本块扫描（跨块的值中间没有指令能消费它）；只影响 unknown external 的寄存器
前缀计数，vararg tail 和 indirect 的 origin 判定保持原样。

## 不做什么

- 不动 indirect call 的 RDX 保守返回（`testIndirectConservativeReturnShapeExcludesRdx`
  钉住，要改先讨论）；
- 不动 `http_parser_parse_url` 的窄 lane 规划；
- 不用 "live-out" 判定：真正的最后一个实参在调用后本来就是死的，按 live-out 会把
  所有实参都判掉；这里要的是"调用点还活着"，即没被别的读消费掉。

## 实现记录

- `include/notdec-bin2llvm/passes/summary/NativeRegisterSummary.h`：
  `NativeRegisterCallsiteSlotEvidence` 新增 `ConsumedLocal`；
- `lib/passes/summary/NativeRegisterSummary.cpp`：新增
  `registerValuePendingAtCall()`，在 `callsiteEvidence()` 里设置标记
  （`callsiteOrigin()` 本身不动，origin 语义不变）；
- `lib/passes/summary/NativeRegisterSummarySSA.cpp`：`localDefinitionPrefix()`
  在连续前缀尾部减掉 `ConsumedLocal`；
- `tests/native_register_summary_ssa_test.cpp`：新增
  `testConsumedScratchRegisterIsNotAnExternalArgument()`（尾部值被 frame store
  消费 → 3 个实参）和 `testConsumedRegisterBeforeLaterArgumentKeepsArity()`
  （中间被复制、后面还有实参 → 6 个实参）。

### 反向验证

- 不设 `ConsumedLocal`（等价于修复前）→
  `testConsumedScratchRegisterIsNotAnExternalArgument()` 报
  `consumed scratch register was counted as an argument`（arity 3 → 4）；
- 减掉**所有** `ConsumedLocal`（等价于最初"读过就截断"的版本）→
  `testConsumedRegisterBeforeLaterArgumentKeepsArity()` 报
  `consumed argument register dropped later argument evidence`（arity 6 → 4）。

两个反向验证都做过，恢复后 `native_register_summary_ssa_test` 全绿。

## 验证（wrk，对照 = 本轮之前的 633aa80）

| 项 | 之前 | 本轮 |
| --- | --- | --- |
| define 数 | 87 | 87 |
| `lua_getfield` 声明 | `(i64,i64,i64,i64)`，27 个调用点全 4 参 | `(i64,i64,i64)`，27 个调用点全 3 参 |
| `script_request` 签名 | `(RDI,RSI,RDX,RCX)` | `(RDI,RSI,RDX)` |
| `script_verify_request` 签名 | `(RDI,RCX)` | `(RDI)` |
| main 里的调用 | `call @script_verify_request(%unique_df00_8594, %notdec.reg.insert.lower1528)` | `call @script_verify_request(%unique_df00_8594)` |
| `summary_clobber` 使用 | 51 | 25 |
| warning TSV | 348 行（347 行数据） | 300 行（299 行数据） |
| warning 明细 | clobber 52 / call_arg 26 / signature 268 | clobber 24 / call_arg 11 / signature 263 |
| 新增 warning | — | 0 条（diff 只有删行 + 2 行 arity 文案 4→3） |
| 外部 callee arity | `lua_getfield` final=4 | 只有它变成 final=3；`unresolved_unknown_external_signature` 4 条不变 |
| `notdec.unknown` 调用 | 65 | 64 |
| residue | 1 行 | 1 行 |
| IR 体积 | 1,952,740 B / 27,539 行 | 1,915,062 B / 27,140 行（-1.9%） |
| 时间 / 内存 | 66.9s / 137MB | 46.1s / 135MB |
| ctest | 12/12 | 12/12（含 fortune x86_64/i386） |

命令：

```bash
build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk \
  --all-confirmed --register-ssa-summary \
  --register-ssa-warning-out /tmp/nb-frame/gate.warn.tsv \
  -o /tmp/nb-frame/gate.native.ll
```

剩余 `notdec.unknown` 的分布没变（`http_parser_execute` 33、`http_parser_parse_url` 21、
其余 10），本轮的收益集中在"消掉假形参 + 被 clobber 的实参值"这条线
（`remaining_summary_clobber_value` 52 → 24）。

### memcached 对照（同一对二进制）

| 项 | 之前（633aa80） | 本轮 |
| --- | --- | --- |
| 结果 | exit=0（654s） | exit=0（324s） |
| define 数 | 230 | 230 |
| `notdec.unknown` | 1,259 | 1,259 |
| `summary_clobber` | 4,072 | 4,076 |
| warning TSV | 4,971 行 | 4,975 行 |
| `sasl_listmech` 声明 | `(i64 × 8)` | `(i64 × 8)`（不变） |
| IR | 10,759,379 B | 10,760,089 B（+0.007%） |
| residue | 5 | 5 |

- 外部 callee 的 arity 全部不变（`inferred_unknown_external_arity` 行 diff 为空）。
  上一版"读过就截断"曾把 `sasl_listmech` 从 8 砍到 4（真实 7），这一版不会；
- 代价：新出现 4 条 `remaining_summary_clobber_value`（都在 `FUN_22b30`，RCX），
  该函数内部多 4 个 `summary_clobber`；其它函数的签名/arity 不变。

（最终二进制在收尾时做过一次"去掉未使用出参"的等价清理：重建后 wrk 的
warning diff 0、归一化后的 IR 多重集合 diff 0，因此上面 memcached 的数字仍然有效。）

### 已知取舍

- 只在**尾部**减掉已被读过的槽，中间的不受影响：memcached `sasl_listmech`
  （0x26d65）R8 被 `mov %r8,%rdx` 读走，但后面还有 R9/栈实参 → arity 不变。
- 仍有的歧义：尾部的真实参恰好被读过时（例如 `p->f = x; foo(x)` 编译成
  `mov %rdi,(%rax); call foo`）会被当成残留而丢掉一个实参。这种形态和
  `script_create` 的 RCX 在 IR 上不可区分，先接受；wrk 上外部 callee 只有
  `lua_getfield` 的 arity 变化、`unresolved_unknown_external_signature` 不变，
  memcached/fortune 见下。
