# /goal: Bin2llvm Native Rizin/Ghidra AutoAnalysis Alignment

## 原始用户需求

```text
在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis里面根据划分的几个关键宏观步骤，创建对应的文件夹，然后在prompt中要求，对应步骤的规划和修改必须放到对应的文件夹里。

本次目标是按照规划，完善bin2llvm项目反汇编转IR的native链路（即不走GhidraScript的链路，而是走libsla，libdecomp的这个链路），从Ghidra和rizin那边复刻足够多的功能。每当实现一部分功能后，就看看能否利用bench2里面这些项目去做初步的测试： （vsftpd可执行文件，libuv动态链接库，memcached可执行文件），测试实现是否正确。规划中每一部分都完成后即可停止。在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis 下创建一个PROGRESS.md里追踪计划的实现。完成一个部分后，同步更新 项目顶层的ARCHITECTURE.md 反映实现的功能模块的架构。

约束：根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## 目标

1. 按照规划完善 bin2llvm 反汇编转 IR 的 native 链路。
2. 把 native 链路拆成 7 个阶段，并把每个阶段的规划和实现记录放到对应目录。
3. 用 `PROGRESS.md` 跟踪完成情况。
4. 每完成一阶段，同步更新 `ARCHITECTURE.md`。
5. 先对齐 Ghidra / rizin 的关键能力，再用 Bench2 里的 `vsftpd`、`libuv`、`memcached` 做分段验证。

## 规则

1. 任何新工作先落到对应阶段目录里的规划文件。
2. 规划文件先写 Ghidra 相关源码文件和关键函数。
3. 再写可复刻策略，优先围绕当前测试用例需要的最小能力。
4. 最后才做实现，并把结果回写到同一规划文件。
5. 不做超出当前阶段的功能。
6. 不把猜测性逻辑塞进 CFG、xref、函数发现里。

## 阶段目录

- `01-state-skeleton`
- `02-entry-discovery`
- `03-recursive-disassembly-cfg`
- `04-cli-query`
- `05-xref-enhancement`
- `06-lowering-integration`
- `07-bench2-regression`

## 当前顺序

1. 先补 `PROGRESS.md` 和 `ARCHITECTURE.md` 的骨架。
2. 再按阶段目录补各自的规划文件。
3. 后续实现时，只改对应阶段目录内的文件。
