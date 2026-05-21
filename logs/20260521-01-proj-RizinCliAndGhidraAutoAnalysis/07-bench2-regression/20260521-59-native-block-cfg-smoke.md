# Native Block CFG Smoke

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

上一小块已经让 native CFG 在已解码 direct branch target 处切开 block，并在追加同函数 block 时截断重叠范围。
但这个性质现在只靠手动检查，后续改 recursive decode 时容易退回。

这次补 Bench2 smoke 门槛：每个目标的 `--blocks-json` 必须满足同函数 block 不重叠，且 successor 不能指向同函数已知 block 的内部。

## Ghidra 相关实现

Ghidra 的 basic block 模型不会允许一个函数内的基本块互相覆盖，也不会把 branch target 隐藏在 block 中间：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/block/BasicBlockModel.java`
  - 按 flow reference、fallthrough 和 entry point 形成 basic block 边界。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/block/CodeBlock.java`
  - 用地址集合表达一个 block 的范围。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/block/CodeBlockReference.java`
  - 表达 block 间 flow 引用。

native 侧现在只有轻量 `NativeBasicBlock`，没有 Ghidra 的完整地址集合模型，所以先用 Bench2 smoke 守两个最基本的不变量。

## native 侧复刻策略

1. `bench2-native-smoke.sh` 在 summary 之后额外生成 `<target>.blocks.json`。
2. 用 Python 读取 `--blocks-json` 和 `--summary-json`。
3. 检查每个函数内 block 两两不重叠。
4. 检查 successor 如果落在同函数某个 block 范围内，必须等于那个 block 的 start。
5. 检查 blocks 数量和 summary `basic_blocks` 一致。

暂时不做：

- 不检查跨函数 successor。
- 不要求所有 successor 都已有 block，因为 bounded decode 仍可能只记录未解码 target。
- 不把这个检查推广到全量 Bench2 manifest。

## 判断标准

1. `bash -n scripts/bench2-native-smoke.sh` 通过。
2. 三个 Bench2 目标 smoke 通过，并生成 `*.blocks.json`。
3. `metrics.tsv` 仍正常输出。

## 风险

1. 这是 smoke 级别 CFG 不变量，不证明完整函数边界正确。
2. Python JSON 检查会让每个目标多跑一次 discovery，但当前三个目标开销可接受。

## 实现记录

已完成 Bench2 native block CFG smoke 检查。

修改点：

- `scripts/bench2-native-smoke.sh:187` 增加 `check_block_cfg(...)`。
- `scripts/bench2-native-smoke.sh:192` 的 Python 检查读取 summary JSON 和 blocks JSON。
- `scripts/bench2-native-smoke.sh:202` 检查 `blocks-json count` 和实际数组长度一致。
- `scripts/bench2-native-smoke.sh:207` 检查 `summary basic_blocks` 和 blocks JSON 数量一致。
- `scripts/bench2-native-smoke.sh:216` 按 `function_entry` 分组检查同函数 block 不重叠。
- `scripts/bench2-native-smoke.sh:230` 检查 successor 不指向同函数已知 block 的内部。
- `scripts/bench2-native-smoke.sh:377` 为每个目标保存 `<name>.blocks.json` 和 `<name>.blocks.stderr`。
- `scripts/bench2-native-smoke.sh:399` 在 summary 和入口 source baseline 之后运行 `--blocks-json` 并调用 `check_block_cfg(...)`。
- `ARCHITECTURE.md:155` 记录 Bench2 smoke 的 block CFG 不变量检查。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:84` 更新 Stage 7 进度。

验证：

```bash
bash -n scripts/bench2-native-smoke.sh
git diff --check
```

结果：通过。

Bench2 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-59
```

结果：通过，并生成：

```text
/tmp/notdec-bin2llvm-bench2-smoke-20260521-59/vsftpd.blocks.json
/tmp/notdec-bin2llvm-bench2-smoke-20260521-59/libuv.blocks.json
/tmp/notdec-bin2llvm-bench2-smoke-20260521-59/memcached.blocks.json
```

指标：

```text
target    elapsed_seconds  confirmed_functions  basic_blocks  instructions  xrefs_total  unresolved_total
vsftpd    10               9                    30            80            304          0
libuv     21               9                    29            85            26           0
memcached 11               9                    30            80            186          0
```

性能：每个目标多跑一次 `notdec-native-discover --blocks-json`，smoke 总耗时从上一轮约 34s 增到约 42s。这个开销只在 smoke 里发生，不影响 native 工具默认行为。

评分：

- 实现效果：8/10。能长期防止 native block 范围重叠和 successor 指向 block 内部的回退。
- 复杂度：8/10。检查逻辑集中在一个函数里，没有改 discovery。
- 维护成本：8/10。后续如果 block schema 扩展，只需调整这个 smoke 检查。
