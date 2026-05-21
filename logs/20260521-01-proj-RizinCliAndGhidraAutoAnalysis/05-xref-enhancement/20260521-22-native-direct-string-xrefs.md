# 原始 prompt

继续改进 native bin2llvm path（`libsla` / native，不走 GhidraScript），按小块推进。每个小块先写计划日志，再实现、验证、更新 `PROGRESS.md` 和 `ARCHITECTURE.md`，最后先提交 submodule，再提交顶层 submodule 指针。

本小块：把 direct `ram` 数据 xref 里目标指向只读内存、且像 C 字符串的引用，分类成 string xref。不要扫描任意内存，不创建字符串符号或数据对象，只分类已经由 P-Code 看到的 direct 引用。

# native 现状

Stage 5 上一块已经把非控制流 P-Code 里的 direct `ram` varnode 记录成 `NativeXrefKind::Data`：

- `lib/NativeAnalysis.cpp::SleighSeedInstructionAnalyzer::addDirectDataXrefs(...)`
- `lib/NativeAnalysis.cpp::SleighSeedInstructionAnalyzer::addDirectDataXref(...)`

当前逻辑很保守：只有 `ram` 地址、且目标不是 executable address，才写入 data xref，来源是 `sleigh-pcode-direct-data`。

`NativeProgramState` 已经保存 loader 读出的 memory range。每个 `NativeMemoryRange` 有 `Readable`、`Writable`、`Executable`、`Bytes`、`Start`、`Size`，`lib/NativeAnalysis.cpp` 顶部已有 `readBytes(...)` 可读指定地址的字节。

# Ghidra / Rizin 对照

Ghidra 侧不会只靠 xref kind 表达字符串，它有 Program database 里的 Data：

- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Listing.java::getDataAt(...)`
- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Data.java::hasStringValue()`
- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java::addMemoryReference(...)`
- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java::getReferencesFrom(...)`
- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java::getReferencesTo(...)`

Rizin 侧常见查询是 `axs` 看 string xref，`izz` 看字符串表，`axt` / `axf` / `axl` 看引用。

# native 策略

只在已发现的 direct data xref 上做轻量分类：

1. 目标仍然必须是 direct `ram`，且不是 executable address。
2. 如果目标所在 memory range 可读、不可写、不可执行，再检查它是否像 C 字符串。
3. C 字符串判断最多看 256 字节，要求遇到 `NUL`，并且 `NUL` 前至少 4 个可打印 ASCII 字符；允许 `\t`、`\n`、`\r`。
4. 命中后写 `NativeXrefKind::String`，来源 `sleigh-pcode-direct-string`。
5. 没命中时保持原来的 `NativeXrefKind::Data` 和 `sleigh-pcode-direct-data`。

不做：

- 不扫描 rodata 找字符串。
- 不创建 string symbol / data object。
- 不把同一个 from/to 同时记成 data 和 string，避免统计重复。
- 不猜宽字符、UTF-8、多段字符串和 relocation 间接指针。

# 风险

主要风险是把只读常量表误判成字符串。用“只读非执行内存 + NUL 结尾 + 最小长度 + ASCII”压低误判，但不能完全避免。Bench2 当前 bounded decode 还偏向 init / plt / startup，三条 smoke 里可能没有 direct 引用到字符串；这种情况下 string 计数可能仍是 0。

# 判断标准

1. `notdec-native-discover` 能正常构建。
2. 三个 Bench2 smoke 目标 `--summary-json` 正常输出。
3. 原有 flow/call/data 统计不因为误记 string 而异常下降。
4. 如果 smoke 里没有 string 命中，日志明确记录原因，不强行制造结果。

# 实现记录

## 改动

- `lib/NativeAnalysis.cpp:98` 新增 `findReadableRangeContaining(...)`，复用 loader 保存的 memory range，找包含目标地址的 readable range。
- `lib/NativeAnalysis.cpp:111` 新增 `isLikelyCStringByte(...)`，只接受可打印 ASCII 和 `\t` / `\n` / `\r`。
- `lib/NativeAnalysis.cpp:116` 新增 `looksLikeReadOnlyCString(...)`，只在 readable、non-writable、non-executable range 内检查，最多看 256 字节，要求 NUL 前至少 4 字节。
- `lib/NativeAnalysis.cpp:1472` 更新 `SleighSeedInstructionAnalyzer::addDirectDataXref(...)`：direct `ram` 目标命中字符串规则时写 `NativeXrefKind::String` 和 `sleigh-pcode-direct-string`，否则继续写 data。
- `ARCHITECTURE.md:56` 更新 native discovery 说明，补上 direct data/string 分类规则。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:29` 更新 Stage 5 进度。

## 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
```

JSON 解析：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-json /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached | python3 -m json.tool >/tmp/notdec-xrefs-string-json-check.txt
```

输出文件大小：`3486` 字节。

文本 report：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
```

能正常输出 `xrefs.string: 0`。

Bench2 smoke：

```bash
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
```

结果：

- `vsftpd`: `xrefs.total=16`, `flow=8`, `call=1`, `data=7`, `string=0`, `elapsed=2.78`
- `libuv.so.1.0.0`: `xrefs.total=20`, `flow=9`, `call=2`, `data=9`, `string=0`, `elapsed=2.94`
- `memcached`: `xrefs.total=22`, `flow=8`, `call=1`, `data=13`, `string=0`, `elapsed=2.79`

当前三条 smoke 没有 string 命中。检查现有 xrefs 后，目标多落在 GOT、data、bss 或 rodata 表项，不是字符串起点。又扫了一遍 Bench2 manifest，未发现现阶段 bounded decode 下自然产出的 `string > 0` 目标。

性能：三条 smoke 和上一块同口径时间基本一致，未看到明显下降。
