# Branch Target Block Split

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

Stage 3 已经能按 `CBRANCH` / `BRANCH` / `BRANCHIND` / `RETURN` 结束 basic block，也能递归消费一部分 direct branch successor。
但当前 block 切分只看“哪条指令结束 block”，没有把已解码范围内的 direct branch target 当作 block 起点。
如果一个条件跳转跳到同一段已解码指令的中间，旧逻辑可能让 target 落在某个 block 内部。

这次补：已解码前缀内的 direct branch target 如果正好是已解码指令地址，就强制成为新 block 起点。

## Ghidra 相关实现

Ghidra 的 block/body 计算会把 flow target 作为边界：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/block/BasicBlockModel.java`
  - 基于 flow reference 和 fallthrough 划分 basic block，入口和被引用目标都会成为 block 边界。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/block/CodeBlockReferenceIterator.java`
  - 遍历 block 之间的引用。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/block/FollowFlow.java`
  - 从入口跟随 flow target 和 fallthrough，避免把跳入点藏在 block 中间。

native 侧已有 `DecodedFlowInfo::BranchTargets`，只需要在构造 `NativeBasicBlock` 时把本地 direct target 纳入 block starts。

## native 侧复刻策略

1. 在 `buildDecodedBlocks(...)` 里先收集本次已解码的 instruction address。
2. 收集 `rangeStart`、terminator 后下一条指令、以及落在已解码 instruction address 上的 direct branch target。
3. 线性生成 block 时，如果当前指令不是 terminator，但下一条指令是 block start，也结束当前 block。
4. 不把 call target 当作当前函数 block start。
5. 不解析 indirect branch / jump table。

## 判断标准

1. 三个 Bench2 目标的 `--blocks-json` 合法。
2. 不存在 successor 指向某个 block 内部但不是 block start 的情况。
3. Bench2 smoke 继续通过。

## 风险

1. 只处理本次已解码指令地址，不猜未解码 target。
2. 这不会让函数边界变完整，只是让当前有限前缀的 block 边界更正确。

## 实现记录

已完成已解码 direct branch target 的 block 起点切分。

实现时发现一个补充问题：后续 branch target decode 会向同一个 function 追加 block，如果新旧 block 重叠，旧逻辑只过滤完全相同的 block，会留下同函数重叠范围。因此这次同时收紧追加 block 的重叠处理。

修改点：

- `lib/NativeAnalysis.cpp:1786`，`SleighSeedInstructionAnalyzer::buildDecodedBlocks(...)` 先收集本轮已解码 instruction start。
- `lib/NativeAnalysis.cpp:1791`，同函数里收集 block starts：`rangeStart`、已解码 direct branch target、terminator 后的下一条指令。
- `lib/NativeAnalysis.cpp:1818`，如果下一条指令是 block start，当前 block 也结束。
- `lib/NativeAnalysis.cpp:1843`，非 terminator 因 block start 被切开时，补 fallthrough successor。
- `lib/NativeAnalysis.cpp:2421`，`NativeProgramState::addBasicBlock(...)` 追加 block 时处理同函数重叠：相同 start 取更短范围，block 插入已有范围内部时截断旧 block，新 block 覆盖已有起点时截断新 block。
- `ARCHITECTURE.md:83` 记录 branch target block split 和同函数 block 重叠截断。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:13` 更新 Stage 3 进度。

专项验证：

```bash
git diff --check
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
```

结果：通过。

```bash
for target in vsftpd libuv memcached; do
  notdec-native-discover --blocks-json "$elf" > "$blocks"
  notdec-native-discover --summary-json "$elf" > "$summary"
  python3 - "$blocks" "$summary" "$target" <<'PY'
  ...
PY
done
```

检查内容：

- 每个函数内 block 不互相重叠。
- 每个 successor 如果落在同函数已知 block 内，必须等于 block start。
- `--blocks-json` 数量和 `--summary-json basic_blocks` 一致。

结果：

```text
vsftpd blocks=30 successors=21
libuv blocks=29 successors=21
memcached blocks=30 successors=21
```

Bench2 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-58
```

结果：通过。

```text
target    elapsed_seconds  confirmed_functions  basic_blocks  instructions  xrefs_total  unresolved_total
vsftpd    7                9                    30            80            304          0
libuv     19               9                    29            85            26           0
memcached 8                9                    30            80            186          0
```

性能：没有增加 Sleigh decode 次数，只在已有指令和 block 列表上做集合查找和同函数重叠截断。Bench2 smoke 耗时保持在同一量级。

评分：

- 实现效果：8/10。已解码范围内的 direct branch target 不再藏在同函数 block 中间。
- 复杂度：7/10。多了一点 block 追加时的重叠收敛逻辑，但仍局限在 `NativeProgramState::addBasicBlock(...)`。
- 维护成本：7/10。后续做完整函数边界时，可能需要把 block normalization 抽成更完整的 CFG builder。
