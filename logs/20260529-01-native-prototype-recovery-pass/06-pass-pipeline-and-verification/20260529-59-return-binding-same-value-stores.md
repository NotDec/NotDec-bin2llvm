# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

上一小步让 return binding 接受多条同寄存器 return store，但要求每条 store value 的 `returnValueKey(...)` 存在且一致。这个条件能覆盖常量和有名字的值，但漏掉一种很常见的 SSA 形态：多个 return path store 的其实是同一个 LLVM `Value*`，只是这个值没有名字。

这类情况在语义上比字符串 key 更直接。native 侧可以先接受“同一个 Value 指针”这个保守条件，不需要做表达式等价。

# Ghidra 实现参考

Ghidra 在 output prototype recovery 时看的是 SSA varnode 身份和数据流，而不是文本名字：

- `Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritagePass(...)`：建立 SSA varnode，合流处用 MULTIEQUAL。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveOutputMap(...)`：根据 SSA 上的 output varnode 整理返回值。
  - `FuncProto::updateAllTypes(...)`：把 output trial 写入函数 prototype。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：在 action pipeline 里持续更新 prototype 和调用点。

native 侧当前没有完整 varnode SSA 对象，但 LLVM `Value*` 本身可作为局部 SSA 身份。这个小步先复刻这一点。

# native 侧复刻策略

- 新增一个小 helper，判断两条 return store value 是否等价：
  - 如果两个 `Value*` 是同一个指针，接受。
  - 否则退回已有 `returnValueKey(...)`，要求 key 都存在且相同。
- `getNativePrototypeReturnBindings(...)` 的多 store 校验改用这个 helper。
- 增加一个测试函数：
  - entry 中创建一个 unnamed `add` 值；
  - 两条 return path 都 store 这个同一个 add 值到 RAX；
  - return binding 和 return-only rewrite 应通过。

# 判断标准

- 新测试通过，证明 unnamed same-value 多 store 能 rewrite。
- 原有冲突 return store 测试仍拒绝。
- 全量 CTest 通过。
- Bench2 smoke 通过，观察真实样本 rewrite/skipped 是否变化。

# 风险

这一步只接受同一个 `Value*` 或已有 key 相同，不做表达式等价，也不合成 PHI。它不会把不同 path 上相似但不确定的值误合并。

# 实现记录

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:125` 新增 `sameReturnStoreValue(...)`。
  - 先按 LLVM `Value*` 指针身份判断同值。
  - 指针不同才退回 `returnValueKey(...)` 的常量 / 有名 value key。
- `lib/passes/NativePrototypeRecovery.cpp:1135` 在 `getNativePrototypeReturnBindings(...)` 的多 return store 校验里改用 `sameReturnStoreValue(...)`。
- `tests/native_prototype_recovery_test.cpp:887` 新增 `createTwoReturnSameValueStoreFunction(...)`。
  - entry 里用本地 `alloca` / `load` / unnamed `add` 生成共享的 LLVM `Value*`。
  - 两条 return path 都 store 这个同一个值到 RAX。
- `tests/native_prototype_recovery_test.cpp:2091` 校验 same-value 多 store 能得到一个 return binding，且 `ReturnStores.size() == 2`。
- `tests/native_prototype_recovery_test.cpp:2121` 校验 return-only rewrite 后函数类型为 `i64()`，并继续 `verifyModule(...)`。

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
- `OUT_DIR=/tmp/notdec-bin2llvm-bench2-return-same-value-stores-smoke scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-return-same-value-stores-smoke`
  - 通过。

## Bench2 smoke 指标

| target | elapsed_seconds | prototype_functions | prototype_input_candidates | prototype_return_candidates | signature_rewrite_seen | signature_rewrite_rewritten | signature_rewrite_skipped |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| vsftpd | 85 | 187 | 163 | 59 | 236 | 120 | 116 |
| libuv | 220 | 485 | 321 | 165 | 571 | 244 | 327 |
| memcached | 118 | 259 | 224 | 99 | 315 | 157 | 158 |

skip reason：

- vsftpd：declaration 49，missing recovered prototype 51，missing return binding 13，unsafe callsite input value 1，unsafe callsite return load 2。
- libuv：declaration 86，missing recovered prototype 198，missing return binding 42，unsafe callsite input value 1。
- memcached：declaration 56，missing recovered prototype 77，missing return binding 22，unsafe callsite return load 3。

## 性能和复杂度

- 性能：Bench2 smoke 三个目标通过，签名重写统计和上一轮同口径结果一致；本步只在多 return store 校验里增加一次指针比较和必要时 key 比较，没有可见性能下降。
- 实现效果：8/10。补上同一个 unnamed LLVM `Value*` 的真实 SSA 身份判断。
- 复杂度：2/10。只新增一个 helper，没有引入新状态。
- 维护成本：2/10。逻辑集中在 return binding 校验，后续如果要支持 PHI 或表达式等价，可以继续扩展 helper。

## 后续不做

这一步不把不同 basic block 里形似相同的表达式合并，也不为多路径返回合成 PHI。那需要更完整的数据流判断，不能靠简单字符串或指针比较处理。
