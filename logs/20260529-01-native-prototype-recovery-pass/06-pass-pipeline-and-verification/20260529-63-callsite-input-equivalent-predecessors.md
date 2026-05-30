# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

Bench2 当前还剩少量 `unsafe callsite input value`。现有 `callsiteInputValueBeforeCall(...)` 只接受同 block 或线性唯一前驱链里的参数寄存器 store。遇到 call block 有多个前驱时直接失败。

但如果所有前驱给同一个参数寄存器写入的是等价值，这和 Ghidra MULTIEQUAL/PHI 的保守同值场景一致，可以安全作为 call 参数。

# Ghidra 实现参考

- `Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritagePass(...)`：建立 SSA。
  - `Heritage::placeMultiequals(...)`：在多前驱合流处插入 MULTIEQUAL。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::buildInputFromTrials(...)`：从 callsite 前的 input varnode/trial 构建调用参数。
  - `ParamActive::registerTrial(...)`：维护参数 trial。

native 侧暂不合成 LLVM PHI，只接受“所有前驱值已经等价”这一种安全形态。

# native 侧复刻策略

- 扩展 `callsiteInputValueBeforeCall(...)`：
  - 先保留同 block 查找。
  - 线性唯一前驱链行为不变。
  - 如果当前 block 有多个前驱，分别在每个前驱末尾反向找目标寄存器 store。
  - 所有前驱都找到同类型值，且这些值通过 `sameReturnStoreValue(...)` 等价时，返回其中一个值。
- 增加测试：
  - 两个前驱都 store 同一个常量到 RDI 后汇合到 call block。
  - input-only callee 的 callsite rewrite 应成功。
  - 已有 unsafe input 负例仍保守失败。

# 判断标准

- 新多前驱等价 input callsite 测试通过。
- 原有 unsafe input skip 测试仍通过。
- 全量 CTest 通过。
- Bench2 smoke 通过，观察 `unsafe callsite input value` 是否下降。

# 风险

这一步不处理不同 incoming 值，也不合成 PHI。只在每条前驱都能证明是等价值时接受，避免把路径相关的参数改成一个固定值。

# 实现记录

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:258` 新增 `equivalentInputValueFromPredecessors(...)`。
  - 遍历 call block 的所有前驱。
  - 每个前驱都必须能在 block 末尾向前找到目标寄存器 store。
  - 所有值都必须类型匹配，并通过 `sameReturnStoreValue(...)` 等价。
- `lib/passes/NativePrototypeRecovery.cpp:284` 扩展 `callsiteInputValueBeforeCall(...)`。
  - 同 block 查找不变。
  - 线性唯一前驱链不变。
  - 遇到多前驱时改为调用 `equivalentInputValueFromPredecessors(...)`，只接受全等价场景。
- `tests/native_prototype_recovery_test.cpp:568` 新增 `createInputStoreEquivalentPredecessorCallerFunction(...)`。
  - 两个前驱都 store 同一个 RDI 常量，再汇合到 call block。
- `tests/native_prototype_recovery_test.cpp:607` 新增 `createInputStoreConflictingPredecessorCallerFunction(...)`。
  - 两个前驱分别 store 不同 RDI 常量，用于负例。
- `tests/native_prototype_recovery_test.cpp:2112` 增加多前驱等价 input callsite rewrite 正例。
- `tests/native_prototype_recovery_test.cpp:2173` 增加多前驱冲突 input callsite rewrite 负例，确认仍返回 `unsafe callsite input value`。

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
- `OUT_DIR=/tmp/notdec-bin2llvm-bench2-callsite-input-equivalent-predecessors-smoke scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-callsite-input-equivalent-predecessors-smoke`
  - 通过。

## Bench2 smoke 指标

| target | elapsed_seconds | prototype_functions | prototype_input_candidates | prototype_return_candidates | signature_rewrite_seen | signature_rewrite_rewritten | signature_rewrite_skipped |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| vsftpd | 84 | 187 | 163 | 56 | 236 | 130 | 106 |
| libuv | 218 | 485 | 321 | 157 | 571 | 283 | 288 |
| memcached | 118 | 259 | 224 | 94 | 315 | 178 | 137 |

和上一轮同口径相比，rewrite/skipped 数没有变化。说明当前真实样本剩余的 `unsafe callsite input value` 不是“多前驱同值 store”这个简单形态。

skip reason：

- vsftpd：declaration 49，missing recovered prototype 52，unsafe callsite input value 2，unsafe callsite return load 3。
- libuv：declaration 86，missing recovered prototype 199，unsafe callsite input value 3。
- memcached：declaration 56，missing recovered prototype 78，unsafe callsite return load 3。

## 性能和复杂度

- 性能：Bench2 smoke 三个目标通过，耗时 84s / 218s / 118s，和上一轮同口径接近；新增逻辑只在 callsite input 找不到线性前驱值时检查多前驱。
- 实现效果：6/10。补齐了一个和 Ghidra MULTIEQUAL 对应的安全形态，但真实样本未变化。
- 复杂度：3/10。没有合成 PHI，没有改 metadata，只复用现有 value 等价判断。
- 维护成本：3/10。后续若支持真正 PHI 参数值，可以替换这个 helper。

## 后续不做

这一步不处理不同前驱值、不完整前驱值、路径相关参数，也不合成新 LLVM PHI。遇到这些场景仍保持 `unsafe callsite input value`。
