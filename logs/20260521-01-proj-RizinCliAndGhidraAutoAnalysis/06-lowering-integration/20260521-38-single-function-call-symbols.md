# Single-Function Call Symbols

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

`--all-confirmed` 已经可以把 confirmed function 之间的 direct `CALL` lower 成普通 LLVM call，
也能把已知 PLT stub lower 成外部 LLVM call。Bench2 libuv 里：

- `0x8dc0` 已经能 lower 成 `__cxa_finalize()`
- `0x9350` 已经能 lower 成 `pthread_key_delete()`

但单函数模式 `-f <entry>` / `-n <name>` 还没共享这张符号表，所以同一个函数单独 lower 时，
已知内部 direct call 和 PLT call 仍会退回 helper。这个缺口会直接影响手工定位和单函数 smoke。

## Ghidra 相关实现

Ghidra / heritage 路线在模块级 lowering 之前就会规划符号：

- `lib/HeritageToLLVM.cpp::planModuleSymbols(...)`
  - 把内部函数和外部函数一起放进模块符号表。
- `lib/HeritageToLLVM.cpp::lowerCall(...)`
  - direct `CALL` 直接落到符号表里的名字。
- `ghidra_scripts/ExportHeritageModule.java::writeExternals(...)`
  - 外部符号和内部函数一起导出。

native 侧单函数模式现在缺的是同一张符号表视图，而不是新的 lowering 语义。

## native 侧复刻策略

1. 在 `tools/notdec-native-llvm.cpp` 里为单函数模式复用 native discovery 的函数名规划。
2. 单函数模式也填 `DirectCallTargets`，让已知 confirmed function 的 direct call 直接落成 LLVM call。
3. 单函数模式也填 `ExternalCallTargets`，让 PLT / PLT.GOT call 直接落成外部 LLVM call。
4. 仍然只 lower 当前选中的函数体，不把其他 confirmed function 追加进同一个 module。

暂时不做：

- 不改 helper call 的签名恢复。
- 不改变 `--all-confirmed` 的现有行为。
- 不扩展到间接 call。

## 判断标准

1. `notdec-native-llvm -f 0x9df0` 在 libuv 里能 lower 出 `__cxa_finalize()` 和 `notdec_native_9d80()`。
2. `-n uv_key_delete` 这类单函数入口也能复用同样的符号映射。
3. `--all-confirmed` 行为不变。

## 风险

1. 单函数模式使用整张 confirmed function 名字表时，可能会多出一些 declaration；这属于可接受的保守行为。
2. 如果某个 binary 的 discovery 结果太少，单函数 direct call 仍可能保留 helper，但外部 PLT call 不应再退回。

## 实现记录

### 修改文件和函数

1. `tools/notdec-native-llvm.cpp:299`
   - 新增 `NativeCallTargets` 和 `planNativeCallTargets(...)`。
   - 统一规划 confirmed function 的 LLVM 名字和 PLT 外部符号名字，供 `--all-confirmed` 和单函数模式共用。
2. `tools/notdec-native-llvm.cpp:341`
   - 修改 `buildConfirmedModule(...)`。
   - 改为复用 `planNativeCallTargets(...)` 的结果，不再单独手写内部/外部符号表。
3. `tools/notdec-native-llvm.cpp:463`
   - 修改单函数模式的 `main(...)` lowering 分支。
   - 对 `-f <entry>` / `-n <name>` 也先跑 native discovery，并填入 `DirectCallTargets` 和 `ExternalCallTargets`。
   - 保持当前函数体仍只 lower 一次，不把其他 confirmed function 加进 module。
4. `scripts/bench2-native-smoke.sh:151`
   - libuv smoke 增加单函数 `-f 0x9df0` 检查。
5. `scripts/bench2-native-smoke.sh:177`
   - libuv smoke 增加单函数 `-n uv_key_delete` 检查。
6. `ARCHITECTURE.md:117`
   - 记录单函数模式也会复用 confirmed function / PLT 外部符号映射。
7. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:38`
   - 更新 Stage 6 和 Stage 7 完成项。

### 验证命令

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm notdec-native-discover -j2
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 \
  -f 0x9df0 -o /tmp/notdec-libuv-9df0-single-20260521-38.ll
rg -n '__cxa_finalize|notdec_native_9d80|notdec_pcode_CALL_void' \
  /tmp/notdec-libuv-9df0-single-20260521-38.ll
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 \
  -n uv_key_delete -o /tmp/notdec-libuv-uv_key_delete-20260521-38.ll
rg -n 'pthread_key_delete|notdec_pcode_CALL_void' \
  /tmp/notdec-libuv-uv_key_delete-20260521-38.ll
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-38b
```

### 关键结果

单函数 libuv IR 里：

```text
call void @__cxa_finalize()
call void @notdec_native_9d80()
declare void @__cxa_finalize()
declare void @notdec_native_9d80()
```

按名字跑的 `-n uv_key_delete` 里：

```text
call void @pthread_key_delete()
declare void @pthread_key_delete()
```

两条单函数路径都没有再出现 `notdec_pcode_CALL_void`。

### Bench2 结果

三个目标都通过 `notdec-native-discover --summary-json`、`notdec-native-llvm --all-confirmed`、
LLVM 22 `llvm-as`、LLVM 22 `opt -passes=verify`，并通过 smoke 里的单函数和多函数 IR 检查。

```text
vsftpd ok elapsed=7s
libuv ok elapsed=19s
memcached ok elapsed=8s
```

libuv 因为多跑两次单函数检查，耗时上升到 19 秒；其余两个目标保持接近之前。

所有 `.stdout` / `.stderr` 日志大小都是 0。

### 性能和影响

这次只是把同一套 call-target 规划复用到单函数模式，并给 smoke 加了两条单函数样本。
实现效果：4/5。单函数模式现在不会把已知 direct call 再退回 helper。
复杂度：2/5。多了一层共用的 target 规划，但比散在两个分支里更稳。
维护成本：2/5。以后如果函数命名策略变了，只需要改一处规划函数和 smoke pattern。
