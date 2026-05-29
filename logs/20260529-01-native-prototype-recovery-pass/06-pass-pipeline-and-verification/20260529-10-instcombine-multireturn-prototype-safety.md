# 20260529-10 Instcombine Multireturn Prototype Safety

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 恢复返回值时不会只看某一个 return 点，而是把函数出口附近的 output storage 作为候选，并要求候选在控制流上有一致含义。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionActiveReturn::apply(...)`：分析 return output trial。
  - `ActionReturnRecovery::apply(...)`：推动返回值恢复。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamActive` / `ParamTrial`：保存 output trial 和是否 active。
  - `FuncProto`：保存 output prototype。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamActive::registerTrial(...)`：登记候选 storage。
  - `ProtoModel::deriveOutputMap(...)`：按 prototype model 推导输出 storage。

native 侧当前已经要求多 return path 都覆盖同一个 ABI output slot，且简单过滤冲突值。`instcombine` 放进链路前，要确认多出口样例在规整后仍能产出返回候选。

## native 复刻方式

这一步只扩展安全测试：

- 构造一个带 `i1` 参数的函数，按参数分支到两个 return block。
- 两个 return block 都 store 同一个常量到 `RAX`，并带 `notdec.register.access` metadata。
- module 上写入测试 ABI：`RAX` 是 output slot 0。
- 先跑 `InstCombinePass`，再跑 `NativeRegisterSSA` 和 `NativePrototypeRecovery`。
- 检查函数上仍有 `notdec.prototype.return_candidates = RAX`。

## 判断标准

- `native_instcombine_metadata_test` 通过。
- instcombine 后多 return 样例仍能标出 RAX 返回候选。

## 实现记录

### 改动

- `tests/native_instcombine_metadata_test.cpp:121` 到 `tests/native_instcombine_metadata_test.cpp:155`：新增 `createMultireturnPrototypeModule(...)`，构造 `condbr -> left/right` 两个 return path，两个出口都 store 同一个 `RAX` 常量。
- `tests/native_instcombine_metadata_test.cpp:614` 到 `tests/native_instcombine_metadata_test.cpp:632`：对多 return 样例先跑 `InstCombinePass`，再跑 `NativeRegisterSSA` 和 `NativePrototypeRecovery`，并验证 module。
- `tests/native_instcombine_metadata_test.cpp:634` 到 `tests/native_instcombine_metadata_test.cpp:639`：断言返回候选计数存在，并检查函数 metadata 中有 `RAX` 返回候选。

### 验证

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_instcombine_metadata_test native_prototype_recovery_test native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|prototype_recovery.input_candidates|register_effects.preserved|instcombine.metadata)' -V
```

结果：通过。5 个测试全部通过，总用时 0.14 秒。

### 性能影响

只扩展测试，不改运行时 pipeline，没有运行时性能影响。Bench2 全量时间没有在这一步重跑。

### 评分

- 实现效果：6/10。覆盖 instcombine 后多 return 返回候选稳定性，但还没覆盖冲突返回值和真实 Bench2 输入。
- 复杂度：3/10。新增一个两出口测试 module 和一段验证逻辑。
- 维护成本：3/10。仍复用现有测试 helper，后续可继续补冲突路径。

### 后续

- 覆盖 instcombine 后冲突返回值不会误标返回候选。
- 再决定是否把 instcombine 放进 native prototype recovery pipeline。
