# Native Relocations JSON

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

native discovery 已经读取 ELF relocation，并用它做几件事：

- 修正 relocated pointer 读取。
- 建 PLT stub 到外部符号的映射。
- 把 relocation pointer 记成 data/string xref。
- 识别外部 `GLOB_DAT` 的 indirect call / tail jump。

但现在只能从 xref、PLT 和 IR pattern 侧间接看 relocation 效果。排查 GOT/PLT 和外部符号时，需要直接看到 relocation 表。

这次补：

```text
notdec-native-discover --relocations-json <elf-file>
```

## Ghidra 相关实现

Ghidra 的 ELF loader 会把 relocation 作为 Program 事实写入符号、内存和引用层：

- `Ghidra/Features/Base/src/main/java/ghidra/app/util/opinion/ElfProgramBuilder.java`
  - 负责 ELF 装载、符号和 relocation 处理调度。
- `Ghidra/Features/Base/src/main/java/ghidra/app/util/opinion/ElfRelocationHandler.java`
  - relocation handler 的通用接口。
- `Ghidra/Processors/x86/src/main/java/ghidra/app/util/bin/format/elf/relocation/X86_64_ElfRelocationHandler.java`
  - x86-64 relocation 的具体处理。

native 侧当前不做完整 ProgramDB 写入，只把 `NativeRelocationInfo` 中已经采集的字段导出。

## native 侧复刻策略

1. `notdec-native-discover` 增加 `--relocations-json <elf-file>`。
2. 遍历 `NativeProgramState::relocations()`。
3. 每条输出：
   - `address`
   - `type`
   - `type_name`
   - `symbol`
   - `symbol_value`
   - `addend`
   - `table`
   - `status`
   - `computed_value`，没有则为 `null`
4. 顶层输出 `relocations[]` 和 `count`。

暂时不做：

- 不输出 raw relocation encoding。
- 不改变 relocation 计算逻辑。
- 不把它接入 smoke 主流程，避免每次多跑 discovery。

## 判断标准

1. 三个 Bench2 目标输出合法 JSON。
2. `count` 大于 0。
3. 三个目标能看到外部 `X86_64_GLOB_DAT` 或 `X86_64_JUMP_SLOT` relocation。
4. 原有 Bench2 smoke 继续通过。

## 风险

1. 这个 JSON 暴露的是 native 当前已理解的 relocation 子集，不等于完整 ELF dump。
2. `computed_value` 只对当前实现能计算的 relocation 输出。

## 实现记录

已完成 `notdec-native-discover --relocations-json <elf-file>`。

修改点：

- `tools/notdec-native-discover.cpp:19` 的 `OutputMode` 增加 `RelocationsJson`。
- `tools/notdec-native-discover.cpp:48` 的 `printUsage(...)` 增加 `--relocations-json`。
- `tools/notdec-native-discover.cpp:161` 的 `parseArgs(...)` 识别 `--relocations-json`。
- `tools/notdec-native-discover.cpp:361` 增加 `printRelocationsJson(...)`，遍历 `NativeProgramState::relocations()`。
- `tools/notdec-native-discover.cpp:731` 的 `main(...)` 增加 `RelocationsJson` 分发。
- `ARCHITECTURE.md:34`、`ARCHITECTURE.md:128` 记录新的 relocation query。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:32` 更新 Stage 4 进度。

验证：

```bash
git diff --check
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
```

结果：通过。

```bash
for target in vsftpd libuv memcached; do
  notdec-native-discover --relocations-json "$elf" > "$out"
  python3 -m json.tool "$out" >/dev/null
done
```

专项结果：

```text
vsftpd relocations=472 external=191 types=3
libuv relocations=232 external=224 types=4
memcached relocations=345 external=188 types=3
```

三项目都能看到外部 `X86_64_GLOB_DAT` 或 `X86_64_JUMP_SLOT` relocation。

性能：新增接口只格式化已有 relocation state，不改变 relocation 分析和 smoke 主流程。

评分：

- 实现效果：8/10。GOT/PLT、external indirect flow 和 relocation pointer xref 的来源可以直接查询。
- 复杂度：9/10。只新增一个 formatter，没有改 relocation 计算。
- 维护成本：9/10。字段直接来自 `NativeRelocationInfo`，后续扩展集中在一个 JSON 输出函数。
