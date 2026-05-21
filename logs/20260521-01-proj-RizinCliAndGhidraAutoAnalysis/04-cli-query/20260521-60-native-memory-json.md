# Native Memory JSON

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

native loader 已经把 ELF `PT_LOAD` segment 放进 `NativeMemoryRange`，也把有地址的 section 放进 `NativeSectionInfo`。
后续判断 executable address、direct RAM data/string、PLT/GOT 和函数边界都依赖这些事实。

现在缺一个直接查询接口。排查 Bench2 时只能从 summary 和 xref 侧反推内存布局，不够直接。

这次补：

```text
notdec-native-discover --memory-json <elf-file>
```

## Ghidra 相关实现

Ghidra 的 Program 先有内存块和地址空间，再在其上做函数、xref、数据引用分析：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/mem/Memory.java`
  - 提供 `getBlocks()`、`getBlock(...)` 等查询。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/mem/MemoryBlock.java`
  - 表达 block 的 start、size、read/write/execute 权限。
- `Ghidra/Features/Base/src/main/java/ghidra/app/util/opinion/ElfProgramBuilder.java`
  - ELF loader 把 segment / section 加载成 Program memory blocks，并建立后续分析需要的布局。

native 侧不复刻 ProgramDB，只把当前已有的 segment range 和 section info 用 JSON 暴露出来。

## native 侧复刻策略

1. `notdec-native-discover` 增加 `--memory-json <elf-file>`。
2. 输出 `pointer_size`。
3. 输出 `ranges[]`：
   - `start`
   - `end`
   - `size`
   - `loaded_size`
   - `readable`
   - `writable`
   - `executable`
4. 输出 `sections[]`：
   - `name`
   - `address`
   - `end`
   - `size`
   - `executable`
5. 不输出 segment 原始类型、section flags 全量枚举，避免变成 ELF dump 工具。

## 判断标准

1. 三个 Bench2 目标输出合法 JSON。
2. `ranges_count`、`sections_count` 大于 0。
3. 三个目标至少有 executable range 和 executable section。
4. 原有 Bench2 smoke 继续通过。

## 风险

1. `loaded_size` 可能小于 `size`，这是 ELF file content 和 virtual memory 的正常差异。
2. 这是查询接口，不改变 loader、discovery、lowering 行为。

## 实现记录

已完成 `notdec-native-discover --memory-json <elf-file>`。

修改点：

- `tools/notdec-native-discover.cpp:19` 的 `OutputMode` 增加 `MemoryJson`。
- `tools/notdec-native-discover.cpp:47` 的 `printUsage(...)` 增加 `--memory-json`。
- `tools/notdec-native-discover.cpp:159` 的 `parseArgs(...)` 识别 `--memory-json`。
- `tools/notdec-native-discover.cpp:309` 增加 `printMemoryJson(...)`。
  - `ranges[]` 来自 `NativeProgramState::memoryRanges()`。
  - `sections[]` 来自 `NativeProgramState::sections()`。
  - 顶层输出 `pointer_size`、`ranges_count`、`sections_count`。
- `tools/notdec-native-discover.cpp:686` 的 `main(...)` 增加 `MemoryJson` 分发。
- `ARCHITECTURE.md:34`、`ARCHITECTURE.md:128` 记录新的 memory query。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:32` 更新 Stage 4 进度。

验证：

```bash
git diff --check
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
```

结果：通过。

```bash
for target in vsftpd libuv memcached; do
  notdec-native-discover --memory-json "$elf" > "$out"
  python3 -m json.tool "$out" >/dev/null
done
```

专项结果：

```text
vsftpd ranges=4 sections=26
libuv ranges=4 sections=24
memcached ranges=4 sections=27
```

三项目 `pointer_size` 都是 8，并且都有 executable range 和 executable section。

性能：新增接口只格式化已有 `NativeProgramState` 内存和 section 数据，不改 discovery pipeline；默认 smoke 不调用它。

评分：

- 实现效果：8/10。能直接观察 native loader 看到的内存布局，方便后续判断 code/data/string/PLT/GOT。
- 复杂度：9/10。只新增一个 formatter，没有改 loader 状态。
- 维护成本：9/10。字段稳定，后续如果需要更多 ELF flags 可在同一个 JSON 对象里扩展。
