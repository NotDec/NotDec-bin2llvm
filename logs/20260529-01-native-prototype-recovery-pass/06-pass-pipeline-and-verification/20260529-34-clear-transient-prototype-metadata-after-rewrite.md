# 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

显式签名重写后，函数参数已经变成 LLVM argument，返回值也变成 LLVM `ret`。但新函数会从旧函数复制 metadata，导致 `notdec.register.external_inputs`、`notdec.prototype.input_candidates`、`notdec.prototype.return_candidates` 这些“重写前的候选状态”继续留在 rewritten 函数上。

`notdec.prototype.recovered` 仍然有用，它记录最终恢复出的原型；但候选 metadata 和 external input metadata 是中间状态，重写后继续保留会让后续 pass 或二次运行误以为函数还处在 register 形式。

# Ghidra 实现参考

Ghidra 的 prototype recovery 会把 trial 变成最终 `FuncProto`，trial 本身不会作为最终函数语义继续存在：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamActive::sortTrials()`：整理 trial。
  - `FuncCallSpecs::deriveInputMap(...)` / `deriveOutputMap(...)`：把 trial 变成最终 storage map。
  - `FuncProto::updateAllTypes(...)`：更新函数原型。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionParamDouble::apply(...)`：处理 trial 去重、冲突和最终保留状态。

native 侧的 `input_candidates` / `return_candidates` 对应 Ghidra 的 trial 阶段；`recovered` 对应已经汇总出的最终原型。这一步只清理 trial 阶段 metadata，不删除 recovered 原型记录。

# native 侧复刻策略

签名重写成功后，对 rewritten 函数清理：

- `notdec.register.external_inputs`
- `notdec.prototype.input_candidates`
- `notdec.prototype.return_candidates`

保留：

- `notdec.prototype.recovered`
- ABI、register global、其它非 prototype recovery metadata

先不做：

- 清理 caller 侧 register store metadata。
- 删除 `notdec.register.access` 的普通寄存器访问信息。

# 判断标准

- input-only rewrite 后，rewritten callee 不再有 `notdec.register.external_inputs` 和 `notdec.prototype.input_candidates`。
- return-only rewrite 后，rewritten callee 不再有 `notdec.prototype.return_candidates`。
- rewritten callee 仍保留 `notdec.prototype.recovered`，所以 eligibility 仍能判断 `already matches`。
- 现有重写和 callsite 测试继续通过。

# 风险

- 如果某个后续工具把候选 metadata 当审计信息读取，这一步会减少 rewritten 函数上的中间状态。但最终原型仍在 `notdec.prototype.recovered`，更适合作为 rewritten 函数的稳定记录。

# 实现记录

改动：

- `lib/passes/NativePrototypeRecovery.cpp:1030` 新增 `clearTransientPrototypeRecoveryMetadata(...)`，清理 rewritten 函数上的 `notdec.register.external_inputs`、`notdec.prototype.input_candidates`、`notdec.prototype.return_candidates`。
- `lib/passes/NativePrototypeRecovery.cpp:1103` 在 return-only rewrite 复制 metadata 后清理临时 metadata。
- `lib/passes/NativePrototypeRecovery.cpp:1203` 在 input-only rewrite 复制 metadata 后清理临时 metadata。
- `lib/passes/NativePrototypeRecovery.cpp:1324` 在 input + single return rewrite 复制 metadata 后清理临时 metadata。
- `lib/passes/NativePrototypeRecovery.cpp:1434` 在 multi-return rewrite 复制 metadata 后清理临时 metadata。
- `lib/passes/NativePrototypeRecovery.cpp:1560` 在 input + multi-return rewrite 复制 metadata 后清理临时 metadata。
- `tests/native_prototype_recovery_test.cpp:1467` 验证 input-only rewrite 后清掉 `external_inputs` 和 `input_candidates`，但保留 `recovered`。
- `tests/native_prototype_recovery_test.cpp:1851` 验证 return-only rewrite 后清掉 `return_candidates`，但保留 `recovered`。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本步骤完成。

验证：

```sh
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
git diff --check
ctest --test-dir build --output-on-failure
```

结果：通过，目标测试 `1/1 Test #4: notdec.native_prototype_recovery.input_candidates ... Passed`；全量测试 `8/8` 通过。

性能：只在显式签名重写成功路径清理 3 个函数 metadata，不影响默认 metadata recovery。目标测试耗时约 `0.04 sec`，全量测试约 `0.80 sec`。

评分：

- 实现效果：7/10。rewritten 函数不再保留 trial 阶段 metadata，最终 recovered 原型仍可读。
- 复杂度：4/10。集中 helper，调用点简单。
- 维护成本：5/10。后续如果增加新的临时 metadata，只需要补到这个 helper。
