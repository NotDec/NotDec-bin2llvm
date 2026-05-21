# CALLOTHER LOCK No-op Lowering

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

Bench2 当前 `CALLIND` helper 已经清掉，`libuv` 还剩两个 `CALLOTHER` helper：

```text
call void (...) @notdec_pcode_CALLOTHER_void(i32 17)
call void (...) @notdec_pcode_CALLOTHER_void(i32 18)
```

它们来自 `uv_library_shutdown` 的 `xchg eax, [0x34170]`。Sleigh P-Code 已经把交换语义展开成：

```text
CALLOTHER (const,0x11,4)
(unique,0xe4200,4) = COPY (ram,0x34170,4)
(ram,0x34170,4) = COPY (register,0x0,4)
(register,0x0,4) = COPY (unique,0xe4200,4)
CALLOTHER (const,0x12,4)
```

这次只处理 `0x11` / `0x12`。

## Ghidra 相关实现

Ghidra x86 SLEIGH 规格里，这两个 userop 定义在：

- `Ghidra/Processors/x86/data/languages/ia.sinc`
  - `define pcodeop LOCK;`
  - `define pcodeop UNLOCK;`

`x86-64.slaspec` include `x86.slaspec`，`x86.slaspec` 再 include `ia.sinc`。按 include 顺序数，
`LOCK` 是 userop 17，`UNLOCK` 是 userop 18。

Ghidra decompiler 会把 lock 边界作为 userop 保留，同时真实内存读写仍由普通 P-Code 表达。

## native 侧复刻策略

1. 在 `PcodeLowerer` 里识别 `CALLOTHER` 第一个输入是 const `17` 或 `18`。
2. 如果没有输出，直接 no-op。
3. 其他 `CALLOTHER` 继续走 helper。
4. 不把普通 load/store 改成 LLVM atomic，避免引入未经验证的内存序语义。

暂时不做：

- 不处理其他 userop。
- 不模拟 lock 内存序。
- 不改 Ghidra/Sleigh 导出。

## 判断标准

1. libuv 的两个 `notdec_pcode_CALLOTHER_void` 消失。
2. `uv_library_shutdown` 的交换 load/store/copy 仍保留。
3. Bench2 smoke 继续通过 LLVM 22 verify。

## 风险

1. no-op 会丢掉 lock/unlock 的并发内存序信息；当前 native lowering 还没有线程内存模型，先保留普通读写语义。
2. 只能针对 x86 userop 17/18，不能泛化到其他 CALLOTHER。

## 实现记录

### 修改文件和函数

- `lib/PcodeToLLVM.cpp`
  - 第 707 行 `PcodeLowerer::lowerCallOther(...)`：新增 x86 `LOCK` / `UNLOCK` userop 17/18 的 no-op lowering。只有无输出、一个 const 输入且值为 17 或 18 时才跳过；其他 `CALLOTHER` 保持 helper。
  - 第 718 行 `PcodeLowerer::lowerOp(...)`：把 `PcodeOpcode::CallOther` 分发到 `lowerCallOther(...)`。
- `scripts/bench2-native-smoke.sh`
  - 第 78 行新增 `forbid_ir_pattern(...)`。
  - 第 89 行 `check_ir_features(...)` 和第 196 行起的 libuv 单函数检查禁止 `notdec_pcode_CALL_void`、`notdec_pcode_CALLIND_void`、`notdec_pcode_CALLOTHER_void` 回退。
- `ARCHITECTURE.md`
  - 第 117 行的 native LLVM 入口说明补充 x86 `CALLOTHER` 17/18 no-op，以及 Bench2 smoke 的 helper 回退检查。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
  - 第 49 行阶段 6 记录 `LOCK` / `UNLOCK` lowering。
  - 第 56 行阶段 7 记录 helper 回退检查。

### 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm notdec-native-discover -j2
```

Bench2 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-41c
```

结果：

```text
vsftpd ok elapsed=8s
libuv ok elapsed=19s
memcached ok elapsed=7s
```

IR 检查：

```bash
rg 'notdec_pcode_CALL|notdec_pcode_CALLOTHER|notdec_pcode_CALLIND|__gmon_start__|__cxa_finalize|pthread_key_delete|notdec_native_' /tmp/notdec-bin2llvm-bench2-smoke-20260521-41c/*.ll
```

结果：

- 没有 `notdec_pcode_CALLOTHER`。
- 没有 `notdec_pcode_CALLIND`。
- 没有 `notdec_pcode_CALL_void`。
- `libuv` 仍有预期的 `__gmon_start__`、`__cxa_finalize`、`pthread_key_delete`、`notdec_native_9d80`。
- `uv_library_shutdown` 的 `xchg` 仍保留普通 read/write/copy，只去掉 lock/unlock helper。

summary：

```text
libuv: confirmed=9 blocks=29 instr=85 xrefs=24 unresolved=2
memcached: confirmed=9 blocks=30 instr=80 xrefs=179 unresolved=7
vsftpd: confirmed=9 blocks=30 instr=80 xrefs=297 unresolved=7
```

stdout/stderr 文件为空。三目标 smoke 总耗时约 34s，和加入单函数检查后的前一次同口径结果接近。

### 评分

- 实现效果：8/10。当前 Bench2 里已知 x86 lock helper 清掉，交换值语义保留。
- 理解成本：2/10。只在 `CALLOTHER` 分发点加窄规则。
- 维护成本：2/10。规则依赖 x86 userop 编号，后续如果支持其他架构，需要改成按 spec 名字或 userop 表处理。
