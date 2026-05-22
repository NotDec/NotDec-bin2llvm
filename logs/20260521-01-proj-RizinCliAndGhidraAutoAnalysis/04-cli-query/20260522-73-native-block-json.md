# Native Block JSON

## 原始 prompt

```text
Another language model started to solve this problem and produced a summary of its thinking process. You also have access to the state of the tools that were used by that language model. Use this to build on the work that has already been done and avoid duplicating work.
```

## 当前目标和已有 native 状态

native CLI 已经有 `--blocks-json` 全量 block 列表、`--cfg-json <entry>` 单函数 CFG、
`--instructions-range-json <start> <end>` 范围指令查询。

这次补：

```text
notdec-native-discover --block-json <start> <elf-file>
```

它按 basic block 起点精确查询一个 block，并带出该 block 内的 instruction。

## Ghidra 相关实现

Ghidra 的 block 和指令查询主要来自 listing 与 block model：

- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/blockmodel/BasicBlockModel.java`
  - 构造 code block，按控制流边界切分基本块。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/block/CodeBlock.java`
  - 表达 block 起止范围和 destination。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Listing.java`
  - `getInstructions(...)` 按地址集合遍历指令。

rizin 侧对应 `pdb` 查看当前 basic block，`afb` / `afbj` 查看函数 basic block。

## native 侧复刻策略

1. `notdec-native-discover` 增加 `--block-json <start> <elf-file>`。
2. 用现有 confirmed function / basic block state，按 `NativeBasicBlock::Start` 精确匹配。
3. 命中后输出 query、found、function_entry、block 起止、size、successors。
4. 用 `NativeProgramState::instructionsInRange(block.Start, block.End)` 输出 block 内 instruction。
5. 未命中时输出 `found=false`、空 instruction 列表和 `instruction_count=0`。

暂时不做：

- 不做 containing-block 模糊查询。
- 不改变 basic block 切分、CFG、xref 或 lowering。
- 不猜 indirect branch/call 目标。

## 判断标准

1. `notdec-native-discover` 能编译。
2. 三个 Bench2 目标的已知 block 起点查询 `found=true`，且 `instruction_count > 0`。
3. 缺失地址查询 `found=false`，输出合法 JSON。
4. 原有 Bench2 smoke 继续通过，性能指标不退化。

## 风险

1. 当前 confirmed block 仍受 native discovery 边界限制，查询结果可能漏掉尚未确认的 block。
2. 精确起点查询会让 block 内部地址返回未命中，这是有意选择，避免隐藏 CFG 边界问题。

## 实现记录

### 修改范围

1. `tools/notdec-native-discover.cpp`
   - 第 19 行附近：`OutputMode` 增加 `BlockJson`。
   - 第 58 行附近：usage 增加 `--block-json <start> <elf-file>`。
   - 第 149 行附近：`parseArgs(...)` 识别 `--block-json`，并把参数放入 `QueryAddress`。
   - 第 703 行附近：新增 `printBlockJson(...)`，按 `NativeBasicBlock::Start` 精确匹配 block。
   - 第 1290 行附近：`main(...)` 分发到 `printBlockJson(...)`。
2. `ARCHITECTURE.md`
   - 第 48 行附近：CLI query 总述加入 `--block-json <start>`。
   - 第 151 行附近：工具职责说明加入单 block JSON。
3. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
   - 第 44 行附近：阶段 4 记录 `--block-json` 已完成。

### 行为

`notdec-native-discover --block-json <start> <elf-file>` 只接受 confirmed basic block 起点。
找到时输出 `function_entry`、`block`、`successors`、`instructions` 和 `instruction_count`。
找不到时输出 `found=false`、空 `instructions` 和 `instruction_count=0`。

这次没有改变 discovery、CFG、xref 或 lowering。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
git diff --check
```

结果：通过。

Bench2 专项检查：

```text
vsftpd --block-json 0x5000: found=true function_entry=0x5000 instruction_count=5
libuv --block-json 0x8000: found=true function_entry=0x8000 instruction_count=5
memcached --block-json 0x5000: found=true function_entry=0x5000 instruction_count=5
missing block 0x1: found=false instruction_count=0
```

完整 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260522-73
```

结果：

```text
vsftpd ok elapsed=12s
libuv ok elapsed=23s
memcached ok elapsed=12s
```

性能和规模：

```text
target	elapsed_seconds	confirmed_functions	basic_blocks	instructions	xrefs_total	xrefs_flow	xrefs_call	xrefs_data	xrefs_string	unresolved_total	unresolved_indirect_call	unresolved_indirect_branch
vsftpd	12	9	28	75	303	13	2	149	139	0	0	0
libuv	23	9	26	80	23	9	3	11	0	0	0	0
memcached	12	9	28	75	179	13	2	62	102	0	0	0
```

新增接口只格式化已有 block / instruction state，不改变 native 分析流程；Bench2 指标没有语义变化。

### 评分

- 实现效果：7/10。能按 block 起点直接查看单个 block 和指令，补足全量 `--blocks-json` 不便定位的问题。
- 复杂度：2/10。只复用已有 confirmed block 和 instruction range 查询。
- 维护成本：2/10。后续如果要支持 containing-block 查询，可以另加字段或新命令，不影响当前精确语义。
