# Low Confidence Seed Boundary

## 原始 prompt

```text
在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis里面根据划分的几个关键宏观步骤，创建对应的文件夹，然后在prompt中要求，对应步骤的规划和修改必须放到对应的文件夹里。

本次目标是按照规划，完善bin2llvm项目反汇编转IR的native链路（即不走GhidraScript的链路，而是走libsla，libdecomp的这个链路），从Ghidra和rizin那边复刻足够多的功能。每当实现一部分功能后，就看看能否利用bench2里面这些项目去做初步的测试： （vsftpd可执行文件，libuv动态链接库，memcached可执行文件），测试实现是否正确。规划中每一部分都完成后即可停止。在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis 下创建一个PROGRESS.md里追踪计划的实现。完成一个部分后，同步更新 项目顶层的ARCHITECTURE.md 反映实现的功能模块的架构。

约束：根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## 当前目标和已有 native 状态

native 已经有 High / Medium / Low 三档 function seed confidence。最近新增的
`elf-relocation-code` seed 是 Low，因为 executable relocation target 可能是真函数入口，
也可能是函数内部标签或 thunk。

当前两个地方仍把所有 seed 都当成强边界：

1. `capBytesAtNextFunctionSeed(...)` 用下一个 seed 截断线性 decode。
2. Bench2 `check_seed_boundaries(...)` 禁止 confirmed block 覆盖任意 seed。

这会让 Low seed 影响真实函数边界。目标是：Low seed 只作为候选入口，不作为函数边界。

## Ghidra 相关实现

Ghidra 不会把所有 code reference 都等价当函数边界：

- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/FunctionStartAnalyzer.java`
  - 会结合函数起始模式和上下文确认函数。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/EntryPointAnalyzer.java`
  - entry point / symbol 等高可信入口更适合作为分析边界。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/FunctionManager.java`
  - confirmed function body 才是强事实。

rizin 侧也区分 xref、flag、function。一个 code pointer 不能直接等同于函数边界。

## native 侧复刻策略

1. 新增 helper 判断 seed 是否可作为边界：High / Medium 可以，Low 不可以。
2. `capBytesAtNextFunctionSeed(...)` 跳过 Low seed。
3. `isKnownOtherFunctionEntry(...)` 也只把 High / Medium seed 当“其他函数入口”边界。
4. Bench2 seed-boundary smoke 只检查非 Low seed，避免测试把候选入口误当强边界。

暂时不做：

- 不删除 Low seed。
- 不强制 decode Low seed。
- 不猜 jump table 或普通函数指针。

## 判断标准

1. `notdec-native-discover` 和 `notdec-native-llvm` 能构建。
2. Bench2 smoke 继续通过。
3. confirmed function / instruction 数量不下降。
4. `elf-relocation-code` seed source 仍存在。

## 风险

1. 如果某个 Low seed 实际是真函数入口，它不会再截断前一个函数的线性 decode。当前单 seed 仍有 8 条 / 64 字节上限，风险可控。
2. 后续如果某类 Low seed 变成可靠来源，应提升 confidence，而不是让所有 Low seed 参与边界判断。

## 实现记录

### 修改范围

1. `lib/NativeAnalysis.cpp`
   - 第 1903 行附近：`isKnownOtherFunctionEntry(...)` 只把 boundary seed 当其他函数入口。
   - 第 1956 行附近：`capBytesAtNextFunctionSeed(...)` 扫描下一个 seed 时跳过 Low confidence seed。
   - 第 1976 行附近：新增 `isBoundarySeed(...)`，当前规则是 confidence 不是 Low。
2. `scripts/bench2-native-smoke.sh`
   - 第 263 行附近：`check_seed_boundaries(...)` 只检查非 Low seed。
3. `ARCHITECTURE.md`
   - 第 67 行附近：记录无 range seed 只按非 Low confidence seed 截断。
   - 第 187 行附近：记录 smoke 的 seed boundary 检查只针对非 Low seed。
4. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
   - 第 28 行附近：阶段 3 记录 Low seed 不参与边界截断。
   - 第 102 行附近：阶段 7 记录 smoke 只检查非 Low seed 边界。

### 行为

Low confidence seed 仍保留在 `functionSeeds()` 和 worklist 中，也可以在预算允许时被 decode。
但它不再用于：

- 截断无 range seed 的 decode 字节数。
- 判断 direct branch successor 是否跳到其他函数入口。
- Bench2 seed-boundary smoke 的强边界检查。

### 验证

```bash
bash -n scripts/bench2-native-smoke.sh
git diff --check
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
```

结果：通过。

快速 summary：

```text
vsftpd: functions=13 blocks=38 instructions=126 xrefs=315 unresolved=0
libuv: functions=11 blocks=28 instructions=91 xrefs=23 unresolved=0
memcached: functions=11 blocks=30 instructions=86 xrefs=187 unresolved=0
```

完整 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260522-80
```

结果：

```text
vsftpd ok elapsed=22s
libuv ok elapsed=30s
memcached ok elapsed=17s
```

metrics：

```text
target	elapsed_seconds	confirmed_functions	basic_blocks	instructions	xrefs_total	xrefs_flow	xrefs_call	xrefs_data	xrefs_string	unresolved_total	unresolved_indirect_call	unresolved_indirect_branch
vsftpd	22	13	38	126	315	19	4	153	139	0	0	0
libuv	30	11	28	91	23	12	3	8	0	0	0	0
memcached	17	11	30	86	187	63	2	20	102	0	0	0
```

当前 Bench2 指标不变。

### 评分

- 实现效果：7/10。低可信候选不会再伪装成强函数边界，边界语义更清楚。
- 复杂度：3/10。改了 native 边界逻辑和对应 smoke。
- 维护成本：3/10。后续新增 confidence 来源时，只需调整 `isBoundarySeed(...)`。
