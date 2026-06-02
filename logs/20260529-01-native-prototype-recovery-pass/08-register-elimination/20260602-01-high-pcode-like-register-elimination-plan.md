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

## 阶段 D 小步：按 rewrite 后实际剩余 flag load 清理 store

上一轮 `removeUnreadFlagStores()` 已经改成按单个 flag 判断，但判断依据还是 `scanBlock()` 阶段记录的原始 load。这个仍然偏保守：如果某个 flag load 已经在 `rewriteLoads()` 中被 SSA 值替换并删除，它就不应该继续保护后面的 flag store。

本轮把 read flag 集合改成从 rewrite 后当前 IR 重新扫描：

- `rewriteLoads()` 先替换可解析的 flag load。
- `removeUnreadFlagStores()` 再扫描当前函数里还存在的 flag load。
- 只有当前 IR 里仍有同一个 flag load 时，才保留该 flag store。

这仍然不是完整 flags SSA。它只是删除已经没有任何当前 IR load 依赖的 flags store；还存在的 `OF.external_input` 相关 store 仍保留。

改动文件和函数：

- `lib/passes/NativeRegisterSSA.cpp:419`
  - `removeUnreadFlagStores()` 不再使用 `LoadedUnits`。
  - 改为扫描当前 IR 中仍存在的 `LoadInst`，用 `registerLoad()` 判断剩余 flag load。
- `tests/native_register_effects_test.cpp:603`
  - 更新 `UnreadFlagStoresRemoved` 断言。
- `tests/native_register_effects_test.cpp:642`
  - 更新断言：已经被 SSA 解析掉的 `CF` load 不再保护对应 `CF` store。

验证：

```bash
cmake --build build --target native_register_effects_test -j2
ctest --test-dir build -R 'notdec.native_register_effects.preserved' --output-on-failure
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-register-residue-post-rewrite-flag-liveness-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-register-residue-post-rewrite-flag-liveness-gate/*.signature-rewrite.ll
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
| `memcached:executable` | 57s | 57s |

残留对比：

| kind | 上一轮 | 本轮 |
| --- | ---: | ---: |
| `flags store access full` | 665 | 16 |
| `gpr store access full` | 3601 | 3601 |
| `gpr load access partial` | 18 | 18 |
| `vector store access partial` | 71 | 71 |

本轮后剩余 flags store 全部是 `OF`：

| flag | count |
| --- | ---: |
| `OF` | 16 |

判断：

- flags store 又下降 `649`，耗时没有明显退化。
- 剩下的 `OF` store 与当前 IR 中仍存在的 `OF.external_input` load 同 flag，不能继续用未读规则删除。下一步要处理它们，需要真正的 flags SSA 或更准确的外部输入消除。

## 阶段 D 小步：按 CFG liveness 清理剩余 OF store 和死 external input

上一轮还剩 16 个 `OF` store 和 15 个 `OF.external_input` load。抽查后发现两类形态：

- `OF.external_input` 只经过 `and ..., 1` 后写回 `@OF`。
- `OF.external_input` 参与了已经无 use 的中间 flag 计算，最后 `@OF` 被常量 `0` 覆盖。

之前按“函数里是否还有同 flag load”判断太粗。只要函数入口还有 `OF.external_input`，所有 `OF` store 都会被保护，即使 store 之后没有任何 load 能读到它。

本轮改成基本块级 liveness：

- 为每个基本块计算 flag `LiveIn` / `LiveOut`。
- 反向扫描指令，store 只有在后面可能有同 flag load 读到时才保留。
- 普通 call 按当前 ABI / callee effect 作为 clobber barrier，call 前的 store 不能被 call 后的 load 证明为 live。
- 删除 store 后，递归清理只依赖 flag external input 的死计算链，并同步移除 `notdec.register.external_inputs` 里的 stale 项。

改动文件和函数：

- `lib/passes/NativeRegisterSSA.cpp:59`
  - 新增 `FlagBlockLiveness`，保存每个基本块的 flag live-in / live-out。
- `lib/passes/NativeRegisterSSA.cpp:425`
  - `removeUnreadFlagStores()` 改为 CFG 反向 liveness，不再只看全函数 read set。
  - 删除 dead store 后调用 `RecursivelyDeleteTriviallyDeadInstructions()`，并通过回调清理 external input 记录。
- `lib/passes/NativeRegisterSSA.cpp:503`
  - 新增 `flagLiveBeforeBlock()`，计算单个 basic block 入口前的 live flags。
- `lib/passes/NativeRegisterSSA.cpp:531`
  - 新增 `eraseClobberedFlagsFromLiveSet()`，让 call clobber 语义参与 flag liveness。
- `lib/passes/NativeRegisterSSA.cpp:541`
  - 新增 `hasRemainingStorageStore()`，避免删除部分 store 后留下错误的 clobber 记录。
- `lib/passes/NativeRegisterSSA.cpp:558`
  - 新增 `forgetExternalInputValue()`，删除死 external input load 时同步更新 metadata 来源集合。
- `lib/passes/NativeRegisterSSA.cpp:569`
  - 新增 `removeDeadFlagExternalInputUsers()` / `valueDependsOnFlagExternalInput()`，清掉 store 删除后暴露出的死 flag external input 链。
- `tests/native_register_effects_test.cpp:431`
  - 新增 `createFlagRestoreOnlyFunction()`，覆盖 `OF.external_input -> and -> store @OF`。
- `tests/native_register_effects_test.cpp:452`
  - 新增 `createDeadFlagInputBeforeConstantStoreFunction()`，覆盖 `OF.external_input` 只参与死中间计算、最后 `store 0 @OF`。
- `tests/native_register_effects_test.cpp:649`
  - 更新 `UnreadFlagStoresRemoved` 断言为 `6`。
- `tests/native_register_effects_test.cpp:693`
  - 增加 dead OF restore / constant-store 形态的 load、store、external input metadata 断言。

验证：

```bash
cmake --build build --target native_register_effects_test -j2
ctest --test-dir build -R 'notdec.native_register_effects.preserved' --output-on-failure
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
ctest --test-dir build --output-on-failure
cmake --build build --target notdec-native-llvm -j2
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-register-residue-flag-liveness-dce-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-register-residue-flag-liveness-dce-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-register-residue-flag-liveness-dce-gate/*.signature-rewrite.ll | \
  awk -F '\t' '$3=="flags" {print}'
```

结果：

- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 40s |
| `libuv:shared-library` | 105s | 106s |
| `memcached:executable` | 57s | 57s |

残留对比：

| kind | 上一轮 | 本轮 |
| --- | ---: | ---: |
| `flags load external_input full` | 15 | 0 |
| `flags store access full` | 16 | 0 |
| `gpr store access full` | 3601 | 3601 |
| `gpr load access partial` | 18 | 17 |
| `vector store access partial` | 71 | 71 |

判断：

- 固定三目标上 flags 残留已经清零，耗时没有明显退化。
- 这仍然不是完整 flags SSA；它只删除没有后继读者的 flag store 和随之暴露的死 external input 链。
- 后续更大的收益在 GPR full store / external input、栈相关寄存器流量、callsite 当前值查询和 partial storage SSA。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 8 | 固定三目标 flags 残留从 `store 16 / input 15` 变成 `0`。 |
| 理解成本 | 6 | 增加了一个局部 liveness 算法，但只限 flags，代码边界清楚。 |
| 维护成本 | 5 | 仍依赖 metadata 和 ABI call effect；后续做完整 flags SSA 时可能需要替换这段保守 cleanup。 |

有没有更好的方案：

- 长期更好的方案是完整 flags SSA，让 flag producer / consumer 直接变成 SSA value。
- 本轮没有直接做完整 flags SSA，是因为当前明显残留是未读 store 和死输入链，先用小步 cleanup 可以稳定清零这批残留。

## 阶段 C 小步：复用同一 call barrier 后的重复 register load

固定三目标 flags 残留清零后，剩余 register load 里有一批形态很明确：

- call 之后读取某个 killed-by-call register。
- 第一次 load 不能被替换，因为 call 确实阻断了 call 前 reaching value。
- 同一个 call barrier 后再次读取同一个 backing register 时，应该复用第一次 load 的 SSA value，而不是继续从全局 register load。

典型例子是 `uv_mutex_trylock`：`pthread_mutex_trylock()` 后连续读取 `RAX/EAX` 做不同比较。第一条 `RAX` load 是 call output 的保守表示；后续同 register load 可以安全复用它。

本轮只做这个窄规则：

- `localValueBefore()` 继续不穿过 clobbering call。
- 如果在同一个 barrier 之后已经有同 register 的 storage-value load，就把它作为当前值。
- 不把 declaration call 强行解释成返回值，不改 ABI，不改 partial metadata。

改动文件和函数：

- `lib/passes/NativeRegisterSSA.cpp:663`
  - `localValueBefore()` 在查找 store 前先识别同 register 的前序 load。
  - 一旦遇到 clobbering call，仍停止使用 call 之前的 load/store。
- `tests/native_register_effects_test.cpp:144`
  - 新增 `createRepeatedLoadAfterCallFunction()`，覆盖 call 后同 register 连续 load。
- `tests/native_register_effects_test.cpp:617`
  - 将新样例加入主测试模块。
- `tests/native_register_effects_test.cpp:685`
  - 断言 call 后重复 `RAX` load 从 2 条降到 1 条。

验证：

```bash
cmake --build build --target native_register_effects_test -j2
ctest --test-dir build -R 'notdec.native_register_effects.preserved' --output-on-failure
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
ctest --test-dir build --output-on-failure
cmake --build build --target notdec-native-llvm -j2
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-register-residue-repeated-load-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-register-residue-repeated-load-gate/*.signature-rewrite.ll
```

结果：

- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 40s | 40s |
| `libuv:shared-library` | 105s | 107s |
| `memcached:executable` | 56s | 56s |

残留对比：

| kind | 上一轮 | 本轮 |
| --- | ---: | ---: |
| `gpr load access full` | 53 | 39 |
| `gpr load access partial` | 17 | 9 |
| `gpr store access full` | 3601 | 3601 |
| `gpr store access partial` | 14 | 14 |
| `flags` | 0 | 0 |

判断：

- 这个改动减少了 call 后重复 register load，没有让 value 穿过 call barrier。
- 耗时没有明显退化。
- 下一步还要处理 store 残留、external input 和真正 partial storage SSA。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 5 | GPR access load 明显下降，但 store 主体还没动。 |
| 理解成本 | 3 | 只在现有 `localValueBefore()` 增加一个前序 load 复用分支。 |
| 维护成本 | 3 | 规则依赖现有 call barrier 逻辑，后续统一 current-value 查询时可以直接迁移。 |

有没有更好的方案：

- 长期应把这类逻辑放入统一的 `valueBefore(inst, storage)` 查询模块。
- 本轮先内联在 `NativeRegisterSSA`，因为当前目标只是消除明显重复 load，代码面小且风险低。

## 阶段 D 小步：partial metadata 的完整 backing store 参与返回候选

固定三目标里还有一些 `store i64 ..., @RAX`，但 `notdec.register.access` 标的是 `AL` / `EAX`。其中一类不是普通 partial write，而是 lowering 已经生成完整 backing storage value，只是 metadata 仍保留原始 access name。

典型例子：

- `uv_is_closing` 最后 `store i64 %4, ptr @RAX`，metadata 是 `base=RAX,name=AL,size=1`。
- 这实际已经有完整 `RAX` backing value，可以作为返回候选。
- 旧逻辑只用 metadata `name` 匹配 ABI output，所以 `AL` / `EAX` 匹配不到 ABI 的 `RAX`，导致函数保持 `void`，返回 store 残留。

本轮只放宽这个窄条件：

- 如果 metadata `name` 不能匹配 ABI output，但 `base` 能匹配。
- store 的 value type 是完整 `i64`。
- store 目标 global 也是 `i64`，且 global 的 `notdec.register name` 等于 metadata `base`。
- 满足这些条件时，按 `base` 作为返回 register。

真正只在部分路径写返回的旧负例仍保持不候选。

改动文件和函数：

- `lib/passes/NativePrototypeRecovery.cpp:1518`
  - `returnTrialsBeforeInstruction()` 在 metadata `name` 匹配失败时，尝试用 `base` 匹配 ABI output。
  - fallback 只接受完整 `i64` backing store，并校验 store 目标 global 的 register name 等于 metadata `base`。
- `tests/native_prototype_recovery_test.cpp:1832`
  - 新增 `createFullStoragePartialMetadataReturnStoreFunction()`，覆盖 `base=RAX,name=AL` 但 store value 是完整 `i64` 的返回样例。
- `tests/native_prototype_recovery_test.cpp:2401`
  - 将新样例加入主测试模块。
- `tests/native_prototype_recovery_test.cpp:2564`
  - 更新 summary 计数。
- `tests/native_prototype_recovery_test.cpp:2625`
  - 断言完整 backing store 的 partial metadata 返回能标成 `RAX` return candidate。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' --output-on-failure
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
ctest --test-dir build --output-on-failure
cmake --build build --target notdec-native-llvm -j2
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-register-residue-partial-return-base-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-register-residue-partial-return-base-gate/*.signature-rewrite.ll
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 41s |
| `libuv:shared-library` | 107s | 105s |
| `memcached:executable` | 57s | 57s |

残留对比：

| kind | 上一轮 | 本轮 |
| --- | ---: | ---: |
| `gpr store access partial` | 14 | 11 |
| `gpr store access full` | 3601 | 3601 |
| `gpr load access full` | 39 | 39 |
| `gpr load access partial` | 9 | 9 |
| `flags` | 0 | 0 |

抽查：

- `uv_is_closing` 从 `void` 变成 `i64 @uv_is_closing(i64 ...)`。
- `uv_os_getenv` 从 partial `AL` return store 残留推进到 `i64` 返回。
- `uv_translate_sys_error` 仍保持保守，因为存在多路径/冲突形态，不能靠本轮规则证明。

判断：

- 这不是把 partial write 全部当 full write，只处理 lowering 已经给出完整 backing value 的返回 store。
- 固定三目标 partial GPR store 残留下降 `3`，耗时没有明显退化。
- 后续真正 partial write 仍需要 storage range SSA 或更精确的 read-modify-write 语义。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 4 | 只下降 3 个 partial store，但修正了真实返回签名漏恢复。 |
| 理解成本 | 4 | 增加了 `name` fallback 到 `base` 的窄条件，需要理解 metadata name/base 区别。 |
| 维护成本 | 4 | 条件较保守，后续 range SSA 完成后可能可替换为统一 storage 匹配。 |

有没有更好的方案：

- 长期应让 ABI storage 匹配支持 range/base，而不是在 return trial 里补局部 fallback。
- 本轮先修返回候选漏标，因为它会直接影响 signature rewrite 和返回语义。

## 阶段 B 小步：整数 partial IR access 进入 backing SSA

前面几轮已经确认，Bench2 固定三目标里的很多 `partial` 残留不是 `i8/i32` 直接访问 register global，而是 `store i64 ..., @RAX` 这种完整 backing value，只是 metadata 仍保留 `AL/EAX`。但 `NativeRegisterSSA` 对真正 IR 类型就是 partial 的访问仍缺少处理。

本轮补第一版整数 partial IR access：

- partial load：先读当前完整 backing value，再按 metadata range 做 shift/trunc。
- partial store：先读当前完整 backing value，按 metadata range 清位、写入新片段，再生成完整 backing store。
- 如果 partial store 需要旧高位且本地没有定义，会生成 base register external input，这是保留未写部分所需的语义。
- flags、RIP、vector lane 不纳入本轮。

改动文件和函数：

- `lib/passes/NativeRegisterSSA.cpp:73`
  - 新增 `bitWidth()`、`bitOffset()`、`isIntegerUnit()`、`canPromotePartialAccess()`，集中判断哪些 partial access 可以进入本轮整数 backing SSA。
- `lib/passes/NativeRegisterSSA.cpp:327`
  - `FunctionPromoter::run()` 在 full load rewrite 前先执行 `rewritePartialStores()`，把直接 partial store 扩成完整 backing store。
- `lib/passes/NativeRegisterSSA.cpp:343`
  - `scanBlock()` 把可提升 partial load 加入 rewrite 列表。
- `lib/passes/NativeRegisterSSA.cpp:375`
  - `rewriteLoads()` 对 partial load 使用 `extractPartialValue()`，替换为 SSA extract，不再保留原始 register load。
- `lib/passes/NativeRegisterSSA.cpp:412`
  - 新增 `rewritePartialStores()`，对 `i8/i16/i32` 等 partial store 做 read-modify-write，并用 `notdec.register.synthetic` 标记由 SSA 合成出的完整 backing store。
- `lib/passes/NativeRegisterSSA.cpp:451`
  - `removeLocalDeadStores()` 删除死 store 后递归清理暴露出的 dead external input 链，避免 partial store 被 full store 覆盖后留下假输入。
- `lib/passes/NativeRegisterSSA.cpp:695`
  - 新增 `resizeInteger()`、`extractPartialValue()`、`replacePartialValue()`、`markSyntheticPartialStore()`。
- `lib/passes/NativeRegisterSSA.cpp:813`
  - `localValueBefore()` 遇到前序 partial store 时即时合成当前 backing value，支持连续 partial store。
- `lib/passes/NativePrototypeRecovery.cpp:1533`
  - `returnTrialsBeforeInstruction()` 跳过 `notdec.register.synthetic` store，避免把 SSA 合成出的 partial backing store 误当成完整返回候选。
- `include/notdec-bin2llvm/passes/NativeRegisterSSA.h:52`
  - 更新 pass 注释，说明当前已处理整数 partial access，flags/vector 仍保守。
- `tests/native_register_effects_test.cpp:395`
  - 新增 `createPartialLoadFunction()`，覆盖 `i8` partial load 被 SSA extract 替换。
- `tests/native_register_effects_test.cpp:733`
  - 更新 partial store 测试预期：连续 `AL` store 会折叠成一个完整 backing store，并保留旧 backing external input。

验证：

```bash
cmake --build build --target native_register_effects_test native_prototype_recovery_test -j2
./build/bin/native_register_effects_test
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
ctest --test-dir build --output-on-failure
cmake --build build --target notdec-native-llvm -j2
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-partial-storage-ssa-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
```

结果：

- `native_register_effects_test` 通过。
- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- `notdec-native-llvm` 构建通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 40s |
| `libuv:shared-library` | 106s | 105s |
| `memcached:executable` | 57s | 58s |

判断：

- 耗时没有明显退化。
- 本轮能消除真实 `i8/i16/i32` partial IR access。
- 固定三目标 residue 数字没有下降，是因为这些目标剩余 partial metadata 行的 IR value 都已经是完整 backing value；它们不是本轮新增能力要处理的直接 partial IR access。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 4 | 补上真正 partial IR access 的 SSA 基础，但固定三目标主要剩的是 partial metadata/full value。 |
| 理解成本 | 5 | 引入 extract/replace-range 和 synthetic store，需要理解 metadata range 与 IR value type 的区别。 |
| 维护成本 | 4 | 仍复用现有 backing global SSA，后续统一 storage range SSA 时可以迁移这些 helper。 |

有没有更好的方案：

- 长期应把 SSA key 从 `GlobalVariable*` 扩展到明确 storage range，并把 current-value 查询独立出来。
- 本轮先在现有 backing global SSA 上做 partial extract/replace，是能验证语义的小步。

## 阶段 A 审计补充：区分 metadata shape 和 IR value shape

残留审计原来只有 `shape` 一列，表示 metadata range 是否覆盖完整 register backing。这个口径容易误导：`store i64 ..., @RAX` 配 `name=AL,size=1` 会被统计成 `partial`，但 IR 本身已经是完整 backing value。

本轮给审计脚本新增 `value_shape`：

- `shape`：metadata range 相对 register backing 是 full 还是 partial。
- `value_shape`：load/store 的 IR value type 是否等于 register backing size。

改动文件和函数：

- `scripts/native-register-residue-audit.py:35`
  - `RegisterAccess` 新增 `value_size` 和 `value_is_full`。
- `scripts/native-register-residue-audit.py:145`
  - 新增 `value_size()`，从 `load` / `store` 指令解析整数 value type。
- `scripts/native-register-residue-audit.py:223`
  - `summarize()` 的 key 增加 `value_shape`。
- `scripts/native-register-residue-audit.py:236`
  - summary 输出列增加 `value_shape`。
- `scripts/native-register-residue-audit.py:246`
  - details 输出列增加 `value_shape` / `value_size`。
- `tests/native_register_residue_audit_test.py:56`
  - 更新脚本单测，断言 full/partial metadata 和 full/partial value shape。

验证：

```bash
python3 tests/native_register_residue_audit_test.py
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-partial-storage-ssa-gate/*.signature-rewrite.ll
```

固定三目标新审计结果：

```text
category  access_kind  metadata_kind  shape    value_shape  count
gpr       load         access         full     full         39
gpr       load         access         partial  full         9
gpr       load         external_input full     full         3191
gpr       store        access         full     full         3601
gpr       store        access         partial  full         11
other     load         access         full     full         8
other     load         external_input full     full         50
other     store        access         partial  full         1
vector    load         external_input full     full         70
vector    store        access         partial  full         71
```

判断：

- 固定三目标里剩下的 GPR partial metadata load/store 都是 `value_shape=full`。
- 下一步不应再把这些当成“直接 partial IR access 未进 SSA”，而应继续处理 partial metadata/full backing value 的 prototype cleanup、callsite 当前值和 return/input 绑定。

## 阶段 D 小步：共享 return successor 的多前驱返回候选

当前 `returnTrialsBefore()` 只处理两类形态：

- return 指令前同 block 内有 ABI output register store。
- return block 沿唯一前驱链能回看到 ABI output register store。

这会漏掉一种常见 CFG：多个 predecessor 先各自写同一个返回寄存器，然后汇入同一个 shared successor，shared successor 里只有 `ret`。这种形态如果每条前驱都写了同一个 ABI output slot，且值等价，就可以作为返回候选；如果任一前驱缺 store 或值冲突，则仍要保守跳过。

本轮只补这个窄规则，不做任意 CFG value query：

- return block 自身没有 return trial 时才尝试。
- 只处理 predecessor 数量大于等于 2 的 shared successor。
- 以第一条 predecessor 的 return trial 为基准。
- 每个其它 predecessor 必须能在 terminator 前找到同 slot return trial。
- 所有 store value 必须通过 `sameReturnStoreValue()` 判定等价。
- 不处理缺 store、跨 call、不同 slot、值不等价的情况。

改动文件和函数：

- `lib/passes/NativePrototypeRecovery.cpp:1571`
  - 新增 `matchingReturnTrialForSlot()`，从一组 return trial 中按 ABI slot 查找候选。
- `lib/passes/NativePrototypeRecovery.cpp:1600`
  - 新增 `returnTrialsFromAllPredecessors()`，对 shared successor 的所有 predecessor 做保守合并。
- `lib/passes/NativePrototypeRecovery.cpp:1663`
  - `returnTrialsBefore()` 在唯一前驱链回看前，先尝试多前驱 shared successor 返回候选。
- `tests/native_prototype_recovery_test.cpp:1934`
  - 新增 `createSharedSuccessorReturnStoreFunction()`，构造同值、冲突值、缺 store 三种 shared successor 样例。
- `tests/native_prototype_recovery_test.cpp:2453`
  - 将三种样例加入主测试模块。
- `tests/native_prototype_recovery_test.cpp:2612`
  - 更新 summary 计数。
- `tests/native_prototype_recovery_test.cpp:2686`
  - 断言同值 shared predecessor 会标 `RAX` return candidate，冲突值和缺 store 不会标。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-shared-return-pred-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-shared-return-pred-gate/*.signature-rewrite.ll
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- 全量 CTest 10/10 通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 40s | 41s |
| `libuv:shared-library` | 106s | 106s |
| `memcached:executable` | 57s | 57s |

残留统计：

```text
category  access_kind  metadata_kind  shape    value_shape  count
gpr       load         access         full     full         39
gpr       load         access         partial  full         9
gpr       load         external_input full     full         3191
gpr       store        access         full     full         3601
gpr       store        access         partial  full         11
other     load         access         full     full         8
other     load         external_input full     full         50
other     store        access         partial  full         1
vector    load         external_input full     full         70
vector    store        access         partial  full         71
```

抽查：

- `uv_translate_sys_error` 仍保持 `void`，因为两条路径的 `RAX` value 不等价，本轮规则正确跳过。
- 固定三目标 residue 没有下降，但 shared successor 返回候选这个 CFG 形态已经有正反例覆盖。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | 修了真实 CFG 漏恢复形态，但固定三目标没有 residue 下降。 |
| 理解成本 | 4 | 多了一个 all-predecessor 合并路径，但条件集中且保守。 |
| 维护成本 | 4 | 后续统一 current-value 查询时，这段逻辑应迁移到共享查询接口。 |

有没有更好的方案：

- 长期应实现统一 `valueBefore(ret, storage)`，让 return recovery、callsite rewrite、cleanup 共用一套 CFG SSA 查询。
- 本轮只补 all-predecessor shared successor，是为了避免在没有统一接口前扩大 CFG 推理范围。

## 阶段 D 小步：清理无用户的 external input load

背景：

- partial store SSA 会在需要旧 backing value 时创建 entry external input load。
- 后续如果这个 partial store 又被完整 store 覆盖，死代码删除会把依赖链删掉，但 `ExternalInputValue` / `ExternalInputs` 里仍可能保留已经没有用户的 entry load。
- 这类 load 不代表真实 ABI 输入，继续保留会让 residue 统计偏高，也会给 prototype recovery 留下噪声。

本轮只做很窄的清理：

- 只删除 `NativeRegisterSSA` 自己创建的 `notdec.register.external_input` load。
- 只在 load `use_empty()` 时删除。
- 放在 `attachRegisterEffectMetadata()` 之后执行，避免影响 preserved/clobbered 判断。之前试过放到 effect metadata 之前，会让 clobbered RBX 测试失败，因为 effect 推导仍需要 entry input 对比。
- 不处理仍有用户的 external input，不推断 RSP/RBP/call effect 语义。

改动文件和函数：

- `lib/passes/NativeRegisterSSA.cpp:333`
  - `FunctionPromoter::run()` 在 `attachRegisterEffectMetadata()` 后调用 `removeDeadExternalInputs()`。
- `lib/passes/NativeRegisterSSA.cpp:650`
  - 新增 `FunctionPromoter::removeDeadExternalInputs()`，扫描 `ExternalInputValue`，删除无用户 external input load，并通过 `forgetExternalInputValue()` 同步函数级 external input 集合。
- `tests/native_register_effects_test.cpp:355`
  - 新增 `createDeadPartialInputFunction()`，构造 partial store 先引入旧 backing input、随后 full store 覆盖、再 full load 被 SSA 替换的场景。
- `tests/native_register_effects_test.cpp:677`
  - 将 `dead_partial_register_input` 样例加入主测试。
- `tests/native_register_effects_test.cpp:759`
  - 断言该样例中 RAX load 被清空，且 `notdec.register.external_inputs` metadata 不再残留。

验证：

```bash
cmake --build build --target native_register_effects_test native_prototype_recovery_test -j2
./build/bin/native_register_effects_test
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-dead-external-input-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-dead-external-input-gate/*.signature-rewrite.ll
```

结果：

- `native_register_effects_test` 通过。
- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- `notdec-native-llvm` 构建通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 41s |
| `libuv:shared-library` | 106s | 105s |
| `memcached:executable` | 56s | 57s |

残留统计变化：

```text
category  access_kind  metadata_kind  shape    value_shape  before  after
gpr       load         external_input full     full         3191    3190
other     load         external_input full     full         50      49
```

完整本轮残留：

```text
category  access_kind  metadata_kind  shape    value_shape  count
gpr       load         access         full     full         39
gpr       load         access         partial  full         9
gpr       load         external_input full     full         3190
gpr       store        access         full     full         3601
gpr       store        access         partial  full         11
other     load         access         full     full         8
other     load         external_input full     full         49
other     store        access         partial  full         1
vector    load         external_input full     full         70
vector    store        access         partial  full         71
```

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 2 | 固定三目标只少了 2 个 external input residue，但去掉了明确无用的噪声。 |
| 理解成本 | 2 | 只新增一个小清理函数，顺序约束需要记住：必须在 effect metadata 之后。 |
| 维护成本 | 2 | 后续如果统一 dead-code cleanup，可以把这段合进去。 |

有没有更好的方案：

- 更大的收益仍在 prototype rewrite 后 cleanup、callsite 当前值查询、RSP/RBP/call effect 建模。
- 本轮没有碰这些语义风险高的部分，只清理已被证明无用户的 load。

## 阶段 C/D 小步：按 ABI stack pointer 处理 call 边界

背景：

- 固定三目标里剩余 GPR access load 主要来自 `RSP/RBP`、声明调用后的 `RAX`，以及少量 callsite fallback。
- 其中 `RSP` load 有一类很常见：调用前已经 store 了当前 `RSP`，调用返回后只是为了下一次压返回地址又 load `@RSP`。
- 普通函数调用返回后，ABI stack pointer 应回到调用点的栈位置。这个事实来自 ABI 的 `stackpointer.register`，不是普通 callee-saved 寄存器规则。

本轮只做窄规则：

- 从 `notdec.abi` metadata 读取 `stackpointer.register`。
- `NativeRegisterSSA` 判断 call clobber 时，如果当前 register unit 是 ABI stack pointer，就认为普通 call 返回后不 clobber 该寄存器。
- 不把 `RSP` 加进 `unaffected`，避免把它当普通 preserved register 输出到 `notdec.register.preserves`。
- 不放宽 `RAX/RDI/RBX/RBP` 等其它寄存器。

改动文件和函数：

- `lib/passes/NativeRegisterSSA.cpp:56`
  - `AbiRegisterEffects` 增加 `StackPointerRegister` 字段，单独记录 ABI stack pointer。
- `lib/passes/NativeRegisterSSA.cpp:175`
  - `collectAbiRegisterEffects()` 从 ABI metadata 读取 `stackpointer.register`。
- `lib/passes/NativeRegisterSSA.cpp:902`
  - `FunctionPromoter::callClobbersRegister()` 对 ABI stack pointer 返回 `false`，允许跨 call 使用调用前的 RSP SSA value。
- `tests/native_register_effects_test.cpp:68`
  - 测试 ABI 显式设置 `RSP` 为 stack pointer。
- `tests/native_register_effects_test.cpp:146`
  - 新增 `createStackPointerCallEffectFunction()`，覆盖外部 call 后读取 `RSP` 的传播。
- `tests/native_register_effects_test.cpp:691`
  - 将该样例加入主测试。
- `tests/native_register_effects_test.cpp:764`
  - 断言 call 后 `RSP` load 被消除；原有 `RAX` call 后 load 保留断言继续覆盖普通 killed-by-call register 不被误传播。

验证：

```bash
cmake --build build --target native_register_effects_test -j2
./build/bin/native_register_effects_test
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-stack-pointer-call-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-stack-pointer-call-gate/*.signature-rewrite.ll
```

结果：

- `native_register_effects_test` 通过。
- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- `notdec-native-llvm` 构建通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 40s |
| `libuv:shared-library` | 105s | 106s |
| `memcached:executable` | 57s | 58s |

残留统计变化：

```text
category  access_kind  metadata_kind  shape  value_shape  before  after
gpr       load         access         full   full         39      19
```

其中 GPR full access load 按 register 变化：

```text
before: RSP=22 RAX=13 RBP=5 RBX=4 RDI=1 RCX=1 R9=1 R8=1
after:  RSP=2  RAX=13 RBP=5 RBX=4 RDI=1 RCX=1 R9=1 R8=1
```

完整本轮残留：

```text
category  access_kind  metadata_kind  shape    value_shape  count
gpr       load         access         full     full         19
gpr       load         access         partial  full         9
gpr       load         external_input full     full         3190
gpr       store        access         full     full         3601
gpr       store        access         partial  full         11
other     load         access         full     full         8
other     load         external_input full     full         49
other     store        access         partial  full         1
vector    load         external_input full     full         70
vector    store        access         partial  full         71
```

抽查：

- `notdec_native_12790` 中两次调用 `notdec_native_126d0` 前的 stack pointer 已由同 block 的 `%3` 继续传播，第二次压返回地址不再读取 `@RSP`。
- 剩余 2 个 `RSP` access load 都在 `uv_timer_stop`，还有其它 `R9/R8/RCX` register access load 混在复杂 CFG 中，本轮不继续扩大规则。
- `notdec_native_8480` 的 `%RDI.callsite_input` 不能安全替换成 entry input，因为该函数 metadata 只有 `RSP/RBP` external input，没有 `RDI`。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 4 | 固定三目标 GPR full access load 从 39 降到 19，主要解决 RSP call 边界噪声。 |
| 理解成本 | 3 | 增加一个 ABI stack pointer 特例，但它来自现有 ABI metadata，范围清楚。 |
| 维护成本 | 3 | 后续 call effect resolver 独立后，应把这个规则迁移到统一 resolver。 |

有没有更好的方案：

- 更完整的方案是独立 call effect resolver，统一处理 ABI、direct callee metadata、recovered prototype 和 stack pointer。
- 本轮先把 ABI stack pointer 从普通 killed-by-call 里拿出来，是一个小而可验证的步骤。

## 阶段 C/D 小步：声明调用输出改成 LLVM 返回值

背景：

- 上一轮后固定三目标里剩余 `RAX` access load 主要来自声明调用后的 ABI 返回寄存器读取。
- 典型形状是 `call void @strlen()` 后紧跟 `load i64, ptr @RAX, !notdec.register.access`。
- 这类声明没有真实函数体，native prototype recovery 也不能从 callee 内部恢复返回值；但 ABI 已经说明 `RAX` 是输出寄存器。

本轮只做窄规则：

- 只处理 `declare void @foo()`，且所有 user 都是零参数 `call void @foo()`。
- 只处理同一个 basic block 内，声明调用之后、目标寄存器读取之前没有同 `base` register store 的 load。
- 只把 `i64` ABI output register load 改成调用结果。
- 把声明从 `declare void @foo()` 重建成 `declare i64 @foo()`，并把匹配 callsite 改成 `call i64 @foo()`。
- 不处理带参数声明、不处理非 `i64` 返回、不跨 block 推断、不处理间接调用。

改动文件和函数：

- `lib/passes/NativePrototypeRecovery.cpp:343`
  - 新增 `registerAccessBase()`，按 metadata `base` 判断同一个物理寄存器，避免 `RAX/EAX` 这种 name 不同但 base 相同的场景误穿过 store。
- `lib/passes/NativePrototypeRecovery.cpp:351`
  - `isDeclarationCallOutputLoad()` 改成用 `base` 检查 store 屏障。
- `lib/passes/NativePrototypeRecovery.cpp:377`
  - 新增 `declarationCallOutputSource()`，找到同 block 内能作为输出来源的声明调用。
- `lib/passes/NativePrototypeRecovery.cpp:406`
  - 新增 `canRewriteDeclarationCallOutputLoad()`，限制 load 类型必须是 `i64` 且 base 必须是 ABI output register。
- `lib/passes/NativePrototypeRecovery.cpp:420`
  - 新增 `DeclarationCallOutputRewrite`，记录一条安全的 call/load 替换关系。
- `lib/passes/NativePrototypeRecovery.cpp:428`
  - 新增 `collectDeclarationCallOutputRewrites()`，按 declaration callee 收集可重写的 call output load，并过滤有复杂 user 的 declaration。
- `lib/passes/NativePrototypeRecovery.cpp:478`
  - 新增 `rewriteDeclarationCallOutputs()`，重建 declaration、替换 call、用 call result 替换 register load。
- `lib/passes/NativePrototypeRecovery.cpp:2256`
  - 在 signature rewrite 后调用 `rewriteDeclarationCallOutputs()`。
- `tests/native_prototype_recovery_test.cpp:1653`
  - 新增 `createDeclarationCallOutputCallerFunction()` 测试构造函数。
- `tests/native_prototype_recovery_test.cpp:2211`
  - 新增 `hasRegisterLoad()`，用于确认 register access load 是否还存在。
- `tests/native_prototype_recovery_test.cpp:4798`
  - 新增正向测试：`declare void` 调用后的 `RAX` load 被 `call i64` 结果替换。
- `tests/native_prototype_recovery_test.cpp:4840`
  - 新增负向测试：`RAX` store 后再读 `EAX` 时不能跨同 base store 重写。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
./build/bin/native_register_effects_test
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-decl-call-output-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-decl-call-output-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-decl-call-output-gate/*.signature-rewrite.ll \
  > /tmp/notdec-reg-details-decl-call-output.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- `notdec-native-llvm` 构建通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 40s |
| `libuv:shared-library` | 106s | 105s |
| `memcached:executable` | 57s | 57s |

残留统计变化：

```text
category  access_kind  metadata_kind  shape    value_shape  before  after
gpr       load         access         full     full         19      13
gpr       load         access         partial  full         9       3
```

完整本轮残留：

```text
category  access_kind  metadata_kind  shape    value_shape  count
gpr       load         access         full     full         13
gpr       load         access         partial  full         3
gpr       load         external_input full     full         3190
gpr       store        access         full     full         3601
gpr       store        access         partial  full         11
other     load         access         full     full         8
other     load         external_input full     full         49
other     store        access         partial  full         1
vector    load         external_input full     full         70
vector    store        access         partial  full         71
```

剩余 GPR access load 按 `base/name`：

```text
5 RBP/RBP
2 RSP/RSP
2 RBX/RBX
2 RBX/EBX
1 RDI/RDI
1 RCX/RCX
1 RAX/EAX
1 R9/R9
1 R8/R8
```

抽查：

- 原先多处 `strlen`、`socket`、`bind`、`calloc`、`SSL_read`、`SSL_write` 等声明调用后的 `RAX` 读取被改成直接使用 `call i64` 结果。
- 剩余 `RAX/EAX` 是 partial/full 形态，且前面存在同 base store，不在本轮安全范围内。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 4 | 固定三目标 GPR access load 从 28 降到 16，主要解决声明调用返回值噪声。 |
| 理解成本 | 3 | 增加一段 declaration 专用 rewrite，但限制很窄，入口集中在 signature rewrite 后。 |
| 维护成本 | 3 | 后续如果有统一 call effect resolver，应把 declaration output 也迁进去。 |

有没有更好的方案：

- 更完整方案是把外部函数原型恢复接到 ABI 和符号信息上，直接生成正确 declaration 类型。
- 本轮只根据现有 ABI output register 和局部 IR 形状做保守替换，不猜参数和非 `i64` 返回。

## 阶段 D 小步：删除安全的 callsite 输入寄存器 store

背景：

- signature rewrite 已经能把 `call void @callee()` 改成 `call void @callee(i64 arg)`。
- 但 caller 里很多调用前 ABI input store 仍保留，例如 `store i64 1, ptr @RDI` 后马上 `call @callee(i64 1)`。
- 这类 store 已经不再用于给 callee 传参；如果调用会 clobber 该寄存器，且 caller 自己没有返回候选依赖，就可以删除。

本轮只做窄规则：

- 只处理同一个 basic block 内，call 前最近的同名 register store。
- 中间遇到同 register load、非 intrinsic call，认为不安全。
- 只有 store value 正好是 rewrite 后 call argument 时才删除。
- 只有 callee effect 表示该输入寄存器会被 clobber 时才删除；ABI unaffected / preserved 的寄存器不删。
- caller 函数如果还有 `notdec.prototype.return_candidates`，不删。原因是 prototype rewrite batch 仍会用这些 store 推导 caller 自己的返回绑定。

实现中踩过的问题：

- 初版只按 callee clobber 判断，固定三目标里 `memcached` 的 `notdec_native_19910` 从 rewritten 退成 `missing return binding`。
- 原因是 caller 自己还有 return candidate；删除一个调用前 store 后影响了后续返回绑定搜索。
- 最终把规则收窄为 caller 没有 return candidates 才删，固定 gate 恢复通过。

改动文件和函数：

- `lib/passes/NativePrototypeRecovery.cpp:549`
  - 增加 `callClobbersRegister()` 前置声明，供 callsite rewrite 收集阶段使用。
- `lib/passes/NativePrototypeRecovery.cpp:576`
  - 新增 `localCallsiteInputStoreBeforeCall()`，只查同 block、call 前、无同 register load / 普通 call 屏障的输入 store。
- `lib/passes/NativePrototypeRecovery.cpp:962`
  - `MultiInputCallsiteRewrite` 增加 `InputStores`，和 ABI 参数顺序对齐保存可删除 store。
- `lib/passes/NativePrototypeRecovery.cpp:975`
  - `collectMultiInputDirectCallsiteRewrites()` 在确定 argument 后记录安全 input store。
- `lib/passes/NativePrototypeRecovery.cpp:1016`
  - 新增 `eraseCallsiteInputStores()`，去重删除已经确认安全的 store。
- `lib/passes/NativePrototypeRecovery.cpp:1025`
  - `rewriteMultiInputDirectCallsites()` 在替换 call 后删除安全 input store。
- `lib/passes/NativePrototypeRecovery.cpp:1655`
  - `InputMultiReturnCallsiteRewrite` 同样记录 `InputStores`。
- `lib/passes/NativePrototypeRecovery.cpp:1639`
  - `collectInputMultiReturnDirectCallsites()` 复用同样规则记录安全 input store。
- `lib/passes/NativePrototypeRecovery.cpp:1689`
  - `rewriteInputMultiReturnDirectCallsites()` 在替换 call 后删除安全 input store。
- `tests/native_prototype_recovery_test.cpp:130`
  - 新增 `attachPreservedInputTestAbi()`，构造“输入寄存器同时是 ABI unaffected”的测试 ABI。
- `tests/native_prototype_recovery_test.cpp:2951`
  - 断言 clobbered `RDI` direct callsite rewrite 后删除旧 input store。
- `tests/native_prototype_recovery_test.cpp:2960`
  - 新增 preserved `RBX` 负例，确认 caller-visible register store 不被删除。
- `tests/native_prototype_recovery_test.cpp:3001`
  - 新增 caller 带 return candidate 的负例，确认不破坏后续 return binding。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
./build/bin/native_register_effects_test
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-callsite-input-store-safe-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-callsite-input-store-safe-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-callsite-input-store-safe-gate/*.signature-rewrite.ll \
  > /tmp/notdec-reg-details-callsite-input-store-safe.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- `notdec-native-llvm` 构建通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 40s | 41s |
| `libuv:shared-library` | 106s | 106s |
| `memcached:executable` | 57s | 58s |

残留统计变化：

```text
category  access_kind  metadata_kind  shape  value_shape  before  after
gpr       store        access         full   full         3601    3578
```

完整本轮残留：

```text
category  access_kind  metadata_kind  shape    value_shape  count
gpr       load         access         full     full         13
gpr       load         access         partial  full         3
gpr       load         external_input full     full         3190
gpr       store        access         full     full         3578
gpr       store        access         partial  full         11
other     load         access         full     full         8
other     load         external_input full     full         49
other     store        access         partial  full         1
vector    load         external_input full     full         70
vector    store        access         partial  full         71
```

剩余 GPR store access 按 `base/name` 前几项：

```text
1132 RSP/RSP
742 RBP/RBP
270 RAX/RAX
240 RDI/RDI
199 RBX/RBX
183 RSI/RSI
167 R12/R12
146 RCX/RCX
135 RDX/RDX
```

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | 固定三目标 GPR full store 下降 23 个，收益小但直接对应 prototype 后 cleanup。 |
| 理解成本 | 4 | callsite rewrite 多带一个 store 来源列表，并要理解 return candidate 的保护条件。 |
| 维护成本 | 4 | 后续统一 current-value / call effect resolver 后，应把这个规则并入统一 callsite cleanup。 |

有没有更好的方案：

- 更完整方案是让 prototype rewrite 分阶段：先完成所有函数自身 input/return binding，再做 caller-side input store cleanup。
- 当前 batch rewrite 还会用 caller 的 return candidates，因此本轮用 `return_candidates == nullptr` 做保守保护。

## 阶段 D 小步：声明调用返回 load 支持无 access metadata

背景：

- 前一轮已经能把 `call void @external(); load @RAX` 改成 `call i64 @external()`，但要求 load 自己带 `notdec.register.access`。
- Bench2 里仍有一些声明调用后的 `load i64, ptr @RAX`，load 没有 access metadata，但 `@RAX` global 仍有 `notdec.register` metadata。
- 这些 load 不会被 `native-register-residue-audit.py` 的 `access` 统计覆盖，但仍是实际寄存器 global 流量。

本轮只放宽声明调用 output load 识别：

- load/store 自己有 `notdec.register.access` 时，仍按原 metadata 的 `base` 判断。
- load/store 没有 access metadata 时，只在 pointer 是带 `notdec.register` metadata 的 register global 时，用 global 的 `name` 当 base。
- 仍要求同一 basic block 内，load 前最近的非 intrinsic call 是 declaration，且中间没有同 base store。
- 仍只处理 ABI output register 和 `i64` 返回，不推断其他类型。

改动文件和函数：

- `lib/passes/NativePrototypeRecovery.cpp:351`
  - 新增 `registerStorageBase()`，统一从 instruction access metadata 或 register global metadata 取 base。
- `lib/passes/NativePrototypeRecovery.cpp:376`
  - `isDeclarationCallOutputLoad()` 改用 `registerStorageBase()`，允许无 access metadata 的 register global load。
- `lib/passes/NativePrototypeRecovery.cpp:402`
  - `declarationCallOutputSource()` 同样改用 `registerStorageBase()`，保证同 base store 仍能阻断。
- `lib/passes/NativePrototypeRecovery.cpp:431`
  - `canRewriteDeclarationCallOutputLoad()` 改用 `registerStorageBase()` 查询 ABI output register。
- `tests/native_prototype_recovery_test.cpp:4923`
  - 在 declaration call output rewrite 测试里清掉 load 的 `notdec.register.access`，确认只靠 `@RAX` global metadata 也能 rewrite。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
./build/bin/native_register_effects_test
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-decl-output-global-load-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-decl-output-global-load-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-decl-output-global-load-gate/*.signature-rewrite.ll \
  > /tmp/notdec-reg-details-decl-output-global-load.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- `notdec-native-llvm` 构建通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 41s |
| `libuv:shared-library` | 106s | 106s |
| `memcached:executable` | 57s | 56s |

`native-register-residue-audit.py` 统计不变，因为它只统计带 access/external_input metadata 的访问：

```text
category  access_kind  metadata_kind  shape    value_shape  count
gpr       load         access         full     full         13
gpr       load         access         partial  full         3
gpr       load         external_input full     full         3190
gpr       store        access         full     full         3578
gpr       store        access         partial  full         11
other     load         access         full     full         8
other     load         external_input full     full         49
other     store        access         partial full         1
vector    load         external_input full     full         70
vector    store        access         partial  full         71
```

但无 metadata 的 register global load 明显下降：

```text
pattern                                      before  after
%EAX = load i64, ptr @RAX, align 4          33      1
load i64 from all register globals, no md    56      9
```

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | 不改变 metadata 审计数字，但固定三目标里无 metadata `@RAX` 返回 load 基本清掉。 |
| 理解成本 | 2 | 只是把“register base”查询从 instruction metadata 放宽到 register global metadata。 |
| 维护成本 | 2 | 规则仍集中在 declaration call output rewrite，后续统一 storage 查询时可直接迁走。 |

有没有更好的方案：

- 更完整方案是让前面的 pass 不丢掉普通 register global load 的 access metadata。
- 当前补法更窄，只服务已经有严格形状约束的 declaration call output rewrite。

## 阶段 A/D 小步：补齐无 metadata 的 direct register global access

背景：

- 上一轮 declaration call output cleanup 后，固定三目标里仍有 9 个 `load i64, ptr @REG` 没有任何 `notdec.register.access` metadata。
- 这些 load 实际仍是 register global 流量，但 residue audit 看不到，后续 SSA / cleanup 也只能靠零散 fallback 处理。
- 这类访问不能简单忽略，否则“寄存器残留数量”会被低估。

本轮规则：

- `NativeRegisterSSA` 遇到 direct register global load/store，如果 instruction 没有 `notdec.register.access`，但 global 有 `notdec.register`，按 full-unit access 处理。
- 普通 load/store 会补一个 full-unit `notdec.register.access` metadata。
- synthetic external input load 不补 `notdec.register.access`，只保留 `notdec.register.external_input`，避免 residue audit 把 external input 双计成普通 access。
- prototype rewrite 的 unsafe return load 检查忽略 external input load。输入直接转返回是合法绑定，不应因为 load 同时可能被识别为 register access 而跳过。

实现中踩过的问题：

- 初版给所有 load 都补 `notdec.register.access`，包括 `*.external_input`。
- 这让 `vsftpd` 里 `notdec_native_151b0` / `notdec_native_151d0` 的 input-forward-return 从 rewritten 退成 `unsafe return value load`。
- 最终修正为：external input load 不补 access metadata；并且 `hasUnsafeReturnValueLoad()` 显式忽略 external input。

改动文件和函数：

- `lib/passes/NativeRegisterSSA.cpp:168`
  - 新增 `fullRegisterAccessMetadata()`，从 `RegisterUnit` 生成 full-unit access metadata。
- `lib/passes/NativeRegisterSSA.cpp:232`
  - `registerLoad()` 支持无 instruction access metadata 的 direct register global load；普通 load 补 metadata，external input load 不补。
- `lib/passes/NativeRegisterSSA.cpp:269`
  - `registerStore()` 支持无 instruction access metadata 的 direct register global store，并补 full-unit metadata。
- `lib/passes/NativePrototypeRecovery.cpp:543`
  - `hasUnsafeReturnValueLoad()` 忽略带 `notdec.register.external_input` 的 load / trunc(load)。
- `tests/native_register_effects_test.cpp:337`
  - 新增 `createUnmarkedRegisterStoreLoadFunction()`，覆盖无 metadata store/load 仍能进入 SSA。
- `tests/native_register_effects_test.cpp:642`
  - 新增 `hasRegisterAccessLoad()` 测试 helper。
- `tests/native_register_effects_test.cpp:735`
  - 接入无 metadata store/load 测试函数。
- `tests/native_register_effects_test.cpp:821`
  - 断言 external input load 不被补 access metadata，保持审计口径。
- `tests/native_register_effects_test.cpp:823`
  - 断言无 metadata RAX store/load 被 SSA 消除。
- `tests/native_prototype_recovery_test.cpp:2648`
  - 给 input-forward-return 的 external input load 加 access metadata，确认 prototype rewrite 不再误判 unsafe return value load。

验证：

```bash
cmake --build build --target native_register_effects_test native_prototype_recovery_test -j2
./build/bin/native_register_effects_test
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-full-register-access-md-clean-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-full-register-access-md-clean-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-full-register-access-md-clean-gate/*.signature-rewrite.ll \
  > /tmp/notdec-reg-details-full-register-access-md-clean.tsv
```

结果：

- `native_register_effects_test` 通过。
- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- `notdec-native-llvm` 构建通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 42s | 41s |
| `libuv:shared-library` | 107s | 106s |
| `memcached:executable` | 57s | 57s |

残留统计变化：

```text
category  access_kind  metadata_kind    shape    value_shape  before  after
gpr       load         access           full     full         13      22
gpr       load         external_input   full     full         3190    3190
vector    load         access           full     full         0       1
vector    load         external_input   full     full         70      70
```

这里 `gpr load access` 上升不是回退，而是之前 9 个无 metadata direct global load 现在被纳入审计。无 metadata register global load 已清零：

```text
pattern                                      before  after
load i64 from register globals, no md        9       0
```

剩余可见的普通 GPR access load 主要是复杂 CFG / frame restore / callee-saved 场景：

```text
notdec_native_9c38 R14/R14
notdec_native_9c38 RBP/RBP
uv_timer_stop RSP/RSP, R8/R8, R9/R9, RCX/RCX
notdec_native_126d0 RBX/EBX, RBP/RBP
notdec_native_12b70 RBX/EBX
notdec_native_12c00 RBX/RBX, RBP/RBP
```

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | 没直接减少 metadata access 数，但修正审计盲区，后续 cleanup 不再漏看 direct global access。 |
| 理解成本 | 3 | `registerLoad/registerStore` 多了 metadata fallback，并要理解 external input 不双计。 |
| 维护成本 | 3 | 后续如果有统一 storage access parser，应把这个 fallback 收进统一接口。 |

有没有更好的方案：

- 最好是在 P-Code lowering 阶段保证所有 register global access 都带准确 access metadata。
- 本轮是在 SSA pass 入口补齐 full-unit fallback，范围只限 direct register global，避免猜复杂 pointer alias。

## 阶段 E 小步：stack input 的 callee 侧 signature rewrite

背景：

- stack input candidate 已能通过 `notdec.stack.input` metadata 恢复到 `notdec.prototype.recovered`。
- 但之前 `buildNativeRecoveredPrototypeFunctionType()` 遇到 `storage=stack` 会直接返回 unsupported，导致 stack input 只能记录，不能进入函数签名。
- 阶段 E 的完整目标还包括 direct callsite 从调用点 stack storage 取值。本轮先做 callee 侧最小实现，不猜 caller 栈。

本轮范围：

- 只支持 size 为 8 的 stack input，统一生成 `i64` 参数。
- `getNativePrototypeInputBindings()` 支持把 recovered stack input 绑定到唯一的 `notdec.stack.input` load。
- input-only、input-return、input-multi-return rewrite 都可用同一套 input binding 替换 callee 内部 load。
- 如果函数有调用者且输入里有 stack input，先返回 `unsupported stack callsite input`，不做 callsite rewrite。

不做：

- 不从任意 GEP 或栈指针表达式猜 stack 参数，只使用已有 `notdec.stack.input` metadata。
- 不实现 caller 侧 stack argument value 查询。
- 不处理非 8 字节 stack input、varargs、动态栈调整、复杂 alias。

改动文件和函数：

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:91`
  - `NativePrototypeInputBinding` 增加 `StackInputLoad`，让 register input 和 stack input 共用绑定结构。
- `lib/passes/NativePrototypeRecovery.cpp:335`
  - 新增 `uniqueStackInputLoad()`，按 `space/offset/size` 找唯一 `notdec.stack.input` load。
- `lib/passes/NativePrototypeRecovery.cpp:2450`
  - `buildNativeRecoveredPrototypeFunctionType()` 接受 8 字节 stack input，生成 `i64` 参数；return 仍只支持 register。
- `lib/passes/NativePrototypeRecovery.cpp:2524`
  - `getNativePrototypeInputBindings()` 支持 register external input 和 stack input 两种绑定。
- `lib/passes/NativePrototypeRecovery.cpp:2560`
  - 新增 `inputBindingLoad()`，rewrite 时统一取 input load。
- `lib/passes/NativePrototypeRecovery.cpp:2567`
  - 新增 `hasStackInputBinding()`，callsite rewrite 遇到 stack input 时保守跳过。
- `lib/passes/NativePrototypeRecovery.cpp:2847`
  - `rewriteNativeRecoveredPrototypeInputOnly()` 用统一 input binding 替换 load；有调用者且含 stack input 时跳过。
- `lib/passes/NativePrototypeRecovery.cpp:2943`
  - `rewriteNativeRecoveredPrototypeInputReturn()` 同样支持 stack input binding，并保守跳过 stack callsite。
- `lib/passes/NativePrototypeRecovery.cpp:3196`
  - `rewriteNativeRecoveredPrototypeInputMultiReturn()` 同样支持 stack input binding，并保守跳过 stack callsite。
- `tests/native_prototype_recovery_test.cpp:1374`
  - `createStackInputFunction()` 增加可选输出 `StackInputLoad`。
- `tests/native_prototype_recovery_test.cpp:2688`
  - stack input 现在可 rewrite，summary 期望从 51/31 调整为 52/32。
- `tests/native_prototype_recovery_test.cpp:2782`
  - 断言 stack input recovered prototype 能构建 `void(i64)`，且 rewrite eligibility 为 true。
- `tests/native_prototype_recovery_test.cpp:2859`
  - 断言 stack input binding 指向正确的 `notdec.stack.input` load。
- `tests/native_prototype_recovery_test.cpp:2928`
  - 断言 stack input-only rewrite 后函数类型为 `void(i64)`，原 stack load use 被新参数替换。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
./build/bin/native_register_effects_test
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-stack-input-callee-final-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-stack-input-callee-final-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-stack-input-callee-final-gate/*.signature-rewrite.ll \
  > /tmp/notdec-reg-details-stack-input-callee-final.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- `notdec-native-llvm` 构建通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 41s |
| `libuv:shared-library` | 107s | 107s |
| `memcached:executable` | 57s | 57s |

signature rewrite 统计：

```text
target      needed  rewritten  unsupported stack callsite input
vsftpd      137     137        0
libuv       336     336        0
memcached   187     187        0
```

残留统计与上一轮相同：

```text
category  access_kind  metadata_kind    shape    value_shape  count
gpr       load         access           full     full         22
gpr       load         access           partial  full         3
gpr       load         external_input   full     full         3190
gpr       store        access           full     full         3583
gpr       store        access           partial  full         11
other     load         access           full     full         8
other     load         external_input   full     full         49
other     store        access           partial  full         1
vector    load         access           full     full         1
vector    load         external_input   full     full         70
vector    store        access           partial  full         71
```

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | 阶段 E 从“只能记录 stack input”推进到“callee 内部能变成 LLVM 参数”；固定三目标暂未出现新增 residue 下降。 |
| 理解成本 | 3 | input binding 从单一 register load 扩展成 register/stack 两类，但 rewrite 入口仍集中。 |
| 维护成本 | 3 | 后续 caller-side stack value 查询实现后，需要替换当前 `unsupported stack callsite input` 保护。 |

有没有更好的方案：

- 完整方案是同时实现 direct callsite stack argument 查询和 rewrite。
- 本轮先把 callee 侧 load-to-argument 打通，避免在 caller 栈语义不明确时猜值。

## 阶段 E 小步：stack input 的简单 direct callsite rewrite

背景：

- 上一轮已经能把 callee 内部 `notdec.stack.input` load 替换成 LLVM 参数。
- 但只要这个函数有 caller，就会因为 `unsupported stack callsite input` 跳过。
- 本轮只补一个很窄的 caller 侧查询：调用点前同一 basic block 内，存在唯一匹配的 `notdec.stack.input` load，且类型和参数一致。

本轮范围：

- register input 仍走原来的寄存器查询和 store 清理。
- stack input 只从调用点前本地 `notdec.stack.input` load 取值。
- 遇到非 intrinsic call、多个匹配 load、类型不匹配、跨 CFG、找不到 metadata，都返回 `unsafe callsite input value`，不 rewrite。
- input-only 和 input-return 两条 direct callsite 路径可用 stack input。

不做：

- 不从任意 GEP / 栈指针表达式猜参数。
- 不跨 basic block 追 stack 值。
- 不删除 caller 侧 stack load。
- 不处理 input-multi-return 的 stack callsite；这条路径还有单独的 callsite rewrite 结构。

改动文件和函数：

- `lib/passes/NativePrototypeRecovery.cpp:335`
  - 新增 `stackInputMetadataMatches()`，统一比较 `space/offset/size`。
- `lib/passes/NativePrototypeRecovery.cpp:344`
  - `uniqueStackInputLoad()` 改为复用 `stackInputMetadataMatches()`。
- `lib/passes/NativePrototypeRecovery.cpp:1024`
  - 新增 `localStackInputValueBeforeCall()`，只在调用点前同一 basic block 内找唯一匹配 stack input load。
- `lib/passes/NativePrototypeRecovery.cpp:1058`
  - 新增按 `NativeRecoveredPrototypeParam::StorageKind` 分派的 `callsiteInputValueBeforeCall()`。
- `lib/passes/NativePrototypeRecovery.cpp:1096`
  - `collectMultiInputDirectCallsiteRewrites()` 改为支持 register/stack input 混合；只有 register input 才尝试删除 caller 侧 input store。
- `lib/passes/NativePrototypeRecovery.cpp:2907`
  - `rewriteNativeRecoveredPrototypeInputOnly()` 移除 stack callsite 统一跳过，交给新查询决定是否安全。
- `lib/passes/NativePrototypeRecovery.cpp:3024`
  - `rewriteNativeRecoveredPrototypeInputReturn()` 同样移除 stack callsite 统一跳过。
- `tests/native_prototype_recovery_test.cpp:1408`
  - 新增 `createStackInputCallerFunction()`，构造调用点前带 `notdec.stack.input` load 的 caller。
- `tests/native_prototype_recovery_test.cpp:3041`
  - 新增 stack input direct callsite rewrite 断言，确认 rewritten call 的实参来自 caller 侧 stack input load。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
./build/bin/native_register_effects_test
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-stack-input-callsite-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-stack-input-callsite-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-stack-input-callsite-gate/*.signature-rewrite.ll \
  > /tmp/notdec-reg-details-stack-input-callsite.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- `notdec-native-llvm` 构建通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 41s |
| `libuv:shared-library` | 105s | 107s |
| `memcached:executable` | 57s | 57s |

signature rewrite 统计：

```text
target      needed  rewritten
vsftpd      137     137
libuv       336     336
memcached   187     187
```

残留统计与上一轮相同：

```text
category  access_kind  metadata_kind    shape    value_shape  count
gpr       load         access           full     full         22
gpr       load         access           partial  full         3
gpr       load         external_input   full     full         3190
gpr       store        access           full     full         3583
gpr       store        access           partial  full         11
other     load         access           full     full         8
other     load         external_input   full     full         49
other     store        access           partial  full         1
vector    load         access           full     full         1
vector    load         external_input   full     full         70
vector    store        access           partial  full         71
```

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | 打通了最简单 stack direct callsite，用单测覆盖；固定三目标暂未命中新场景，所以 residue 不变。 |
| 理解成本 | 3 | callsite input 查询开始按 storage kind 分派，但 stack 分支很小，边界清楚。 |
| 维护成本 | 3 | 后续跨 block stack value 或 input-multi-return 需要扩展同一入口，不能在各 rewrite 路径继续复制逻辑。 |

有没有更好的方案：

- 更完整的做法是阶段 C 的统一 current-value 查询，stack/register 都走同一套 CFG 查询。
- 本轮先用 metadata 限定一个窄 case，是为了避免在 stack alias 还没建模时猜值。

## 阶段 E 小步：stack input 的 input-multi-return callsite rewrite

背景：

- 上一轮只让 input-only 和 input-return 两条 direct callsite 路径支持简单 stack input。
- input-multi-return 仍然在有 stack input 且存在 caller 时返回 `unsupported stack callsite input`。
- 这条路径的 input 查询逻辑和前两条路径重复，适合直接复用上轮新增的 storage-kind 分派查询。

本轮范围：

- `input + multi-return` 的 direct callsite 支持简单 stack input。
- stack input 仍只接受调用点前同一 basic block 内唯一匹配的 `notdec.stack.input` load。
- register input 仍保持原来的 store 查询和可删除 input store 逻辑。
- return load 处理不变，仍沿用已有 multi-return 的 `extractvalue` / shared successor PHI 逻辑。

不做：

- 不跨 basic block 追 stack 值。
- 不从 GEP 或栈指针表达式猜参数。
- 不删除 caller 侧 stack load。
- 不扩大 return load 搜索范围。

改动文件和函数：

- `lib/passes/NativePrototypeRecovery.cpp:1775`
  - `collectInputMultiReturnDirectCallsites()` 改为调用按 `NativeRecoveredPrototypeParam::StorageKind` 分派的 `callsiteInputValueBeforeCall()`。
  - 只有 register input 才查询和删除 caller 侧 input store；stack input 的 `InputStores` 保持空。
- `lib/passes/NativePrototypeRecovery.cpp:3281`
  - `rewriteNativeRecoveredPrototypeInputMultiReturn()` 移除 stack input 的统一跳过，交给 callsite input 查询判断是否安全。
- `tests/native_prototype_recovery_test.cpp:1441`
  - 新增 `createStackInputReturnLoadCallerFunction()`，构造 caller 侧 stack input load、direct call 和两个 return register load。
- `tests/native_prototype_recovery_test.cpp:2149`
  - 新增 `createStackInputTwoOutputReturnStoreFunction()`，构造 stack input + 两个 register return 的 callee。
- `tests/native_prototype_recovery_test.cpp:2685`
  - 增加 stack input multi-return 的 callee/caller 样例。
- `tests/native_prototype_recovery_test.cpp:2807`
  - summary 期望随新增样例调整：函数数 `52 -> 54`，input candidate `21 -> 22`，return candidate `34 -> 36`，rewrite eligible `52 -> 54`，signature rewrite needed `32 -> 33`。
- `tests/native_prototype_recovery_test.cpp:6076`
  - 断言 stack input multi-return 能 rewrite，调用点参数来自 caller 侧 stack input load，返回值变为 struct，旧 return register load 被替换。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
./build/bin/native_register_effects_test
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-stack-input-multireturn-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-stack-input-multireturn-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-stack-input-multireturn-gate/*.signature-rewrite.ll \
  > /tmp/notdec-reg-details-stack-input-multireturn.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- `notdec-native-llvm` 构建通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 40s |
| `libuv:shared-library` | 105s | 106s |
| `memcached:executable` | 57s | 57s |

signature rewrite 统计：

```text
target      needed  rewritten
vsftpd      137     137
libuv       336     336
memcached   187     187
```

残留统计与上一轮相同：

```text
category  access_kind  metadata_kind    shape    value_shape  count
gpr       load         access           full     full         22
gpr       load         access           partial  full         3
gpr       load         external_input   full     full         3190
gpr       store        access           full     full         3583
gpr       store        access           partial  full         11
other     load         access           full     full         8
other     load         external_input   full     full         49
other     store        access           partial  full         1
vector    load         access           full     full         1
vector    load         external_input   full     full         70
vector    store        access           partial  full         71
```

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | 把 stack input direct callsite 支持补到 input-multi-return 路径；固定三目标未命中新场景，所以 residue 不变。 |
| 理解成本 | 2 | 主要是删除一处重复 register-only 查询，复用已有 storage-kind 查询入口。 |
| 维护成本 | 3 | 后续如果做统一 current-value 查询，需要把这三条 direct callsite 路径继续合并，避免逻辑再分叉。 |

有没有更好的方案：

- 更完整方案仍然是阶段 C，把 register/stack/current value 查询集中成独立模块。
- 本轮只消掉一处明确重复和保守跳过，不提前处理复杂 stack CFG。

## 阶段 C/E 小步：stack input callsite 的唯一前驱查询

背景：

- 前两轮已经让三条 direct callsite rewrite 路径支持简单 stack input。
- 但 caller 侧 stack input 只支持和 call 在同一 basic block 内。
- 很多简单 CFG 会在前一个 block 里准备参数，然后跳到 call block。这个场景不需要完整 stack alias，也不需要跨多前驱合并。

本轮范围：

- stack input callsite 查询从“同一 basic block”扩到“唯一 predecessor 链”。
- 每个 block 内仍只接受唯一匹配的 `notdec.stack.input` load。
- 遇到多 predecessor、普通 call、循环、类型不匹配、多个匹配 load，都返回 `unsafe callsite input value`。
- register input 查询不变。

不做：

- 不在多 predecessor 上比较 stack input 是否等价。
- 不跨普通 call 追栈值。
- 不从 GEP 或栈指针表达式猜参数。
- 不删除 caller 侧 stack load。

改动文件和函数：

- `lib/passes/NativePrototypeRecovery.cpp:1024`
  - 新增 `stackInputLoadInReverseRange()`，在一段反向指令范围内找唯一匹配 stack input load，并把非 intrinsic call 当成屏障。
- `lib/passes/NativePrototypeRecovery.cpp:1057`
  - `localStackInputValueBeforeCall()` 改为复用 `stackInputLoadInReverseRange()`。
- `lib/passes/NativePrototypeRecovery.cpp:1065`
  - 新增 `stackInputValueBeforeCall()`，沿唯一 predecessor 链查找 stack input load；多 predecessor、普通 call、循环都保守失败。
- `lib/passes/NativePrototypeRecovery.cpp:1110`
  - stack input 的 `callsiteInputValueBeforeCall()` 改为调用 `stackInputValueBeforeCall()`。
- `tests/native_prototype_recovery_test.cpp:1441`
  - 新增 `createStackInputUniquePredecessorCallerFunction()`，构造 stack input 在前驱 block、call 在后继 block 的正例。
- `tests/native_prototype_recovery_test.cpp:1476`
  - 新增 `createStackInputAmbiguousPredecessorCallerFunction()`，构造两个 predecessor 都有 stack input 的保守跳过反例。
- `tests/native_prototype_recovery_test.cpp:3259`
  - 断言唯一前驱 stack input callsite 能 rewrite，参数来自前驱 block 的 stack input load。
- `tests/native_prototype_recovery_test.cpp:3311`
  - 断言多前驱 stack input callsite 不 rewrite，skip reason 为 `unsafe callsite input value`。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
./build/bin/native_register_effects_test
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-stack-input-pred-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-stack-input-pred-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-stack-input-pred-gate/*.signature-rewrite.ll \
  > /tmp/notdec-reg-details-stack-input-pred.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- `notdec-native-llvm` 构建通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 41s |
| `libuv:shared-library` | 105s | 106s |
| `memcached:executable` | 56s | 57s |

signature rewrite 统计：

```text
target      needed  rewritten
vsftpd      137     137
libuv       336     336
memcached   187     187
```

残留统计与上一轮相同：

```text
category  access_kind  metadata_kind    shape    value_shape  count
gpr       load         access           full     full         22
gpr       load         access           partial  full         3
gpr       load         external_input   full     full         3190
gpr       store        access           full     full         3583
gpr       store        access           partial  full         11
other     load         access           full     full         8
other     load         external_input   full     full         49
other     store        access           partial  full         1
vector    load         access           full     full         1
vector    load         external_input   full     full         70
vector    store        access           partial  full         71
```

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | 覆盖了简单跨 block stack input callsite；固定三目标没有命中新场景，所以 residue 不变。 |
| 理解成本 | 3 | stack input 查询多了一层唯一前驱回溯，但失败条件清楚。 |
| 维护成本 | 3 | 后续应把 register/stack value-before-call 查询收进阶段 C 的统一接口，避免继续分散。 |

有没有更好的方案：

- 更完整方案是统一 current-value 查询，并支持 PHI / 多前驱等价判断。
- 本轮只处理唯一前驱链，是为了在不引入 stack alias 和 PHI 复杂度的情况下先覆盖简单 CFG。

## 阶段 A 小步：残留审计区分 synthetic partial store

背景：

- 最新固定三目标里，vector 长尾很集中：`vector load external_input full/full 70`，`vector store access partial/full 71`。
- 这些 store 看起来像 partial SSA 生成的 RMW 残留，但实际需要先区分“`NativeRegisterSSA` 合成的 store”和“lowering 原本生成的 full backing store + partial metadata”。
- 如果误把这两类混在一起，后续 cleanup 容易删掉真实保留 upper lane 的写。

本轮范围：

- residue audit 新增 `synthetic` 维度。
- 识别 `!notdec.register.synthetic` metadata。
- `--details | head` 这种常见用法不再因 SIGPIPE 打 traceback。
- 不改 IR 生成，不改寄存器消除语义。

改动文件和函数：

- `scripts/native-register-residue-audit.py:10`
  - 引入 `signal`，在 `main()` 里设置 SIGPIPE 默认处理。
- `scripts/native-register-residue-audit.py:19`
  - 新增 `SYNTHETIC_RE`，匹配 `!notdec.register.synthetic`。
- `scripts/native-register-residue-audit.py:36`
  - `RegisterAccess` 增加 `synthetic` 字段。
- `scripts/native-register-residue-audit.py:205`
  - `parse_accesses()` 记录每条 access 是否 synthetic。
- `scripts/native-register-residue-audit.py:227`
  - `summarize()` 的 key 增加 `synthetic`。
- `scripts/native-register-residue-audit.py:241`
  - summary 输出增加 `synthetic` 列。
- `scripts/native-register-residue-audit.py:251`
  - details 输出增加 `synthetic` 列。
- `tests/native_register_residue_audit_test.py:28`
  - 单测增加一条 `partial_storage_ssa` store，断言统计为 `synthetic=yes`。

验证：

```bash
python3 tests/native_register_residue_audit_test.py
ctest --test-dir build -R native_register_residue --output-on-failure
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-stack-input-pred-gate/*.signature-rewrite.ll | head -5
ctest --test-dir build --output-on-failure
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-stack-input-pred-gate/*.signature-rewrite.ll
```

结果：

- `native_register_residue_audit_test.py` 通过。
- `notdec.native_register_residue_audit.unit` 通过。
- 全量 CTest 10/10 通过。
- `--details | head` 不再输出 `BrokenPipeError`。
- 使用上一轮 `/tmp/notdec-bin2llvm-stack-input-pred-gate` 的 residue 复查，当前 vector partial stores 全部是 `synthetic=no`：

```text
category  access_kind  metadata_kind    shape    value_shape  synthetic  count
gpr       load         access           full     full         no         22
gpr       load         access           partial  full         no         3
gpr       load         external_input   full     full         no         3190
gpr       store        access           full     full         no         3583
gpr       store        access           partial  full         no         11
other     load         access           full     full         no         8
other     load         external_input   full     full         no         49
other     store        access           partial  full         no         1
vector    load         access           full     full         no         1
vector    load         external_input   full     full         no         70
vector    store        access           partial  full         no         71
```

判断：

- 下一步不能只依赖 `notdec.register.synthetic` 删除 vector RMW。
- 这些 vector stores 是 lowering 已经写成 full `ZMM*` backing value，但 metadata 仍标注 `XMM*` / lane range。
- 后续如果要消这 70/71 个 vector 残留，需要按 register range 和 liveness 判断：未写 upper lane 是否真的会被后续读到，或者是否被后续 full store 覆盖。
- 不应该直接把 partial vector write 当 full vector clobber，也不应该简单删除 external input。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 2 | 没直接减少 residue，但把 vector 尾巴的性质查清楚，避免下一步误删。 |
| 理解成本 | 1 | 只是审计输出多一列。 |
| 维护成本 | 1 | 对 IR pass 无影响，脚本格式变化需要读统计的人注意多了 `synthetic` 列。 |

有没有更好的方案：

- 可以直接做 vector liveness cleanup，但当前还没证明这些 store 是否都是 dead。
- 先加审计维度更稳，后续再按 range/liveness 做窄 cleanup。

## 阶段 D 小步：rewrite 后清理 killedbycall scratch store

背景：

- 上一轮确认固定三目标里的 vector partial store 不是 `notdec.register.synthetic`。
- 这些 store 多数是 caller-saved register 的 scratch 写，例如 `XMM0_Qb` 写到 `ZMM0` backing value。
- `NativeRegisterSSA` 在 prototype recovery 之前运行，不能在 SSA 阶段删这类 store，否则可能破坏 return recovery。
- 更合适的位置是 signature rewrite 后：此时 return store 已经被绑定和删除，剩下的 ABI `killedbycall` store 如果函数内没人读，就只是 scratch register traffic。

本轮范围：

- 只在 `RewriteSignatures` 流程后清理。
- 只处理 ABI metadata 明确写出的 `killedbycall` register。
- 只删当前函数已经是 recovered prototype 类型的函数里的 store。
- 如果函数里还有同 register / lane register load，则保留 store。
- 只删 store 后同一 basic block 直接到 `ret`，且中间没有普通 call 的 store。
- `XMM0` 可以匹配 `XMM0_Qa` / `XMM0_Qb`，但不从 `XMM0` 推到 `ZMM0`，也不推到 `XMM1`。
- 不改 `NativeRegisterSSA`，不提前删 return candidate。

改动文件和函数：

- `lib/passes/NativePrototypeRecovery.cpp:1200`
  - 新增 `registerNameMatchesEffect()`，支持 `XMM0` 匹配 `XMM0_*` lane 名。
- `lib/passes/NativePrototypeRecovery.cpp:1206`
  - 新增 `accessMatchesEffectRegister()`，按 access metadata 的 `name` / `base` 判断是否命中 ABI effect register。
- `lib/passes/NativePrototypeRecovery.cpp:1221`
  - 新增 `functionHasRegisterAccessLoad()`，函数内有同 register load 时保守不清理。
- `lib/passes/NativePrototypeRecovery.cpp:1238`
  - 新增 `storeIsDeadAtReturn()`，要求 store 后到本 block 的 `ret` 前没有普通 call，避免删除后续 call 的参数准备。
- `lib/passes/NativePrototypeRecovery.cpp:1251`
  - 新增 `killedByCallRegisterNames()`，从 ABI effect 收集明确的 killed-by-call register 名。
- `lib/passes/NativePrototypeRecovery.cpp:1264`
  - 新增 `eraseDeadKilledByCallRegisterStores()`，删除 rewrite 后无读的 killed-by-call register stores，并递归清理死 RMW 计算和 external input load。
- `lib/passes/NativePrototypeRecovery.cpp:2592`
  - `runNativePrototypeRecovery()` 在 `rewriteNativeRecoveredPrototypes()` 和 `rewriteDeclarationCallOutputs()` 之后调用 cleanup。
- `tests/native_prototype_recovery_test.cpp:174`
  - 新增 `attachKilledVectorScratchTestAbi()`，构造 `XMM0` killed-by-call ABI。
- `tests/native_prototype_recovery_test.cpp:1668`
  - 新增 `createKilledVectorScratchStoreFunction()`，构造 dead/live/call 后继三种 vector scratch store。
- `tests/native_prototype_recovery_test.cpp:7209`
  - 断言 dead `XMM0_Qb` store 被删除，有同名 load 的 `XMM0_Qb` store 保留，store 后还有 call 的 `XMM0_Qb` store 也保留。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
./build/bin/native_register_effects_test
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-killed-scratch-ret-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-killed-scratch-ret-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-killed-scratch-ret-gate/*.signature-rewrite.ll \
  > /tmp/notdec-reg-details-killed-scratch-ret.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- `notdec-native-llvm` 构建通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 42s |
| `libuv:shared-library` | 106s | 107s |
| `memcached:executable` | 57s | 57s |

residue 变化：

```text
category  access_kind  metadata_kind    shape    value_shape  synthetic  before  after
gpr       load         access           full     full         no         22      22
gpr       load         access           partial  full         no         3       3
gpr       load         external_input   full     full         no         3190    3190
gpr       store        access           full     full         no         3583    3179
gpr       store        access           partial  full         no         11      4
other     load         access           full     full         no         8       8
other     load         external_input   full     full         no         49      49
other     store        access           partial  full         no         1       1
vector    load         access           full     full         no         1       1
vector    load         external_input   full     full         no         70      15
vector    store        access           partial  full         no         71      14
```

判断：

- 这一步直接消掉了大部分 `XMM0` scratch vector RMW 残留。
- 收紧到 return 前 store 后，fixed gate residue 和过宽版本相同，说明被删除的 store 都不是 call 前参数准备。
- 仍剩下的 vector 残留主要是 `XMM1`、`XMM2`、`XMM3` 等 ABI metadata 当前没有明确 `killedbycall` 的寄存器，或者函数内存在对应 load。
- 下一步如果要继续消 vector，需要先补 ABI effect 导出范围，或者在 cleanup 里引入更明确的 ABI alias 表；不能靠猜测把 `XMM1+` 都当 killed-by-call。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 7 | fixed gate 上 vector external input 从 70 降到 15，vector partial store 从 71 降到 14，GPR store 也下降一批。 |
| 理解成本 | 3 | 新增的是 rewrite 后 cleanup，条件较窄，但读代码时要知道它故意不在 SSA 阶段运行。 |
| 维护成本 | 3 | 依赖 ABI effect metadata 的准确性；后续 ABI 导出更完整时，这个 cleanup 会自然覆盖更多寄存器。 |

有没有更好的方案：

- 更完整方案是把 ABI effect alias 做成统一 resolver，供 SSA、prototype recovery 和 cleanup 共用。
- 本轮先做 rewrite 后窄清理，是为了避免提前影响 return recovery，同时先解决 fixed gate 上明显的 vector residue。

## 阶段 D 小步：清理非返回 vector partial scratch store

目标：

- 在 signature rewrite 后，继续清理 return 前明显无用的 vector partial store。
- 范围只限已经 rewrite 或本来就匹配 recovered prototype 的函数。
- 不依赖 x86 `XMM1+` killed-by-call 猜测，也不把有函数内 load 依赖的 RMW store 删掉。

实现：

- `lib/passes/NativePrototypeRecovery.cpp:1251`
  - 新增 `isVectorRegisterName()`，只识别 `XMM` / `YMM` / `ZMM` register metadata。
- `lib/passes/NativePrototypeRecovery.cpp:1256`
  - 新增 `accessIsVectorPartialStore()`，要求 store 有 `notdec.register.access` metadata、目标是 register global，且 access size 小于 backing register size。
- `lib/passes/NativePrototypeRecovery.cpp:1280`
  - 新增 `accessMatchesRecoveredReturn()`，如果该 access 对应 recovered return register，就不当 scratch 清理。
- `lib/passes/NativePrototypeRecovery.cpp:1293`
  - 新增 `functionHasRegisterAccessLoadForAccess()`，按 access 的 `name` 和 `base` 检查函数内是否存在对应 register access load；存在就保守保留 store。
- `lib/passes/NativePrototypeRecovery.cpp:1369`
  - 新增 `eraseDeadNonReturnVectorStores()`。
  - 删除条件：同一 block 内 store 后到 `ret` 之前没有 call、是 vector partial store、不是 recovered return、函数内没有对应 register access load。
- `lib/passes/NativePrototypeRecovery.cpp:2709`
  - 在 `runNativePrototypeRecovery()` 的 signature rewrite 后调用该 cleanup。
- `tests/native_prototype_recovery_test.cpp:187`
  - 新增 `attachVectorXmm1ReturnTestAbi()`，构造 `XMM1_Qa` 返回 ABI，用来确认 cleanup 不抢在 return rewrite 前误删返回语义。
- `tests/native_prototype_recovery_test.cpp:1678`
  - `createKilledVectorScratchStoreFunction()` 新增 `baseName` 参数，让测试里 `ZMM1` global 的 metadata base 也能写成 `ZMM1`。
- `tests/native_prototype_recovery_test.cpp:7244`
  - 新增 dead non-return vector scratch 用例，断言 `XMM1` partial store 被清理。
- `tests/native_prototype_recovery_test.cpp:7266`
  - 新增 `XMM1_Qa` return 用例，断言函数被 rewrite 成 `i64()`，旧 register store 被 return rewrite 移除。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
./build/bin/native_register_effects_test
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-vector-cleanup-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-vector-cleanup-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-vector-cleanup-gate/*.signature-rewrite.ll \
  > /tmp/notdec-reg-details-vector-cleanup.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- `notdec-native-llvm` 构建通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 41s |
| `libuv:shared-library` | 105s | 106s |
| `memcached:executable` | 57s | 59s |

residue 结果：

```text
category  access_kind  metadata_kind    shape    value_shape  synthetic  count
gpr       load         access           full     full         no         22
gpr       load         access           partial  full         no         3
gpr       load         external_input   full     full         no         3190
gpr       store        access           full     full         no         3381
gpr       store        access           partial  full         no         9
other     load         access           full     full         no         8
other     load         external_input   full     full         no         49
other     store        access           partial  full         no         1
vector    load         access           full     full         no         1
vector    load         external_input   full     full         no         7
vector    store        access           partial  full         no         3
```

和上一步 `killedbycall scratch store` cleanup 的同口径结果相比：

- `vector store access partial/full`：14 -> 3。
- `vector load external_input full/full`：15 -> 7。
- `gpr` store 计数这次输出和上一步记录不同，但本轮代码路径只删除 vector partial store，不会新增 GPR load/store。这个差异先记录，后续如果要精确归因，应在同一 commit 基线上重新跑一次 before/after。

判断：

- 这一步进一步消掉了 `uv_metrics_info`、`notdec_native_1da10` 这类 return 前非返回 vector scratch store。
- 保留了三类风险用例：函数内有对应 vector load 的 RMW、store 后到 `ret` 前还有 call、recovered return 对应的 vector store。
- 仍剩下的 3 个 vector partial store 需要继续看具体函数，不能靠扩大本 cleanup 条件直接删。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 6 | fixed gate 上 vector partial store 从 14 降到 3，vector external input 从 15 降到 7。 |
| 理解成本 | 3 | 条件都集中在 post-rewrite cleanup，规则比较短，但需要知道它只处理 return 前死 store。 |
| 维护成本 | 3 | 依赖 access metadata 的 base/name/size 准确性；如果 metadata 不准，会选择保守不删。 |

有没有更好的方案：

- 更完整方案仍然是统一 storage current-value 查询和 ABI effect alias resolver。
- 本轮选择 cleanup，是因为它能先消掉明确无用的 vector scratch，不需要提前处理完整 vector/lane SSA。

## 阶段 D 小步：跨 block 清理死 vector RMW store

背景：

- 上一步后 fixed gate 只剩 3 个 `vector store access partial/full`。
- 它们都在 `libuv` 的 `uv_fs_futime`、`uv_fs_lutime`、`uv_fs_utime` 里，形态是用 `ZMM0/ZMM1` external input 拼出一个 `XMM0_Qb` partial backing store，然后所有后续路径直接 return。
- 这不是 ABI vector lane input 没进签名的问题。x86-64 cspec 里 vector float input 是 `XMM0_Qa/XMM1_Qa`，残留 store 是 `XMM0_Qb`，不能把它误恢复成参数。
- 真正问题是上一步 cleanup 只看同一 block 的 `store -> ret`，没覆盖 `store -> branch -> ret`。

实现：

- `lib/passes/NativePrototypeRecovery.cpp:1293`
  - 新增 `instructionReadsRegisterAccess()`，判断 store 之后是否有同一 access name/base 的 register load。
- `lib/passes/NativePrototypeRecovery.cpp:1316`
  - 新增 `reachesReturnWithoutCallOrAccessLoad()`，扫描一段指令，遇到 return 视为安全，遇到 call 或同 storage load 视为不安全。
- `lib/passes/NativePrototypeRecovery.cpp:1332`
  - 新增 `allSuccessorsReachReturnWithoutCallOrAccessLoad()`，处理 store 所在 block 后继都是 return path 的简单 CFG。
- `lib/passes/NativePrototypeRecovery.cpp:1360`
  - 新增 `storeIsDeadOnAllReturnPaths()`，把同 block 和简单后继 block 的死 store 判断合并。
- `lib/passes/NativePrototypeRecovery.cpp:1440`
  - `eraseDeadNonReturnVectorStores()` 改用 `storeIsDeadOnAllReturnPaths()`，不再因为 store 的 value 依赖旧 vector external input 就保留死写。
- `tests/native_prototype_recovery_test.cpp:1678`
  - `createKilledVectorScratchStoreFunction()` 增加 `loadAfterStore` 参数，用来构造真正可观察的后续 load。
- `tests/native_prototype_recovery_test.cpp:7240`
  - 原来的 live 用例从“store 前读旧值”改成“store 后读同一 vector access”。前者只是 RMW 构造过程，store 后如果没有读/call/return 绑定，仍然是死写。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
./build/bin/native_register_effects_test
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-vector-rmw-cleanup-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-vector-rmw-cleanup-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-vector-rmw-cleanup-gate/*.signature-rewrite.ll \
  > /tmp/notdec-reg-details-vector-rmw-cleanup.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- `notdec-native-llvm` 构建通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 42s | 41s |
| `libuv:shared-library` | 107s | 107s |
| `memcached:executable` | 57s | 58s |

residue 结果：

```text
category  access_kind  metadata_kind    shape    value_shape  synthetic  count
gpr       load         access           full     full         no         22
gpr       load         access           partial  full         no         3
gpr       load         external_input   full     full         no         3190
gpr       store        access           full     full         no         3381
gpr       store        access           partial  full         no         9
other     load         access           full     full         no         8
other     load         external_input   full     full         no         49
other     store        access           partial  full         no         1
vector    load         access           full     full         no         1
vector    load         external_input   full     full         no         1
```

和上一步相比：

- `vector store access partial/full`：3 -> 0。
- `vector load external_input full/full`：7 -> 1。
- 剩余 vector residue 只剩：
  - `notdec_native_9e70` 的 `ZMM0` external input。
  - `uv_timer_stop` 的 `ZMM0` full access load。

判断：

- 这一步把 fixed gate 上最后 3 个 vector partial store 清掉了。
- 清理条件仍然围绕“store 后是否可观察”：后续路径有 call、同 storage load、recovered return 时都不删。
- 当前 CFG 判断只处理简单后继路径；遇到环或复杂 successor 会保守不删。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 7 | fixed gate 上 vector partial store 清零，vector external input 从 7 降到 1。 |
| 理解成本 | 4 | 比同 block cleanup 多了简单 CFG 扫描，但仍集中在 post-rewrite cleanup。 |
| 维护成本 | 4 | 后续如果要覆盖更复杂 CFG，应复用统一 liveness/current-value 查询，避免这个 helper 继续长大。 |

有没有更好的方案：

- 更完整方案是阶段 C 的统一 current-value/liveness 查询。
- 本轮先补简单后继 CFG，是因为 fixed gate 剩余 case 很明确，且不需要引入 vector 参数恢复的语义风险。

## 阶段 A 小步：残留审计补充位置和 storage role

背景：

- vector partial store 清完后，fixed gate 剩余大头转到 GPR。
- 直接看 summary 只能知道 GPR load/store 很多，不能区分 `RSP/RBP` 栈建模、callee-saved 保存恢复、call 附近真实寄存器传值和普通 partial access。
- 在没有这个分类前，继续改 SSA 容易把 stack pointer / frame pointer 残留误当成普通 GPR 消除问题。

本轮范围：

- 只增强 residue audit 的 `--details` 输出。
- 不改 IR 生成，不改寄存器 SSA，不改 prototype rewrite。
- summary 输出保持原有列，便于继续和前几轮数据对比。

实现：

- `scripts/native-register-residue-audit.py:25`
  - 新增 `LABEL_RE`，识别 LLVM IR basic block label。
- `scripts/native-register-residue-audit.py:38`
  - `RegisterAccess` 新增 `line`、`block`、`storage_role`、`local_context` 字段。
- `scripts/native-register-residue-audit.py:144`
  - 新增 `current_block()`，记录每条访问所在 block。
- `scripts/native-register-residue-audit.py:179`
  - 新增 `is_call_instruction()`、`is_ret_instruction()`、`previous_instruction()`、`next_instruction()` 等小 helper，用来判断访问是否紧贴 call/ret。
- `scripts/native-register-residue-audit.py:215`
  - 新增 `storage_role()`，把寄存器粗分成 `stack_pointer`、`frame_pointer`、`callee_saved_gpr`、`caller_saved_gpr`、`flags`、`vector`、`other`。
- `scripts/native-register-residue-audit.py:230`
  - 新增 `local_context()`，把访问粗分成 `entry_external_input`、`before_call`、`after_call`、`before_ret`、`ordinary`。
- `scripts/native-register-residue-audit.py:249`
  - `parse_accesses()` 记录行号、block、storage role 和 local context。
- `scripts/native-register-residue-audit.py:342`
  - `write_details()` 输出新增列：`line`、`block`、`storage_role`、`local_context`。
- `tests/native_register_residue_audit_test.py:36`
  - 测试 IR 增加一个 call，让位置分类能覆盖 `before_call`。
- `tests/native_register_residue_audit_test.py:65`
  - 增加 `line`、`block`、`storage_role`、`local_context` 断言。

验证：

```bash
python3 tests/native_register_residue_audit_test.py
ctest --test-dir build -R native_register_residue --output-on-failure
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-vector-rmw-cleanup-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-vector-rmw-cleanup-gate/*.signature-rewrite.ll \
  > /tmp/notdec-reg-details-context.tsv
awk -F '\t' 'NR>1 {count[$6"\t"$7"\t"$8"\t"$9]++} END {for (k in count) print count[k]"\t"k}' \
  /tmp/notdec-reg-details-context.tsv | sort -nr | head -30
```

结果：

- `native_register_residue_audit_test.py` 通过。
- `notdec.native_register_residue_audit.unit` 通过。
- summary 统计不变：

```text
category  access_kind  metadata_kind    shape    value_shape  synthetic  count
gpr       load         access           full     full         no         22
gpr       load         access           partial  full         no         3
gpr       load         external_input   full     full         no         3190
gpr       store        access           full     full         no         3381
gpr       store        access           partial  full         no         9
other     load         access           full     full         no         8
other     load         external_input   full     full         no         49
other     store        access           partial  full         no         1
vector    load         access           full     full         no         1
vector    load         external_input   full     full         no         1
```

新增分类里 fixed gate 前 30 类如下：

```text
1633  callee_saved_gpr  entry_external_input  load   external_input
855   stack_pointer     entry_external_input  load   external_input
830   stack_pointer     ordinary              store  access
747   caller_saved_gpr  ordinary              store  access
723   frame_pointer     ordinary              store  access
692   frame_pointer     entry_external_input  load   external_input
502   callee_saved_gpr  ordinary              store  access
302   stack_pointer     before_ret            store  access
140   callee_saved_gpr  before_ret            store  access
97    caller_saved_gpr  before_ret            store  access
49    other             entry_external_input  load   external_input
28    caller_saved_gpr  after_call            store  access
18    frame_pointer     before_ret            store  access
10    caller_saved_gpr  entry_external_input  load   external_input
8     other             ordinary              load   access
7     frame_pointer     ordinary              load   access
6     callee_saved_gpr  ordinary              load   access
4     caller_saved_gpr  ordinary              load   access
3     stack_pointer     ordinary              load   access
2     caller_saved_gpr  after_call            load   access
1     vector            ordinary              load   access
1     vector            entry_external_input  load   external_input
1     other             ordinary              store  access
1     frame_pointer     before_call           store  access
1     frame_pointer     after_call            load   access
1     caller_saved_gpr  before_call           store  access
1     caller_saved_gpr  before_call           load   access
1     callee_saved_gpr  after_call            store  access
1     callee_saved_gpr  after_call            load   access
```

判断：

- 当前 fixed gate 最大残留不是 partial access，而是：
  - callee-saved GPR entry external input；
  - `RSP/RBP` 的 entry load、ordinary store、ret 前 store；
  - callee-saved 保存恢复 store。
- 这说明下一步如果只追 GPR partial SSA，收益很小；更应该先拆清 stack pointer/frame pointer 建模残留和 callee-saved save/restore 是否能在 prototype rewrite 后清理。
- 剩余 3 个 `gpr load access partial/full` 仍是 call-after-load case，不应盲删。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 4 | 没减少 IR 残留，但把下一步方向从“猜 partial SSA”变成了可量化分类。 |
| 理解成本 | 2 | 只是审计脚本新增几列，summary 不变。 |
| 维护成本 | 2 | 分类是粗粒度辅助判断，不参与 pass 语义；后续不应把它当证明条件。 |

有没有更好的方案：

- 更完整方案是直接构建统一 storage liveness/current-value 查询。
- 本轮先补审计，是因为当前 GPR residue 里 `RSP/RBP` 和 callee-saved 占大头，先看清来源比继续扩大 cleanup 条件更稳。

## 阶段 A 小步：残留审计区分 return path

背景：

- 上一步 `local_context` 只能把紧贴 `ret` 的访问标成 `before_ret`。
- 实际 epilogue 常见形态是连续恢复 `RBX/R12/RBP/RSP`，只有最后一条紧贴 `ret`，前面的恢复 store 会被归到 `ordinary`。
- 这会夸大“普通 GPR store”数量，不利于判断下一步应该做 partial SSA 还是处理保存恢复残留。

本轮范围：

- 只增强 `--details` 的位置分类。
- 同一 basic block 内，如果某条访问之后没有 call，最后到达 ret，则标成 `return_path`。
- 不跨 block 推断，不改 summary，不改 IR 生成和 pass 语义。

实现：

- `scripts/native-register-residue-audit.py:215`
  - 新增 `reaches_return_in_block_without_call()`，判断同一 block 内后续是否无 call 到 ret。
- `scripts/native-register-residue-audit.py:257`
  - `local_context()` 增加 `return_path` 分类，优先级低于 `before_call` 和 `before_ret`。
- `tests/native_register_residue_audit_test.py:41`
  - 测试 IR 增加一条 ret path 上的 full store。
- `tests/native_register_residue_audit_test.py:62`
  - 更新 full store 数量断言。
- `tests/native_register_residue_audit_test.py:71`
  - 增加 `return_path` 断言。

验证：

```bash
python3 tests/native_register_residue_audit_test.py
ctest --test-dir build -R native_register_residue --output-on-failure
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-vector-rmw-cleanup-gate/*.signature-rewrite.ll \
  > /tmp/notdec-reg-details-return-path.tsv
awk -F '\t' 'NR>1 {count[$6"\t"$7"\t"$8"\t"$9]++} END {for (k in count) print count[k]"\t"k}' \
  /tmp/notdec-reg-details-return-path.tsv | sort -nr | head -30
git diff --check
```

结果：

- `native_register_residue_audit_test.py` 通过。
- `notdec.native_register_residue_audit.unit` 通过。
- `git diff --check` 通过。
- fixed gate 前 30 类变为：

```text
1633  callee_saved_gpr  entry_external_input  load   external_input
855   stack_pointer     entry_external_input  load   external_input
692   frame_pointer     entry_external_input  load   external_input
603   frame_pointer     return_path           store  access
534   stack_pointer     return_path           store  access
511   caller_saved_gpr  ordinary              store  access
439   callee_saved_gpr  return_path           store  access
302   stack_pointer     before_ret            store  access
296   stack_pointer     ordinary              store  access
236   caller_saved_gpr  return_path           store  access
140   callee_saved_gpr  before_ret            store  access
120   frame_pointer     ordinary              store  access
97    caller_saved_gpr  before_ret            store  access
63    callee_saved_gpr  ordinary              store  access
49    other             entry_external_input  load   external_input
28    caller_saved_gpr  after_call            store  access
18    frame_pointer     before_ret            store  access
10    caller_saved_gpr  entry_external_input  load   external_input
8     other             ordinary              load   access
4     frame_pointer     return_path           load   access
4     callee_saved_gpr  ordinary              load   access
3     frame_pointer     ordinary              load   access
3     caller_saved_gpr  ordinary              load   access
2     stack_pointer     ordinary              load   access
2     caller_saved_gpr  after_call            load   access
2     callee_saved_gpr  return_path           load   access
1     vector            return_path           load   access
1     vector            entry_external_input  load   external_input
1     stack_pointer     return_path           load   access
1     other             ordinary              store  access
```

判断：

- 大量原本看起来是 `ordinary store` 的 `RBP/RSP/callee-saved`，实际是在 return path 上恢复寄存器和栈状态。
- 下一步不应盲删这些 store。callee-saved preserve 语义需要由函数 metadata、call effect 和 caller 侧 current-value 查询共同承接。
- 当前更合理的后续方向是：先做“callee-saved / stack pointer return-path residue”归类和语义判断，再决定是否做 post-rewrite cleanup。

## 阶段 D 小步：按 store 后续路径清理 killed-by-call GPR 死写

背景：

- 现有 `eraseDeadKilledByCallRegisterStores()` 只删同一 block 内紧贴 return 的 killed-by-call register store。
- 它还有一个过粗条件：如果函数里任何地方有同寄存器 load，就整类 store 都不删。
- fixed gate 里仍有一些 caller-saved GPR return-path store，store 后没有 call/load，返回 ABI 也不要求保留；这些可以用已有 path liveness 条件清掉。

本轮范围：

- 只处理 ABI `killedbycall` register。
- 按单条 store 判断后续所有路径是否到 return，且中间没有 call 或同 storage load。
- 如果 store 对应 recovered return register，不删。
- 不处理 `RSP/RBP`，不处理 ABI unaffected / callee-saved restore，不处理跨复杂 CFG 的推断。

实现：

- `lib/passes/NativePrototypeRecovery.cpp:1221`
  - 删除本轮改动后不再使用的 `functionHasRegisterAccessLoad()` 和 `storeIsDeadAtReturn()`。
- `lib/passes/NativePrototypeRecovery.cpp:1362`
  - `eraseDeadKilledByCallRegisterStores()` 改成逐条 store 使用 `storeIsDeadOnAllReturnPaths()` 判断。
  - 增加 `accessMatchesRecoveredReturn()` 保护，避免删除已经恢复成返回值语义的寄存器写。
- `tests/native_prototype_recovery_test.cpp:187`
  - 新增 `attachKilledGprScratchTestAbi()`，构造 killed-by-call GPR ABI。
- `tests/native_prototype_recovery_test.cpp:1729`
  - 新增 `createKilledGprScratchStoreFunction()`，构造 store 前有 load、store 后有/无 load 两种用例。
- `tests/native_prototype_recovery_test.cpp:7318`
  - 增加 GPR killed-by-call cleanup 测试：
    - store 前有同寄存器 load，但 store 后无 load/call，期望删除；
    - store 后还有同寄存器 load，期望保留。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
./build/bin/native_register_effects_test
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-gpr-return-cleanup-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-gpr-return-cleanup-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-gpr-return-cleanup-gate/*.signature-rewrite.ll \
  > /tmp/notdec-reg-details-gpr-return-cleanup.tsv
git diff --check
```

结果：

- `native_prototype_recovery_test` 通过。
- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- `notdec-native-llvm` 构建通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 41s |
| `libuv:shared-library` | 105s | 107s |
| `memcached:executable` | 57s | 58s |

residue 结果：

```text
category  access_kind  metadata_kind    shape    value_shape  synthetic  count
gpr       load         access           full     full         no         22
gpr       load         access           partial  full         no         3
gpr       load         external_input   full     full         no         3190
gpr       store        access           full     full         no         3332
gpr       store        access           partial  full         no         4
other     load         access           full     full         no         8
other     load         external_input   full     full         no         49
other     store        access           partial  full         no         1
vector    load         access           full     full         no         1
vector    load         external_input   full     full         no         1
```

和上一轮 `/tmp/notdec-bin2llvm-vector-rmw-cleanup-gate` 相比：

- `gpr store access full/full`：3381 -> 3332。
- `gpr store access partial/full`：9 -> 4。
- 其它大类不变。

剩余 4 个 GPR partial store：

```text
libuv      uv_ip6_addr           SI
libuv      uv_read_start         CL
memcached  notdec_native_6740    BL
memcached  notdec_native_17ab0   EDI
```

判断：

- 这一步只删除 store 后不可观察的 killed-by-call GPR 写，没有碰 `RSP/RBP/callee-saved` return-path restore。
- fixed gate 没有明显性能退化，耗时和上一轮同口径基本持平。
- 剩余 GPR 大头仍然是 stack/frame/callee-saved 状态。要继续大幅下降，需要先让 preserve/call effect/current-value 语义承接这些保存恢复，而不是简单扩大死写删除条件。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 5 | 清掉 49 个 GPR full store 和 5 个 GPR partial store，范围小但确定。 |
| 理解成本 | 3 | 复用已有 `storeIsDeadOnAllReturnPaths()`，逻辑比原来更细，但没有新增 CFG 算法。 |
| 维护成本 | 3 | 依赖 ABI killed-by-call metadata 和 access metadata；条件集中，后续可被统一 liveness 查询替代。 |

有没有更好的方案：

- 更完整方案还是阶段 C 的统一 storage liveness/current-value 查询。
- 这次先按单条 killed-by-call store 清理，是因为它能减少明确不可观察的 caller-saved residue，同时避开 callee-saved 和 stack pointer 语义风险。

## 阶段 A 小步：残留审计显示函数 effect metadata

背景：

- GPR return-path cleanup 后，剩余 caller-saved store 里有不少看起来也在 return path 上。
- 但 fixed gate 里 ABI `killedbycall` 只覆盖 `RAX/RDX/XMM0` 等少数寄存器，不能把所有 caller-saved 都按名字假设成可删。
- 需要直接看到当前函数 metadata 对某个 storage 的说法：它是 `clobbers`、`preserves`、`external_inputs`，还是根本没有 effect 记录。

本轮范围：

- 只增强 `native-register-residue-audit.py --details`。
- summary 输出保持不变。
- 不改 pass，不改 IR 生成。

实现：

- `scripts/native-register-residue-audit.py:18`
  - 新增 `FUNCTION_EFFECT_RE`，匹配函数定义行上的 `notdec.register.clobbers/preserves/external_inputs`。
- `scripts/native-register-residue-audit.py:49`
  - `RegisterAccess` 增加 `function_effects` 字段。
- `scripts/native-register-residue-audit.py:145`
  - 新增 `parse_function_effects()`，递归解析函数 effect metadata node 里的 `name=`。
- `scripts/native-register-residue-audit.py:311`
  - 新增 `function_effects_for_access()`，按 access name/base 匹配当前函数 effect。
- `scripts/native-register-residue-audit.py:335`
  - `parse_accesses()` 给每条 access 记录 effect 分类。
- `scripts/native-register-residue-audit.py:435`
  - `write_details()` 增加 `function_effects` 列。
- `tests/native_register_residue_audit_test.py:34`
  - 测试 IR 增加 `notdec.register.clobbers` 和 `notdec.register.external_inputs`。
- `tests/native_register_residue_audit_test.py:74`
  - 增加 `function_effects` 断言。

验证：

```bash
python3 tests/native_register_residue_audit_test.py
ctest --test-dir build -R native_register_residue --output-on-failure
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-gpr-return-cleanup-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-gpr-return-cleanup-gate/*.signature-rewrite.ll \
  > /tmp/notdec-reg-details-effects.tsv
git diff --check
```

结果：

- `native_register_residue_audit_test.py` 通过。
- `notdec.native_register_residue_audit.unit` 通过。
- summary 不变。
- 剩余 4 个 GPR partial store 的 effect 情况：

```text
libuv      uv_ip6_addr         SI   caller_saved_gpr  return_path  none
libuv      uv_read_start       CL   caller_saved_gpr  ordinary     none
memcached  notdec_native_6740  BL   callee_saved_gpr  ordinary     clobbers,external_inputs
memcached  notdec_native_17ab0 EDI  caller_saved_gpr  ordinary     none
```

判断：

- 多数剩余 GPR partial store 的 `function_effects=none`，当前 metadata 不足以证明它们是返回边界不可观察的 killed/clobbered store。
- `memcached notdec_native_6740 BL` 虽然有 `clobbers,external_inputs`，但它位于普通路径且后面有 call/merge，不能用 return-path dead store 条件删。
- 下一步如果要继续减少这些 partial store，应该进入阶段 B 的 storage range SSA，或者先补 call/effect metadata 的准确性；不应再扩大 post-rewrite cleanup。

## 阶段 C 小步：入口 fallback callsite load 标成 caller external input

背景：

- direct callsite rewrite 找不到本地 store、PHI 或 caller entry value 时，会在 caller 侧生成 `RDI.callsite_input = load @RDI`。
- 如果这条 fallback load 所在 callsite 沿唯一前驱链回到函数入口，且中间没有非 intrinsic call，那么它语义上就是 caller 的 entry register input。
- 旧逻辑把这种 load 标成普通 `notdec.register.access`，会留在 register access residue 里；这不利于区分真实未消除寄存器流量和函数入口输入。

本轮范围：

- 只处理“回到 caller 入口、且中间无非 intrinsic call”的 fallback load。
- 多前驱冲突、call 后 fallback、找不到唯一 register global 等情况仍保持旧逻辑。
- 不把它替换成函数参数；只把 metadata 从普通 access 改成 external input，并同步 caller 的 `notdec.register.external_inputs`。

实现：

- `lib/passes/NativePrototypeRecovery.cpp:884`
  - 新增 `registerExternalInputMetadata()`，为 caller fallback load 生成 `notdec.register.external_input` metadata。
- `lib/passes/NativePrototypeRecovery.cpp:894`
  - 新增 `functionExternalInputsHasRegister()`，避免重复写同一个 external input entry。
- `lib/passes/NativePrototypeRecovery.cpp:916`
  - 新增 `ensureFunctionExternalInputMetadata()`，把 fallback input 同步到 caller 函数 metadata。
- `lib/passes/NativePrototypeRecovery.cpp:959`
  - `registerGlobalValueBeforeCall()` 增加 `isCallerEntryValue` 参数；入口 fallback 时挂 `external_input`，其它 fallback 仍挂普通 access。
- `lib/passes/NativePrototypeRecovery.cpp:1043`
  - `callsiteInputValueBeforeCall()` 在唯一链回到入口且无 intervening call 时，使用 entry fallback 模式。
- `tests/native_prototype_recovery_test.cpp:4166`
  - missing callsite input fallback 用例改为断言生成 load 带 `notdec.register.external_input name=RDI`，不再带 `notdec.register.access`，且 caller metadata 包含 `RDI`。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
./build/bin/native_register_effects_test
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-entry-fallback-input-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-entry-fallback-input-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-entry-fallback-input-gate/*.signature-rewrite.ll \
  > /tmp/notdec-reg-details-entry-fallback-input.tsv
git diff --check
```

结果：

- `native_prototype_recovery_test` 通过。
- `native_register_effects_test` 通过。
- 相关 CTest 6/6 通过。
- 全量 CTest 10/10 通过。
- `notdec-native-llvm` 构建通过。
- 固定三目标 Bench2 gate 通过，LLVM 22 assemble/verify 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 41s |
| `libuv:shared-library` | 107s | 107s |
| `memcached:executable` | 57s | 57s |

residue 结果：

```text
category  access_kind  metadata_kind    shape    value_shape  synthetic  count
gpr       load         access           full     full         no         21
gpr       load         access           partial  full         no         3
gpr       load         external_input   full     full         no         3191
gpr       store        access           full     full         no         3332
gpr       store        access           partial  full         no         4
other     load         access           full     full         no         8
other     load         external_input   full     full         no         49
other     store        access           partial  full         no         1
vector    load         access           full     full         no         1
vector    load         external_input   full     full         no         1
```

和上一轮 `/tmp/notdec-bin2llvm-gpr-return-cleanup-gate` 相比：

- `gpr load access full/full`：22 -> 21。
- `gpr load external_input full/full`：3190 -> 3191。

抽查：

```text
notdec_native_8480  %RDI.callsite_input = load i64, ptr @RDI, !notdec.register.external_input
```

该函数的 `notdec.register.external_inputs` 也同步包含 `RDI`。

判断：

- 这一步没有消除输入值本身，但把“caller entry register input”从普通 access residue 中移出，减少了一个误导性的 access load。
- 后续如果要进一步消除这类 entry input，需要阶段 C 的统一 current-value 查询和函数签名 rewrite，而不是把 fallback load 当死代码删除。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | access load 少 1，主要是分类和语义标注更准确。 |
| 理解成本 | 3 | 新增 metadata helper，逻辑仍局限在 callsite input fallback。 |
| 维护成本 | 3 | 后续统一 current-value 查询落地后，应把这个入口 fallback 判断收进去。 |

有没有更好的方案：

- 更完整方案是让 caller 也参与 signature rewrite，把 entry fallback input 变成 caller 参数。
- 本轮只改 metadata，是因为当前 caller 未必已经被 rewrite；直接升成参数会扩大调用链改动范围。

## 2026-06-02 实现记录：覆盖 store 也作为前序 dead store 的结束点

背景：

- 上一轮 cleanup 只把“从 store 到所有 return 路径没有 call、没有同寄存器 load”的 store 当作 dead。
- 这会漏掉一种简单情况：同一个寄存器后面先被另一个 store 覆盖，前一个 store 已经不可能被读到。
- 本次只处理 killed-by-call scratch store 的这个漏删点，不处理 RSP/RBP、callee-saved return-path store，也不改变 prototype 恢复规则。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:1347)
  - 新增 `instructionWritesRegisterAccess`，按 `notdec.register.access` 的 `name/base` 判断后续 store 是否覆盖同一寄存器访问。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:1370)
  - `reachesReturnWithoutCallOrAccessLoad` 遇到同寄存器后续 store 时返回 true，表示当前 store 在这条路径上已被覆盖。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:1417)
  - `storeIsDeadOnAllReturnPaths` 的同块扫描也加入同样判断；遇到 call 或同寄存器 load 仍然保守停止。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:1729)
  - `createKilledGprScratchStoreFunction` 增加 `overwriteAfterStore` 测试开关。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:7340)
  - 新增 `overwritten_gpr_scratch` 用例，确认被覆盖的 killed-by-call GPR store 会被清掉。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-overwrite-dead-store-gate \
  --target vsftpd:executable \
  --target libuv:shared-library \
  --target memcached:executable
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-overwrite-dead-store-gate/*.signature-rewrite.ll
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- 全量 CTest 10/10 通过。
- 固定三目标 Bench2 gate 通过。
- 固定三目标耗时：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `vsftpd:executable` | 41s | 40s |
| `libuv:shared-library` | 106s | 106s |
| `memcached:executable` | 57s | 58s |

residue 结果：

```text
category  access_kind  metadata_kind    shape    value_shape  synthetic  count
gpr       load         access           full     full         no         21
gpr       load         access           partial  full         no         3
gpr       load         external_input   full     full         no         3191
gpr       store        access           full     full         no         3311
gpr       store        access           partial  full         no         4
other     load         access           full     full         no         8
other     load         external_input   full     full         no         49
other     store        access           partial  full         no         1
vector    load         access           full     full         no         1
vector    load         external_input   full     full         no         1
```

和上一轮 `/tmp/notdec-bin2llvm-entry-fallback-input-gate` 相比：

- `gpr store access full/full`：3332 -> 3311，少 21 个。
- 主要变化是 `caller_saved_gpr ordinary clobbers store access full/full`：151 -> 130。
- `gpr load access` 没变，说明这一步只删覆盖后的死 store，没有扩大到读取语义。

判断：

- 这一步是局部 cleanup，能删除确定被同寄存器后续 store 覆盖的 killed-by-call 残留。
- 对性能没有明显负面影响，固定 gate 耗时和上一轮同量级。
- 还没解决 stack/frame/callee-saved residue，也没解决 caller entry input 参数化。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 4 | 删除 21 个真实 GPR store residue，但范围只限已证明死的覆盖 store。 |
| 理解成本 | 2 | 只是补齐 dead-store 路径判断，和已有 load/call 停止逻辑并列。 |
| 维护成本 | 2 | 后续如果引入统一 current-value 查询，这段仍可作为简单 cleanup 保留。 |

有没有更好的方案：

- 更完整方案是做寄存器级 def-use/liveness，而不是在路径扫描里补条件。
- 当前 residue 还没到需要引入完整 liveness 的程度；本次先保留简单路径判断，避免扩大实现面。
