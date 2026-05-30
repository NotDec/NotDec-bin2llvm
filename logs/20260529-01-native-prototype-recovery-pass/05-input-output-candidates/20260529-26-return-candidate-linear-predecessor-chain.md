# 原始 prompt

```text
继续实现 logs/20260529-01-native-prototype-recovery-pass/GOAL.md；基于已有进度，选择下一小步先写计划，再实现、验证、更新 PROGRESS.md，并提交。
```

# 背景

当前 return candidate 会先看 `ret` 前的 ABI output register store；如果 return block 本身没有 store，只看一层唯一前驱。真实 lifted CFG 常有 `store -> br -> br -> ret` 这种空跳转链，只看一层会漏掉返回值。

这一步补唯一前驱链回看。只处理线性链，不处理多前驱合流。

# Ghidra 实现参考

Ghidra 返回值恢复看的是 return op 输入位置上的 varnode 和它的数据流来源，不要求写返回寄存器的 P-Code op 必须紧贴 return：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::deriveOutputMap(...)`：根据函数出口附近的数据流和 `ParamActive` 推出输出 storage。
  - `FuncCallSpecs::deriveOutputMap(...)`：调用点侧按返回 storage 建 output map。
- `Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - heritage SSA 会让跨 block 的 varnode 定义仍能被追踪，不依赖源码块相邻。

native 侧没有完整 P-Code varnode SSA，这里先用 LLVM CFG 复刻一个保守子集：如果 return block 没有候选 store，就沿唯一前驱链向前找，直到找到候选、遇到多前驱或检测到环。

# native 侧复刻策略

- 把 `returnTrialsBefore(...)` 从“一层唯一前驱”改成“沿唯一前驱链查找”。
- 每一层仍复用 `returnTrialsBeforeInstruction(...)`，保持 ABI output register 和 slot 去重规则不变。
- 用 visited set 防止异常 CFG 成环。
- 增加一个测试函数：`store RAX -> br mid -> br exit -> ret`，验证 `RAX` 被标成 return candidate。

暂时不做：

- 不处理多前驱合流的 PHI/一致性证明。
- 不跨越含多后继语义的复杂控制流做推断。
- 不改变 callsite return load 的逻辑。

# 判断标准

- 线性唯一前驱链上的 ABI output register store 能成为 return candidate。
- 现有 partial/multi-predecessor 冲突负例不放宽。
- 全量测试继续通过。

# 风险

- 向前走太远可能误认旧 store。这里限制在唯一前驱链，遇到多前驱或环就停，仍是保守策略。

# 实现记录

改动：

- `lib/passes/NativePrototypeRecovery.cpp:645` 修改 `returnTrialsBefore(...)`，在当前 return block 找不到 ABI output store 时，沿唯一前驱链继续调用 `returnTrialsBeforeInstruction(...)`，并用 visited set 防环。
- `tests/native_prototype_recovery_test.cpp:865` 新增 `createLinearPredecessorReturnStoreFunction(...)`，构造 `store RAX -> middle -> exit -> ret` 的测试 CFG。
- `tests/native_prototype_recovery_test.cpp:1255` 把线性前驱链返回函数加入 prototype recovery 测试。
- `tests/native_prototype_recovery_test.cpp:1331` 同步 summary 计数。
- `tests/native_prototype_recovery_test.cpp:1386` 验证线性前驱链上的 `RAX` 被标成 `notdec.prototype.return_candidates`。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本步骤完成。

验证：

```sh
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
git diff --check
ctest --test-dir build --output-on-failure
```

结果：通过，目标测试 `1/1 Test #4: notdec.native_prototype_recovery.input_candidates ... Passed`，耗时约 `0.04 sec`；全量测试 `9/9` 通过，总耗时约 `0.90 sec`。

性能：只在 return block 没有候选 store 时沿唯一前驱链回看。每条 return 路径有 visited set 防环，复杂度随线性链长度增长；常见紧邻 return 的路径不受影响。

评分：

- 实现效果：7/10。补上常见空跳转链上的返回寄存器候选。
- 复杂度：3/10。复用现有扫描逻辑，只扩大 CFG 查找范围。
- 维护成本：3/10。后续如果做 PHI/多前驱合流，可以在同一入口继续扩展。
