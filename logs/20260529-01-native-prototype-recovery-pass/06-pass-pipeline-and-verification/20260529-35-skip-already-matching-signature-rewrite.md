# 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

当前 `getNativePrototypeRewriteEligibility(...)` 能判断函数签名已经等于 recovered prototype，并返回 `already matches`。但统一入口 `rewriteNativeRecoveredPrototype(...)` 没有先看这个结果，会继续进入具体 rewrite helper。对于已经是 `void(i64)` 这类函数，helper 会因为原函数不是 `void()` 返回 `original function is not void()`，这个原因不准确。

module 级 rewrite 也会继承这个问题：已经匹配的函数会被当成失败形状，而不是明确跳过。

# Ghidra 实现参考

Ghidra 在原型恢复时会区分“原型已经符合当前推断”和“需要更新原型”：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::updateAllTypes(...)`：只有需要更新时才改写函数原型相关状态。
  - `FuncCallSpecs::deriveInputMap(...)` / `deriveOutputMap(...)`：生成 storage map 后再和当前原型协调。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionFuncLink::apply(...)`：围绕 call/function proto 做增量更新，不把已匹配原型当错误。

native 侧已有 `NativePrototypeRewriteEligibility`，这一步只是把它接到统一 rewrite 入口前面。

# native 侧复刻策略

- 在 `rewriteNativeRecoveredPrototype(...)` 开头读取 `getNativePrototypeRewriteEligibility(...)`。
- 如果 eligible 但 `NeedsRewrite == false`，直接返回 `Rewritten=false`、`Reason=already matches`。
- 缺 prototype、unsupported type 仍走原有 reason。
- 具体 shape helper 不改，保持它们作为低层 helper 的严格前置条件。

暂时不做：

- 改 shape-specific helper 的错误原因。
- 改 module summary 数据结构，只通过 skip reason 体现 already matches。

# 判断标准

- 已经匹配 recovered prototype 的函数通过统一 rewrite 返回 `already matches`。
- module 级 rewrite 对已匹配函数计入 `SkippedByReason["already matches"]`。
- 现有 rewrite 成功和失败原因测试继续通过。

# 风险

- module rewrite 的 skipped 数量会因为新增已匹配样例而变化；测试需要同步明确计数。

# 实现记录

改动：

- `lib/passes/NativePrototypeRecovery.cpp:1607` 在 `rewriteNativeRecoveredPrototype(...)` 开头调用 `getNativePrototypeRewriteEligibility(...)`。
- `lib/passes/NativePrototypeRecovery.cpp:1609` 对 eligible 且 `NeedsRewrite == false` 的函数直接返回 `already matches`，不再进入具体 rewrite helper。
- `tests/native_prototype_recovery_test.cpp:1757` 验证已匹配 input prototype 通过统一 rewrite 入口不会被重写，reason 是 `already matches`。
- `tests/native_prototype_recovery_test.cpp:2742` 在 batch rewrite 测试里加入一个已匹配 input prototype。
- `tests/native_prototype_recovery_test.cpp:2786` 同步 batch rewrite 计数，并验证 `SkippedByReason["already matches"] == 1`。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本步骤完成。

验证：

```sh
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
git diff --check
ctest --test-dir build --output-on-failure
```

结果：通过，目标测试 `1/1 Test #4: notdec.native_prototype_recovery.input_candidates ... Passed`；全量测试 `8/8` 通过。

性能：统一 rewrite 入口只多一次 eligibility 判断，module rewrite 对 already-matches 函数会更早返回。目标测试耗时约 `0.04 sec`，全量测试约 `0.80 sec`。

评分：

- 实现效果：7/10。module 和统一入口的 skip reason 更准确。
- 复杂度：3/10。只在统一入口加现有 eligibility gate。
- 维护成本：4/10。后续新 shape 也自动受这个 gate 保护。
