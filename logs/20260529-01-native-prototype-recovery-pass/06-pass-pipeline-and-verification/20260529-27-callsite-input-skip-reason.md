# 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

上一小步把 unsafe callsite return load 的 skip reason 从 `function has uses` 细化出来。input 参数侧还有同样问题：直接调用点存在，但 call 前找不到可用的参数寄存器 store 时，也统一报 `function has uses`。

这会让 Bench2 统计看不出是“callee 有无法处理的 user”，还是“callsite 参数值接不上”。这一步先补诊断，不扩大参数查找范围。

# Ghidra 实现参考

Ghidra 的调用点参数恢复保留了独立状态：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncCallSpecs` 持有每个 CALL 的 `ParamActive activeinput` 和 `ParamActive activeoutput`。
  - `FuncCallSpecs::initActiveInput()` / `getActiveInput()`：为单个调用点启动输入参数 trial。
  - `FuncCallSpecs::checkInputTrialUse(...)` / `buildInputFromTrials(...)`：检查 trial 是否真的被调用点消费，再把参数接到 CALL。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionFuncLink::funcLinkInput(...)`：对 locked prototype，把参数 storage 注册成 active trial，并插入到 CALL input。
  - `ActionInputPrototype::apply(...)`：对函数入口 input varnode 生成 `ParamTrial`，用 `hasNoDescend()` 判断是否 active，再 `deriveInputMap(...)`。

这里的关键不是简单“有 use 就失败”，而是区分 CALL 这个 user 是否支持、参数 trial 是否 active、storage 是否能接到值。native 侧还没有完整 `FuncCallSpecs`，但 skip reason 应该先把这些边界分开。

# native 侧复刻策略

这一步只细化 input callsite 的失败原因：

- direct call user 形状不支持时，仍返回 `function has uses`。
- direct call 形状支持但找不到 call 前参数值，返回 `unsafe callsite input value`。
- input-only 和 input-return 两条 rewrite 都复用同一个原因。
- module 级 `SkippedByReason` 增加对应覆盖。

不做的事：

- 不支持多参数。
- 不扩展多前驱、多分支、PHI 参数值追踪。
- 不改变现有 rewrite 成功条件。

# 判断标准

- 缺少参数 store 的 input-only direct callsite 不再报 `function has uses`。
- input-return 中参数接不上时也报同一个 reason。
- batch rewrite 统计能看到 `unsafe callsite input value`。
- `native_prototype_recovery_test` 通过。

# 风险

- 这一步是诊断改进，不提高真实可重写覆盖率。
- reason 字符串成为测试约束，后续改名要同步测试和日志。

# 实现记录

改动：

- `lib/passes/NativePrototypeRecovery.cpp:257` 新增 `InputOnlyCallsiteCollectionResult`，让 input callsite 收集返回 rewrite 列表和失败原因。
- `lib/passes/NativePrototypeRecovery.cpp:262` 调整 `collectInputOnlyDirectCallsiteRewrites(...)`：非 direct void call user 返回 `function has uses`，direct call 但找不到参数值返回 `unsafe callsite input value`。
- `lib/passes/NativePrototypeRecovery.cpp:1008` 调整 `rewriteNativeRecoveredPrototypeInputOnly(...)`，把 input callsite 收集的失败原因写入 rewrite result。
- `lib/passes/NativePrototypeRecovery.cpp:1109` 调整 `rewriteNativeRecoveredPrototypeInputReturn(...)`，同样复用新的 input callsite 失败原因。
- `tests/native_prototype_recovery_test.cpp:1067` 把 direct call 但缺少 RDI 参数 store 的旧断言改成 `unsafe callsite input value`。
- `tests/native_prototype_recovery_test.cpp:1220` 新增独立缺参数 store 的 input-only callsite 样例。
- `tests/native_prototype_recovery_test.cpp:1252` 新增 address-taken input-only 样例，确认真正非 call user 仍报 `function has uses`。
- `tests/native_prototype_recovery_test.cpp:1968` 在 batch rewrite 测试中加入 unsafe input callsite，覆盖 `SkippedByReason["unsafe callsite input value"]`。

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

性能：只在已有 users 遍历里多携带一个失败字符串，不增加 CFG 搜索范围。目标测试总耗时约 `0.04 sec`，无可见下降。

评分：

- 实现效果：6/10。input callsite 的失败边界更清楚，后续 Bench2 统计更可用。
- 复杂度：3/10。新增局部结果结构，没有改公共 API。
- 维护成本：3/10。reason 字符串需要和 CLI / 测试保持一致。
