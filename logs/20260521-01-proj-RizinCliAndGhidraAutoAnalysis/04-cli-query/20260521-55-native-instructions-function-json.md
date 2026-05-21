# Native Instructions Function JSON

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

上一小步补了 `--instructions-range-json`，可以按地址范围查 instruction。Stage 4 还要求能看函数。
这次补最小函数入口查询：

```text
notdec-native-discover --instructions-function-json <entry> <elf-file>
```

它只查 confirmed function 的 `[RangeStart, RangeEnd)`，不做额外函数发现。

## Ghidra 相关实现

Ghidra 的函数视图也是从 Function body / Listing 读已有 Program state：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/FunctionManager.java`
  - `getFunctionAt(Address)` 根据入口查函数。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Function.java`
  - `getBody()` 返回函数地址集。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Listing.java`
  - `getInstructions(AddressSetView, boolean)` 根据地址集遍历 instruction。

native 侧不建完整 AddressSet；先用 `NativeFunction::RangeStart/RangeEnd` 和已有
`instructionsInRange(...)` 输出函数范围内的 accepted instruction。

## native 侧复刻策略

1. 增加 `--instructions-function-json <entry> <elf-file>`。
2. 用 `NativeProgramState::functionAt(entry)` 查 confirmed function。
3. 命中时输出 `found=true`、函数 entry/range/name/source、instructions 和 count。
4. 未命中时输出 `found=false`、空 instructions 和 count 0。
5. 不对未知 entry 做 `functionContaining(...)` 或 speculative decode。

暂时不做：

- 不支持函数名查询。
- 不用 basic block 集合拼地址集。
- 不输出 P-Code 或 operands。

## 判断标准

1. `vsftpd --instructions-function-json 0x5000` 输出合法 JSON，并返回 `_init` 指令。
2. 未确认入口输出合法 JSON 且 `found=false`。
3. 原有 range 查询和 Bench2 smoke 继续通过。

## 风险

1. 当前函数范围是保守 decoded range，不是完整 Ghidra Function body。
2. 如果一个函数有多个非连续 block，当前输出会按 range 包含已接受 instruction；后续可以改为按 block 集合输出。

## 实现记录

### 改动

- `tools/notdec-native-discover.cpp:19` 的 `OutputMode` 增加 `InstructionsFunctionJson`。
- `tools/notdec-native-discover.cpp:34` 的 `CliOptions` 增加 `QueryFunctionEntry`。
- `tools/notdec-native-discover.cpp:43` 的 usage 增加
  `--instructions-function-json <entry> <elf-file>`。
- `tools/notdec-native-discover.cpp:99` 的 `parseArgs(...)` 支持三参数函数 instruction 查询。
- `tools/notdec-native-discover.cpp:429` 新增 `printInstructionsFunctionJson(...)`：
  - 命中 confirmed function 时输出 `found=true`、函数 range/name/source 和 instruction 列表。
  - 未命中时输出 `found=false` 和空 instruction 列表。
- `tools/notdec-native-discover.cpp:543` 在 main 输出分发里接入函数 instruction 查询。
- `ARCHITECTURE.md:37` 和 `ARCHITECTURE.md:123` 记录新 CLI。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:38` 更新阶段 4 进度。

### 验证

格式检查：

```bash
git diff --check
```

结果：通过。

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
```

结果：通过。

函数查询：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --instructions-function-json 0x5000 \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool
```

结果：合法 JSON，`found=true`，返回 8 条 `_init` instruction，range 为 `0x5000` 到 `0x501b`。

未知入口：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --instructions-function-json 0x123456 \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool
```

结果：合法 JSON，`found=false`，`count=0`。

和 range 查询对比：

```text
/tmp/notdec-vsftpd-instr-func.json 8
/tmp/notdec-vsftpd-instr-range-55.json 8
```

Bench2 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-55
```

结果：

```text
vsftpd ok elapsed=7s
libuv ok elapsed=19s
memcached ok elapsed=8s
```

metrics：

```text
target	elapsed_seconds	confirmed_functions	basic_blocks	instructions	xrefs_total	xrefs_flow	xrefs_call	xrefs_data	xrefs_string	unresolved_total	unresolved_indirect_call	unresolved_indirect_branch
vsftpd	7	9	30	80	304	14	2	149	139	0	0	0
libuv	19	9	29	85	26	10	3	13	0	0	0	0
memcached	8	9	30	80	186	14	2	68	102	0	0	0
```

性能：函数查询复用已有 native analysis 和 `instructionsInRange(...)`，只增加最终输出过滤。
Bench2 smoke 时间和前几轮同口径一致，没有看到明显变慢。

### 评分

- 实现效果：7/10。可以直接按 confirmed function 入口查看 instruction，补上 Stage 4 的函数级排查能力。
- 复杂度：2/10。只查 `functionAt(...)` 和已有 instruction range。
- 维护成本：2/10。输出结构和 range 查询共用 instruction list helper。

### 未做

- 没有函数名查询。
- 没有按 basic block 集合输出非连续 body。
- 没有输出 P-Code 或 operands。
