# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

`recoveredPrototypeMetadata(...)` 的 writer 在 input 和 return 都为空时返回 `nullptr`，不会写出空 recovered prototype。

reader 现在会接受手工构造的空 metadata：`input_count=0`、`return_count=0`、inputs/returns 都为空。这会让普通 `void()` 看起来像一个已经恢复出的 prototype，后续 eligibility 会报 `already matches`，掩盖“其实没有恢复结果”。

# Ghidra 实现参考

Ghidra 的 `FuncProto` 是函数 prototype 状态，但 prototype recovery 的 trial 结果要经过输入/输出映射后才算恢复结果：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto`：保存最终 prototype。
  - `ParamActive` / `ParamTrial`：保存候选 trial。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`：从 input trial 生成参数映射。
  - `FuncCallSpecs::deriveOutputMap(...)`：从 output trial 生成返回映射。
  - `FuncProto::updateAllTypes(...)`：写入最终 prototype。

native 侧的 `notdec.prototype.recovered` 只表示“确实恢复到至少一个 input 或 return”。空列表不是有效恢复结果，应该和缺失 metadata 一样处理。

# native 侧复刻策略

- 修改 `readNativeRecoveredPrototypeMetadata(...)`。
- 在 count 和列表长度一致后，若 inputs / returns 都为空，返回 `std::nullopt`。
- 补测试：
  - 手工构造空 recovered metadata，确认 reader 拒绝读取。
  - rewrite eligibility 应保持 `missing recovered prototype`，不能变成 `already matches`。

暂时不做：

- 不改 writer；writer 已经不写空 recovered metadata。
- 不增加诊断字符串。
- 不把空 prototype 当作合法 `void()` recovery。

# 判断标准

- 合法 recovered metadata 仍能读回。
- 空 recovered metadata 不能读回。
- 空 recovered metadata 不会让 `void()` 函数变成 rewrite eligible。
- 定向 native prototype recovery 测试通过。
- 全量 CTest 通过。

# 风险

如果历史文件里存在空 recovered metadata，这次会拒绝读取。这个行为和当前 writer 一致，也更符合“recovered metadata 代表真实恢复结果”的语义。

# 实现记录

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:957` 到 `:959` 在 `readNativeRecoveredPrototypeMetadata(...)` 中拒绝空 recovered prototype。
  - inputs 和 returns 都为空时返回 `std::nullopt`。
- `tests/native_prototype_recovery_test.cpp:2948` 到 `:2966` 增加空 recovered prototype 负例。
  - 手工构造 `input_count=0`、`return_count=0`、空 inputs、空 returns。
  - 验证 reader 拒绝读取。
  - 验证 rewrite eligibility 是 `missing recovered prototype`。

## 验证

已通过：

```sh
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
git diff --check
ctest --test-dir build --output-on-failure
```

结果：

- `notdec.native_prototype_recovery.input_candidates` 通过，约 `0.04 sec`。
- 全量 CTest 通过，`9/9`，约 `1.28 sec`。

## 性能影响

运行时只在读取 recovered metadata 时多做一次空列表判断。开销可以忽略。

## 评分

- 实现效果：5/10。reader 和 writer 对空 recovered prototype 的语义保持一致。
- 复杂度：1/10。只加一个空列表判断。
- 后期维护成本：1/10。空 prototype 仍统一按缺失 recovered prototype 处理。
