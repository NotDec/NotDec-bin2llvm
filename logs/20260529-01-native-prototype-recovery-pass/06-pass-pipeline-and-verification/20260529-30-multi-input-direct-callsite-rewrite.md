# 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

当前 recovered prototype 已经能记录多个 input，`buildNativeRecoveredPrototypeFunctionType(...)` 也能构造多参数 LLVM 函数类型。但显式签名重写只支持单 input，所以两个寄存器参数的函数还会停在 unsupported shape。

这一步先补最小的多参数形状：多 input、无 return。真实 x86-64 SysV 里 RDI、RSI 两个参数很常见，先让这种形状能完成 callee 和 direct caller 的一致重写。

# Ghidra 实现参考

Ghidra 的参数恢复不是按“一个参数一个特殊分支”处理，而是把多个 input trial 排序后统一写回 call：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamActive::sortTrials()`：按 prototype storage 顺序整理参数 trial。
  - `FuncCallSpecs::buildInputFromTrials(...)`：根据 used trial 重建 CALL 输入列表。
  - `FuncCallSpecs::deriveInputMap(...)`：把 active trial 变成最终 input storage 映射。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionFuncLink::funcLinkInput(...)`：为 callsite 建立输入参数恢复环境。
  - `ActionParamDouble::apply(...)`：处理重复、重叠或不该参与的参数 trial。

native 侧当前已经用 `NativeRecoveredPrototype::Inputs` 记录了排序后的 register input 列表。这一步复刻的是 `buildInputFromTrials(...)` 的最小效果：按 recovered input 顺序收集 callsite 前寄存器 store value，然后用这些 value 创建新的 typed call。

# native 侧复刻策略

先支持当前测试需要的窄形状：

- 原始函数必须是 `void()`。
- recovered prototype 必须是两个或更多 i64 register input，且没有 return。
- callee 内每个 input 都必须能绑定唯一 `external_input` load。
- 每个 direct caller 必须还是无参数 `void` call。
- callsite 参数值沿用 `callsiteInputValueBeforeCall(...)`，支持同 block 或唯一前驱里的 register store。
- 新 call 按 ABI slot 顺序传参，旧 external input load 分别替换成新函数参数。

暂时不做：

- 多 input + 单 return。
- 多 input + 多 return。
- 部分参数缺失时的部分重写。
- 参数类型恢复，仍使用当前 i64 register 粒度。

# 判断标准

- RDI + RSI input-only 函数能重写成 `void(i64, i64)`。
- callee 中两个 external input load 分别替换成两个 LLVM 参数。
- direct caller 中新 call 带两个参数，并且顺序是 RDI、RSI。
- 单 input、single return、多 return 现有测试继续通过。

# 风险

- 仍复用当前 callsite input 查找，只适合简单线性 CFG；复杂 caller 会保守跳过。
- 多 input helper 和单 input helper 会有少量重复。等多 input + return 也补上后，再考虑统一成参数列表 helper。

# 实现记录

改动：

- `lib/passes/NativePrototypeRecovery.cpp:257` 新增 `MultiInputCallsiteRewrite`，保存旧 call 和 ABI 顺序参数 value。
- `lib/passes/NativePrototypeRecovery.cpp:270` 新增 `MultiInputCallsiteCollectionResult`，保留多 input callsite 收集结果和失败原因。
- `lib/passes/NativePrototypeRecovery.cpp:302` 新增 `collectMultiInputDirectCallsiteRewrites(...)`，按 recovered input 顺序调用 `callsiteInputValueBeforeCall(...)` 收集每个参数。
- `lib/passes/NativePrototypeRecovery.cpp:332` 新增 `rewriteMultiInputDirectCallsites(...)`，创建多参数 typed call 并删除旧 void call。
- `lib/passes/NativePrototypeRecovery.cpp:1162` 放宽 `rewriteNativeRecoveredPrototypeInputOnly(...)`，允许一个或多个 input，并逐个替换 external input load。
- `lib/passes/NativePrototypeRecovery.cpp:1605` 在统一 dispatch 中让多 input、无 return 也走 input-only rewrite。
- `tests/native_prototype_recovery_test.cpp:224` 新增 `createTwoInputStoreCallerFunction(...)`，构造 caller 写 RDI/RSI 后调用。
- `tests/native_prototype_recovery_test.cpp:572` 新增 `createTwoUsedExternalInputFunction(...)`，构造 callee 使用 RDI/RSI 两个 external input。
- `tests/native_prototype_recovery_test.cpp:1336` 添加 RDI + RSI direct callsite 重写测试，验证 `void(i64, i64)`、参数顺序和旧 input load 删除。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本步骤完成。

验证：

```sh
git diff --check
cmake -S . -B build \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

结果：通过，目标测试 `1/1 Test #4: notdec.native_prototype_recovery.input_candidates ... Passed`；全量测试 `8/8` 通过。

性能：显式签名重写遇到多 input、无 return 函数时，会对每个 direct caller 按 input 数量查找寄存器 store。默认 metadata recovery 不变。目标测试耗时约 `0.04 sec`。

评分：

- 实现效果：7/10。多 register input 的 callee 和 direct caller 已能一起重写。
- 复杂度：6/10。新增参数列表 helper，沿用现有保守 CFG 查找。
- 维护成本：6/10。后续多 input + return 可以继续复用这个 helper，但还需要把 input + return 分支改成参数列表版本。
