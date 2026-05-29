# 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

上一小步已经把 callsite return load 的不确定 CFG 形状改成阻断。但 `rewriteNativeRecoveredPrototypeReturnOnly(...)` 和 `rewriteNativeRecoveredPrototypeInputReturn(...)` 仍把这些失败都报成 `function has uses`。

真实 Bench2 调试时，这个 reason 太粗。需要区分普通“有无法处理的 use”和“callsite 返回寄存器读取不安全”，这样 CLI 的 skip reason 统计能直接反映当前卡在哪里。

# Ghidra 实现参考

Ghidra 的 prototype / callsite 信息不是一个粗略布尔值：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncCallSpecs`：每个 callsite 持有独立 prototype 信息。
  - `FuncCallSpecs::hasEffectTranslate(...)`：按 callsite 视角查询 storage effect。
  - `FuncProto::hasEffect(...)`：返回 `EffectRecord::unaffected`、`killedbycall`、`unknown_effect` 等具体 effect。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::hasEffect(...)`：优先查函数自己的 effect list，否则回落到 `ProtoModel`。
  - `FuncCallSpecs::hasEffectTranslate(...)`：遇到无法翻译的 stack offset 返回 `unknown_effect`。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionReturnRecovery::apply(...)`：通过 `ParamTrial` 的 checked/active/used 状态逐步筛返回 trial。
  - `ActionReturnRecovery::buildReturnOutput(...)`：只把确认 used 的 trial 接到 `RETURN`。

也就是说，Ghidra 会保留更细的状态，后续 action 可以知道是 unknown effect、未 active，还是 storage 不匹配。native 侧现在还没有完整 `FuncCallSpecs`，但 skip reason 应该先别把所有 callsite 失败压成一个原因。

# native 侧复刻策略

这一步只改诊断，不扩大签名重写范围：

- 给 return-only direct callsite 收集返回一个小结果，包含 callsite 列表和失败原因。
- `callsiteHasMismatchedReturnLoad(...)` 保持策略不变，但失败时上层 reason 写成 `unsafe callsite return load`。
- input-return rewrite 里同样把返回 load unsafe 与 input 参数收集失败区分开。
- 测试先覆盖 return-only 的 clobber / 不确定 CFG 负例 reason；后续再补 input-return 的同类细分。

# 判断标准

- unsafe return load 不再报 `function has uses`。
- 现有 direct callsite rewrite 正例不回退。
- module 级 `SkippedByReason` 统计能出现新 reason。
- `native_prototype_recovery_test` 通过。

# 风险

- 这一步只是更细的 reason，不解决多分支/PHI 下真实可重写的返回值接线。
- reason 字符串会进入测试和 CLI 输出，后续改名要同步测试。

# 实现记录

改动：

- `lib/passes/NativePrototypeRecovery.cpp:299` 新增 `ReturnOnlyCallsiteCollectionResult`，让 return-only callsite 收集同时返回 callsite 列表和失败原因。
- `lib/passes/NativePrototypeRecovery.cpp:408` 调整 `collectReturnOnlyDirectCallsites(...)`，普通无法处理的 use 仍返回 `function has uses`，返回 load 阻断或类型不匹配返回 `unsafe callsite return load`。
- `lib/passes/NativePrototypeRecovery.cpp:909` 调整 `rewriteNativeRecoveredPrototypeReturnOnly(...)`，把收集 helper 的失败原因写入 `NativePrototypeRewriteResult::Reason`。
- `lib/passes/NativePrototypeRecovery.cpp:1109` 调整 `rewriteNativeRecoveredPrototypeInputReturn(...)`，input 收集成功但返回 load unsafe 时也返回 `unsafe callsite return load`。
- `tests/native_prototype_recovery_test.cpp:1509` 增加 clobber return load 的 reason 断言。
- `tests/native_prototype_recovery_test.cpp:1522` 扩展 unsafe CFG 负例共用断言，确认 reason 为 `unsafe callsite return load`。
- `tests/native_prototype_recovery_test.cpp:1897` 在 batch rewrite 测试里加入 unsafe return callsite，覆盖 module 级 `SkippedByReason["unsafe callsite return load"]`。

验证：

```sh
git diff --check
cmake -S . -B build \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
```

结果：通过，`1/1 Test #4: notdec.native_prototype_recovery.input_candidates ... Passed`。

性能：只在已有 callsite 遍历里携带一个失败字符串，不增加 CFG 搜索范围。目标测试总耗时约 `0.04 sec`，无可见下降。

评分：

- 实现效果：6/10。skip reason 更可用，方便后续 Bench2 定位，但不提升可重写形状。
- 复杂度：3/10。只增加一个局部结果结构。
- 维护成本：3/10。字符串成为测试约束，后续改名需要同步。
