# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

阶段 6 已经有 `.ll` / `.bc` fixture 的显式签名重写 smoke，也有 Bench2 三个真实目标的 prototype recovery summary 和 assemble/verify 验证。

但 Bench2 smoke 目前还没有在真实目标上打开 `--rewrite-prototype-signatures`。这意味着真实项目里如果签名重写输出了不能 assemble/verify 的 IR，或者 summary 统计格式退化，现有 Bench2 smoke 不一定能发现。

# Ghidra 实现参考

Ghidra 的 decompiler pipeline 会在真实函数上持续把 prototype 结果喂给调用点和函数签名相关逻辑：

- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：在 action pipeline 中恢复和更新 prototype。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::updateAllTypes(...)`：把 input/output trial 更新到函数 prototype。
  - `FuncCallSpecs::deriveInputMap(...)` / `deriveOutputMap(...)`：按 prototype model 整理调用点 input/output。
- `Ghidra/Features/Decompiler/src/decompile/cpp/funcdata.cc`
  - `Funcdata::syncVarnodesWithSymbols(...)`：让恢复后的符号和 varnode 状态继续影响后续输出。

native 侧现在的 `rewriteNativeRecoveredPrototypes(...)` 是这条链路的显式 opt-in 版本。Bench2 smoke 应该至少证明它在真实目标上能产出可被 LLVM 22 接受的 IR。

# native 侧复刻策略

- 只改 `scripts/bench2-native-smoke.sh`。
- 每个 Bench2 目标在现有默认 pass 验证后，额外跑一次：
  - `notdec-native-llvm --all-confirmed --prototype-recovery-summary --rewrite-prototype-signatures`
- 对 rewrite 输出做：
  - `llvm-as`
  - `opt -passes=verify`
  - 检查 prototype recovery summary 里有签名重写统计。
- metrics 里追加三列：
  - `signature_rewrite_seen`
  - `signature_rewrite_rewritten`
  - `signature_rewrite_skipped`

暂时不做：

- 不要求真实目标 rewrites 非零。当前 recovery 和安全检查可能让真实样本全部 skip，这不应该阻断 smoke。
- 不固定每个目标的 exact rewrite 数字。
- 不检查每个 skip reason 的分布。

# 判断标准

- Bench2 smoke 能在三目标上跑显式签名重写输出，并用 LLVM 22 assemble/verify 通过。
- `metrics.tsv` 表头和数据行列数一致。
- 全量 CTest 通过。

# 风险

这会增加 Bench2 smoke 时间，因为每个真实目标多跑一次 native lowering 和 LLVM verify。收益是显式签名重写进入真实样本验证，不再只靠小 fixture。

# 实现记录

## 代码改动

- `scripts/bench2-native-smoke.sh:431`：新增 `require_prototype_metric(...)`，用于检查可为 0 但必须存在的签名重写统计。
- `scripts/bench2-native-smoke.sh:513`：`metrics.tsv` 增加 `signature_rewrite_seen`、`signature_rewrite_rewritten`、`signature_rewrite_skipped` 三列。
- `scripts/bench2-native-smoke.sh:544`：为每个目标增加签名重写 `.ll/.bc/.opt.bc` 和 stdout/stderr 输出文件。
- `scripts/bench2-native-smoke.sh:596`：每个 Bench2 目标额外执行 `notdec-native-llvm --all-confirmed --prototype-recovery-summary --rewrite-prototype-signatures`，再用 LLVM 22 `llvm-as` 和 `opt -passes=verify` 验证。
- `scripts/bench2-native-smoke.sh:605`：从签名重写 stderr 解析 seen/rewritten/skipped 三个统计，并在 `scripts/bench2-native-smoke.sh:706` 写入 metrics。
- `lib/passes/NativePrototypeRecovery.cpp:209`：新增 `isRegisterAccessLoad(...)` 和 `hasUnsafeReturnValueLoad(...)`。如果函数返回值仍是 `notdec.register.access` load，先跳过签名重写，原因是 `unsafe return value load`。这个值在批量重写里可能同时被后续调用点重写删除，保守跳过比生成悬空 `ret` 更安全。
- `lib/passes/NativePrototypeRecovery.cpp:1164`、`1382`、`1506`、`1635`：return-only、input-return、multi-return、input-multi-return 四种形状都接入上面的保护。
- `lib/passes/NativePrototypeRecovery.cpp:1426`、`1673`：当 return value 正好是 external input load 时，替换 input load 前先把返回值改成新函数参数，避免 `ret` 引用被删除的 load。
- `tests/native_prototype_recovery_test.cpp:724`：新增 `createReturnRegisterLoadFunction(...)`，构造返回值来自 register load 的负例。
- `tests/native_prototype_recovery_test.cpp:772`：新增 `createInputForwardReturnFunction(...)`，覆盖 input load 直接转发为返回值的正例。
- `tests/native_prototype_recovery_test.cpp:1427`：更新主 summary 计数，纳入新增 input-forward-return 用例。
- `tests/native_prototype_recovery_test.cpp:2282`：验证 register-load return 会被 `unsafe return value load` 跳过，模块仍可验证。
- `tests/native_prototype_recovery_test.cpp:2387`：验证 input-forward-return 改写后直接返回新参数，不保留旧 external input load。

## 验证

- `cmake --build build --target native_prototype_recovery_test -j2`：通过。
- `ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V`：通过。
- `git diff --check`：通过。
- `ctest --test-dir build --output-on-failure`：9/9 通过。
- `cmake --build build --target notdec-native-llvm -j2`：通过。
- `build/bin/notdec-native-llvm /tmp/notdec-bin2llvm-bench2-signature-rewrite-verify-smoke/vsftpd.all-confirmed.ll --no-instcombine-pass --no-register-ssa-pass --prototype-recovery-summary --rewrite-prototype-signatures -o /tmp/notdec-bin2llvm-bench2-signature-rewrite-verify-smoke/vsftpd.rewrite-from-ll.ll`：通过。
- `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as .../vsftpd.rewrite-from-ll.ll` + `/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify .../vsftpd.rewrite-from-ll.bc`：通过。
- `OUT_DIR=/tmp/notdec-bin2llvm-bench2-signature-rewrite-verify-smoke scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-signature-rewrite-verify-smoke`：通过。

## Bench2 结果

| target | elapsed_seconds | prototype_functions | input_candidates | return_candidates | signature_rewrite_seen | signature_rewrite_rewritten | signature_rewrite_skipped |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| vsftpd | 85 | 187 | 163 | 59 | 236 | 120 | 116 |
| libuv | 218 | 485 | 321 | 165 | 571 | 244 | 327 |
| memcached | 116 | 259 | 224 | 99 | 315 | 157 | 158 |

## 性能影响

Bench2 smoke 每个目标多跑一次 native lowering、一次 LLVM 22 assemble、一次 LLVM 22 verify。当前三目标总耗时约 419 秒。这个开销只在 smoke 脚本里出现，不影响默认 pass pipeline。

## 评分

- 实现效果：8/10。真实 Bench2 目标已经覆盖显式签名重写输出的 assemble/verify，并发现和修掉一类批量重写悬空返回值风险。
- 复杂度：7/10。脚本只是增加一条验证路径；pass 新增的保护偏保守，理解成本低。
- 维护成本：7/10。短期会增加 smoke 时间；长期更好的方案是给批量签名重写做依赖顺序或两阶段改写，但这一步先保证真实样本输出合法。
