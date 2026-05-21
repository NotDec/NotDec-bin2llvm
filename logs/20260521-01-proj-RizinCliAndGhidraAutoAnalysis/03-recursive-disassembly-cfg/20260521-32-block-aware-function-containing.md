# Block Aware Function Containing

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

native recursive decode 已经会把同一函数内的 direct branch successor 作为新 block 加到
`NativeFunction::Blocks`。为了让 `addBasicBlock(...)` 能维护一个粗范围，`RangeStart/RangeEnd`
会覆盖所有 block 的最小到最大地址。

问题是 `NativeProgramState::functionContaining(...)` 仍然只看 `RangeStart/RangeEnd`。如果一个函数
有多个不连续 block，中间空洞也会被误判成属于这个函数。之前日志里已经把这个列为后续要补的点。

这次只改查询语义：`functionContaining(...)` 优先按 `NativeBasicBlock` 的半开区间判断。

## Ghidra 相关实现

Ghidra 的函数归属不是靠一个连续范围：

- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Function.java`
  - `getBody()` 返回函数 body 的地址集合。
- `/sn640/ghidra/Ghidra/Features/Base/src/main/java/ghidra/program/util/SymbolMerge.java`
  - 使用 `function.getBody()` 复制函数范围。
- `/sn640/ghidra/Ghidra/Features/Base/src/main/java/ghidra/program/util/ProgramMerge.java`
  - 使用 `FunctionManager.getFunctionsOverlapping(...)` 和 `function.getBody()` 判断函数 body。
- `/sn640/ghidra/Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/FindNoReturnFunctionsAnalyzer.java`
  - `fixCallingFunctionBody(...)` 里会拿旧 `getBody()`，再用 `CreateFunctionCmd.getFunctionBody(...)` 修正。

native 还没有 Ghidra 那种完整 `AddressSetView`。当前最接近 body set 的数据就是 `NativeFunction::Blocks`。

## native 侧复刻策略

1. `functionContaining(...)` 遍历 confirmed functions。
2. 对每个函数遍历 `Blocks`。
3. 只有 `block.Start <= address < block.End` 才返回该函数。
4. 先不做 range fallback，避免继续把 block 空洞算进去。

暂时不做：

- 不引入新的 body set 数据结构。
- 不改 `RangeStart/RangeEnd`，它仍用于 summary、lowering 保守范围和显示。
- 不改 block 添加逻辑。

## 判断标准

1. `functionContaining(...)` 不再把 block 空洞地址当成函数内部。
2. Bench2 smoke 继续通过。
3. 当前 summary 不应该因为这个查询改动而变化，除非有现有路径真实依赖它。

## 风险

1. 如果某个 `NativeFunction` 还没有 block，`functionContaining(...)` 会返回 null。这符合当前 confirmed function 的定义：至少应有保守 body/block。
2. 后续如果有非 block 化的函数来源，需要先补 block 或显式 body set。

## 实现记录

### 修改文件和函数

1. `lib/NativeAnalysis.cpp:2019`
   - 修改 `NativeProgramState::functionContaining(...)`。
   - 原来用 `RangeStart <= address < RangeEnd` 判断。
   - 现在遍历 `NativeFunction::Blocks`，只有 `block.Start <= address < block.End` 才返回函数。
2. `ARCHITECTURE.md:73`
   - 记录 `functionContaining(...)` 已改为 block-aware 查询。
3. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:11`
   - 更新 Stage 3 完成项。

### 验证命令

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm notdec-native-discover -j2
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-32
diff -u /tmp/notdec-bin2llvm-bench2-smoke-20260521-31/vsftpd.summary.json /tmp/notdec-bin2llvm-bench2-smoke-20260521-32/vsftpd.summary.json
diff -u /tmp/notdec-bin2llvm-bench2-smoke-20260521-31/libuv.summary.json /tmp/notdec-bin2llvm-bench2-smoke-20260521-32/libuv.summary.json
diff -u /tmp/notdec-bin2llvm-bench2-smoke-20260521-31/memcached.summary.json /tmp/notdec-bin2llvm-bench2-smoke-20260521-32/memcached.summary.json
```

### Bench2 结果

三个目标都通过 `notdec-native-discover --summary-json`、`notdec-native-llvm --all-confirmed`、
LLVM 22 `llvm-as`、LLVM 22 `opt -passes=verify`。

```text
vsftpd ok elapsed=8s
libuv ok elapsed=8s
memcached ok elapsed=8s
```

summary 关键数据：

```text
vsftpd: confirmed_functions=9, basic_blocks=30, instructions=80, xrefs.total=297, data=149, string=139
libuv: confirmed_functions=10, basic_blocks=32, instructions=93, xrefs.total=24, data=13, string=0
memcached: confirmed_functions=9, basic_blocks=30, instructions=80, xrefs.total=179, data=68, string=102
```

和上一轮同口径 summary diff：无差异。

IR 里 direct call 仍然存在：

```text
vsftpd: call void @notdec_native_5ba0(), call void @notdec_native_8290()
libuv: call void @notdec_native_9d80(), call void @notdec_native_9350()
memcached: call void @notdec_native_5b80(), call void @notdec_native_b950()
```

所有 `.stdout` / `.stderr` 日志大小都是 0。

### 性能和影响

这次只改一个查询函数。Bench2 三目标 smoke 总耗时约 24 秒，和上一轮同口径接近。

实现效果：4/5。查询语义更接近 Ghidra function body，不再把 block 空洞当函数内部。
复杂度：1/5。只遍历已有 block。
维护成本：1/5。后续如果引入 body set，可以替换这一层查询。
