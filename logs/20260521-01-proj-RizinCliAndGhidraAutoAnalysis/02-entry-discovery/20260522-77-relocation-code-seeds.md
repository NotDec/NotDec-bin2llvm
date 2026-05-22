# Relocation Code Seeds

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

native 现在已经能把 relocated pointer 指向 executable address 的引用记成：

```text
kind=flow, source=elf-relocation-code
```

但这些 code pointer target 还没有进入 function seed。比如 init/fini array 和函数指针表都能提供真实代码入口，
只把它们当 xref 不够。

这次补：

- executable relocation target 作为低可信 function seed。
- source 仍为 `elf-relocation-code`。

## Ghidra 相关实现

Ghidra 会从 relocation、data reference 和入口分析里发现代码入口：

- `Ghidra/Processors/x86/src/main/java/ghidra/app/util/bin/format/elf/relocation/X86_64_ElfRelocationHandler.java`
  - `relocate(...)` 计算 relocation target。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java`
  - `addMemoryReference(...)` 记录 relocation 形成的 memory reference。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/EntryPointAnalyzer.java`
  - `analyze(...)` 从入口和符号继续触发反汇编。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/FunctionStartAnalyzer.java`
  - 结合 code reference 和函数起始模式确认函数。

rizin 侧类似 `aad` / `aav` / `aar` 会从 data references 和 section 指针里找 code refs，再配合
`af` / `afr` 形成函数。

## native 侧复刻策略

1. 在 `SleighSeedInstructionAnalyzer` 开始前，从 `state.relocatedPointers()` 找 executable target。
2. 对这些 target 调 `addFunctionSeed(..., "elf-relocation-code", Low)`。
3. 不给 size/range，避免误造函数边界。
4. 追加发生在 entry/symbol/eh-frame analyzer 之后，所以不会抢掉已有高可信 seed 的 worklist 顺序。
5. 继续由 bounded decode 决定是否消费这些低可信 seed。

暂时不做：

- 不把所有 relocation code seed 都强制 decode。
- 不猜 jump table。
- 不把 relocation slot 当 callsite。

## 判断标准

1. `notdec-native-discover` 和 `notdec-native-llvm` 能构建。
2. 三个 Bench2 目标 `--seeds-json` 都能看到 `elf-relocation-code` source。
3. 完整 Bench2 smoke 继续通过。
4. 当前 confirmed function / instruction 数量不能因为低可信 seed 抢队而下降。

## 风险

1. 有些 executable pointer 可能是函数内部标签或 thunk，不一定是真函数入口，所以只给 Low confidence。
2. 如果后续提高 decode 预算，低可信 seed 会被更多消费，需要再看函数边界质量。

## 实现记录

### 修改范围

1. `lib/NativeAnalysis.cpp`
   - 第 1312 行附近：`SleighSeedInstructionAnalyzer::run(...)` 在建立 decode 队列前调用 `addRelocationCodeSeeds(...)`。
   - 第 1377 行附近：新增 `addRelocationCodeSeeds(...)`，遍历 `state.relocatedPointers()`。
   - 第 1380 行附近：只接受 `state.isExecutableAddress(target)`。
   - 第 1383 行附近：写入 `elf-relocation-code` / `Low` function seed，不设置 size/range。
2. `ARCHITECTURE.md`
   - 第 17 行附近：`NativeFunctionSeed` 来源加入 executable relocation target。
   - 第 67 行附近：记录 Sleigh analyzer 开始前会追加低可信 `elf-relocation-code` seed。
3. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
   - 第 13 行附近：阶段 2 记录 relocation code seed 来源。

### 行为

`elf-relocation-code` 现在既是 xref source，也是 function seed source。
如果 target 已经有更高可信来源，`addFunctionSeed(...)` 只合并 source，不降低 confidence。
如果 target 是新地址，则作为低可信 seed 追加到 worklist 后面。

这次没有强制消费低可信 seed，也没有修改 decode 预算。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
git diff --check
```

结果：通过。

`--seeds-json` 专项检查：

```text
vsftpd: total seeds=187, seeds with elf-relocation-code=2
libuv: total seeds=485, seeds with elf-relocation-code=3
memcached: total seeds=259, seeds with elf-relocation-code=33
```

`--summary-json` source 计数：

```text
vsftpd: elf-relocation-code=2
libuv: elf-relocation-code=3
memcached: elf-relocation-code=50
```

完整 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260522-77
```

结果：

```text
vsftpd ok elapsed=23s
libuv ok elapsed=29s
memcached ok elapsed=17s
```

metrics：

```text
target	elapsed_seconds	confirmed_functions	basic_blocks	instructions	xrefs_total	xrefs_flow	xrefs_call	xrefs_data	xrefs_string	unresolved_total	unresolved_indirect_call	unresolved_indirect_branch
vsftpd	23	13	38	126	315	19	4	153	139	0	0	0
libuv	29	11	28	91	23	12	3	8	0	0	0	0
memcached	17	11	30	86	187	63	2	20	102	0	0	0
```

confirmed function / instruction 数量没有下降，说明新增低可信 seed 没有抢掉当前高可信 decode 预算。

### 评分

- 实现效果：7/10。relocation code pointer 现在进入入口发现体系，但仍保守，不强制 decode。
- 复杂度：2/10。只在现有 analyzer 里追加 seed 来源。
- 维护成本：3/10。后续提高 decode 预算或加入优先级队列时，需要继续关注 Low seed 的消费顺序。
