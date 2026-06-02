# 原始 prompt

```text
如果想要做到寄存器基本完全消除，类似那边Java的HighPCode链路，还缺哪几块部分，怎么规划一下实现？

为什么partial access无法被SSA消除？比如读取可以看作访问了完整的值，然后只拿一部分出来用？写入可以看作是写完整的值，没写到的部分就看作是读取了那部分，和当前的值组合了一下？

x86 写 EAX这种情况已经被pcode lifting考虑了吧？所以没必要特殊当做特例？另外，这些复杂的情况，Ghidra那边是怎么处理的？

把刚才提到的所有规划和改进都写到一个logs/下的规划文档里面
```

# 背景

目标不是只让 IR 能过 `llvm-as`，也不是只把部分函数签名改成参数和返回值。这里的目标是让 native 链路尽量接近 Ghidra Java HighPCode / decompiler heritage 后的状态：大部分架构寄存器不再以 `@RAX`、`@RDI` 这类全局状态反复 load/store 出现，而是变成 LLVM SSA value、函数参数、返回值和必要的 PHI。

当前 native 已有基础：

- `RegisterStorage` 已能把重叠寄存器范围合到一个 backing global，并能对 partial read/write 生成切片和 read-modify-write。
- `PcodeToLLVM` / `HeritageToLLVM` 已支持 `INT_ZEXT`、`SUBPIECE`、`PIECE` 等 P-Code op。
- `NativeRegisterSSA` 已能提升 full-unit register load/store，生成 external input、PHI、preserved/clobbered metadata。
- `NativePrototypeRecovery` 已恢复 register 参数、register 返回值、第一版 stack 参数候选，并支持 direct callsite signature rewrite。

主要缺口是：`NativeRegisterSSA` 目前只处理 full-unit access。对 partial access、callsite 当前值、rewrite 后残留寄存器流量，还没有统一的 storage SSA 视图。

# Ghidra 对应实现

Ghidra 的关键分工是：指令语义先由 SLEIGH/P-Code 表达，后续 heritage 和 prototype recovery 不再按寄存器名字补架构特例。

相关源码：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.hh`
  - `Heritage`：维护 SSA heritage 状态。
  - `LocationMap`：记录哪些地址范围已经 heritaged。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritage()`：执行 heritage pass，把 free varnode 接到 reaching definition。
  - `normalizeReadSize(...)` / `normalizeWriteSize(...)`：heritage 前处理读写范围大小。
  - 处理更大范围重新 heritage 时，会清理旧的 `MULTIEQUAL` / `INDIRECT` / `COPY` marker。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamTrial`：一个候选参数 storage。
  - `ParamActive`：当前函数或 callsite 的 trial 集合。
  - `ParamList` / `ParamListStandard`：ABI 参数 storage 匹配接口。
  - `FuncProto::deriveInputMap()` / `deriveOutputMap()`：从 trial 推导最终 prototype。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamListStandard::possibleParamWithSlot()`：判断 storage 是否落在 ABI 参数 slot。
  - `ParamListStandard::fillinMap()`：按 ABI 规则筛选 trial。
  - `FuncCallSpecs::checkInputTrialUse()`：判断 callsite 输入 trial 是否真的被使用。
  - `FuncCallSpecs::buildInputFromTrials()`：把 used trial 写回 CALL 输入。
  - `FuncCallSpecs::hasEffectTranslate()`：查询 call 对某个 storage 的 effect。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/subflow.hh`
  - `SplitDatatype`、`SubfloatFlow`、`LaneDivide`：处理值拆分、lane 拆分和精度传播。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/subflow.cc`
  - `SplitDatatype::buildInSubpieces()`：把大值输入拆成 `SUBPIECE`。
  - `SplitDatatype::buildOutConcats()`：用 `PIECE` 拼回大值。
  - `LaneDivide::buildZext()`：把 `INT_ZEXT` 拆到 lane 层面。
  - `LaneDivide::buildPiece()` / `buildMultiequal()`：处理 `PIECE` 和 `MULTIEQUAL` 的 lane 传播。

x86 特殊语义在 SLEIGH/P-Code 层表达。例如 64 位模式下 32 位寄存器写清零高位、部分 AVX 指令清零高位，Ghidra 的 `.sinc` 里会显式用 `zext` 或写完整目标寄存器。native 侧不应该在 SSA pass 里按 `EAX/RAX` 名字再补一次特例；如果 P-Code lowering 正确，SSA 只需要按 varnode 的 space/offset/size 语义做传播。

# native 侧缺口

## 1. partial register access 还没有进入 SSA

当前 `NativeRegisterSSA` 用 full-unit 判断决定是否提升：

- full load 能替换成 reaching SSA value。
- full store 能作为 reaching definition。
- partial load/store 只统计，不提升。

更完整的做法是把 SSA key 从 `GlobalVariable*` 扩展成 register storage range，读 partial 时从当前完整值切片，写 partial 时用当前完整值合成新完整值。

规则是：

- partial read：`base = current(base storage)`，再 `extract(base, offset, size)`。
- partial write：`old = current(base storage)`，`new = replace_range(old, offset, size, value)`，然后把 `new` 作为 base storage 的新定义。
- 如果 P-Code 已经生成完整 storage 写入，例如 `zext` 后写 `RAX`，就走 full write，不在 SSA pass 里硬编码 x86 名字。
- 如果 partial write 需要保留未写部分，而函数内没有本地定义，就必须生成 base storage 的 external input。这是语义正确的，不是误报。

## 2. callsite 当前寄存器值查询分散

现在 callsite input/return rewrite 里有多处 helper，从 store、PHI、entry external input、register global fallback 里找值。这个逻辑应该收敛成统一接口。

需要的查询形态：

- `valueBefore(inst, storage)`：某条指令前的 storage 当前值。
- `valueAtBlockEntry(block, storage)`：block 入口值，必要时建 PHI。
- `valueAtBlockExit(block, storage)`：block 出口值。
- `valueAfterCall(call, storage)`：考虑 direct callee metadata 和 ABI effect 后的值。

这样 prototype recovery 和后续 register cleanup 不再各自做一套 CFG 回看。

## 3. call effect 需要独立出来

当前 call clobber 逻辑主要在 `NativeRegisterSSA` 内部。后续要支持更完整寄存器消除，call effect 应成为单独的 resolver：

- direct internal call：优先读 callee 的 `notdec.register.preserves` / `notdec.register.clobbers` / recovered prototype。
- external declaration：按 ABI `unaffected` / `killedbycall`。
- indirect call：保守按 ABI 处理 unknown effect。
- 已 rewrite 的 direct call：ABI input/output storage 应按新 call 参数和返回值处理，不再当普通寄存器 global 流量。

## 4. signature rewrite 后缺少统一清理

当前 rewrite 已经能改函数签名和 direct callsite，但 rewrite 后还可能残留：

- 函数入口的 `notdec.register.external_input` load。
- return 前的 ABI output register store。
- callsite 前给 callee 准备 ABI input 的 register store。
- callsite 后读取 ABI output 的 register load。

需要一个 post-prototype register cleanup，只删除有明确 metadata 和唯一绑定的寄存器访问，避免误删普通寄存器状态。

## 5. stack 参数还没进入 signature rewrite

第一版 stack 参数候选已经有 metadata，但 signature rewrite 仍以 register storage 为主。要接近 HighPCode，简单 stack 参数也要进入函数参数，否则调用边界语义仍不完整。

第一版范围应收紧：

- 只支持 x86-64 SysV 简单正 offset stack input。
- 只使用 `notdec.stack.input` metadata，不从任意 GEP 猜 stack 参数。
- 先做 callee prototype 和简单 direct callsite，再考虑复杂 alias、varargs、动态栈调整。

## 6. flags 和 vector/lane 是后续长尾

如果目标是“基本完全消除”，flags 不能一直留成 global。很多控制流和条件计算依赖 `ZF/CF/SF/OF`。

建议顺序：

- 先统计 flags 残留数量和主要来源。
- 先做同一 basic block 内 producer/consumer 的 flags SSA。
- 再做跨 block PHI。
- vector 先处理 full XMM/YMM/ZMM，再处理 lane/slice。不要一开始追全 AVX/AVX512。

# 阶段计划

## 阶段 A：残留寄存器审计

先不要直接大改 SSA。先对 rewrite 后 IR 做统计，确认真实残留来自哪里。

统计内容：

- rewrite 后还有多少 `notdec.register.access`。
- 按 register global / storage kind 分类的 load/store 数。
- full access 和 partial access 数量。
- 残留位置：函数入口、return 前、callsite 附近、普通指令中。
- flags、GPR、XMM/YMM/ZMM 分别占比。

判断标准：

- 能列出 Bench2 固定样例里前几类寄存器残留。
- 能判断下一步收益最大的是 partial SSA、prototype cleanup，还是 flags。

## 阶段 B：storage range SSA

把 `NativeRegisterSSA` 从 full-unit global SSA 扩展到 register storage range SSA。

第一版只做整数 register backing unit：

- partial read 转成 extract。
- partial write 转成 replace-range。
- entry partial write 需要旧高位时生成 base external input。
- 跨 block 合流仍用 PHI。
- call barrier 仍按 ABI/callee effect 保守处理。

不做：

- 不按 `EAX/RAX` 名字硬编码 zero-extension。
- 不做 vector lane。
- 不做 flags。

判断标准：

- 小 IR 样例覆盖 low byte、高 byte、word、dword、跨 block PHI、call clobber。
- Bench2 中 GPR partial access 残留下降。
- LLVM 22 `llvm-as` / `opt -passes=verify` 通过。
- 固定 Bench2 回归耗时无明显退化。

## 阶段 C：统一 current-value 查询

把 callsite input/return rewrite 中分散的 CFG 回看逻辑收敛到一个查询模块。

目标：

- prototype recovery 不直接扫描各种 register store/load 形态。
- shared successor PHI、ambiguous predecessor fallback、call clobber 都走统一接口。
- 后续 register cleanup 复用同一接口。

判断标准：

- 现有 register 参数/返回值/direct callsite rewrite 测试不退化。
- Bench2 固定回归 skip reason 仍只剩合理类别。
- 代码里 callsite 当前值逻辑不再分散扩张。

## 阶段 D：prototype 后 register cleanup

在 signature rewrite 成功后，清理能唯一证明已被参数/返回值替代的寄存器访问。

范围：

- recovered input 的 external input load 替换为 function argument。
- recovered return 的 register store 替换为 LLVM return value。
- direct callsite 的 input store / output load 在有唯一绑定时删除或替换。
- 清理后删除无用 PHI 和无用 register access。

判断标准：

- 已 rewrite 函数内 ABI input/output register traffic 明显下降。
- 不删除没有 metadata 或绑定不唯一的普通寄存器访问。
- Bench2 固定回归 verify 通过，且抽查代表函数语义不变。

## 阶段 E：stack 参数 signature rewrite

把已有 stack input candidate 接入 signature rewrite。

第一版：

- callee 签名包含 stack input 参数。
- direct callsite 能从调用点 stack storage 取值。
- 暂不处理 varargs、动态栈、复杂 alias、callee pop 复杂情况。

判断标准：

- 简单 stack input 样例能 rewrite。
- 含 register + stack 混合参数时顺序符合 ABI slot。
- 遇到复杂栈形态明确跳过，不误 rewrite。

## 阶段 F：flags 和 vector/lane

根据阶段 A 的残留统计再决定顺序。建议 flags 优先于完整 vector，因为 flags 直接影响控制流语义。

第一版 flags：

- 同 block 内 flags producer 到 consumer 的 SSA 替换。
- 常见条件跳转和 setcc 用例。
- 跨 block 再补 PHI。

vector/lane：

- full XMM/YMM/ZMM 先消除。
- lane/slice 后做，参考 Ghidra `LaneDivide` 的处理思路。

# 风险

- partial write 会引入对旧 base value 的依赖。这个依赖如果来自函数入口，就会增加 external input；这是正确语义，但会影响 prototype recovery，需要用 ABI 和 use 判断过滤。
- 如果 P-Code lowering 没有正确表达架构语义，例如某些 32 位写没有变成完整 zero-extension 写，SSA pass 不应偷偷补特例，应先修 SLEIGH/P-Code 导出或 lowering。
- call effect 不准会导致错误跨 call 传播寄存器值。这个风险比 verifier 失败更严重。
- cleanup 必须只处理有 metadata 和唯一绑定的访问。为了减少寄存器残留而误删访问，会直接破坏语义。
- vector/lane 和 flags 范围很大，必须靠残留统计和真实 blocker 选小步，不要一开始追全。

# 不做什么

- 不退回旧的 slot + mem2reg 思路。
- 不把 x86 `EAX` zero-extension 硬编码进 SSA pass。
- 不靠寄存器名猜架构语义；架构语义应来自 P-Code。
- 不为了清零 register global 数量牺牲 call effect 和 alias 的保守性。
- 不在第一轮处理 varargs、动态栈调整、复杂 vector lane 和完整 flags 体系。

# 首个建议小步

先做阶段 A 的残留寄存器审计。原因是现在无法确定真实收益最大的点是 partial SSA 还是 rewrite 后 cleanup。

审计完成后再从结果里选一个小块：

- 如果 partial GPR 占主要残留，先做 storage range SSA。
- 如果 ABI input/output register traffic 占主要残留，先做 prototype 后 cleanup。
- 如果 flags 占主要残留，先做同 block flags SSA。

这个顺序能避免按直觉大改 SSA，也符合当前 Bench2 真实项目优先的目标。

# 实现记录

## 阶段 A：残留寄存器审计脚本

本轮先实现审计工具，不改 lowering、SSA 或 prototype rewrite 语义。

改动文件和函数：

- `scripts/native-register-residue-audit.py:1`
  - 新增 `.ll` 文本审计脚本。
  - `parse_metadata()` 读取 LLVM metadata 节点里的 `key=value` 字段。
  - `parse_globals()` 读取带 `!notdec.register` 的 register global，得到 backing unit 的 name、space、offset、size。
  - `parse_accesses()` 只统计 `!notdec.register.access` 和 `!notdec.register.external_input`，不把 register global 声明误算成访问。
  - `summarize()` 按 `category/access_kind/metadata_kind/shape` 汇总，其中 `shape` 区分 full 和 partial。
  - `write_details()` 输出逐条访问，方便定位残留位置和函数。
- `tests/native_register_residue_audit_test.py:1`
  - 新增 Python 单测，构造一个最小 `.ll`，覆盖 full external input、full store、partial load、flags store。
- `CMakeLists.txt:13`
  - 把新增 Python 单测注册成 `notdec.native_register_residue_audit.unit`。

验证：

```bash
python3 tests/native_register_residue_audit_test.py
python3 scripts/native-register-residue-audit.py tests/ir/native-prototype/cli-signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details tests/ir/native-prototype/cli-signature-rewrite.ll | head -20
cmake -S . -B build
ctest --test-dir build -R 'native_register_residue|bench2_native_discovery' --output-on-failure
```

结果：

- Python 单测通过。
- `tests/ir/native-prototype/cli-signature-rewrite.ll` 汇总结果：

```text
category	access_kind	metadata_kind	shape	count
gpr	load	external_input	full	7
gpr	store	access	full	9
```

- 重新配置 CMake 后，目标 CTest 2/2 通过：
  - `notdec.bench2_native_discovery_debug_oracle_unit`
  - `notdec.native_register_residue_audit.unit`

性能判断：

- 这是文本审计脚本，不在 native 生成和 pass pipeline 中运行，不影响当前 IR 生成性能。
- 后续在 Bench2 gate 里调用时，只线性扫描 `.ll` 文本，成本应远小于 lifting 和 LLVM verify。

限制：

- 当前只解析文本 `.ll`，不直接读 `.bc`。
- full/partial 判断依赖 register global 的 `!notdec.register` metadata 和 access metadata 的 `base/offset/size` 字段。
- 目前只做统计，不给出“该先修哪一类”的自动决策；下一步应在固定 Bench2 rewrite 输出上跑这个脚本，再按真实残留选择阶段 B 或阶段 D。

## 固定 Bench2 残留审计和本地 dead store cleanup

先用阶段 A 脚本跑固定三目标 signature-rewrite 输出：

```bash
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-register-residue-fixed-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-register-residue-fixed-gate/*.signature-rewrite.ll
```

结果显示 partial GPR 不是当前最大残留：

```text
category	access_kind	metadata_kind	shape	count
flags	load	external_input	full	15
flags	store	access	full	4740
gpr	load	access	full	53
gpr	load	access	partial	19
gpr	load	external_input	full	3186
gpr	store	access	full	5790
gpr	store	access	partial	31
other	load	access	full	8
other	load	external_input	full	50
other	store	access	full	245
other	store	access	partial	1
vector	load	external_input	full	70
vector	store	access	partial	72
```

按 base 看，主要是 `RSP` store、flags store、`RBP` store/external input 和 preserved register external input。基于这个结果，本轮没有直接做 storage range SSA，而是先做一个很保守的本地 dead register store cleanup：同一 basic block 内，full-unit register store 如果被后续同 register full store 覆盖，且中间没有同 register load、普通 call 或 terminator，就删除前一个 store。

改动文件和函数：

- `include/notdec-bin2llvm/passes/NativeRegisterSSA.h:24`
  - `NativeRegisterSSAFunctionSummary` 和 `NativeRegisterSSASummary` 新增 `DeadStoresRemoved` 计数。
- `lib/passes/NativeRegisterSSA.cpp:252`
  - `FunctionPromoter::run()` 在 `rewriteLoads()` 后调用 `removeLocalDeadStores()`。
- `lib/passes/NativeRegisterSSA.cpp:326`
  - 新增 `removeLocalDeadStores()`。
  - 只处理 full-unit register store。
  - 遇到同 register full load 时清掉该 register 的 pending store。
  - 遇到普通 call 或 terminator 时清掉全部 pending store，避免跨 call 或跨 block 删除。
  - partial access 暂不参与，后续交给 storage range SSA。
- `lib/passes/NativeRegisterSSA.cpp:744`
  - summary 聚合 `DeadStoresRemoved`。
- `lib/passes/NativeRegisterSSA.cpp:831`
  - summary 打印 `dead stores removed`。
- `tests/native_register_effects_test.cpp:244`
  - 新增 `createOverwrittenStoreFunction()`，覆盖同 block 后 store 覆盖前 store 的正例。
- `tests/native_register_effects_test.cpp:266`
  - 新增 `createCallBetweenStoresFunction()`，覆盖普通 call barrier，不允许删除 call 前 store。
- `tests/native_register_effects_test.cpp:308`
  - 新增 `countRegisterStores()`。

验证：

```bash
cmake --build build --target native_register_effects_test -j2
ctest --test-dir build -R 'notdec.native_register_effects.preserved' --output-on-failure
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-register-residue-dead-store-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-register-residue-dead-store-gate/*.signature-rewrite.ll
ctest --test-dir build --output-on-failure
```

结果：

- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过，skip reason 仍只剩 `already matches` / `declaration`。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 40s |
| `libuv:shared-library` | 105s | 105s |
| `memcached:executable` | 57s | 57s |

残留对比：

| kind | cleanup 前 | cleanup 后 |
| --- | ---: | ---: |
| `flags store access full` | 4740 | 3813 |
| `gpr store access full` | 5790 | 3601 |
| `gpr store access partial` | 31 | 14 |
| `vector store access partial` | 72 | 71 |

按 base 看，`RSP` store 从 3141 降到 1132。这个说明当前大量残留是局部覆写，不需要等完整 storage range SSA 才能减少。

注意：

- `libuv` 的 `prototype_return_candidates` / rewritten 数从 157/338 变成 155/337，skip reason gate 仍通过。判断是被删除的 store 本来被后续同 block store 覆盖，旧 return candidate 是更弱的候选；cleanup 让候选更保守。后续如果要确认具体函数，可继续用 residue details 和 prototype summary 定位。
- 这个 cleanup 不是 flags SSA，也不是 partial access SSA。它只是删除同 block 内确定被覆盖的 full store。
- 不跨 call、不跨 block、不处理 partial access，避免用局部清理替代真正的 storage SSA。

## 阶段 A 局部清理补充：partial store 被 full store 覆盖

上一轮只删除 full store 被后续 full store 覆盖的情况。本轮把同一 basic block 内“先 partial store，后 full store”的情况也纳入 cleanup：如果中间没有同 base register load、普通 call 或 terminator，后面的 full store 会完整覆盖前面的 partial store，前面的 partial store 可以删除。

这个规则仍然不把 partial access 提升成 SSA，也不做 partial store 之间的合并。两个 partial store 连续出现时会保留，因为第二个 partial store 不一定覆盖第一个 store 涉及的全部位。遇到任意同 base register load 会清掉 pending store，避免删掉被读到的旧写。

改动文件和函数：

- `lib/passes/NativeRegisterSSA.cpp:36`
  - `AccessInfo` 新增 `IsRegisterAccess`，把“这是寄存器访问”和“这是 full-unit 访问”分开。
- `lib/passes/NativeRegisterSSA.cpp:158`
  - `registerLoad()` 返回 `IsRegisterAccess=true`，partial load 也会作为 pending store 的读屏障。
- `lib/passes/NativeRegisterSSA.cpp:180`
  - `registerStore()` 返回 `IsRegisterAccess=true`，partial store 能进入局部 pending store 队列。
- `lib/passes/NativeRegisterSSA.cpp:327`
  - `FunctionPromoter::removeLocalDeadStores()` 的 pending store 从单个 full store 改成同 base register 的 store 列表。
  - register load 清掉同 base pending store。
  - full-unit store 删除同 base 已 pending 的 full/partial store，然后把自己作为新的 pending store。
  - partial store 只加入 pending，不删除之前的 store。
- `tests/native_register_effects_test.cpp:32`
  - 新增可指定 `base/name/offset/size` 的 `registerAccessMetadata()`，用于构造 partial access metadata。
- `tests/native_register_effects_test.cpp:305`
  - 新增 `createPartialStoreCoveredByFullStoreFunction()`，覆盖 partial store 被 full store 删除的正例。
- `tests/native_register_effects_test.cpp:326`
  - 新增 `createPartialStoresOnlyFunction()`，覆盖 partial store 之间不能互删的反例。
- `tests/native_register_effects_test.cpp:497`
  - 新增两个 store 数量断言。

验证：

```bash
cmake --build build --target native_register_effects_test -j2
ctest --test-dir build -R 'notdec.native_register_effects.preserved' --output-on-failure
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-register-residue-partial-covered-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-register-residue-partial-covered-gate/*.signature-rewrite.ll
ctest --test-dir build --output-on-failure
```

结果：

- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过，skip reason 仍只剩 `already matches` / `declaration`。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 41s |
| `libuv:shared-library` | 107s | 107s |
| `memcached:executable` | 57s | 57s |

残留统计和上一轮 full-store cleanup 后相同：

| kind | count |
| --- | ---: |
| `flags store access full` | 3813 |
| `gpr store access full` | 3601 |
| `gpr store access partial` | 14 |
| `vector store access partial` | 71 |

判断：

- 这一步是语义上成立的小补强，但固定三目标没有新的残留下降。
- 当前收益更大的方向仍然是 flags store、RSP/RBP store、以及后续真正的 storage range SSA。

## 阶段 A 局部清理补充：函数内未读 flags store

固定三目标里，上一轮后仍有 `3813` 个 flags store。先用 details 做函数级统计：

```bash
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-register-residue-partial-covered-gate/*.signature-rewrite.ll \
  > /tmp/notdec-bin2llvm-register-residue-partial-covered-gate/register-residue-details.tsv
awk -F '\t' '$3=="flags" {if ($4=="load") l[$1 "\t" $2]=1; if ($4=="store") s[$1 "\t" $2]++} END {for (k in s) {total+=s[k]; funcs++; if (!(k in l)) {nol+=s[k]; nolf++}} print funcs, total, nolf, nol}' \
  /tmp/notdec-bin2llvm-register-residue-partial-covered-gate/register-residue-details.tsv
```

结果是 `472` 个函数有 flags store，总数 `3813`；其中 `457` 个函数在 rewrite 后没有 flags load，包含 `3739` 个 store。

本轮先做保守清理：如果函数原始扫描阶段没有任何 flags load，就删除该函数内的 flags store。判断使用 `scanBlock()` 阶段看到的原始 load，不使用 `rewriteLoads()` 后的 IR 状态，避免 load 被替换后误判。删掉 flags store 后，同步从 `StoredFullUnits` 移除对应 flags，避免函数 metadata 继续把它们当作 clobber。

这仍然不是 flags SSA：

- 有任何 flags load 的函数不清理。
- 不跨函数推导 flags effect。
- 不把 flags 当作 ABI 参数或返回值。
- 不处理 flags producer/consumer 的 PHI。

改动文件和函数：

- `include/notdec-bin2llvm/passes/NativeRegisterSSA.h:24`
  - `NativeRegisterSSAFunctionSummary` 新增 `UnreadFlagStoresRemoved`。
- `include/notdec-bin2llvm/passes/NativeRegisterSSA.h:39`
  - `NativeRegisterSSASummary` 新增 `UnreadFlagStoresRemoved`。
- `lib/passes/NativeRegisterSSA.cpp:51`
  - 新增 `isFlagRegisterName()`，只识别 x86 常见 flags 名。
- `lib/passes/NativeRegisterSSA.cpp:252`
  - `FunctionPromoter::run()` 在 `removeLocalDeadStores()` 后调用 `removeUnreadFlagStores()`。
- `lib/passes/NativeRegisterSSA.cpp:271`
  - `scanBlock()` 记录 `LoadedUnits`，用于判断函数原始 IR 是否读过 flags。
- `lib/passes/NativeRegisterSSA.cpp:384`
  - 新增 `removeUnreadFlagStores()`。
  - 如果原始函数读过任何 flags，直接返回。
  - 否则删除函数内所有 flags store，并更新 `UnreadFlagStoresRemoved`。
  - 从 `StoredFullUnits` 删除这些 flags，保持 clobber metadata 和实际 IR 一致。
- `lib/passes/NativeRegisterSSA.cpp:801`
  - `addFunctionSummary()` 聚合 `UnreadFlagStoresRemoved`。
- `lib/passes/NativeRegisterSSA.cpp:889`
  - `printNativeRegisterSSASummary()` 打印 unread flags store 删除数量。
- `tests/native_register_effects_test.cpp:46`
  - `createRegisterGlobal()` 支持指定 offset/size，测试里能构造 i8 `CF`。
- `tests/native_register_effects_test.cpp:349`
  - 新增 `createUnreadFlagStoresFunction()`，覆盖没有 flags load 时删除 store。
- `tests/native_register_effects_test.cpp:369`
  - 新增 `createReadFlagStoresFunction()`，覆盖有 flags load 时不能删。
- `tests/native_register_effects_test.cpp:516`
  - 新增 `UnreadFlagStoresRemoved` 断言。
- `tests/native_register_effects_test.cpp:549`
  - 新增 unread/read flags store 数量断言。

验证：

```bash
cmake --build build --target native_register_effects_test -j2
ctest --test-dir build -R 'notdec.native_register_effects.preserved' --output-on-failure
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-register-residue-unread-flags-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-register-residue-unread-flags-gate/*.signature-rewrite.ll
ctest --test-dir build --output-on-failure
```

结果：

- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 41s |
| `libuv:shared-library` | 106s | 107s |
| `memcached:executable` | 57s | 57s |

残留对比：

| kind | 上一轮 | 本轮 |
| --- | ---: | ---: |
| `flags store access full` | 3813 | 2366 |
| `gpr store access full` | 3601 | 3601 |
| `gpr store access partial` | 14 | 14 |
| `vector store access partial` | 71 | 71 |

判断：

- flags store 下降 `1447`，耗时没有明显退化。
- 本轮清理后仍有 `2366` 个 flags store，其中很多来自原始函数中出现过 flags load 的函数，后续需要真正的 flags SSA 或 producer/consumer 局部化，不能靠“未读 store”规则继续硬删。

## 阶段 A 局部清理补充：函数内未读 RIP store

上一轮后 `other store access full` 主要是 `RIP`：

```bash
awk -F '\t' '$3=="other" && $4=="store" {c[$7]++} END {for (k in c) print c[k], k}' \
  /tmp/notdec-bin2llvm-register-residue-unread-flags-gate/register-residue-details.tsv | sort -nr
```

结果是 `245 RIP`。继续看函数级读写：

```bash
awk -F '\t' '$7=="RIP" {if ($4=="load") l[$1 "\t" $2]=1; if ($4=="store") s[$1 "\t" $2]++} END {for (k in s) {total+=s[k]; funcs++; if (!(k in l)) {nol+=s[k]; nolf++}} print funcs, total, nolf, nol}' \
  /tmp/notdec-bin2llvm-register-residue-unread-flags-gate/register-residue-details.tsv
```

结果是 `227` 个函数有 RIP store，总数 `245`；这些函数都没有 RIP load。RIP store 是 lifted instruction pointer 状态残留，不是当前 ABI 参数/返回值，也不参与现有 prototype recovery。这里做一个很窄的清理：函数原始 IR 没有 RIP load 时，删除该函数内 RIP store。有 RIP load 的函数不动。

不做：

- 不把规则泛化到普通 GPR。
- 不删除 `FS_OFFSET` 这类 segment base load。
- 不修改控制流或 branch target。

改动文件和函数：

- `include/notdec-bin2llvm/passes/NativeRegisterSSA.h:26`
  - `NativeRegisterSSAFunctionSummary` 新增 `UnreadRipStoresRemoved`。
- `include/notdec-bin2llvm/passes/NativeRegisterSSA.h:42`
  - `NativeRegisterSSASummary` 新增 `UnreadRipStoresRemoved`。
- `lib/passes/NativeRegisterSSA.cpp:57`
  - 新增 `isInstructionPointerName()`，目前只识别 `RIP`。
- `lib/passes/NativeRegisterSSA.cpp:265`
  - `FunctionPromoter::run()` 在 `removeUnreadFlagStores()` 后调用 `removeUnreadRipStores()`。
- `lib/passes/NativeRegisterSSA.cpp:432`
  - 新增 `removeUnreadRipStores()`。
  - 如果原始函数读过 RIP，直接返回。
  - 否则删除函数内所有 RIP store，并更新 `UnreadRipStoresRemoved`。
- `lib/passes/NativeRegisterSSA.cpp:843`
  - `addFunctionSummary()` 聚合 `UnreadRipStoresRemoved`。
- `lib/passes/NativeRegisterSSA.cpp:933`
  - `printNativeRegisterSSASummary()` 打印 unread RIP store 删除数量。
- `tests/native_register_effects_test.cpp:388`
  - 新增 `createUnreadRipStoresFunction()`，覆盖没有 RIP load 时删除 store。
- `tests/native_register_effects_test.cpp:405`
  - 新增 `createReadRipStoresFunction()`，覆盖有 RIP load 时不能删。
- `tests/native_register_effects_test.cpp:557`
  - 新增 `UnreadRipStoresRemoved` 断言。
- `tests/native_register_effects_test.cpp:594`
  - 新增 unread/read RIP store 数量断言。

验证：

```bash
cmake --build build --target native_register_effects_test -j2
ctest --test-dir build -R 'notdec.native_register_effects.preserved' --output-on-failure
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-register-residue-unread-rip-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-register-residue-unread-rip-gate/*.signature-rewrite.ll
ctest --test-dir build --output-on-failure
```

结果：

- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 41s |
| `libuv:shared-library` | 107s | 108s |
| `memcached:executable` | 58s | 57s |

残留对比：

| kind | 上一轮 | 本轮 |
| --- | ---: | ---: |
| `other store access full` | 245 | 0 |
| `flags store access full` | 2366 | 2366 |
| `gpr store access full` | 3601 | 3601 |
| `gpr store access partial` | 14 | 14 |
| `vector store access partial` | 71 | 71 |

判断：

- RIP store 清理移除了 `245` 个明显未读状态，耗时没有明显退化。
- 下一步主要残留仍是 GPR full store/load、flags store，以及少量 partial/vector。

## 阶段 B 基础修正：按 metadata 判断 partial/full

RegisterStorage 的 partial access 形态和 `NativeRegisterSSA` 原来的判断不一致：

- partial read 会先从 backing global 读完整值，例如 `load i64, ptr @RAX`，再在后续 IR 里截取。
- partial write 会先组合出新的完整 backing value，再 `store i64, ptr @RAX`。
- 但 `notdec.register.access` metadata 会标出真实架构访问范围，例如 `name=EAX,size=4` 或 `name=AL,size=1`。

原来的 `NativeRegisterSSA` 只按 LLVM load/store 的类型判断 full-unit。这样会把 `load i64 @RAX` + metadata `EAX` 误当作 full `RAX` 访问。这个判断和 residue audit 的口径不一致，也会挡住后续真正的 storage range SSA。

本轮修正 `AccessInfo`：

- `IsFullUnit`：按 metadata 的 `offset/size` 和 backing global 的 `offset/size` 判断。
- `IsStorageValue`：按 LLVM IR 类型判断这条 load/store 是否携带 backing full value。
- 现有 SSA 读写仍只替换 `IsStorageValue` 的访问，因为 RegisterStorage 已经把 partial write 合成为 backing full value。

这一步不是完整 partial bit-range SSA。它还不处理 IR 类型本身就是 `i8/i32` 的 partial load/store，也不做 extract/replace-range。它只是先修正分类，让后续阶段 B 能站在正确的 range 语义上。

改动文件和函数：

- `lib/passes/NativeRegisterSSA.cpp:30`
  - `RegisterUnit` 新增 `Offset`。
- `lib/passes/NativeRegisterSSA.cpp:37`
  - `AccessInfo` 新增 `IsStorageValue`、`Offset`、`Size`、`Name`。
- `lib/passes/NativeRegisterSSA.cpp:113`
  - `collectRegisterUnits()` 记录 backing global 的 offset。
- `lib/passes/NativeRegisterSSA.cpp:179`
  - `registerLoad()` 从 `notdec.register.access` metadata 读取 access offset/size/name。
  - `IsFullUnit` 改为按 metadata range 判断。
  - `IsStorageValue` 记录 load type 是否等于 backing global type。
- `lib/passes/NativeRegisterSSA.cpp:211`
  - `registerStore()` 从 metadata 读取 access offset/size/name。
  - `IsFullUnit` 改为按 metadata range 判断。
  - `IsStorageValue` 记录 store value type 是否等于 backing global type。
- `lib/passes/NativeRegisterSSA.cpp:309`
  - `scanBlock()` 用 `IsStorageValue` 判断 load 是否可进入现有 SSA rewrite。
- `lib/passes/NativeRegisterSSA.cpp:321`
  - `scanBlock()` 用 `IsStorageValue` 判断 store 是否写入 backing full value。
- `lib/passes/NativeRegisterSSA.cpp:338`
  - `rewriteLoads()` 用 `IsStorageValue` 作为替换条件。
- `lib/passes/NativeRegisterSSA.cpp:360`
  - `collectExternalInputsOnly()` 用 `IsStorageValue` 作为读取条件。
- `lib/passes/NativeRegisterSSA.cpp:398`
  - `removeLocalDeadStores()` 用 `IsStorageValue` 判断后续 store 是否完整覆盖 pending store。
- `lib/passes/NativeRegisterSSA.cpp:529`
  - `localValueBefore()` 用 `IsStorageValue` 判断 store 是否能作为当前 backing full value。
- `tests/native_register_effects_test.cpp:349`
  - 新增 `createPartialMetadataStorageValueFunction()`，构造 `i64 load/store @RAX` 但 metadata 为 `EAX` 的生产形态。
- `tests/native_register_effects_test.cpp:612`
  - 新增断言，确认这类 backing value load 能被 SSA 替换。

验证：

```bash
cmake --build build --target native_register_effects_test -j2
ctest --test-dir build -R 'notdec.native_register_effects.preserved' --output-on-failure
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-register-residue-partial-metadata-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-register-residue-partial-metadata-gate/*.signature-rewrite.ll
ctest --test-dir build --output-on-failure
```

结果：

- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 41s |
| `libuv:shared-library` | 105s | 106s |
| `memcached:executable` | 57s | 56s |

残留对比：

| kind | 上一轮 | 本轮 |
| --- | ---: | ---: |
| `gpr load access partial` | 19 | 19 |
| `gpr store access partial` | 14 | 14 |
| `gpr store access full` | 3601 | 3601 |
| `flags store access full` | 2366 | 2366 |
| `vector store access partial` | 71 | 71 |

判断：

- 固定三目标 residue 没有下降，但 pass 的 full/partial 判断口径已经和 metadata/audit 对齐。
- 后续真正做 extract/replace-range SSA 时，可以复用 `AccessInfo::Offset/Size/IsFullUnit/IsStorageValue`，不需要再先修分类。

## 阶段 C 小修：callsite fallback load 补完整 access metadata

阶段 B 修正后，固定三目标还剩一个明显的误判 partial：

```text
notdec_native_8480 gpr load access partial  RDI  %RDI.callsite_input = load i64, ptr @RDI ...
```

这个不是实际 partial access。来源是 `NativePrototypeRecovery` 在 direct callsite rewrite 时，如果找不到明确的 SSA input value，会在 caller 侧插入一个 register global fallback load。旧代码把 global 的 `notdec.register` metadata 直接挂到 load 的 `notdec.register.access` 上。global metadata 没有 `base=` 字段，residue audit 只能把 base 解析成空字符串，于是把一个 full `RDI` load 误判成 partial。

本轮修正这个 metadata 生成：

- 从 global 的 `notdec.register` 读取 `name/space/offset/size`。
- 重新生成完整 `notdec.register.access` metadata。
- 明确补上 `base=<name>` 和 `name=<name>`。

改动文件和函数：

- `lib/passes/NativePrototypeRecovery.cpp:632`
  - 新增 `registerGlobalAccessMetadata()`，把 register global metadata 转成完整 access metadata。
- `lib/passes/NativePrototypeRecovery.cpp:668`
  - `registerGlobalValueBeforeCall()` 不再直接复用 `notdec.register`，改为挂新生成的 `notdec.register.access`。
- `tests/native_prototype_recovery_test.cpp:2131`
  - 新增 `metadataHasField()`。
- `tests/native_prototype_recovery_test.cpp:3448`
  - 在 missing callsite input fallback 用例中断言生成的 load metadata 包含 `base=RDI` 和 `name=RDI`。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' --output-on-failure
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-register-residue-callsite-access-metadata-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-register-residue-callsite-access-metadata-gate/*.signature-rewrite.ll
ctest --test-dir build --output-on-failure
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 40s | 41s |
| `libuv:shared-library` | 106s | 106s |
| `memcached:executable` | 58s | 57s |

残留对比：

| kind | 上一轮 | 本轮 |
| --- | ---: | ---: |
| `gpr load access full` | 53 | 54 |
| `gpr load access partial` | 19 | 18 |
| `gpr store access partial` | 14 | 14 |
| `gpr store access full` | 3601 | 3601 |

判断：

- 这一步没有改变语义，只修正 metadata 口径。
- 固定三目标中一个误判 partial 被正确归类为 full。

## 阶段 D 小步：按单个 flag 清理未读 store

上一轮后 flags 还剩 `2366` 个 store。重新按 flag 名看读写后发现，剩余 flags load 全部是 `OF.external_input`，但旧规则只要函数读过任意 flags，就保留该函数内所有 flags store。这太粗。

固定三目标同口径统计：

```bash
awk -F '\t' '$3=="flags" {if ($4=="load") l[$1 "\t" $2 "\t" $7]=1; if ($4=="store") s[$1 "\t" $2 "\t" $7]++} END {for (k in s) {split(k,a,"\t"); flag=a[3]; total[flag]+=s[k]; funcs[flag]++; if (!(k in l)) {nol[flag]+=s[k]; nolf[flag]++}} for (flag in total) print flag, total[flag], nolf[flag]+0, nol[flag]+0}' \
  /tmp/notdec-bin2llvm-register-residue-callsite-access-metadata-gate/register-residue-details.tsv
```

结果：

| flag | stores | no same-flag load funcs | stores in no same-flag load funcs |
| --- | ---: | ---: | ---: |
| `CF` | 475 | 215 | 475 |
| `OF` | 475 | 200 | 459 |
| `PF` | 470 | 213 | 470 |
| `SF` | 473 | 213 | 473 |
| `ZF` | 473 | 213 | 473 |

本轮把 `removeUnreadFlagStores()` 从“函数是否读过任意 flag”改成“该 flag 自己是否被读过”。例如函数里读过 `OF` 时，只保留 `OF` store；同函数内没有被读过的 `CF/SF/ZF/PF` store 仍然删除。

改动文件和函数：

- `lib/passes/NativeRegisterSSA.cpp:419`
  - `removeUnreadFlagStores()` 改为收集 `readFlags` 集合。
  - 删除条件改为：这是 flag store，且同一个 flag 没有在原始函数中被 load。
- `tests/native_register_effects_test.cpp:408`
  - 新增 `createReadOneFlagStoreOtherFlagFunction()`，覆盖同函数读 `CF` 但写 `OF` 的情况。
- `tests/native_register_effects_test.cpp:603`
  - 更新 `UnreadFlagStoresRemoved` 断言。
- `tests/native_register_effects_test.cpp:644`
  - 新增断言：读过的 `CF` store 保留，未读的 `OF` store 删除。

验证：

```bash
cmake --build build --target native_register_effects_test -j2
ctest --test-dir build -R 'notdec.native_register_effects.preserved' --output-on-failure
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-register-residue-per-flag-unread-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-register-residue-per-flag-unread-gate/*.signature-rewrite.ll
ctest --test-dir build --output-on-failure
```

结果：

- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 41s |
| `libuv:shared-library` | 107s | 107s |
| `memcached:executable` | 57s | 58s |

残留对比：

| kind | 上一轮 | 本轮 |
| --- | ---: | ---: |
| `flags store access full` | 2366 | 665 |
| `gpr store access full` | 3601 | 3601 |
| `gpr load access partial` | 18 | 18 |
| `vector store access partial` | 71 | 71 |

按 flag 看，本轮后 flags store 剩余：

| flag | count |
| --- | ---: |
| `ZF` | 441 |
| `CF` | 108 |
| `OF` | 75 |
| `SF` | 39 |
| `PF` | 2 |

判断：

- flags store 下降 `1701`，耗时没有明显退化。
- 剩下的 flags store 已经不能靠“单个 flag 没被读过”继续删，后续需要真正做 flags producer/consumer SSA。
