# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

recovered metadata reader 已经校验 count 和 slot 顺序，但它仍接受空字符串字段：

- `model=`
- `name=`

后续签名重写会把读回结果当作 native 侧最终 prototype 使用。空 ABI model 或空寄存器名都不是有效 prototype，应在 reader 入口拒绝。

# Ghidra 实现参考

Ghidra 的 prototype model 和参数 storage 都是结构化对象，不会把空名字当作有效 ABI model 或 register storage：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ProtoModel`：描述 prototype model。
  - `ParamEntry` / `ParamList`：描述有效 storage。
  - `FuncProto`：保存最终函数 prototype。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::updateAllTypes(...)`：写入最终 prototype。
  - `FuncCallSpecs::deriveInputMap(...)` / `deriveOutputMap(...)`：把 trial 变成可用 storage map。

native 侧用 metadata 承接这些结构化结果。reader 应该拒绝空字符串字段，而不是让调用方再判断。

# native 侧复刻策略

- 修改 `readNativeRecoveredPrototypeMetadata(...)`：
  - `model=` 缺失或为空时返回 `std::nullopt`。
- 修改 `readRecoveredParamList(...)`：
  - `name=` 缺失或为空时返回 `std::nullopt`。
- 补测试：
  - 空 model 的 recovered metadata 不能读回。
  - 空 input register name 的 recovered metadata 不能读回。
  - 空 return register name 的 recovered metadata 不能读回。

暂时不做：

- 不引入诊断字符串。
- 不改 metadata schema。
- 不在 reader 中尝试修复空字段。

# 判断标准

- 合法 recovered metadata 仍能读回。
- 空 model / 空 input name / 空 return name 都不能读回。
- 定向 native prototype recovery 测试通过。
- 全量 CTest 通过。

# 风险

如果历史 metadata 有空字段，这次会拒绝读取。当前 writer 来自 ABI register 名和 ABI prototype name，不应生成空字段。

# 实现记录

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:738` 到 `:741` 在 `readRecoveredParamList(...)` 中拒绝空 `name=` 字段。
- `lib/passes/NativePrototypeRecovery.cpp:930` 到 `:932` 在 `readNativeRecoveredPrototypeMetadata(...)` 中拒绝空 `model=` 字段。
- `tests/native_prototype_recovery_test.cpp:2914` 到 `:2930` 增加空 model 负例。
  - 验证 reader 拒绝读取。
  - 验证 rewrite eligibility 是 `missing recovered prototype`。
- `tests/native_prototype_recovery_test.cpp:2931` 到 `:2938` 增加空 input register name 负例。
- `tests/native_prototype_recovery_test.cpp:2939` 到 `:2946` 增加空 return register name 负例。

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
- 全量 CTest 通过，`9/9`，约 `1.32 sec`。

## 性能影响

运行时只在读取 recovered metadata 时多做两个字符串空值判断。开销可以忽略。

## 评分

- 实现效果：5/10。reader 不再接受空 model 或空 register name 的损坏 metadata。
- 复杂度：1/10。只加空字符串判断。
- 后期维护成本：1/10。保持 reader 集中校验。
