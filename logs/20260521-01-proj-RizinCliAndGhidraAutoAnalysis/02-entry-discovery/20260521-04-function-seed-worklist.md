# Function seed worklist

## 原始 prompt

在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis里面根据划分的几个关键宏观步骤，创建对应的文件夹，然后在prompt中要求，对应步骤的规划和修改必须放到对应的文件夹里。

本次目标是按照规划，完善bin2llvm项目反汇编转IR的native链路（即不走GhidraScript的链路，而是走libsla，libdecomp的这个链路），从Ghidra和rizin那边复刻足够多的功能。每当实现一部分功能后，就看看能否利用bench2里面这些项目去做初步的测试： （vsftpd可执行文件，libuv动态链接库，memcached可执行文件），测试实现是否正确。规划中每一部分都完成后即可停止。在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis 下创建一个PROGRESS.md里追踪计划的实现。完成一个部分后，同步更新 项目顶层的ARCHITECTURE.md 反映实现的功能模块的架构。

约束：根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

## 当前目标和已有 native 状态

当前 native 已经能从 ELF entry、dynamic init/fini、symbol、PLT relocation、`.eh_frame` 得到 `NativeFunctionSeed`。但后续 recursive decode 需要一个统一入口队列，不能让每个 analyzer 自己扫 seed map。

本次只把新 seed 放进 worklist，不做调度、不做 decode。

## Ghidra 对应实现

Ghidra 的入口分析和后续分析靠 AutoAnalysis 队列衔接：

1. `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/disassembler/EntryPointAnalyzer.java`
   - `added(...)`
   - `analyze(...)`
   - 把入口、code symbol、外部入口等收集成待反汇编地址。
2. `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/AutoAnalysisManager.java`
   - `schedule(...)`
   - `startAnalysis(...)`
   - 负责 analyzer 间的任务推进。
3. `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/FunctionManager.java`
   - `createFunction(...)`
   - 反汇编和函数创建最终落到 function manager。

native 侧现在已有 `NativeAnalysisManager`，但它只是按 priority 跑 analyzer，还没有可被 recursive decode 消费的函数队列。

## 本次复刻范围

新增：

1. `NativeFunctionWorkItem`：记录一个待处理函数入口和首次来源。
2. `NativeProgramState::functionWorklist()`：让后续 analyzer 能按 seed 插入顺序取入口。
3. `addFunctionSeed(...)` 在新 seed 插入时追加 work item。
4. discovery report 输出 worklist 数量。

暂不做：

1. 不实现 pop/ack 状态。
2. 不做优先级重排。
3. 不把重复 seed 重复入队。
4. 不把 worklist 自动变 confirmed function。

## 判断标准

1. 代码能编译。
2. Bench2 三个目标 report 里 worklist 数量和 function seed 数量一致。
3. 现有 seed 来源、range、PLT、eh_frame 统计不变。

## 实现记录

修改文件：

1. `include/notdec-bin2llvm/NativeAnalysis.h`
   - 第 51 行到第 57 行新增 `NativeFunctionWorkItem`。
   - 第 201 行到第 203 行新增 `functionWorklist()` 查询接口。
   - 第 256 行新增 `FunctionWorklist` 存储。
2. `lib/NativeAnalysis.cpp`
   - 第 1137 行到第 1139 行在 discovery report 中输出 worklist 数量。
   - 第 1550 行到第 1552 行在新 seed 首次插入时追加 work item。
3. `ARCHITECTURE.md`
   - 第 19 行补充 `NativeFunctionWorkItem` 的位置。
   - 第 29 行补充 `functionWorklist()` 查询接口。
4. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
   - 记录阶段 2 已完成 function seed 到 worklist 的最小桥接。

验证：

1. 构建：
   - `cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2`
2. Bench2 初步运行：
   - `vsftpd`：function seeds 186，function worklist 186。
   - `libuv.so.1.0.0`：function seeds 484，function worklist 484。
   - `memcached`：function seeds 258，function worklist 258。

性能：

1. `addFunctionSeed(...)` 只在新 seed 插入时多做一次 vector append。
2. report 只是读 vector size，对当前 Bench2 发现时间没有可见影响。
