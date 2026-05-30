# 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

上一小步让统一签名重写入口能用 `already matches` 跳过已匹配函数。`runNativePrototypeRecovery(... RewriteSignatures=true)` 会把 module rewrite summary 透传给 CLI/reporting，所以 opt-in 路径也应该覆盖这个 skip reason。

这一步不改核心逻辑，补上 opt-in summary 的验证，避免以后 CLI 统计回退。

# Ghidra 实现参考

Ghidra 的 prototype update 会把“已经匹配当前原型”和“需要更新”区分开，而不是把已匹配原型当失败：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::updateAllTypes(...)`：协调当前原型和推断出的 storage/type。
  - `FuncCallSpecs::deriveInputMap(...)` / `deriveOutputMap(...)`：生成最终 input/output map。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionFuncLink::apply(...)`：在已有函数原型基础上增量更新。

native 侧上一小步已经在 `rewriteNativeRecoveredPrototype(...)` 做了这个区分。这一步确认 opt-in pass pipeline 的 summary 也能看到相同结果。

# native 侧复刻策略

- 在现有 opt-in rewrite 测试 module 中加入一个签名已匹配的 input prototype。
- 继续调用 `runNativePrototypeRecovery(... RewriteSignatures=true)`。
- 验证：
  - seen 计数包含这个函数。
  - rewritten 计数不增加。
  - skipped 计数增加。
  - `SignatureRewriteSkippedByReason["already matches"] == 1`。

暂时不做：

- 改 CLI 输出格式。
- 改 summary 数据结构。

# 判断标准

- opt-in summary 能统计 already-matches skip reason。
- 现有 opt-in rewrite 成功和 missing prototype 统计继续正确。

# 风险

- 这是测试覆盖增强，核心代码不变。风险主要是计数同步错误。

# 实现记录

改动：

- `tests/native_prototype_recovery_test.cpp:2849` 在 opt-in rewrite module 里新增一个已经匹配 recovered prototype 的 input-only 函数。
- `tests/native_prototype_recovery_test.cpp:2860` 将 opt-in summary 的 `FunctionsSeen` / `FunctionsSkipped` 计数同步到新增样例，并验证 `SignatureRewriteSkippedByReason["already matches"] == 1`。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本步骤完成。

验证：

```sh
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
git diff --check
ctest --test-dir build --output-on-failure
```

结果：通过，目标测试 `1/1 Test #4: notdec.native_prototype_recovery.input_candidates ... Passed`；全量测试 `8/8` 通过。

性能：只增加一个已匹配样例和一次统计断言，不影响 runtime 行为。目标测试耗时约 `0.04 sec`，全量测试约 `0.82 sec`。

评分：

- 实现效果：6/10。只是把 already-matches 统计补到 opt-in 流程里。
- 复杂度：2/10。纯测试调整。
- 维护成本：2/10。后续 summary 变更时，这个样例会提醒统计不要回退。
