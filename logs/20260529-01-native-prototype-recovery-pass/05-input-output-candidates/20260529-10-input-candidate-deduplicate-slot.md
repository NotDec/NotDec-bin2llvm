# 20260529-10 Input Candidate Deduplicate Slot

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的参数 trial 是围绕 storage 建模的，不应该因为同一个 storage 被重复发现就形成重复参数。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamTrial`：保存 trial 的地址、大小、slot、匹配到的 `ParamEntry`。
  - `ParamActive`：保存当前 trial 集合。
  - `ParamActive::whichTrial(...)`：按地址范围判断已有 trial 是否和新 storage 重叠。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamActive::registerTrial(...)`：创建 trial 并分配 slot。
  - `ParamListStandard::buildTrialMap(...)`：把 trial 关联到 `ParamEntry`，再按 ABI entry/group 组织。
  - `ParamListRegister::fillinMap(...)`：为 active trial 匹配 `ParamEntry` 并排序。

native 侧当前从 `notdec.register.external_inputs` 逐项生成 input candidate。如果 metadata 里重复出现同一个 ABI register，当前会重复输出同一个 slot，这不符合 Ghidra 以 storage 为核心的 trial 语义。

## native 复刻方式

这一步只处理 register input 的简单情况：

- 收集 input candidates 时维护 `seenInputSlots`。
- 同一个 ABI input slot 只保留第一次出现的 trial。
- 继续保留 `ExternalInputsSeen` 的原始计数，方便观察上游 metadata 是否重复。
- 后续 stack storage 和部分寄存器重叠再按 storage range 处理。

测试用例：

- 新增 `input_duplicate`：函数级 external input metadata 写入两次 `RDI`。
- recovery 后 `notdec.prototype.input_candidates` 里只应该有一个 `RDI`。

## 判断标准

- `native_prototype_recovery_test` 通过。
- `input_duplicate` 只输出一个 `RDI` input candidate。

## 实现记录

### 改动

- `lib/passes/NativePrototypeRecovery.cpp:205` 到 `lib/passes/NativePrototypeRecovery.cpp:206`：为单个函数的 input candidate 收集增加 `inputSlots`。
- `lib/passes/NativePrototypeRecovery.cpp:221` 到 `lib/passes/NativePrototypeRecovery.cpp:235`：匹配到 ABI input register 后按 `NativeStorageMatch::Slot` 去重，同一 slot 只保留第一次出现的 trial。
- `tests/native_prototype_recovery_test.cpp:398` 到 `tests/native_prototype_recovery_test.cpp:400`：新增 `input_duplicate`，在 `notdec.register.external_inputs` 中重复写入 `RDI`。
- `tests/native_prototype_recovery_test.cpp:437` 到 `tests/native_prototype_recovery_test.cpp:459`：更新 summary 断言，并检查 `input_duplicate` 只输出一个 `RDI` input candidate。

### 验证

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_prototype_recovery_test native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|prototype_recovery.input_candidates|register_effects.preserved)' -V
```

结果：通过。4 个测试全部通过，总用时 0.10 秒。

### 性能影响

每个函数多维护一个 input slot set，插入和查询开销按 input candidate 数量增长。数量通常很小，影响可以忽略。Bench2 全量时间没有在这一步重跑。

### 评分

- 实现效果：6/10。解决 register input 的重复 slot 问题，但还没覆盖 stack range 和部分寄存器重叠。
- 复杂度：2/10。只增加一个局部 set。
- 维护成本：2/10。后续可替换为完整 storage range 去重。

### 后续

- stack input candidate 需要按地址范围去重和重叠判断。
- 部分寄存器或别名寄存器需要结合 register storage offset/size 处理。
