# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

recovered prototype metadata 的 writer 已经按 ABI slot 排序并去重。但 reader 当前只读取列表，不验证 slots 是否严格递增。

后续签名重写会按 reader 返回的顺序生成 LLVM 参数和 struct return 字段。如果 metadata 被手工构造、旧版本输出或中间工具改坏，重复 slot 或乱序 slot 会把原型顺序变成不可信状态。

# Ghidra 实现参考

Ghidra 的 prototype 参数不是无序文本列表，而是经过 prototype model 归并后的有序 storage：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamEntry` / `ParamList`：描述 ABI storage 顺序和可用范围。
  - `FuncProto`：通过 `getParam(index)`、`numParams()` 读取有序参数。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`：从 trial 生成有序 input map。
  - `FuncCallSpecs::deriveOutputMap(...)`：从 trial 生成 output map。
  - `FuncProto::updateAllTypes(...)`：把整理后的 input/output 写入最终 prototype。

native 侧已经把 ABI slot 写进 metadata。读回时校验 slot 顺序，是把 metadata 恢复成结构化 prototype 前的基本防线。

# native 侧复刻策略

- 修改 `readRecoveredParamList(...)`。
- 每个参数必须有 `name=` 和 `slot=`。
- 参数列表按读取顺序要求 slot 严格递增。
  - 重复 slot 拒绝。
  - 后一个 slot 小于或等于前一个 slot 拒绝。
- 补测试：
  - inputs list 中 slot 顺序 `RSI:1, RDI:0` 应拒绝。
  - returns list 中重复 `RAX:0, RDX:0` 应拒绝。

暂时不做：

- 不在 reader 中重新排序或去重。损坏 metadata 应该被拒绝，而不是静默修复。
- 不改变 writer 的排序逻辑。
- 不增加诊断字符串。

# 判断标准

- 合法 recovered metadata 仍能读回。
- 乱序 input slots 不能读回。
- 重复 return slots 不能读回。
- 定向 native prototype recovery 测试通过。
- 全量 CTest 通过。

# 风险

如果历史 metadata 里 slot 顺序不稳定，这次会拒绝读取。当前 writer 已经按 slot 排序并去重，CLI smoke 和 Bench2 产物都应保持合法。

# 实现记录

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:731` 到 `:750` 在 `readRecoveredParamList(...)` 中校验 slot 严格递增。
  - 每读一个参数都和上一个 slot 比较。
  - 后一个 slot 小于或等于前一个 slot 时返回 `std::nullopt`。
- `tests/native_prototype_recovery_test.cpp:2884` 到 `:2903` 增加 input slot 乱序负例。
  - inputs list 是 `RSI:1, RDI:0`。
  - 验证 reader 拒绝读取。
  - 验证 rewrite eligibility 是 `missing recovered prototype`。
- `tests/native_prototype_recovery_test.cpp:2904` 到 `:2912` 增加 return slot 重复负例。
  - returns list 是 `RAX:0, RDX:0`。
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
- 全量 CTest 通过，`9/9`，约 `1.42 sec`。

## 性能影响

运行时只在读取 recovered metadata 的参数列表时做一次线性 slot 比较。开销和参数/返回寄存器数量成正比，数量很小。

## 评分

- 实现效果：6/10。reader 不再接受乱序或重复 slot 的 recovered prototype。
- 复杂度：1/10。只加一个 `previousSlot` 比较。
- 后期维护成本：2/10。reader 保持集中校验，不在调用方重复判断。
