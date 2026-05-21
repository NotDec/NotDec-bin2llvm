# 原始 prompt

继续按照规划完善 bin2llvm native 链路。本小块属于阶段 6 Lowering integration：修复 P-Code lowering 里函数入口 block 被跳回时，LLVM verifier 报 `Entry block to function must not have predecessors!` 的问题。

# 当前 native 状态

`notdec-native-llvm --all-confirmed` 已经能把多个 confirmed functions 放到同一个 LLVM module。但 `vsftpd` 和 `memcached` 都会跳过 `0x5020`：

```text
Entry block to function must not have predecessors!
label %entry
```

检查 `vsftpd 0x5020` 的 P-Code：

```text
BRANCHIND (ram,0x25a00,8)
...
BRANCH (ram,0x5020,8)
```

也就是说 `.plt` 里有跳回函数入口的 direct branch。当前 `PcodeToLLVM.cpp` 把第一个真实 P-Code block 直接复用成 LLVM function 的 `entry` block，于是后面的 block 会跳到 `%entry`，LLVM verifier 不允许。

# Ghidra / Rizin 对照

Ghidra 侧相关实现：

- `/sn640/ghidra/Ghidra/Features/Decompiler/ghidra_scripts/GraphASTScript.java::buildAST(...)` 把 Ghidra `Function` 交给 `DecompInterface.decompileFunction(...)`，真正 CFG 由 decompiler 内部处理，不需要把机器入口 block 硬映射成 LLVM entry。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/main/java/ghidra/app/decompiler/parallel/ParallelDecompiler.java::decompileFunctions(...)` 批量处理 Function，也是以 Function 为单位交给 decompiler。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/main/java/ghidra/app/decompiler/component/Decompiler.java::decompile(...)` 最终也是通过 decompiler 结果展示函数 CFG，而不是要求函数入口 basic block 没有前驱。

Rizin 侧 `pdf` / `agf` 这类函数视图允许 CFG 中存在回边到函数入口。这个限制是 LLVM IR 自己的 verifier 要求，不是二进制 CFG 语义限制。

# native 策略

在 `PcodeToLLVM.cpp` 里把 LLVM entry block 变成单独 trampoline：

1. LLVM function 仍先创建 `entry` block。
2. 所有真实 P-Code block 都单独创建为 `bb_<address>`，包括第一个 P-Code block。
3. `entry` block 只无条件跳到第一个 P-Code block。
4. 后续 direct branch 可以跳到 `bb_<function_entry>`，不再跳到 LLVM `%entry`。

不做：

- 不改 P-Code CFG 切分策略。
- 不特殊处理 `.plt`。
- 不改变 indirect branch 仍跳 `notdec_exit` 的保守策略。

# 风险

这会让所有生成的 IR 多一个 trampoline block。语义上只是把 LLVM entry 和机器入口 block 分开，成本很小。需要确认单函数 `-f` / `-n` 和 `--all-confirmed` 都仍能通过 verifier。

# 判断标准

1. `notdec-native-llvm -f 0x5020` 能在 `vsftpd` 上生成 `.ll` 并通过 LLVM 22 `llvm-as` / `opt -passes=verify`。
2. `notdec-native-llvm --all-confirmed` 在 `vsftpd` 和 `memcached` 不再因为 `0x5020` 被跳过。
3. `libuv --all-confirmed` 仍正常。
4. 三个 Bench2 discovery smoke 仍正常，性能不明显下降。

# 实现记录

已完成。

改动文件：

- `lib/PcodeToLLVM.cpp:64`：`PcodeLowerer::lower(...)` 在 CFG block 建好后，让 LLVM `%entry` 无条件跳到第一个真实 P-Code block。
- `lib/PcodeToLLVM.cpp:162`：`PcodeLowerer::buildBasicBlocks(...)` 不再把第一个真实 P-Code block 复用成 LLVM `%entry`，而是所有 P-Code block 都创建为 `bb_<address>`。
- `ARCHITECTURE.md:110`：补充 native LLVM lowering 使用 entry trampoline，机器入口 block 可以有 CFG 回边。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:33`：阶段 6 记录本小步完成。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm notdec-native-discover -j2
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd -f 0x5020 -o /tmp/notdec-vsftpd-5020-after.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-vsftpd-5020-after.ll -o /tmp/notdec-vsftpd-5020-after.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-vsftpd-5020-after.bc -o /tmp/notdec-vsftpd-5020-after.opt.bc
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd --all-confirmed -o /tmp/notdec-vsftpd-all-after.ll 2>/tmp/notdec-vsftpd-all-after.err
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-vsftpd-all-after.ll -o /tmp/notdec-vsftpd-all-after.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-vsftpd-all-after.bc -o /tmp/notdec-vsftpd-all-after.opt.bc
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 --all-confirmed -o /tmp/notdec-libuv-all-after.ll 2>/tmp/notdec-libuv-all-after.err
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-libuv-all-after.ll -o /tmp/notdec-libuv-all-after.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-libuv-all-after.bc -o /tmp/notdec-libuv-all-after.opt.bc
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached --all-confirmed -o /tmp/notdec-memcached-all-after.ll 2>/tmp/notdec-memcached-all-after.err
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-memcached-all-after.ll -o /tmp/notdec-memcached-all-after.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-memcached-all-after.bc -o /tmp/notdec-memcached-all-after.opt.bc
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 -n uv_sem_init -o /tmp/notdec-libuv-name-after-trampoline.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-libuv-name-after-trampoline.ll -o /tmp/notdec-libuv-name-after-trampoline.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-libuv-name-after-trampoline.bc -o /tmp/notdec-libuv-name-after-trampoline.opt.bc
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f elapsed=%e /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
```

结果：

- `vsftpd -f 0x5020` 通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。生成 IR 中 `%entry` 只跳到 `%bb_5020`，`%bb_5034` 可以跳回 `%bb_5020`。
- `vsftpd --all-confirmed` 输出 9 个函数，包含 `notdec_native_5020`，stderr 为空，通过 LLVM 22 验证。
- `libuv.so.1.0.0 --all-confirmed` 输出 10 个函数，stderr 为空，通过 LLVM 22 验证。
- `memcached --all-confirmed` 输出 9 个函数，包含 `notdec_native_5020`，stderr 为空，通过 LLVM 22 验证。
- 旧的 `-n uv_sem_init` 仍通过 LLVM 22 验证。
- discovery smoke：
  - `vsftpd`: `xrefs.total=297`, `data=149`, `string=139`, `elapsed=2.89`
  - `libuv.so.1.0.0`: `xrefs.total=24`, `data=13`, `string=0`, `elapsed=3.06`
  - `memcached`: `xrefs.total=179`, `data=68`, `string=102`, `elapsed=2.93`

性能判断：本小块只改变 LLVM IR block 形状，不影响 native discovery。三条 smoke 时间没有明显变化。

评分：

- 实现效果：8/10。修掉了 `.plt` 入口回边导致 verifier 失败的问题，Bench2 两个可执行文件的 `0x5020` 都能进入 module。
- 复杂度：2/10。只把 LLVM entry 和机器入口 block 分开。
- 后期维护成本：2/10。这是 LLVM IR 的正常约束，后续 CFG lowering 可以继续沿用。
