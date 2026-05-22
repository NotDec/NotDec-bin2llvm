# Native Callgraph DOT

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

native CLI 已经有 `--callgraph-json`，能从 call xref 输出 callsite、caller、callee kind 和 external symbol。
原始规划里 graph 命令要求 DOT 和 JSON；上一小块已经补了单函数 CFG DOT。

这次补：

```text
notdec-native-discover --callgraph-dot <elf-file>
```

## Ghidra 相关实现

Ghidra 的调用图也是从函数和 call reference 派生，不重新反汇编：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/FunctionManager.java`
  - 管理函数对象和入口。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java`
  - 管理 call reference。
- `Ghidra/Features/GraphFunctionCalls/src/main/java/ghidra/app/plugin/core/calltree/CallTreePlugin.java`
  - 展示调用关系。
- `Ghidra/Features/GraphFunctionCalls/src/main/java/ghidra/app/plugin/core/calltree/CallNode.java`
  - 表达调用图节点。

rizin 对应 `agc` / `agC`，底层消费 `RzAnalysisFunction` 和 `RzAnalysisXRef`。

## native 侧复刻策略

1. `notdec-native-discover` 增加 `--callgraph-dot <elf-file>`。
2. 遍历 `NativeProgramState::xrefs()` 里的 `NativeXrefKind::Call`。
3. caller 用 `functionContaining(...)`，找不到则用 callsite 地址建 unknown caller。
4. callee 优先 internal function，其次 PLT/GOT external symbol，最后 unknown target address。
5. 输出 Graphviz DOT edge，label 记录 callsite 和 xref source。

暂时不做：

- 不合并重复边，保留 callsite 粒度。
- 不把 callgraph DOT 反向驱动函数发现。
- 不做图片渲染。

## 判断标准

1. `notdec-native-discover` 能编译。
2. 三个 Bench2 目标输出以 `digraph` 开头的 DOT。
3. DOT edge 数和 `--callgraph-json` 的 `count` 一致。
4. 原有 Bench2 smoke 继续通过。

## 风险

1. 当前 callgraph 只反映已记录 call xref，不代表完整程序调用图。
2. 函数边界仍不完整时，caller 可能是 unknown callsite 节点。

## 实现记录

### 修改范围

1. `tools/notdec-native-discover.cpp`
   - 第 19 行附近：`OutputMode` 增加 `CallgraphDot`。
   - 第 54 行附近：usage 增加 `--callgraph-dot <elf-file>`。
   - 第 197 行附近：`parseArgs(...)` 识别 `--callgraph-dot`。
   - 第 789 行附近：新增 `printCallgraphDot(...)`，遍历 call xref 输出 DOT node / edge。
   - 第 1092 行附近：`main(...)` 分发到 `printCallgraphDot(...)`。
2. `ARCHITECTURE.md`
   - 第 40 行附近：CLI query 说明加入 `--callgraph-dot`。
   - 第 137 行附近：工具说明加入 callgraph DOT。
3. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
   - 阶段 4 记录 `--callgraph-dot` 已完成。

### 行为

`notdec-native-discover --callgraph-dot <elf-file>` 从已有 call xref 生成 Graphviz DOT。
caller 仍由 `functionContaining(...)` 判断。
callee 优先 confirmed internal function，其次 PLT/GOT external symbol，最后 unknown target address。
edge label 记录 callsite 和 xref source。

这次没有改变 call xref 生成、函数发现或 lowering。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
git diff --check
```

结果：通过。

Bench2 专项检查：

```text
vsftpd --callgraph-dot: ok, 2 edges
libuv --callgraph-dot: ok, 3 edges
memcached --callgraph-dot: ok, 2 edges
```

三个目标 DOT 都以 `digraph` 开头，edge 数等于 `--callgraph-json` 的 `count`。

完整 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260522-69
```

结果：

```text
vsftpd ok elapsed=12s
libuv ok elapsed=23s
memcached ok elapsed=12s
```

性能和规模：

```text
target	elapsed_seconds	confirmed_functions	basic_blocks	instructions	xrefs_total	xrefs_flow	xrefs_call	xrefs_data	xrefs_string	unresolved_total	unresolved_indirect_call	unresolved_indirect_branch
vsftpd	12	9	28	75	303	13	2	149	139	0	0	0
libuv	23	9	26	80	23	9	3	11	0	0	0	0
memcached	12	9	28	75	179	13	2	62	102	0	0	0
```

新增接口只格式化已有 state，不改变 native 分析流程；Bench2 指标没有语义变化。

### 评分

- 实现效果：7/10。补上 callgraph 的 DOT 输出，对齐 rizin graph 命令形态。
- 复杂度：2/10。复用现有 callgraph JSON 的判定逻辑。
- 维护成本：2/10。后续新增 call xref 来源时会自然进入 DOT 输出。
