# 原始 prompt

```text
继续实现 logs/20260529-01-native-prototype-recovery-pass/GOAL.md；基于已有进度，选择下一小步先写计划，再实现、验证、更新 PROGRESS.md，并提交。
```

# 背景

`NativeRegisterSSA` 已经能在 direct call 处读取 callee 的 `notdec.register.preserves` / `notdec.register.clobbers`。但这些 metadata 也是同一个 pass 生成的。如果 caller 在 module 顺序里排在 callee 前，caller 会先按 ABI fallback 判断，漏掉 callee 后续生成的 clobber metadata。

这一步先补非递归 direct call 的分析顺序：尽量先分析 callee，再分析 caller。完整 SCC/fixpoint 后续再做。

# Ghidra 实现参考

Ghidra 的 call effect 不只靠函数出现顺序：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::hasEffect(...)`：优先查询具体函数原型上的 effect，再回到 prototype model。
  - `ProtoModel::hasEffect(...)`：提供 ABI 默认 effect。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionFuncLink::apply(...)`：把 callsite 和 callee prototype 联系起来。
- `Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::guardCalls(...)`：在 call 处按 callee/call specs 插入或省略 guard。

native 侧现在还没有全局 decompile action fixpoint。先用直接调用图的 callee-first 顺序复刻一小部分，让普通非递归调用能看到 callee effect metadata。

# native 侧复刻策略

- 在 `runNativeRegisterSSA(...)` 中先收集本模块定义函数。
- 对 direct call 做 DFS，callee 先入队，caller 后入队。
- 递归或环路先保守跳过重复访问，不做 SCC 迭代。
- 增加测试：caller 在 module 中排在 callee 前，callee 会 clobber ABI unaffected 的 `RBX`。正确顺序下 caller 的 call 后 `RBX` load 不能被传播。

暂时不做：

- 不做多轮 fixpoint。
- 不处理 indirect call。
- 不拆 `NativeCallEffectResolver`。

# 判断标准

- caller-before-callee 的 direct call 能看到 callee 后续生成的 `notdec.register.clobbers`。
- 外部 call 的 ABI fallback 不变。
- 现有 register SSA / prototype recovery 测试继续通过。

# 风险

- DFS 顺序不能解决递归和互递归。遇到环时仍可能 fallback 到 ABI。这个风险明确留给后续 SCC/fixpoint。

# 实现记录

改动：

- `lib/passes/NativeRegisterSSA.cpp:690` 新增 `visitDirectCalleesFirst(...)`，沿本模块 direct call DFS，把 callee 放在 caller 前。
- `lib/passes/NativeRegisterSSA.cpp:718` 新增 `directCalleeFirstOrder(...)`，作为当前小型 direct call effect 顺序替代方案；递归环路只防重复，不做迭代求稳。
- `lib/passes/NativeRegisterSSA.cpp:744` 让 `runNativeRegisterSSA(...)` 按 callee-first 顺序运行 `FunctionPromoter`。
- `tests/native_register_effects_test.cpp:174` 新增 caller-before-callee 样例：caller 在 module 里先出现，callee 后出现且 clobber `RBX`。
- `tests/native_register_effects_test.cpp:305` 验证 caller 的 call 后 `RBX` load 没有被错误传播。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本步骤完成。

验证：

```sh
cmake --build build --target native_register_effects_test -j2
ctest --test-dir build -R 'notdec.native_register_effects.preserved' -V
git diff --check
ctest --test-dir build --output-on-failure
```

结果：通过，目标测试 `1/1 Test #5: notdec.native_register_effects.preserved ... Passed`，耗时约 `0.03 sec`；全量测试 `9/9` 通过，总耗时约 `0.95 sec`。

性能：新增一次 direct call DFS 排序，复杂度约为函数数加 direct call 数。函数体提升逻辑不变。

评分：

- 实现效果：7/10。非递归 direct call 能先生成 callee register effect，再服务 caller。
- 复杂度：4/10。只加 DFS 顺序，没有引入全局 fixpoint。
- 维护成本：4/10。后续做 SCC/fixpoint 时可以替换这个顺序函数。
