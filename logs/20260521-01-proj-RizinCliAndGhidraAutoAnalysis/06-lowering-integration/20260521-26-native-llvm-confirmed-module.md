# 原始 prompt

继续按照规划完善 bin2llvm native 链路。本小块属于阶段 6 Lowering integration：让 `notdec-native-llvm` 能把 native discovery 当前确认且可验证的多个函数输出到同一个 LLVM module。

# 当前 native 状态

现在 `notdec-native-llvm` 已经支持三种单函数入口：

- `-a <address> -l <length>`：手工指定范围。
- `-f <entry>`：按 native confirmed function 入口找范围。
- `-n <name>`：按 native confirmed function 名字找范围。

这些都只输出一个 LLVM function。GhidraScript 路线的 `ExportHeritageModule.java` 是模块级导出，`HeritageToLLVM.cpp` 也能处理模块级 JSON。native 路线如果只能单函数输出，还不算真正接近“项目二进制 -> IR module”的链路。

# Ghidra / Rizin 对照

Ghidra 侧相关实现：

- `ghidra_scripts/ExportHeritageModule.java::run()` 遍历 Ghidra `FunctionManager` 里的函数，逐个 decompile，再写入同一个 module JSON。
- `/sn640/ghidra/Ghidra/Features/Decompiler/ghidra_scripts/CompareFunctionSizesScript.java::run()` 通过 `currentProgram.getFunctionManager().getFunctionsNoStubs(true)` 取函数列表，并用 `ParallelDecompiler.decompileFunctions(...)` 批量 decompile。
- `/sn640/ghidra/Ghidra/Features/Decompiler/ghidra_scripts/GraphASTScript.java::buildAST(...)` 展示单个函数的 `DecompInterface.decompileFunction(...)`。批量时本质上也是对一组 Function 重复这个步骤。

Rizin 侧常见流程：

- `aaa` / `aaaa` 先分析。
- `afl` 得到函数列表。
- 对列表里的函数执行 `pdf`、`pdg`、`pdd`，再把结果按函数组织起来。

# native 策略

本小块先做保守闭环：

1. 给 `notdec-native-llvm` 增加 `--all-confirmed`。
2. 内部跑 native discovery，遍历 `state.functions()`。
3. 对每个函数用 `entry -> range_end` 收集 P-Code。
4. 先在临时 module 中 lower 并用 LLVM verifier 检查；可验证才追加到最终 module。
5. 最终 module 共享 register globals 和 `notdec_ram`，避免每个函数生成一套 `RAX.1` / `notdec_ram.1`。
6. 跳过当前 P-Code lowering 还不能验证的函数，向 stderr 记录原因。

不做：

- 不保证所有 confirmed function 都能 lower。现在 `.plt` 这类函数仍可能因为 CFG 入口前驱问题被 verifier 拒绝。
- 不做跨函数 call 链接。
- 不做完整 ABI / 参数 / 返回值。

# 风险

- 多函数 module 会暴露已有 lowering 的共享状态问题，比如寄存器 global 和 `notdec_ram` 复用。
- 跳过不可验证函数会让输出不是完整 module，但比输出坏 IR 更适合当前阶段。
- 每个函数先临时验证一次，开销会增加；当前 bounded confirmed functions 数量很小，可以接受。

# 判断标准

1. `notdec-native-llvm --all-confirmed` 能在 Bench2 三个目标上生成 `.ll`。
2. 生成的 `.ll` 能用 LLVM 22 `llvm-as` 和 `opt -passes=verify` 验证。
3. `libuv.so.1.0.0` 输出里至少包含多个函数，例如 `uv_library_shutdown` 和 `uv_sem_init`。
4. stderr 明确记录跳过了哪些当前不可验证的函数。
5. 三个 Bench2 discovery smoke 仍正常，性能不明显下降。

# 实现记录

已完成。

改动文件：

- `include/notdec-bin2llvm/PcodeToLLVM.h:24`：新增 `appendPcodeFunction(...)`，允许向已有 LLVM module 追加一个 P-Code 函数。
- `lib/PcodeToLLVM.cpp:546`：`PcodeLowerer::memoryGlobal(...)` 复用已有 `notdec_ram` global，避免多函数 module 生成 `notdec_ram.1`。
- `lib/PcodeToLLVM.cpp:729`：实现 `appendPcodeFunction(...)`，`buildPcodeModule(...)` 改为复用它。
- `lib/RegisterStorage.cpp:134`：`RegisterStorage::globalFor(...)` 复用已有同名同类型 register global，避免多函数 module 生成 `RAX.1`、`RSP.1`。
- `tools/notdec-native-llvm.cpp:41`：`CliOptions` 增加 `AllConfirmed`。
- `tools/notdec-native-llvm.cpp:45`：usage 增加 `--all-confirmed`。
- `tools/notdec-native-llvm.cpp:63`：参数解析允许无值 flag `--all-confirmed`，并要求 `-a/-l`、`-f`、`-n`、`--all-confirmed` 四种选择只出现一种。
- `tools/notdec-native-llvm.cpp:209`：新增 `runNativeDiscovery(...)`，供单函数和多函数模式复用。
- `tools/notdec-native-llvm.cpp:271`：新增多函数命名和 module verifier helper。
- `tools/notdec-native-llvm.cpp:301`：新增 `buildConfirmedModule(...)`，遍历 confirmed functions，先临时 lower + verify，成功后追加到最终 module。
- `tools/notdec-native-llvm.cpp:413`：main 中接入 `--all-confirmed`。
- `ARCHITECTURE.md:110`：补充 `notdec-native-llvm --all-confirmed`。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:33`：阶段 6 记录本小步完成。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm notdec-native-discover -j2
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd --all-confirmed -o /tmp/notdec-native-vsftpd-all.ll 2>/tmp/notdec-native-vsftpd-all.err
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-native-vsftpd-all.ll -o /tmp/notdec-native-vsftpd-all.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-native-vsftpd-all.bc -o /tmp/notdec-native-vsftpd-all.opt.bc
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 --all-confirmed -o /tmp/notdec-native-libuv-all.ll 2>/tmp/notdec-native-libuv-all.err
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-native-libuv-all.ll -o /tmp/notdec-native-libuv-all.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-native-libuv-all.bc -o /tmp/notdec-native-libuv-all.opt.bc
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached --all-confirmed -o /tmp/notdec-native-memcached-all.ll 2>/tmp/notdec-native-memcached-all.err
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-native-memcached-all.ll -o /tmp/notdec-native-memcached-all.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-native-memcached-all.bc -o /tmp/notdec-native-memcached-all.opt.bc
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 -n uv_sem_init -o /tmp/notdec-native-libuv-name-regress.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-native-libuv-name-regress.ll -o /tmp/notdec-native-libuv-name-regress.bc
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
```

结果：

- `vsftpd --all-confirmed` 输出 8 个函数，跳过 `0x5020`，原因是 `Entry block to function must not have predecessors!`。
- `libuv.so.1.0.0 --all-confirmed` 输出 10 个函数，包括 `uv_library_shutdown`、`uv_sem_init`、`uv_key_delete`、`uv_loop_fork`，stderr 为空。
- `memcached --all-confirmed` 输出 8 个函数，跳过 `0x5020`，原因同上。
- 三个 `.ll` 都通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
- `libuv` 多函数 `.ll` 中只有共享的 `@RAX` 和 `@notdec_ram`，没有 `@RAX.1` / `@notdec_ram.1`。
- 旧的 `-n uv_sem_init` 仍能生成 `define void @uv_sem_init()`，并可被 `llvm-as` 汇编。
- discovery smoke：
  - `vsftpd`: `xrefs.total=297`, `data=149`, `string=139`, `elapsed=2.87`
  - `libuv.so.1.0.0`: `xrefs.total=24`, `data=13`, `string=0`, `elapsed=3.03`
  - `memcached`: `xrefs.total=179`, `data=68`, `string=102`, `elapsed=2.88`

性能判断：本小块只增加 `notdec-native-llvm --all-confirmed` 路径；`notdec-native-discover` 三条 smoke 时间没有明显变化。`--all-confirmed` 因为每个函数先临时 verify 一次，会比单函数慢，但当前 confirmed function 数量很小。

评分：

- 实现效果：8/10。已经能从 native confirmed function 列表生成同一个可验证 LLVM module。
- 复杂度：5/10。新增了 append API 和共享 global 复用，比单函数路径复杂一些。
- 后期维护成本：4/10。后续需要修 `.plt` 入口前驱问题，让跳过数量下降；接口本身可以继续保留。
