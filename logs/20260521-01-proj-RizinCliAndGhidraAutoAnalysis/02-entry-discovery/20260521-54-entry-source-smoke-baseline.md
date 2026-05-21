# Entry Source Smoke Baseline

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

Stage 2 要求入口发现能解释 seed 来源，并且 shared object 的 `e_entry = 0` 不作为有效函数。
当前 `notdec-native-discover --summary-json` 已经输出 `sources`：

- `vsftpd` / `memcached` 有 `elf-entry`、`dt-init`、`dt-fini`、init/fini array、`eh-frame`。
- `libuv.so.1.0.0` 是 shared object，没有 `elf-entry`，但有 dynamic/static symbol 和 `eh-frame`。

这些事实还没有进入 Bench2 smoke，容易回退后只靠函数数不容易看出来。

## Ghidra 相关实现

Ghidra 的入口发现来自多个 analyzer 和 ELF 信息：

- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/EntryPointAnalyzer.java`
  - 负责把入口点、符号等信息转成函数入口。
- `Ghidra/Features/Base/src/main/java/ghidra/app/util/opinion/ElfProgramBuilder.java`
  - ELF loader 会处理 dynamic table、init/fini array、entry point 和 relocation 相关信息。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/FunctionStartAnalyzer.java`
  - 结合已知入口和函数起始模式继续发现函数。

native 侧不用 Ghidra ProgramDB，只固定当前 Bench2 需要的高可信入口来源，并把 source baseline
写进 smoke。

## native 侧复刻策略

1. 在 `bench2-native-smoke.sh` 里解析 summary JSON 的 `sources` 数字。
2. 三个目标都要求有 `dt-init`、`dt-fini`、`dt-init-array`、`dt-fini-array`、`eh-frame`。
3. `vsftpd` / `memcached` 要求有 `elf-entry`。
4. `libuv` 要求没有 `elf-entry`，同时要求有 `elf-dynamic-symbol` 和 `elf-symbol`。
5. 只检查来源存在，不把具体数量作为硬门槛，避免因为包版本或 Ghidra/LLVM 细节改变而过度脆弱。

暂时不做：

- 不做 prelude scan。
- 不把 PLT stub 当内部函数 seed。
- 不新增低可信 speculative seed。

## 判断标准

1. Bench2 smoke 能挡住入口 source 明显回退。
2. shared object `libuv` 继续没有 `elf-entry` source。
3. 原有 native LLVM verify、IR pattern、metrics 和 heritage compare 继续通过。

## 风险

1. source baseline 是当前 Bench2 rootfs 的事实；换二进制版本时可能需要更新。
2. 这只验证 source 可观测性，不代表函数边界已经完整。

## 实现记录

### 改动

- `scripts/bench2-native-smoke.sh:99` 增加 `summary_source_count(...)`，从 summary JSON 的
  `sources` 中取某个 source 的计数。
- `scripts/bench2-native-smoke.sh:106` 增加 `require_summary_source(...)`。
- `scripts/bench2-native-smoke.sh:118` 增加 `forbid_summary_source(...)`。
- `scripts/bench2-native-smoke.sh:270` 增加 `check_entry_sources(...)`：
  - 三目标都要求 `dt-init`、`dt-fini`、`dt-init-array`、`dt-fini-array`、`eh-frame`。
  - `vsftpd` / `memcached` 要求 `elf-entry`。
  - `libuv` 禁止 `elf-entry`，并要求 `elf-dynamic-symbol` 和 `elf-symbol`。
- `scripts/bench2-native-smoke.sh:341` 在 discovery summary 检查后调用 `check_entry_sources(...)`。
- `ARCHITECTURE.md:147` 记录 smoke 现在检查入口 source baseline。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:11` 更新阶段 2 进度。

### 验证

格式和脚本语法：

```bash
git diff --check
bash -n scripts/bench2-native-smoke.sh
```

结果：通过。

Bench2 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-54
```

结果：

```text
vsftpd ok elapsed=8s
libuv ok elapsed=19s
memcached ok elapsed=7s
```

当前 source baseline：

```text
vsftpd: dt-fini=1, dt-fini-array=1, dt-init=1, dt-init-array=1, eh-frame=182, elf-entry=1
libuv: dt-fini=1, dt-fini-array=2, dt-init=1, dt-init-array=1, eh-frame=480, elf-dynamic-symbol=307, elf-symbol=307, no elf-entry
memcached: dt-fini=1, dt-fini-array=1, dt-init=1, dt-init-array=1, eh-frame=254, elf-entry=1
```

metrics：

```text
target	elapsed_seconds	confirmed_functions	basic_blocks	instructions	xrefs_total	xrefs_flow	xrefs_call	xrefs_data	xrefs_string	unresolved_total	unresolved_indirect_call	unresolved_indirect_branch
vsftpd	8	9	30	80	304	14	2	149	139	0	0	0
libuv	19	9	29	85	26	10	3	13	0	0	0	0
memcached	7	9	30	80	186	14	2	68	102	0	0	0
```

性能：本次 smoke 合计约 34s，和前几轮同口径一致。新增 source 检查只是 `sed` 解析 summary，
没有看到明显变慢。

### 评分

- 实现效果：7/10。入口来源的关键 Bench2 行为现在有 smoke 保护。
- 复杂度：2/10。只解析现有 summary JSON，不改 native analysis。
- 维护成本：3/10。source 名称是稳定接口，但具体 baseline 仍跟 Bench2 rootfs 版本绑定。

### 未做

- 没有新增入口发现策略。
- 没有把 PLT stub 当内部 function seed。
- 没有做 prelude scan 或低可信 speculative seed。
