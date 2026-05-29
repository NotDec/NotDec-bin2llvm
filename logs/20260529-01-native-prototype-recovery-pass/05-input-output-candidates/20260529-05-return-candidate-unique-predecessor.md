# 20260529-05 Return Candidate Unique Predecessor

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的返回值恢复基于 p-code CFG 和 SSA 后的 varnode，不只看 `RETURN` 所在 basic block 内的最后几条指令。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionReturnRecovery::apply(...)`：遍历 `CPUI_RETURN`，检查 return 附近的 active output trial，再调用 `FuncProto::deriveOutputMap(...)`。
  - `ActionReturnRecovery::buildReturnOutput(...)`：把 used output trial 写回 `RETURN`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritage(...)`：把跨 block 的 varnode 定义和使用接成 SSA，return recovery 看到的是 CFG/SSA 后的数据流。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamListStandardOut::fillinMap(...)`：按 ABI output storage 选出可用返回 trial。

native 侧现在只从 `ret` 所在 basic block 向前扫 store。真实 IR 里常见形态是前驱 block 写 `RAX`，然后跳到统一 return block。这个小步先覆盖唯一前驱这一种安全子集。

## native 复刻方式

这一步不做完整 SSA，也不跨多前驱合流。只增加一层保守查找：

- 先沿用当前 `returnTrialBefore(...)`，在 return block 内向前找 ABI output store。
- 如果当前 block 找不到，并且 return block 只有一个 predecessor，就到这个 predecessor 的 terminator 前向前找同类 store。
- 如果 predecessor 不唯一，不猜。
- 如果跨一层仍找不到，不产生返回候选。

测试用例：

- 新增 `return_rax_unique_pred`：entry block 写 `RAX` 后跳到 return block，return block 只有 `ret`。
- prototype recovery 应该标出一个 `RAX` return candidate。

## 判断标准

- `native_prototype_recovery_test` 通过。
- `return_rax_unique_pred` 能产生 `RAX` return candidate。
- 之前 partial return 不误标。

## 实现记录

### 改动

- `lib/passes/NativePrototypeRecovery.cpp:7`：引入 `llvm/IR/CFG.h`，用于查询 predecessor。
- `lib/passes/NativePrototypeRecovery.cpp:52` 到 `lib/passes/NativePrototypeRecovery.cpp:81`：把原来的 return block 内扫描改成 `returnTrialBeforeInstruction(...)`，可从任意指令前向前查最近 ABI output register store。
- `lib/passes/NativePrototypeRecovery.cpp:84` 到 `lib/passes/NativePrototypeRecovery.cpp:93`：新增 `uniquePredecessor(...)`，只接受唯一前驱。
- `lib/passes/NativePrototypeRecovery.cpp:95` 到 `lib/passes/NativePrototypeRecovery.cpp:110`：`returnTrialBefore(...)` 先查当前 return block，找不到时查唯一前驱 terminator 前的 store。
- `tests/native_prototype_recovery_test.cpp:178` 到 `tests/native_prototype_recovery_test.cpp:200`：新增 `createUniquePredecessorReturnStoreFunction(...)`，构造前驱 block 写 `RAX`、return block 只 `ret` 的样例。
- `tests/native_prototype_recovery_test.cpp:296` 到 `tests/native_prototype_recovery_test.cpp:299`：新增 `return_rax_unique_pred`。
- `tests/native_prototype_recovery_test.cpp:311` 到 `tests/native_prototype_recovery_test.cpp:339`：更新 summary 断言，并断言唯一前驱写 `RAX` 能产生 return candidate。

### 验证

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_prototype_recovery_test native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|prototype_recovery.input_candidates|register_effects.preserved)' -V
```

结果：通过。`notdec.native_abi.cspec`、`notdec.native_prototype_model.register`、`notdec.native_prototype_recovery.input_candidates`、`notdec.native_register_effects.preserved` 全部通过，总用时 0.10 秒。

### 性能影响

只有 return block 内找不到候选时，才扫描唯一前驱 block 一次。多前驱不扫描。这个开销和 return 数量、单个前驱 block 指令数相关，当前范围很小。Bench2 全量时间没有在这一步重跑。

### 评分

- 实现效果：7/10。覆盖常见统一 return block 形态，但仍不是完整跨块 SSA。
- 复杂度：3/10。只查一层唯一前驱，不处理多前驱合流。
- 维护成本：3/10。后续如果做完整 SSA value 追踪，可以替换这个 helper。

### 后续

- 多前驱 return block 需要基于 SSA value 或 PHI 判断，不能简单扫描任意前驱。
- 后续应比较不同 path 的 return value 是否一致。
