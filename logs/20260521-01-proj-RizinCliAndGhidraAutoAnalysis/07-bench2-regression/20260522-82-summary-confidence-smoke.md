# Summary Confidence Smoke

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

`notdec-native-discover --summary-json` 已经输出 seed confidence 计数：

```json
"confidence": {
  "high": ...,
  "medium": ...,
  "low": ...
}
```

但 Bench2 smoke 还没有检查这些字段。后续如果 summary 漏字段，或者计数和 `function_seeds`
不一致，当前回归不会失败。

这次补 smoke 检查，并把 confidence 数量写进 `metrics.tsv`。

## Ghidra 相关实现

Ghidra 的分析结果会把入口、符号、引用和函数状态留在 ProgramDB，CLI 和测试可以从同一套状态读出统计：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/Symbol.java`
  - symbol 的 source / address / name 能解释入口来源。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/EntryPointAnalyzer.java`
  - 消费入口和符号来源。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/FunctionStartAnalyzer.java`
  - 消费更弱的函数起点候选。

native 侧没有 ProgramDB，这里只对已有 summary 做一致性检查。

## native 侧复刻策略

1. 在 `scripts/bench2-native-smoke.sh` 中读取 `function_seeds`、`confidence.high`、`confidence.medium`、`confidence.low`。
2. 要求四个数字都存在。
3. 要求 `high + medium + low == function_seeds`。
4. 在 `metrics.tsv` 中增加 `function_seeds`、`seed_confidence_high`、`seed_confidence_medium`、`seed_confidence_low`。

## 判断标准

1. `bash -n scripts/bench2-native-smoke.sh` 通过。
2. 完整 Bench2 smoke 通过。
3. `metrics.tsv` 能看到三个 confidence 计数列。
4. smoke 耗时不明显增加。

## 风险

1. 这里只检查计数一致，不判断 high/medium/low 策略是否合理。
2. `metrics.tsv` 增加列后，人工对比旧 TSV 时需要注意列数变化。

## 实现记录

### 修改内容

- `scripts/bench2-native-smoke.sh:187` 到 `scripts/bench2-native-smoke.sh:205`
  - 新增 `check_summary_confidence(...)`，检查 `function_seeds`、`confidence.high`、`confidence.medium`、`confidence.low` 都存在。
  - 检查 `high + medium + low == function_seeds`，不一致时直接让 smoke 失败。
- `scripts/bench2-native-smoke.sh:460` 到 `scripts/bench2-native-smoke.sh:462`
  - `metrics.tsv` 表头增加 `function_seeds`、`seed_confidence_high`、`seed_confidence_medium`、`seed_confidence_low`。
- `scripts/bench2-native-smoke.sh:571` 到 `scripts/bench2-native-smoke.sh:607`
  - 从 summary 读取 seed confidence 指标，调用一致性检查，并写入 `metrics.tsv`。
- `ARCHITECTURE.md:181` 到 `ARCHITECTURE.md:193`
  - 记录 Bench2 smoke 会检查 seed confidence 计数，并把这些指标写入 metrics。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:102` 到 `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:106`
  - 记录阶段 7 已补 seed confidence 计数一致性检查。

### 验证

```bash
bash -n scripts/bench2-native-smoke.sh
git diff --check
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260522-82
```

结果：通过。

完整 smoke：

```text
vsftpd ok elapsed=22s
libuv ok elapsed=30s
memcached ok elapsed=17s
```

`metrics.tsv`：

```text
target     elapsed_seconds  function_seeds  seed_confidence_high  seed_confidence_medium  seed_confidence_low  confirmed_functions  basic_blocks  instructions  xrefs_total
vsftpd     22               187             187                   0                       0                    13                   38            126           315
libuv      30               485             485                   0                       0                    11                   28            91            23
memcached  17               259             259                   0                       0                    11                   30            86            187
```

### 性能和判断

这次只解析已有 summary JSON 的四个数字，不增加工具运行次数。Bench2 smoke 耗时仍是
vsftpd 22s、libuv 30s、memcached 17s，没有观察到性能下降。

实现效果：4/5。能防止 confidence 字段漏出或计数错误，并把数据留在 metrics。

复杂度：1/5。只是 smoke helper 和 TSV 列。

维护成本：2/5。`metrics.tsv` 列数变多，后续人工或脚本消费这个文件时要按新表头处理。
