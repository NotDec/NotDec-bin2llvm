# Known Range Decode Limit

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

native discovery 已经会从 ELF symbol size 和 `.eh_frame` FDE 记录 function seed 的保守范围：

- `NativeFunctionSeed::RangeStart`
- `NativeFunctionSeed::RangeEnd`
- `NativeProgramState::addFunctionRange(...)`

但 `SleighSeedInstructionAnalyzer::decodeSeed(...)` 当前只按 executable memory 剩余长度和固定
`MaxBytesPerSeed=64` 截断。这样在已知函数很短时，仍可能线性 decode 到下一个函数开头。

这次只做一个小修：如果当前 function entry 有已知 seed range，且当前 block address 落在这个范围内，
decode 字节数不能超过 `RangeEnd - blockAddress`。

## Ghidra 相关实现

Ghidra 会把函数 body 当成明确的地址集合，而不是只看段剩余大小：

- `/sn640/ghidra/Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/AutoAnalysisManager.java`
  - `createFunction(...)` 调度 `CreateFunctionCmd`。
- `/sn640/ghidra/Ghidra/Features/Base/src/main/java/ghidra/app/cmd/function/CreateFunctionCmd.java`
  - `getFunctionBody(...)` / `fixupFunctionBody(...)` 根据流关系计算和修正函数 body。
- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Function.java`
  - `getBody()` 返回函数地址集合。
- 多个 analyzer 会用 `function.getBody().contains(...)` 或 `getFunctionContaining(...)` 判断地址是否属于函数。

native 侧还没有完整 body set。这次先复用已有 seed range，避免最容易出现的跨已知范围线性 decode。

## native 侧复刻策略

1. 在 decode 前先按 executable memory 算可读字节。
2. 查当前 `functionEntry` 的 `NativeFunctionSeed`。
3. 如果 seed 有非空 `[RangeStart, RangeEnd)`，且 `blockAddress` 在范围内，则再截断到 `RangeEnd - blockAddress`。
4. 如果没有已知 range，保持原行为。

暂时不做：

- 不把 `NativeFunction` 改成完整 body set。
- 不处理 range 以外的 branch target 是否应该归属到同一函数。
- 不改变 call target 入队逻辑。

## 判断标准

1. 已知短函数 range 时，decode 不超过该 range。
2. 没有已知 range 的函数仍按原来的 `MaxBytesPerSeed` 和 executable memory 截断。
3. Bench2 smoke 继续通过。

## 风险

1. symbol size 或 `.eh_frame` range 错误时，可能少 decode；但它们本来就是较强的函数边界来源。
2. 当前 `RangeStart/RangeEnd` 仍不是完整 body set，只是先减少跨函数线性 decode。

## 实现记录

### 修改文件和函数

1. `lib/NativeAnalysis.cpp:1359`
   - 修改 `SleighSeedInstructionAnalyzer::decodeSeed(...)`。
   - `decodeBytes` 现在取 `MaxBytesPerSeed` 和 `boundedBytesForFunctionSeed(...)` 的较小值。
2. `lib/NativeAnalysis.cpp:1654`
   - 新增 `SleighSeedInstructionAnalyzer::boundedBytesForFunctionSeed(...)`。
   - 如果当前 `functionEntry` 的 `NativeFunctionSeed` 有非空 `[RangeStart, RangeEnd)`，并且当前 block address 落在这个范围内，就返回 `min(availableBytes, RangeEnd - blockAddress)`。
   - 没有 seed range、range 无效、block 不在 range 内时，保持旧行为。
3. `ARCHITECTURE.md:47`
   - 记录 Sleigh decode 会受已知 seed range 截断。
4. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:11`
   - 更新 Stage 3 完成项。

### 验证命令

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm notdec-native-discover -j2
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-31
diff -u /tmp/notdec-bin2llvm-bench2-smoke-20260521-30/vsftpd.summary.json /tmp/notdec-bin2llvm-bench2-smoke-20260521-31/vsftpd.summary.json
diff -u /tmp/notdec-bin2llvm-bench2-smoke-20260521-30/libuv.summary.json /tmp/notdec-bin2llvm-bench2-smoke-20260521-31/libuv.summary.json
diff -u /tmp/notdec-bin2llvm-bench2-smoke-20260521-30/memcached.summary.json /tmp/notdec-bin2llvm-bench2-smoke-20260521-31/memcached.summary.json
```

### Bench2 结果

三个目标都通过 `notdec-native-discover --summary-json`、`notdec-native-llvm --all-confirmed`、
LLVM 22 `llvm-as`、LLVM 22 `opt -passes=verify`。

```text
vsftpd ok elapsed=7s
libuv ok elapsed=8s
memcached ok elapsed=8s
```

summary 关键数据：

```text
vsftpd: confirmed_functions=9, basic_blocks=30, instructions=80, xrefs.total=297, data=149, string=139
libuv: confirmed_functions=10, basic_blocks=32, instructions=93, xrefs.total=24, data=13, string=0
memcached: confirmed_functions=9, basic_blocks=30, instructions=80, xrefs.total=179, data=68, string=102
```

和上一轮同口径对比：

```text
vsftpd: basic_blocks 31 -> 30
libuv: no summary diff
memcached: basic_blocks 31 -> 30
```

IR 里 direct call 仍然存在：

```text
vsftpd: call void @notdec_native_5ba0(), call void @notdec_native_8290()
libuv: call void @notdec_native_9d80(), call void @notdec_native_9350()
memcached: call void @notdec_native_5b80(), call void @notdec_native_b950()
```

所有 `.stdout` / `.stderr` 日志大小都是 0。

### 性能和影响

这次只增加一次 seed map 查询和一个整数截断。Bench2 三目标 smoke 总耗时约 23 秒，和上一轮同口径接近。

实现效果：4/5。已知短函数不会再按 executable segment 继续线性 decode。
复杂度：1/5。只加一个 helper。
维护成本：2/5。后续改成完整 function body set 时，这里需要换成 body-aware 判断。
