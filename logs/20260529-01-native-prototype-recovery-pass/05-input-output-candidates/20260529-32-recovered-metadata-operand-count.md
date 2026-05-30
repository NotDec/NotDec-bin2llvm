# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

`notdec.prototype.recovered` 的 writer 固定写 5 个 operand：

1. `model=...`
2. `input_count=...`
3. `return_count=...`
4. inputs list
5. returns list

reader 当前只要求 operand 数不少于 5。多出来的 operand 说明 metadata schema 或数据已经和当前 reader 预期不一致，继续静默消费不合适。

# Ghidra 实现参考

Ghidra 的 prototype 不是开放式文本列表，而是固定结构的数据对象：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto`：保存最终函数 prototype。
  - `ProtoParameter` / `ProtoStore`：保存输入和输出 storage。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::updateAllTypes(...)`：写入整理后的 prototype。
  - `FuncCallSpecs::deriveInputMap(...)` / `deriveOutputMap(...)`：生成 input/output 映射。

native 侧 metadata 是这个结构的临时表示。reader 应当按当前 schema 精确读取，避免忽略未知字段导致后续 pass 使用不明确的数据。

# native 侧复刻策略

- 修改 `readNativeRecoveredPrototypeMetadata(...)`：
  - `notdec.prototype.recovered` 必须恰好 5 个 operand。
  - 少于或多于 5 个都返回 `std::nullopt`。
- 补测试：
  - 手工构造带第 6 个 operand 的 recovered metadata。
  - 验证 reader 拒绝读取。
  - 验证 rewrite eligibility 是 `missing recovered prototype`。

暂时不做：

- 不设计 metadata 版本字段。
- 不兼容未知扩展 operand。
- 不改变 writer。

# 判断标准

- 合法 recovered metadata 仍能读回。
- 多余 operand 的 recovered metadata 不能读回。
- 定向 native prototype recovery 测试通过。
- 全量 CTest 通过。

# 风险

如果未来 schema 扩展，需要先显式升级 reader，而不是让旧 reader 忽略新增字段。这是更保守的行为。

# 实现记录

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:925` 到 `:927` 将 `readNativeRecoveredPrototypeMetadata(...)` 的 operand 数校验从“至少 5 个”改成“恰好 5 个”。
- `tests/native_prototype_recovery_test.cpp:2968` 到 `:2991` 增加多余 operand 的负例。
  - 基于合法 recovered metadata 追加第 6 个 operand：`extra=true`。
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
- 全量 CTest 通过，`9/9`，约 `1.31 sec`。

## 性能影响

运行时只是把 metadata operand 数比较从 `< 5` 改成 `!= 5`，无实际性能影响。

## 评分

- 实现效果：5/10。reader 不再静默忽略未知 extra operand。
- 复杂度：1/10。只改一个条件并补负例。
- 后期维护成本：1/10。未来 schema 扩展需要显式升级 reader。
