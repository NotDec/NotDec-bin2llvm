# Seed Confidence Priority

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

`SleighSeedInstructionAnalyzer` 当前从 `FunctionWorklist` 取前 10 个 seed 作为初始 decode 队列。
随着 relocation code pointer 被加入低可信 seed，单纯插入顺序不够稳：如果后续 analyzer 顺序变化，
低可信 seed 可能抢掉高可信 entry、symbol、`.eh_frame` seed。

这次只改初始 seed 入队策略：

- 先取 high confidence seed。
- 再取 medium。
- 最后取 low。
- 同一 confidence 内保留 worklist 原顺序。

## Ghidra 相关实现

Ghidra 的 AutoAnalysis 不是简单按发现顺序全部等价处理：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/app/services/Analyzer.java`
  - analyzer 有 priority 和 enablement。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/app/plugin/core/analysis/AnalysisScheduler.java`
  - 按任务和 analyzer 调度分析。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/EntryPointAnalyzer.java`
  - entry point / symbol 这类高可信入口优先触发反汇编。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/FunctionStartAnalyzer.java`
  - 函数起始模式、代码引用等来源更保守。

rizin 侧 `aa` / `aaf` 也会把 symbol、entry 和递归分析分层处理。native 侧还没有完整 scheduler，
先在 bounded queue 入口做一个最小优先级。

## native 侧复刻策略

1. `enqueueInitialSeeds(...)` 不再直接遍历前 10 个 worklist item。
2. 按 `NativeFunctionSeed::Confidence` 过滤三轮：High、Medium、Low。
3. 每轮仍按 `FunctionWorklist` 原顺序扫描，保持可解释性。
4. 不改变 `MaxInitialSeeds` / `MaxSeeds`。
5. direct call / direct branch 运行中入队规则不变。

## 判断标准

1. `notdec-native-discover` 和 `notdec-native-llvm` 能构建。
2. Bench2 smoke 继续通过。
3. 三目标 confirmed function / instruction 指标不下降。
4. 低可信 relocation seed 仍存在，但不抢占初始高可信 seed。

## 风险

1. 如果某个目标的高可信 seed 特别多，low seed 仍不会被当前 10 个初始名额消费。这是有意保守。
2. 后续如果要更积极消费 low seed，应先补更可靠的函数边界。

## 实现记录

### 修改范围

1. `lib/NativeAnalysis.cpp`
   - 第 1413 行附近：更新 `SleighSeedInstructionAnalyzer::enqueueInitialSeeds(...)`。
   - 第 1418 行附近：按 High、Medium、Low 三轮扫描 worklist。
   - 第 1425 行附近：通过 `state.functionSeeds()` 查 seed confidence。
   - 第 1430 行附近：同层仍调用原 `enqueueSeed(...)`，保持去重和 executable 检查。
2. `ARCHITECTURE.md`
   - 第 62 行附近：记录初始 seed 现在按 confidence 分层取前 10 个。
3. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
   - 第 21 行附近：阶段 3 记录 confidence-aware 初始 seed 入队。

### 行为

初始 decode 队列现在优先消费高可信 seed，低可信 relocation code seed 不会因为插入顺序变化抢占
高可信入口。预算仍是初始 10 个、同轮 20 个。direct call 和 direct branch 的同轮入队逻辑不变。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
git diff --check
```

结果：通过。

快速 summary 检查：

```text
vsftpd: functions=13 blocks=38 instructions=126 xrefs=315 unresolved=0
libuv: functions=11 blocks=28 instructions=91 xrefs=23 unresolved=0
memcached: functions=11 blocks=30 instructions=86 xrefs=187 unresolved=0
```

完整 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260522-79
```

结果：

```text
vsftpd ok elapsed=23s
libuv ok elapsed=30s
memcached ok elapsed=17s
```

metrics：

```text
target	elapsed_seconds	confirmed_functions	basic_blocks	instructions	xrefs_total	xrefs_flow	xrefs_call	xrefs_data	xrefs_string	unresolved_total	unresolved_indirect_call	unresolved_indirect_branch
vsftpd	23	13	38	126	315	19	4	153	139	0	0	0
libuv	30	11	28	91	23	12	3	8	0	0	0	0
memcached	17	11	30	86	187	63	2	20	102	0	0	0
```

confirmed function / instruction 数量没有下降。

### 评分

- 实现效果：6/10。调度更稳，避免低可信 seed 抢预算，但不新增 decode 覆盖。
- 复杂度：2/10。只改初始队列选择逻辑。
- 维护成本：2/10。后续如果引入更完整 scheduler，可以替换这段分层扫描。
