# 本次用户原始 prompt

继续按照规划完善 bin2llvm native 链路。阶段 4 已有 summary、functions、blocks、xrefs JSON，下一小步补最小 instruction JSON，让 Bench2 smoke 能检查已接受的指令列表。

## 背景

当前 native decode 已经会把 Sleigh 解出的指令写进 `NativeInstruction`。`--summary-json` 只能看到 instruction 总数，`--blocks-json` 只能看到 block 范围，看不到具体指令地址、大小和显示文本。后续要对齐 rizin 的 `pdJ/pi` 这类命令，先需要能导出当前已接受的指令列表。

Ghidra 侧相关实现：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Listing.java::getInstructions(...)` 可以按地址范围遍历指令。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Instruction.java::getAddress()` 返回指令地址。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Instruction.java::getLength()` 返回指令长度。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Instruction.java::toString()` 提供显示文本。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Instruction.java::getBytes()` 提供原始字节。

Rizin 侧相关实现：

- `pdJ` 按当前地址输出反汇编 JSON。
- `pi` / `pij` 输出更轻量的指令列表。
- `pdbJ` 输出当前 basic block 的 JSON 反汇编。

native 侧已有状态：

- `NativeProgramState::instructions()` 返回按地址排序的 `std::map<uint64_t, NativeInstruction>`。
- `NativeInstruction` 有 `Address`、`Size`、`Bytes`、`Mnemonic`、`Source`。
- 当前指令来自 bounded Sleigh decode，还不是完整程序指令表。

## 目标

1. `notdec-native-discover --instructions-json <elf>` 输出已接受的指令列表。
2. 每条指令输出 `address`、`size`、`bytes`、`text`、`source`。
3. 地址和 bytes 用十六进制字符串；`bytes` 不带分隔符。
4. 顶层输出 `instructions[]` 和 `count`，`count` 和 `--summary-json instructions` 同口径。
5. 暂时不做按函数、block、地址范围过滤。

## 技术路线

- 扩展 `OutputMode` 和参数解析，增加 `InstructionsJson`。
- 新增 `bytesHex(...)`，把 `std::vector<uint8_t>` 转成小写十六进制字符串。
- 新增 `printInstructionsJson(...)`，遍历 `state.instructions()`。
- 复用 `hexString(...)` 和 `jsonEscape(...)`。
- 其他 CLI 模式保持原行为。

## 风险

- 当前 instruction 列表只覆盖 bounded decode 接受的前缀，不是完整 disassembly。
- `text` 只是当前 Sleigh summary 拼出的显示文本，不包含 operand 结构化字段。
- 后续要做 `pdJ/pdbJ` 等价能力时，需要加地址范围和 block/function 过滤。

## 判断标准

- `--instructions-json` 输出能被 JSON parser 解析。
- Bench2 三个 smoke 都能输出非空 instruction 数组。
- `instructions[].length` 和 `--summary-json` 的 `instructions` 同口径。
- 其他 CLI 模式不回退。
- 用时不应明显增加。

## 实现记录

已完成。

改动文件：

- `tools/notdec-native-discover.cpp:19`：`OutputMode` 增加 `InstructionsJson`。
- `tools/notdec-native-discover.cpp:33`：`printUsage(...)` 增加 `--instructions-json <elf-file>`。
- `tools/notdec-native-discover.cpp:42`：`parseArgs(...)` 识别 `--instructions-json`。
- `tools/notdec-native-discover.cpp:77`：新增 `bytesHex(...)`，把 `NativeInstruction::Bytes` 输出为连续十六进制字符串。
- `tools/notdec-native-discover.cpp:269`：新增 `printInstructionsJson(...)`，遍历 `state.instructions()` 输出 `address`、`size`、`bytes`、`text`、`source` 和 `count`。
- `tools/notdec-native-discover.cpp:312`：`main(...)` 继续跑同一批 analyzer；JSON 模式 run 后按输出模式打印。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:22`：阶段 4 记录本小步完成。
- `ARCHITECTURE.md:34`、`ARCHITECTURE.md:93`：说明 `--instructions-json` 的输出范围。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --instructions-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --instructions-json /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --instructions-json /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --instructions-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool >/tmp/notdec-instructions-json-check.txt
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool >/tmp/notdec-xrefs-json-check.txt
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --blocks-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool >/tmp/notdec-blocks-json-check.txt
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
```

Bench2 smoke 结果：

- `vsftpd`：`count=80`，和 `--summary-json instructions=80` 一致，instructions JSON 2.89s，summary JSON 2.83s，文本模式 2.85s。
- `libuv.so.1.0.0`：`count=93`，instructions JSON 3.03s。
- `memcached`：`count=80`，instructions JSON 2.90s。

性能判断：这一步只多做最终 instruction JSON 格式化，分析流程不变。三条 smoke 仍在 3 秒左右，没有看到明显变慢。

评分：

- 实现效果：8/10。能把当前 accepted instruction 导出，方便后续做 `pdJ/pi` 类 smoke。
- 复杂度：2/10。只新增一个输出模式、一个 bytes formatter 和一个小 printer。
- 后期维护成本：3/10。当前是全量列表；后续按函数、block、地址范围过滤和结构化 operand 应分小步继续做。
