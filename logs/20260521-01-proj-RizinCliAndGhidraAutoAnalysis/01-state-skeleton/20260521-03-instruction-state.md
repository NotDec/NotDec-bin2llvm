# Instruction state

## 原始 prompt

在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis里面根据划分的几个关键宏观步骤，创建对应的文件夹，然后在prompt中要求，对应步骤的规划和修改必须放到对应的文件夹里。

本次目标是按照规划，完善bin2llvm项目反汇编转IR的native链路（即不走GhidraScript的链路，而是走libsla，libdecomp的这个链路），从Ghidra和rizin那边复刻足够多的功能。每当实现一部分功能后，就看看能否利用bench2里面这些项目去做初步的测试： （vsftpd可执行文件，libuv动态链接库，memcached可执行文件），测试实现是否正确。规划中每一部分都完成后即可停止。在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis 下创建一个PROGRESS.md里追踪计划的实现。完成一个部分后，同步更新 项目顶层的ARCHITECTURE.md 反映实现的功能模块的架构。

约束：根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

## 当前目标和已有 native 状态

上一小块已经在 `NativeProgramState` 里加入 confirmed function、basic block、xref。还缺 instruction 存储。递归反汇编后需要把每条已接受的指令放进共享状态，这样 CLI、CFG、xref、lowering 可以用同一份事实。

本次只加状态，不做 decode。

## Ghidra 对应实现

Ghidra 把指令作为 Listing/CodeManager 里的 code unit：

1. `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Listing.java`
   - `getInstructionAt(...)`
   - `getInstructions(...)`
   - `createInstruction(...)`
2. `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/database/ListingDB.java`
   - `getInstructionAt(...)`
   - `getInstructions(...)`
   - `createInstruction(...)`
3. `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/database/code/CodeManager.java`
   - `createCodeUnit(...)`
   - `addInstruction(...)`
   - `getInstructionAt(...)`
   - `getInstructions(...)`
   - `addReferencesForInstruction(...)`
4. `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/database/code/InstructionDB.java`
   - 代表单条数据库里的 instruction。
   - 能提供地址、长度、fallthrough、operand reference 等信息。

Ghidra 的设计是先把 instruction 放入 Listing，再由 reference manager、function manager 等读取它。native 侧现在先保留这个拆分：instruction 是独立事实，不直接塞进 basic block。

## 本次复刻范围

新增：

1. `NativeInstruction`：记录地址、长度、原始字节、助记符、来源。
2. `NativeProgramState::addInstruction(...)`。
3. `NativeProgramState::instructionAt(...)`。
4. `NativeProgramState::instructionsInRange(...)`。
5. report 输出 instruction 数量。

暂不做：

1. 不保存 operand 细节。
2. 不保存 P-Code。
3. 不从 bytes 自动解析 mnemonic。
4. 不自动生成 xref。

## 判断标准

1. 代码能编译。
2. discovery report 能打印 instruction 数量。
3. Bench2 三个目标仍能跑完 seed discovery。
4. 当前 instruction 数量为 0 是预期，因为 decode 还没接入。

## 实现记录

修改文件：

1. `include/notdec-bin2llvm/NativeAnalysis.h`
   - 第 159 行到第 169 行新增 `NativeInstruction`。
   - 第 209 行到第 231 行给 `NativeProgramState` 增加 instruction 查询和写入接口。
   - 第 254 行增加 instruction 存储。
2. `lib/NativeAnalysis.cpp`
   - 第 1193 行到第 1213 行在 discovery report 中输出 instruction 数量。
   - 第 1476 行到第 1497 行实现 `instructionAt` 和 `instructionsInRange`。
   - 第 1609 行到第 1628 行实现 `addInstruction`。
3. `ARCHITECTURE.md`
   - 增加 `NativeInstruction` 的职责说明，并补上 instruction 查询接口。
4. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
   - 标记阶段 1 的 instruction 存储已完成。

验证：

1. 构建：
   - `cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2`
2. Bench2 初步运行：
   - `vsftpd`：function seeds 186，instructions 0。
   - `libuv.so.1.0.0`：function seeds 484，instructions 0。
   - `memcached`：function seeds 258，instructions 0。
3. 这一步没有让 instruction 数量变成非零，因为还没有 decode 接口喂数据，这是预期。

性能：

1. 这次只加一个 `std::map` 和两个只读查询，`discover` 路径没有明显变化。
2. `instructionsInRange` 现在也是线性收集，后续只有 decode 量上来才需要换索引。
