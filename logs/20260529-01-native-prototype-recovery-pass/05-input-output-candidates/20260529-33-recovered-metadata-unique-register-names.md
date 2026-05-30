# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

recovered metadata reader 已经校验 count、slot 顺序、空字段和 operand 数。但同一个 inputs 或 returns 列表里仍可能手工构造出重复 register name，比如：

- inputs: `RDI:0, RDI:1`
- returns: `RAX:0, RAX:1`

writer 侧来自 ABI slot 去重，不会生成这种结果。reader 如果接受，会让后续签名重写把同一个寄存器当成两个参数或两个返回值。

# Ghidra 实现参考

Ghidra 的 prototype storage 由 `ParamList` / `ParamEntry` 和 `FuncProto` 维护，不会把同一个 storage 在同一 input 或 output map 中重复当成两个结果：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamEntry` / `ParamList`：描述 ABI storage 和顺序。
  - `FuncProto`：保存最终函数 prototype。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`：从 input trial 生成参数映射。
  - `FuncCallSpecs::deriveOutputMap(...)`：从 output trial 生成返回映射。
  - `FuncProto::updateAllTypes(...)`：写入整理后的 prototype。

native 侧 recovered metadata 是 `FuncProto` 的简化表示。reader 需要拒绝同一列表内重复 register name。

# native 侧复刻策略

- 修改 `readRecoveredParamList(...)`。
- 在读取同一列表时记录已经见过的 register name。
- 如果同一 inputs 或 returns 列表重复出现同名 register，返回 `std::nullopt`。
- 补测试：
  - inputs list 中 `RDI:0, RDI:1` 应拒绝。
  - returns list 中 `RAX:0, RAX:1` 应拒绝。

暂时不做：

- 不跨 inputs 和 returns 检查同名。某些 ABI 中同一寄存器名可能分别用于 input/output，不在这一步扩大范围。
- 不对重复 name 做修复或去重。
- 不增加诊断字符串。

# 判断标准

- 合法 recovered metadata 仍能读回。
- 同一 inputs list 里的重复 register name 不能读回。
- 同一 returns list 里的重复 register name 不能读回。
- 定向 native prototype recovery 测试通过。
- 全量 CTest 通过。

# 风险

如果历史 metadata 有重复 register name，这次会拒绝读取。当前 writer 不应生成这种结果。

# 实现记录

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:731` 到 `:745` 在 `readRecoveredParamList(...)` 中记录同一列表已经读到的 register name。
  - 同一 inputs 或 returns 列表内重复 name 时返回 `std::nullopt`。
  - 仍允许 input 列表和 return 列表各自出现同名 register；这一步不做跨列表检查。
- `tests/native_prototype_recovery_test.cpp:2993` 到 `:3012` 增加重复 input register name 负例。
  - inputs list 是 `RDI:0, RDI:1`。
  - 验证 reader 拒绝读取。
  - 验证 rewrite eligibility 是 `missing recovered prototype`。
- `tests/native_prototype_recovery_test.cpp:3013` 到 `:3021` 增加重复 return register name 负例。
  - returns list 是 `RAX:0, RAX:1`。
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
- 全量 CTest 通过，`9/9`，约 `1.28 sec`。

## 性能影响

运行时只在读取单个 recovered metadata 参数列表时维护一个小 `std::set`。参数和返回寄存器数量很少，影响可以忽略。

## 评分

- 实现效果：5/10。reader 不再接受同一 input/return 列表内重复 register name。
- 复杂度：2/10。只增加一个局部 set。
- 后期维护成本：2/10。校验仍集中在 reader。
