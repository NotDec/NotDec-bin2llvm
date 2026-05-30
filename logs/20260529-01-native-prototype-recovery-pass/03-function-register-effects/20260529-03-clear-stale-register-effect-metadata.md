# 原始 prompt

```text
继续实现 logs/20260529-01-native-prototype-recovery-pass/GOAL.md；基于已有进度，选择下一小步先写计划，再实现、验证、更新 PROGRESS.md，并提交。
```

# 背景

`NativeRegisterSSA` 会写函数级 `notdec.register.external_inputs`、`notdec.register.preserves`、`notdec.register.clobbers`。这些 metadata 后续会被 prototype recovery 和 direct call effect 读取。

当前问题是：如果函数原来带旧 metadata，而本次分析结果为空，代码会直接 return，不清掉旧值。二次运行 pass、或读入已有 IR 再跑 pass 时，后续分析可能读到过期的 register effect。

# Ghidra 实现参考

Ghidra 的 heritage/prototype 信息按当前函数状态重新计算，不应保留上一轮失效的 effect：

- `Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritage(...)`：按当前 varnode def-use 重建 SSA 信息。
  - `Heritage::guardCalls(...)`：按当前 call effect 决定 guard。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::hasEffect(...)`：查询当前 prototype/effect list。

native 侧对应做法是：本轮没有 external input / preserve / clobber 结果时，明确删除旧 metadata，而不是让旧 metadata 留在函数上。

# native 侧复刻策略

- `attachExternalInputMetadata()`：如果本轮没有 external input，清掉 `notdec.register.external_inputs`。
- `attachRegisterEffectMetadata()`：每次都写入或清掉 `notdec.register.preserves` / `notdec.register.clobbers`。
- 增加测试：构造一个无寄存器读写的函数，先手工挂旧 metadata，跑 pass 后确认三种 metadata 都被清掉。

暂时不做：

- 不清理 instruction 级旧 metadata。
- 不改变 register SSA 数据流规则。

# 判断标准

- 空分析结果不会保留旧 `external_inputs` / `preserves` / `clobbers`。
- 现有 preserved/clobbered/direct call effect 测试不回退。
- 全量测试继续通过。

# 风险

- 如果某些输入 IR 依赖手工预置 metadata 而不希望 pass 覆盖，会被清掉。但这些 metadata 是本 pass 的输出，重跑时应以当前结果为准。

# 实现记录

改动：

- `lib/passes/NativeRegisterSSA.cpp:552` 修改 `attachExternalInputMetadata()`，本轮没有 external input 时清掉 `notdec.register.external_inputs`。
- `lib/passes/NativeRegisterSSA.cpp:597` 修改 `attachRegisterEffectMetadata()`，本轮没有 register effect 输入时清掉 `notdec.register.preserves` / `notdec.register.clobbers`。
- `lib/passes/NativeRegisterSSA.cpp:626` 对 preserves/clobbers 都调用 `setMetadata(...)`，空结果写 `nullptr`，避免旧 metadata 残留。
- `tests/native_register_effects_test.cpp:212` 新增 `createStaleMetadataFunction(...)`，构造一个无寄存器访问但预置三类旧 metadata 的函数。
- `tests/native_register_effects_test.cpp:327` 验证 pass 后旧 `external_inputs` / `preserves` / `clobbers` 都被清掉。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本步骤完成。

验证：

```sh
cmake --build build --target native_register_effects_test -j2
ctest --test-dir build -R 'notdec.native_register_effects.preserved' -V
git diff --check
ctest --test-dir build --output-on-failure
```

结果：通过，目标测试 `1/1 Test #5: notdec.native_register_effects.preserved ... Passed`，耗时约 `0.03 sec`；全量测试 `9/9` 通过，总耗时约 `0.91 sec`。

性能：只多了几次函数级 metadata 清理，不改变数据流扫描复杂度。

评分：

- 实现效果：6/10。保证 register effect metadata 是本轮分析结果，不读旧值。
- 复杂度：2/10。只补清理逻辑和测试。
- 维护成本：2/10。后续新增 register effect metadata 时按同样规则清理即可。
