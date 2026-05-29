# 20260529-08 Instcombine Indirect Call Safety

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 对无法精确解析 effect 的 call 不能乐观穿透 storage 值。prototype recovery 要尊重 `unknown_effect` / killed-by-call 这类保守结果。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto::hasEffect(...)`：查询某个 storage 在调用处的 effect。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::hasEffect(...)`、`ProtoModel::hasEffect(...)`：返回 `unaffected` / `killedbycall` / `unknown_effect`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionActiveParam::apply(...)`：callsite 参数恢复依赖 call effect 和 trial use 检查。
  - `ActionFuncLink::apply(...)`：尝试把 callsite 和 callee prototype 联系起来；无法精确解析时仍要保守。

native 侧 `NativeRegisterSSA` 对 indirect call / unresolved call 使用保守 barrier，避免把 call 前 register store 传播到 call 后 load。`instcombine` 进入 pipeline 前，需要确认 indirect call 形态和 barrier 行为不被破坏。

## native 复刻方式

这一步继续只扩展安全测试，不改 pipeline：

- 构造一个函数：store `RDI`，通过函数指针参数 indirect call，再 load `RDI`。
- baseline module 直接跑 `NativeRegisterSSA`，call 后 load 不应该被替换。
- instcombine module 先跑 `InstCombinePass`，再跑 `NativeRegisterSSA`。
- 对比 instcombine 后 `CallsSeen` 不下降，`LoadsReplaced` 不高于 baseline。

## 判断标准

- `native_instcombine_metadata_test` 通过。
- instcombine 后 indirect call 仍被 `NativeRegisterSSA` 看到。
- indirect call barrier 样例里 `LoadsReplaced` 不高于 baseline。

## 实现记录

### 改动

- `tests/native_instcombine_metadata_test.cpp:202` 到 `tests/native_instcombine_metadata_test.cpp:228`：新增 `createIndirectCallModule(...)`，构造 `store RDI -> indirect call -> load RDI` 样例。
- `tests/native_instcombine_metadata_test.cpp:465` 到 `tests/native_instcombine_metadata_test.cpp:490`：baseline 和 instcombine 后分别跑 `NativeRegisterSSA`，并验证 module。
- `tests/native_instcombine_metadata_test.cpp:492` 到 `tests/native_instcombine_metadata_test.cpp:495`：断言 indirect call 数量不下降，call 后 load replacement 不增加。

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

- 实现效果：6/10。覆盖 indirect call barrier 的 instcombine 安全性，但还没把 instcombine 接入正式 pipeline。
- 复杂度：3/10。只新增一个测试 module 构造 helper 和两组对比断言。
- 维护成本：3/10。后续可以继续在同一测试文件补 prototype recovery metadata 稳定性。

### 后续

- 补 prototype recovery metadata 在 canonicalization 后的稳定性验证。
- 再决定是否把 instcombine 放进 native prototype recovery pipeline。
