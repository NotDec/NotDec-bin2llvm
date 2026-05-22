# Native CFG DOT

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

native CLI 已经有 `--cfg-json <entry>`，能按 confirmed function 入口输出 block 和 successor。
原始规划里 graph 命令希望对齐 rizin `agf/agF`，第一版建议 JSON 和 DOT。

这次补：

```text
notdec-native-discover --cfg-dot <entry> <elf-file>
```

## Ghidra 相关实现

Ghidra 的函数图展示来自函数、basic block 和 flow reference：

- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/functiongraph/FunctionGraphPlugin.java`
  - 单函数图视图入口。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/functiongraph/graph/FunctionGraph.java`
  - 管理函数图里的 vertex / edge。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/block/BasicBlockModel.java`
  - 根据指令和 flow reference 切出 basic block。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/block/CodeBlockReference.java`
  - 表达 block 间边。

rizin 对应的是 `agf` / `agF`，底层从 `RzAnalysisFunction` 和 `RzAnalysisBlock` 输出图。

## native 侧复刻策略

1. `notdec-native-discover` 增加 `--cfg-dot <entry> <elf-file>`。
2. 复用 `NativeProgramState::functionAt(entry)` 精确查 confirmed function。
3. 每个 block 输出一个 DOT node，label 包含 start/end/size。
4. 每个 successor 输出一条 DOT edge。

暂时不做：

- 不做图片渲染。
- 不改变 CFG 恢复。
- 不按地址猜所属函数。
- 不给 unknown successor 补节点属性，只输出边。

## 判断标准

1. `notdec-native-discover` 能编译。
2. 三个 Bench2 目标任选 confirmed function entry，输出以 `digraph` 开头的 DOT。
3. 不存在 entry 时仍输出合法 DOT，并标记 `found=false`。
4. 原有 Bench2 smoke 继续通过。

## 风险

1. DOT 只反映当前 confirmed block，不代表完整函数图。
2. successor 可能指向尚未解码 block，这是当前 bounded decode 的真实状态。

## 实现记录

### 修改范围

1. `tools/notdec-native-discover.cpp`
   - 第 19 行附近：`OutputMode` 增加 `CfgDot`。
   - 第 53 行附近：usage 增加 `--cfg-dot <entry> <elf-file>`。
   - 第 138 行附近：`parseArgs(...)` 接受 `--cfg-dot`，复用 `QueryFunctionEntry`。
   - 第 262 行附近：新增 `dotEscape(...)`。
   - 第 668 行附近：新增 `printCfgDot(...)`，按 confirmed function 输出 Graphviz DOT。
   - 第 1035 行附近：`main(...)` 分发到 `printCfgDot(...)`。
2. `ARCHITECTURE.md`
   - 第 42 行附近：CLI query 说明加入 `--cfg-dot <entry>`。
   - 第 137 行附近：工具说明加入 Graphviz DOT 输出。
3. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
   - 阶段 4 记录 `--cfg-dot` 已完成。

### 行为

`notdec-native-discover --cfg-dot <entry> <elf-file>` 只查精确 confirmed function entry。
找到函数时输出 DOT node 和 successor edge。
找不到时仍输出合法 `digraph`，包含 `found=false query=...` 节点。

这次没有改变 CFG 恢复、函数发现或 lowering。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
git diff --check
```

结果：通过。

Bench2 专项检查：

```text
vsftpd --cfg-dot 0x5000: ok, 3 edges
libuv --cfg-dot 0x8000: ok, 3 edges
memcached --cfg-dot 0x5000: ok, 3 edges
missing entry 0x1: found=false query=0x1
```

完整 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260522-68
```

结果：

```text
vsftpd ok elapsed=13s
libuv ok elapsed=23s
memcached ok elapsed=12s
```

性能和规模：

```text
target	elapsed_seconds	confirmed_functions	basic_blocks	instructions	xrefs_total	xrefs_flow	xrefs_call	xrefs_data	xrefs_string	unresolved_total	unresolved_indirect_call	unresolved_indirect_branch
vsftpd	13	9	28	75	303	13	2	149	139	0	0	0
libuv	23	9	26	80	23	9	3	11	0	0	0	0
memcached	12	9	28	75	179	13	2	62	102	0	0	0
```

这次只是新增输出模式，不改变分析路径；Bench2 指标没有语义变化，耗时没有明显退化。

### 评分

- 实现效果：7/10。补上单函数 CFG 的 DOT 输出，对齐 rizin graph 命令形态。
- 复杂度：2/10。复用已有 CFG state，只新增 formatter 和参数分发。
- 维护成本：2/10。后续 CFG 字段扩展时同步 DOT formatter 即可。
