# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

`notdec.prototype.recovered` 里有 `model=...` 字段，writer 写的是当前 module ABI 的 prototype name。当前 `getNativePrototypeRewriteEligibility(...)` 只要 metadata 可读，就会用 recovered prototype 生成 LLVM `FunctionType`，没有检查这个 model 是否和 module 上的 `!notdec.abi` 一致。

这会让手工构造或旧版本残留的错误 ABI model 被当成可重写 prototype。前面二次运行保留 recovered metadata 时已经做过 model 匹配，但 eligibility 入口本身还没有校验。

# Ghidra 实现参考

Ghidra 的 recovered prototype 是挂在具体 prototype model 上的，不是脱离 ABI model 的裸参数列表：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ProtoModel`：描述调用约定和参数/返回 storage 规则。
  - `FuncProto`：保存函数 prototype，并关联 prototype model。
  - `ParamList` / `ParamEntry`：来自 prototype model 的 storage 规则。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::updateAllTypes(...)`：在 prototype model 语境下更新 input/output。
  - `FuncCallSpecs::deriveInputMap(...)` / `deriveOutputMap(...)`：按 model 规则整理 trial。

native 侧 `NativeAbiSpec::PrototypeName` 对应当前简化的 `ProtoModel` 名称。签名重写前至少要确认 recovered metadata 的 `ModelName` 和 module ABI 一致。

# native 侧复刻策略

- 修改 `getNativePrototypeRewriteEligibility(...)`。
- 如果函数所在 module 能读出 ABI metadata，则要求：
  - `prototype->ModelName == abi->PrototypeName`
- 不一致时返回 ineligible。
- 没有 module 或没有 ABI metadata 时保持现有行为，避免破坏纯 metadata 读回和无 ABI 小测试。
- 补测试：
  - 在有 `__stdcall` ABI 的 module 中手工构造 `model=__fastcall` 的 recovered metadata。
  - 验证 rewrite eligibility 不通过。

暂时不做：

- 不改变 recovered metadata reader；reader 只负责 schema 合法性。
- 不清理错 model metadata；这是 recovery pass rerun 的职责。
- 不引入多 ABI model 支持。

# 判断标准

- ABI model 不匹配时不能 rewrite eligible。
- 合法 recovered metadata 的 eligibility 继续通过。
- 定向 native prototype recovery 测试通过。
- 全量 CTest 通过。

# 风险

如果没有 ABI metadata，仍保持现有行为。这是为了不把 reader/eligibility 的纯 metadata 测试和 `.ll` 中间产物场景一次性收紧过度。

# 实现记录

## 修改内容

- `lib/passes/NativePrototypeRecovery.cpp:1008`，`getNativePrototypeRewriteEligibility(...)`
  - 读回 `notdec.prototype.recovered` 后，如果函数属于某个 module，并且 module 上能读出 `!notdec.abi`，就比较 `prototype->ModelName` 和 `NativeAbiSpec::PrototypeName`。
  - 不一致时返回 ineligible，原因是 `recovered prototype ABI model mismatch`。
- `lib/passes/NativePrototypeRecovery.cpp:1674`，`rewriteNativeRecoveredPrototype(...)`
  - 统一签名重写入口在 eligibility 不通过时直接返回同一个原因。
  - 这样 ABI model 不匹配不会继续落到后面的具体 rewrite helper。
- `tests/native_prototype_recovery_test.cpp:2854`
  - 新增有 `__stdcall` ABI、但 recovered metadata 写成 `model=__fastcall` 的负向用例。
  - 同时检查 eligibility 和 rewrite 入口都不会改写函数。

## 验证

- `cmake --build build --target native_prototype_recovery_test -j2`
  - 通过。
- `ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V`
  - 通过，1/1。
- `git diff --check`
  - 通过。
- `ctest --test-dir build --output-on-failure`
  - 通过，9/9，总耗时约 1.30s。

## 性能影响

只在显式签名重写 eligibility 路径中多读一次 module ABI metadata，并且只作用于单个函数的 rewrite 判断。全量 CTest 时间仍约 1.30s，没有看到测试级性能退化。

## 评分

- 实现效果：8/10。能挡住错误 ABI model 的 recovered metadata，避免错调用约定签名重写。
- 复杂度：2/10。只增加一次已有 metadata reader 调用和一个分支。
- 维护成本：2/10。原因字符串和检查点集中在 eligibility 入口，后续扩展多 ABI model 时再调整。

## 后续

这一步只做重写入口防线。二次运行时发现已过期 metadata 的清理策略仍由 recovery pass 本身负责。
