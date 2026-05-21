# 原始 prompt

继续按照规划完善 bin2llvm native 链路。本小块仍属于阶段 5 XRef enhancement：把 ELF relocation 已经算出的本地指针补成 xref。

# 当前 native 状态

native 已经在 `lib/NativeAnalysis.cpp::RelocationPltAnalyzer` 里读取 ELF relocation：

- `X86_64_RELATIVE` / `X86_64_RELATIVE64` 会按 addend 算出本地地址，写入 `NativeProgramState::addRelocatedPointer(...)`。
- 部分有本地 symbol value 的 `X86_64_GLOB_DAT` 也会写入 relocated pointer。
- `NativeProgramState::readPointer(...)` 已经优先用 `relocatedPointers()`，所以 dynamic init/fini array 可以读到重定位后的函数地址。

但这些 relocation slot 本身还没有进入 `NativeXref`。因此 CLI 能看到 relocation 数量，却不能用 `--xrefs-from-json` / `--xrefs-to-json` 查询“这个数据槽指向哪里”。

# Ghidra / Rizin 对照

Ghidra 相关源码：

- `/sn640/ghidra/Ghidra/Features/Base/src/main/java/ghidra/app/util/bin/format/elf/ElfHeader.java::parseRelocationTables(...)` 解析 ELF relocation table。
- `/sn640/ghidra/Ghidra/Features/Base/src/main/java/ghidra/app/util/bin/format/elf/relocation/ElfRelocationContext.java::processRelocation(...)` 分发 relocation 处理。
- `/sn640/ghidra/Ghidra/Processors/x86/src/main/java/ghidra/app/util/bin/format/elf/relocation/X86_64_ElfRelocationHandler.java::relocate(...)` 处理 `R_X86_64_RELATIVE`、`R_X86_64_RELATIVE64`、`R_X86_64_64` 等类型。
- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java::addMemoryReference(...)` 和 `getReferencesFrom(...)` / `getReferencesTo(...)` 是后续引用写入和查询接口。

Rizin 侧类似信息常通过 relocation 和 xref 命令组合观察：

- `ir` / `iR` 看 relocation。
- `axd` 表示 data xref。
- `axf` / `axt` / `axl` 查询 from/to/list。

Ghidra 完整逻辑会把 loader、relocation、data markup、引用分析串起来。native 当前先不建完整数据类型，只记录已经能确定的 slot -> target。

# native 策略

本小块只处理 `RelocationPltAnalyzer` 已经确认为本地地址的 relocated pointer：

1. from = relocation slot 地址。
2. to = computed pointer value。
3. target 必须落在当前已加载内存 range 内，避免把常量或外部地址误记成 xref。
4. 如果 target 是只读 C 字符串，记 `NativeXrefKind::String`，source 为 `elf-relocation-string`。
5. 其他本地 target 记 `NativeXrefKind::Data`，source 为 `elf-relocation-pointer`。
6. 不处理 `JUMP_SLOT` 外部符号、不处理 unsupported relocation、不新增 CLI。

这一步允许 target 是 executable address。因为 relocation slot 是数据槽，slot 指向函数入口也是有效 data-to-code 引用，不等同于指令 direct data varnode。

# 风险

- `R_X86_64_RELATIVE` 数量可能不少，xref 总数会明显增加。这是预期行为，但要看输出 JSON 是否仍能正常解析。
- target 如果落在 jump table 或只读常量表，会按 data 记录；这比猜成 code/string 更保守。
- 这一步只覆盖已经应用的本地 relocation，不覆盖运行时绑定外部符号。

# 判断标准

1. `notdec-native-discover` 能构建。
2. Bench2 三个 smoke 的 `xrefs.total` 和 `xrefs.data` 应明显增加。
3. `--xrefs-json` 仍能被 JSON parser 解析。
4. 至少一个已知 relocation slot 能通过 `--xrefs-from-json` 查到 `elf-relocation-pointer` 或 `elf-relocation-string`。
5. 三条 smoke 的运行时间不应明显变慢。

# 实现记录

已完成。

改动文件：

- `lib/NativeAnalysis.cpp:147`：新增 `isMappedAddress(...)`，判断 target 是否落在当前加载内存 range 内。
- `lib/NativeAnalysis.cpp:313`：`RelocationPltAnalyzer::run(...)` 在 PLT 映射后调用 relocation pointer xref 记录。
- `lib/NativeAnalysis.cpp:317`：新增 `RelocationPltAnalyzer::addRelocationPointerXrefs(...)`，遍历 `state.relocatedPointers()`，写入 `elf-relocation-pointer` 或 `elf-relocation-string`。
- `ARCHITECTURE.md:61`：补充 relocation pointer xref 的 native 架构说明。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:32`：阶段 5 记录本小步完成。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool >/tmp/notdec-xrefs-reloc-json-check.txt
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-from-json 0x257a0 /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-from-json 0x3e598 /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached | python3 -m json.tool
```

Bench2 smoke 结果：

- `vsftpd`：`xrefs.total=297`，`flow=8`，`call=1`，`data=149`，`string=139`，`elapsed=2.83`。
- `libuv.so.1.0.0`：`xrefs.total=24`，`flow=9`，`call=2`，`data=13`，`string=0`，`elapsed=3.02`。
- `memcached`：`xrefs.total=179`，`flow=8`，`call=1`，`data=68`，`string=102`，`elapsed=2.84`。

查询结果：

- `vsftpd --xrefs-from-json 0x257a0`：查到 `0x257a0 -> 0x8340`，kind 为 `data`，source 为 `elf-relocation-pointer`。
- `memcached --xrefs-from-json 0x3e598`：查到 `0x3e598 -> 0x3210c`，kind 为 `string`，source 为 `elf-relocation-string`。
- `vsftpd --xrefs-json` 可被 JSON parser 解析，输出文件大小 `46518` 字节。

性能判断：三条 smoke 仍在 3 秒左右。xref 数量增加明显，但当前 JSON 和文本 report 都能正常跑完，未看到明显性能下降。

评分：

- 实现效果：8/10。已应用的本地 relocation 现在能被 xref 查询消费，Bench2 两个可执行文件效果明显。
- 复杂度：3/10。只在 relocation analyzer 里消费已有 `relocatedPointers()`，没有改接口。
- 后期维护成本：3/10。后续可以再补外部符号、更多 relocation 类型或更细的 data type markup。
