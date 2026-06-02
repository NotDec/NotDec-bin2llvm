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
