# 20260529-21 Dispatch Signature Rewrite

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的 prototype recovery 不是每个小动作单独对外调用，而是在 decompiler action pipeline 里逐步更新 `FuncProto`，再让后续 action 使用这个结果。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionInputPrototype::apply(...)`：发现 input trial，更新输入 prototype。
  - `ActionPrototypeTypes::apply(...)`：根据已恢复 prototype 给输入 varnode 和 call/return 类型补信息。
  - `ActionReturnRecovery::apply(...)`：从 return op 恢复 output trial。
  - `ActionReturnRecovery::buildReturnOutput(...)`：把返回 varnode 接到 return op。
  - `ActionOutputPrototype::apply(...)`：把 return op 的输出更新回函数 prototype。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto::deriveInputMap(...)` / `FuncProto::deriveOutputMap(...)`：统一维护最终 prototype。
  - `FuncProto::updateAllTypes(...)`：prototype 确定后，把类型应用到调用约定相关 varnode。

native 侧现在已经有三个分散的最小 rewrite helper：return-only、input-only、input+return。下一步先做一个统一入口，根据 recovered prototype 形状分发到现有 helper。

## native 复刻方式

这一步只做显式 helper，不接默认 pass pipeline：

- 新增 `rewriteNativeRecoveredPrototype(...)`。
- 先读 `notdec.prototype.recovered`。
- 没有 recovered prototype 时返回 `"missing recovered prototype"`。
- `Inputs.empty() && Returns.size() == 1` 时调用 return-only helper。
- `Inputs.size() == 1 && Returns.empty()` 时调用 input-only helper。
- `Inputs.size() == 1 && Returns.size() == 1` 时调用 input+return helper。
- 其他形状返回 `"unsupported recovered prototype shape"`。

这一步不做 callsite rewrite、不遍历 module、不新增 option，也不改变默认 pass 行为。

## 判断标准

- return-only、input-only、input+return 三类无调用者函数可以通过统一 helper 改写。
- 多返回、缺失 recovered prototype、已经不属于当前最小形状的函数不改写。
- 原有单独 helper 行为不变。

## 实现记录

已实现。

### 改动

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:138`
  - 新增 `rewriteNativeRecoveredPrototype(...)` 声明。
- `lib/passes/NativePrototypeRecovery.cpp:870`
  - 新增 `rewriteNativeRecoveredPrototype(...)`。
  - 先读取 `notdec.prototype.recovered`。
  - 按当前支持的三种最小形状分发到已有 helper：
    - return-only：`rewriteNativeRecoveredPrototypeReturnOnly(...)`
    - input-only：`rewriteNativeRecoveredPrototypeInputOnly(...)`
    - input+return：`rewriteNativeRecoveredPrototypeInputReturn(...)`
  - 缺失 recovered prototype 时返回 `"missing recovered prototype"`。
  - 其他形状返回 `"unsupported recovered prototype shape"`。
- `tests/native_prototype_recovery_test.cpp:632`
  - 新增 dispatch 专用 input-only、return-only、input+return 三个函数。
  - summary 计数调整为 23 个函数、14 个 external input、11 个 input candidate、9 个 return candidate、15 个 rewrite eligible、14 个 needs rewrite。
- `tests/native_prototype_recovery_test.cpp:967`
  - 验证统一 helper 能改写三种已支持形状。
  - 验证缺失 recovered prototype 和多返回 prototype 不会改写。

### 验证

命令：

```sh
cmake -S . -B build \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON &&
cmake --build build --target native_prototype_recovery_test native_instcombine_metadata_test -j2 &&
ctest --test-dir build -R 'notdec.native_(prototype_recovery.input_candidates|instcombine.metadata)' -V
```

结果：

- `notdec.native_prototype_recovery.input_candidates` 通过。
- `notdec.native_instcombine.metadata` 通过。
- 总计 2 个测试通过。

性能：

- 统一 helper 只在显式调用时读取一次 recovered metadata，然后做简单分发。
- 还没有接入默认 pass pipeline，当前主链路无额外开销。
- 测试总耗时 0.06 秒。

### 风险和限制

- 仍然不处理 callsite rewrite。
- 仍然不处理多参数、多返回、stack 参数。
- 这一步只是统一入口，后续接默认 pipeline 前还需要 module 级遍历、选项和更完整的负例。

### 评分

- 实现效果：7/10。统一了当前三个最小 rewrite helper 的入口。
- 复杂度：3/10。只是按 recovered prototype 形状分发。
- 维护成本：4/10。后续支持新形状时需要在这里加分支，或改成表驱动/统一 rewrite 流程。
