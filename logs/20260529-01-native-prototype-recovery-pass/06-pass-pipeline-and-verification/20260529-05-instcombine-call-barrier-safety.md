# 20260529-05 Instcombine Call Barrier Safety

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的 prototype recovery 依赖 call effect 判断，call 不能在规整过程中丢失副作用边界。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto::hasEffect(...)`：查询函数 prototype 对给定 storage 的影响。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::hasEffect(...)`、`ProtoModel::hasEffect(...)`：返回 `unaffected` / `killedbycall` / `unknown_effect`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionActiveParam::apply(...)`：callsite 参数恢复会调用 `FuncCallSpecs` 的 input trial use 检查。
  - `ActionReturnRecovery::apply(...)`：返回值恢复在 SSA 和 call effect 已稳定后做 output trial 判断。

native 侧的 `NativeRegisterSSA` 用 call barrier 防止把 call 前的 caller-saved register 值错误传播到 call 后。`instcombine` 放进 pipeline 前，需要确认它不会让普通 call 消失，也不会导致 call 后 register load 被错误替换。

## native 复刻方式

这一步继续只扩展安全测试，不改 pipeline：

- 构造一个函数：store `RDI`，call 一个外部函数，再 load `RDI`。
- 没有 ABI `unaffected` metadata，所以 call 对 `RDI` 保守视为 clobber。
- baseline module 直接跑 `NativeRegisterSSA`。
- instcombine module 先跑 `InstCombinePass`，再跑 `NativeRegisterSSA`。
- 对比 `CallsSeen` 不下降。
- 对比 call 后 register load 没有被错误替换：`LoadsReplaced` 不应比 baseline 增加。

## 判断标准

- `native_instcombine_metadata_test` 通过。
- instcombine 后 `CallsSeen` 不低于 baseline。
- call barrier 样例里 `LoadsReplaced` 不高于 baseline。

## 实现记录

### 改动

- `tests/native_instcombine_metadata_test.cpp:73` 到 `tests/native_instcombine_metadata_test.cpp:102`：新增 `createCallBarrierModule(...)`，构造 `store RDI -> call external_call -> load RDI` 的 call barrier 样例。
- `tests/native_instcombine_metadata_test.cpp:206` 到 `tests/native_instcombine_metadata_test.cpp:230`：分别在 baseline module 和 instcombine 后 module 上跑 `NativeRegisterSSA`。
- `tests/native_instcombine_metadata_test.cpp:233` 到 `tests/native_instcombine_metadata_test.cpp:236`：断言 instcombine 后 `CallsSeen` 不下降，并且 call barrier 样例里的 `LoadsReplaced` 不增加。

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

- 实现效果：6/10。覆盖普通外部 call barrier 的 instcombine 安全性，但还没覆盖 direct callee metadata、indirect call、prototype recovery metadata。
- 复杂度：2/10。只扩展一个测试 helper 和两条断言。
- 维护成本：2/10。后续可继续在同一测试里扩展更多 canonicalization 场景。

### 后续

- 补 direct callee preserves/clobbers metadata 的 instcombine 安全样例。
- 补 prototype recovery metadata 在 canonicalization 后的稳定性验证。
