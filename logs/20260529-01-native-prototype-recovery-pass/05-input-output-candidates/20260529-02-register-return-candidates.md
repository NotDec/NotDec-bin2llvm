# 20260529-02 Register Return Candidates

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的返回值恢复也走 `ParamActive` / `ParamTrial`，只是候选来源来自 `RETURN` 附近的 output storage。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionReturnRecovery::apply(...)`：遍历 `CPUI_RETURN`，检查 active output trial 在 return 点是否有真实数据流使用，active 后调用 `deriveOutputMap(...)`，再用 `buildReturnOutput(...)` 把返回值接到 `RETURN`。
  - `ActionReturnRecovery::buildReturnOutput(...)`：按 used trial 顺序把 return varnode 重新放进 `CPUI_RETURN`。
  - `ActionActiveReturn::apply(...)`：处理 callsite 的输出参数，调用 `checkOutputTrialUse(...)`、`deriveOutputMap(...)` 和 `buildOutputFromTrials(...)`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ProtoModel::deriveOutputMap(...)`：把 active output trial 交给 output `ParamList`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamListStandardOut::fillinMap(...)`：只把 active 且能匹配 ABI output pentry 的 trial 标成 used。
  - `ParamListStandardOut::fillinMapFallback(...)`：在规则不足时找覆盖最好的 output entry。

关键点是：返回值也不是“所有写过的寄存器”。Ghidra 会看 return 点附近 active 的 output storage，再用 ABI output map 筛选。

## native 复刻方式

当前 native 侧还没有 P-Code `RETURN` input，也不改 LLVM 函数签名。这一步先做最小可验证版本：

- 继续使用 `NativeParamTrial` / `NativeParamActive`。
- 遍历每个函数的 `ret` 指令。
- 在同一个 basic block 内，从 `ret` 往前找最近的 `store` 到 register global。
- 如果 store 带 `notdec.register.access`，且 register 能被 `NativePrototypeModel::findOutputRegister(...)` 匹配，就写入 `notdec.prototype.return_candidates`。
- 先只记录寄存器名和 ABI slot。

这个做法只覆盖当前测试用例，也符合第一阶段 metadata 目标。后续再补：

- 跨 basic block 的 return 前 SSA 值。
- 多 return path 合并。
- `RDX:RAX` 这类多寄存器返回。
- 过滤 trash value / constant artifact。

测试用例：

- 函数 `return_rax` 在 `ret` 前 store `RAX`，应标成返回候选。
- 函数 `return_rbx` 在 `ret` 前 store `RBX`，不是 ABI output，不应标成返回候选。
- 保留上一小步的 `RDI` 输入候选和 `RBX` saved-register 过滤测试。

## 判断标准

- `RAX` return 前 store 会写入 `notdec.prototype.return_candidates`。
- `RBX` return 前 store 不会写入返回候选。
- 输入候选逻辑不回退。
- 现有 ABI、prototype model、register effects 测试不回退。

## 实现记录

修改文件：

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:38`：summary 增加 `ReturnCandidates`。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:45`：pass 总 summary 增加 `ReturnCandidates`。
- `lib/passes/NativePrototypeRecovery.cpp:49`：新增 `returnTrialBefore(...)`，从 `ret` 往前扫描同 basic block 内最近的 register store。
- `lib/passes/NativePrototypeRecovery.cpp:67`：用 `NativePrototypeModel::findOutputRegister(...)` 过滤 ABI output register。
- `lib/passes/NativePrototypeRecovery.cpp:142`：遍历函数里的 `ReturnInst`，收集返回候选 trial。
- `lib/passes/NativePrototypeRecovery.cpp:160`：写 `notdec.prototype.return_candidates`。
- `lib/passes/NativePrototypeRecovery.cpp:181`：summary 输出返回候选数量。
- `tests/native_prototype_recovery_test.cpp:48`：新增 `registerAccessMetadata(...)`，构造 register store metadata。
- `tests/native_prototype_recovery_test.cpp:95`：新增 `createReturnStoreFunction(...)`，构造 return 前 store register 的样例。
- `tests/native_prototype_recovery_test.cpp:176`：新增 `return_rax` 和 `return_rbx` 测试函数。
- `tests/native_prototype_recovery_test.cpp:196`：断言返回候选数量为 1。
- `tests/native_prototype_recovery_test.cpp:204`：断言 `RAX` 被标成返回候选，`RBX` 没有被标成返回候选。

验证命令：

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_prototype_recovery_test native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|prototype_recovery.input_candidates|register_effects.preserved)' -V
```

结果：通过。`notdec.native_abi.cspec`、`notdec.native_prototype_model.register`、`notdec.native_prototype_recovery.input_candidates`、`notdec.native_register_effects.preserved` 全部通过，总用时 0.10 秒。

调试记录：

- 第一次测试失败，原因是 `reverse_iterator(ret.getIterator())` 已经指向 `ret` 前一条指令，代码里额外 `++iter` 跳过了紧邻的 store。
- 修正后 `RAX` return 前 store 能正常识别。

## 风险

- 当前只看同 basic block 内 return 前的 store，不处理跨块 SSA 值。
- 多 return path 现在会把每个 return path 的候选都记录下来，还没有去重和一致性判断。
- 只支持单寄存器返回，`RDX:RAX` 和结构体返回后续再做。

## 评分

- 实现效果：6/10。能覆盖第一版 return register metadata，但还不是完整 Ghidra output recovery。
- 复杂度：4/10。扫描 return 前 store，改动小。
- 维护成本：5/10。输入和返回候选共用 `NativeParamTrial`，短期够用；后续多寄存器返回时要扩展 trial 描述。
