# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

上一小步允许 rewritten IR 二次进入 prototype recovery 时保留 `notdec.prototype.recovered`，前提是旧 metadata 可读、ABI model 一致、当前 LLVM 函数类型和 recovered prototype 对应类型一致。

还需要补一个反向测试：如果旧 recovered metadata 和当前函数类型不一致，本轮又没有新的 input/return candidate，就必须清掉旧 metadata，避免把 stale prototype 当成可信结果。

# Ghidra 实现参考

Ghidra 的 prototype 恢复结果最终落在 `FuncProto`，不是无条件复用旧 trial：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`：从当前 trial 推导 input storage。
  - `FuncCallSpecs::deriveOutputMap(...)`：从当前 trial 推导 output storage。
  - `FuncProto::updateAllTypes(...)`：把确认后的 input/output 类型写回 prototype。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：驱动 prototype 类型恢复。

这里 native 侧的 `notdec.prototype.recovered` 对应最终 prototype。只有它和当前 LLVM 函数类型匹配时，才适合作为二次运行时的已有结果继续保留。

# native 侧复刻策略

- 不改生产代码，只补测试锁住现有行为。
- 构造一个有 ABI 的函数，当前类型保持 `void()`。
- 手工附加格式正确的 `notdec.prototype.recovered`，内容表示 `void(i64)` 的 RDI input。
- 函数体不提供 external input 或 return store，本轮不会产生新 candidate。
- 跑 `runNativePrototypeRecovery(...)` 后检查：
  - 旧 recovered metadata 被清掉。
  - rewrite eligibility 是 `missing recovered prototype`。

暂时不做：

- 不扩展 metadata schema。
- 不对 CLI smoke 增加重复覆盖。
- 不做从 rewritten body 反推 prototype 的逻辑。

# 判断标准

- 定向 native prototype recovery 测试通过。
- 全量 CTest 通过。
- 旧 metadata 类型不匹配时不会被保留。

# 风险

这个测试直接手工构造 metadata，字段格式要和 reader 期望一致。它只覆盖 input-only 类型不匹配；return-only 和 multi-return 后续可以按需要再补。

# 实现记录

## 改动

- `tests/native_prototype_recovery_test.cpp:1155` 到 `:1187` 增加 `makeRecoveredPrototypeMetadata(...)`。
  - 用正式 `notdec.prototype.recovered` 字段格式构造测试 metadata。
  - 字段包含 model、input_count、return_count、inputs list、returns list。
- `tests/native_prototype_recovery_test.cpp:2823` 到 `:2844` 增加类型不匹配的 stale recovered metadata 测试。
  - 模块有 `__stdcall` ABI。
  - 函数当前类型是 `void()`。
  - 旧 recovered metadata 表示 RDI input，也就是 `void(i64)`。
  - 本轮无 external input / return store candidate。
  - 验证 recovery 后 recovered metadata 被清掉。
  - 验证 rewrite eligibility 变成 `missing recovered prototype`。

## 验证

已通过：

```sh
cmake --build build -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
git diff --check
ctest --test-dir build --output-on-failure
```

结果：

- `notdec.native_prototype_recovery.input_candidates` 通过，约 `0.04 sec`。
- 全量 CTest 通过，`9/9`，约 `1.12 sec`。

## 性能影响

只增加单元测试，不改生产代码。运行时无影响。定向 native prototype recovery 测试仍约 `0.04 sec`。

## 评分

- 实现效果：6/10。补齐 matched-preserve 逻辑的反向测试。
- 复杂度：2/10。只新增一个测试 metadata helper 和一个小用例。
- 后期维护成本：2/10。helper 使用正式 metadata schema，后续可复用。
