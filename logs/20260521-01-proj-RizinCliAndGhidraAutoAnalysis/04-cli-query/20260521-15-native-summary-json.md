# 本次用户原始 prompt

继续按照规划完善 bin2llvm native 链路。阶段 3 已经能输出文本 report；下一小步进入阶段 4，先给 `notdec-native-discover` 增加最小 JSON summary，方便 Bench2 smoke 后续做机器检查。

## 背景

当前目标是 native 链路先能围绕 Bench2 真实二进制观察结果。已有 native 状态包括 function seed、worklist、confirmed function、basic block、instruction、xref、unresolved indirect flow。现在只能读文本 report，不利于后续批量对比。

Ghidra 侧相关实现：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/FunctionManager.java::getFunctionAt(...)` 和 `getFunctions(...)` 提供函数查询。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java::getReferencesFrom(...)`、`getReferencesTo(...)` 提供 xref 查询。
- Ghidra 脚本侧常见做法是通过这些 API 把 Program 事实导出成结构化数据，再由外部工具消费。

Rizin 侧相关实现：

- Rizin 命令习惯用 `j` 后缀输出 JSON，例如 `aflj` 列函数、`afbj` 列 basic block、`axfj` 查 xref。
- `/sn640/rizin/librz/core/cmd_descs/cmd_descs.c` 中很多 analysis 命令注册了 `RZ_OUTPUT_MODE_JSON`，说明同一份分析状态需要同时支持人读输出和机器读输出。

native 侧先不一次做完整 `aflj/afbj/axfj`。本小步只补最小 summary JSON，让三条 Bench2 smoke 可以稳定检查关键数量。

## 目标

1. `notdec-native-discover <elf>` 保持现有文本 report。
2. `notdec-native-discover --summary-json <elf>` 输出 JSON summary。
3. JSON 包含 function seeds、worklist、source counts、confirmed functions、basic blocks、instructions、xrefs 分类、unresolved indirect flow 分类。
4. 不输出完整 seed 列表、函数列表、block 列表。
5. 不引入新依赖。

## 技术路线

- 扩展 `tools/notdec-native-discover.cpp` 的参数解析。
- 分析流程保持不变，只在是否加入 `ReportAnalyzer` 和最终输出 summary JSON 上分支。
- 手写很小的 JSON 输出，字段都是数字和已知 source 字符串，避免引入额外 JSON writer。
- source 字符串做最小 JSON escape。

## 风险

- 手写 JSON 只能覆盖当前简单字段，后续复杂结构应换成 LLVM JSON 或复用项目 JSON 工具。
- `--summary-json` 不替代后续函数、block、xref 细查命令。
- report 文本和 JSON summary 需要保持同口径计数。

## 判断标准

- `notdec-native-discover --summary-json` 能输出可被简单工具解析的 JSON。
- Bench2 三个 smoke 的 JSON 中关键计数和文本 report 同口径。
- 原文本 report 行为不回退。
- 时间不应明显增加。

## 实现记录

已完成。

改动文件：

- `tools/notdec-native-discover.cpp:15`：`CliOptions` 增加 `SummaryJson`，保留默认文本模式。
- `tools/notdec-native-discover.cpp:20`：`printUsage(...)` 增加 `--summary-json <elf-file>` 用法。
- `tools/notdec-native-discover.cpp:25`：`parseArgs(...)` 支持 `<elf>` 和 `--summary-json <elf>` 两种形式。
- `tools/notdec-native-discover.cpp:44`：新增 `jsonEscape(...)`，只处理当前 summary key 需要的字符串转义。
- `tools/notdec-native-discover.cpp:77`：新增 `countBasicBlocks(...)`，和文本 report 同口径统计 confirmed function 里的 block。
- `tools/notdec-native-discover.cpp:86`：新增 `printSummaryJson(...)`，输出 function seed、worklist、source counts、confirmed functions、basic blocks、instructions、xref 分类、unresolved indirect flow 分类。
- `tools/notdec-native-discover.cpp:159`：`main(...)` 仍跑同一批 analyzer；JSON 模式不加入 `ReportAnalyzer`，`manager.run(...)` 后打印 summary。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:22`：阶段 4 记录本小步完成。
- `ARCHITECTURE.md:34`、`ARCHITECTURE.md:91`：说明 `notdec-native-discover --summary-json` 的用途和输出范围。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
```

Bench2 smoke 结果：

- `vsftpd`：187 seeds，187 worklist，9 confirmed functions，31 blocks，80 instructions，9 xrefs，7 unresolved indirect flows，JSON 2.85s；文本模式 2.93s。
- `libuv.so.1.0.0`：486 seeds，486 worklist，10 confirmed functions，32 blocks，93 instructions，11 xrefs，5 unresolved indirect flows，JSON 3.05s。
- `memcached`：259 seeds，259 worklist，9 confirmed functions，31 blocks，80 instructions，9 xrefs，7 unresolved indirect flows，JSON 2.84s。

性能判断：JSON 模式少跑文本 report 的长列表打印，只输出 summary。三条 smoke 用时仍在 3 秒左右，没有看到明显变慢。

评分：

- 实现效果：8/10。能给后续 Bench2 smoke 直接消费 summary，但还不是完整 query。
- 复杂度：2/10。只在 CLI 层手写小 JSON，没有影响 native state。
- 后期维护成本：3/10。字段少，暂时可维护；后续如果输出列表或嵌套结构，应换成统一 JSON writer。
