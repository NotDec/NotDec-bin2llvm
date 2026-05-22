# Native Function XRefs JSON

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

native CLI 已经有 `--function-json <entry>`，但只给 outgoing / incoming 计数。
排查函数语义时还需要直接看到该函数相关 xref 明细。

这次补：

```text
notdec-native-discover --function-xrefs-json <entry> <elf-file>
```

## Ghidra 相关实现

Ghidra 的函数引用信息来自 `FunctionManager`、`Function` 和 `ReferenceManager`：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/FunctionManager.java`
  - `getFunctionAt(...)` 按入口查函数。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Function.java`
  - `getBody()` 表达函数体范围。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java`
  - `getReferencesFrom(...)`
  - `getReferencesTo(...)`

rizin 侧对应 `afx`、`axf`、`axt` 这类函数和地址引用查询。

## native 侧复刻策略

1. `notdec-native-discover` 增加 `--function-xrefs-json <entry> <elf-file>`。
2. 用 `NativeProgramState::functionAt(entry)` 精确查 confirmed function。
3. outgoing：遍历 `state.xrefs()`，`from` 归属于该函数 block 的 xref。
4. incoming：只列精确指向函数入口的 xref。
5. 复用现有 `printXrefObject(...)`。

暂时不做：

- 不按地址模糊匹配所属函数。
- 不统计指向函数内部非入口地址的 incoming。
- 不合并重复引用。

## 判断标准

1. `notdec-native-discover` 能编译。
2. 三个 Bench2 目标任选 confirmed function entry，输出合法 JSON。
3. `outgoing_count == len(outgoing)`，`incoming_entry_count == len(incoming_entry)`。
4. 不存在 entry 输出 `found=false`。
5. 原有 Bench2 smoke 继续通过。

## 风险

1. outgoing 依赖当前 confirmed block，函数边界不完整时会漏引用。
2. incoming 目前只查入口，后续如果需要函数体内引用应另补字段。

## 实现记录

### 修改范围

1. `tools/notdec-native-discover.cpp`
   - 第 19 行附近：`OutputMode` 增加 `FunctionXrefsJson`。
   - 第 57 行附近：usage 增加 `--function-xrefs-json <entry> <elf-file>`。
   - 第 148 行附近：`parseArgs(...)` 识别 `--function-xrefs-json` 并复用 `QueryFunctionEntry`。
   - 第 914 行附近：新增 `printFunctionXrefsJson(...)`。
   - 第 1217 行附近：`main(...)` 分发到 `printFunctionXrefsJson(...)`。
2. `ARCHITECTURE.md`
   - 第 43 行附近：CLI query 说明加入 `--function-xrefs-json <entry>`。
   - 第 144 行附近：工具说明加入单函数引用 JSON。
3. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
   - 阶段 4 记录 `--function-xrefs-json` 已完成。

### 行为

`notdec-native-discover --function-xrefs-json <entry> <elf-file>` 只查精确 confirmed function entry。
找到函数时输出：

- `outgoing`：`from` 落在该函数 confirmed block 内的 xref。
- `incoming_entry`：精确指向函数入口的 xref。

找不到时输出 `found=false` 和空列表。

这次没有改变 discovery、xref 生成或 lowering。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
git diff --check
```

结果：通过。

Bench2 专项检查：

```text
vsftpd --function-xrefs-json 0x5000: outgoing=3 incoming_entry=0
libuv --function-xrefs-json 0x8000: outgoing=3 incoming_entry=0
memcached --function-xrefs-json 0x5000: outgoing=3 incoming_entry=0
missing entry 0x1: found=false, counts=0
```

完整 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260522-72
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

新增接口只格式化已有 function / xref state，不改变 native 分析流程；Bench2 指标没有语义变化。

### 评分

- 实现效果：7/10。能直接查看函数相关 xref 明细，补足 `--function-json` 只有计数的问题。
- 复杂度：2/10。复用已有 `functionContaining(...)`、`xrefsTo(...)` 和 xref JSON formatter。
- 维护成本：2/10。后续如果要加函数体内 incoming，可在当前 JSON 下追加字段。
