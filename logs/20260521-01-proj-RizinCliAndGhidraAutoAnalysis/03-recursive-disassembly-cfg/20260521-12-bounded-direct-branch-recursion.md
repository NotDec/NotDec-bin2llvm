# 本次用户原始 prompt

继续按照规划完善 bin2llvm native 链路。上一小步已经能在同轮消费 direct call seed；下一小步开始受控消费同函数内的 direct branch successor，让 CFG 不只停在入口线性前缀。

## 背景

Ghidra 侧相关实现：

- `Ghidra/Features/Base/src/main/java/ghidra/app/cmd/function/CreateFunctionCmd.java::getFunctionBody(...)` 会用 `FollowFlow` 计算函数 body。它把 `COMPUTED_CALL`、`CONDITIONAL_CALL`、`UNCONDITIONAL_CALL`、`INDIRECTION` 放进 `dontFollow`，所以函数内会跟随 jump，但不会把 call 目标直接并进同一个函数。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/block/FollowFlow.java::getFlowAddressSet(...)` 是对外入口，会从起始地址向前收集 flow 地址。
- `FollowFlow.java::followCode(...)` 维护待处理 instruction stack，逐步消费新的 flow 地址。
- `FollowFlow.java::followInstruction(...)` 会先处理 `getFlowsFromInstruction(...)` 得到的跳转目标，再按 `hasFallthrough()` 处理 fallthrough。
- `FollowFlow.java::getFlowsFromInstruction(...)` 会按 flow type 判断当前 flow 是否应继续跟随。
- `Instruction.java::getFlows()`、`Instruction.java::getFlowType()` 和 `RefType.java` 里的 `CONDITIONAL_JUMP`、`UNCONDITIONAL_JUMP`、`COMPUTED_JUMP` 是 Ghidra 侧区分 direct / computed flow 的基础。

native 侧现在已经能从 Sleigh P-Code 识别 direct `BRANCH` / `CBRANCH`，并写入 xref 和 block successor。但这些 successor 还不会进入 decode 队列，所以函数体仍然主要是入口附近的线性前缀。

## 目标

本小步只做 direct branch 的受控递归：

1. function seed 仍从 worklist 前 8 个开始。
2. direct call target 仍作为新 function seed 进入本地队列。
3. direct `BRANCH` / `CBRANCH` 的可执行 successor 作为同一个 function entry 下的 block seed 入队。
4. 本轮总 decode 上限仍是 16 个 block/function seed。
5. 不跟随 indirect branch / indirect call。
6. 不把 direct call 目标并入当前函数。

## 技术路线

- 本地 decode 队列从单个地址改成 `{FunctionEntry, BlockAddress}`。
- 初始 function seed 入队时，`FunctionEntry == BlockAddress`。
- direct call target 入队时，也作为新的 `{target, target}`。
- direct branch successor 入队时，保持当前 `FunctionEntry`，只改变 `BlockAddress`。
- `decodeSeed(...)` 接收 function entry 和 block address。解码 block address，但把 block 写入 function entry 对应的 `NativeFunction`。
- 为了让后续 block 能写进同一个函数，`NativeProgramState::addBasicBlock(...)` 在添加 block 时扩展已确认函数的 decoded range。
- 只把当前解码范围外的 branch successor 入队，避免把已经在线性前缀里切出来的 block 重复 decode。

## 风险

- 现在的 decode 仍然是每个 block 最多 8 条 / 64 字节，可能切在 block 中间。
- 扩展 `NativeFunction.RangeStart/RangeEnd` 后，`functionContaining(...)` 仍是范围查询，可能覆盖中间空洞。这是已有设计的保守折中，后续需要 block-aware 查询时再补。
- 如果 Bench2 前 8 个 seed 的 direct branch 都落在当前前缀内，统计数字可能变化不大。

## 判断标准

- Bench2 三个 smoke 能跑通。
- `libuv.so.1.0.0` 中 `sleigh-direct-call: 1` 保持存在。
- confirmed function 不应该因为 branch target 被误当成新函数而异常增加。
- basic block / instruction / xref 数量允许小幅增加。
- 时间仍在小范围内。

## 实现记录

改动文件和函数：

- `lib/NativeAnalysis.cpp:1156`，`SleighSeedInstructionAnalyzer::run(...)`：decode 队列改为 `DecodeQueueItem`，用 `{FunctionEntry, BlockAddress}` 去重。direct call 仍以 `{target, target}` 入队，direct branch 以 `{currentFunctionEntry, target}` 入队。
- `lib/NativeAnalysis.cpp:1209`，新增 `DecodeQueueItem`：明确区分“当前函数入口”和“当前要解码的 block 地址”。
- `lib/NativeAnalysis.cpp:1244`，`enqueueInitialSeeds(...)`：初始 function seed 仍只取前 8 个，并写成 `{entry, entry}`。
- `lib/NativeAnalysis.cpp:1260`，`enqueueSeed(...)`：同时检查 function entry 和 block address 是否可执行，并按 pair 去重。
- `lib/NativeAnalysis.cpp:1274`，`decodeSeed(...)`：接收 function entry 和 block address，返回 direct call targets 和 direct branch targets。
- `lib/NativeAnalysis.cpp:1318`，`addDecodedFunctionBlocks(...)`：把当前 block 写入 function entry 对应的 `NativeFunction`；只把当前解码范围外的 successor 加入 branch target 队列。
- `lib/NativeAnalysis.cpp:1973`，`NativeProgramState::addBasicBlock(...)`：追加 block 时允许扩展 `NativeFunction.RangeStart/RangeEnd`，并跳过完全重复的 block。
- `include/notdec-bin2llvm/NativeAnalysis.h:146`：更新 `NativeFunction` 注释，说明 range 会随 direct branch block 增长。
- `ARCHITECTURE.md:41`、`ARCHITECTURE.md:55`：更新 native Sleigh decode 队列和 direct branch successor 行为。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:18`：记录本小步已完成。

实现效果：

- direct branch target 不再被当作新函数入口，而是在同一个 function entry 下作为 block address 解码。
- direct call target 仍然是新 function seed，不并入当前函数。
- 因为 branch 扩展能看到更多 block，后续 block 里的 direct call 也会被发现，所以 `sleigh-direct-call` 数量可能增加。
- indirect branch / call 仍然只记录当前已知事实，不跟随。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
```

验证结果：

- build：通过。
- `vsftpd`：function seeds 187，`sleigh-direct-call: 1`，confirmed functions 9，basic blocks 31，instructions 80，xrefs 9，TIME 2.82。
- `libuv.so.1.0.0`：function seeds 488，`sleigh-direct-call: 4`，confirmed functions 11，basic blocks 35，instructions 107，xrefs 17，TIME 3.17。
- `memcached`：function seeds 259，`sleigh-direct-call: 1`，confirmed functions 9，basic blocks 31，instructions 80，xrefs 9，TIME 2.80。

性能和效果判断：

- 相比上一轮，三个样例 instruction / block / xref 都增加，说明 direct branch successor 已被消费。
- `libuv.so.1.0.0` 的 `sleigh-direct-call` 从 1 增到 4，是因为新 block 内发现了更多 direct call，不是 branch target 被误当成函数。
- 时间从约 1.6-1.9s 增到约 2.8-3.2s，仍在 smoke 可接受范围内；后续扩大上限前需要先补 stop rule 和更细统计。

评分：

- 实现效果：8/10。已经开始同函数 direct branch 递归，但仍是小上限。
- 理解成本：7/10。队列 pair 比单地址复杂一点，但语义更接近 Ghidra 的 function body 跟随。
- 维护成本：7/10。`RangeStart/RangeEnd` 会覆盖 block 间空洞，后续如果要严格函数包含关系，需要改成 block-aware 查询。

更好的方案：

- 后续应把 function body 变成显式 block set 查询，而不是只靠 `RangeStart/RangeEnd` 判断包含。
- 还需要按 Ghidra `dontFollow` 语义继续补 computed flow、call terminator、函数边界和已知函数交叉处理。
