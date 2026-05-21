# GOT BRANCHIND External Xref

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

当前 Bench2 剩余 unresolved indirect branch 主要有两类：

- PLT0 resolver：`0x5026`，先不处理。
- `.text` 里的 guarded external tail jump：
  - `vsftpd 0x82af -> _ITM_deregisterTMCloneTable`
  - `libuv 0x9d9f -> _ITM_deregisterTMCloneTable`
  - `memcached 0xb96f -> _ITM_deregisterTMCloneTable`

这些 tail jump 的形状和之前的 guarded `CALLIND` 类似：先从外部 `GLOB_DAT` GOT slot 读值，
判空，再 `jmp *%rax`。这不是 jump table，也不是本模块内部 CFG successor。当前 discovery
已经有局部来源追踪，只是 `BRANCHIND` 还没复用它。

## Ghidra 相关实现

Ghidra 对 external thunk / external reference 会用符号和引用层面的信息处理，不会把它当普通
jump table：

- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/ExternalSymbolResolverAnalyzer.java`
  - 负责外部符号解析和外部引用建立。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java`
  - 保存指令到外部目标的引用。
- `Ghidra/Features/Base/src/main/java/ghidra/app/cmd/function/CreateFunctionCmd.java::getFunctionBody(...)`
  - 对 computed flow 保守，不把它直接扩进函数 body。
- `Ghidra/Processors/x86/data/languages/ia.sinc`
  - `jmp *reg` 仍会生成 `BRANCHIND`，真实目标要结合寄存器来源和 relocation 判断。

native 侧这次只复刻这个明确模式：`BRANCHIND` 输入来源能追到带符号名的外部 `GLOB_DAT`。

## native 侧复刻策略

1. 复用 `addDirectControlFlow(...)` 里已有的 `sourceRamByVarnode`。
2. `BRANCHIND` 先保留已有 direct `ram` PLT GOT slot 判断。
3. 如果未命中 PLT，再看输入 varnode 的来源是否能追到外部 `X86_64_GLOB_DAT`。
4. 命中时记录 flow xref：`sleigh-pcode-got-indirect-branch`。
5. 命中后不再写 unresolved。
6. 其他 `BRANCHIND` 保持 unresolved。

暂时不做：

- 不解析 PLT0 resolver。
- 不解析 jump table。
- 不把 external tail jump 当内部 function seed。
- 不改 lowering。

## 判断标准

1. 三个 Bench2 目标的 `_ITM_deregisterTMCloneTable` tail jump 不再 unresolved。
2. PLT0 resolver 仍保留 unresolved。
3. xref 查询能看到 `sleigh-pcode-got-indirect-branch`。
4. Bench2 smoke 继续通过 LLVM 22 verify。
5. 运行时间不明显变慢。

## 风险

1. xref 目标仍是 GOT slot，不是外部符号节点；这是当前 `NativeXref` 的限制。
2. 只处理来源能追到 direct RAM GOT 的模式，覆盖面有限。
3. `GLOB_DAT` 符号类型可能是 `NOTYPE`，所以不强制 FUNC；但必须真实流入 `BRANCHIND`。

## 实现记录

改动文件：

- `lib/NativeAnalysis.cpp`
  - 第 1571 行 `SleighSeedInstructionAnalyzer::addDirectControlFlow(...)` 的 `BRANCHIND` 分支：保留 direct `ram` PLT GOT 判断；未命中 PLT 时，复用 P-Code 来源追踪，若输入来源是外部 `X86_64_GLOB_DAT` GOT slot，记录 `sleigh-pcode-got-indirect-branch` flow xref，并跳过 unresolved。
  - 第 1652 行 `branchIndGotSource(...)`：提取 `BRANCHIND` 输入 varnode 对应的来源 RAM 地址。
- `scripts/bench2-native-smoke.sh`
  - 第 195-199 行：把 unresolved indirect branch 基线收紧为 `vsftpd` / `memcached` <= 1，`libuv` <= 0。
- `ARCHITECTURE.md`
  - 第 61-65 行：记录 `BRANCHIND` 来源追到外部 `GLOB_DAT` 时的 xref 行为。
  - 第 139-144 行：记录当前 Bench2 branch 基线。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
  - 第 26 行：阶段 3 记录外部 `GLOB_DAT` `BRANCHIND` xref。
  - 第 64 行：阶段 7 记录 branch 基线收紧。

实现说明：

- 这次没有解析 PLT0，也没有做 jump table。
- xref 目标仍是 GOT slot，因为当前 `NativeXref` 还没有 external symbol node。
- `_ITM_deregisterTMCloneTable` 这类弱 `NOTYPE UND` 符号仍可命中；判断依据是外部 `GLOB_DAT` slot 真实流入 `BRANCHIND`。

验证：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
```

通过。

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-45b
```

结果：

- `vsftpd`：ok，8s，confirmed 9，blocks 30，instr 80，xrefs 303，flow 13，call 2，unresolved 1，indirect call 0，indirect branch 1。
- `libuv`：ok，19s，confirmed 9，blocks 29，instr 85，xrefs 26，flow 10，call 3，unresolved 0，indirect call 0，indirect branch 0。
- `memcached`：ok，8s，confirmed 9，blocks 30，instr 80，xrefs 185，flow 13，call 2，unresolved 1，indirect call 0，indirect branch 1。

stdout/stderr 文件为空。LLVM 22 `llvm-as` 和 `opt -passes=verify` 均通过。

xref 查询：

```text
vsftpd 0x82af -> 0x25fe8 flow sleigh-pcode-got-indirect-branch
libuv 0x9d9f -> 0x33fb8 flow sleigh-pcode-got-indirect-branch
memcached 0xb96f -> 0x3efe8 flow sleigh-pcode-got-indirect-branch
```

性能：

- 本次只在已有 P-Code 扫描中多查一次来源 map 和 relocation 判断。
- 三目标 smoke 总耗时约 35s，和前面加入单函数检查后的同口径 smoke 接近，没有看到明显变慢。

评分：

- 实现效果：8/10。Bench2 当前三个 guarded external tail jump 都不再 unresolved，并且 xref 可查。
- 复杂度：2/10。复用已有来源追踪和外部 `GLOB_DAT` 判断，没有新状态。
- 维护成本：2/10。后续如果要把 GOT slot 映射成 external symbol，需要扩展 `NativeXref` 形状；本次逻辑可以保留为判定入口。
