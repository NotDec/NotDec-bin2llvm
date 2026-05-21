# 本次用户原始 prompt

继续按照规划完善 bin2llvm native 链路。阶段 4 已有 summary、functions、blocks JSON，下一小步补最小 xref JSON，让 Bench2 smoke 能检查 direct call / branch 引用是否落到查询接口。

## 背景

当前 native decode 已经会把直接 `CALL`、`BRANCH`、`CBRANCH` 识别成 `NativeXref`。`--summary-json` 只能看到 xref 总数和分类数量，不能看到具体 from/to。后续要对齐 rizin 的 `axl/axfj/axt/axf`，需要先有一个全量 xref 列表输出。

Ghidra 侧相关实现：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java::getReferenceIterator(...)` 可以遍历引用。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java::getReferencesFrom(...)` 查询从某地址发出的引用。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java::getReferencesTo(...)` 查询指向某地址的引用。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/Reference.java::getFromAddress()`、`getToAddress()`、`getReferenceType()` 提供 from/to/type。

Rizin 侧相关实现：

- `axl` 列全部 xref。
- `axf` 列从当前地址发出的 xref。
- `axt` 列指向当前地址的 xref。
- `axfj` / `axtj` 这类 JSON 输出适合脚本检查。

native 侧已有状态：

- `NativeProgramState::xrefs()` 返回全量 xref。
- `NativeProgramState::xrefsFrom(...)` 和 `xrefsTo(...)` 已有查询接口。
- `NativeXref` 有 `From`、`To`、`Kind`、`Source`。

## 目标

1. `notdec-native-discover --xrefs-json <elf>` 输出全量 xref 列表。
2. 每条 xref 输出 `from`、`to`、`kind`、`source`。
3. 地址用十六进制字符串。
4. 顶层输出 `xrefs[]` 和 `count`，`count` 和 `--summary-json xrefs.total` 同口径。
5. 暂时不做 `--xrefs-from` / `--xrefs-to` 过滤，避免这一步变大。

## 技术路线

- 扩展 `OutputMode` 和参数解析，增加 `XrefsJson`。
- 新增 `printXrefsJson(...)`，遍历 `state.xrefs()`。
- 复用 `hexString(...)`、`jsonEscape(...)` 和 `toString(NativeXrefKind)`。
- 文本 report、summary、functions、blocks 模式都保持原行为。

## 风险

- 当前 xref 只覆盖已有 decoder 能识别的 direct control flow，data/string xref 还没有真实分析。
- xref 顺序是插入顺序，不保证等同 Ghidra 或 rizin。
- 后续 from/to 过滤和图输出需要单独做。

## 判断标准

- `--xrefs-json` 输出能被 JSON parser 解析。
- Bench2 三个 smoke 都能输出 xref 列表。
- `xrefs[].length` 和 `--summary-json` 的 `xrefs.total` 同口径。
- 其他 CLI 模式不回退。
- 用时不应明显增加。

## 实现记录

已完成。

改动文件：

- `tools/notdec-native-discover.cpp:19`：`OutputMode` 增加 `XrefsJson`。
- `tools/notdec-native-discover.cpp:32`：`printUsage(...)` 增加 `--xrefs-json <elf-file>`。
- `tools/notdec-native-discover.cpp:40`：`parseArgs(...)` 识别 `--xrefs-json`。
- `tools/notdec-native-discover.cpp:233`：新增 `printXrefsJson(...)`，遍历 `state.xrefs()` 输出 `from`、`to`、`kind`、`source` 和 `count`。
- `tools/notdec-native-discover.cpp:271`：`main(...)` 继续跑同一批 analyzer；JSON 模式 run 后按输出模式打印。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:22`：阶段 4 记录本小步完成。
- `ARCHITECTURE.md:34`、`ARCHITECTURE.md:92`：说明 `--xrefs-json` 的输出范围。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-json /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-json /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool >/tmp/notdec-xrefs-json-check.txt
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --functions-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool >/tmp/notdec-functions-json-check.txt
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --blocks-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool >/tmp/notdec-blocks-json-check.txt
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
```

Bench2 smoke 结果：

- `vsftpd`：`count=9`，和 `--summary-json xrefs.total=9` 一致，xrefs JSON 2.83s，summary JSON 2.95s，文本模式 2.82s。
- `libuv.so.1.0.0`：`count=11`，xrefs JSON 3.03s。
- `memcached`：`count=9`，xrefs JSON 2.84s。

性能判断：这一步只多做最终 xref JSON 格式化，分析流程不变。三条 smoke 仍在 3 秒左右，没有看到明显变慢。

评分：

- 实现效果：8/10。能把 direct control-flow xref 导出，方便后续 xref smoke 和 from/to 查询。
- 复杂度：2/10。只新增一个输出模式和一个小 printer。
- 后期维护成本：3/10。当前是全量列表；后续 from/to 过滤、data/string xref、xref 图应分小步继续做。
