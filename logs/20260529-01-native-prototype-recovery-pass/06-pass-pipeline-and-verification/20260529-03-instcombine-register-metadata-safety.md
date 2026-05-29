# 20260529-03 Instcombine Register Metadata Safety

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的 prototype recovery 运行在 decompiler 自己的 p-code/SSA 规整流程之后，不依赖 LLVM `instcombine`。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritage(...)`：先把 varnode 定义和使用连成 SSA。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionActiveParam::apply(...)`：在数据流已经规整后判断 input trial 是否 active。
  - `ActionReturnRecovery::apply(...)`：在 SSA 后检查 return output trial。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamListStandard::fillinMap(...)`、`ParamListStandardOut::fillinMap(...)`：基于规整后的 active trials 选择参数和返回 storage。

native 侧 GOAL 里计划在识别 saved register、参数、返回值前做一层保守 LLVM canonicalization。第一候选是 `instcombine`，但放进 pipeline 前要先验证它不会破坏 register metadata 和 `NativeRegisterSSA` 的识别。

## native 复刻方式

这一步不直接改 `notdec-native-llvm` pipeline，只增加安全验证：

- 构造一个带 `notdec.register.access` metadata 的 register global load。
- 先在原始 module 上跑 `NativeRegisterSSA`，记录 `ExternalInputs` / `LoadsReplaced`。
- 再在等价 module 上跑 LLVM `InstCombinePass`，确认 register load 上的 metadata 还在。
- 然后跑 `NativeRegisterSSA`，确认 `ExternalInputs` / `LoadsReplaced` 没有下降。

测试只覆盖当前最关键风险：`instcombine` 不应让 register access metadata 消失，也不应让 `NativeRegisterSSA` 看不到入口 register load。

## 判断标准

- 新增测试通过。
- instcombine 后 `notdec.register.access` metadata 仍存在。
- instcombine 后 `NativeRegisterSSA` 的 `ExternalInputs` 和 `LoadsReplaced` 不低于 baseline。

## 实现记录

### 改动

- `tests/native_instcombine_metadata_test.cpp:1` 到 `tests/native_instcombine_metadata_test.cpp:18`：新增测试文件，引入 `NativeRegisterSSA`、LLVM pass manager 和 `InstCombinePass`。
- `tests/native_instcombine_metadata_test.cpp:22` 到 `tests/native_instcombine_metadata_test.cpp:68`：构造带 `notdec.register` global 和 `notdec.register.access` load 的最小 module。
- `tests/native_instcombine_metadata_test.cpp:70` 到 `tests/native_instcombine_metadata_test.cpp:87`：新增 `runInstCombine(...)`，用 LLVM new pass manager 跑 `InstCombinePass`。
- `tests/native_instcombine_metadata_test.cpp:89` 到 `tests/native_instcombine_metadata_test.cpp:102`：新增 `hasRegisterAccessLoad(...)`，检查 instcombine 后 register load metadata 仍存在。
- `tests/native_instcombine_metadata_test.cpp:114` 到 `tests/native_instcombine_metadata_test.cpp:150`：对比 baseline 和 instcombine 后的 `NativeRegisterSSA` summary，确认 `ExternalInputs` / `LoadsReplaced` 没下降。
- `CMakeLists.txt:194` 到 `CMakeLists.txt:207`：新增 `native_instcombine_metadata_test` 和 `notdec.native_instcombine.metadata`。

### 验证

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_instcombine_metadata_test native_prototype_recovery_test native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|prototype_recovery.input_candidates|register_effects.preserved|instcombine.metadata)' -V
```

结果：通过。5 个测试全部通过，总用时 0.13 秒。

### 性能影响

这一步只新增测试，不改运行时 pipeline，没有运行时性能影响。Bench2 全量时间没有在这一步重跑。

### 评分

- 实现效果：5/10。覆盖了 register metadata 和 `NativeRegisterSSA` 关键计数的最小安全样例，还没覆盖复杂 CFG、store、call barrier 和 prototype recovery metadata。
- 复杂度：3/10。新增一个独立测试，不改主流程。
- 维护成本：3/10。后续如果 pipeline 接入 canonicalization，可以扩展这个测试而不是重写。

### 后续

- 继续补 store metadata、call barrier、prototype recovery metadata 的 instcombine 安全样例。
- 只有这些验证稳定后，再考虑把 canonicalization 放入 `notdec-native-llvm` pipeline。
