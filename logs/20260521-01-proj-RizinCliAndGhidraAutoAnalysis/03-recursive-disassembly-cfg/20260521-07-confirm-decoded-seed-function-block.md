# 本次用户原始 prompt

继续按照规划完善 bin2llvm native 链路。当前已经能从 function worklist 用 Sleigh 解码少量指令并写入 `NativeInstruction`；下一小步把已经成功解码的 seed 保守落成 confirmed function 和单个 basic block，但仍不恢复 CFG 边。

## 背景

Ghidra 侧相关实现：

- `EntryPointAnalyzer.java::createFunctions(...)` 会在入口点已有 instruction 后调用 `CreateFunctionCmd`。
- `CreateFunctionCmd.java::applyTo(...)` 负责批量创建函数。
- `CreateFunctionCmd.java::createFunction(...)` 要求 entry 位置已有 code unit，然后调用 `Listing.createFunction(...)`。
- `CreateFunctionCmd.java::getFunctionBody(...)` / `fixupFunctionBody(...)` 会通过 `FollowFlow` 重新计算函数 body。
- `FunctionManager.java::createFunction(...)` 是 Program 数据库里真正创建函数的接口。

这里的核心思路是：Ghidra 不是只凭符号就确认函数，而是在 disassembly 已经产生 code unit 后，再把入口和 body 交给 function manager。body 可以后续通过 flow 修正。

## 目标

本小步只做最小函数骨架：

1. 对已经成功解码出至少一条指令的 seed，创建 `NativeFunction`。
2. 函数范围只覆盖本次解码出的连续指令，不使用 `.eh_frame` 或符号给出的更大范围。
3. 给函数加一个 `NativeBasicBlock`，范围同已解码指令范围。
4. 不加 successor，不加 xref，不声称已经恢复 CFG。

## 技术路线

- 复用 `SleighSeedInstructionAnalyzer` 已经拿到的 `SleighInstructionSummary`。
- 解码成功后取第一条地址和最后一条 `end` 作为当前保守范围。
- 从 `NativeProgramState::functionSeeds()` 里找 seed 名字，用作函数名；没有名字就留空。
- 调用 `NativeProgramState::addFunction(...)` 写入 confirmed function。
- 如果同一 entry 已存在函数，保持现状，不覆盖。

## 风险

- 这不是完整函数 body，只是已解码前缀。
- 没有分支语义，所以 block 没有 successor。
- 后续接入真实 CFG 后，需要用真实 block/range 替换这个保守骨架，不能把它当最终语义。

## 判断标准

- Bench2 三个 smoke 仍能跑通。
- report 中 `confirmed functions` 和 `basic blocks` 大于 0。
- `instructions` 仍大于 0。
- 运行时间仍保持在当前 smoke 量级。

## 实现记录

本小步已完成。

改动文件和函数：

- `lib/NativeAnalysis.cpp:1216` 在 `SleighSeedInstructionAnalyzer::decodeSeed(...)` 中记录本次解码出的连续范围。
- `lib/NativeAnalysis.cpp:1237` 新增 `addDecodedFunctionBlock(...)`，只在 entry 本身成功解码时创建 `NativeFunction`。
- `lib/NativeAnalysis.cpp:1244` 创建函数时只使用本次已解码指令范围，不扩大到 `.eh_frame` 或符号范围。
- `lib/NativeAnalysis.cpp:1249` 从 `NativeProgramState::functionSeeds()` 里复用 seed 名字。
- `lib/NativeAnalysis.cpp:1254` 给函数加一个 `NativeBasicBlock`，block 范围同已解码范围，没有 successor。

没有做的事：

- 没有恢复真实 CFG。
- 没有识别 terminator。
- 没有添加 flow/call xref。
- 没有覆盖已有函数。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
```

验证结果：

- `vsftpd`：function seeds 186，worklist 186，confirmed functions 8，basic blocks 8，instructions 55，时间 1.59s。
- `libuv.so.1.0.0`：function seeds 484，worklist 484，confirmed functions 8，basic blocks 8，instructions 60，时间 1.62s。
- `memcached`：function seeds 258，worklist 258，confirmed functions 8，basic blocks 8，instructions 55，时间 1.60s。

性能说明：

- 本次只复用已经解码出的 summary，多做一次内存态写入，没有增加 Sleigh 解码次数。
- 当前 smoke 时间仍在约 1.6s，和上一小步同量级。

评分：

- 实现效果：6/10。状态里开始有 confirmed function 和 block，但还不是完整 CFG。
- 复杂度：5/10。只在现有 analyzer 里补一个小函数，没有新增调度层。
- 维护成本：5/10。后续真实 CFG 接入时需要替换这个保守前缀范围。
