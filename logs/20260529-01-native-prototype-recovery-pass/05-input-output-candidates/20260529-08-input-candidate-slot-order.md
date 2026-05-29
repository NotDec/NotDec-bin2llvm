# 20260529-08 Input Candidate Slot Order

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的参数 trial 最终不是按发现顺序输出，而是按 prototype model 的参数顺序整理。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamActive::sortTrials(...)`：对当前 trial 列表排序。
  - `FuncProto::deriveInputMap(...)`：调用 input `ParamList::fillinMap(...)`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamListStandard::fillinMap(...)`：调用 `buildTrialMap(...)`，把 trial 关联到 `ParamEntry`，后续按 ABI entry 分组和顺序处理。
  - `ParamListRegister::fillinMap(...)`：给 active trial 关联 `ParamEntry`，然后调用 `active->sortTrials()`。
  - `ParamTrial::operator<(...)`：先按 `ParamEntry` group 排，再按 entry/address/size 排。

native 侧当前已经用 `NativePrototypeModel::findInputRegister(...)` 得到 ABI slot，但写 `notdec.prototype.input_candidates` 时仍按 `notdec.register.external_inputs` metadata 的原顺序输出。这个顺序可能不是 ABI 参数顺序。

## native 复刻方式

这一步不复刻完整 `ParamEntry` group/address 排序，只做已有数据结构能准确表达的部分：

- 给 `NativeParamActive` 增加一个小 helper，把 trial 按 `Slot` 升序排序。
- input candidates 在写 metadata 前排序。
- return candidates 暂时不改，当前 return output slot 只有一类测试，后续多 output 时再单独补。

测试用例：

- 新增 `input_reversed`：函数级 external input metadata 按 `RSI, RDI` 顺序给出。
- recovery 后 `notdec.prototype.input_candidates` 应该按 ABI slot 输出为 `RDI, RSI`。

## 判断标准

- `native_prototype_recovery_test` 通过。
- `input_reversed` 有两个 input candidates。
- `input_reversed` 的 metadata 顺序是 `RDI` 在 `RSI` 前。

## 实现记录

### 改动

- `lib/passes/NativePrototypeRecovery.cpp:16`：引入 `<algorithm>`。
- `lib/passes/NativePrototypeRecovery.cpp:173` 到 `lib/passes/NativePrototypeRecovery.cpp:179`：新增 `sortTrialsBySlot(...)`，按 `NativeParamTrial::Slot` 稳定排序。
- `lib/passes/NativePrototypeRecovery.cpp:200` 到 `lib/passes/NativePrototypeRecovery.cpp:235`：input candidates 收集完成后调用 `sortTrialsBySlot(active)`，写 metadata 前固定 ABI slot 顺序。
- `tests/native_prototype_recovery_test.cpp:296` 到 `tests/native_prototype_recovery_test.cpp:314`：新增 `metadataRegisterAt(...)`，用于检查 metadata 条目顺序。
- `tests/native_prototype_recovery_test.cpp:360` 到 `tests/native_prototype_recovery_test.cpp:362`：新增 `input_reversed`，按 `RSI, RDI` 顺序写入 external input metadata。
- `tests/native_prototype_recovery_test.cpp:396` 到 `tests/native_prototype_recovery_test.cpp:414`：更新 summary 断言，并检查 `input_reversed` 输出顺序为 `RDI, RSI`。

### 验证

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_prototype_recovery_test native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|prototype_recovery.input_candidates|register_effects.preserved)' -V
```

结果：通过。4 个测试全部通过，总用时 0.10 秒。

### 性能影响

只对每个函数的 input trials 做一次稳定排序。候选数量通常很小，开销可以忽略。Bench2 全量时间没有在这一步重跑。

### 评分

- 实现效果：6/10。复刻了 ABI slot 顺序，但还没复刻 Ghidra `ParamEntry` group/address 的完整排序。
- 复杂度：2/10。只加一个排序 helper。
- 维护成本：2/10。后续如果 `NativeParamTrial` 增加完整 `ParamEntry` 信息，可以扩展 comparator。

### 后续

- return candidates 的多 output slot 顺序后续单独处理。
- input sorting 后续应按 `NativeParamEntry` group、offset、size 补齐。
