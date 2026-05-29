# 20260529-04 Require All Return Output Slot

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的返回值恢复会检查每个 `RETURN` 附近的 output trial，再交给 prototype model。它不会因为某一条 return path 写过 output register，就直接认为整个函数一定返回这个 register。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionReturnRecovery::apply(...)`：遍历 `CPUI_RETURN`，对 return output 做 trial 检查，然后调用 `deriveOutputMap(...)`。
  - `ActionReturnRecovery::buildReturnOutput(...)`：只把 used trial 写回 return output。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto::deriveOutputMap(...)`：把 active output trials 交给 `ProtoModel`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamListStandardOut::fillinMap(...)`：按 output storage 匹配 active trial，只有符合 ABI output entry 的 trial 才会 used。

当前 native 还没有完整 SSA return value 检查，所以应该更保守：多 return path 里如果某条 path 没看到同一个 output slot，就不要标成该函数返回候选。

## native 复刻方式

上一小步已经按 ABI output slot 去重，但还没有确认所有 return path 都有同一个 slot。这一步做最小保守过滤：

- 对每个 `ret` 仍用 `returnTrialBefore(...)` 找同 block 内最近 ABI output store。
- 统计函数里的 return 数量。
- 只有某个 ABI output slot 在所有 return path 都出现，才写入 `notdec.prototype.return_candidates`。
- 单 return 函数行为不变。
- 如果函数没有 `ret`，不产生返回候选。

暂不做：

- 不比较不同 return path 的实际 SSA 值。
- 不跨 basic block 追踪 return 前值。
- 不处理 `RDX:RAX` 多寄存器返回。

测试用例：

- 保留 `return_rax_twice`：两个 return 都写 `RAX`，应有一个 `RAX` 候选。
- 新增 `return_rax_partial`：一个 return 写 `RAX`，另一个 return 直接返回，应没有 `RAX` 候选。

## 判断标准

- `native_prototype_recovery_test` 通过。
- `return_rax_partial` 不产生返回候选。
- 现有 input candidate、单 return、双 return 去重测试不回退。

## 实现记录

### 改动

- `lib/passes/NativePrototypeRecovery.cpp:13`：引入 `std::map`。
- `lib/passes/NativePrototypeRecovery.cpp:153` 到 `lib/passes/NativePrototypeRecovery.cpp:175`：收集 return candidates 时统计 `returnCount` 和每个 ABI output slot 出现次数，只有出现次数等于 return 数量的 slot 才进入 `notdec.prototype.return_candidates`。
- `tests/native_prototype_recovery_test.cpp:147` 到 `tests/native_prototype_recovery_test.cpp:176`：新增 `createPartialReturnStoreFunction(...)`，构造一条 return path 写 `RAX`、另一条直接 return 的样例。
- `tests/native_prototype_recovery_test.cpp:269` 到 `tests/native_prototype_recovery_test.cpp:270`：新增 `return_rax_partial`。
- `tests/native_prototype_recovery_test.cpp:283` 到 `tests/native_prototype_recovery_test.cpp:309`：更新函数数量断言，并断言 partial return 不产生 `RAX` 返回候选。

### 验证

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_prototype_recovery_test native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|prototype_recovery.input_candidates|register_effects.preserved)' -V
```

结果：通过。`notdec.native_abi.cspec`、`notdec.native_prototype_model.register`、`notdec.native_prototype_recovery.input_candidates`、`notdec.native_register_effects.preserved` 全部通过，总用时 0.10 秒。

### 性能影响

每个函数多维护两个小 `std::map`，key 是 ABI output slot。x86-64 SysV 的 output slot 很少，单测没有观察到耗时变化。Bench2 全量时间没有在这一步重跑。

### 评分

- 实现效果：7/10。避免了“只有部分 return path 写返回寄存器”时的误标。
- 复杂度：3/10。仍是局部统计，没有引入跨块 SSA。
- 维护成本：3/10。后续做真实 value 一致性检查时，可以复用这层 return path 覆盖判断。

### 后续

- 跨 basic block 追踪 return 前 output value。
- 比较不同 return path 上的 output value 是否语义一致。
