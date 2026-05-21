# Heritage Compare Metrics

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

上一小块已经让 Bench2 native smoke 输出 `metrics.tsv`，记录 native route 的函数、block、
xref、unresolved 和耗时。Stage 7 还要求和 GhidraScript heritage 路线保留同口径对比。

Bench2 当前已有两个可直接消费的 heritage module：

- `/sn640/NotDec-Exp/Bench2/bin2llvm-ir/vsftpd/module-limit5.json`
- `/sn640/NotDec-Exp/Bench2/bin2llvm-ir/libuv/module-limit5.json`

`memcached` 当前只有单函数 JSON，没有 module-limit5。

## Ghidra 相关实现

heritage route 的同口径来源是 GhidraScript 导出的 module JSON：

- `ghidra_scripts/ExportHeritageModule.java::run()`
  - 遍历 Ghidra function，成功 decompile 的写入 `functions[]`，失败的写入 `failures[]`。
- `ghidra_scripts/ExportHeritageModule.java::writeModuleStats(...)`
  - 写 `attemptedFunctionCount`、`functionCount`、`failureCount`、`externalCount`、`elapsedMs`。
- `tools/notdec-heritage-module-check.cpp::printSummary(...)`
  - 从 heritage module 统计 functions、externals、failures、direct calls、resolved calls、unknown calls。

native 侧不重新跑 Ghidra；这次只消费已有 module JSON，把 heritage 关键数字和 native smoke 数字放到同一个输出目录里。

## native 侧复刻策略

1. `bench2-native-smoke.sh` 增加 `--bench2-ir-root DIR`，默认
   `/sn640/NotDec-Exp/Bench2/bin2llvm-ir`。
2. 每个目标 smoke 结束后，如果存在 `$BENCH2_IR_ROOT/$name/module-limit5.json`：
   - 调 `notdec-heritage-module-check`，保存输出。
   - 写 `heritage-metrics.tsv`。
   - 写 `native-heritage-compare.tsv`，把 native confirmed/call/unresolved 和 heritage
     functions/direct calls/unknown calls 放同一行。
3. 如果没有 module-limit5，只在 compare TSV 中标成 `heritage_available=0`，不让 smoke 失败。

暂时不做：

- 不自动运行 Ghidra。
- 不把 single-function heritage JSON 混进 module 对比。
- 不用 heritage 数字作为硬性门槛；现在只保留趋势对比。

## 判断标准

1. smoke 输出目录生成 `heritage-metrics.tsv` 和 `native-heritage-compare.tsv`。
2. `vsftpd` / `libuv` 的 heritage module 被检查并写入 metrics。
3. `memcached` 没有 module-limit5 时不失败，compare 中标记不可用。
4. 原有 native smoke、LLVM 22 verify、IR pattern 检查继续通过。

## 风险

1. 当前 heritage module 是 `--limit=5` 的旧产物，不能直接代表完整 Ghidra 覆盖率。
2. heritage 和 native 入口集合不完全一致；这次只做数字并排，不做结论判定。

## 实现记录

### 改动

- `scripts/bench2-native-smoke.sh:9` 增加 `BENCH2_IR_ROOT`，默认
  `/sn640/NotDec-Exp/Bench2/bin2llvm-ir`。
- `scripts/bench2-native-smoke.sh:31` 增加 `--bench2-ir-root` 参数。
- `scripts/bench2-native-smoke.sh:56` 增加 `notdec-heritage-module-check` 路径。
- `scripts/bench2-native-smoke.sh:156` 增加 `parse_heritage_metric(...)` 和
  `require_heritage_metric(...)`。
- `scripts/bench2-native-smoke.sh:256` 创建 `heritage-metrics.tsv` 和
  `native-heritage-compare.tsv`。
- `scripts/bench2-native-smoke.sh:381` 如果目标存在 `module-limit5.json`，运行
  `notdec-heritage-module-check` 并写 heritage / compare metrics；不存在时标记
  `heritage_available=0`，不让 smoke 失败。
- `ARCHITECTURE.md:145` 记录 smoke 输出 heritage 对比 TSV。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:76` 更新阶段 7 进度。

### 验证

格式和脚本语法：

```bash
git diff --check
bash -n scripts/bench2-native-smoke.sh
```

结果：通过。

构建相关工具：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-heritage-module-check notdec-native-discover notdec-native-llvm -j2
```

结果：通过。

Bench2 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-52
```

结果：

```text
vsftpd ok elapsed=8s
libuv ok elapsed=19s
memcached ok elapsed=7s
```

`heritage-metrics.tsv`：

```text
target	heritage_available	functions	externals	failures	direct_calls	resolved_internal_calls	resolved_external_calls	unknown_calls
vsftpd	1	5	95	0	240	0	240	0
libuv	1	5	2	0	4	0	4	0
memcached	0							
```

`native-heritage-compare.tsv`：

```text
target	heritage_available	native_confirmed_functions	heritage_functions	native_call_xrefs	heritage_direct_calls	native_unresolved_total	heritage_unknown_calls
vsftpd	1	9	5	2	240	0	0
libuv	1	9	5	3	4	0	0
memcached	0	9		2		0	
```

性能：本次 smoke 合计约 34s，三个目标分别约 8s / 19s / 7s。新增 heritage check 只消费已有
module JSON，未看到 smoke 时间明显变慢。

### 评分

- 实现效果：7/10。已有 heritage module 现在能和 native metrics 放到同一输出目录里对比。
- 复杂度：3/10。只扩展 smoke 脚本，未新增 native 分析逻辑。
- 维护成本：3/10。依赖 `notdec-heritage-module-check` 文本输出，后续如果工具输出改动要同步脚本。

### 未做

- 没有自动运行 GhidraScript 生成新的 heritage module。
- 没有对 single-function JSON 做混合对比。
- 没有把 heritage 数字作为硬性通过门槛。
