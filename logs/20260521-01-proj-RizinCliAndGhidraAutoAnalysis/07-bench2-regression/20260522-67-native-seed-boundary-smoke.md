# Native Seed Boundary Smoke

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

上一小块已经让无 range seed 的 decode 被后续最近的 function seed 截断。
但 Bench2 smoke 还没有直接检查这个边界性质。

这次补一个回归门槛：confirmed block 不能覆盖其他已知 function seed 入口。
这能防止后续 recursive decode 又把相邻函数入口吞进当前 block。

## Ghidra 相关实现

Ghidra 会把函数入口作为函数体边界的重要信号：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/FunctionManager.java`
  - 维护函数入口和函数体。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/function/FunctionStartAnalyzer.java`
  - 发现函数起点。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/disassembler/DisassembleCommand.java`
  - 从入口反汇编时不会无约束覆盖已有函数入口。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/block/BasicBlockModel.java`
  - basic block 边界来自 flow、fallthrough 和入口点。

rizin 的 `RzAnalysisFunction` / `RzAnalysisBlock` 也会把已知函数入口和 block 起点拆开，`afl`、`afb`、`agf` 查询依赖这个不变量。

## native 侧复刻策略

1. Bench2 smoke 额外保存 `<target>.seeds.json`。
2. 用 Python 读取 `--seeds-json` 和 `--blocks-json`。
3. 对每个 confirmed block 检查：如果某个 seed address 落在 `[block.start, block.end)` 内部，必须等于当前函数入口。
4. 继续保留已有 block 不重叠和 successor 不指向 block 内部检查。

暂时不做：

- 不要求每个 seed 都已经 confirmed。
- 不检查跨函数 direct flow 是否完整。
- 不把这个 smoke 扩大到全 Bench2 manifest。

## 判断标准

1. `bash -n scripts/bench2-native-smoke.sh` 通过。
2. 三个 Bench2 目标 smoke 通过。
3. 三个目标都生成 `<name>.seeds.json`。
4. `metrics.tsv` 继续输出，并且 unresolved indirect call / branch 仍为 0。

## 风险

1. 如果未来引入 thunk/alias，让多个函数入口共享同一个 block，需要重新定义这个 smoke。
2. 这是边界回归检查，不代表完整函数边界恢复完成。

## 实现记录

### 修改范围

1. `scripts/bench2-native-smoke.sh`
   - 第 242 行附近：新增 `check_seed_boundaries(...)`。
   - 第 247 行附近：Python 读取 seeds JSON 和 blocks JSON。
   - 第 257 行附近：检查 seeds JSON 的 `count` 和数组长度一致。
   - 第 263 行附近：逐个 confirmed block 检查是否覆盖其他 seed address。
   - 第 416 行附近：每个目标新增 `<name>.seeds.json` 和 `<name>.seeds.stderr`。
   - 第 440 行附近：调用 `notdec-native-discover --seeds-json`。
   - 第 443 行附近：在 block CFG 检查后调用 seed 边界检查。
2. `ARCHITECTURE.md`
   - 第 169 行附近：Bench2 smoke 说明加入 `--seeds-json` 边界检查。
3. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
   - 阶段 7 增加 function seed 边界检查完成项。

### 行为

Bench2 smoke 现在会保存每个目标的 seeds JSON。
如果某个 confirmed block 的 `[start, end)` 内部包含其他 function seed 入口，就直接失败。

这次只加回归检查，不改变 native discovery 和 lowering。

### 验证

```bash
bash -n scripts/bench2-native-smoke.sh
git diff --check
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260522-67
```

结果：通过。

Bench2 smoke：

```text
vsftpd ok elapsed=12s
libuv ok elapsed=23s
memcached ok elapsed=12s
```

生成：

```text
/tmp/notdec-bin2llvm-bench2-smoke-20260522-67/vsftpd.seeds.json
/tmp/notdec-bin2llvm-bench2-smoke-20260522-67/libuv.seeds.json
/tmp/notdec-bin2llvm-bench2-smoke-20260522-67/memcached.seeds.json
```

性能和规模：

```text
target	elapsed_seconds	confirmed_functions	basic_blocks	instructions	xrefs_total	xrefs_flow	xrefs_call	xrefs_data	xrefs_string	unresolved_total	unresolved_indirect_call	unresolved_indirect_branch
vsftpd	12	9	28	75	303	13	2	149	139	0	0	0
libuv	23	9	26	80	23	9	3	11	0	0	0	0
memcached	12	9	28	75	179	13	2	62	102	0	0	0
```

和上一轮相比，每个目标多跑一次 `--seeds-json`，smoke 耗时略增。
这是测试脚本开销，不影响 native 工具默认路径。

### 评分

- 实现效果：7/10。能防止 confirmed block 再覆盖其他已知函数入口。
- 复杂度：2/10。只加一个 JSON 检查函数。
- 维护成本：2/10。依赖现有 `--seeds-json` / `--blocks-json` schema，后续 schema 变更时同步即可。
