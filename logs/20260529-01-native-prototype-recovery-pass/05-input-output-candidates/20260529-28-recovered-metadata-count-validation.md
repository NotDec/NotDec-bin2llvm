# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

`notdec.prototype.recovered` 里已经写入 `input_count=` 和 `return_count=`。读回 API 当前会读取 inputs / returns 列表，但没有校验 count 字段和列表长度是否一致。

后续签名重写会把 recovered metadata 当作 native 侧的最终 prototype 使用。count 字段如果和列表不一致，说明 metadata 已经损坏或 schema 被错误写入，应该保守拒绝。

# Ghidra 实现参考

Ghidra 的最终函数 prototype 保存在 `FuncProto`，参数数量和返回 storage 都是结构化状态，不是两个可以互相矛盾的文本字段：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto`：通过 `numParams()`、`getParam(...)`、`getOutput()` 读取最终 prototype。
  - `ProtoParameter` / `ProtoStore`：保存输入参数和返回 storage。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::updateAllTypes(...)`：把确认后的 input/output 写回 `FuncProto`。
  - `FuncCallSpecs::deriveInputMap(...)` / `deriveOutputMap(...)`：把 trial 结果整理成参数和返回映射。

native 侧用 metadata 暂存这些结构化结果，所以 reader 需要保证 count 字段和实际列表一致，避免后续 pass 消费损坏数据。

# native 侧复刻策略

- 修改 `readNativeRecoveredPrototypeMetadata(...)`。
- 读取 `input_count=` 和 `return_count=`。
- 如果 count 字段缺失、不是整数，或和 inputs / returns 列表长度不一致，返回 `std::nullopt`。
- 补一个单元测试：手工构造 count 和列表长度不一致的 recovered metadata，确认读回失败，rewrite eligibility 仍是 `missing recovered prototype`。

暂时不做：

- 不改变 metadata schema。
- 不迁移旧 metadata。
- 不增加诊断输出；reader 仍保持 `std::nullopt` 的保守接口。

# 判断标准

- 合法 recovered metadata 仍能读回。
- count 不一致的 recovered metadata 不能读回。
- 定向 native prototype recovery 测试通过。
- 全量 CTest 通过。

# 风险

如果历史输出里存在缺少 count 字段的 recovered metadata，这次会拒绝读取。当前 writer 一直写 count 字段，测试 fixture 也按正式格式构造，因此这个收紧是合理的。

# 实现记录

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:929` 到 `:934` 在 `readNativeRecoveredPrototypeMetadata(...)` 中读取 `input_count` 和 `return_count`。
  - 缺少字段或字段不是整数时返回 `std::nullopt`。
- `lib/passes/NativePrototypeRecovery.cpp:949` 到 `:951` 校验 count 和实际 inputs / returns 列表长度一致。
  - 不一致时返回 `std::nullopt`。
- `tests/native_prototype_recovery_test.cpp:1155` 到 `:1195` 将测试用 recovered metadata helper 拆成两层。
  - `makeRecoveredPrototypeMetadataWithCounts(...)` 用于构造 count 不一致的负例。
  - `makeRecoveredPrototypeMetadata(...)` 继续按列表长度写合法 count。
- `tests/native_prototype_recovery_test.cpp:2854` 到 `:2882` 增加 input_count / return_count 不一致的负例。
  - input_count 写 2，但 inputs list 只有 1 个 RDI。
  - return_count 写 2，但 returns list 只有 1 个 RAX。
  - 验证 reader 拒绝读取。
  - 验证 input_count 不一致时 rewrite eligibility 是 `missing recovered prototype`。

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

运行时只在读取 recovered metadata 时多解析两个整数字段并比较列表长度。这个路径主要用于签名重写和 summary 判断，对默认 Bench2 lowering 影响很小。

## 评分

- 实现效果：6/10。reader 不再接受 count 和列表矛盾的 recovered prototype。
- 复杂度：2/10。复用已有 `parseUint64Field(...)`，只加两处校验。
- 后期维护成本：2/10。后续 metadata schema 变化时，reader 仍是集中校验点。
