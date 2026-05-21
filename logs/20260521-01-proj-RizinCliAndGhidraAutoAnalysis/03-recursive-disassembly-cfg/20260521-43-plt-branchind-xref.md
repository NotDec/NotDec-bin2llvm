# PLT BRANCHIND Xref

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

当前 Bench2 已没有 unresolved indirect call，剩下的是 indirect branch。里面有一批明确的 PLT
外部跳板：

```text
BRANCHIND (ram,0x25fc8,8)
BRANCHIND (ram,0x25a08,8)
BRANCHIND (ram,0x25a10,8)
BRANCHIND (ram,0x25a18,8)
```

这些来自 `.plt.got` / `.plt.sec`，对应 `NativePltEntry::GotAddress`。它们不是 jump table，
也不是普通函数指针。native 已经知道 GOT slot 和外部符号名，只是 discovery 仍把它们记成
unresolved indirect branch。

这次只处理 `BRANCHIND` 第一个输入直接是 `ram` GOT slot，且该 GOT slot 命中 `NativePltEntry`
的情况。

## Ghidra 相关实现

Ghidra 对 PLT / thunk / external reference 会在符号和引用层面建模，不把它和普通 jump table
混在一起：

- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/ExternalSymbolResolverAnalyzer.java`
  - 负责外部符号解析和外部引用建立。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/FunctionStartAnalyzer.java`
  - 结合已有引用和符号信息识别函数入口和 thunk。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java`
  - 保存指令到目标地址或外部符号的引用。
- `Ghidra/Processors/x86/data/languages/ia.sinc`
  - x86 `jmp *mem` 仍会表现为 `BRANCHIND`，是否是 PLT 外部跳板要结合 relocation / PLT 信息判断。

native 已有 `RelocationPltAnalyzer` 产出的 `NativePltEntry`，可以先复刻这一个明确模式。

## native 侧复刻策略

1. 在 `SleighSeedInstructionAnalyzer::addDirectControlFlow(...)` 的 `BRANCHIND` 分支前，检查
   第一个输入是否是 direct `ram`。
2. 如果 `ram` 地址命中任一 `NativePltEntry::GotAddress`，记录 flow xref：
   `sleigh-pcode-plt-indirect-branch`。
3. 命中后不再写 `NativeUnresolvedFlow`。
4. 未命中的 `BRANCHIND` 保持原样，仍结束 block、仍 unresolved。

暂时不做：

- 不解析 PLT0 resolver 入口。
- 不解析 `.text` 里的 `_ITM_*` guarded tail jump。
- 不解析 jump table。
- 不增加外部符号节点。

## 判断标准

1. `vsftpd` / `memcached` 的 `.plt.got` / `.plt.sec` branchind 从 unresolved 中移除。
2. libuv 当前样本只剩 `.text` 里的 `_ITM_*` guarded tail jump，数量不应变化。
3. Bench2 smoke 继续通过 LLVM 22 verify。
4. xref 中能查到 `sleigh-pcode-plt-indirect-branch`。
5. 运行时间不明显变慢。

## 风险

1. xref 目标记录为 GOT slot，不是外部符号节点；这是当前 `NativeXref` 数据结构的限制。
2. 只处理 direct `ram` 输入，覆盖面有限，但不会把普通间接跳转误判成 PLT。
3. PLT0 resolver 仍 unresolved，后续如果要清理，需要单独建模 dynamic resolver。

## 实现记录

### 修改文件和函数

- `lib/NativeAnalysis.cpp`
  - 第 1548 行 `SleighSeedInstructionAnalyzer::addDirectControlFlow(...)`：在 `BRANCHIND` 分支里识别 direct `ram` GOT slot。
  - 第 1550 行：命中 `NativePltEntry::GotAddress` 时记录 `sleigh-pcode-plt-indirect-branch` flow xref，并跳过 unresolved。
  - 第 1612 行 `branchIndGotTarget(...)`：提取 `BRANCHIND (ram,...)` 的 GOT 地址。
  - 第 1652 行 `isPltGotSlot(...)`：判断 GOT 地址是否来自已有 `NativePltEntry`。
- `scripts/bench2-native-smoke.sh`
  - 第 76 行 `require_unresolved_indirect_branches_at_most(...)`：新增 unresolved branch 上限检查。
  - 第 193 行起：当前 Bench2 基线为 `vsftpd <= 3`、`libuv <= 1`、`memcached <= 3`。
- `ARCHITECTURE.md`
  - 第 61 行：记录 PLT `BRANCHIND` xref 行为。
  - 第 138 行：记录 smoke 对 unresolved branch 基线的检查。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
  - 第 24 行：阶段 3 记录 PLT `BRANCHIND` xref。
  - 第 60 行：阶段 7 记录 unresolved branch 基线检查。

实现时确认：

- 当前 `vsftpd` / `memcached` 的 `.plt.sec` stub 命中 `NativePltEntry`，会被移出 unresolved。
- 当前 `vsftpd` / `memcached` 的 `.plt.got` `__cxa_finalize` stub 没出现在 `--plt-json` 里，所以本小步不强行处理。
- PLT0 resolver 入口和 `.text` 里的 `_ITM_deregisterTMCloneTable` guarded tail jump 仍 unresolved。

### 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
```

Bench2 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-43b
```

结果：

```text
vsftpd ok elapsed=7s
libuv ok elapsed=19s
memcached ok elapsed=7s
```

summary：

```text
vsftpd: confirmed=9 blocks=30 instr=80 xrefs=301 flow=11 call=2 unresolved=3 indirect_call=0 indirect_branch=3
libuv: confirmed=9 blocks=29 instr=85 xrefs=25 flow=9 call=3 unresolved=1 indirect_call=0 indirect_branch=1
memcached: confirmed=9 blocks=30 instr=80 xrefs=183 flow=11 call=2 unresolved=3 indirect_call=0 indirect_branch=3
```

xref 查询：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-from-json 0x5bc4 /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-from-json 0x5bd4 /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-from-json 0x5ba4 /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-from-json 0x5bb4 /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
```

结果：

```text
vsftpd 0x5bc4 -> 0x25a10 flow sleigh-pcode-plt-indirect-branch
vsftpd 0x5bd4 -> 0x25a18 flow sleigh-pcode-plt-indirect-branch
memcached 0x5ba4 -> 0x3ea28 flow sleigh-pcode-plt-indirect-branch
memcached 0x5bb4 -> 0x3ea30 flow sleigh-pcode-plt-indirect-branch
```

所有 stdout / stderr 文件为空。

### 评分

- 实现效果：7/10。当前 Bench2 中已建模的 PLT stub `BRANCHIND` 被清掉，普通间接跳转仍保守保留。
- 理解成本：3/10。只加 direct `ram` GOT slot 到 `NativePltEntry` 的判断。
- 维护成本：3/10。后续如果要处理 `.plt.got` 缺口或 PLT0，需要补 `RelocationPltAnalyzer` 的建模，不应堆到这个判断里。
