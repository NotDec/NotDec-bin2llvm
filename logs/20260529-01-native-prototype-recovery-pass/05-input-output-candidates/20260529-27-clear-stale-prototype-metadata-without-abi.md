# 原始 prompt

```text
继续实现 logs/20260529-01-native-prototype-recovery-pass/GOAL.md；基于已有进度，选择下一小步先写计划，再实现、验证、更新 PROGRESS.md，并提交。
```

# 背景

`NativePrototypeRecovery` 的候选和 recovered prototype metadata 都依赖 module 级 `!notdec.abi`。当前如果 module 没有 ABI，pass 直接返回，不会清理已有的 `notdec.prototype.input_candidates`、`notdec.prototype.return_candidates`、`notdec.prototype.recovered`。

这会让二次处理或手写 IR 输入带着过期 prototype metadata 输出，后续工具可能误读。

# Ghidra 实现参考

Ghidra 的 prototype recovery 是按当前 compiler spec / prototype model 重新推导的：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ProtoModel::hasEffect(...)` 和 ParamList 查询都依赖当前 compiler spec。
  - `FuncProto::updateAllTypes(...)` 用当前模型更新函数原型。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)` / `ActionFuncLink::apply(...)` 在当前函数上下文中推进 prototype 信息。

native 侧如果没有 ABI，就不能证明旧 prototype metadata 仍然有效。保守做法是清掉本 pass 负责的 prototype recovery metadata。

# native 侧复刻策略

- 新增一个清理 helper，清掉：
  - `notdec.prototype.input_candidates`
  - `notdec.prototype.return_candidates`
  - `notdec.prototype.recovered`
- `runNativePrototypeRecovery(...)` 发现 module 没有 ABI 时，对所有定义函数执行清理后返回空 summary。
- 增加一个无 ABI module 测试：函数先带三类旧 prototype metadata，跑 pass 后应全部清掉。

暂时不做：

- 不让无 ABI module 直接执行显式签名重写。
- 不清理外部 declaration 上的 metadata。

# 判断标准

- 无 ABI 时不会保留旧 prototype recovery metadata。
- 有 ABI 的正常 recovery / rewrite 测试不回退。
- 全量测试继续通过。

# 风险

- 如果某些输入 IR 想保留手工 recovered metadata 但没有 `!notdec.abi`，现在会被清掉。当前 native prototype recovery 的来源是 ABI + register metadata，所以这属于正确覆盖。

# 实现记录

改动：

- `lib/passes/NativePrototypeRecovery.cpp:107` 新增 `clearPrototypeRecoveryMetadata(...)`，统一清理 input candidates、return candidates 和 recovered prototype metadata。
- `lib/passes/NativePrototypeRecovery.cpp:756` 在缺少 ABI 时遍历所有定义函数，清理本 pass 的 prototype metadata 后返回。
- `tests/native_prototype_recovery_test.cpp:2767` 新增无 ABI module 样例，先手工写入三类 stale prototype metadata。
- `tests/native_prototype_recovery_test.cpp:2779` 验证无 ABI 运行后三类 metadata 都被清掉。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本步骤完成。

验证：

```sh
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
git diff --check
ctest --test-dir build --output-on-failure
```

结果：通过，目标测试 `1/1 Test #4: notdec.native_prototype_recovery.input_candidates ... Passed`，耗时约 `0.04 sec`；全量测试 `9/9` 通过，总耗时约 `0.91 sec`。

性能：只有无 ABI 的早退路径会遍历定义函数清理 metadata；正常有 ABI 路径不增加额外扫描。

评分：

- 实现效果：6/10。避免 no-ABI 输入输出 stale prototype metadata。
- 复杂度：2/10。只加一个清理 helper 和测试。
- 维护成本：2/10。后续新增 prototype recovery metadata 时需要同步 helper。
