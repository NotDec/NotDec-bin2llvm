# Confirmed function / block / xref state

## 原始 prompt

在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis里面根据划分的几个关键宏观步骤，创建对应的文件夹，然后在prompt中要求，对应步骤的规划和修改必须放到对应的文件夹里。

本次目标是按照规划，完善bin2llvm项目反汇编转IR的native链路（即不走GhidraScript的链路，而是走libsla，libdecomp的这个链路），从Ghidra和rizin那边复刻足够多的功能。每当实现一部分功能后，就看看能否利用bench2里面这些项目去做初步的测试： （vsftpd可执行文件，libuv动态链接库，memcached可执行文件），测试实现是否正确。规划中每一部分都完成后即可停止。在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis 下创建一个PROGRESS.md里追踪计划的实现。完成一个部分后，同步更新 项目顶层的ARCHITECTURE.md 反映实现的功能模块的架构。

约束：根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

## Ghidra 对应实现

Ghidra 的 Program 是中心状态，几个关键 manager 分开存储：

1. `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/FunctionManager.java`
   - `createFunction(...)`
   - `getFunctionAt(...)`
   - `getFunctionContaining(...)`
   - `getFunctions(...)`
2. `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/database/function/FunctionManagerDB.java`
   - `createFunction(...)`
   - 负责把函数 entry、body、symbol 等写入数据库。
3. `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/database/code/CodeManager.java`
   - `createCodeUnit(...)`
   - `addInstruction(...)`
   - `addReferencesForInstruction(...)`
   - 负责 instruction/code unit 的持久状态。
4. `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java`
   - `addMemoryReference(...)`
   - `getReferencesFrom(...)`
   - `getReferencesTo(...)`
5. `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/database/references/ReferenceDBManager.java`
   - `addMemoryReference(...)`
   - `getReferencesFrom(...)`
   - `getReferencesTo(...)`
   - 负责 xref 的实际存储。

这里先不复刻 Ghidra 的数据库和事务。对 native 链路来说，当前更需要一个能被 analyzer 共享的内存态。

## 本次复刻范围

本次只补 `NativeProgramState` 的轻量状态：

1. `NativeFunction`：confirmed function，和 seed 分开。
2. `NativeBasicBlock`：最小 basic block，记录 start/end 和 successor。
3. `NativeXref`：记录 from/to/kind/source，可查 from/to/list。

暂不做：

1. 不引入复杂 interval tree。
2. 不做递归反汇编。
3. 不推断 jump table、data/string xref。
4. 不让 seed 自动变 confirmed function，避免还没 decode 就污染状态。

## 判断标准

1. 代码能编译。
2. `NativeProgramState` 能写入和查询 confirmed function、block、xref。
3. `notdec-native-discover` 报告能打印 confirmed function、block、xref 数量。
4. 现有 seed 发现行为不变。

## 实现记录

修改文件：

1. `include/notdec-bin2llvm/NativeAnalysis.h`
   - 第 119 行新增 `NativeXrefKind`。
   - 第 128 行新增 `NativeBasicBlock`。
   - 第 137 行新增 `NativeFunction`。
   - 第 149 行新增 `NativeXref`。
   - 第 190 行到第 209 行给 `NativeProgramState` 增加 function/xref 查询和写入接口。
   - 第 228 行到第 231 行增加 confirmed function、xref 及 from/to 索引存储。
2. `lib/NativeAnalysis.cpp`
   - 第 1193 行到第 1212 行在 discovery report 中输出 confirmed function、basic block、xref 统计。
   - 第 1319 行新增 `toString(NativeXrefKind)`。
   - 第 1426 行到第 1472 行实现 `functionAt`、`functionContaining`、`xrefsFrom`、`xrefsTo`。
   - 第 1530 行到第 1582 行实现 `addFunction`、`addBasicBlock`、`addXref`。
3. `ARCHITECTURE.md`
   - 增加 native discovery state 小节，说明 seed 和 confirmed function 分开。
4. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
   - 记录阶段 1 的已完成小块和剩余 instruction 存储。

验证：

1. 构建：
   - `cmake -S . -B /tmp/notdec-bin2llvm-build -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON -DNOTDEC_BIN2LLVM_SLEIGH_SOURCE_DIR=/sn640/sleigh -DNOTDEC_BIN2LLVM_GHIDRA_SOURCE_DIR=/sn640/ghidra`
   - `cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2`
2. Bench2 初步运行：
   - `vsftpd`：function seeds 186，confirmed functions 0，xrefs 0。
   - `libuv.so.1.0.0`：function seeds 484，confirmed functions 0，xrefs 0。
   - `memcached`：function seeds 258，confirmed functions 0，xrefs 0。

confirmed functions 和 xrefs 还是 0 是本小块的预期结果：这里只放状态和报告字段，还没有递归 decode 去填充它们。

性能：

1. 本次只增加空状态容器和报告统计，现有 seed 发现路径没有新增循环级别的工作。
2. `functionContaining` 目前线性扫 confirmed functions。当前还没有 confirmed function，后续数量上来后再按实际查询压力改成区间索引。

评分：

1. 实现效果：7/10。状态骨架已经能承接后续 CFG 和 xref，但还没有 instruction 存储。
2. 复杂度：2/10。只加简单 vector/map，不引入新框架。
3. 维护成本：3/10。接口少，后续需要在 recursive decode 时补更多字段。

更好的方案：

可以直接做一个接近 Ghidra Program DB 的 manager 层，但现在会过早变复杂。当前保持简单内存态，等 CLI 和 recursive decode 真正需要更多索引时再补。
