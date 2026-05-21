# Native LLVM Direct Call Target

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

native lowering 已经可以把 confirmed function 输出到同一个 LLVM module，并且 LLVM `%entry`
只是跳板，真实机器入口在 `bb_<address>`。现在还缺一块很实际的能力：模块内 direct `CALL`
仍然被降成通用 helper call，没有真正连到同一 module 里的已知函数。

这次只补最小闭环：

- 如果 `CALL` 的目标地址就是本轮 `--all-confirmed` 里某个 confirmed function 的入口，
  就直接调用那个 LLVM function。
- 如果目标不是本模块里的 confirmed function，继续走现有 helper call。
- 先不处理带输出值的 call，也不补参数类型恢复。

## Ghidra 相关实现

Ghidra 的 decompiler 已经把 direct call 和 target 函数入口连起来了：

- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/pcode/PcodeOp.java`
  - `CALL` / `CALLIND` / `CALLOTHER` 定义了 P-Code 级别的调用语义。
  - `getInput(int)` 取 direct call 的目标 varnode。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/main/java/ghidra/app/decompiler/component/DecompilerUtils.java`
  - 读取 `CALL` 的目标地址，按 function entry 找回目标函数。
- `ghidra_scripts/ExportHeritageModule.java`
  - `directCallTargetAddress(...)` / `directCallTargetName(...)` 会把 `CALL` 的目标地址和函数名写进导出结果。

native 侧要复刻的是这个最关键的连接关系：已确认函数之间的直接调用，不要再写成匿名 helper。

## native 侧复刻策略

1. 在 `--all-confirmed` 路径里，先给每个 confirmed function 规划稳定的 LLVM 名字。
2. lower P-Code `CALL` 时，如果目标地址能在这份名字表里找到，就发成普通 LLVM call。
3. 只有找不到目标，或者 call 形式不是当前支持的最小形式时，才继续走 helper call。

暂时不做：

- 不做参数个数和类型恢复。
- 不做 `CALLIND` 的目标解析。
- 不把这条逻辑扩到单函数 `-f/-n` 模式。

## 判断标准

1. `--all-confirmed` 生成的 module 里，confirmed function 间的 direct `CALL` 不再落到 `notdec_pcode_CALL_*` helper。
2. Bench2 smoke 继续通过 LLVM 22 verify。
3. 现有 helper call 路径不被破坏。

## 风险

1. 如果误把未知 call 直接连到本地函数，会比 helper call 更容易把语义做错。
2. 当前只处理无输出的 direct call，覆盖面有限，但比完全匿名的 helper call 更接近真实模块调用图。
3. 这个改动会依赖 `--all-confirmed` 的符号规划，单函数模式先保持原样更稳。

## 实现记录

### 修改文件和函数

1. `include/notdec-bin2llvm/PcodeToLLVM.h:17`
   - `PcodeLoweringConfig` 新增 `DirectCallTargets`。
   - 这个表只表达“已确认函数入口地址 -> 本 module 内 LLVM symbol”，避免 P-Code lowering 自己去猜函数名。
2. `lib/PcodeToLLVM.cpp:51`
   - `PcodeLowerer` 构造函数接收 `PcodeLoweringConfig`。
   - `PcodeLowerer::lowerCall(...)` 新增最小 direct call lowering：无输出 `CALL` 且 target 在 `DirectCallTargets` 时，生成 `call void @target()`。
   - `PcodeLowerer::lowerOp(...)` 把 `PcodeOpcode::Call` 接到 `lowerCall(...)`，`CALLIND` / `CALLOTHER` 继续走 helper。
3. `tools/notdec-native-llvm.cpp:301`
   - `buildConfirmedModule(...)` 在 lower 任何函数前，先为所有非空 range 的 confirmed function 规划 `namesByEntry`。
   - 每个函数 lowering 时复用同一份名字表，保证模块内直接调用的 symbol 和函数定义名字一致。
4. `ARCHITECTURE.md:111`
   - 记录 `--all-confirmed` 会把命中的 direct `CALL` lowered 成 LLVM direct call。
5. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:33`
   - 更新 Stage 6 完成项。

### 验证命令

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm notdec-native-discover -j2
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-29
rg -n "call void @notdec_native|call void @[_A-Za-z]" /tmp/notdec-bin2llvm-bench2-smoke-20260521-29/*.all-confirmed.ll
rg -n "notdec_pcode_CALL" /tmp/notdec-bin2llvm-bench2-smoke-20260521-29/*.all-confirmed.ll
```

### Bench2 结果

三个目标都通过 `notdec-native-discover --summary-json`、`notdec-native-llvm --all-confirmed`、
LLVM 22 `llvm-as`、LLVM 22 `opt -passes=verify`。

```text
vsftpd ok elapsed=8s
libuv ok elapsed=8s
memcached ok elapsed=7s
```

summary 关键数据：

```text
vsftpd: confirmed_functions=9, basic_blocks=31, instructions=80, xrefs.total=297, data=149, string=139
libuv: confirmed_functions=10, basic_blocks=32, instructions=93, xrefs.total=24, data=13, string=0
memcached: confirmed_functions=9, basic_blocks=31, instructions=80, xrefs.total=179, data=68, string=102
```

IR 里已经出现 confirmed function 之间的 direct call：

```text
vsftpd: call void @notdec_native_5ba0(), call void @notdec_native_8290()
libuv: call void @notdec_native_9d80(), call void @notdec_native_9350()
memcached: call void @notdec_native_5b80(), call void @notdec_native_b950()
```

仍有保守 helper call：

```text
CALLIND / CALLOTHER 保持 helper call。
libuv 还有一个未命中 confirmed function 的 CALL helper。
```

所有 `.stdout` / `.stderr` 日志大小都是 0。

### 性能和影响

这次只增加一个小表查找和一次预先命名，Bench2 三目标 smoke 总耗时约 23 秒，和上一轮约 24 秒同口径接近。

实现效果：4/5。模块内已确认 direct call 能进入 LLVM 调用图。
复杂度：2/5。新增配置表和一条 lowering 分支，范围较小。
维护成本：2/5。后续做参数/返回值恢复时需要扩展这里，但现在不会妨碍旧 helper 路径。

更好的后续方案：等 native function prototype 恢复后，把 direct call 的参数和返回值也接到真实函数类型；现在先不做，避免编出假的 ABI 语义。
