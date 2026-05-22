# Summary Confidence JSON

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

`NativeFunctionSeed` 已经有 High / Medium / Low confidence。native decode 也开始按 confidence
分层消费 seed，并且 Low seed 不再作为函数边界。

但 `notdec-native-discover --summary-json` 还没有输出 confidence 计数。现在要看 Low seed 是否存在，
只能跑 `--seeds-json` 再手动统计。

这次补：

```json
"confidence": {
  "high": ...,
  "medium": ...,
  "low": ...
}
```

## Ghidra 相关实现

Ghidra 的入口和函数发现能从 ProgramDB / Symbol / Analyzer 状态追踪来源和可信程度：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/Symbol.java`
  - symbol source 和属性用于解释入口来源。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/EntryPointAnalyzer.java`
  - 高可信 entry/symbol 入口触发反汇编。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/FunctionStartAnalyzer.java`
  - 更保守地处理函数起始候选。

native 侧没有完整 ProgramDB，这里先把已有 confidence 状态暴露到 summary。

## native 侧复刻策略

1. `printSummaryJson(...)` 遍历 `state.functionSeeds()`。
2. 按 `NativeFunctionConfidence` 统计 high/medium/low。
3. 输出到顶层 `"confidence"` 对象。
4. 不改变 `--seeds-json`，不改变分析逻辑。

## 判断标准

1. `notdec-native-discover` 能构建。
2. 三个 Bench2 目标 `--summary-json` 都能输出 `confidence.high/medium/low`。
3. `high + medium + low == function_seeds`。
4. 完整 Bench2 smoke 继续通过。

## 风险

1. 只是 summary 可观测性，不代表低可信 seed 已被 decode。
2. smoke 如果依赖这些字段，后续 confidence 策略变化时要同步调整。

## 实现记录

### 修改内容

- `tools/notdec-native-discover.cpp:335` 到 `tools/notdec-native-discover.cpp:363`
  - 在 `printSummaryJson(...)` 中遍历 `state.functionSeeds()`，按 `NativeFunctionConfidence` 统计 seed 数量。
  - 在 `--summary-json` 顶层输出 `"confidence"`，固定包含 `high`、`medium`、`low` 三个字段。
- `ARCHITECTURE.md:34` 到 `ARCHITECTURE.md:40`
  - 补充 `--summary-json` 现在包含 seed source 和 confidence 计数。
- `ARCHITECTURE.md:144` 到 `ARCHITECTURE.md:148`
  - 更新 `tools/notdec-native-discover.cpp` 的 CLI 功能说明。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:36` 到 `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:39`
  - 记录阶段 4 已补 summary seed confidence 计数。

### 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
```

结果：通过。

空白检查：

```bash
git diff --check
```

结果：通过。

三个 Bench2 目标检查 `high + medium + low == function_seeds`：

```bash
for target in \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
do
  /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json "$target" \
    | jq -r '[.function_seeds,.confidence.high,.confidence.medium,.confidence.low,((.confidence.high+.confidence.medium+.confidence.low)==.function_seeds)] | @tsv'
done
```

结果：

```text
vsftpd      187  187  0  0  true
libuv       485  485  0  0  true
memcached   259  259  0  0  true
```

Bench2 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260522-81
```

结果：

```text
vsftpd ok elapsed=22s
libuv ok elapsed=30s
memcached ok elapsed=17s
```

summary 关键指标：

```text
target      seeds  high  medium  low  confirmed  blocks  instructions  xrefs
vsftpd      187    187   0       0    13         38      126           315
libuv       485    485   0       0    11         28      91            23
memcached   259    259   0       0    11         30      86            187
```

### 性能和判断

这次只在 summary 输出阶段遍历一次 `functionSeeds()`，不改变 loader、decode、xref 或 lowering。
Bench2 smoke 耗时和上一轮基线一致：vsftpd 22s、libuv 30s、memcached 17s，没有观察到性能下降。

实现效果：4/5。CLI 可直接看到 confidence 分布，便于后续判断 Low seed 是否被误用。

复杂度：1/5。只增加一个小统计块和 JSON 字段，没有新数据结构。

维护成本：1/5。字段名来自已有 `NativeFunctionConfidence` 字符串，后续 confidence 枚举变化时需要同步这里的固定输出列表。
