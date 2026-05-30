# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

上一小步让 Bench2 显式签名重写输出进入 assemble/verify。为避免真实目标里出现悬空 `ret`，当前对“返回值仍是 register load”的函数先保守跳过。

这个保护是必要的，但也说明批量签名重写的顺序还不够像真实 decompiler pipeline：如果 caller 的返回值来自 callee 调用后的返回寄存器 load，先重写 callee 可以把这个 load 改成 typed call result，caller 后续就不需要跳过。

# Ghidra 实现参考

Ghidra 不是简单按地址顺序一次性改函数签名，而是在 action pipeline 中不断让函数 prototype、调用点和 SSA 状态互相更新：

- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPool::apply(...)`：按 action 列表反复运行，直到当前阶段稳定。
  - `ActionPrototypeTypes::apply(...)`：更新当前函数 prototype，并让调用点信息参与后续分析。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::updateAllTypes(...)`：把 input/output trial 写回函数 prototype。
  - `FuncCallSpecs::deriveInputMap(...)` / `deriveOutputMap(...)`：按 callee prototype 解释调用点的输入和输出。
- `Ghidra/Features/Decompiler/src/decompile/cpp/funcdata.cc`
  - `Funcdata::syncVarnodesWithSymbols(...)`：把恢复后的符号和 varnode 状态同步回当前函数。

native 侧还没有完整 action pool。这一步先复刻一个小的顺序策略：module 级显式签名重写时，先处理本模块 direct callee，再处理 caller。

# native 侧复刻策略

- 只改 `rewriteNativeRecoveredPrototypes(...)` 的函数处理顺序。
- 扫描 module 内 direct call，构造“caller 依赖 callee”的关系。
- 用 DFS 后序把 callee 排在 caller 前。
- 遇到递归、环、间接调用或外部声明时保持保守：不强行解环，仍由现有 per-function 安全检查决定是否 rewrite。
- 增加一个小测试：module 顺序刻意放 caller 在 callee 前。caller 的返回值来自 callee 调用后的 RAX load。批量重写应先重写 callee，再重写 caller，最终两个函数都变成 typed return。

# 判断标准

- 单测能证明 module 顺序为 caller-before-callee 时，module rewrite 仍能改写 direct callee 和 caller。
- 现有 full CTest 通过。
- Bench2 smoke 仍通过；重点观察签名重写 rewritten/skipped 数是否有改善或至少不退化。

# 风险

DFS 排序只看本模块 direct call。它不能替代 Ghidra 的完整 action pool，也不处理真实递归 SCC 内的最优顺序。这个限制可以接受，因为现有 per-function guard 仍会阻止不安全 rewrite。

# 实现记录

## 代码改动

- `lib/passes/NativePrototypeRecovery.cpp:1122`：新增 `appendCalleeFirstFunction(...)`，扫描函数内 direct call，把同 module callee 递归加入顺序列表。
- `lib/passes/NativePrototypeRecovery.cpp:1150`：新增 `calleeFirstFunctions(...)`，为 module 级签名重写生成 callee-first 顺序。递归和环不做特殊强解，仍交给现有安全检查。
- `lib/passes/NativePrototypeRecovery.cpp:1797`：`rewriteNativeRecoveredPrototypes(...)` 改用 callee-first 顺序，不再直接按 module 插入顺序遍历。
- `tests/native_prototype_recovery_test.cpp:3323`：新增 caller-before-callee 的批量重写测试。测试里先创建 caller，再创建 callee，caller 的返回值来自 callee 调用后的 `RAX` load；module rewrite 后两个函数都应改成 `i64()`。

## 验证

- `cmake --build build --target native_prototype_recovery_test -j2`：通过。
- `git diff --check`：通过。
- `ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V`：通过。
- `ctest --test-dir build --output-on-failure`：9/9 通过。
- `cmake --build build --target notdec-native-llvm -j2`：通过。
- `OUT_DIR=/tmp/notdec-bin2llvm-bench2-callee-first-signature-rewrite-smoke scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-callee-first-signature-rewrite-smoke`：通过。

## Bench2 结果

| target | elapsed_seconds | prototype_functions | input_candidates | return_candidates | signature_rewrite_seen | signature_rewrite_rewritten | signature_rewrite_skipped |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| vsftpd | 84 | 187 | 163 | 59 | 236 | 120 | 116 |
| libuv | 219 | 485 | 321 | 165 | 571 | 244 | 327 |
| memcached | 118 | 259 | 224 | 99 | 315 | 157 | 158 |

这组三目标的 rewrite/skipped 数和上一轮相同，没有退化。新增测试覆盖的是真实样本当前没有触发的小型 caller-before-callee 形态。

## 性能影响

module rewrite 前多一次 direct call DFS，复杂度约为函数数加 direct call 指令数。Bench2 smoke 总耗时约 421 秒，和上一轮 419 秒接近，主要时间仍在 native lowering 和 LLVM verify。

## 评分

- 实现效果：7/10。module 级显式签名重写不再受简单 caller-before-callee 插入顺序影响，向 Ghidra 的 callee/callsite 迭代方向靠近了一步。
- 复杂度：7/10。只加局部 DFS，不引入新的公开数据结构。
- 维护成本：7/10。递归 SCC 仍靠原有安全检查保守处理，后续如果要继续减少 skip，需要再做真正的两阶段或 SCC 内固定点。
