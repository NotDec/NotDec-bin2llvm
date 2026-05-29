# 20260529-07 Instcombine Direct Callee Clobber Safety

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的 call effect 不只表达 preserved，也表达 killed-by-call。prototype recovery 不能把 call 前的 storage 值穿过 killed call。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto::hasEffect(...)`：查询 prototype 对给定 storage 的影响。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::hasEffect(...)`、`ProtoModel::hasEffect(...)`：返回 `unaffected` / `killedbycall` / `unknown_effect`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionFuncLink::apply(...)`：把 callsite 和 callee prototype 关联。
  - `ActionActiveParam::apply(...)`：callsite 参数恢复依赖 call effect 判断。

native 侧 `NativeRegisterSSA` 对 direct call 会读取 callee 的 `notdec.register.clobbers` metadata。`instcombine` 进入 pipeline 前，需要确认 canonicalization 不会丢掉 clobber metadata，也不会把 call 后 load 错误替换成 call 前 store。

## native 复刻方式

这一步继续只扩展安全测试，不改 pipeline：

- 构造一个 callee 函数，标注 `notdec.register.clobbers = RDI`。
- caller 中先 store `RDI`，direct call callee，再 load `RDI`。
- baseline module 直接跑 `NativeRegisterSSA`，call 后 load 不应该被替换。
- instcombine module 先跑 `InstCombinePass`，确认 callee 的 clobbers metadata 仍存在，再跑 `NativeRegisterSSA`。
- 对比 instcombine 后 `CallsSeen` 不下降，`LoadsReplaced` 不高于 baseline。

## 判断标准

- `native_instcombine_metadata_test` 通过。
- instcombine 后 callee 的 `notdec.register.clobbers` metadata 仍在。
- direct callee clobbers 样例里 `LoadsReplaced` 不高于 baseline。

## 实现记录

### 改动

- `tests/native_instcombine_metadata_test.cpp:117` 到 `tests/native_instcombine_metadata_test.cpp:128`：新增 `attachClobbersMetadata(...)`，给 callee 写入 `notdec.register.clobbers`。
- `tests/native_instcombine_metadata_test.cpp:166` 到 `tests/native_instcombine_metadata_test.cpp:200`：新增 `createDirectClobberModule(...)`，构造 `store RDI -> direct call clobbers_rdi -> load RDI` 样例。
- `tests/native_instcombine_metadata_test.cpp:398` 到 `tests/native_instcombine_metadata_test.cpp:430`：baseline 和 instcombine 后分别跑 `NativeRegisterSSA`，并检查 clobber callee metadata 仍在。
- `tests/native_instcombine_metadata_test.cpp:432` 到 `tests/native_instcombine_metadata_test.cpp:435`：断言 direct clobber call 数量不下降，call 后 load replacement 不增加。

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

- 实现效果：6/10。覆盖 direct callee clobbers metadata 的 instcombine 安全性，但还没覆盖 indirect call 和 prototype recovery metadata。
- 复杂度：3/10。新增一个 module 构造 helper 和 clobber metadata helper。
- 维护成本：3/10。后续间接 call 和 prototype metadata 测试可以继续复用当前测试骨架。

### 后续

- 补 indirect call 的 instcombine 安全样例。
- 补 prototype recovery metadata 在 canonicalization 后的稳定性验证。
