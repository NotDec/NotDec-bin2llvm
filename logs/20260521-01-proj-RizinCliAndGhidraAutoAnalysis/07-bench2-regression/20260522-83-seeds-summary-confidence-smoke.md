# Seeds Summary Confidence Smoke

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

上一小块已经让 Bench2 smoke 检查 summary 里的 confidence 三个桶加起来等于 `function_seeds`。
但它还没有确认 summary 计数和 `--seeds-json` 里的每个 seed confidence 真的一致。

这次补一个跨 CLI 输出的一致性检查：

```text
summary.confidence == count(seeds-json.seeds[].confidence)
```

## Ghidra 相关实现

Ghidra 的 ProgramDB 里，函数入口相关信息最终来自同一份数据库状态，不同 UI / query 视图应保持一致：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/database/ProgramDB.java`
  - 提供程序级数据库入口。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/Symbol.java`
  - symbol source 和地址用于解释入口来源。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/EntryPointAnalyzer.java`
  - 入口分析消费 symbol / entry 信息。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/FunctionStartAnalyzer.java`
  - 函数起点候选分析。

native 侧没有 ProgramDB，所以用 smoke 保证 `--summary-json` 和 `--seeds-json` 两个视图来自同一状态。

## native 侧复刻策略

1. 在 smoke 中新增 `check_seed_confidence_summary(...)`。
2. 读取 summary 的 `function_seeds` 和 confidence 计数。
3. 读取 seeds JSON 的 `count` 和 `seeds[].confidence`。
4. 要求 seed 总数和 high/medium/low 三个桶完全一致。

## 判断标准

1. `bash -n scripts/bench2-native-smoke.sh` 通过。
2. 完整 Bench2 smoke 通过。
3. 如果 summary confidence 和 seeds JSON 不一致，smoke 会失败。

## 风险

1. 仍然只检查 CLI 输出一致，不判断 confidence 策略本身是否合理。
2. smoke 已经运行 `--seeds-json`，所以这个检查不会新增工具运行次数。

## 实现记录

### 修改内容

- `scripts/bench2-native-smoke.sh:306` 到 `scripts/bench2-native-smoke.sh:342`
  - 新增 `check_seed_confidence_summary(...)`。
  - 读取 summary 和 seeds JSON，检查 `seeds-json.count`、`summary.function_seeds`、实际 seeds 数量一致。
  - 按 `seeds[].confidence` 统计 high / medium / low，并和 `summary.confidence` 对比。
- `scripts/bench2-native-smoke.sh:540` 到 `scripts/bench2-native-smoke.sh:544`
  - 在已生成 `--seeds-json` 后调用交叉检查。
- `ARCHITECTURE.md:184` 到 `ARCHITECTURE.md:190`
  - 记录 smoke 会确认 summary confidence 计数和 seed 列表一致。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:102` 到 `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:107`
  - 记录阶段 7 已补 summary / seeds confidence 交叉检查。

### 验证

```bash
bash -n scripts/bench2-native-smoke.sh
git diff --check
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260522-83
```

结果：通过。

完整 smoke：

```text
vsftpd ok elapsed=23s
libuv ok elapsed=30s
memcached ok elapsed=17s
```

metrics：

```text
target      elapsed_seconds  function_seeds  seed_confidence_high  seed_confidence_medium  seed_confidence_low  confirmed_functions  basic_blocks  instructions  xrefs_total
vsftpd      23               187             187                   0                       0                    13                   38            126           315
libuv       30               485             485                   0                       0                    11                   28            91            23
memcached   17               259             259                   0                       0                    11                   30            86            187
```

### 性能和判断

这次不增加 `notdec-native-discover` 或 LLVM 运行次数，只多一次 Python JSON 检查。
vsftpd 本轮从 22s 到 23s，libuv 30s、memcached 17s 不变，属于 smoke 计时波动；没有看到实际分析性能下降。

实现效果：4/5。能防止 summary 和 seeds 两个 CLI 视图不一致。

复杂度：1/5。复用 smoke 现有 Python JSON 检查风格。

维护成本：1/5。字段来自已有 seeds JSON 和 summary JSON，后续字段名变化时这里会明确失败。
