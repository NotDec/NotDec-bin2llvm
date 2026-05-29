# 20260529-07 Input Candidate Active Use

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的函数入口参数恢复不会把所有 input varnode 都直接当参数。它会先看 input varnode 有没有真实使用。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - 入口参数恢复逻辑在 `ActionPrototypeTypes::apply(...)` 附近：遍历 `Varnode::input`，如果 `FuncProto::possibleInputParam(...)` 接受该 storage，就注册 `ParamActive` trial；只有 `!vn->hasNoDescend()` 时才 `markActive()`。
  - `ActionActiveParam::apply(...)`：对 callsite 参数也用 `checkInputTrialUse(...)` 检查 active input，再 `deriveInputMap(...)`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto::deriveInputMap(...)` 调用 prototype model 的 input `fillinMap(...)`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamActive::registerTrial(...)` 创建 trial。
  - `ParamListStandard::buildTrialMap(...)` 根据 active trial 和 ABI storage 规则组织参数。

native 侧当前只要 `notdec.register.external_inputs` 里出现 ABI input register，就标成 `notdec.prototype.input_candidates`。这比 Ghidra 宽。下一步先补一个很小的 active-use 判断。

## native 复刻方式

这一步只处理当前 IR 已经能表达的情况：

- 在函数内查找带 `notdec.register.external_input` metadata 的 load。
- 如果找到目标寄存器的 external input load，且所有这类 load 都没有 use，则认为它不是 active input，不生成参数候选。
- 如果没有找到 load，只看到函数级 `notdec.register.external_inputs`，保持旧行为。原因是旧测试和部分中间 IR 可能只有 summary metadata，没有单条 load 证据。
- 不做跨 call、alias、stack 参数判断。

测试用例：

- 新增 `unused_rdi`：函数级 metadata 里有 `RDI`，函数体里也有 `RDI.external_input` load，但 load 没有 use。
- prototype recovery 不应该标出 `RDI` input candidate。

## 判断标准

- `native_prototype_recovery_test` 通过。
- `input_rdi` 仍标出 `RDI`。
- `unused_rdi` 不标出 `RDI`。

## 实现记录

### 改动

- `lib/passes/NativePrototypeRecovery.cpp:66` 到 `lib/passes/NativePrototypeRecovery.cpp:90`：新增 `hasActiveExternalInputUse(...)`，查找函数内带 `notdec.register.external_input` metadata 的 load；如果能看到目标寄存器的 external input load 且全部无 use，则返回 false。
- `lib/passes/NativePrototypeRecovery.cpp:211` 到 `lib/passes/NativePrototypeRecovery.cpp:217`：生成 input candidate 前增加 active-use 过滤。
- `tests/native_prototype_recovery_test.cpp:95` 到 `tests/native_prototype_recovery_test.cpp:116`：新增 `createUnusedExternalInputFunction(...)`，构造带 unused external input load 的样例。
- `tests/native_prototype_recovery_test.cpp:340` 到 `tests/native_prototype_recovery_test.cpp:342`：新增 `unused_rdi`。
- `tests/native_prototype_recovery_test.cpp:372` 到 `tests/native_prototype_recovery_test.cpp:385`：更新 summary 断言，并断言 `unused_rdi` 不产生 `RDI` input candidate。

### 验证

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_prototype_recovery_test native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|prototype_recovery.input_candidates|register_effects.preserved)' -V
```

结果：通过。4 个测试全部通过，总用时 0.10 秒。

### 性能影响

每个 external input candidate 会扫描一次当前函数的指令来找对应 load。当前候选数量很小，影响有限。后续如果 input 候选变多，可以先构建 register 到 active-use 的表，避免重复扫描。

### 评分

- 实现效果：6/10。能过滤明确 unused 的 external input load，但还不是 Ghidra 的 descendant/use 全量判断。
- 复杂度：3/10。只加一个 helper，不改变 metadata 结构。
- 维护成本：3/10。后续可以把 helper 替换成一次性收集的 active-use map。

### 后续

- 需要区分只被保存恢复使用、只被 debug/lifetime 使用、以及真正参与计算或 call 参数的 input。
- 后续应把 active-use 判断和 `NativeRegisterSSA` 产生的 external input value 关系固定下来。
