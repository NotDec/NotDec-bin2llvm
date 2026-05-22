# Relocation Code Seed Smoke

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

上一小块已经让 executable relocation target 进入 function seed，source 为
`elf-relocation-code`。当前 Bench2 三个目标的 summary source 数量是：

```text
vsftpd: 2
libuv: 3
memcached: 50
```

这次把这个来源纳入 Bench2 smoke 的 entry/source baseline。

## Ghidra 相关实现

Ghidra 会把 loader、relocation 和 code reference 共同用于入口发现：

- `Ghidra/Processors/x86/src/main/java/ghidra/app/util/bin/format/elf/relocation/X86_64_ElfRelocationHandler.java`
  - `relocate(...)` 计算 relocation target。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java`
  - `addMemoryReference(...)` 保存 relocation 形成的引用。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/EntryPointAnalyzer.java`
  - `analyze(...)` 消费入口点和符号来源。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/FunctionStartAnalyzer.java`
  - 从代码引用和已知入口继续确认函数。

rizin 侧类似 `aav` / `aad` 从数据指针找代码引用，再由函数分析命令消费。

## native 侧复刻策略

1. 在 `check_entry_sources(...)` 中要求三目标都有 `elf-relocation-code`。
2. 只检查非零，不固定具体数量。
3. 不新增 CLI，也不改变 native analysis。

## 判断标准

1. `bash -n scripts/bench2-native-smoke.sh` 通过。
2. 完整 Bench2 smoke 通过。
3. 如果 `elf-relocation-code` seed source 消失，smoke 会失败。

## 风险

1. 这个 baseline 绑定当前 Bench2 rootfs；换二进制版本时可能需要调整。
2. 它只证明 source 存在，不证明这些低可信 seed 已被 decode。

## 实现记录

### 修改范围

1. `scripts/bench2-native-smoke.sh`
   - 第 407 行附近：`check_entry_sources(...)` 增加 `elf-relocation-code` source 非零检查。
2. `ARCHITECTURE.md`
   - 第 184 行附近：记录 Bench2 smoke 会检查 `elf-relocation-code` seed source。
3. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
   - 第 102 行附近：阶段 7 记录 relocation code seed source baseline。

### 行为

Bench2 smoke 现在会要求三个目标 summary 里都有：

```text
"elf-relocation-code": <non-zero>
```

这只检查 source 存在，不固定数量。

### 验证

```bash
bash -n scripts/bench2-native-smoke.sh
git diff --check
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260522-78
```

结果：通过。

完整 smoke：

```text
vsftpd ok elapsed=22s
libuv ok elapsed=29s
memcached ok elapsed=17s
```

metrics：

```text
target	elapsed_seconds	confirmed_functions	basic_blocks	instructions	xrefs_total	xrefs_flow	xrefs_call	xrefs_data	xrefs_string	unresolved_total	unresolved_indirect_call	unresolved_indirect_branch
vsftpd	22	13	38	126	315	19	4	153	139	0	0	0
libuv	29	11	28	91	23	12	3	8	0	0	0	0
memcached	17	11	30	86	187	63	2	20	102	0	0	0
```

source baseline：

```text
vsftpd: elf-relocation-code=2
libuv: elf-relocation-code=3
memcached: elf-relocation-code=50
```

性能：本次只多一个 summary source 检查，不增加额外工具运行。

### 评分

- 实现效果：7/10。能挡住 relocation code seed source 回退。
- 复杂度：1/10。复用已有 source baseline helper。
- 维护成本：2/10。只检查非零，Bench2 版本轻微变化时不太脆。
