# 本次用户原始 prompt

继续推进 `external/NotDec-bin2llvm` native bin2llvm 路线。上一段工作已经完成 Stage 1 和 Stage 2 的部分内容；下一步做 Stage 3 的第一个小功能：从 function worklist / seed 出发，用现有 Sleigh / libsla 解码指令摘要并写入 `NativeInstruction`，先不建 CFG。

## 背景

当前 native 状态里已经有 `NativeFunctionSeed`、`NativeFunctionWorkItem` 和 `NativeInstruction`，但 `notdec-native-discover` 还没有真正从 seed 解码任何机器指令。

Ghidra 侧相关实现：

- `EntryPointAnalyzer.java` 负责从入口点启动反汇编。
- `CodeManager.java` / `InstructionDB.java` 负责把接受的 instruction 写入 Program 数据库。
- native libsla 可参考 `sleighexample.cc`，核心是 `Sleigh::oneInstruction(...)` 和 `Sleigh::printAssembly(...)`。

native 侧已有基础：

- `lib/SleighLift.cpp::collectSleighPcode(...)` 已经能初始化 `.sla` / `.pspec` 并循环调用 `oneInstruction(...)`。
- `include/notdec-bin2llvm/NativeAnalysis.h::NativeInstruction` 已经能保存地址、长度、字节、助记符和来源。
- `lib/NativeAnalysis.cpp::NativeProgramState::addInstruction(...)` 已经会拒绝非可执行范围和字节长度不匹配的记录。

## 目标

先打通最小链路：

1. 从 worklist 里取少量函数入口。
2. 用 libsla 解码固定数量的连续指令。
3. 把地址、长度、原始字节、助记符写入 `NativeInstruction`。
4. `notdec-native-discover` 的 report 能看到非 0 instruction 数量。

这一步不做 CFG，不判断 call/jump target，不确认函数范围。

## 技术路线

- 在 `SleighLift` 增加一个指令摘要接口，和现有 P-Code 收集共用同一套 Sleigh spec 初始化逻辑。
- 在 native analysis 里增加一个低优先级 analyzer，运行在 seed 收集之后、report 之前。
- 自动 spec 先只支持 x86-64 ELF，使用 `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64.sla` 和同目录 `x86-64.pspec`。
- 为避免 Bench2 变慢，默认只解码前几个 worklist item，每个入口只解码少量指令。

## 风险

- 线性解码遇到跳转后会继续往下读，所以结果只能当“已解码指令样本”，不能当函数语义。
- 目前只支持 x86-64，其他架构要先记录 note，不失败。
- 助记符来自 `printAssembly(...)`，不同 Ghidra 版本格式可能有小差异。

## 判断标准

- `notdec-native-discover` 能在 Bench2 的 `vsftpd`、`libuv.so.1.0.0`、`memcached` 上运行成功。
- report 的 instruction total 大于 0。
- 同口径运行时间没有明显变慢。

## 实现记录

本小步已完成。

改动文件和函数：

- `include/notdec-bin2llvm/SleighLift.h:27` 新增 `SleighInstructionSummary`，只保存地址、长度、助记符和 operand 文本。
- `include/notdec-bin2llvm/SleighLift.h:47` 新增 `collectSleighInstructionSummaries(...)`。
- `lib/SleighLift.cpp:141` 新增 `AssemblyCollector`，承接 `Sleigh::printAssembly(...)` 输出。
- `lib/SleighLift.cpp:244` 新增 `initializeSleighEngine(...)`，把 `.sla` / `.pspec` 初始化逻辑从 P-Code 收集里提出来复用。
- `lib/SleighLift.cpp:307` 调整 `collectSleighPcode(...)`，继续使用同一套初始化逻辑。
- `lib/SleighLift.cpp:346` 实现 `collectSleighInstructionSummaries(...)`，按最大指令数和最大字节数线性解码。
- `include/notdec-bin2llvm/NativeAnalysis.h:300` 新增 `createSleighSeedInstructionAnalyzer()`。
- `lib/NativeAnalysis.cpp:1138` 新增 `SleighSeedInstructionAnalyzer`。它只取前 8 个 worklist seed，每个 seed 最多 8 条 / 64 字节，并把结果写入 `NativeInstruction`。
- `lib/NativeAnalysis.cpp:1178` 自动选择 x86-64 Sleigh spec；非 x86-64 只记录 note，不失败。
- `lib/NativeAnalysis.cpp:1203` 解码前把长度截到所在 executable range，避免在 `.fini` 尾部越界。
- `tools/notdec-native-discover.cpp:56` 把 analyzer 接入 discover pipeline。
- `lib/CMakeLists.txt:56` 给 native library 注入默认 Ghidra source dir。

没有做的事：

- 没有确认函数。
- 没有建 basic block。
- 没有从指令 operand 里提取 call / jump / data xref。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
```

验证结果：

- `vsftpd`：function seeds 186，worklist 186，instructions 55，时间 1.59s。
- `libuv.so.1.0.0`：function seeds 484，worklist 484，instructions 60，时间 1.62s。
- `memcached`：function seeds 258，worklist 258，instructions 55，时间 1.63s。

性能说明：

- 本次 analyzer 有硬上限：最多 8 个 seed、每个 seed 最多 8 条指令，所以当前 Bench2 smoke 仍在约 1.6s。
- 尝试用上一提交 `6508c71` 建独立 worktree 做可执行基线，但新 build 会重新编译完整 LIEF，耗时过高，已中止并删除临时 worktree。因此这里记录当前同口径 smoke 时间，下一步如果要做严格对比，应复用同一个依赖 build cache。

评分：

- 实现效果：7/10。最小链路打通，能看到真实 decoded instruction。
- 复杂度：6/10。复用了 Sleigh 初始化，新增 analyzer 有硬上限，但每个 seed 仍会重新初始化 Sleigh，后续应合并为一次初始化多入口解码。
- 维护成本：6/10。接口小，风险主要是当前线性解码不能代表 CFG。
