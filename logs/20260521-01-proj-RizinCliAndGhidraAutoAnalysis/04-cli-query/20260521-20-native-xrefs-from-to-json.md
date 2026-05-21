# 本次用户原始 prompt

继续按照规划完善 bin2llvm native 链路。阶段 4 已有全量 xref JSON，下一小步补按地址查询 xref from/to，让 CLI 更接近 Ghidra ReferenceManager 和 rizin `axf/axt`。

## 背景

当前 `--xrefs-json` 能列出全量 xref，但 smoke 或人工定位某条边时还要自己过滤。Ghidra 和 rizin 都有按地址查引用的能力。native state 已经维护了 from/to 索引，这一步只把现有能力接到 CLI。

Ghidra 侧相关实现：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java::getReferencesFrom(...)` 查询从某地址发出的引用。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java::getReferencesTo(...)` 查询指向某地址的引用。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/Reference.java::getFromAddress()`、`getToAddress()`、`getReferenceType()` 提供引用事实。

Rizin 侧相关实现：

- `axf` 列从当前地址发出的 xref。
- `axt` 列指向当前地址的 xref。
- JSON 模式适合脚本做 smoke，例如确认某条 direct call 或 direct branch 是否存在。

native 侧已有状态：

- `NativeProgramState::xrefsFrom(uint64_t)` 返回 from 地址匹配的 xref 指针列表。
- `NativeProgramState::xrefsTo(uint64_t)` 返回 to 地址匹配的 xref 指针列表。
- `NativeXref` 有 `From`、`To`、`Kind`、`Source`。

## 目标

1. `notdec-native-discover --xrefs-from-json <addr> <elf>` 输出从 `<addr>` 发出的 xref。
2. `notdec-native-discover --xrefs-to-json <addr> <elf>` 输出指向 `<addr>` 的 xref。
3. 地址参数支持 `0x...` 或十进制。
4. 输出包含 `query`、`direction`、`xrefs[]`、`count`。
5. 保持已有 CLI 模式不回退。

## 技术路线

- 扩展 `CliOptions`，加一个可选 `QueryAddress`。
- 扩展 `parseArgs(...)` 支持三参数查询形式。
- 新增 `parseAddress(...)`，用 `std::stoull(..., 0)` 解析地址。
- 把 xref JSON 单项输出抽成小 helper，复用全量和 from/to 查询。
- 新增 `printXrefsQueryJson(...)`，根据 from/to 调用 `state.xrefsFrom(...)` 或 `state.xrefsTo(...)`。

## 风险

- 查询地址必须精确等于 xref 的 from 或 to 地址，不做 instructionContaining 或 blockContaining 模糊匹配。
- 当前 xref 仍只覆盖 direct control flow，data/string xref 后续再补。
- 命令参数变成两种形态，parseArgs 需要保持简单，避免影响已有模式。

## 判断标准

- `--xrefs-from-json` 和 `--xrefs-to-json` 输出能被 JSON parser 解析。
- 在 Bench2 `vsftpd` 上，`--xrefs-from-json 0x8327` 能查到 call 到 `0x8290`。
- 在 Bench2 `vsftpd` 上，`--xrefs-to-json 0x82b8` 能查到两条 flow。
- 三个 Bench2 smoke 的全量 `--xrefs-json` 仍可解析。
- 用时不应明显增加。

## 实现记录

已完成。

改动文件：

- `tools/notdec-native-discover.cpp:19`：`OutputMode` 增加 `XrefsFromJson` / `XrefsToJson`。
- `tools/notdec-native-discover.cpp:30`：`CliOptions` 增加 `QueryAddress`。
- `tools/notdec-native-discover.cpp:36`：`printUsage(...)` 增加 `--xrefs-from-json <addr> <elf-file>` 和 `--xrefs-to-json <addr> <elf-file>`。
- `tools/notdec-native-discover.cpp:47`：新增 `parseAddress(...)`，支持 `0x...` 和十进制。
- `tools/notdec-native-discover.cpp:60`：`parseArgs(...)` 支持带地址的四参数查询形式。
- `tools/notdec-native-discover.cpp:282`：新增 `printXrefObject(...)`，复用 xref 单项 JSON 输出。
- `tools/notdec-native-discover.cpp:310`：新增 `printXrefsQueryJson(...)`，调用 `state.xrefsFrom(...)` 或 `state.xrefsTo(...)`。
- `tools/notdec-native-discover.cpp:374`：`main(...)` 继续跑同一批 analyzer；JSON 模式 run 后按输出模式打印。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:22`：阶段 4 记录本小步完成。
- `ARCHITECTURE.md:34`、`ARCHITECTURE.md:94`：说明 xref from/to 查询。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-from-json 0x8327 /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-to-json 0x82b8 /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-from-json 0x8327 /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool >/tmp/notdec-xrefs-from-json-check.txt
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-to-json 0x82b8 /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool >/tmp/notdec-xrefs-to-json-check.txt
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool >/tmp/notdec-xrefs-vsftpd-json-check.txt
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-json /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 | python3 -m json.tool >/tmp/notdec-xrefs-libuv-json-check.txt
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-json /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached | python3 -m json.tool >/tmp/notdec-xrefs-memcached-json-check.txt
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
```

Bench2 smoke 结果：

- `vsftpd --xrefs-from-json 0x8327`：`count=1`，查到 `0x8327 -> 0x8290` 的 `call`。
- `vsftpd --xrefs-to-json 0x82b8`：`count=2`，查到 `0x82a1 -> 0x82b8` 和 `0x82ad -> 0x82b8` 的 `flow`。
- `vsftpd` / `libuv.so.1.0.0` / `memcached` 的全量 `--xrefs-json` 仍可被 JSON parser 解析。
- `vsftpd --summary-json` 仍显示 `xrefs.total=9`。

性能判断：这一步只多做最终 xref 过滤输出，分析流程不变。`vsftpd` 两个查询分别 2.79s 和 2.87s，没有看到明显变慢。

评分：

- 实现效果：8/10。能按 from/to 精确查 xref，足够支持当前 direct call / branch smoke。
- 复杂度：3/10。参数解析多了一种形态，但仍集中在 CLI 文件里。
- 后期维护成本：3/10。后续可继续加地址范围、函数内过滤和 data/string xref，不需要改现有 state 接口。
