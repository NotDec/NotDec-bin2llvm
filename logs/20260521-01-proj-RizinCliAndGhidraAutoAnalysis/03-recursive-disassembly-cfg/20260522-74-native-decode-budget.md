# Native Decode Budget

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

当前 `SleighSeedInstructionAnalyzer` 只消费前 8 个初始 function seed，同轮最多 decode 16 个
function/block seed。这对 smoke 足够，但 Bench2 真实项目的高可信入口很多，confirmed function 数量偏少。

这次只提高受控 decode 预算：

- 初始 function seed：8 -> 10
- 同轮总 seed：16 -> 20

## Ghidra 相关实现

Ghidra 的入口反汇编和函数发现不是只处理少量入口：

- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/EntryPointAnalyzer.java`
  - `analyze(...)` 会把 entry point 和 symbol 入口交给 disassembler / function creation。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/FunctionStartAnalyzer.java`
  - 从已知入口和模式继续确认函数起点。
- `Ghidra/Features/Base/src/main/java/ghidra/app/cmd/disassemble/DisassembleCommand.java`
  - 从入口地址启动反汇编，并让结果进入 listing。

rizin 侧 `aa` / `aaf` / `afr` 也会围绕 entry、symbol、direct call 递归扩展函数。native 侧现在还没有完整边界和调度，所以只做小步增加预算。

## native 侧复刻策略

1. 保留当前 direct branch/call 规则，不新增 speculative seed。
2. 保留每个 seed 8 条 / 64 字节的局部 decode 上限。
3. 只把初始 seed 和总 seed 数量翻倍，让更多已有高可信入口进入 confirmed function。
4. 用 Bench2 smoke 对比 confirmed function、block、instruction、xref 和耗时。

暂时不做：

- 不解析普通 indirect call / jump table。
- 不扩大单个 seed 的线性 decode 长度。
- 不把所有 symbol 一次性全部 lower，避免函数边界还不完整时误伤性能。

## 判断标准

1. `notdec-native-discover` 和 `notdec-native-llvm` 能编译。
2. Bench2 smoke 继续通过。
3. 三个目标的 confirmed function / instruction 数量应增加。
4. 记录同口径 smoke 时间；如果预算导致耗时接近翻倍，则收紧预算。

## 风险

1. 多确认函数会让 `--all-confirmed` lower 更多函数，可能暴露更多 unsupported P-Code。
2. 如果高可信 seed 排序不理想，增加预算未必对应更有价值的函数。
3. 这仍不是完整函数发现，只是更少地截断当前保守队列。

## 实现记录

### 修改范围

1. `lib/NativeAnalysis.cpp`
   - 第 1340 行附近：更新 `SleighSeedInstructionAnalyzer` 的预算注释。
   - 第 1342 行附近：`MaxInitialSeeds` 从 8 提到 10。
   - 第 1343 行附近：`MaxSeeds` 从 16 提到 20。
2. `ARCHITECTURE.md`
   - 第 62 行附近：记录初始 seed 预算为 10。
   - 第 103 行附近：记录同轮总 decode 上限为 20。
3. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
   - 第 19 行附近：阶段 3 记录新的 bounded seed 消费预算。

### 行为

这次只扩大入口队列的受控消费量。单个 seed 仍最多 decode 8 条 / 64 字节，direct branch/call、
函数边界截断、unresolved indirect flow 规则都不变。

实现时试过 16/32 和 12/24：两个配置 smoke 都通过，但 vsftpd/libuv 耗时增加较多。
最终保留 10/20，收益较小但性能压力更低。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
git diff --check
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260522-74c
```

结果：通过。

最终 10/20 指标：

```text
target	elapsed_seconds	confirmed_functions	basic_blocks	instructions	xrefs_total	xrefs_flow	xrefs_call	xrefs_data	xrefs_string	unresolved_total	unresolved_indirect_call	unresolved_indirect_branch
vsftpd	19	13	38	126	315	17	4	155	139	0	0	0
libuv	27	11	28	91	23	9	3	11	0	0	0	0
memcached	14	11	30	86	187	13	2	70	102	0	0	0
```

上一轮 8/16 指标：

```text
target	elapsed_seconds	confirmed_functions	basic_blocks	instructions	xrefs_total	xrefs_flow	xrefs_call	xrefs_data	xrefs_string	unresolved_total	unresolved_indirect_call	unresolved_indirect_branch
vsftpd	12	9	28	75	303	13	2	149	139	0	0	0
libuv	23	9	26	80	23	9	3	11	0	0	0	0
memcached	12	9	28	75	179	13	2	62	102	0	0	0
```

对比：

- vsftpd：confirmed function 9 -> 13，instruction 75 -> 126，耗时 12s -> 19s。
- libuv：confirmed function 9 -> 11，instruction 80 -> 91，耗时 23s -> 27s。
- memcached：confirmed function 9 -> 11，instruction 75 -> 86，耗时 12s -> 14s。

unresolved indirect call / branch 仍为 0。

### 评分

- 实现效果：6/10。三个 Bench2 目标都有更多 confirmed function 和 instruction，但仍是保守预算，不是完整函数发现。
- 复杂度：1/10。只调整现有预算常量和说明。
- 维护成本：2/10。后续如果 CFG 边界规则更稳，可以继续提高预算或改成可配置分析级别。
