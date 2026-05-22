# Next Seed Decode Boundary

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

native recursive decode 已经会用 symbol size 或 `.eh_frame` FDE range 限制 seed decode。
但有些 seed 没有 range，比如 dynamic init/fini 或 direct call 发现的入口。
这些入口当前最多线性 decode 64 字节，可能跨到下一个已知函数入口。

本次只补一个保守边界：当当前 seed 没有 range 时，如果后面已经有更近的 function seed，就把 decode 字节数截断到下一个 seed 前。

## Ghidra 相关实现

Ghidra 不会只按固定字节数把相邻函数混在一起：

- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/AutoAnalysisManager.java`
  - 统一调度分析器，函数、符号、反汇编会反复影响地址归属。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/function/FunctionStartAnalyzer.java`
  - 识别函数起点，并把候选入口交给后续函数创建。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/disassembler/DisassembleCommand.java`
  - 从入口反汇编时会尊重 Program 里已有 code / function 边界。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/FunctionManager.java`
  - 维护函数入口和函数体，避免一个函数体无约束吞掉其他函数入口。

rizin 的 `RzAnalysisFunction` / `RzAnalysisBlock` 也会把已知函数入口作为函数边界信号。

## native 侧复刻策略

1. 在 `boundedBytesForFunctionSeed(...)` 里保留已有 range 优先逻辑。
2. 如果当前 seed 没有 range，就扫描 `state.functionSeeds()`。
3. 找到 `blockAddress` 后面最近的 seed address。
4. 如果最近 seed 在当前可执行范围内，就用它截断 `availableBytes`。

暂时不做：

- 不新增函数入口。
- 不用线性扫描反推出未知边界。
- 不解析 jump table 或函数指针。
- 不改变当前 MaxBytes / MaxInstructions 限制。

## 判断标准

1. `notdec-native-discover` 能编译。
2. Bench2 smoke 继续通过。
3. 三个目标 unresolved indirect call / branch 仍为 0。
4. 如果当前用例指标变化，必须能解释为已知入口边界收紧导致。

## 风险

1. 如果已有 seed 本身是假阳性，截断会让当前函数少 decode；但 seed 只来自强来源或 direct call，风险比跨函数 decode 低。
2. 当前只限制 decode 字节数，不代表完整函数边界恢复完成。

## 实现记录

### 修改范围

1. `lib/NativeAnalysis.cpp`
   - 第 1908 行附近：`boundedBytesForFunctionSeed(...)` 在无可用 range 时不再直接返回 `availableBytes`。
   - 第 1928 行附近：新增 `capBytesAtNextFunctionSeed(...)`，扫描 `state.functionSeeds()`，用后续最近 seed address 截断 decode 字节数。
2. `ARCHITECTURE.md`
   - 第 56 行附近：补充无 range seed 会被后续最近 function seed 入口截断。
3. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
   - 阶段 3 记录该边界收紧已完成。

### 行为

已有 `RangeStart/RangeEnd` 的 seed 仍优先使用明确 range。
没有 range，或者 queued block 不在 seed range 内时，decode 上限会被下一个已知 function seed 截断。

这次没有新增 seed，没有解析间接流，也没有改变 8 条 / 64 字节和 16 个 seed 的当前上限。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm -j2
git diff --check
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260522-66b
```

结果：通过。

Bench2 smoke：

```text
vsftpd ok elapsed=10s
libuv ok elapsed=20s
memcached ok elapsed=10s
```

性能和规模：

```text
target	elapsed_seconds	confirmed_functions	basic_blocks	instructions	xrefs_total	xrefs_flow	xrefs_call	xrefs_data	xrefs_string	unresolved_total	unresolved_indirect_call	unresolved_indirect_branch
vsftpd	10	9	28	75	303	13	2	149	139	0	0	0
libuv	20	9	26	80	23	9	3	11	0	0	0	0
memcached	10	9	28	75	179	13	2	62	102	0	0	0
```

和上一轮同口径相比，三个目标的 confirmed function 数不变，block / instruction / xref 略少。
这是无 range seed 被后续已知函数入口截断后的预期结果；unresolved indirect call / branch 仍为 0。
耗时没有增加。

### 评分

- 实现效果：7/10。减少跨函数线性 decode，推进函数边界，但还不是完整边界恢复。
- 复杂度：2/10。只在现有 decode cap 路径上加一个 seed 扫描。
- 维护成本：2/10。逻辑依赖已有 `functionSeeds()` 排序，后续如果 seed 来源变多仍可复用。
