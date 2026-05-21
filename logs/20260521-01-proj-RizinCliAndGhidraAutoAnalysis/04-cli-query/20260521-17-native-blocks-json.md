# 本次用户原始 prompt

继续按照规划完善 bin2llvm native 链路。阶段 4 已有 summary 和 functions JSON，下一小步补最小 basic block JSON，让 Bench2 smoke 能检查 confirmed function 里的 CFG 前缀。

## 背景

当前 native decode 已经会把成功解码的函数前缀切成 `NativeBasicBlock`，并记录 direct branch / conditional branch 的 successor。`--functions-json` 只能看到每个函数的 block 数，看不到 block 范围和 successor。后续要对齐 rizin 的 `afb/afbj` 和 Ghidra 的函数 body/block 查询，需要先把这部分状态稳定导出。

Ghidra 侧相关实现：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Function.java::getBody()` 返回函数 body 地址集合。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/block/BasicBlockModel.java::getCodeBlocksContaining(...)` 和 `getCodeBlocks(...)` 能按函数 body 找 code block。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/block/CodeBlock.java::getSources(...)`、`getDestinations(...)` 提供 block 间引用。

Rizin 侧相关实现：

- `afb` / `afbj` 列当前函数 basic block，包含 block 地址、大小、jump/fail 等控制流信息。
- `agf` / `agfj` 基于函数 block 和边输出 CFG。
- 第一小步不做图，也不按地址查单个函数，只导出当前已确认函数里的 block 列表，后续再加过滤。

native 侧已有状态：

- `NativeFunction::Blocks` 保存函数内 block。
- `NativeBasicBlock` 有 `Start`、`End`、`Successors`。
- 当前 successor 只来自已识别的 direct branch / CBRANCH，间接跳转和 return 暂不记录。

## 目标

1. `notdec-native-discover --blocks-json <elf>` 输出 confirmed function 下的 block 列表。
2. 每个 block 输出所属函数入口、block 起止地址、size、successors。
3. 地址用十六进制字符串。
4. 保持文本 report、`--summary-json`、`--functions-json` 行为不变。
5. 不输出指令明细，不做 DOT/graph。

## 技术路线

- 扩展 `OutputMode` 和参数解析，增加 `BlocksJson`。
- 新增 `printBlocksJson(...)`。
- 遍历 `state.functions()` 和每个 `function.Blocks`。
- 输出顶层 `blocks[]` 和 `count`，`count` 和 `--summary-json basic_blocks` 同口径。

## 风险

- 当前 block 只是 bounded decode 的 CFG 前缀，不是完整函数 CFG。
- successor 目前只覆盖 direct branch / CBRANCH 的已知目标。
- direct branch 到已知其他函数入口时不会进入当前函数 successor，这是阶段 3 已明确的保守策略。

## 判断标准

- `--blocks-json` 输出能被 JSON parser 解析。
- Bench2 三个 smoke 都能输出非空 block 数组。
- `blocks[].length` 和 `--summary-json` 的 `basic_blocks` 同口径。
- 其他 CLI 模式不回退。
- 用时不应明显增加。

## 实现记录

已完成。

改动文件：

- `tools/notdec-native-discover.cpp:19`：`OutputMode` 增加 `BlocksJson`。
- `tools/notdec-native-discover.cpp:31`：`printUsage(...)` 增加 `--blocks-json <elf-file>`。
- `tools/notdec-native-discover.cpp:38`：`parseArgs(...)` 识别 `--blocks-json`。
- `tools/notdec-native-discover.cpp:192`：新增 `printAddressArray(...)`，输出 successor 地址数组。
- `tools/notdec-native-discover.cpp:202`：新增 `printBlocksJson(...)`，遍历 `state.functions()` 和 `NativeFunction::Blocks`，输出 block 范围、大小和 successors。
- `tools/notdec-native-discover.cpp:246`：`main(...)` 继续跑同一批 analyzer；JSON 模式 run 后按输出模式打印。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:22`：阶段 4 记录本小步完成。
- `ARCHITECTURE.md:34`、`ARCHITECTURE.md:92`：说明 `--blocks-json` 的输出范围。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --blocks-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --blocks-json /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --blocks-json /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --blocks-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool >/tmp/notdec-blocks-json-check.txt
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool >/tmp/notdec-summary-json-check.txt
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --functions-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool >/tmp/notdec-functions-json-check.txt
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
```

Bench2 smoke 结果：

- `vsftpd`：`count=31`，和 `--summary-json basic_blocks=31` 一致，blocks JSON 2.92s，文本模式 2.94s。
- `libuv.so.1.0.0`：`count=32`，blocks JSON 3.14s。
- `memcached`：`count=31`，blocks JSON 2.90s。

性能判断：这一步只多做最终 block JSON 格式化，分析流程不变。三条 smoke 仍在 3 秒左右，没有看到明显变慢。

评分：

- 实现效果：8/10。能把 confirmed function 下的 block 和 successor 导出，方便后续 CFG smoke。
- 复杂度：2/10。只新增一个输出模式和一个小 printer。
- 后期维护成本：3/10。当前只导出已有 block 状态；后续完整 CFG、按函数过滤、DOT 输出应分小步继续做。
