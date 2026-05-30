# 原始 prompt

```text
继续实现 logs/20260529-01-native-prototype-recovery-pass/GOAL.md；基于已有进度，选择下一小步先写计划，再实现、验证、更新 PROGRESS.md，并提交。
```

# 背景

`NativeRegisterSSA` 现在会生成 `notdec.register.preserves` 和 `notdec.register.clobbers`。这些 metadata 是 direct call effect 和 prototype recovery 的基础。但 summary 里只显示 loads/stores/calls，看不到本轮到底识别了多少 preserved/clobbered register。

这一步补 summary 计数，方便 CLI `--register-ssa-summary` 和测试观察 register effect。

# Ghidra 实现参考

Ghidra 的 decompiler action 会保留并报告 prototype/effect 推导状态，call effect 本身由当前 function prototype / prototype model 查询：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::hasEffect(...)`：查询函数当前 effect。
  - `ProtoModel::hasEffect(...)`：查询默认 ABI effect。
- `Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::guardCalls(...)`：根据 effect 决定 call guard。

native 侧已经把 effect 写成函数 metadata。这一步不改语义，只把本轮推导出的数量放进 summary，让后续定位 call effect 问题更直接。

# native 侧复刻策略

- 在 `NativeRegisterSSAFunctionSummary` 增加：
  - `PreservedRegisters`
  - `ClobberedRegisters`
- 在 `NativeRegisterSSASummary` 增加总计字段。
- `attachRegisterEffectMetadata()` 写 metadata 的同时同步本函数计数。
- `printNativeRegisterSSASummary(...)` 输出总计和每函数计数。
- 测试里保留 `runNativeRegisterSSA(...)` 返回值，检查至少覆盖当前样例的 preserved/clobbered 数。

暂时不做：

- 不把 register 名列表放进 summary，只计数。
- 不改 JSON summary。

# 判断标准

- summary 能统计 preserved/clobbered register 数。
- 现有 register effect metadata 行为不变。
- 全量测试继续通过。

# 风险

- summary 计数来自本轮写 metadata 的 vector，和 metadata 一致。风险主要是未来新增 effect metadata 时忘记同步字段。

# 实现记录

改动：

- `include/notdec-bin2llvm/passes/NativeRegisterSSA.h:28` 在 per-function summary 增加 `PreservedRegisters` / `ClobberedRegisters`。
- `include/notdec-bin2llvm/passes/NativeRegisterSSA.h:41` 在 module summary 增加 preserved/clobbered 总计。
- `lib/passes/NativeRegisterSSA.cpp:639` 在写 preserves/clobbers metadata 时同步本函数计数。
- `lib/passes/NativeRegisterSSA.cpp:700` 汇总 module 级 preserved/clobbered 计数。
- `lib/passes/NativeRegisterSSA.cpp:786` 在 `printNativeRegisterSSASummary(...)` 输出总计和每函数计数。
- `tests/native_register_effects_test.cpp:323` 验证当前样例 summary 中 preserved 为 1、clobbered 为 3。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本步骤完成。

验证：

```sh
cmake --build build --target native_register_effects_test -j2
ctest --test-dir build -R 'notdec.native_register_effects.preserved' -V
git diff --check
ctest --test-dir build --output-on-failure
```

结果：通过，目标测试 `1/1 Test #5: notdec.native_register_effects.preserved ... Passed`，耗时约 `0.03 sec`；全量测试 `9/9` 通过，总耗时约 `0.89 sec`。

性能：只增加两个 summary 计数和输出字段，不改变分析复杂度。

评分：

- 实现效果：5/10。补齐 register effect 可观测性，方便后续 call effect 调试。
- 复杂度：2/10。只扩 summary 字段。
- 维护成本：2/10。字段跟 metadata 写入点绑定，后续新增 effect 类型时再扩展。
