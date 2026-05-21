# CALLIND GOT External Lowering

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

Bench2 三个目标现在都还剩一个 `notdec_pcode_CALLIND_void`，位置都在 `_init`：

- `vsftpd`: `0x5014`
- `libuv`: `0x8014`
- `memcached`: `0x5014`

反汇编和 P-Code 都显示这是同一种模式：

```text
MOV RAX, qword ptr [__gmon_start__@GOT]
TEST RAX, RAX
JZ ...
CALL RAX
```

Sleigh P-Code 里对应：

```text
(register,0x0,8) = COPY (ram,<got>,8)
(unique,0x38a00,8) = COPY (register,0x0,8)
CALLIND (unique,0x38a00,8)
```

这次只解析这种“CALLIND 的输入来源是一个直接 GOT 外部符号”的保守模式。

## Ghidra 相关实现

Ghidra / heritage 路线里，外部函数会先被识别成符号，再由 call lowering 使用：

- `ghidra_scripts/ExportHeritageModule.java::rememberExternal(...)`
  - 记录外部地址和符号名。
- `ghidra_scripts/ExportHeritageModule.java::writeExternals(...)`
  - 导出外部函数表。
- `lib/HeritageToLLVM.cpp::planModuleSymbols(...)`
  - 规划外部函数 LLVM symbol。
- `lib/HeritageToLLVM.cpp::lowerCall(...)`
  - 已知外部目标可以 lower 成 LLVM call，未知目标才走保守路径。

native 侧这里不做通用函数指针分析，只复刻当前可证明的 GOT 外部符号模式。

## native 侧复刻策略

1. 在 `PcodeLoweringConfig` 增加 GOT 地址到外部符号名的表。
2. `notdec-native-llvm` 从 `NativeProgramState::relocations()` 中收集外部 `GLOB_DAT` 符号。
3. P-Code lowering 记录简单来源：`COPY (ram,<addr>)` 和 `COPY` 传播。
4. `CALLIND` 如果输入 varnode 的来源是这张 GOT 外部表，就生成 `call void @symbol()`。
5. 不满足这个模式的 `CALLIND` 继续走 helper。

暂时不做：

- 不解析任意寄存器/内存函数指针。
- 不解析 vtable / jump table。
- 不恢复参数和返回值。
- 不取消原来的 unresolved flow 记录。

## 判断标准

1. Bench2 三个目标的 `_init` `CALLIND` helper 消失。
2. IR 中出现 `call void @__gmon_start__()`。
3. `CALLIND` 其他未知模式仍保留 helper。
4. Bench2 smoke 继续通过 LLVM 22 verify。

## 风险

1. 只应在来源能追到直接 `ram` GOT 地址时 lower，不能只凭寄存器名猜。
2. `__gmon_start__` 是弱符号，当前仍按 `void ()` declaration 处理，后续有 ABI/原型恢复后再补签名。

## 实现记录

### 修改文件和函数

1. `include/notdec-bin2llvm/PcodeToLLVM.h:31`
   - 在 `PcodeLoweringConfig` 增加 `IndirectExternalCallTargets`。
   - key 是 GOT slot 地址，value 是外部 LLVM symbol。
2. `lib/PcodeToLLVM.cpp:304`
   - 新增 `sourceRam(...)` / `setSourceRam(...)`。
   - 记录 `COPY (ram,<addr>)` 和 `COPY` 传播出来的直接 RAM 来源。
3. `lib/PcodeToLLVM.cpp:664`
   - 抽出 `lowerKnownVoidCall(...)`，供 direct call 和 indirect GOT call 共用。
4. `lib/PcodeToLLVM.cpp:695`
   - 新增 `lowerCallInd(...)`。
   - 只有当 `CALLIND` 输入能追到直接 RAM GOT 地址，且该地址在 `IndirectExternalCallTargets` 里时，才 lower 成外部 call。
   - 其他 `CALLIND` 仍走 helper。
5. `tools/notdec-native-llvm.cpp:299`
   - `NativeCallTargets` 增加 `IndirectExternal`。
   - `planNativeCallTargets(...)` 从外部 `X86_64_GLOB_DAT` relocation 生成 GOT slot 到外部 symbol 的映射。
6. `tools/notdec-native-llvm.cpp:382`
   - `--all-confirmed` 给 `PcodeLoweringConfig` 填 `IndirectExternalCallTargets`。
7. `tools/notdec-native-llvm.cpp:478`
   - `-f` / `-n` 单函数模式也填 `IndirectExternalCallTargets`。
8. `scripts/bench2-native-smoke.sh:82`
   - 三个 Bench2 目标的 IR pattern 增加 `call void @__gmon_start__()`。
9. `ARCHITECTURE.md:117`
   - 记录 `CALLIND` 的 GOT 外部符号 lowering 规则。
10. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:39`
   - 更新 Stage 6 和 Stage 7 完成项。

### 验证命令

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm notdec-native-discover -j2
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 \
  -f 0x8000 -o /tmp/notdec-libuv-8000-callind-20260521-40.ll
rg -n '__gmon_start__|notdec_pcode_CALLIND_void' \
  /tmp/notdec-libuv-8000-callind-20260521-40.ll
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-40b
```

### 关键结果

单函数 libuv `_init`：

```text
call void @__gmon_start__()
declare void @__gmon_start__()
```

Bench2 三个目标的 `--all-confirmed` IR：

```text
vsftpd: call void @__gmon_start__()
libuv: call void @__gmon_start__()
memcached: call void @__gmon_start__()
```

`rg notdec_pcode_CALLIND_void` 已经没有命中。libuv 里仍有两个 `CALLOTHER` helper，暂不处理。

### Bench2 结果

三个目标都通过 `notdec-native-discover --summary-json`、`notdec-native-llvm --all-confirmed`、
LLVM 22 `llvm-as`、LLVM 22 `opt -passes=verify`，并通过 smoke 里的 IR pattern 检查。

```text
vsftpd ok elapsed=8s
libuv ok elapsed=18s
memcached ok elapsed=8s
```

summary 关键数据：

```text
vsftpd: confirmed_functions=9, basic_blocks=30, instructions=80, xrefs.total=297, unresolved_indirect_flows=7
libuv: confirmed_functions=9, basic_blocks=29, instructions=85, xrefs.total=24, unresolved_indirect_flows=2
memcached: confirmed_functions=9, basic_blocks=30, instructions=80, xrefs.total=179, unresolved_indirect_flows=7
```

所有 `.stdout` / `.stderr` 日志大小都是 0。

### 性能和影响

这次在 P-Code lowering 里增加了很小的 varnode 来源跟踪，只跟踪直接 RAM 来源，不做通用数据流。
Bench2 smoke 总耗时约 34 秒，和上一轮加了单函数检查后的耗时接近。

实现效果：4/5。三个目标共同的 `_init` `CALLIND` helper 已消失。
复杂度：2/5。来源跟踪很窄，但引入了一个新的 lowering 辅助状态。
维护成本：2/5。后续如果要解析更多 indirect call，需要扩展来源种类，而不是放宽这条规则。
