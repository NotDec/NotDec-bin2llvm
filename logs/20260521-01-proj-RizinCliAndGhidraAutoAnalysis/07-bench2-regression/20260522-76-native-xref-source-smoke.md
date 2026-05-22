# Native XRef Source Smoke

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

上一小块把 executable relocation target 从 `data` 改成 `flow`，source 为
`elf-relocation-code`。当前 Bench2 smoke 只记录 `flow/data/string` 总数，不能直接挡住
source 分类回退。

这次在 smoke 中补 xref source 检查：

- 三个目标都要有 `elf-relocation-code`
- 三个目标都要保留 `elf-relocation-pointer`
- `vsftpd` 和 `memcached` 要保留 `elf-relocation-string`

## Ghidra 相关实现

Ghidra 的 reference regression 依赖 ProgramDB 里的引用表：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java`
  - `getReferencesFrom(...)`
  - `getReferencesTo(...)`
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/Reference.java`
  - 保存 from、to、reference type。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/RefType.java`
  - 区分 data、flow、computed call/jump 等引用类型。

rizin 侧对应 `axl` / `axf` / `axt` 查询 xref；native smoke 用 `--xrefs-json` 固定检查当前
Bench2 的关键来源。

## native 侧复刻策略

1. smoke 每个目标额外运行 `notdec-native-discover --xrefs-json`。
2. 用 Python 解析 JSON，按 `(kind, source)` 计数。
3. 检查当前 Bench2 必须存在的 relocation source。
4. 不检查精确数量，只检查非零，避免目标二进制小版本变化时过脆。

暂时不做：

- 不把所有 source 都写进 `metrics.tsv`，避免表过宽。
- 不用 grep 解析 JSON。
- 不新增 native CLI。

## 判断标准

1. `bash -n scripts/bench2-native-smoke.sh` 通过。
2. 完整 Bench2 smoke 通过。
3. smoke 输出目录里生成每个目标的 `*.xrefs.json`。
4. 如果 `elf-relocation-code` 消失，smoke 会失败。

## 风险

1. 额外生成 `--xrefs-json` 会略增 smoke 时间，但三目标 xref 数量当前不大。
2. 如果 Bench2 rootfs 目标换版本，可能需要调整 source baseline。

## 实现记录

### 修改范围

1. `scripts/bench2-native-smoke.sh`
   - 第 281 行附近：新增 `check_xref_sources(...)`，用 Python 解析 `--xrefs-json`。
   - 第 300 行附近：按 `(kind, source)` 计数。
   - 第 301 行附近：三目标要求 `flow/elf-relocation-code` 和 `data/elf-relocation-pointer`。
   - 第 305 行附近：`vsftpd` / `memcached` 额外要求 `string/elf-relocation-string`。
   - 第 452 行附近：每个目标新增 `$name.xrefs.json` 输出。
   - 第 480 行附近：运行 `--xrefs-json` 并调用 `check_xref_sources(...)`。
2. `ARCHITECTURE.md`
   - 第 184 行附近：记录 smoke 会检查 relocation code/data/string xref source baseline。
3. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
   - 第 100 行附近：阶段 7 记录 xref source baseline 已完成。

### 行为

Bench2 smoke 现在会在 discovery summary、seeds、blocks 检查后，额外生成：

```text
<target>.xrefs.json
<target>.xrefs.stderr
```

并检查 relocation source 是否仍按当前语义分类。它只检查存在性，不检查精确数量。

### 验证

```bash
bash -n scripts/bench2-native-smoke.sh
git diff --check
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260522-76
```

结果：通过。

完整 smoke：

```text
vsftpd ok elapsed=22s
libuv ok elapsed=29s
memcached ok elapsed=18s
```

metrics：

```text
target	elapsed_seconds	confirmed_functions	basic_blocks	instructions	xrefs_total	xrefs_flow	xrefs_call	xrefs_data	xrefs_string	unresolved_total	unresolved_indirect_call	unresolved_indirect_branch
vsftpd	22	13	38	126	315	19	4	153	139	0	0	0
libuv	29	11	28	91	23	12	3	8	0	0	0	0
memcached	18	11	30	86	187	63	2	20	102	0	0	0
```

生成文件：

```text
libuv.xrefs.json
memcached.xrefs.json
vsftpd.xrefs.json
```

source baseline：

```text
libuv: elf-relocation-code=3, elf-relocation-pointer=1
memcached: elf-relocation-code=50, elf-relocation-pointer=5, elf-relocation-string=102
vsftpd: elf-relocation-code=2, elf-relocation-pointer=140, elf-relocation-string=139
```

性能：新增一次 `--xrefs-json` discovery，smoke 时间略增，但三目标仍在可接受范围内。

### 评分

- 实现效果：7/10。能挡住 relocation xref source 分类回退。
- 复杂度：2/10。复用已有 `--xrefs-json`，只加 smoke 检查。
- 维护成本：3/10。source baseline 绑定 Bench2 rootfs，但只检查非零，版本变化时不太脆。
