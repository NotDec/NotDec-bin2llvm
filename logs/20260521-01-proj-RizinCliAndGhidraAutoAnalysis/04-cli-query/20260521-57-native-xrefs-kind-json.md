# Native Xrefs Kind JSON

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

Stage 4 已经有全量 xref JSON 和按地址的 from/to 查询。Stage 5 已经把 xref 分成
`flow`、`call`、`data`、`string`。现在排查时如果只想看 string 或 data 引用，仍要客户端自己过滤。

这次补：

```text
notdec-native-discover --xrefs-kind-json <flow|call|data|string> <elf-file>
```

## Ghidra 相关实现

Ghidra 的引用查询不是只给一张全量表，而是能按地址和引用类型取：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java`
  - `getReferencesFrom(...)`、`getReferencesTo(...)` 提供 from/to 查询。
  - `getReferenceSourceIterator(...)` 等接口支撑批量遍历引用来源。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/RefType.java`
  - 表达 call、jump、data、read/write 等引用类型。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/database/references/ReferenceDBManager.java`
  - ProgramDB 里引用表的主要实现。

native 侧没有 Ghidra 的完整 `RefType` 体系，当前只保留 Bench2 smoke 需要的四类：
`NativeXrefKind::Flow`、`Call`、`Data`、`String`。

## native 侧复刻策略

1. `notdec-native-discover` 增加 `--xrefs-kind-json <kind> <elf-file>`。
2. 只接受当前已有的四个 kind 字符串，避免引入新分类。
3. 输出字段沿用 `--xrefs-json` 的 xref 对象，并在顶层加 `kind` 和 `count`。
4. 不改变 xref 采集、分类和去重逻辑。

## 判断标准

1. 三个 Bench2 目标的四类 kind 查询都能输出合法 JSON。
2. 每类 `count` 和 `--summary-json xrefs.<kind>` 一致。
3. 原有 Bench2 smoke 继续通过。

## 风险

1. 当前分类比 Ghidra 粗，先只暴露已有分类，不假装已经有完整 `RefType`。
2. 这是查询接口，不应该影响 discovery 和 lowering 行为。

## 实现记录

已完成 `notdec-native-discover --xrefs-kind-json <flow|call|data|string> <elf-file>`。

修改点：

- `tools/notdec-native-discover.cpp:19` 的 `OutputMode` 增加 `XrefsKindJson`。
- `tools/notdec-native-discover.cpp:36` 的 `CliOptions` 增加 `QueryXrefKind`。
- `tools/notdec-native-discover.cpp:46` 的 `printUsage(...)` 增加新用法。
- `tools/notdec-native-discover.cpp:79` 增加 `parseXrefKind(...)`，只接受现有四类 xref kind。
- `tools/notdec-native-discover.cpp:98` 的 `parseArgs(...)` 解析 `--xrefs-kind-json`。
- `tools/notdec-native-discover.cpp:430` 增加 `printXrefsKindJson(...)`，复用现有 xref JSON 对象输出。
- `tools/notdec-native-discover.cpp:634` 的 `main(...)` 在输出分发里增加 `XrefsKindJson`。
- `ARCHITECTURE.md:34`、`ARCHITECTURE.md:125` 记录新的 xref kind 查询。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:31` 更新 Stage 4 进度。

验证：

```bash
git diff --check
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
```

结果：通过。

```bash
for target in vsftpd libuv memcached; do
  for kind in flow call data string; do
    notdec-native-discover --xrefs-kind-json "$kind" "$elf" > "$out"
    python3 -m json.tool "$out" >/dev/null
  done
done
```

结果：每类 `count` 都和 `--summary-json xrefs.<kind>` 一致。

```text
vsftpd    flow 14  call 2  data 149  string 139
libuv     flow 10  call 3  data 13   string 0
memcached flow 14  call 2  data 68   string 102
```

Bench2 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-57
```

结果：通过。

```text
target    elapsed_seconds  confirmed_functions  basic_blocks  instructions  xrefs_total  unresolved_total
vsftpd    7                9                    30            80            304          0
libuv     19               9                    29            85            26           0
memcached 8                9                    30            80            186          0
```

性能：新增接口只过滤已存在的 `state.xrefs()`，复杂度是一次线性遍历；Bench2 smoke 主流程不额外调用它。

评分：

- 实现效果：8/10。能直接按 native xref 分类排查 flow/call/data/string。
- 复杂度：9/10。没有新增状态或采集逻辑。
- 维护成本：9/10。以后新增 `NativeXrefKind` 时只需要扩展解析函数和测试。
