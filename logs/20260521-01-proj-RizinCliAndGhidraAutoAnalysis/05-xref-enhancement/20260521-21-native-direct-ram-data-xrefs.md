# 本次用户原始 prompt

继续按照规划完善 bin2llvm native 链路。阶段 5 先做一小块 xref 增强：从 Sleigh P-Code 里识别 direct `ram` 数据访问，记录 data xref。

## 背景

当前 native xref 只有 direct control-flow：`CALL`、`BRANCH`、`CBRANCH`。Bench2 smoke 里能看到 call/flow，但 GOT、全局变量、rodata 这类 data ref 还没有进入 state。阶段 5 先补最保守的一类：P-Code 里已经是直接 `ram` varnode 的数据读写。

Ghidra 侧相关实现：

- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java::addMemoryReference(...)` 用来记录内存引用。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java::getReferencesFrom(...)` 和 `getReferencesTo(...)` 查询引用。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/X86FunctionAnalyzer.java` 这类 analyzer 会基于指令语义补引用和函数信息。
- Ghidra 的核心思路是：指令/语义分析产生 reference，统一写入 `ReferenceManager`，后续函数、xref、图查询都消费同一份事实。

Rizin 侧相关实现：

- `aar` 分析当前 section 或范围内引用。
- `aad` 分析 data references to code。
- `axd` 表示 data xref。
- `axl` / `axf` / `axt` 查询 xref。

native 侧已有状态：

- `NativeXrefKind::Data` 已存在。
- `NativeProgramState::addXref(...)` 会维护 from/to 索引。
- `SleighSeedInstructionAnalyzer` 已经拿到 `PcodeProgram`，现在只从里面提取 direct control-flow。
- 对 x86-64，Sleigh 已经能把部分 RIP-relative 访问表示成直接 `ram` varnode。例如 `MOV RAX,[0x25ff0]` 会变成 `COPY (ram,0x25ff0,8)`。

## 目标

1. 从 P-Code op 的 input/output 中识别 direct `ram` varnode。
2. 对非 control-flow op，记录 `NativeXrefKind::Data`。
3. 只记录目标不是 executable address 的 data xref，避免把 code target 混成 data。
4. 复用现有 xref 查询和 JSON 输出，不新增 CLI。
5. 不处理寄存器间接、栈访问、计算出来的地址、string 分类。

## 技术路线

- 在 `SleighSeedInstructionAnalyzer` 的 P-Code 扫描里增加 direct data xref 提取。
- 把现有 `addUniqueXref(...)` 加一个 source 参数，control-flow 仍用 `sleigh-pcode-direct-flow`，data xref 用 `sleigh-pcode-direct-data`。
- 新增 `addDirectDataXrefs(...)`，遍历 op output 和 inputs 的 `ram` varnode。
- 跳过 `CALL`、`BRANCH`、`CBRANCH`、`CALLIND`、`BRANCHIND`、`RETURN`。
- 使用本轮局部 seen set 去重。

## 风险

- 这一步只能覆盖 Sleigh 已经解析成 direct `ram` varnode 的访问，覆盖面有限。
- `ram` varnode 可能来自不同语义场景，先只保守排除 executable address。
- string xref 暂不做，需要后续基于 memory bytes 和终止符判断。

## 判断标准

- Bench2 三个 smoke 的 `--summary-json` 中 `xrefs.data` 大于 0。
- `vsftpd --xrefs-to-json 0x25ff0` 能查到 data xref。
- 原 call/flow xref 数量不应下降。
- 全量 `--xrefs-json` 仍能被 JSON parser 解析。
- 用时不应明显增加。

## 实现记录

已完成。

改动文件：

- `lib/NativeAnalysis.cpp:1364`：`addDirectControlFlow(...)` 在保留 call/flow/indirect/return 逻辑的同时，对非控制流 op 调用 data xref 提取。
- `lib/NativeAnalysis.cpp:1372`、`lib/NativeAnalysis.cpp:1391`：`addUniqueXref(...)` 增加 source 参数，control-flow 仍标记为 `sleigh-pcode-direct-flow`。
- `lib/NativeAnalysis.cpp:1411`：新增 `addDirectDataXrefs(...)`，遍历 P-Code op 的 output 和 inputs。
- `lib/NativeAnalysis.cpp:1423`：新增 `addDirectDataXref(...)`，只接受 `ram` varnode，且排除 executable address，记录 `NativeXrefKind::Data` 和 `sleigh-pcode-direct-data`。
- `lib/NativeAnalysis.cpp:1449`：`addUniqueXref(...)` 支持传入 source，继续用本轮 seen set 去重。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:29`：阶段 5 记录本小步完成。
- `ARCHITECTURE.md:53`：补充 direct `ram` data xref 的实现说明。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-to-json 0x25ff0 /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool >/tmp/notdec-xrefs-data-json-check.txt
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-from-json 0x8327 /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd | python3 -m json.tool >/tmp/notdec-xrefs-from-data-check.txt
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
```

Bench2 smoke 结果：

- `vsftpd`：`xrefs.total=16`，`flow=8`，`call=1`，`data=7`，2.86s。
- `libuv.so.1.0.0`：`xrefs.total=20`，`flow=9`，`call=2`，`data=9`，3.03s。
- `memcached`：`xrefs.total=22`，`flow=8`，`call=1`，`data=13`，2.88s。
- `vsftpd --xrefs-to-json 0x25ff0`：`count=1`，查到 `0x5008 -> 0x25ff0` 的 `data` xref。
- `vsftpd --xrefs-json` 可被 JSON parser 解析。
- `vsftpd --xrefs-from-json 0x8327` 仍可解析，旧 call xref 没丢。

性能判断：只在已有 P-Code 扫描里多看 input/output varnode。三条 smoke 仍在 3 秒左右，没有看到明显变慢。

评分：

- 实现效果：8/10。能记录最常见的 direct data ref，Bench2 三个样本都有 data xref。
- 复杂度：3/10。逻辑仍在 Sleigh analyzer 内，复用现有 xref state。
- 后期维护成本：3/10。后续可继续补 string 分类、relocation guided ref 和寄存器间接地址，不需要改接口。
