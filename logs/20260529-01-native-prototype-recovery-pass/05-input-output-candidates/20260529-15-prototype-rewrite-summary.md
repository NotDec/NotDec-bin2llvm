# 20260529-15 Prototype Rewrite Summary

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的 prototype recovery 不是只产出候选。候选被收敛进 `FuncProto` 后，后续 action 会按 prototype 状态继续处理函数输入和返回。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto`：保存当前函数的参数、返回值和 prototype model。
  - `ProtoModel::deriveInputMap(...)` / `ProtoModel::deriveOutputMap(...)`：把 `ParamActive` 里的 trial 变成最终 prototype。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::isInputLocked()`：判断输入 prototype 是否已经固定。
  - `FuncProto::setInputLock(...)` / `FuncProto::setOutputLock(...)`：固定输入和输出时同步固定 model。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：按 `FuncProto` 更新 RETURN 输出和锁定输入。
  - `ActionInputPrototype::apply(...)`：在输入未锁定时注册 trial 并派生输入 prototype。

native 侧上一小步已经有 `getNativePrototypeRewriteEligibility(...)`。但 summary 里还看不到有多少函数已经能进入后续签名重写，也看不到有多少函数类型确实需要改。

## native 复刻方式

这一步只把 eligibility 接进 `NativePrototypeRecoverySummary`：

- 每个函数 summary 增加：
  - `RewriteEligible`
  - `NeedsSignatureRewrite`
- 总 summary 增加：
  - `RewriteEligibleFunctions`
  - `SignatureRewriteNeededFunctions`
- `runNativePrototypeRecovery(...)` 在写完 recovered metadata 后调用 `getNativePrototypeRewriteEligibility(...)`。
- `printNativePrototypeRecoverySummary(...)` 打印这两个计数。

这一步仍然不替换函数、不改 callsite。它只是把 Ghidra “prototype 已经可以进入后续 ActionPrototypeTypes” 这个状态显式统计出来。

## 判断标准

- input-only、return-only、已匹配类型的 recovered prototype 都计入 rewrite eligible。
- 已匹配类型的函数不计入 needs rewrite。
- 多 return 当前不能形成 `FunctionType`，不计入 rewrite eligible。
- 没有 recovered metadata 的函数不计入 rewrite eligible。

## 实现记录

### 源码改动

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:70`
  - `NativePrototypeRecoveryFunctionSummary` 增加 `RewriteEligible` 和 `NeedsSignatureRewrite`。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:79`
  - `NativePrototypeRecoverySummary` 增加 `RewriteEligibleFunctions` 和 `SignatureRewriteNeededFunctions`。
- `lib/passes/NativePrototypeRecovery.cpp:209`
  - `addFunctionSummary(...)` 汇总 rewrite eligibility 计数。
- `lib/passes/NativePrototypeRecovery.cpp:403`
  - `runNativePrototypeRecovery(...)` 写完 `notdec.prototype.recovered` metadata 后调用 `getNativePrototypeRewriteEligibility(...)`。
  - 只统计状态，不替换函数、不改 callsite。
- `lib/passes/NativePrototypeRecovery.cpp:511`
  - `printNativePrototypeRecoverySummary(...)` 打印总计和每个函数的 rewrite eligibility 状态。
- `tests/native_prototype_recovery_test.cpp:524`
  - 验证 13 个测试函数里 7 个可进入后续签名重写，6 个确实需要改签名。

### 验证

```sh
cmake -S . -B build \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON
cmake --build build --target native_prototype_recovery_test native_instcombine_metadata_test -j2
ctest --test-dir build -R 'notdec.native_(prototype_recovery.input_candidates|instcombine.metadata)' -V
```

结果：`notdec.native_prototype_recovery.input_candidates` 和 `notdec.native_instcombine.metadata` 通过。

### 性能和风险

- 性能：每个函数多一次 metadata 读取和 `FunctionType` 构造。只和函数数、参数数线性相关，当前测试耗时无变化。
- 风险：summary 现在会暴露“可重写但未重写”的状态，后续 CLI 使用时不能把这个计数误解为已经完成签名修复。
- 实现效果：5/10。能看到后续签名重写影响面，但仍未改 IR。
- 复杂度：2/10。只加计数和打印。
- 维护成本：2/10。后续真正 rewrite pass 可以继续复用同一字段。
