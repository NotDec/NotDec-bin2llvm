# 20260529-09 Return Candidate Slot Order

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的返回值 trial 也会按 prototype model 的 storage 顺序整理。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionReturnRecovery::apply(...)`：对 active output trial 做检查后调用 `FuncProto::deriveOutputMap(...)`。
  - `ActionReturnRecovery::buildReturnOutput(...)`：按 `active->getTrial(i)` 的顺序把 used return varnode 写回 `RETURN`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto::deriveOutputMap(...)` 调用 output `ParamList::fillinMap(...)`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamListStandardOut::fillinMap(...)`：给 active output trial 关联 `ParamEntry`，然后调用 `active->sortTrials()`，再由 model rule 标记 used。
  - `ParamListStandardOut::fillinMapFallback(...)`：fallback 路径也会在匹配后调用 `active->sortTrials()`。
  - `ParamTrial::operator<(...)`：按 `ParamEntry` group、entry、address、size 排序。

native 侧当前 return candidates 从 `std::map<slot, count>` 遍历得到，已经通常是 slot 升序，但这是容器副作用，不是 `NativeParamActive` 自己的语义。上一小步已经给 input trial 加了显式 slot 排序，这一步把同样的语义用于 return trial。

## native 复刻方式

这一步做最小复刻：

- 复用 `sortTrialsBySlot(...)`，在 return candidates 写 metadata 前排序。
- return 点扫描从“只取第一个 ABI output store”改成“收集当前 return 点前每个 ABI output slot 最近的一次 store”。
- 测试里给 ABI 增加第二个 output register `RDX`。
- 新增 `return_rdx_rax_order`：函数体先写 `RDX`，再写 `RAX`，两者都是 ABI output。
- recovery 后 return metadata 应该按 ABI output slot 输出为 `RAX, RDX`。

## 判断标准

- `native_prototype_recovery_test` 通过。
- `return_rdx_rax_order` 有两个 return candidates。
- `return_rdx_rax_order` 的 metadata 顺序是 `RAX` 在 `RDX` 前。

## 实现记录

### 改动

- `lib/passes/NativePrototypeRecovery.cpp:93` 到 `lib/passes/NativePrototypeRecovery.cpp:129`：把 `returnTrialBeforeInstruction(...)` 改为 `returnTrialsBeforeInstruction(...)`，同一 return 点前按 ABI output slot 收集多个 trial，并用 `seenSlots` 保留每个 slot 最近一次 store。
- `lib/passes/NativePrototypeRecovery.cpp:142` 到 `lib/passes/NativePrototypeRecovery.cpp:158`：把 `returnTrialBefore(...)` 改为 `returnTrialsBefore(...)`，当前 block 找不到时仍只查唯一前驱。
- `lib/passes/NativePrototypeRecovery.cpp:242` 到 `lib/passes/NativePrototypeRecovery.cpp:279`：返回候选汇总改为遍历每个 return 点的 trial 列表，并在写 metadata 前调用 `sortTrialsBySlot(returns)`。
- `tests/native_prototype_recovery_test.cpp:66` 到 `tests/native_prototype_recovery_test.cpp:80`：测试 ABI 增加第二个 output register `RDX`。
- `tests/native_prototype_recovery_test.cpp:265` 到 `tests/native_prototype_recovery_test.cpp:288`：新增 `createTwoOutputReturnStoreFunction(...)`，构造先写 `RDX`、再写 `RAX` 的返回样例。
- `tests/native_prototype_recovery_test.cpp:420` 到 `tests/native_prototype_recovery_test.cpp:422`：新增 `return_rdx_rax_order`。
- `tests/native_prototype_recovery_test.cpp:433` 到 `tests/native_prototype_recovery_test.cpp:440`：更新函数数和返回候选计数。
- `tests/native_prototype_recovery_test.cpp:479` 到 `tests/native_prototype_recovery_test.cpp:486`：检查 `return_rdx_rax_order` 的 return metadata 顺序为 `RAX, RDX`。

### 验证

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_prototype_recovery_test native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|prototype_recovery.input_candidates|register_effects.preserved)' -V
```

结果：通过。4 个测试全部通过，总用时 0.10 秒。

### 性能影响

return 前扫描从“找到一个 output store 就停”变成“扫描到 block 起点，收集每个 ABI output slot 最近 store”。开销仍只发生在 return block 或唯一前驱 block，和这些 block 的指令数相关。Bench2 全量时间没有在这一步重跑。

### 评分

- 实现效果：7/10。开始支持多个 output trial，并固定 ABI slot 顺序；仍不是完整 Ghidra output model rule。
- 复杂度：4/10。返回值扫描从 optional 变成 vector，但范围仍局部。
- 维护成本：4/10。后续做 SSA/PHI 后，这个扫描 helper 可能继续演进。

### 后续

- 多 output slot 在多 return path 下还需要更细的 value consistency。
- 多前驱统一 return block 仍要基于 SSA/PHI 做，不应简单扫所有前驱。
