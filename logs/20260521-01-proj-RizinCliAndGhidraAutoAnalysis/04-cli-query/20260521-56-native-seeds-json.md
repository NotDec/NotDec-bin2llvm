# Native Seeds JSON

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

Stage 2 要求 seed 来源能解释。现在 summary JSON 只给 source 计数，`--functions-json` 只给 confirmed
function。排查入口发现时，还不能直接看到所有 function seed 的地址、来源、confidence 和 range。

这次补：

```text
notdec-native-discover --seeds-json <elf-file>
```

## Ghidra 相关实现

Ghidra 里入口候选最后都会落到 Program 里的符号、函数和 entry point 状态：

- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/EntryPointAnalyzer.java`
  - 把入口、符号等高可信地址转成函数入口。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/FunctionManager.java`
  - 提供函数入口查询和遍历。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/SymbolTable.java`
  - 可查询符号和 alias 类信息。

native 侧没有 ProgramDB；`NativeFunctionSeed` 已经保存 address、primary name、aliases、sources、
confidence 和 range。这次只是把它导出成 JSON。

## native 侧复刻策略

1. `notdec-native-discover` 增加 `--seeds-json <elf-file>`。
2. 遍历 `NativeProgramState::functionSeeds()`。
3. 每个 seed 输出：
   - `address`
   - `size`
   - `range_start`
   - `range_end`
   - `range_source`
   - `primary_name`
   - `aliases[]`
   - `sources[]`
   - `confidence`
4. 顶层输出 `seeds[]` 和 `count`。

暂时不做：

- 不输出低层 analyzer 内部状态。
- 不把 seed JSON 加入 smoke 主流程，避免每次 smoke 额外跑三次 discovery。
- 不改变 seed 合并策略。

## 判断标准

1. 三个 Bench2 目标的 `--seeds-json` 输出合法 JSON。
2. `count` 和 `--summary-json function_seeds` 一致。
3. `libuv` seed 里能看到 `elf-dynamic-symbol` / `elf-symbol` 来源。
4. 原有 Bench2 smoke 继续通过。

## 风险

1. seed 列表较大，作为排查接口比 smoke 固定门槛更合适。
2. JSON schema 以后可能随 seed 字段扩展，但当前字段都来自稳定的 `NativeFunctionSeed`。

## 实现记录

已完成 `notdec-native-discover --seeds-json <elf-file>`。

修改点：

- `tools/notdec-native-discover.cpp:19` 的 `OutputMode` 增加 `SeedsJson`。
- `tools/notdec-native-discover.cpp:44` 的 `printUsage(...)` 增加 `--seeds-json`。
- `tools/notdec-native-discover.cpp:75` 的 `parseArgs(...)` 识别 `--seeds-json`。
- `tools/notdec-native-discover.cpp:262` 增加 `printStringArray(...)`，复用 JSON 字符串转义。
- `tools/notdec-native-discover.cpp:272` 增加 `printSeedsJson(...)`，输出 seed 地址、size、range、名字、alias、sources 和 confidence。
- `tools/notdec-native-discover.cpp:579` 的 `main(...)` 增加 `SeedsJson` 分发。
- `ARCHITECTURE.md:34`、`ARCHITECTURE.md:124` 记录新的 native discovery 查询接口。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:8`、`:31` 更新 Stage 2 / Stage 4 进度。

验证：

```bash
git diff --check
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
```

结果：通过。

```bash
for target in vsftpd libuv memcached; do
  notdec-native-discover --seeds-json "$elf" > "$out"
  python3 -m json.tool "$out" >/dev/null
done
```

结果：

- `vsftpd`: seeds `187`，summary `function_seeds` `187`。
- `libuv`: seeds `485`，summary `function_seeds` `485`，能看到 `elf-dynamic-symbol` 和 `elf-symbol` 来源。
- `memcached`: seeds `259`，summary `function_seeds` `259`。

Bench2 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-56
```

结果：通过。

```text
target    elapsed_seconds  confirmed_functions  basic_blocks  instructions  xrefs_total  unresolved_total
vsftpd    8                9                    30            80            304          0
libuv     19               9                    29            85            26           0
memcached 7                9                    30            80            186          0
```

性能：新增接口只复用已有 discovery pipeline，默认 smoke 不额外调用 `--seeds-json`，不会增加 smoke 主流程耗时。

评分：

- 实现效果：9/10。能直接解释 seed 来源，满足 Stage 2 排查需求。
- 复杂度：9/10。只新增一个 formatter，没有改 seed 合并逻辑。
- 维护成本：9/10。字段直接来自 `NativeFunctionSeed`，后续扩展也集中在一个输出函数里。
