# Native CFG JSON

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

native discovery 已经有 confirmed function、basic block 和 successor。
`--blocks-json` 能列出全量 block，但还不能像 rizin 的函数图命令那样按某个函数入口直接看 CFG。

这次补：

```text
notdec-native-discover --cfg-json <entry> <elf-file>
```

## Ghidra 相关实现

Ghidra 的函数 CFG 也是从 Program 里的函数 body 和 code block 派生：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Function.java`
  - `getEntryPoint()` 和 `getBody()` 表达函数入口和函数体。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/functiongraph/FunctionGraphPlugin.java`
  - 展示单个函数的 block 图。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/blockmodel/BasicBlockModel.java`
  - 基于 listing 和 flow reference 构建 basic block 视图。

rizin 对应的是 `agf` / `agF`，底层使用 `RzAnalysisFunction` 和 `RzAnalysisBlock`。

## native 侧复刻策略

1. `notdec-native-discover` 增加 `--cfg-json <entry> <elf-file>`。
2. 用 `NativeProgramState::functionAt(entry)` 精确查 confirmed function。
3. 输出函数 entry、range、name、source 和 block 列表。
4. 每个 block 输出 start、end、size、successors。

暂时不做：

- 不输出 DOT。
- 不按地址模糊匹配所属函数。
- 不补全当前尚未恢复的间接边。
- 不把 CFG query 接入函数发现。

## 判断标准

1. 三个 Bench2 目标任选一个 confirmed function entry，输出合法 JSON。
2. `found=true` 时 `count == len(blocks)`，且 `count` 等于函数的 `block_count`。
3. 不存在的 entry 输出 `found=false`。
4. 原有 Bench2 smoke 继续通过。

## 风险

1. 当前 CFG 只反映 native 已确认 block 和 successor，不代表完整函数语义。
2. 函数边界仍不完整，部分 Bench2 函数 range 后续还需要继续收敛。

## 实现记录

### 修改范围

1. `tools/notdec-native-discover.cpp`
   - 第 19 行附近：`OutputMode` 增加 `CfgJson`。
   - 第 52 行附近：usage 增加 `--cfg-json <entry> <elf-file>`。
   - 第 136 行附近：`parseArgs` 接受 `--cfg-json`，复用 `QueryFunctionEntry`。
   - 第 596 行附近：新增 `printCfgJson(...)`，按 exact confirmed function entry 输出函数信息和 block successor。
   - 第 964 行附近：`main` 分发到 `printCfgJson(...)`。
2. `ARCHITECTURE.md`
   - 第 42 行附近：CLI query 说明加入 `--cfg-json <entry>`。
   - 第 135 行附近：目录说明加入单函数 CFG JSON。
3. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
   - 阶段 4 增加 `--cfg-json` 完成项。

### 行为

`notdec-native-discover --cfg-json <entry> <elf-file>` 只查精确入口。
找到 confirmed function 时输出 entry、range、name、source、blocks、successors 和 count。
找不到时输出 `found=false`、空 blocks 和 `count=0`。

这次没有新增 CFG 边，也没有按地址猜所属函数。

### 验证

```bash
git diff --check
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
```

结果：通过。

Bench2 专项检查：

```text
vsftpd entry=0x5000 blocks=3 missing_found=False
libuv entry=0x8000 blocks=3 missing_found=False
memcached entry=0x5000 blocks=3 missing_found=False
```

完整 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260522-65
```

结果：

```text
vsftpd ok elapsed=11s
libuv ok elapsed=22s
memcached ok elapsed=10s
```

性能和规模：

```text
target	elapsed_seconds	confirmed_functions	basic_blocks	instructions	xrefs_total	xrefs_flow	xrefs_call	xrefs_data	xrefs_string	unresolved_total	unresolved_indirect_call	unresolved_indirect_branch
vsftpd	11	9	30	80	304	14	2	149	139	0	0	0
libuv	22	9	29	85	26	10	3	13	0	0	0	0
memcached	10	9	30	80	186	14	2	68	102	0	0	0
```

这次只是查询输出，未改变 discovery 或 lowering，Bench2 当前用例没有看到性能退化。

### 评分

- 实现效果：8/10。能按函数入口直接看 CFG，满足当前 CLI 查询需求。
- 复杂度：2/10。只新增一个 formatter 和参数分发。
- 维护成本：2/10。复用现有 state，后续如果 block 字段扩展，只需同步 formatter。
