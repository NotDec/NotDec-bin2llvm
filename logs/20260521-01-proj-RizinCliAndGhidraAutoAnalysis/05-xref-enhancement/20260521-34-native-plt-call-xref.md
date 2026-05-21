# Native PLT Call XRef

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

native 侧已经能通过 `RelocationPltAnalyzer` 建出 `NativePltEntry`，上一小步也补了
`notdec-native-discover --plt-json`。但 `SleighSeedInstructionAnalyzer` 识别 direct `CALL`
时还只看目标是否可执行：只要可执行，就记录普通 call xref，并把目标作为内部 function seed。

这对 PLT stub 不够对。PLT stub 是外部符号入口，不应该被当作普通内部函数递归 decode。

这次只补 direct `CALL` 命中 PLT stub 的分类：

- xref 仍是 `NativeXrefKind::Call`。
- source 改成 `sleigh-pcode-plt-call`。
- 不把 PLT stub 加进 direct call seed 队列。

## Ghidra / rizin 相关实现

Ghidra 侧：

- `ghidra_scripts/ExportHeritageModule.java::directCallTargetName(...)`
  - direct call 目标地址能找到 external function 时，会通过 `rememberExternal(...)` 记录外部函数。
- `ghidra_scripts/ExportHeritageModule.java::writeExternals(...)`
  - 把外部函数表写到模块 JSON。
- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/FunctionManager.java`
  - `getExternalFunctions()` / `getFunctionAt(...)` 是 external function 查询的基础。

rizin 侧通常把 imports / relocations / PLT 信息作为单独表输出，callgraph 里会区分内部函数和导入符号。
native 这次先只做最小边界：PLT stub 不再进入内部函数递归。

## native 侧复刻策略

1. 在 `SleighSeedInstructionAnalyzer::addDirectControlFlow(...)` 里，对 direct `CALL` target 先查
   `NativeProgramState::lookupPltExternal(...)`。
2. 如果命中 PLT：
   - 写 call xref，source 为 `sleigh-pcode-plt-call`。
   - 不加入 `DirectControlFlowResult::CallTargets`。
3. 如果不命中 PLT，保持现有行为：source 为 `sleigh-pcode-direct-flow`，并加入 call target seed。

暂时不做：

- 不把外部符号名写进 `NativeXref`，当前 xref 结构没有这个字段。
- 不改 lowering。
- 不处理 indirect call 经 GOT 跳转的情况。

## 判断标准

1. direct call 到 PLT stub 不再产生内部 function seed。
2. Bench2 smoke 继续通过。
3. `--plt-json` 继续能输出三目标 PLT 映射。

## 风险

1. 当前 Bench2 小上限下可能没有 direct call 到 PLT 的样本，因此 summary 可能无变化。
2. `lookupPltExternal(...)` 依赖已有 PLT stub 匹配，没匹配到的目标仍按普通 direct call 处理。

## 实现记录

### 修改文件和函数

1. `lib/NativeAnalysis.cpp:1451`
   - 修改 `SleighSeedInstructionAnalyzer::addDirectControlFlow(...)`。
   - direct `CALL` target 是 executable address 时，先调用 `NativeProgramState::lookupPltExternal(...)`。
   - 如果命中 PLT stub，写 `NativeXrefKind::Call`，source 为 `sleigh-pcode-plt-call`，并跳过 `CallTargets` 入队。
   - 如果没有命中 PLT，保持原 `sleigh-pcode-direct-flow` 和 direct call seed 行为。
2. `ARCHITECTURE.md:55`
   - 记录 direct `CALL` 命中 PLT stub 的处理。
3. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:32`
   - 更新 Stage 5 完成项。

### 验证命令

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm notdec-native-discover -j2
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-json /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-34
```

### Bench2 结果

`libuv.so.1.0.0` 出现一条 PLT call xref：

```text
"kind": "call"
"source": "sleigh-pcode-plt-call"
```

这条 call 不再作为 `sleigh-direct-call` seed 继续递归 decode。

三个目标都通过 `notdec-native-discover --summary-json`、`notdec-native-llvm --all-confirmed`、
LLVM 22 `llvm-as`、LLVM 22 `opt -passes=verify`。

```text
vsftpd ok elapsed=8s
libuv ok elapsed=8s
memcached ok elapsed=7s
```

summary 关键数据：

```text
vsftpd: confirmed_functions=9, basic_blocks=30, instructions=80, xrefs.total=297, data=149, string=139
libuv: confirmed_functions=9, basic_blocks=29, instructions=85, xrefs.total=24, data=13, string=0
memcached: confirmed_functions=9, basic_blocks=30, instructions=80, xrefs.total=179, data=68, string=102
```

和上一轮同口径对比：

```text
libuv: function_seeds 486 -> 485
libuv: sleigh-direct-call 2 -> 1
libuv: confirmed_functions 10 -> 9
libuv: basic_blocks 32 -> 29
libuv: instructions 93 -> 85
libuv: unresolved indirect flows 5 -> 2
vsftpd / memcached: summary 无变化
```

所有 `.stdout` / `.stderr` 日志大小都是 0。

### 性能和影响

这次只增加一次 PLT 映射查询。Bench2 三目标 smoke 总耗时约 23 秒，和上一轮同口径接近。

实现效果：5/5。实际挡住了 `libuv` 里一个 PLT stub 被当作内部函数递归 decode。
复杂度：1/5。只改 direct call 分支。
维护成本：2/5。后续如果 `NativeXref` 增加 symbol 字段，可以把外部符号名带出来。
