# 20260529-11 Instcombine Conflicting Return Safety

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的返回值恢复会先登记 output trial，再检查这些 trial 在所有 return 点的真实使用情况。最后 `deriveOutputMap(...)` 只选择符合 prototype model 的输出 trial。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionActiveReturn::apply(...)`：处理 callsite 的 active output，调用 `checkOutputTrialUse(...)`、`deriveOutputMap(...)`、`buildOutputFromTrials(...)`。
  - `ActionReturnRecovery::apply(...)`：遍历 `CPUI_RETURN`，用 `AncestorRealistic` / `ancestorOpUse(...)` 检查 trial 是否 active；fully checked 后调用 `data.getFuncProto().deriveOutputMap(active)`。
  - `ActionReturnRecovery::buildReturnOutput(...)`：把选中的 return output 写回 `CPUI_RETURN`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ProtoModel::deriveOutputMap(...)`：把 output trial 交给 output `ParamList::fillinMap(...)`。
  - `FuncProto::deriveOutputMap(...)`：调用当前 prototype model 的 output map 推导。
  - `ParamActive` / `ParamTrial`：保存 output storage trial、slot、active/used 状态。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamActive::registerTrial(...)`：登记 storage trial。
  - `ParamActive::whichTrial(...)`：按 storage overlap 查找已有 trial。

native 侧已经有一条逻辑：多 return path 同一 output slot 的简单返回值不一致时，不写 `notdec.prototype.return_candidates`。`instcombine` 放进链路前，要确认这个反例在规整后仍然不误标。

## native 复刻方式

这一步只扩展安全测试：

- 构造一个带 `i1` 参数的函数，按参数分支到两个 return block。
- 两个 return block 都 store `RAX`，但写入不同常量。
- module 上写入测试 ABI：`RAX` 是 output slot 0。
- 先跑 `InstCombinePass`，再跑 `NativeRegisterSSA` 和 `NativePrototypeRecovery`。
- 检查函数上没有 `notdec.prototype.return_candidates = RAX`。

## 判断标准

- `native_instcombine_metadata_test` 通过。
- instcombine 后冲突 return 样例不会标出 RAX 返回候选。

## 实现记录

### 改动

- `tests/native_instcombine_metadata_test.cpp:157` 到 `tests/native_instcombine_metadata_test.cpp:192`：新增 `createConflictingReturnPrototypeModule(...)`，构造两个 return path，分别 store `0x1111` 和 `0x2222` 到 `RAX`。
- `tests/native_instcombine_metadata_test.cpp:678` 到 `tests/native_instcombine_metadata_test.cpp:696`：对冲突返回样例先跑 `InstCombinePass`，再跑 `NativeRegisterSSA` 和 `NativePrototypeRecovery`，并验证 module。
- `tests/native_instcombine_metadata_test.cpp:698` 到 `tests/native_instcombine_metadata_test.cpp:703`：断言返回候选计数为 0，并检查函数 metadata 中没有 `RAX` 返回候选。

### 验证

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_instcombine_metadata_test native_prototype_recovery_test native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|prototype_recovery.input_candidates|register_effects.preserved|instcombine.metadata)' -V
```

结果：通过。5 个测试全部通过，总用时 0.13 秒。

### 性能影响

只扩展测试，不改运行时 pipeline，没有运行时性能影响。Bench2 全量时间没有在这一步重跑。

### 评分

- 实现效果：6/10。覆盖 instcombine 后冲突返回值不会误标返回候选，但还没覆盖真实 Bench2 的复杂 CFG。
- 复杂度：3/10。新增一个反例 module 和一段验证逻辑。
- 维护成本：3/10。复用现有 ABI helper 和 metadata 检查函数。

### 后续

- 开始把前面的 instcombine 安全验证结果用于正式 pipeline 接入判断。
- Bench2 selected native 全量验证仍未完成。
