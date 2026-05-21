# Native Instructions Range JSON

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

Stage 4 已有 `--instructions-json` 全量导出，但排查函数或 block 时还要手工过滤。总计划里
Stage 4 要求能“反汇编地址、block、函数”。这次先补最小地址范围查询：

```text
notdec-native-discover --instructions-range-json <start> <end> <elf-file>
```

它只读现有 `NativeProgramState::instructionsInRange(...)`，不改变分析结果。

## Ghidra 相关实现

Ghidra 的 Listing / CodeUnit 查询都是按地址范围读已经分析好的 Program：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Listing.java`
  - `getInstructions(AddressSetView, boolean)` / `getInstructions(Address, boolean)` 用于按地址集或地址遍历指令。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/InstructionIterator.java`
  - 表达指令迭代结果。
- `Ghidra/Features/Base/src/main/java/ghidra/app/util/viewer/listingpanel/ListingPanel.java`
  - UI 层也是从 Program listing 取地址范围内的 code unit 展示。

native 侧不做 UI，也不反复 decode；只把已有 instruction store 的范围查询接到 CLI。

## native 侧复刻策略

1. `notdec-native-discover` 增加 `--instructions-range-json <start> <end> <elf-file>`。
2. 解析两个地址，要求 `<start> <end>` 都支持 `0x...` 或十进制。
3. 调 `NativeProgramState::instructionsInRange(start, end)`。
4. 输出 `query.start`、`query.end`、`instructions[]`、`count`。
5. 复用全量 instruction JSON 的单项格式。

暂时不做：

- 不按函数名、block id 查询。
- 不做模糊查找，例如地址落在某条指令中间时反查 containing instruction。
- 不重新 decode 未进入 native state 的地址。

## 判断标准

1. `--instructions-range-json` 输出合法 JSON。
2. 对 Bench2 `vsftpd` 的 `_init` 范围能返回少量指令。
3. 全量 `--instructions-json` 和 Bench2 smoke 继续通过。
4. 性能不明显变慢。

## 风险

1. 当前 range 查询只看已接受的 native instruction；如果地址范围未被当前 seed 覆盖，会返回空。
2. 这是 CLI 可观测性增强，不直接提高 CFG 或 IR 语义。

## 实现记录

### 改动

- `tools/notdec-native-discover.cpp:19` 的 `OutputMode` 增加 `InstructionsRangeJson`。
- `tools/notdec-native-discover.cpp:33` 的 `CliOptions` 增加 `QueryStart` / `QueryEnd`。
- `tools/notdec-native-discover.cpp:41` 的 usage 增加
  `--instructions-range-json <start> <end> <elf-file>`。
- `tools/notdec-native-discover.cpp:69` 的 `parseArgs(...)` 支持五参数 range 查询。
- `tools/notdec-native-discover.cpp:358` 把 instruction 单项输出抽成 `printInstructionObject(...)`，
  全量和 range 输出共用。
- `tools/notdec-native-discover.cpp:403` 新增 `printInstructionsRangeJson(...)`，调用
  `NativeProgramState::instructionsInRange(start, end)`。
- `tools/notdec-native-discover.cpp:499` 在 main 输出分发里接入 range 模式。
- `ARCHITECTURE.md:34` 和 `ARCHITECTURE.md:121` 记录新 CLI。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:36` 更新阶段 4 进度。

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

range 查询：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --instructions-range-json 0x5000 0x5020 \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool
```

结果：合法 JSON，返回 8 条 `_init` 范围内的 instruction，包括 `ENDBR64`、`SUB RSP,0x8`、
`CALL RAX`、`RET`。

空范围：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --instructions-range-json 0x5000 0x5000 \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool
```

结果：合法 JSON，`count` 为 0。

全量 instruction JSON：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --instructions-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool
```

结果：合法 JSON，`count` 为 80。

Bench2 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-53
```

结果：

```text
vsftpd ok elapsed=8s
libuv ok elapsed=19s
memcached ok elapsed=7s
```

metrics：

```text
target	elapsed_seconds	confirmed_functions	basic_blocks	instructions	xrefs_total	xrefs_flow	xrefs_call	xrefs_data	xrefs_string	unresolved_total	unresolved_indirect_call	unresolved_indirect_branch
vsftpd	8	9	30	80	304	14	2	149	139	0	0	0
libuv	19	9	29	85	26	10	3	13	0	0	0	0
memcached	7	9	30	80	186	14	2	68	102	0	0	0
```

性能：range 查询和全量 instruction JSON 都复用现有 native analysis，只改变最终输出过滤。
Bench2 smoke 时间和前几轮同口径，没有看到明显变慢。

### 评分

- 实现效果：7/10。已经能按地址范围查看 accepted instruction，补上 Stage 4 的一块排查能力。
- 复杂度：2/10。只接已有 `instructionsInRange(...)`，没有新增分析逻辑。
- 维护成本：2/10。复用全量 instruction JSON 的单项输出，后续函数/block 过滤可继续沿用。

### 未做

- 没有按函数名或 block 查询 instruction。
- 没有做 containing instruction 模糊匹配。
- 没有重新 decode 未进入 native state 的地址。
