# Dynamic symbol seeds

## 原始 prompt

在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis里面根据划分的几个关键宏观步骤，创建对应的文件夹，然后在prompt中要求，对应步骤的规划和修改必须放到对应的文件夹里。

本次目标是按照规划，完善bin2llvm项目反汇编转IR的native链路（即不走GhidraScript的链路，而是走libsla，libdecomp的这个链路），从Ghidra和rizin那边复刻足够多的功能。每当实现一部分功能后，就看看能否利用bench2里面这些项目去做初步的测试： （vsftpd可执行文件，libuv动态链接库，memcached可执行文件），测试实现是否正确。规划中每一部分都完成后即可停止。在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis 下创建一个PROGRESS.md里追踪计划的实现。完成一个部分后，同步更新 项目顶层的ARCHITECTURE.md 反映实现的功能模块的架构。

约束：根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

## 当前目标和已有 native 状态

当前 `ElfSymbolAnalyzer` 会从 `binary.symbols()` 加 `STT_FUNC` seed。Bench2 的 shared object 还需要明确解释 dynamic/export symbol 来源，尤其是 `libuv.so.1.0.0` 这种库。

本次只把 `.dynsym` 里的已定义函数符号作为独立来源记录。重复地址不重复入 worklist，只给已有 seed 增加 source/name。

## Ghidra 对应实现

Ghidra 会把 loader 导入的 symbol 和 external entry 交给入口分析：

1. `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/disassembler/EntryPointAnalyzer.java`
   - `added(...)`
   - `analyze(...)`
   - 会从 symbol 和 external entry 收集待反汇编地址。
2. `Ghidra/Features/Base/src/main/java/ghidra/app/util/opinion/ElfLoader.java`
   - `load(...)`
   - `process(...)`
   - ELF loader 负责把 ELF symbols/import/export 先落到 program symbol 体系里。
3. `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/SymbolTable.java`
   - `getSymbols(...)`
   - `createLabel(...)`
   - EntryPointAnalyzer 后续从 symbol table 读取 code symbol。

native 侧没有完整 symbol table，这里先直接用 LIEF 的 `Binary::dynamic_symbols()`。

## 本次复刻范围

新增：

1. `ElfSymbolAnalyzer` 扫 `binary.dynamic_symbols()`。
2. 只接受 `STT_FUNC`、非 `UNDEF`、地址非 0、落在 executable segment 内的符号。
3. 来源写成 `elf-dynamic-symbol`。

暂不做：

1. 不处理 imported undefined symbol。
2. 不把 dynamic object symbol 当函数。
3. 不猜测地址为 0 的 IFUNC/import。

## 判断标准

1. 代码能编译。
2. `libuv.so.1.0.0` report 里能看到 `elf-dynamic-symbol` 来源。
3. `vsftpd`、`memcached` seed/worklist 仍稳定。
4. worklist 不因为重复 dynamic symbol 地址重复入队。

## 实现记录

修改文件：

1. `lib/NativeAnalysis.cpp`
   - 第 346 行到第 354 行让 `ElfSymbolAnalyzer::run(...)` 同时扫描 `binary.symbols()` 和 `binary.dynamic_symbols()`。
   - 第 357 行到第 377 行新增 `addSymbolSeed(...)`，统一过滤 `STT_FUNC`、非 `UNDEF`、非 0、executable 地址。
   - 第 372 行的去重键包含 source，确保同一地址同一名字的 dynamic symbol 也能给已有 seed 补来源，但不会重复入 worklist。
2. `ARCHITECTURE.md`
   - 第 17 行到第 18 行把 dynamic symbol 写进 native seed 来源。
3. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
   - 阶段 2 记录 `.dynsym` 已定义函数符号 seed 来源已完成。

验证：

1. 构建：
   - `cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2`
2. Bench2 初步运行：
   - `vsftpd`：function seeds 186，function worklist 186。
   - `libuv.so.1.0.0`：function seeds 484，function worklist 484，`elf-dynamic-symbol: 307`。
   - `memcached`：function seeds 258，function worklist 258。

性能：

1. 新增一次 `dynamic_symbols()` 扫描，只处理符号表，和当前 seed discovery 同级。
2. worklist 数量没有增加，说明重复 dynamic symbol 没有造成重复 decode 队列。
