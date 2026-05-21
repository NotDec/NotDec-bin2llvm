# 本次用户原始 prompt

继续按照规划完善 bin2llvm native 链路。当前已经能从 worklist 解码少量指令，并把已解码 seed 保守写成 confirmed function / 单 basic block；下一小步从 Sleigh P-Code 里识别直接控制流，写入 xref，并给保守 block 添加直接 successor。

## 背景

Ghidra 侧相关实现：

- `CreateFunctionCmd.java::getFunctionBody(...)` 创建函数 body 时使用 `FollowFlow`，并明确不跟随 call、indirection 等 flow。
- `FollowFlow.java` 按 `FlowType` 决定哪些边要跟随，支持 computed/unconditional/conditional call 和 jump。
- `RefTypeFactory.java::getDefaultFlowType(...)` 会读取 instruction 的 P-Code；对 `PcodeOp.CBRANCH` / `PcodeOp.BRANCH` / `PcodeOp.CALL`，如果输入地址等于目标地址，就给出对应 flow type。
- `ReferenceManager.java::addMemoryReference(...)` 是 Ghidra 写入代码引用的接口。

这里的关键点是：Ghidra 会从 instruction/P-Code 里拿直接控制流目标，再分类成 jump/call reference。computed flow 会单独处理，不能把寄存器/内存间接目标假装成直接地址。

## 目标

本小步只识别直接目标：

1. 从本次已解码 seed 前缀的 P-Code 中找 `CALL`、`BRANCH`、`CBRANCH`。
2. 只接受第一个输入是 `ram` 地址的目标。
3. `CALL` 写 `NativeXrefKind::Call`。
4. `BRANCH` / `CBRANCH` 写 `NativeXrefKind::Flow`，并作为当前 block successor。
5. 忽略 `CALLIND`、`BRANCHIND`、`RETURN`，只保留后续扩展空间。

## 技术路线

- 在 `SleighSeedInstructionAnalyzer::decodeSeed(...)` 里，复用同一个 seed/range，再调用一次 `collectSleighPcode(...)` 获取 P-Code。
- 解析 `PcodeProgram::Ops`，只看 `Opcode` 和 `Inputs[0]`。
- 写入 `NativeXref` 前确认目标是可执行地址。
- `addDecodedFunctionBlock(...)` 接受 successors，把它们写入 `NativeBasicBlock::Successors`。

当前为了敏捷验证，仍只处理前 8 个 seed、每个 seed 8 条 / 64 字节。这样 Bench2 smoke 成本可控。

## 风险

- P-Code 地址是指令地址，多个 P-Code op 会共享同一条指令地址，需要避免重复 xref。
- x86 PLT 前缀里常见 `BRANCHIND`，本小步会忽略，所以 xref 数可能不多。
- 这仍不是完整 CFG：没有 fallthrough 分块，没有递归解码 successor。

## 判断标准

- Bench2 三个 smoke 能跑通。
- xref total 在至少一个样例中大于 0。
- confirmed function、basic block、instruction 数仍保持正常。
- 时间仍在当前 bounded smoke 量级。

## 实现记录

本小步已完成。

改动文件和函数：

- `include/notdec-bin2llvm/SleighLift.h:38` 新增 `SleighInstructionDecode`，把指令摘要和同一段 P-Code 放在一个结果里。
- `include/notdec-bin2llvm/SleighLift.h:62` 新增 `collectSleighInstructionDecode(...)`。
- `lib/SleighLift.cpp:348` 保留 `collectSleighInstructionSummaries(...)`，改为复用新的 decode 接口。
- `lib/SleighLift.cpp:359` 实现 `collectSleighInstructionDecode(...)`，同一次 Sleigh 初始化里调用 `printAssembly(...)` 和 `oneInstruction(...)`。
- `lib/NativeAnalysis.cpp:1211` 在 `SleighSeedInstructionAnalyzer::decodeSeed(...)` 中改用 `collectSleighInstructionDecode(...)`，避免为 xref 再初始化一次 Sleigh。
- `lib/NativeAnalysis.cpp:1233` 收集 block successors，并传入 `addDecodedFunctionBlock(...)`。
- `lib/NativeAnalysis.cpp:1266` 新增 `addDirectControlFlow(...)`，从 P-Code 中提取直接 `CALL`、`BRANCH`、`CBRANCH`。
- `lib/NativeAnalysis.cpp:1293` 新增 `directRamTarget(...)`，只接受第一个输入为 `ram` 的直接目标。
- `lib/NativeAnalysis.cpp:1308` 新增 `addUniqueXref(...)`，避免同一 seed 前缀内重复写 xref。

没有做的事：

- 没有处理 `CALLIND`、`BRANCHIND`。
- 没有创建新 seed 或递归解码 successor。
- 没有拆分 fallthrough block。
- 没有把 return 记录到状态里。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-pcode -j2
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
```

验证结果：

- `vsftpd`：confirmed functions 8，basic blocks 8，instructions 55，xrefs 5，flow 5，call 0，时间 1.67s。
- `libuv.so.1.0.0`：confirmed functions 8，basic blocks 8，instructions 60，xrefs 7，flow 6，call 1，时间 1.66s。
- `memcached`：confirmed functions 8，basic blocks 8，instructions 55，xrefs 5，flow 5，call 0，时间 1.64s。

性能说明：

- 初版实现为每个 seed 额外调用一次 `collectSleighPcode(...)`，三例时间约 3.2s。
- 已改成 `collectSleighInstructionDecode(...)`，同一次 Sleigh 初始化里收集 instruction 和 P-Code，三例回到约 1.6s。

评分：

- 实现效果：6/10。直接控制流 xref 已进入状态，但还没有递归 CFG。
- 复杂度：6/10。新增一个复合 decode 结果，避免了明显重复初始化。
- 维护成本：6/10。后续要补间接流、fallthrough block 和递归 worklist。
