# 本次用户原始 prompt

继续按照规划完善 bin2llvm native 链路。阶段 4 已有 `--summary-json`，下一小步补一个最小函数列表 JSON，让 Bench2 smoke 能检查已确认函数的入口、范围和 block 数。

## 背景

当前 native discovery 已经能从 seed 做 bounded decode，并把成功解码的入口落到 `NativeFunction`。`--summary-json` 只能看总数，不能知道具体确认了哪些函数。后续要对齐 rizin 的 `afl/aflj` 和 Ghidra 的函数查询，先需要一个稳定的函数列表输出。

Ghidra 侧相关实现：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/FunctionManager.java::getFunctions(...)` 按地址顺序遍历函数。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Function.java::getEntryPoint()` 返回函数入口。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Function.java::getBody()` 返回函数地址范围集合。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Function.java::getName()` 返回显示名。

Rizin 侧相关实现：

- `afl` / `aflj` 用来列函数，JSON 模式适合脚本消费。
- `afb` / `afbj` 用来列函数 block。第一步不做完整 block 列表，只在函数列表里放 `block_count`，方便 smoke 判断 CFG 是否有基本落地。
- rizin 的 CLI 命令层只是把已有 analysis state 格式化输出，不在查询命令里重新发明分析逻辑。

native 侧已有状态：

- `NativeProgramState::functions()` 返回按 entry 排序的 `std::map<uint64_t, NativeFunction>`。
- `NativeFunction` 有 `Entry`、`RangeStart`、`RangeEnd`、`Name`、`Blocks`、`Source`。
- 当前 `SleighSeedInstructionAnalyzer` 已经会为成功 decode 的 seed 写入 confirmed function 和 basic block。

## 目标

1. `notdec-native-discover --functions-json <elf>` 输出已确认函数列表。
2. 每个函数先只输出 `entry`、`range_start`、`range_end`、`name`、`source`、`block_count`。
3. 地址用十六进制字符串，避免 JSON number 精度和显示问题。
4. 保持 `<elf>` 文本 report 和 `--summary-json <elf>` 行为不变。
5. 不输出完整 block、instruction、xref 列表，避免这一步变大。

## 技术路线

- 扩展 `tools/notdec-native-discover.cpp` 的 CLI 选项，把输出模式从 bool 改成小 enum。
- 分析流程仍然跑同一批 analyzer。
- 文本模式继续挂 `ReportAnalyzer`。
- JSON 模式不挂 `ReportAnalyzer`，run 完后调用对应 printer。
- 复用现有 `jsonEscape(...)`，新增很小的 `hexString(...)` 和 `printFunctionsJson(...)`。

## 风险

- 当前 confirmed function 数量还受 bounded decode 限制，函数列表不是完整程序函数列表。
- `RangeStart/RangeEnd` 现在只是已解码保守范围，不等于 Ghidra 完整 function body。
- `block_count` 只能说明当前 CFG 前缀，不代表完整 CFG。

## 判断标准

- `--functions-json` 输出能被 JSON parser 解析。
- Bench2 三个 smoke 都能输出非空函数数组。
- `functions[].length` 和 `--summary-json` 的 `confirmed_functions` 同口径。
- 原文本 report 和 `--summary-json` 不回退。
- 用时不应明显增加。

## 实现记录

已完成。

改动文件：

- `tools/notdec-native-discover.cpp:16`：新增 `OutputMode`，把文本 report、summary JSON、functions JSON 三种输出模式分清楚。
- `tools/notdec-native-discover.cpp:24`：`CliOptions` 改为保存 `OutputMode`。
- `tools/notdec-native-discover.cpp:29`：`printUsage(...)` 增加 `--functions-json <elf-file>`。
- `tools/notdec-native-discover.cpp:35`：`parseArgs(...)` 识别 `--summary-json` 和 `--functions-json`。
- `tools/notdec-native-discover.cpp:58`：新增 `hexString(...)`，函数列表里的地址统一用十六进制字符串。
- `tools/notdec-native-discover.cpp:162`：新增 `printFunctionsJson(...)`，输出 `functions[]` 和 `count`。
- `tools/notdec-native-discover.cpp:204`：`main(...)` 继续跑同一批 analyzer；文本模式挂 `ReportAnalyzer`，JSON 模式 run 后按模式打印。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:22`：阶段 4 记录本小步完成。
- `ARCHITECTURE.md:34`、`ARCHITECTURE.md:91`：说明 `--functions-json` 的输出范围。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --functions-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --functions-json /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --functions-json /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --functions-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool >/tmp/notdec-functions-json-check.txt
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
```

Bench2 smoke 结果：

- `vsftpd`：`count=9`，和 `--summary-json confirmed_functions=9` 一致，functions JSON 2.83s，summary JSON 2.84s，文本模式 2.75s。
- `libuv.so.1.0.0`：`count=10`，functions JSON 3.08s。
- `memcached`：`count=9`，functions JSON 2.84s。

性能判断：这一步只多做最终 JSON 格式化，仍复用原分析流程。三条 smoke 都在 3 秒左右，没有看到明显变慢。

评分：

- 实现效果：8/10。能列 confirmed function，方便后续 Bench2 做函数级检查。
- 复杂度：2/10。只扩展 CLI 输出模式和一个小 printer。
- 后期维护成本：3/10。字段很少，后续增加完整 block 列表时应单独拆到下一步。
