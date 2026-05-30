# 原始 prompt

Another language model started to solve this problem and produced a summary of its thinking process. You also have access to the state of the tools that were used by that language model. Use this to build on the work that has already been done and avoid duplicating work. Here is the summary produced by the other language model, use the information in this summary to assist with your own analysis:
We are in `/sn640/NotDec/external/NotDec-bin2llvm`, branch `main`. Reply in concise Chinese. Active goal persists: keep implementing small steps for `logs/20260529-01-native-prototype-recovery-pass/GOAL.md`, each step must first create a plan markdown in the right phase dir, then implement narrowly, verify, update the same plan with implementation record, update `PROGRESS.md`, remove `scripts/__pycache__`, and commit. Do not call `update_goal` unless the full GOAL is proven complete. It is not complete.

# 背景

Bench2 当前还能看到 `missing recovered prototype`、`unsafe callsite input value`、`unsafe callsite return load` 等签名重写跳过原因，但 summary 只有聚合计数。下一步继续修策略前，需要先知道具体哪些函数被跳过。

Ghidra 侧相关参考：

- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`：`ActionPrototypeTypes::apply(...)` 负责在 decompiler action 中推进 prototype 推断。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`：`FuncProto::updateAllTypes(...)` 更新函数 prototype；`FuncCallSpecs::deriveInputMap(...)` / `deriveOutputMap(...)` 从调用点推导输入输出。

# 目标

在 native prototype recovery summary 中输出每个函数的签名重写结果和原因。只做诊断信息，不改变 recovered prototype、callsite rewrite 或函数签名重写策略。

# 路线

1. 给 module 级签名重写 summary 增加 per-function 记录，保存原函数名、是否重写、原因。
2. `rewriteNativeRecoveredPrototypes(...)` 每处理一个函数就写入该记录。
3. `runNativePrototypeRecovery(...)` 把记录带到总 summary。
4. `printNativePrototypeRecoverySummary(...)` 打印 `signature rewrite function ...` 行，方便 Bench2 stderr 直接 `rg`。
5. 在已有 opt-in 测试里检查 rewritten、already matches、missing recovered prototype 三类记录。

# 风险

- 输出会变长，但只在 `PrintSummary` 时出现。Bench2 诊断更清楚，IR 语义不变。
- 函数被重写后 LLVM Function 对象会替换，所以 per-function 记录用处理前的原函数名，避免名字丢失。

# 判断标准

- 单元测试通过。
- Bench2 smoke 的 rewritten / skipped 聚合指标不应因为本步变化而改变。
- stderr 能看到具体函数级签名重写原因。

# 实现记录

## 改动

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:100` 新增 `NativePrototypeModuleRewriteFunctionSummary`，记录函数名、是否重写、原因。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:110` 在 `NativePrototypeModuleRewriteSummary` 增加 `Functions`。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:127` 在 `NativePrototypeRecoverySummary` 增加 `SignatureRewriteFunctions`。
- `lib/passes/NativePrototypeRecovery.cpp:1072` 的 `runNativePrototypeRecovery(...)` 把 module rewrite 的 per-function 记录带到总 summary。
- `lib/passes/NativePrototypeRecovery.cpp:1967` 的 `rewriteNativeRecoveredPrototypes(...)` 每处理一个函数就记录原函数名、`rewritten` 或跳过原因。
- `lib/passes/NativePrototypeRecovery.cpp:1994` 的 `printNativePrototypeRecoverySummary(...)` 输出 `signature rewrite function <name>: rewritten=<0|1> reason=<reason>`。
- `tests/native_prototype_recovery_test.cpp:1511` 增加测试 helper `findRewriteFunctionSummary(...)`。
- `tests/native_prototype_recovery_test.cpp:3994` 的 opt-in rewrite 测试检查 rewritten、already matches、missing recovered prototype 三类 per-function 记录。

## 验证

- `cmake --build build --target native_prototype_recovery_test -j2`：通过。
- `ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V`：通过。
- `ctest --test-dir build --output-on-failure`：9/9 通过。
- `cmake --build build --target notdec-native-llvm -j2`：通过。
- `OUT_DIR=/tmp/notdec-bin2llvm-bench2-signature-rewrite-function-reasons-smoke scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-signature-rewrite-function-reasons-smoke`：通过。

Bench2 metrics：

| target | elapsed_seconds | signature_rewrite_seen | rewritten | skipped |
| --- | ---: | ---: | ---: | ---: |
| vsftpd | 85 | 236 | 130 | 106 |
| libuv | 217 | 571 | 283 | 288 |
| memcached | 117 | 315 | 178 | 137 |

聚合 rewrite 数与上一轮基线一致。stderr 已能列出具体函数级原因，例如：

- vsftpd：`notdec_native_91c0` 为 `unsafe callsite input value`，`notdec_native_131f0` 为 `unsafe callsite return load`。
- libuv：`notdec_native_9e70`、`uv_is_active`、`notdec_native_17770` 为 `unsafe callsite input value`。
- memcached：`notdec_native_bc60`、`notdec_native_f3f0`、`notdec_native_1d760` 为 `unsafe callsite return load`。

## 性能

本步只增加 summary 记录和打印，不改变 IR 重写策略。Bench2 同口径耗时为 85s / 217s / 117s，和上一轮 86s / 220s / 118s 同级，没有看到性能下降。

## 评分

- 实现效果：9/10。能直接从 stderr 定位剩余跳过函数。
- 复杂度：2/10。只增加一个简单记录结构和打印。
- 维护成本：2/10。字段含义直接，和现有 aggregate summary 同路径维护。

更好的方案暂时不做：把 per-function 记录合并进已有 recovery function summary。那会遇到 declaration 和重写后 Function 替换的匹配问题；当前独立记录更直接。
