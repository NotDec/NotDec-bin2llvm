# 20260529-06 Return Candidate Value Consistency

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的返回值恢复不是只要所有 return path 都写了同一个 output storage 就直接认定返回值。它会在 SSA 后检查这些 trial 对应的 varnode 是否真的有返回语义。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionReturnRecovery::apply(...)`：遍历 `CPUI_RETURN`，对 active output trial 调用 `AncestorRealistic::execute(...)` 和 `Funcdata::ancestorOpUse(...)`，确认 return 输入上的 varnode 有实际返回用途。
  - `ActionReturnRecovery::buildReturnOutput(...)`：把最终 used trial 写回 `RETURN` 输入。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamListRegister::fillinMap(...)`：只把 active trial 标为 used，并按 ABI entry 排序。
  - `ParamListStandardOut::fillinMap(...)`：标准 ABI output 也会基于 active trial 和 storage 规则决定最终 output map。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritage(...)`：先构建 SSA，使 return recovery 能看见跨 block 的真实定义和值合流。

native 侧目前只检查“每个 return path 都覆盖同一个 ABI output slot”。这会把两个 return path 分别写不同常量到 `RAX` 的样例也标成返回候选。它至少比缺 path 好，但还不够接近 Ghidra 的 active/value 判断。

## native 复刻方式

这一步先做一个小的保守改进，不做完整 ancestor 分析：

- `NativeParamTrial` 增加一个可选 `ValueKey`，记录 return 前写入 ABI output register 的值。
- 对 `ConstantInt` 记录 `const:<value>`。
- 对其它有 name 的 `llvm::Value` 记录 `value:<name>`。
- 其它值暂时不记录 key，保持旧行为，避免误删真实返回。
- 多 return path 同一个 slot 如果都有可比较 key，则必须完全一致才保留为返回候选。
- 只有部分 path 有 key 时先不过滤，后续做 SSA/PHI 再收紧。

测试用例：

- 新增 `return_rax_conflict`：两个 return path 都写 `RAX`，但分别写不同常量。
- prototype recovery 不应该把它标成 `RAX` return candidate。

## 判断标准

- `native_prototype_recovery_test` 通过。
- `return_rax_conflict` 不产生 `RAX` return candidate。
- 现有 `return_rax_twice`、`return_rax_unique_pred` 仍保持原有结果。

## 实现记录

### 改动

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:4`：引入 `<optional>`。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:23` 到 `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:29`：`NativeParamTrial` 增加 `ValueKey`，用于记录可比较的返回写入值。
- `lib/passes/NativePrototypeRecovery.cpp:6` 到 `lib/passes/NativePrototypeRecovery.cpp:9`：引入 `SmallString` 和 `Constants`。
- `lib/passes/NativePrototypeRecovery.cpp:54` 到 `lib/passes/NativePrototypeRecovery.cpp:64`：新增 `returnValueKey(...)`，先支持 `ConstantInt` 和有 name 的 `llvm::Value`。
- `lib/passes/NativePrototypeRecovery.cpp:89` 到 `lib/passes/NativePrototypeRecovery.cpp:93`：构造返回 trial 时记录 store value 的 `ValueKey`。
- `lib/passes/NativePrototypeRecovery.cpp:198` 到 `lib/passes/NativePrototypeRecovery.cpp:235`：汇总多 return path 时记录每个 slot 的 key，所有路径都有 key 且 key 冲突时，不保留该 slot 的返回候选。
- `tests/native_prototype_recovery_test.cpp:137` 到 `tests/native_prototype_recovery_test.cpp:140`：把 `return_rax_twice` 的两条路径改成相同常量，保持“同值多 path 去重”覆盖。
- `tests/native_prototype_recovery_test.cpp:202` 到 `tests/native_prototype_recovery_test.cpp:232`：新增 `createConflictingReturnStoreFunction(...)`，构造两条 return path 都写 `RAX` 但常量不同的样例。
- `tests/native_prototype_recovery_test.cpp:332` 到 `tests/native_prototype_recovery_test.cpp:334`：新增 `return_rax_conflict`。
- `tests/native_prototype_recovery_test.cpp:345` 到 `tests/native_prototype_recovery_test.cpp:379`：更新 summary 断言，并断言冲突返回值不产生 `RAX` return candidate。

### 验证

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_prototype_recovery_test native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|prototype_recovery.input_candidates|register_effects.preserved)' -V
```

结果：通过。4 个测试全部通过，总用时 0.10 秒。

### 性能影响

每个 return trial 只多一次简单 value 分类，多 return path 汇总时多维护三个小 map/set。复杂度仍跟 return 数量和候选 slot 数量相关，不涉及指令级全函数扫描。Bench2 全量时间没有在这一步重跑。

### 评分

- 实现效果：6/10。能挡住当前测试里明显冲突的返回值，但还不是 Ghidra 的完整 ancestor/use 判断。
- 复杂度：3/10。只加一个可选 key 和汇总过滤。
- 维护成本：3/10。后续做 SSA/PHI 值追踪时可以替换 `ValueKey` 生成逻辑。

### 后续

- 对无名 instruction、PHI、load 需要接 SSA value 等价判断。
- 当前只在所有路径都有 key 时过滤；部分未知值仍保守保留。
