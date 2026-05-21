# Native Smoke Metrics

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

`scripts/bench2-native-smoke.sh` 已经固定跑 `vsftpd`、`libuv`、`memcached`，并保存每个目标的
summary JSON、IR、bitcode 和工具 stderr/stdout。现在缺的是一个稳定的汇总文件。

总计划 Stage 7 的判断标准要求记录函数数、block 数、xref 数、unresolved 数、失败原因和运行时间。
当前这些数据分散在 summary JSON 和终端输出里，不方便跨提交对比。

## Ghidra 相关实现

Ghidra 的 auto analysis 不是只给“成功/失败”，它会保留日志和可查询的 program facts：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/app/util/importer/MessageLog.java`
  - `MessageLog` 用于收集 import / analysis 过程中的警告和错误。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/AutoAnalysisManager.java`
  - auto analysis manager 调度 analyzer，并让各 analyzer 通过 Program state 留下函数、引用等结果。
- 各 analyzer 的 `added(Program, AddressSetView, TaskMonitor, MessageLog)` 入口
  - 例如 ELF、PLT、processor analyzer 都会在同一 Program 里沉淀可查询结果。

native 侧不做 Ghidra GUI，也不做 ProgramDB；这次只补 smoke 的汇总产物，让每次 Bench2 验证能留下同口径数字。

## native 侧复刻策略

1. 在 `bench2-native-smoke.sh` 的输出目录写 `metrics.tsv`。
2. 表头固定，包含 target、elapsed_seconds、confirmed_functions、basic_blocks、instructions、
   xrefs total/flow/call/data/string、unresolved total/call/branch。
3. 数据从现有 summary JSON 解析，不新增工具依赖。
4. 每个目标 smoke 成功后追加一行；失败时保留已有分散日志供定位。

暂时不做：

- 不把 metrics 和 GhidraScript heritage 做自动 diff。
- 不记录完整失败分类；失败时仍依赖当前 stdout/stderr 文件。
- 不新增 Python/JQ 依赖。

## 判断标准

1. smoke 输出目录稳定生成 `metrics.tsv`。
2. 三个 Bench2 目标各有一行。
3. 指标能反映当前 summary JSON 的函数、block、xref、unresolved 数和运行时间。
4. 现有 LLVM 22 verify 和 IR pattern 检查继续通过。

## 风险

1. shell 解析 JSON 只适合当前 summary 的简单数字字段；如果 JSON 结构变了，需要同步改脚本。
2. metrics 只是回归对比辅助，不证明语义正确。

## 实现记录

### 改动

- `scripts/bench2-native-smoke.sh:125` 新增 `summary_number_first(...)` 和
  `summary_number_last(...)`，从当前 summary JSON 中抽取简单数字字段。
- `scripts/bench2-native-smoke.sh:139` 新增 `require_summary_number(...)`，避免 summary 结构变化时
  静默写空字段。
- `scripts/bench2-native-smoke.sh:229` 在输出目录创建 `metrics.tsv`，写固定表头。
- `scripts/bench2-native-smoke.sh:318` 在每个目标通过 discovery、LLVM verify 和 IR pattern
  检查后，追加一行 metrics。
- `ARCHITECTURE.md:145` 记录 smoke 输出 `metrics.tsv` 的职责。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:75` 更新阶段 7 进度。

### 验证

格式检查：

```bash
git diff --check
```

结果：通过。

Bench2 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-51
```

结果：

```text
vsftpd ok elapsed=8s
libuv ok elapsed=19s
memcached ok elapsed=7s
```

`metrics.tsv` 内容：

```text
target	elapsed_seconds	confirmed_functions	basic_blocks	instructions	xrefs_total	xrefs_flow	xrefs_call	xrefs_data	xrefs_string	unresolved_total	unresolved_indirect_call	unresolved_indirect_branch
vsftpd	8	9	30	80	304	14	2	149	139	0	0	0
libuv	19	9	29	85	26	10	3	13	0	0	0	0
memcached	7	9	30	80	186	14	2	68	102	0	0	0
```

性能：本次 smoke 合计约 34s，三个目标分别约 8s / 19s / 7s。新增 TSV 写入只做少量
`sed` 解析和 `printf`，没有看到 smoke 时间明显变慢。

### 评分

- 实现效果：8/10。Stage 7 要的核心数字现在有固定汇总文件。
- 复杂度：2/10。只改 smoke 脚本，不改 native analysis 或 lowering。
- 维护成本：3/10。shell JSON 解析依赖当前 summary 结构，后续字段变动时要同步更新。

### 未做

- 没有自动对比 GhidraScript heritage 路线。
- 没有完整失败分类汇总；失败定位仍看各 stdout/stderr 文件。
