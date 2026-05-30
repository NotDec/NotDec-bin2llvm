# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

`notdec.prototype.recovered` 顶层 metadata 已经要求恰好 5 个 operand。但每个参数条目本身目前仍是开放式读取：只要能找到 `name=` 和 `slot=`，额外字段会被忽略。

writer 侧 `recoveredParamListMetadata(...)` 固定给每个参数条目写两个字段：

- `name=...`
- `slot=...`

因此 reader 也应按这个 schema 精确读取，避免忽略未知字段。

# Ghidra 实现参考

Ghidra 的 parameter storage 是结构化对象，不是随意扩展的文本字段：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ProtoParameter` / `ProtoStore`：保存参数 storage。
  - `FuncProto`：保存最终函数 prototype。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`：从 input trial 整理参数 storage。
  - `FuncCallSpecs::deriveOutputMap(...)`：从 output trial 整理返回 storage。
  - `FuncProto::updateAllTypes(...)`：把整理后的 storage 写入最终 prototype。

native 侧用 `name` 和 `slot` 两个字段表达当前最小 storage 形状。reader 不应忽略第三个未知字段。

# native 侧复刻策略

- 修改 `readRecoveredParamList(...)`。
- 每个参数条目必须恰好 2 个 operand。
- 条目缺字段或多字段时都返回 `std::nullopt`。
- 补测试：
  - input 参数条目追加 `extra=true`，reader 应拒绝。
  - return 参数条目追加 `extra=true`，reader 应拒绝。

暂时不做：

- 不增加 metadata version。
- 不兼容未知扩展字段。
- 不改变 writer。

# 判断标准

- 合法 recovered metadata 仍能读回。
- input 参数条目多字段不能读回。
- return 参数条目多字段不能读回。
- 定向 native prototype recovery 测试通过。
- 全量 CTest 通过。

# 风险

未来如果参数条目 schema 扩展，需要显式升级 reader 和 writer。本次先保持当前最小 schema 严格一致。

# 实现记录

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:735` 到 `:740` 在 `readRecoveredParamList(...)` 中校验每个 recovered 参数条目恰好 2 个 operand。
  - 不是 `MDNode`、少字段或多字段都返回 `std::nullopt`。
- `tests/native_prototype_recovery_test.cpp:3023` 到 `:3053` 增加 input 参数条目多字段负例。
  - `RDI` 条目包含 `name=RDI`、`slot=0`、`extra=true`。
  - 验证 reader 拒绝读取。
  - 验证 rewrite eligibility 是 `missing recovered prototype`。
- `tests/native_prototype_recovery_test.cpp:3055` 到 `:3073` 增加 return 参数条目多字段负例。
  - `RAX` 条目包含 `name=RAX`、`slot=0`、`extra=true`。
  - 验证 reader 拒绝读取。

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
- 全量 CTest 通过，`9/9`，约 `1.30 sec`。

## 性能影响

运行时只在读取 recovered metadata 参数条目时多做一次 operand 数比较。开销可以忽略。

## 评分

- 实现效果：5/10。reader 不再忽略参数条目里的未知字段。
- 复杂度：1/10。只加一个字段数判断。
- 后期维护成本：1/10。未来 schema 扩展需要显式升级 reader。
