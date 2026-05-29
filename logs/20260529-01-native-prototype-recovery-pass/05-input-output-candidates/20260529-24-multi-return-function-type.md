# 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

当前 native 侧已经能把同一 return 点的多个 ABI output slot 写入 `notdec.prototype.recovered`，例如 RAX/RDX。但 `buildNativeRecoveredPrototypeFunctionType(...)` 遇到多个 return 直接返回空，导致 eligibility 把这类 recovered prototype 标成 unsupported。

这一步先补“类型表示”：把多个 register return 表示成 LLVM literal struct return。暂时不接签名重写，因为多返回值需要同时改 return store、callsite result extract 和寄存器 load 接线，风险比类型构造大。

# Ghidra 实现参考

Ghidra 对多个返回 storage 会先在 prototype 层保留多个 trial，再在构造返回值时按需要拼接：

- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionReturnRecovery::apply(...)`：遍历 `CPUI_RETURN`，对 `ParamTrial` 做 active/use 判断，最后调用 `deriveOutputMap(...)`。
  - `ActionReturnRecovery::buildReturnOutput(...)`：按 trial 顺序收集 RETURN 输入；一个返回值直接接入，两个或多个 piece 时用 `CPUI_PIECE` 和 join address 拼成一个逻辑返回值。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamActive` / `ParamTrial`：保存 trial 的 slot、entry、used/active 状态。
  - `ParamList::deriveOutputMap(...)`：根据 active trials 推导最终输出 storage。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamActive::joinTrial(...)`：把相邻 storage trial 合并成 join storage。
  - `FuncCallSpecs::buildOutputFromTrials(...)`：把调用点 output trial 接回 callsite。

Ghidra 的完整做法更接近“join storage / PIECE 拼接”。LLVM 侧当前还没有 join storage 类型，所以先用 struct return 表示“多个独立寄存器返回值”，保留顺序和宽度。

# native 侧复刻策略

本小步只做类型构造：

- 0 return：`void(...)`。
- 1 return：`i64(...)`，保持现有行为。
- 多 return：`{ i64, i64, ... } (...)`，元素顺序沿用 recovered return metadata 的 ABI slot 顺序。

不做：

- 不把多返回函数签名重写接通。
- 不把 callsite RAX/RDX load 改成 `extractvalue`。
- 不做 join 整数类型，比如 i128。当前先保留多个寄存器独立语义。

# 判断标准

- `buildNativeRecoveredPrototypeFunctionType(...)` 对 RAX/RDX recovered prototype 返回 struct type。
- eligibility 对多返回 recovered prototype 能给出 `needs rewrite`，说明类型表示已经成立。
- 现有单返回、input-return、unsupported shape rewrite 行为不回退。
- `native_prototype_recovery_test` 通过。

# 风险

- struct return 只是 native 侧中间表示，不一定等价于所有 ABI 的真实 C 类型。
- 多返回签名重写仍未实现，dispatch rewrite 对多返回 shape 仍应保守跳过。

# 实现记录

改动：

- `lib/passes/NativePrototypeRecovery.cpp:768` 调整 `buildNativeRecoveredPrototypeFunctionType(...)`：不再拒绝多个 return slot。
- `lib/passes/NativePrototypeRecovery.cpp:781` 对多个 return slot 构造 LLVM literal struct return type，每个寄存器 slot 暂按 `i64` 表示。
- `tests/native_prototype_recovery_test.cpp:947` 更新 summary 计数：多返回 prototype 现在也能构造 recovered type，所以 rewrite eligible / needed 各增加 1。
- `tests/native_prototype_recovery_test.cpp:1907` 验证 RAX/RDX recovered prototype 的函数类型为 `{ i64, i64 } ()`。
- `tests/native_prototype_recovery_test.cpp:1919` 验证 eligibility 对多返回 prototype 返回 eligible + needs rewrite。
- `tests/native_prototype_recovery_test.cpp:1928` 验证实际 rewrite dispatch 仍保守跳过，reason 为 `unsupported recovered prototype shape`。

验证：

```sh
git diff --check
cmake -S . -B build \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
```

结果：通过，`1/1 Test #4: notdec.native_prototype_recovery.input_candidates ... Passed`。

性能：只在类型构造阶段按 return slot 数量创建一个小 vector，不影响 CFG 或数据流扫描。目标测试总耗时约 `0.04 sec`，无可见下降。

评分：

- 实现效果：6/10。多返回 recovered prototype 有了 LLVM 类型表示，为后续真正重写铺路。
- 复杂度：3/10。只改类型构造，不改签名重写。
- 维护成本：4/10。后续如果选择 i128/join 类型而不是 struct，需要改这里和测试。
