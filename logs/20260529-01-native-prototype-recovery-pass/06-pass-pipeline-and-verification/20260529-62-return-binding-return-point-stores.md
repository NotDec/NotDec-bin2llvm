# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

当前 `getNativePrototypeReturnBindings(...)` 用 `registerAccessStores(...)` 收集函数内某个返回寄存器的全部 store。这个做法过宽：如果函数中间临时写过 RAX，最后 return 前又写入真正返回值，binding 会把临时 store 也纳入一致性检查，可能误报 `missing return binding`。

Ghidra 的 output recovery 看的是 return 点上的 output varnode，不是函数内所有同寄存器写。

# Ghidra 实现参考

- `Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritagePass(...)`：为寄存器 varnode 建 SSA，使每个 use 能对应到定义。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveOutputMap(...)`：从函数出口相关的 output varnode 推导返回值。
  - `FuncProto::updateAllTypes(...)`：把 output trial 写回函数 prototype。

native 侧现在还没有完整 varnode use-def 图，但已有 `returnTrialsBefore(...)` 会按 return 点回看 ABI output store。这一步让 binding 复用同样的 return 点口径。

# native 侧复刻策略

- 新增一个 helper：按每个 `ReturnInst` 调用 `returnTrialsBefore(...)`，只收集目标 register 对应的 store。
- 如果同一个 store 被多个 return 点看到，只保留一次。
- 仍要求所有 return 点都能找到该返回寄存器的 store。
- 多 store 的 value 一致性仍用 `sameReturnStoreValue(...)`。
- 增加测试：
  - 函数先临时 store 一个不同常量到 RAX。
  - return 前再 store 真正返回值到 RAX。
  - candidate 和 binding 应使用 return 点前 store，rewrite 后删除 return store，临时 store 不应阻断。

# 判断标准

- 新测试通过。
- 原有 conflicting return store 仍拒绝。
- 全量 CTest 通过。
- Bench2 smoke 通过，重点看 `missing return binding` 是否下降，不能引入 verify 失败。

# 风险

这一步只改变 binding 的 store 收集口径，不扩大 return candidate。遇到多前驱不确定、缺少 return 点 store、value 不一致时仍保守失败。

# 实现记录

## 改动

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:40` 给 `NativeParamTrial` 增加 `Store`。
  - 这个字段只在本轮 recovery 内保存 output register store 指针。
  - metadata 格式不变。
- `lib/passes/NativePrototypeRecovery.cpp:748` 新增 `returnPointStores(...)`。
  - 遍历每个 `ReturnInst`。
  - 复用 `returnTrialsBefore(...)` 找 return 点前的目标寄存器 store。
  - 同一 store 被多个 return 点看到时只记录一次。
  - 某个 return 点找不到目标寄存器 store 时保守失败。
- `lib/passes/NativePrototypeRecovery.cpp:1155` 调整 `getNativePrototypeReturnBindings(...)`。
  - 先读取 module ABI，构造 `NativePrototypeModel`。
  - 对每个 recovered return 只绑定 `returnPointStores(...)` 返回的 store。
  - 删除旧的全函数 `registerAccessStores(...)` 扫描，避免中间临时 store 误参与 return binding。
- `tests/native_prototype_recovery_test.cpp:755` 新增 `createTemporaryReturnStoreFunction(...)`。
  - 函数先临时写 RAX，再在 return 前写真正返回值。
- `tests/native_prototype_recovery_test.cpp:2167` 校验 temporary store 不会进入 return binding。
- `tests/native_prototype_recovery_test.cpp:2269` 校验该函数能完成 return-only rewrite。

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
- `OUT_DIR=/tmp/notdec-bin2llvm-bench2-return-point-stores-smoke scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-return-point-stores-smoke`
  - 通过。

## Bench2 smoke 指标

| target | elapsed_seconds | prototype_functions | prototype_input_candidates | prototype_return_candidates | signature_rewrite_seen | signature_rewrite_rewritten | signature_rewrite_skipped |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| vsftpd | 84 | 187 | 163 | 56 | 236 | 130 | 106 |
| libuv | 219 | 485 | 321 | 157 | 571 | 283 | 288 |
| memcached | 118 | 259 | 224 | 94 | 315 | 178 | 137 |

和上一轮同口径相比：

- rewritten：vsftpd 122 -> 130，libuv 251 -> 283，memcached 161 -> 178。
- skipped：vsftpd 114 -> 106，libuv 320 -> 288，memcached 154 -> 137。
- missing return binding：vsftpd 10 -> 0，libuv 34 -> 0，memcached 17 -> 0。

skip reason：

- vsftpd：declaration 49，missing recovered prototype 52，unsafe callsite input value 2，unsafe callsite return load 3。
- libuv：declaration 86，missing recovered prototype 199，unsafe callsite input value 3。
- memcached：declaration 56，missing recovered prototype 78，unsafe callsite return load 3。

## 性能和复杂度

- 性能：Bench2 smoke 三个目标通过，耗时 84s / 219s / 118s，和上一轮同口径接近。return binding 从全函数扫描改为按 return 点回看，真实样本没有可见性能下降。
- 实现效果：9/10。真实样本里 `missing return binding` 已清零，签名重写成功数明显增加。
- 复杂度：4/10。新增 `Store` 字段和一个 helper，但复用了现有 `returnTrialsBefore(...)`。
- 维护成本：4/10。后续如果实现完整 SSA varnode use-def，可以替换 `returnPointStores(...)` 的回看逻辑。

## 后续不做

这一步不处理缺少 recovered prototype、unsafe callsite input value、unsafe callsite return load。它只修正 return binding 的 store 收集口径。
