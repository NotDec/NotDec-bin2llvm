# Native Callgraph JSON

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

native discovery 已经有 confirmed function、call xref、PLT/GOT 外部符号映射。
现在能用 `--xrefs-kind-json call` 看每条 call，但还没有像 rizin `agc` / `agC` 那样直接看 callgraph。

这次补：

```text
notdec-native-discover --callgraph-json <elf-file>
```

## Ghidra 相关实现

Ghidra 的 callgraph 不是单独重新反汇编出来的，而是从 Program 里的函数和引用关系派生：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/FunctionManager.java`
  - 管理函数入口和函数对象。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java`
  - 管理从指令到目标地址的引用。
- `Ghidra/Features/GraphFunctionCalls/src/main/java/ghidra/app/plugin/core/calltree/CallTreePlugin.java`
  - 基于函数和 call reference 展示调用关系。

rizin 侧对应的是 `agc` / `agC` 这类 graph 命令，底层消费 `RzAnalysisFunction` 和 `RzAnalysisXRef`。

## native 侧复刻策略

1. `notdec-native-discover` 增加 `--callgraph-json <elf-file>`。
2. 遍历 `NativeProgramState::xrefs()` 中 `NativeXrefKind::Call`。
3. `from` 用 `functionContaining(...)` 找 caller function。
4. `to` 优先用 `functionAt(...)` 判断 internal callee。
5. 如果 `to` 命中 PLT stub 或 GOT slot，则标为 external，并输出 symbol。
6. 其他目标标为 unknown，不猜测。

暂时不做：

- 不输出 DOT。
- 不把 callgraph 反向驱动函数发现。
- 不合并重复边；当前先保留 callsite 粒度，方便排查每条调用来源。

## 判断标准

1. 三个 Bench2 目标输出合法 JSON。
2. `count == len(edges)`。
3. `count` 与 `--xrefs-kind-json call` 的 `count` 一致。
4. 原有 Bench2 smoke 继续通过。

## 风险

1. 当前 callgraph 只反映 native 已确认函数和已记录 call xref，不代表完整程序调用图。
2. caller 只按 confirmed basic block 判断，函数边界不完整时会出现 `caller_found=false`。

## 实现记录

已完成 `notdec-native-discover --callgraph-json <elf-file>`。

修改点：

- `tools/notdec-native-discover.cpp:19` 的 `OutputMode` 增加 `CallgraphJson`。
- `tools/notdec-native-discover.cpp:51` 的 `printUsage(...)` 增加 `--callgraph-json`。
- `tools/notdec-native-discover.cpp:167` 的 `parseArgs(...)` 识别 `--callgraph-json`。
- `tools/notdec-native-discover.cpp:590` 增加 `lookupExternalCallTarget(...)`，按 PLT stub、PLT GOT slot、external relocation 找外部符号。
- `tools/notdec-native-discover.cpp:611` 增加 `printCallgraphJson(...)`，遍历 call xref，输出 callsite、caller、callee kind 和名字。
- `tools/notdec-native-discover.cpp:902` 的 `main(...)` 增加 `CallgraphJson` 分发。
- `ARCHITECTURE.md:34`、`ARCHITECTURE.md:132` 记录新的 callgraph query。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:32` 更新 Stage 4 进度。

验证：

```bash
git diff --check
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
```

结果：通过。

专项检查：

```bash
notdec-native-discover --callgraph-json <bench2-elf>
notdec-native-discover --xrefs-kind-json call <bench2-elf>
```

结果：

```text
vsftpd edges=2 internal=1 external=1 unknown=0
libuv edges=3 internal=1 external=2 unknown=0
memcached edges=2 internal=1 external=1 unknown=0
```

三项目 JSON 合法，`count == len(edges)`，且 callgraph `count` 与 call xref `count` 一致。

完整 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-64
```

结果：

```text
vsftpd elapsed=10s confirmed=9 blocks=30 inst=80 xrefs=304 unresolved=0
libuv elapsed=22s confirmed=9 blocks=29 inst=85 xrefs=26 unresolved=0
memcached elapsed=10s confirmed=9 blocks=30 inst=80 xrefs=186 unresolved=0
```

性能：新增接口只格式化已有 function、xref、PLT 和 relocation state，不改变 native 分析流程。smoke 耗时仍在当前基线范围。

评分：

- 实现效果：8/10。现在可以直接看 callsite 粒度调用图，内部/外部调用都有明确分类。
- 复杂度：8/10。逻辑集中在 CLI formatter，但 external 目标需要同时查 PLT 和 relocation。
- 维护成本：8/10。后续如果新增 call xref 来源，只要它写入已有 xref / relocation / PLT state，callgraph 会自动消费。
