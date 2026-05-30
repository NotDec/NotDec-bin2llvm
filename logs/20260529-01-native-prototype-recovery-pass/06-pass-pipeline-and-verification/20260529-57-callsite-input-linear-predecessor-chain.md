# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

签名重写 direct callsite 参数时，native 侧现在只从 call 所在 block 或“一层唯一前驱”里找调用前的 ABI input register store。真实 lowering 里常见形态是 store 在前面的线性 block，call 在后面的 block，中间还有一两个无分支跳转 block。

返回值 load 查找已经支持线性唯一后继链。callsite input 查找也应该补上同样保守的线性唯一前驱链，减少不必要的 `unsafe callsite input value`。

# Ghidra 实现参考

Ghidra 不按相邻 basic block 文本形态找参数，而是在 heritage SSA 和 call spec 上工作：

- `Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritagePass(...)`：先把寄存器 varnode 建成 SSA。
  - `Heritage::guardCalls(...)`：按 call effect 在 SSA 图上处理调用边界。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`：按 callee prototype 和当前函数 SSA 状态收集调用输入。
  - `FuncCallSpecs::buildInputFromTrials(...)`：把 active input trials 映射到调用参数。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：在 pipeline 中持续把 prototype 结果反馈到调用点。

native 侧还没有完整 SSA use-def 查询。当前小步先把 CFG 线性链补齐，只接受唯一前驱链，遇到分叉、合流、环就停止。

# native 侧复刻策略

- 扩展 `callsiteInputValueBeforeCall(...)`。
- 先保留同 block 反向查找。
- 如果同 block 找不到，沿唯一前驱链逐块反向查找 register store。
- 每一步要求当前 block 只有一个前驱，并且前驱只有当前 block 这一个后继，避免跨分支错误拿值。
- 遇到环、无前驱、多前驱、多后继、类型不匹配时保持现有 `unsafe callsite input value`。
- 增加一个 IR 小样例：`store RDI -> br middle -> br call_block -> call`，签名重写应能把 store value 作为 typed call 参数。

# 判断标准

- 新测试能证明跨线性唯一前驱链的 input-only callsite 被重写。
- 原有多前驱 / 缺失参数值负例仍保持保守。
- 全量 CTest 通过。
- Bench2 smoke 通过；重点观察 `unsafe callsite input value` 是否减少，不能因扩大查找导致 verify 失败。

# 风险

这一步仍不是完整 SSA 数据流。它只处理线性 CFG 形态，不处理分支汇合里的 PHI 或不同路径不同参数值。这样保守一些，但不会把不确定路径误改成确定参数。

# 实现记录

## 代码改动

- `lib/passes/NativePrototypeRecovery.cpp:253`：扩展 `callsiteInputValueBeforeCall(...)`。同 block 反向查找保持不变；同 block 找不到时，沿唯一前驱链逐块反向查找 register store。
- `lib/passes/NativePrototypeRecovery.cpp:262`：新增 visited 集合，遇到环时停止，避免 CFG 异常时无限回溯。
- `lib/passes/NativePrototypeRecovery.cpp:276`：要求前驱也只有当前 block 这一个后继，避免跨分支拿到不确定参数值。
- `tests/native_prototype_recovery_test.cpp:537`：新增 `createInputStoreLinearPredecessorCallerFunction(...)`，构造 `store -> br -> br -> call` 的线性前驱链。
- `tests/native_prototype_recovery_test.cpp:1847`：新增 input-only callsite 测试，验证线性前驱链里的 RDI store 会成为重写后 typed call 的参数。

## 验证

- `cmake --build build --target native_prototype_recovery_test -j2`：通过。
- `git diff --check`：通过。
- `ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V`：通过。
- `ctest --test-dir build --output-on-failure`：9/9 通过。
- `cmake --build build --target notdec-native-llvm -j2`：通过。
- `OUT_DIR=/tmp/notdec-bin2llvm-bench2-input-linear-predecessor-smoke scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-input-linear-predecessor-smoke`：通过。

## Bench2 结果

| target | elapsed_seconds | prototype_functions | input_candidates | return_candidates | signature_rewrite_seen | signature_rewrite_rewritten | signature_rewrite_skipped |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| vsftpd | 85 | 187 | 163 | 59 | 236 | 120 | 116 |
| libuv | 218 | 485 | 321 | 165 | 571 | 244 | 327 |
| memcached | 117 | 259 | 224 | 99 | 315 | 157 | 158 |

当前三目标 rewrite/skipped 数和上一轮一致。`unsafe callsite input value` 仍是 vsftpd 1、libuv 1，说明这两个真实 skip 不是简单线性唯一前驱链形态。

## 性能影响

每个 direct callsite 参数查找最多多走一段唯一前驱链，并用 visited 集合防环。Bench2 smoke 总耗时约 420 秒，和前两轮 419/421 秒基本一致；主要耗时仍在 native lowering 和 LLVM verify。

## 评分

- 实现效果：7/10。补齐了 callsite input 和 return load 在简单线性 CFG 上的对称能力。
- 复杂度：7/10。只扩展已有 helper，没有新公开 API。
- 维护成本：7/10。逻辑仍保守；后续若要覆盖分支合流，需要接 PHI/SSA 语义，而不是继续扩 CFG 特判。
