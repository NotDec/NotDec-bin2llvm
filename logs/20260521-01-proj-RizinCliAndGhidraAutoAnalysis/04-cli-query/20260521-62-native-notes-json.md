# Native Notes JSON

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

native discovery 已经会把 loader、relocation、`.eh_frame`、Sleigh decode 里的非致命问题写到 `NativeProgramState::notes()`。
文本 report 也会打印这些 notes，但机器检查现在只能看 summary、xref、unresolved 等结构化结果。

这次补：

```text
notdec-native-discover --notes-json <elf-file>
```

## Ghidra 相关实现

Ghidra auto analysis 用 `MessageLog` 收集 analyzer 的提示和警告：

- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/AutoAnalysisManager.java`
  - 持有 `MessageLog log = new MessageLog()`。
  - `getMessageLog()` 让插件和 analyzer 读取分析日志。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/AutoAnalysisPlugin.java`
  - 通过 `manager.getMessageLog()` 展示分析过程消息。
- analyzer 的 `added(..., MessageLog log)` 会收到同一个日志对象。
  - `MingwRelocationAnalyzer.java`、`AbstractDemanglerAnalyzer.java`、`DWARFAnalyzer.java` 里都有 `log.appendMsg(...)` 这类用法。

native 侧不需要复刻完整 UI 日志系统，只需要把当前已有 notes 作为轻量 message log 暴露出来。

## native 侧复刻策略

1. `notdec-native-discover` 增加 `--notes-json <elf-file>`。
2. 遍历 `NativeProgramState::notes()`。
3. 输出顶层字段：
   - `notes[]`
   - `count`

暂时不做：

- 不给 note 分级。
- 不改变 notes 产生逻辑。
- 不把 notes 数量作为 Bench2 smoke 通过条件，因为不同目标可能没有 note。

## 判断标准

1. 三个 Bench2 目标输出合法 JSON。
2. `count == len(notes)`。
3. 原有 Bench2 smoke 继续通过。

## 风险

1. notes 是当前 native 分析的提示集合，不等于 Ghidra 的完整 analyzer message log。
2. 当前没有 severity，后续如果要区分 warn/info，需要扩展 `NativeProgramState`。

## 实现记录

已完成 `notdec-native-discover --notes-json <elf-file>`。

修改点：

- `tools/notdec-native-discover.cpp:19` 的 `OutputMode` 增加 `NotesJson`。
- `tools/notdec-native-discover.cpp:49` 的 `printUsage(...)` 增加 `--notes-json`。
- `tools/notdec-native-discover.cpp:163` 的 `parseArgs(...)` 识别 `--notes-json`。
- `tools/notdec-native-discover.cpp:402` 增加 `printNotesJson(...)`，输出 `NativeProgramState::notes()` 和 `count`。
- `tools/notdec-native-discover.cpp:741` 的 `main(...)` 增加 `NotesJson` 分发。
- `ARCHITECTURE.md:34`、`ARCHITECTURE.md:130` 记录新的 notes query。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:32` 更新 Stage 4 进度。

验证：

```bash
git diff --check
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
```

结果：通过。

```bash
notdec-native-discover --notes-json <bench2-elf>
python3 -m json.tool <out>
```

专项结果：

```text
vsftpd notes=0
libuv notes=0
memcached notes=0
```

三项目 JSON 合法，`count == len(notes)`。

完整 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-62
```

结果：

```text
vsftpd elapsed=10s confirmed=9 blocks=30 inst=80 xrefs=304 unresolved=0
libuv elapsed=22s confirmed=9 blocks=29 inst=85 xrefs=26 unresolved=0
memcached elapsed=10s confirmed=9 blocks=30 inst=80 xrefs=186 unresolved=0
```

性能：新增接口只格式化已有 notes，不改变 native 分析流程。smoke 耗时仍在当前基线范围。

评分：

- 实现效果：8/10。现在 loader、relocation、`.eh_frame`、Sleigh 的非致命提示可以机器查询。
- 复杂度：9/10。只新增一个 formatter 和 CLI 分支。
- 维护成本：9/10。字段直接来自 `NativeProgramState::notes()`，以后扩展 severity 时再改 state。
