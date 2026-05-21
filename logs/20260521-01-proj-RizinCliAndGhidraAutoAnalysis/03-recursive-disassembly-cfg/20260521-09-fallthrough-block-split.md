# 本次用户原始 prompt

继续按照规划完善 bin2llvm native 链路。当前已能从 Sleigh P-Code 识别直接 `CALL` / `BRANCH` / `CBRANCH` 并写入 xref；下一小步把已解码前缀按控制流指令切成多个保守 basic block，并给条件跳转补 fallthrough successor。

## 背景

Ghidra 侧相关实现：

- `CreateFunctionCmd.java::getFunctionBody(...)` 用 `FollowFlow` 从函数入口跟随 flow，计算函数 body。
- `FollowFlow.java` 会按 `FlowType` 决定是否跟随 computed call、conditional call、unconditional call、computed jump、conditional jump、unconditional jump、indirection。
- `RefTypeFactory.java::getDefaultFlowType(...)` 从 instruction 的 P-Code 里识别 `CBRANCH`、`BRANCH`、`CALL`，区分 jump/call。
- `RefType.java` 定义了 `FALL_THROUGH`、`CONDITIONAL_JUMP`、`UNCONDITIONAL_JUMP`、`TERMINATOR` 等控制流类型。

Ghidra 的做法不是把整段线性反汇编当一个 block，而是以控制流边界切分，再把 fallthrough 和跳转目标交给后续 flow/body 计算。

## 目标

本小步只处理已解码前缀内的 block 切分：

1. 遇到 `CBRANCH`，当前 block 结束，successor 包含直接跳转目标和下一条指令的 fallthrough。
2. 遇到 `BRANCH`，当前 block 结束，successor 只包含直接跳转目标。
3. 遇到 `RETURN` 或 `BRANCHIND`，当前 block 结束，不加 successor。
4. `CALL` / `CALLIND` 不切 block，函数内继续 fallthrough。
5. 仍不递归解码新目标，不处理间接目标解析。

## 技术路线

- 在 `SleighSeedInstructionAnalyzer` 里先按 instruction address 汇总 P-Code 控制流信息。
- 直接 xref 仍按上一小步写入。
- 用 `SleighInstructionSummary` 的地址和长度顺序生成 `NativeBasicBlock` 列表。
- `NativeFunction::Blocks` 改为多个 block，而不是单个 block。

当前仍只针对前 8 个 seed、每个 seed 最多 8 条指令。

## 风险

- 线性解码后面的 block 可能并不真的可达，尤其是 unconditional branch 后面的顺序指令。
- 没有递归 worklist，所以 successor 可能指向尚未解码的地址。
- 这只是 CFG 骨架，不代表函数完整语义。

## 判断标准

- Bench2 三个 smoke 能跑通。
- basic block 数量应大于 confirmed function 数量。
- xref 数量保持上一小步水平。
- 时间仍在当前 bounded smoke 量级。

## 实现记录

本小步已完成。

改动文件和函数：

- `lib/NativeAnalysis.cpp:1176` 新增 `DecodedFlowInfo`，按 instruction address 记录直接 branch 目标、条件跳转、无条件跳转、间接跳转和 return。
- `lib/NativeAnalysis.cpp:1241` 在 `SleighSeedInstructionAnalyzer::decodeSeed(...)` 中收集 `flowInfos`，再交给 block 构造逻辑。
- `lib/NativeAnalysis.cpp:1249` 将原来的单 block 创建改成 `addDecodedFunctionBlocks(...)`。
- `lib/NativeAnalysis.cpp:1273` 调整 `addDirectControlFlow(...)`，既写直接 xref，也返回每条指令的控制流信息。
- `lib/NativeAnalysis.cpp:1336` 新增 `buildDecodedBlocks(...)`，遇到 `CBRANCH` / `BRANCH` / `BRANCHIND` / `RETURN` 时结束当前 block。
- `lib/NativeAnalysis.cpp:1359` 对 `CBRANCH` 同时加入直接跳转目标和下一条指令作为 fallthrough successor。
- `lib/NativeAnalysis.cpp:1365` 对 `BRANCH` 只加入直接跳转目标，不加 fallthrough。
- `lib/NativeAnalysis.cpp:1391` 新增 `addUniqueAddress(...)`，避免 successor 重复。

没有做的事：

- 没有递归解码 successor。
- 没有解析 `BRANCHIND` / `CALLIND` 的真实目标。
- 没有按 branch target 反向切开已经开始的 block；当前只按线性前缀里的控制流指令切分。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
```

验证结果：

- `vsftpd`：confirmed functions 8，basic blocks 18，instructions 55，xrefs 5，时间 1.63s。
- `libuv.so.1.0.0`：confirmed functions 8，basic blocks 16，instructions 60，xrefs 7，时间 1.68s。
- `memcached`：confirmed functions 8，basic blocks 18，instructions 55，xrefs 5，时间 1.63s。

性能说明：

- 没有增加 Sleigh 初始化次数，只在已有 P-Code 上做内存态切分。
- 三例时间仍在约 1.6s，和上一小步同量级。

评分：

- 实现效果：6/10。block 已能按控制流指令切分，并补了条件跳转 fallthrough。
- 复杂度：6/10。逻辑仍限制在 bounded analyzer 内，没有引入全局 CFG 调度。
- 维护成本：6/10。后续递归 CFG 需要进一步按目标地址和 fallthrough 建 worklist。
