# 20260601-112 return binding register copy

## 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass。首先将相关的数据结构划分为几个工作量大约相等的部分（目前应该差不多完成了，主要是在数据集上测试并进一步提升），其次，规划一下如何做测试和验证。把进度记录到PROGRESS.md里面。依然要求，

根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

在数据集上测试并进一步提升时，因为不涉及实现具体的模块或数据结构，可以不需要遵守上面的功能复刻流程。

1. 每一轮先看 Bench2 当前 skip reason 和真实函数样本，再决定实现点。
2. 先按 Ghidra 数据结构和 Bench2 blocker 识别“大块能力”，再从大块能力里切小步。小步服务于大块任务，不允许小步自己变成主线。
3. 每次实现不应以“一个极小 CFG 变体”作为默认粒度。应把同一类语义问题合并成一个小阶段处理，同类问题多修复一些再合并commit。
```

## 当前 Bench2 现象

`python:interpreter --decode-seed-limit 200` 暴露新的非合理 skip reason：

- `unsafe return value load=1`
- 函数：`PyStatus_Exit`

真实 IR 形状：

```llvm
%RDI.external_input = load i64, ptr @RDI, !notdec.register.external_input
store i64 %RDI.external_input, ptr @RDX, !notdec.register.access !RDX
...
%RDX13 = load i64, ptr @RDX, !notdec.register.access !RDX
store i64 %RDX13, ptr @RAX, !notdec.register.access !RAX
ret void
```

recovered prototype 是 `i64(i64)`，返回 storage 是 `RAX`。当前 return binding 看到 `store @RAX` 的 value 是 register access load `%RDX13`，因此按 `unsafe return value load` 保守跳过。

候选大块任务：

- return binding 追踪函数内寄存器 copy：本轮处理。
- 更完整的 register SSA current-value 查询复用：后续可以整理，但本轮不重构。
- 穿过 call 或复杂 alias 的寄存器 load 追踪：暂不做，避免把不安全 callsite return load 误当本函数返回值。

## Ghidra 对应实现

Ghidra 在 P-code 层通过 heritage SSA 让寄存器 varnode 有明确 def-use。返回恢复不是盯着最终机器寄存器 load，而是看 return op 输入和它的 varnode 定义链。

相关源码：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/opcodes.hh`
  - `CPUI_COPY` 表示 varnode 之间的拷贝。
  - `CPUI_LOAD` 表示从地址空间加载。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionReturnRecovery::buildReturnOutput(...)` 根据 active output trials 构造返回输出。
  - `ActionReturnRecovery` 周围逻辑会处理 COPY 链和 return output。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ProtoModel::deriveOutputMap(...)` 从 output trials 判断返回 storage。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::buildOutputFromTrials(...)` 把 output trials 转成最终 call output。

对应到 native 侧，`store @RDX` 后 `load @RDX` 再 `store @RAX` 是 register-global 形式的 COPY。这个 COPY 在当前函数内、路径上没有非 intrinsic call 且前驱值一致时，可以把返回值绑定到前一个 `store @RDX` 的 value，而不是保留 register load。

## native 侧复刻策略

本轮做最小安全复刻：

- 在 `returnValueForStore(...)` 中，如果 return store 的 value 是带 `notdec.register.access` 的 register load：
  - 读取 load 的 register name。
  - 先在同一 basic block 内向前查找同 register store。
  - 如果同块找不到，再沿 predecessor 向入口查找，多个前驱必须得到同一个 store value。
  - 如果查找过程中遇到非 intrinsic call、类型不一致、多个前驱值不一致，就停止，不做替换。
  - 找到类型一致且路径一致的 store value 后，把 return binding 的值替换为该 store value。
- 只处理当前函数内 register-global 形式的 COPY，不处理 alias，不穿过 call。
- 原有“无法解析的 register load 是 unsafe return value load”的保护保留。

判断标准：

- 新增单元测试覆盖 `RDI external_input -> store RDX -> load RDX -> store RAX -> ret`，期望 rewrite 成 `i64(i64)`，返回值直接用参数。
- 原有“直接 `load RAX -> store RAX`”负例仍保持 `unsafe return value load`。
- `python:interpreter --decode-seed-limit 200` 的 `unsafe return value load` 消失，只剩 `already matches` / `declaration`。

## 实现记录

- `lib/passes/NativePrototypeRecovery.cpp:265` 新增 `RegisterValueLookup`，用 `Unsafe` 区分“没有找到定义”和“路径不安全”。
- `lib/passes/NativePrototypeRecovery.cpp:272` 新增 `registerValueInReverseRange(...)`，在同一 block 内反向查找同 register store，遇到非 intrinsic call 或类型不一致时标为 unsafe。
- `lib/passes/NativePrototypeRecovery.cpp:304` 新增 `registerValueAtBlockEntry(...)`，沿 predecessor 查询 block 入口处的 register 当前值；多个前驱值必须通过 `sameReturnStoreValue(...)` 证明一致，循环回边没有新 store 时不强行构造值。
- `lib/passes/NativePrototypeRecovery.cpp:342` 新增 `registerStoreValueBeforeLoad(...)`，把带 `notdec.register.access` 的 register load 解析到当前函数内最近的安全 store value。
- `lib/passes/NativePrototypeRecovery.cpp:1512` 修改 `returnValueForStore(...)`，return store 的 value 如果是可解析 register load，则绑定到解析出的源值。
- `tests/native_prototype_recovery_test.cpp:128` 新增 `attachInputRaxReturnTestAbi(...)`，构造只有 `RDI` 输入和 `RAX` 输出的测试 ABI，避免 `RDX` 被当成第二返回值。
- `tests/native_prototype_recovery_test.cpp:1523` 新增 `createInputRegisterCopyReturnFunction(...)`，构造跨 block 的 `RDI external_input -> store RDX -> load RDX -> store RAX -> ret`。
- `tests/native_prototype_recovery_test.cpp:4339` 新增 `native-prototype-input-register-copy-return-test`，验证 rewrite 后函数返回新参数，且 unresolved register load 负例仍由原有测试覆盖。

验证：

- `cmake --build /tmp/notdec-bin2llvm-build --target native_prototype_recovery_test -j$(nproc)`：通过。
- `ctest --test-dir /tmp/notdec-bin2llvm-build -R 'notdec.native_prototype_recovery' --output-on-failure`：通过。
- `cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm -j$(nproc)`：通过。
- `scripts/bench2-native-prototype-audit.sh --build-dir /tmp/notdec-bin2llvm-build --out-dir /tmp/notdec-bin2llvm-bench2-python-seed200-return-copy --target python:interpreter --decode-seed-limit 200`：通过。

Bench2 结果：

- `python:interpreter` seed limit 200：`signature rewrite needed functions=156`，`rewritten functions=156`，`skipped functions=69`。
- skip reason 只剩 `already matches=44` 和 `declaration=25`。
- `PyStatus_Exit` 从 `unsafe return value load` 变为 `rewritten=1`，输出为 `define i64 @PyStatus_Exit(i64 %RDI.external_input1)`，最终 `ret i64 %RDI.external_input1`。
- 同口径耗时：all-confirmed 45s，signature rewrite 46s。

风险：

- 这不是完整 register SSA current-value 查询，只用于 return binding 的安全 register COPY 解析。
- 跨 block 查询不穿过非 intrinsic call；如果前驱值不一致或类型不一致，仍回到原有 unsafe 路径。
