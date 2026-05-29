# 20260529-04 Instcombine Register Store Safety

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 在 prototype recovery 前已经把 register varnode 的定义和使用放进 p-code SSA，store/definition 这类信息不会因为表达式规整而丢掉 storage 身份。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritage(...)`：把 varnode 的定义和使用接成 SSA。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionActiveParam::apply(...)`：在规整后的数据流上检查 active input。
  - `ActionReturnRecovery::apply(...)`：在规整后的数据流上检查 output trial。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamListStandard::fillinMap(...)`、`ParamListStandardOut::fillinMap(...)`：基于 active trials 选择参数和返回 storage。

native 侧如果以后把 `instcombine` 放进 prototype recovery 前的 canonicalization，除了 load，还要确认 register store 不会被优化成 `NativeRegisterSSA` 无法识别的形态。上一小步只验证了 load metadata 和 external input/replaced load 计数，这一步补 store。

## native 复刻方式

这一步继续不改 pipeline，只扩展安全测试：

- 构造一个函数，先 load `RDI`，做一个简单 add，再 store 回 `RDI`。
- load 和 store 都带 `notdec.register.access` metadata。
- baseline module 直接跑 `NativeRegisterSSA`。
- instcombine module 先跑 `InstCombinePass`，检查 register store metadata 仍存在。
- 再跑 `NativeRegisterSSA`，确认 `LoadsSeen`、`StoresSeen`、`LoadsReplaced`、`ExternalInputs` 不低于 baseline。

## 判断标准

- `native_instcombine_metadata_test` 通过。
- instcombine 后 register load/store metadata 都存在。
- instcombine 后 `NativeRegisterSSA` 的关键计数不低于 baseline。

## 实现记录

### 改动

- `tests/native_instcombine_metadata_test.cpp:50` 到 `tests/native_instcombine_metadata_test.cpp:70`：测试 module 增加 `RDI` store，store value 来自带 metadata 的 `RDI` load。
- `tests/native_instcombine_metadata_test.cpp:107` 到 `tests/native_instcombine_metadata_test.cpp:120`：新增 `hasRegisterAccessStore(...)`，检查 instcombine 后仍存在带 `notdec.register.access` 的 store。
- `tests/native_instcombine_metadata_test.cpp:153` 到 `tests/native_instcombine_metadata_test.cpp:157`：新增 store metadata 断言。
- `tests/native_instcombine_metadata_test.cpp:166` 到 `tests/native_instcombine_metadata_test.cpp:173`：增加 `LoadsSeen` 和 `StoresSeen` 对比，确认 instcombine 后 `NativeRegisterSSA` 没少看到 register load/store。

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

- 实现效果：6/10。覆盖 register load 和 store 的 metadata/计数安全，但还没覆盖 call barrier、复杂 CFG、prototype recovery metadata。
- 复杂度：2/10。只扩展现有测试。
- 维护成本：2/10。后续可继续在同一个测试里扩展 canonicalization 风险样例。

### 后续

- 补 call barrier 和 prototype recovery metadata 的 instcombine 安全样例。
- 安全样例足够后，再考虑把 canonicalization 接入 `notdec-native-llvm` pipeline。
