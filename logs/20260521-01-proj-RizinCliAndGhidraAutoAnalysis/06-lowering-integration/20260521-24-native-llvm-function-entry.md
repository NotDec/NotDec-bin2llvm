# 原始 prompt

继续按照规划完善 bin2llvm native 链路。本小块属于阶段 6 Lowering integration：让 `notdec-native-llvm` 可以按 native discovery 已确认的函数入口生成 LLVM IR，而不是只能手工传 `-a` / `-l`。

# 当前 native 状态

现在已有两条还没真正接上的链路：

1. `notdec-native-discover` 会跑 native analyzers，得到 `NativeFunction`、`NativeBasicBlock`、`NativeInstruction`、xref 等状态。
2. `notdec-native-llvm` 会从 ELF 指定地址和长度收集 Sleigh P-Code，再调用 `PcodeToLLVM.cpp::buildPcodeModule(...)` 生成 LLVM IR。

问题是 `notdec-native-llvm` 现在不消费 native discovery 的 confirmed function。用户必须手工知道地址和长度，这不算 native 反汇编到 IR 的闭环。

# Ghidra / Rizin 对照

Ghidra 侧相关实现：

- `/sn640/ghidra/Ghidra/Features/Decompiler/ghidra_scripts/GraphASTScript.java::run()` 通过 `DecompInterface` 打开当前 Program。
- `/sn640/ghidra/Ghidra/Features/Decompiler/ghidra_scripts/GraphASTScript.java::run()` 调用 `DecompInterface.decompileFunction(...)`，输入是 Ghidra 已识别的 `Function`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/ghidra_scripts/GraphASTScript.java::run()` 从 `DecompileResults.getHighFunction()` 取 decompiler 结果。
- `/sn640/ghidra/Ghidra/Features/Decompiler/ghidra_scripts/CompareFunctionSizesScript.java` 也通过 `ParallelDecompiler.decompileFunctions(...)` 对已发现函数批量 decompile，并遍历 `HighFunction.getPcodeOps()`。

Ghidra 的关键点不是“用户手填一段地址”，而是先由分析阶段建立 Function，再由 decompiler 消费 Function。

Rizin 侧常见流程也是先分析函数，再对函数反汇编或反编译：

- `aaa` / `aaaa` 做分析。
- `afl` 看函数列表。
- `pdf @ <func>` 看函数反汇编。
- `pdd` / `pdg` 这类 decompiler 输出通常也是围绕当前函数。

# native 策略

本小块先做最小闭环：

1. 给 `notdec-native-llvm` 增加 `-f <entry>`。
2. 传 `-f` 时，工具内部跑和 `notdec-native-discover` 同一组 native analyzers。
3. 用 `NativeProgramState::functionAt(entry)` 找 confirmed function。
4. 用 `function.Entry` 到 `function.RangeEnd` 的范围收集 Sleigh P-Code。
5. 继续复用现有 `buildPcodeModule(...)`，不在本小块改 lowering 语义。
6. 原来的 `-a <addr> -l <len>` 保留，方便手工定位。

先不做：

- 不一次性 lowering 全模块。
- 不按 native basic block 精确拼 P-Code，只用当前函数保守 range。
- 不修 `PcodeToLLVM` 的未支持 opcode。
- 不承诺 IR 语义已经完整，只先打通 discovery -> P-Code -> LLVM IR 的命令行闭环。

# 风险

- 当前 confirmed function range 还很保守，可能包含未完全属于该函数的字节。先用 `entry` 到 `range_end`，避免 range_start 小于 entry 时从前一个 block 开始 lower。
- `PcodeToLLVM` 仍是早期实现，某些函数可能遇到未支持 opcode。Bench2 smoke 先选当前已知能 lower 的小函数。
- 这一步不会增加 xref / function 数量，判断标准主要看生成的 `.ll` 能否被 LLVM 22 接受。

# 判断标准

1. `notdec-native-llvm` 能构建。
2. `notdec-native-llvm <elf> -f 0x5000 -o ...` 能在 `vsftpd` 上生成 `.ll`。
3. 生成的 `.ll` 能用 `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as` 汇编，并能用同版本 `opt -passes=verify` 验证。
4. 三个 Bench2 smoke 目标仍能跑 `notdec-native-discover --summary-json`，时间不明显变慢。

# 实现记录

已完成。

改动文件：

- `tools/notdec-native-llvm.cpp:3`：引入 `NativeAnalysis.h`，让 native LLVM 入口可以复用 discovery analyzers。
- `tools/notdec-native-llvm.cpp:32`：`CliOptions` 增加 `FunctionEntry`。
- `tools/notdec-native-llvm.cpp:41`：usage 改成支持 `-a/-l` 或 `-f` 两种模式。
- `tools/notdec-native-llvm.cpp:57`：参数解析支持 `-f <entry>`，并禁止和 `-a` / `-l` 混用。
- `tools/notdec-native-llvm.cpp:161`：新增 `functionName(...)`，让 `-f 0x5000` 生成 `notdec_native_5000`。
- `tools/notdec-native-llvm.cpp:167`：新增 `resolveFunctionRange(...)`，跑 native analyzers，用 `NativeProgramState::functionAt(...)` 找 confirmed function，并设置 lowering 地址和长度。
- `tools/notdec-native-llvm.cpp:236`：main 中在收集 P-Code 前解析 `-f` 函数范围。
- `tools/notdec-native-llvm.cpp:252`：`-f` 模式下设置 LLVM 函数名。
- `ARCHITECTURE.md:110`：补充 `notdec-native-llvm -f` 的工具职责。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:33`：阶段 6 记录本小步完成。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm notdec-native-discover -j2
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd -f 0x5000 -o /tmp/notdec-native-vsftpd-f5000.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-native-vsftpd-f5000.ll -o /tmp/notdec-native-vsftpd-f5000.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-native-vsftpd-f5000.bc -o /tmp/notdec-native-vsftpd-f5000.opt.bc
rg -n "define void @notdec_native_5000" /tmp/notdec-native-vsftpd-f5000.ll
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd -a 0x5000 -l 0x1b -o /tmp/notdec-native-vsftpd-manual.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-native-vsftpd-manual.ll -o /tmp/notdec-native-vsftpd-manual.bc
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
```

结果：

- `vsftpd -f 0x5000` 生成的 `.ll` 中有 `define void @notdec_native_5000()`。
- 该 `.ll` 通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
- 手工 `-a 0x5000 -l 0x1b` 仍能生成可汇编 `.ll`。
- discovery smoke：
  - `vsftpd`: `xrefs.total=297`, `data=149`, `string=139`, `elapsed=2.76`
  - `libuv.so.1.0.0`: `xrefs.total=24`, `data=13`, `string=0`, `elapsed=3.01`
  - `memcached`: `xrefs.total=179`, `data=68`, `string=102`, `elapsed=2.85`

性能判断：本小块只影响 `notdec-native-llvm -f`，`notdec-native-discover` 三条 smoke 时间没有明显变化。

评分：

- 实现效果：7/10。已经打通 confirmed function -> P-Code -> LLVM IR 的最小闭环。
- 复杂度：3/10。只复用现有 analyzers 和 Pcode lowering，没有改 lowering 语义。
- 后期维护成本：3/10。后续可以改成按 native basic block 精确拼接，或一次输出多个函数。
