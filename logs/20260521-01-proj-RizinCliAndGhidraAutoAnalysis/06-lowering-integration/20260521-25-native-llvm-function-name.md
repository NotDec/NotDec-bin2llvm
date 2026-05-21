# 原始 prompt

继续按照规划完善 bin2llvm native 链路。本小块仍属于阶段 6 Lowering integration：让 `notdec-native-llvm` 可以按 native confirmed function 的名字生成 LLVM IR。

# 当前 native 状态

上一小块已经补了 `notdec-native-llvm -f <entry>`：

- 工具内部跑 native discovery analyzers。
- 用 `NativeProgramState::functionAt(entry)` 找 confirmed function。
- 从 `function.Entry` 到 `function.RangeEnd` 收集 Sleigh P-Code，再走 `PcodeToLLVM.cpp::buildPcodeModule(...)`。

现在还只能按地址选函数。Bench2 的 `libuv.so.1.0.0` 已经能从符号表确认一些带名字的函数，比如 `uv_sem_init`。如果能按名字 lower，就更接近 Ghidra / Rizin 的函数级工作流。

# Ghidra / Rizin 对照

Ghidra 侧相关实现：

- `/sn640/ghidra/Ghidra/Features/Decompiler/ghidra_scripts/GraphASTScript.java::buildAST(...)` 创建 `DecompInterface`，打开当前 Program，然后对当前 `Function` 调用 `decompileFunction(...)`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/ghidra_scripts/CompareFunctionSizesScript.java::run()` 通过 `currentProgram.getFunctionManager().getFunctionsNoStubs(true)` 遍历已分析函数，再用 `ParallelDecompiler.decompileFunctions(...)` 批量处理。
- `/sn640/ghidra/Ghidra/Features/Decompiler/ghidra_scripts/StringParameterPropagator.java` 多处使用 `currentProgram.getFunctionManager().getFunctionAt(...)` 或 `getFunctionContaining(...)` 从 Program 的函数库取函数。

Ghidra 的模式是：分析先产生 Function，decompiler 再按 Function 对象工作。函数对象有入口、名字、body 等属性。

Rizin 侧常见流程：

- `aaa` / `aaaa` 先分析。
- `afl` 列出函数名和地址。
- `pdf @ sym.name` 或 `s sym.name; pdf` 按函数名查看反汇编。
- `pdg` / `pdd` 这类 decompiler 输出也通常围绕当前函数。

# native 策略

本小块只补最小名字选择：

1. 给 `notdec-native-llvm` 增加 `-n <name>`。
2. `-n` 和 `-f` 一样跑 native discovery。
3. 遍历 `state.functions()`，匹配 `NativeFunction::Name`。
4. 找到唯一函数后复用同一套 `entry -> range_end` lowering。
5. LLVM 函数名使用清理后的名字，如 `uv_sem_init`。

不做：

- 不匹配 aliases。当前 `NativeFunction` 只保存 primary name。
- 不做模糊匹配或 demangle。
- 名字重复时报错，不随便选一个。
- 不修 lowering opcode 覆盖面。

# 风险

- 目前 confirmed function 数量很少，不是所有符号名都能被 `-n` 找到。
- C++ 符号或特殊符号可能不是合法 LLVM 函数名，需要做简单清理。
- `-n` 只是选择方式，IR 语义仍受当前 P-Code lowering 限制。

# 判断标准

1. `notdec-native-llvm` 能构建。
2. `libuv.so.1.0.0 -n uv_sem_init` 能生成 `.ll`。
3. 生成的 `.ll` 里出现 `define void @uv_sem_init()`。
4. 该 `.ll` 能用 LLVM 22 `llvm-as` 和 `opt -passes=verify` 验证。
5. 三个 Bench2 discovery smoke 仍正常，性能不明显下降。

# 实现记录

已完成。

改动文件：

- `tools/notdec-native-llvm.cpp:39`：`CliOptions` 增加 `FunctionName`。
- `tools/notdec-native-llvm.cpp:43`：usage 增加 `-n <name>`。
- `tools/notdec-native-llvm.cpp:60`：参数解析支持 `-n`，并禁止和 `-f`、`-a`、`-l` 混用。
- `tools/notdec-native-llvm.cpp:171`：原 `functionName(...)` 改成 `entryFunctionName(...)`，继续服务 `-f`。
- `tools/notdec-native-llvm.cpp:177`：新增 `sanitizeLlvmFunctionName(...)`，把函数名清成简单 LLVM 名字。
- `tools/notdec-native-llvm.cpp:194`：`resolveFunctionRange(...)` 支持按 `NativeFunction::Name` 查唯一 confirmed function。
- `tools/notdec-native-llvm.cpp:274`：`-n` 模式下用清理后的名字作为 LLVM function name。
- `ARCHITECTURE.md:110`：补充 `notdec-native-llvm -n`。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:33`：阶段 6 记录本小步完成。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm notdec-native-discover -j2
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 -n uv_sem_init -o /tmp/notdec-native-libuv-uv_sem_init-name.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-native-libuv-uv_sem_init-name.ll -o /tmp/notdec-native-libuv-uv_sem_init-name.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-native-libuv-uv_sem_init-name.bc -o /tmp/notdec-native-libuv-uv_sem_init-name.opt.bc
rg -n "define void @uv_sem_init" /tmp/notdec-native-libuv-uv_sem_init-name.ll
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 -f 0x1bb80 -o /tmp/notdec-native-libuv-uv_sem_init-entry.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-native-libuv-uv_sem_init-entry.ll -o /tmp/notdec-native-libuv-uv_sem_init-entry.bc
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
```

结果：

- `libuv.so.1.0.0 -n uv_sem_init` 生成的 `.ll` 中有 `define void @uv_sem_init()`。
- 该 `.ll` 通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
- 旧的 `-f 0x1bb80` 仍能生成 `define void @notdec_native_1bb80()`，并可被 `llvm-as` 汇编。
- discovery smoke：
  - `vsftpd`: `xrefs.total=297`, `data=149`, `string=139`, `elapsed=2.89`
  - `libuv.so.1.0.0`: `xrefs.total=24`, `data=13`, `string=0`, `elapsed=3.04`
  - `memcached`: `xrefs.total=179`, `data=68`, `string=102`, `elapsed=2.82`

性能判断：本小块只影响 `notdec-native-llvm -n` 的函数选择，`notdec-native-discover` 三条 smoke 时间没有明显变化。

评分：

- 实现效果：7/10。已能按 native confirmed function 名字进入 P-Code 到 LLVM IR。
- 复杂度：2/10。只加选择逻辑，复用现有 discovery 和 lowering。
- 后期维护成本：3/10。后续要补 aliases、demangle 或批量函数时，可以在同一入口扩展。
