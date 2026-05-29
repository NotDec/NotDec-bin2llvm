# 20260529-15 Signature Rewrite Skip Reasons

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 不只给出 prototype 结果，也会在 action 状态里保留为什么当前轮不能继续推进。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionInputPrototype::apply(...)`：调用 input recovery，失败或没有 active trial 时不会强行改 prototype。
  - `ActionReturnRecovery::apply(...)`：只在 output trial 明确时更新返回候选。
  - `ActionOutputPrototype::apply(...)`：根据当前 `FuncProto` 和 output trial 决定是否修改输出。
  - `ActionPrototypeTypes::apply(...)`：把已经稳定的 prototype 应用到类型传播。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto::deriveInputMap(...)` / `FuncProto::deriveOutputMap(...)`：prototype 推导入口。
  - `FuncProto::updateAllTypes(...)`：把最终 prototype 应用到函数体。

native 侧现在已有 `NativePrototypeRewriteResult::Reason`，但 module summary 只统计总 skip 数。继续做 callsite rewrite 前，需要先能看到到底是“有调用者”“缺 recovered metadata”还是“形状不支持”。

## native 复刻方式

这一步只补统计，不改变重写策略：

- `NativePrototypeModuleRewriteSummary` 增加 `SkippedByReason`。
- `rewriteNativeRecoveredPrototypes(...)` 在跳过时按 `NativePrototypeRewriteResult::Reason` 累加。
- `NativePrototypeRecoverySummary` 增加同样的汇总字段，并在 opt-in rewrite 后带出来。
- `printNativePrototypeRecoverySummary(...)` 输出每类 skip reason。
- 测试复用已有 batch rewrite 样例，检查当前已知的 skip reason 计数。

这一步不新增 callsite rewrite，也不改变已有 reason 字符串。

## 判断标准

- 现有 rewrite 行为不变。
- module 级 summary 能区分 skip reason。
- opt-in pass summary 能把 skip reason 传出来。
- 现有 prototype recovery 测试通过。

## 实现记录

已实现。

### 改动位置

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:93` 的 `NativePrototypeModuleRewriteSummary` 增加 `SkippedByReason`。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:111` 的 `NativePrototypeRecoverySummary` 增加 `SignatureRewriteSkippedByReason`。
- `lib/passes/NativePrototypeRecovery.cpp:469` 的 `runNativePrototypeRecovery(...)` 在 opt-in rewrite 后把 skip reason 带到 pass summary。
- `lib/passes/NativePrototypeRecovery.cpp:908` 的 `rewriteNativeRecoveredPrototypes(...)` 在跳过时按 reason 计数。
- `lib/passes/NativePrototypeRecovery.cpp:932` 的 `printNativePrototypeRecoverySummary(...)` 输出 skip reason 统计。
- `tests/native_prototype_recovery_test.cpp:1100` 的 batch rewrite 样例检查 `function has uses` 和 `missing recovered prototype`。
- `tests/native_prototype_recovery_test.cpp:1160` 的 opt-in rewrite 样例检查 pass summary 里的 `missing recovered prototype`。

### 验证

命令：

```sh
cmake -S . -B build \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON &&
cmake --build build --target native_prototype_recovery_test -j2 &&
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
```

结果：1 个测试通过。

### 性能和风险

- 默认 rewrite 逻辑不变，只在已有 skip 分支上多做一次 `std::map` 计数。
- 默认不打开签名重写时不会调用 module rewrite，因此默认链路没有新增统计成本。
- reason 字符串仍来自现有 `NativePrototypeRewriteResult::Reason`，后续如果改 reason 文案，测试也要同步调整。

### 评分

- 实现效果：8/10。能区分当前主要跳过原因，方便后续 callsite rewrite。
- 复杂度：9/10。只增加 summary 字段和计数。
- 维护成本：8/10。使用字符串 reason 简单直接；后续若 reason 种类变多，再考虑 enum。
