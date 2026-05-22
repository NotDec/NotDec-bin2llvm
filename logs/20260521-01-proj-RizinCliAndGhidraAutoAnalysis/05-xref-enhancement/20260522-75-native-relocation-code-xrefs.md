# Native Relocation Code XRefs

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

`RelocationPltAnalyzer` 已经会把已应用的本地 relocation 写成 xref：

- 指向只读 C 字符串：`string` / `elf-relocation-string`
- 其他已加载地址：`data` / `elf-relocation-pointer`

但 init/fini array、函数指针表这类 relocation slot 会指向 executable address。现在它们也被归为
`data`，不利于后续 callgraph、entry 解释和 code reference 查询。

本次补：

- target 落在 executable memory 时，记 `NativeXrefKind::Flow`
- source 为 `elf-relocation-code`

## Ghidra 相关实现

Ghidra relocation 和引用类型来自 loader 与 reference manager：

- `Ghidra/Features/Base/src/main/java/ghidra/app/util/bin/format/elf/ElfHeader.java`
  - `parseRelocationTables(...)` 解析 ELF relocation。
- `Ghidra/Processors/x86/src/main/java/ghidra/app/util/bin/format/elf/relocation/X86_64_ElfRelocationHandler.java`
  - `relocate(...)` 计算 relocation 结果。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java`
  - `addMemoryReference(...)` 记录 from/to 引用。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/RefType.java`
  - 区分 data、computed call/jump、flow 等引用类型。

rizin 侧同类能力是 `axc` / `axC` 这类 code/call xref，以及 `axf` / `axt` 查询。

## native 侧复刻策略

1. 在 `RelocationPltAnalyzer::addRelocationPointerXrefs(...)` 里先判断 target 是否 executable。
2. executable target 记 `NativeXrefKind::Flow` 和 `elf-relocation-code`。
3. 只读 C 字符串仍记 `String`。
4. 其他 mapped target 仍记 `Data`。
5. 不把这类引用直接当 `Call`，因为 relocation slot 本身不是 callsite，只是 data-to-code pointer。

暂时不做：

- 不新增 xref kind。
- 不从这些 relocation code xref 自动新增 function seed；init/fini array 已由 entry discovery 处理。
- 不猜 jump table 和普通函数指针调用关系。

## 判断标准

1. `notdec-native-discover` 能构建。
2. Bench2 三个目标至少有 `elf-relocation-code` xref。
3. 旧的 string/data relocation xref 仍存在。
4. 完整 Bench2 smoke 继续通过。
5. xref 统计变化可解释：flow 增加，data 相应减少或保持。

## 风险

1. 这会改变 `xrefs.flow` / `xrefs.data` 指标，但总数应基本不变。
2. data-to-code pointer 不是控制流执行边，后续图查询要看 source，不能把它误当 direct branch。

## 实现记录

### 修改范围

1. `lib/NativeAnalysis.cpp`
   - 第 324 行附近：更新 `RelocationPltAnalyzer::addRelocationPointerXrefs(...)`。
   - 第 330 行附近：默认仍按 `data` / `elf-relocation-pointer` 处理普通 mapped target。
   - 第 332 行附近：target 命中 `state.isExecutableAddress(...)` 时改为 `flow` / `elf-relocation-code`。
   - 第 335 行附近：只读 C 字符串仍保留 `string` / `elf-relocation-string`。
2. `ARCHITECTURE.md`
   - 第 89 行附近：记录 relocation pointer 现在会区分 code、string 和 data target。
3. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
   - 第 64 行附近：阶段 5 记录 executable target 的 relocation flow xref 分类。

### 行为

已应用的本地 relocation 现在按 target 分类：

- executable address：`flow`，source 为 `elf-relocation-code`
- 只读 C 字符串：`string`，source 为 `elf-relocation-string`
- 其他 mapped address：`data`，source 为 `elf-relocation-pointer`

这次没有新增 function seed，也没有把 data-to-code pointer 当成 callsite。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
git diff --check
```

结果：通过。

专项检查：

```text
vsftpd elf-relocation-code xref count: 2
libuv elf-relocation-code xref count: 3
memcached elf-relocation-code xref count: 50
vsftpd --xrefs-from-json 0x257a0: kind=flow, source=elf-relocation-code
memcached --xrefs-from-json 0x3e598: kind=string, source=elf-relocation-string
vsftpd data elf-relocation-pointer count: 140
```

完整 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260522-75
```

结果：

```text
vsftpd ok elapsed=18s
libuv ok elapsed=26s
memcached ok elapsed=14s
```

指标：

```text
target	elapsed_seconds	confirmed_functions	basic_blocks	instructions	xrefs_total	xrefs_flow	xrefs_call	xrefs_data	xrefs_string	unresolved_total	unresolved_indirect_call	unresolved_indirect_branch
vsftpd	18	13	38	126	315	19	4	153	139	0	0	0
libuv	26	11	28	91	23	12	3	8	0	0	0	0
memcached	14	11	30	86	187	63	2	20	102	0	0	0
```

对比上一轮同口径，xref 总数不变，executable relocation 从 data 移到 flow：

- vsftpd：flow 17 -> 19，data 155 -> 153
- libuv：flow 9 -> 12，data 11 -> 8
- memcached：flow 13 -> 63，data 70 -> 20

性能没有明显变化。

### 评分

- 实现效果：8/10。relocation 里的代码引用现在能和普通数据引用分开查询。
- 复杂度：2/10。只调整已有 relocation xref 分类。
- 维护成本：2/10。后续如果增加 code/call 更细 kind，可以从当前 `elf-relocation-code` source 继续细分。
