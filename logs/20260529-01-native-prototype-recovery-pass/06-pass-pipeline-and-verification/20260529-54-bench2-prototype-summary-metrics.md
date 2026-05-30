# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

Bench2 smoke 现在会打开 `--prototype-recovery-summary`，并要求 `functions`、`external inputs`、`input candidates`、`return candidates` 四个指标非零。

但这些数字只在每个目标的 `native-llvm.stderr` 里。`metrics.tsv` 还没有记录 prototype recovery 的同口径数字，后续比较退化时不够直观。

# Ghidra 实现参考

Ghidra decompiler 会把 prototype 状态作为 action pipeline 的一部分持续维护：

- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：运行 prototype 类型恢复。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`：生成 input map。
  - `FuncCallSpecs::deriveOutputMap(...)`：生成 output map。
  - `FuncProto::updateAllTypes(...)`：更新最终 prototype。

native 侧还没有完整 action trace。`NativePrototypeRecoverySummary` 是现阶段最直接的观测值，应该出现在 Bench2 汇总指标里。

# native 侧复刻策略

- 不改 pass 逻辑。
- 只扩展 `scripts/bench2-native-smoke.sh` 的 `metrics.tsv`。
- 在现有 discovery / IR 指标后追加四列：
  - `prototype_functions`
  - `prototype_external_inputs`
  - `prototype_input_candidates`
  - `prototype_return_candidates`
- 复用上一小步已经解析和校验过的变量，不重复跑命令。

暂时不做：

- 不固定 golden 数字。
- 不新增新的 metrics 文件。
- 不把每函数 summary 展开成表。

# 判断标准

- Bench2 smoke 通过。
- `metrics.tsv` 表头和每行列数一致。
- 表里能直接看到 prototype recovery 四个总数。
- 全量 CTest 继续通过。

# 风险

这是脚本输出格式变化。`metrics.tsv` 的消费者如果假设固定列数，可能需要适配；但追加列比改旧列更稳。

# 实现记录

## 改动

- `scripts/bench2-native-smoke.sh:431` 到 `:440` 增加 `check_tsv_columns(...)`。
  - 脚本结束前检查 `metrics.tsv` 表头和数据行列数一致。
- `scripts/bench2-native-smoke.sh:501` 到 `:503` 扩展 `metrics.tsv` 表头。
  - 新增 `prototype_functions`。
  - 新增 `prototype_external_inputs`。
  - 新增 `prototype_input_candidates`。
  - 新增 `prototype_return_candidates`。
- `scripts/bench2-native-smoke.sh:664` 到 `:672` 将 prototype summary 四个总数写入每个目标的 metrics 行。
- `scripts/bench2-native-smoke.sh:717` 在三目标跑完后调用 `check_tsv_columns "$METRICS"`。

## 验证

已通过：

```sh
bash -n scripts/bench2-native-smoke.sh
git diff --check
OUT_DIR=/tmp/notdec-bin2llvm-bench2-prototype-metrics-smoke \
  scripts/bench2-native-smoke.sh --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-bench2-prototype-metrics-smoke
awk -F '\t' '{print NR, NF}' /tmp/notdec-bin2llvm-bench2-prototype-metrics-smoke/metrics.tsv
ctest --test-dir build --output-on-failure
```

Bench2 `metrics.tsv` 新增列后的结果：

| target | elapsed | prototype_functions | prototype_external_inputs | prototype_input_candidates | prototype_return_candidates |
| --- | ---: | ---: | ---: | ---: | ---: |
| vsftpd | 43s | 187 | 924 | 163 | 59 |
| libuv | 112s | 485 | 1739 | 321 | 165 |
| memcached | 59s | 259 | 1196 | 224 | 99 |

`metrics.tsv` 表头和三行数据都是 21 列。

全量 CTest 通过，`9/9`，约 `1.30 sec`。

## 性能影响

不改生产代码。Bench2 smoke 不增加额外 lowering，只多写 4 个 TSV 字段并做一次 awk 列数检查。本次三目标总耗时约 `214s`。

## 评分

- 实现效果：5/10。prototype recovery 的 Bench2 总量现在进入同口径 metrics，便于后续看退化。
- 复杂度：1/10。只改 TSV 输出和列数检查。
- 后期维护成本：2/10。追加列会影响固定列消费者，但字段名明确，旧字段顺序不变。
