# 20260529-11 Recovered Prototype Metadata

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 不只保留候选 trial，还会把筛选结果写回函数 prototype。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamTrial`：保存一个候选 storage，包含 slot、address、size、active/use 等状态。
  - `ParamActive`：保存当前函数或 callsite 的候选 trial 列表。
  - `ProtoModel::deriveInputMap(...)` / `ProtoModel::deriveOutputMap(...)`：把 active trial 按 prototype model 规则收敛成输入/输出参数列表。
  - `FuncProto`：保存函数最终 prototype，包括输入参数和返回值。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：调用 `FuncProto::deriveInputMap(...)`，把 input trial 写回函数 prototype。
  - `ActionReturnRecovery::apply(...)`：调用 `FuncProto::deriveOutputMap(...)`，把 output trial 写回函数 prototype。

native 侧现在只有 `notdec.prototype.input_candidates` 和 `notdec.prototype.return_candidates`。这相当于 Ghidra 的 `ParamActive` 阶段，还没有一个稳定的“函数已恢复 prototype”结果。

## native 复刻方式

这一步先做最小复刻：不改 LLVM 函数类型，不改 callsite，只把已经筛出的 input/return candidate 汇总成函数级 metadata：

- 新增 `NativeRecoveredPrototype` / `NativeRecoveredPrototypeParam`。
- `runNativePrototypeRecovery(...)` 在写 candidate metadata 后，再写 `notdec.prototype.recovered`。
- metadata 先只包含：
  - `model=<abi prototype name>`
  - `input_count=<N>`
  - `return_count=<N>`
  - `inputs` 子节点，按 slot 排序，字段为 `name=` 和 `slot=`
  - `returns` 子节点，按 slot 排序，字段为 `name=` 和 `slot=`

这一步只跟着当前测试用例做 register 参数和 register 返回值。栈参数、类型、真实 LLVM 函数签名重写后续再做。

## 判断标准

- 原有 input/return candidate 行为不变。
- 有候选时写出 `notdec.prototype.recovered`。
- 没有候选时清掉 `notdec.prototype.recovered`。
- `native_prototype_recovery_test` 覆盖 input/return count 和 slot 顺序。

## 实现记录

### 源码改动

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:38`
  - 新增 `NativeRecoveredPrototypeParam` / `NativeRecoveredPrototype`。
  - 设计意图：先保存已经恢复出的有序 storage，不改 LLVM 函数类型，后续签名重写直接消费这个结构。
- `lib/passes/NativePrototypeRecovery.cpp:55`
  - 新增 `recoveredParams(...)`，把 `NativeParamActive` 中已排序的 trial 转成 recovered prototype 参数。
- `lib/passes/NativePrototypeRecovery.cpp:67`
  - 新增 `recoveredParamListMetadata(...)`，写出 `name=` / `slot=` 参数列表。
- `lib/passes/NativePrototypeRecovery.cpp:81`
  - 新增 `recoveredPrototypeMetadata(...)`，写出 `model=`、`input_count=`、`return_count=`、inputs 子节点和 returns 子节点。
- `lib/passes/NativePrototypeRecovery.cpp:345`
  - `runNativePrototypeRecovery(...)` 在写 input/return candidates 后，同步写 `notdec.prototype.recovered`。
  - 如果没有 input 和 return 候选，清掉该 metadata。
- `tests/native_prototype_recovery_test.cpp:372`
  - 新增 recovered prototype metadata 读取 helper。
- `tests/native_prototype_recovery_test.cpp:534`
  - 覆盖 model、input/return count、input slot 顺序、return slot 顺序和空 prototype 清理。

### 验证

```sh
cmake -S . -B build \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
```

结果：`notdec.native_prototype_recovery.input_candidates` 通过。

### 性能和风险

- 性能：只多写一个函数级 metadata，成本和候选数量线性相关。当前候选数量很小，预期影响可以忽略。
- 风险：metadata 结构现在用固定 operand 位置表示 inputs/returns，后续如果要给外部工具长期消费，最好再加显式 `"inputs"` / `"returns"` 标签或独立命名节点。
- 实现效果：7/10。已经有一个稳定的 recovered prototype 入口，但还没改函数签名。
- 复杂度：2/10。复用现有 trial，不新增推断规则。
- 维护成本：3/10。后续签名重写时需要保持 candidate metadata 和 recovered metadata 一致。
