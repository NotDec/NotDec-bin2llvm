# Native Bench2 Smoke Script

## 原始 prompt

```text
在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis里面根据划分的几个关键宏观步骤，创建对应的文件夹，然后在prompt中要求，对应步骤的规划和修改必须放到对应的文件夹里。

本次目标是按照规划，完善bin2llvm项目反汇编转IR的native链路（即不走GhidraScript的链路，而是走libsla，libdecomp的这个链路），从Ghidra和rizin那边复刻足够多的功能。每当实现一部分功能后，就看看能否利用bench2里面这些项目去做初步的测试： （vsftpd可执行文件，libuv动态链接库，memcached可执行文件），测试实现是否正确。规划中每一部分都完成后即可停止。在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis 下创建一个PROGRESS.md里追踪计划的实现。完成一个部分后，同步更新 项目顶层的ARCHITECTURE.md 反映实现的功能模块的架构。

约束：根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## 当前目标和已有 native 状态

native 链路现在已经能从 Bench2 的 ELF / shared object 里发现一批 confirmed function，
也能用 `notdec-native-llvm --all-confirmed` 输出可被 LLVM 22 verify 的 IR。

现在的问题是：每个小功能完成后都手工跑三组命令，容易漏掉同口径记录。Stage 7 先补一个
很小的 smoke 脚本，把当前关注的三个目标固定下来：

- `/sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd`
- `/sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0`
- `/sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached`

## Ghidra 相关实现

Ghidra 的 headless 路线不是简单跑一个 pass，而是有固定入口把导入、分析、脚本和日志串起来：

- `/sn640/ghidra/Ghidra/Features/Base/src/main/java/ghidra/app/util/headless/AnalyzeHeadless.java`
  - `main(...)` 解析 headless 参数。
  - 调用 `HeadlessAnalyzer.processLocal(...)` 处理本地项目和输入文件。
- `/sn640/ghidra/Ghidra/Features/Base/src/main/java/ghidra/app/util/headless/HeadlessAnalyzer.java`
  - `processLocal(...)` 组织本地文件导入和分析。
  - `analyzeProgram(...)` 串起 pre script、auto analysis、post script。
  - `runScriptsList(...)` 执行脚本列表并决定是否继续。
- `/sn640/ghidra/Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/AutoAnalysisManager.java`
  - `startAnalysis(...)` 启动分析。
  - `waitForAnalysis(...)` 等待分析完成。
  - `schedule(...)` / `scheduleOneTimeAnalysis(...)` 调度具体 analyzer。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/main/java/ghidra/app/decompiler/parallel/ParallelDecompiler.java`
  - `decompileFunctions(...)` 对函数集合做批量 decompile。

这里要复刻的不是 Ghidra 的调度框架，而是它的基本习惯：固定输入集合、固定分析入口、固定输出日志，
让每次改动后可以复查结果。

## native 侧复刻策略

先做最小脚本，不引入测试框架：

1. 使用已有构建目录 `/tmp/notdec-bin2llvm-build`。
2. 对三个 Bench2 目标分别运行：
   - `notdec-native-discover --summary-json`
   - `notdec-native-llvm --all-confirmed`
   - LLVM 22 的 `llvm-as`
   - LLVM 22 的 `opt -passes=verify`
3. 每个目标输出 `.summary.json`、`.ll`、`.bc`、`.opt.bc` 和 stderr 日志。
4. 脚本只检查 confirmed function 数不是 0，以及 LLVM verify 成功。

暂时不做：

- 不和 GhidraScript 输出逐函数 diff。
- 不固定数量阈值，避免 native 还在补功能时因为合理增长而失败。
- 不把脚本接入 CTest，先作为手动 smoke 入口。

## 判断标准

1. 脚本能在当前构建目录下跑完三个 Bench2 目标。
2. 三个目标都有非 0 confirmed function。
3. 三个目标的 `--all-confirmed` IR 都能通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
4. 输出目录里留下每个目标的 summary、IR、bitcode 和日志，方便后续对比。

## 风险

1. `notdec-native-llvm --all-confirmed` 当前会重新跑 native discovery，脚本总耗时会比只跑 discover 更高。
2. smoke 只证明 IR 可验证和基础数量不为 0，不能证明语义完全正确。
3. Bench2 路径是本地绝对路径，后续如要迁移环境需要加参数覆盖。

## 实现记录

### 修改文件和函数

1. `scripts/bench2-native-smoke.sh:1`
   - 新增手动 smoke 脚本。
   - `usage()`：说明 `--build-dir`、`--bench2-root`、`--out-dir`、`--llvm-bin`。
   - `require_executable()` / `require_file()`：提前检查工具和 Bench2 目标是否存在。
   - 主循环固定跑 `vsftpd`、`libuv`、`memcached`，每个目标依次执行 discovery、native LLVM lowering、`llvm-as`、`opt -passes=verify`。
2. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/07-bench2-regression/README.md:17`
   - 记录 Stage 7 当前 smoke 入口和执行步骤。
3. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:38`
   - 标记 Stage 7 已补 Bench2 native smoke 脚本。
4. `ARCHITECTURE.md:81`
   - 在目录说明里加入 `scripts/`。
   - 在文件职责里加入 `scripts/bench2-native-smoke.sh` 的用途。

### 验证命令

```bash
bash -n scripts/bench2-native-smoke.sh
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm notdec-native-discover -j2
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-28
```

### Bench2 结果

输出目录：

```text
/tmp/notdec-bin2llvm-bench2-smoke-20260521-28
```

三组目标均通过 `notdec-native-discover --summary-json`、`notdec-native-llvm --all-confirmed`、
LLVM 22 `llvm-as` 和 LLVM 22 `opt -passes=verify`。

```text
vsftpd ok elapsed=8s
libuv ok elapsed=8s
memcached ok elapsed=8s
```

summary 关键数据：

```text
vsftpd: confirmed_functions=9, basic_blocks=31, instructions=80, xrefs.total=297, data=149, string=139
libuv: confirmed_functions=10, basic_blocks=32, instructions=93, xrefs.total=24, data=13, string=0
memcached: confirmed_functions=9, basic_blocks=31, instructions=80, xrefs.total=179, data=68, string=102
```

所有 `.stdout` / `.stderr` 日志大小都是 0。

### 性能和影响

这次没有改 native analysis / lowering 代码，不会改变生成 IR 的性能。
脚本本身跑三个 Bench2 目标总耗时约 24 秒，主要成本来自当前
`notdec-native-llvm --all-confirmed` 会再次运行 native discovery。

实现效果：4/5。以后每个小块都能用同一条命令跑三个目标。
复杂度：1/5。只是 Bash 脚本和文档。
维护成本：1/5。目标路径和工具路径都可用参数覆盖。
