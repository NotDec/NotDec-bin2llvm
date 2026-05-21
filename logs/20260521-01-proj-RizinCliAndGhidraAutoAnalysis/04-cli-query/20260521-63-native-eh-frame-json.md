# Native Eh Frame JSON

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

native 已有 `EhFrameAnalyzer`，会解析 `.eh_frame` 和 `.eh_frame_hdr`，并把 FDE 范围变成 function seed 的 range 来源。
文本 report 会打印统计，但现在没有单独 JSON 查询，排查入口发现和函数 range 时还要从 report 里读文本。

这次补：

```text
notdec-native-discover --eh-frame-json <elf-file>
```

## Ghidra 相关实现

Ghidra 里 `.eh_frame` 信息通常由 DWARF / exception frame 相关 analyzer 读入 Program：

- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/DWARFAnalyzer.java`
  - analyzer 入口会接收 `MessageLog`，解析 DWARF 相关信息，并把结果写入 Program。
- `Ghidra/Features/Base/src/main/java/ghidra/app/util/bin/format/dwarf/DWARFProgram.java`
  - 承接 DWARF 数据和 Program 的关系。
- `Ghidra/Features/Base/src/main/java/ghidra/app/util/bin/format/dwarf/sectionprovider/DWARFSectionProvider.java`
  - 按 section 名字提供 DWARF / frame 相关 section 数据。

Ghidra 的最终目标是把分析结果写到 ProgramDB。native 当前没有 ProgramDB，所以先保守暴露 `.eh_frame` 读到的事实和统计。

## native 侧复刻策略

1. `notdec-native-discover` 增加 `--eh-frame-json <elf-file>`。
2. 读取 `NativeProgramState::ehFrameStats()`。
3. 输出：
   - section 是否存在、hdr 是否已解析。
   - CIE / FDE / parsed FDE / seed / invalid / unsupported 计数。
   - `.eh_frame` FDE 起点和 FDE 地址。
   - `.eh_frame_hdr` initial location 和 FDE 地址。
   - hdr mismatch 和 unsupported 样本。

暂时不做：

- 不改变 `.eh_frame` parser。
- 不输出 CIE augmentation 的完整内部状态。
- 不把所有 FDE 都当函数边界判断依据；这里只是查询接口。

## 判断标准

1. 三个 Bench2 目标输出合法 JSON。
2. `frame_fdes_count == len(frame_fdes)`。
3. `hdr_entries_count == len(hdr_entries)`。
4. 原有 Bench2 smoke 继续通过。

## 风险

1. 当前 parser 支持的是 Bench2 已覆盖的编码子集，不等于完整 DWARF frame dump。
2. JSON 暴露的是 native 的观察结果，不能替代后续完整函数边界判断。

## 实现记录

已完成 `notdec-native-discover --eh-frame-json <elf-file>`。

修改点：

- `tools/notdec-native-discover.cpp:19` 的 `OutputMode` 增加 `EhFrameJson`。
- `tools/notdec-native-discover.cpp:50` 的 `printUsage(...)` 增加 `--eh-frame-json`。
- `tools/notdec-native-discover.cpp:165` 的 `parseArgs(...)` 识别 `--eh-frame-json`。
- `tools/notdec-native-discover.cpp:416` 增加 `printEhFrameJson(...)`，输出 `NativeProgramState::ehFrameStats()` 的统计、FDE 列表、hdr 表和样本。
- `tools/notdec-native-discover.cpp:818` 的 `main(...)` 增加 `EhFrameJson` 分发。
- `ARCHITECTURE.md:34`、`ARCHITECTURE.md:131` 记录新的 eh-frame query。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:32` 更新 Stage 4 进度。

验证：

```bash
git diff --check
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
```

结果：通过。

```bash
notdec-native-discover --eh-frame-json <bench2-elf>
python3 -m json.tool <out>
```

专项结果：

```text
vsftpd fdes=182 hdr_entries=182 parsed_hdr=True added_seeds=181
libuv fdes=480 hdr_entries=480 parsed_hdr=True added_seeds=173
memcached fdes=254 hdr_entries=254 parsed_hdr=True added_seeds=253
```

三项目 JSON 合法，`frame_fdes_count == len(frame_fdes)`，`hdr_entries_count == len(hdr_entries)`。

完整 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-63
```

结果：

```text
vsftpd elapsed=11s confirmed=9 blocks=30 inst=80 xrefs=304 unresolved=0
libuv elapsed=21s confirmed=9 blocks=29 inst=85 xrefs=26 unresolved=0
memcached elapsed=11s confirmed=9 blocks=30 inst=80 xrefs=186 unresolved=0
```

性能：新增接口只格式化已有 `NativeEhFrameStats`，不改变 `.eh_frame` 解析和 smoke 主流程。smoke 耗时仍在当前基线范围。

评分：

- 实现效果：8/10。`.eh_frame` seed 来源和 hdr/frame 对齐情况现在可以机器查询。
- 复杂度：8/10。formatter 字段较多，但没有改 parser。
- 维护成本：8/10。字段直接来自 `NativeEhFrameStats`，后续 parser 扩展时可继续补 JSON 字段。
