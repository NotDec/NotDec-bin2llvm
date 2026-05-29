# 20260529-03 Deduplicate Return Candidates

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的返回值恢复不是按 `RETURN` 指令个数直接输出结果，而是把 output storage 注册成 trial，再由 prototype model 的 output `ParamList` 归并和筛选。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionReturnRecovery::apply(...)`：遍历 `CPUI_RETURN`，为 return output 建 active trial，然后调用 `deriveOutputMap(...)`。
  - `ActionReturnRecovery::buildReturnOutput(...)`：按 used trial 生成最终 return output。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto::deriveOutputMap(...)`：把 active output trials 交给当前 `ProtoModel`。
  - `ProtoModel::deriveOutputMap(...)`：调用 output `ParamList` 的 `fillinMap(...)`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamActive::registerTrial(...)`：相同 storage 会在 trial 容器中作为同类候选处理。
  - `ParamListStandardOut::fillinMap(...)`：按 output entry 匹配 active trial，并把能作为返回 storage 的 trial 标成 used。

这里的关键点是 output storage，而不是 return 语句数量。多个 return path 都写 `RAX` 时，最终也应该是一个 `RAX` 返回候选。

## native 复刻方式

当前 native 侧只做 metadata，不改 LLVM 函数签名。已有实现会对每个 `ret` 向前扫描最近的 ABI output register store，并把每个 return path 都追加到 `notdec.prototype.return_candidates`。

这一步先做最小修正：

- 继续复用 `NativeParamTrial` / `NativeParamActive`。
- 收集 return candidates 时按 ABI output slot 去重。
- 如果两个 return path 都是 `RAX` / slot 0，只保留一条 metadata。
- 先不判断不同 return path 的值是否一致；这个留给后续跨 basic block SSA 语义检查。

测试用例：

- 新增一个有两个 return block 的函数，两个 return 前都写 `RAX`。
- 运行 prototype recovery 后，函数级 `notdec.prototype.return_candidates` 里 `RAX` 只出现一次。
- 总 summary 的 `ReturnCandidates` 不因两个 return path 重复增加。

## 判断标准

- `native_prototype_recovery_test` 通过。
- 已有 input candidate 和单 return candidate 测试不回退。
- module verify 通过。

## 实现记录

### 改动

- `lib/passes/NativePrototypeRecovery.cpp:14`：引入 `std::set`。
- `lib/passes/NativePrototypeRecovery.cpp:91` 到 `lib/passes/NativePrototypeRecovery.cpp:98`：新增 `addUniqueTrialBySlot(...)`，按 ABI slot 去重 return trial。
- `lib/passes/NativePrototypeRecovery.cpp:152` 到 `lib/passes/NativePrototypeRecovery.cpp:159`：收集 return candidates 时用 `returnSlots` 去重，多个 return path 写同一个 output slot 时只保留一条 metadata。
- `tests/native_prototype_recovery_test.cpp:114` 到 `tests/native_prototype_recovery_test.cpp:145`：新增 `createTwoReturnStoreFunction(...)`，构造两个 return block 都写同一个 register 的样例。
- `tests/native_prototype_recovery_test.cpp:186` 到 `tests/native_prototype_recovery_test.cpp:207`：新增 `countMetadataRegister(...)`，检查 metadata 中同一 register 出现次数。
- `tests/native_prototype_recovery_test.cpp:236` 到 `tests/native_prototype_recovery_test.cpp:237`：新增 `return_rax_twice` 测试函数。
- `tests/native_prototype_recovery_test.cpp:249` 到 `tests/native_prototype_recovery_test.cpp:270`：更新 summary 断言，并断言 `return_rax_twice` 只产生一个 `RAX` return candidate。

### 验证

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_prototype_recovery_test native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|prototype_recovery.input_candidates|register_effects.preserved)' -V
```

结果：通过。`notdec.native_abi.cspec`、`notdec.native_prototype_model.register`、`notdec.native_prototype_recovery.input_candidates`、`notdec.native_register_effects.preserved` 全部通过，总用时 0.10 秒。

### 性能影响

每个函数收集 return candidates 时增加一个小 `std::set<uint64_t>`，规模等于 ABI output slot 数。当前 x86-64 SysV 只涉及很少的返回寄存器，影响可以忽略。Bench2 全量时间没有在这一步重跑。

### 评分

- 实现效果：7/10。多 return path 不再重复标同一返回寄存器，但还没有验证不同路径上的值是否一致。
- 复杂度：2/10。只按 ABI slot 去重，没有引入新数据流。
- 维护成本：2/10。后续多寄存器返回可以继续扩展 `NativeParamTrial`，这次不阻碍。

### 后续

- 补跨 basic block 的 return 前 SSA 值检查。
- 对不同 return path 的 output value 一致性做保守判断。
