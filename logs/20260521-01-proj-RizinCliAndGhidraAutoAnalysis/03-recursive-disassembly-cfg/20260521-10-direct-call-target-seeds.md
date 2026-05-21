# 本次用户原始 prompt

继续按照规划完善 bin2llvm native 链路。当前已能从 Sleigh P-Code 识别直接 call / branch xref，并把已解码前缀切成多个 block；下一小步把直接 call 目标加入 function seed / worklist，为后续递归 decode 做准备。

## 背景

Ghidra 侧相关实现：

- `RefTypeFactory.java::getDefaultFlowType(...)` 会从 instruction P-Code 识别 `PcodeOp.CALL`，并把直接目标分类成 call flow。
- `ReferenceManager.java::addMemoryReference(...)` 是 Ghidra 写入 call reference 的入口。
- `AutoAnalysisManager.java::schedule(...)` 会调度 `CreateFunctionCmd`，对新的函数入口执行函数创建。
- `CreateFunctionCmd.java::applyTo(...)` / `createFunction(...)` 负责在入口位置已有 code unit 时创建函数。
- `OperandReferenceAnalyzer.java` 在分析 operand reference 时，也会在确认 call/jump 目标后调度函数创建。

这里的核心策略是：直接 call 目标不只是 xref；如果目标在可执行内存里，它也是后续函数恢复的入口候选。

## 目标

本小步只把直接 call 目标接到现有 seed/worklist：

1. 从已识别的直接 `CALL` P-Code 中收集可执行目标地址。
2. analyzer 当前轮先只记录这些目标，不在遍历 worklist 时立刻追加，避免迭代中修改 worklist。
3. 当前轮 decode 完成后，调用 `NativeProgramState::addFunctionSeed(...)`，source 使用 `sleigh-direct-call`。
4. 不立即递归解码新 seed；后续小步再处理 worklist 消费策略。

## 技术路线

- 在 `SleighSeedInstructionAnalyzer::run(...)` 开始时复制当前 worklist 快照。
- `decodeSeed(...)` 接受 `pendingCallSeeds`，识别 direct call 时写入该列表。
- `addDirectControlFlow(...)` 除了写 xref，也返回 direct call 目标。
- run 结束后统一把 `pendingCallSeeds` 写入 function seeds。

当前还是保持前 8 个 seed 的 bounded decode。

## 风险

- Bench2 当前前缀里 direct call 不多，新增 seed 数可能很少；重复 seed 只会补 source，不会重复进 worklist。
- 这一步只推进“发现函数入口”，不是完整递归 CFG。
- 如果未来边遍历边追加 worklist，要避免 vector 迭代失效。

## 判断标准

- Bench2 三个 smoke 能跑通。
- 至少在 `libuv.so.1.0.0` 中 report 出现 `sleigh-direct-call` source。
- xref、function、block、instruction 数不回退。
- 时间仍在当前 bounded smoke 量级。

## 实现记录

本小步已完成。

改动文件和函数：

- `lib/NativeAnalysis.cpp:1155` 在 `SleighSeedInstructionAnalyzer::run(...)` 中复制 `functionWorklist()` 快照，避免 decode 期间追加 seed 导致迭代失效。
- `lib/NativeAnalysis.cpp:1157` 新增 `pendingCallSeeds`，当前轮只收集 direct call 目标。
- `lib/NativeAnalysis.cpp:1169` decode 循环结束后统一调用 `NativeProgramState::addFunctionSeed(...)`，source 为 `sleigh-direct-call`。
- `lib/NativeAnalysis.cpp:1191` 新增 `DirectControlFlowResult`，同时返回 flow info 和 direct call targets。
- `lib/NativeAnalysis.cpp:1224` 调整 `decodeSeed(...)`，把 `pendingCallSeeds` 传入。
- `lib/NativeAnalysis.cpp:1255` 从 direct control flow 结果中取 `CallTargets`，去重后加入 pending list。
- `lib/NativeAnalysis.cpp:1290` 调整 `addDirectControlFlow(...)`，遇到可执行 direct `CALL` 时既写 call xref，也记录 call target。

没有做的事：

- 没有在同一轮立刻递归解码新增 seed。
- 没有处理 `CALLIND`。
- 没有给新 seed 推断名字或范围。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
```

验证结果：

- `vsftpd`：function seeds 186，worklist 186，xrefs 5，时间 1.62s。
- `libuv.so.1.0.0`：function seeds 485，worklist 485，`sleigh-direct-call: 1`，xrefs 7，时间 1.62s。
- `memcached`：function seeds 258，worklist 258，xrefs 5，时间 1.64s。

性能说明：

- 本次只复用已有 P-Code 的 call 目标，不增加 Sleigh 解码次数。
- 三例时间仍在约 1.6s。

评分：

- 实现效果：6/10。direct call 目标已进入 seed/worklist，但还没有自动消费新增 seed。
- 复杂度：5/10。通过 worklist 快照规避迭代失效，改动范围小。
- 维护成本：5/10。后续递归 decode 需要把 worklist 消费策略做成显式队列。
