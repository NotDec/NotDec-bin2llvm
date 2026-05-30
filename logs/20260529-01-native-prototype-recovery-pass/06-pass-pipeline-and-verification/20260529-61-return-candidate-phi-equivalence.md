# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

上一小步让签名重写阶段的 return binding 支持 PHI incoming 全等价。但 return candidate 阶段还只用 `ValueKey` 字符串检查多 return path 的返回值一致性。

这会造成一个不一致：某些 PHI 等价返回值能被 binding 接受，但在 candidate 阶段可能已经被过滤。prototype recovery 应该先给出同样保守的候选。

# Ghidra 实现参考

Ghidra prototype recovery 的输出值来自 SSA varnode，而不是字符串 key：

- `Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritagePass(...)`：完成 varnode SSA。
  - `Heritage::placeMultiequals(...)`：用 MULTIEQUAL 表示合流。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveOutputMap(...)`：基于 output varnode 和 trial map 推导输出。
  - `FuncProto::updateAllTypes(...)`：将输出 trial 写回 prototype。

native 侧的 `PHINode` 是 MULTIEQUAL 的 LLVM 表示。candidate 阶段也应复用同一套保守 SSA value 等价判断。

# native 侧复刻策略

- `NativeParamTrial` 增加一个内部用的 `llvm::Value *Value` 字段。
  - metadata 仍只写寄存器名和 slot，不改变外部格式。
  - `ValueKey` 暂时保留，避免扩大改动。
- `returnTrialsBeforeInstruction(...)` 同时记录 store value 指针。
- return candidate 的多 return path 一致性：
  - 如果同 slot 的多条 trial 都有 value，使用 `sameReturnStoreValue(...)` 判断。
  - 仍要求所有 return path 覆盖同一个 slot。
  - 不做一般表达式等价。
- 增加一个 candidate 专用测试：
  - 两条 return path 都覆盖 RAX。
  - 一条 path store 常量，另一条 path store PHI，PHI incoming 都等价。
  - 确认 `notdec.prototype.return_candidates` 含 RAX。

# 判断标准

- PHI 等价 return candidate 被标出。
- 原有 conflicting return store 仍不标 RAX return candidate。
- 全量 CTest 通过。
- Bench2 smoke 通过，关注 return candidate 和 rewrite 指标是否变化。

# 风险

这一步只把已经用于 binding 的保守等价判断前移到 candidate 阶段。PHI incoming 不一致、复杂表达式等价、路径条件仍不处理。

# 实现记录

## 改动

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:37` 给 `NativeParamTrial` 增加运行期字段 `Value`。
  - 只在本轮 recovery 内保存 LLVM SSA value 指针。
  - 不写入 metadata，不改变 `notdec.prototype.*` 外部格式。
- `lib/passes/NativePrototypeRecovery.cpp:699` 在 `returnTrialsBeforeInstruction(...)` 中记录 return store value。
  - `ValueKey` 仍保留，用于原有简单身份信息。
- `lib/passes/NativePrototypeRecovery.cpp:712` 新增 `hasConflictingReturnTrialValue(...)`。
  - 同 slot 的 return trial 用 `sameReturnStoreValue(...)` 判断是否冲突。
  - 因此 candidate 阶段也能识别 PHI incoming 全等价。
- `lib/passes/NativePrototypeRecovery.cpp:922` 调整 return candidate 合并逻辑。
  - 先收集所有 return trial。
  - 仍要求同 slot 覆盖所有 return path。
  - 再用 SSA value 等价判断过滤冲突。
- `tests/native_prototype_recovery_test.cpp:962` 将 PHI 测试值命名为 `merged_value`，确保旧 `ValueKey` 字符串逻辑会把它和常量看成不同。
- `tests/native_prototype_recovery_test.cpp:1601` 新增直接断言：PHI 等价函数的 `notdec.prototype.return_candidates` 包含 RAX。

## 验证

- `git diff --check`
  - 通过。
- `cmake --build build --target native_prototype_recovery_test -j2`
  - 通过。
- `ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V`
  - 通过，1/1。
- `ctest --test-dir build --output-on-failure`
  - 通过，9/9。
- `cmake --build build --target notdec-native-llvm -j2`
  - 通过。
- `OUT_DIR=/tmp/notdec-bin2llvm-bench2-return-candidate-phi-equivalence-smoke scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-return-candidate-phi-equivalence-smoke`
  - 通过。

## Bench2 smoke 指标

| target | elapsed_seconds | prototype_functions | prototype_input_candidates | prototype_return_candidates | signature_rewrite_seen | signature_rewrite_rewritten | signature_rewrite_skipped |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| vsftpd | 85 | 187 | 163 | 56 | 236 | 122 | 114 |
| libuv | 218 | 485 | 321 | 157 | 571 | 251 | 320 |
| memcached | 117 | 259 | 224 | 94 | 315 | 161 | 154 |

和上一轮同口径相比：

- return candidates：vsftpd 59 -> 56，libuv 165 -> 157，memcached 99 -> 94。
- rewritten：vsftpd 120 -> 122，libuv 244 -> 251，memcached 157 -> 161。
- missing return binding：vsftpd 13 -> 10，libuv 42 -> 34，memcached 22 -> 17。

这个结果符合预期：旧 `ValueKey` 逻辑会放过一部分 unknown value 的多 return path，后续 binding 再失败；现在 candidate 阶段用同一套 SSA value 判断，先过滤掉这些不一致候选，同时保留 PHI incoming 全等价的形态。

skip reason：

- vsftpd：declaration 49，missing recovered prototype 52，missing return binding 10，unsafe callsite input value 1，unsafe callsite return load 2。
- libuv：declaration 86，missing recovered prototype 199，missing return binding 34，unsafe callsite input value 1。
- memcached：declaration 56，missing recovered prototype 78，missing return binding 17，unsafe callsite return load 3。

## 性能和复杂度

- 性能：Bench2 smoke 三个目标通过，耗时 85s / 218s / 117s，和上一轮同口径接近；新增判断只在 return candidate 合并阶段遍历同函数内 return trial。
- 实现效果：8/10。candidate 和 binding 现在共用 PHI 等价语义，真实样本的 `missing return binding` 下降。
- 复杂度：4/10。`NativeParamTrial` 多了一个运行期字段，但 metadata 格式不变。
- 维护成本：4/10。后续如果把 trial 拆成更完整的 SSA varnode 模型，这个 `Value` 字段可以自然迁移。

## 后续不做

这一步不处理复杂表达式等价，不根据路径条件证明值相等，也不改变 recovered prototype metadata 格式。
