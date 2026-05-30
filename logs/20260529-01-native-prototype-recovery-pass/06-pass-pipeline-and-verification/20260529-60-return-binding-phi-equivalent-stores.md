# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

return binding 已经支持两种简单等价：

- 多条 return store 写同一个常量或同一个有名 value。
- 多条 return store 写同一个 LLVM `Value*`。

还剩一个很常见的 SSA 形态：return store 写的是 PHI，而另一条 path 直接 store PHI 的某个 incoming 值。如果 PHI 的所有 incoming 都等价于这个值，那么语义上仍是同一个返回值。

这一步只处理这个保守子集，不做一般表达式等价。

# Ghidra 实现参考

Ghidra 的 heritage 会把多路径数据流合流成 SSA varnode：

- `Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritagePass(...)`：插入 MULTIEQUAL，形成 SSA 合流。
  - `Heritage::placeMultiequals(...)`：在支配边界处放置 MULTIEQUAL。
- `Ghidra/Features/Decompiler/src/decompile/cpp/funcdata_varnode.cc`
  - `Funcdata::newVarnodeOut(...)`：为 PcodeOp 输出建立 varnode。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveOutputMap(...)`：根据 output varnode 的 SSA 身份推导返回值。

LLVM IR 中 PHI 对应 Ghidra 的 MULTIEQUAL。native 侧现在没有完整 varnode 对象，但可以把 `PHINode` 的 incoming value 作为最小 SSA 合流信息。

# native 侧复刻策略

- 扩展 `sameReturnStoreValue(...)`：
  - 先保留指针相同判断。
  - 再保留已有 `returnValueKey(...)` fallback。
  - 如果一边是 PHI，且 PHI 的所有 incoming value 都和另一边等价，则接受。
- 为避免递归环，给 helper 加一个小的深度上限。
- 增加测试：
  - 一个函数中，一条 return path store 常量到 RAX。
  - 另一条 return path store 一个 PHI 到 RAX，PHI 的 incoming 都是同一个常量。
  - return binding 和 return-only rewrite 应通过。
  - 冲突 store 负例继续拒绝。

# 判断标准

- 新测试通过。
- 原有 `conflicting return stores` 仍拒绝。
- 全量 CTest 通过。
- Bench2 smoke 通过，观察 `missing return binding` 和整体耗时。

# 风险

这一步不接受 PHI incoming 不一致的情况，也不跨 PHI 反推控制流。它只把“PHI 实际合流同一个值”作为同值 return store 处理。

# 实现记录

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:125` 扩展 `sameReturnStoreValue(...)`。
  - 保留指针相同和 `returnValueKey(...)` 相同两条旧路径。
  - 一边是 `PHINode` 时，要求 PHI 的每个 incoming 都能和另一边递归匹配。
  - 递归深度上限为 4，避免 PHI 环导致无限递归。
- `lib/passes/NativePrototypeRecovery.cpp:1141` 的 `getNativePrototypeReturnBindings(...)` 继续复用该 helper，因此 return binding 自动支持 PHI 同值判断。
- `tests/native_prototype_recovery_test.cpp:924` 新增 `createTwoReturnPhiEquivalentStoreFunction(...)`。
  - 一条 return path 直接 store 常量到 RAX。
  - 另一条 return path store 一个 PHI 到 RAX，PHI 的两个 incoming 都是同一个常量。
- `tests/native_prototype_recovery_test.cpp:2156` 校验 PHI 等价多 return store 能得到一个 binding，且 `ReturnStores.size() == 2`。
- `tests/native_prototype_recovery_test.cpp:2201` 校验 PHI 等价函数能完成 return-only rewrite，结果类型为 `i64()`。

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
- `OUT_DIR=/tmp/notdec-bin2llvm-bench2-return-phi-equivalent-smoke scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-return-phi-equivalent-smoke`
  - 通过。

## Bench2 smoke 指标

| target | elapsed_seconds | prototype_functions | prototype_input_candidates | prototype_return_candidates | signature_rewrite_seen | signature_rewrite_rewritten | signature_rewrite_skipped |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| vsftpd | 85 | 187 | 163 | 59 | 236 | 120 | 116 |
| libuv | 222 | 485 | 321 | 165 | 571 | 244 | 327 |
| memcached | 118 | 259 | 224 | 99 | 315 | 157 | 158 |

skip reason：

- vsftpd：declaration 49，missing recovered prototype 51，missing return binding 13，unsafe callsite input value 1，unsafe callsite return load 2。
- libuv：declaration 86，missing recovered prototype 198，missing return binding 42，unsafe callsite input value 1。
- memcached：declaration 56，missing recovered prototype 77，missing return binding 22，unsafe callsite return load 3。

## 性能和复杂度

- 性能：Bench2 smoke 三个目标通过，rewrite/skipped 数与上一轮同口径结果一致；只在多 return store 校验里多看 PHI incoming，没有可见性能下降。
- 实现效果：7/10。补上了最简单的 PHI/MULTIEQUAL 同值形态。
- 复杂度：3/10。递归 helper 有深度上限，逻辑仍集中。
- 维护成本：3/10。后续如果要支持不同 incoming 但最终等价的表达式，需要更完整 SSA value 分析。

## 后续不做

这一步不处理 PHI incoming 不一致、不同表达式等价、控制流路径条件等情况。遇到这些形态仍保持 `missing return binding`。
