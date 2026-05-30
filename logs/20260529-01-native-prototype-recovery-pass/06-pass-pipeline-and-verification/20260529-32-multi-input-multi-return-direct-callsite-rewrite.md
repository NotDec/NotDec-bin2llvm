# 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

当前 direct callsite 签名重写已经覆盖多 input 无 return、多 input 单 return、无 input 多 return、单 input 多 return。还缺多 input + 多 return。这个形状在 native ABI 上对应多个寄存器参数加多个返回寄存器，LLVM 侧可以继续用参数列表加 struct return 表达。

# Ghidra 实现参考

Ghidra 不是按单参数形状写特殊分支，而是统一处理 input trial 和 output trial：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamActive::sortTrials()`：按 prototype storage 顺序整理 trial。
  - `FuncCallSpecs::buildInputFromTrials(...)`：按 input trial 更新 CALL 输入。
  - `FuncCallSpecs::deriveInputMap(...)`：生成最终输入 storage map。
  - `FuncCallSpecs::deriveOutputMap(...)`：生成最终输出 storage map。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionFuncLink::funcLinkInput(...)`：为 callsite 收集 input trial。
  - `ActionFuncLink::funcLinkOutput(...)`：为 callsite 收集 output trial。

native 侧已经有 `NativeRecoveredPrototype::Inputs` / `Returns`，且 input 和 return 都按 ABI slot 排序。这一步复刻 Ghidra 的组合效果：callsite 前按 input 顺序取寄存器 store value，callsite 后按 return 顺序取寄存器 load，然后创建带多参数和 struct 返回的新 call。

# native 侧复刻策略

先支持当前测试需要的窄形状：

- 原始函数必须是 `void()`。
- recovered prototype 必须是一个或多个 i64 input，加两个或更多 i64 return。
- callee 内每个 input 都必须绑定唯一 external input load。
- callee 内每个 return 都必须绑定唯一 register store value。
- direct caller 必须是无参数 void call。
- callsite input 继续复用 `callsiteInputValueBeforeCall(...)`。
- callsite return load 继续复用 `findCallsiteReturnLoad(...)`。
- 新 call 使用完整参数列表，返回 struct 后用 `extractvalue` 替换旧返回寄存器 load。

暂时不做：

- 栈参数。
- 返回 struct 字段以外的类型恢复。
- 部分 callsite 可改、部分不可改的混合重写。

# 判断标准

- RDI + RSI input、RAX + RDX return 的函数能重写成 `{i64, i64}(i64, i64)`。
- callee 内两个 external input load 都替换成 LLVM 参数。
- direct caller 中新 call 带两个参数，顺序是 RDI、RSI。
- direct caller 中旧 RAX/RDX load 被两个 `extractvalue` 结果替换。
- 现有单 input 多 return、多 input 单 return、多 input 无 return 测试继续通过。

# 风险

- 当前 return load 查找仍只覆盖简单 CFG；复杂 caller 会保守跳过。
- 这一步继续保留 `rewriteNativeRecoveredPrototypeInputMultiReturn(...)` 名字，但实际支持多 input + 多 return。后续可以统一改名，当前不做无关改名。

# 实现记录

改动：

- `lib/passes/NativePrototypeRecovery.cpp:327` 将 `InputMultiReturnCallsiteRewrite` 的单个 `Argument` 改成 ABI 顺序的 `Arguments` 列表。
- `lib/passes/NativePrototypeRecovery.cpp:531` 将 `collectInputMultiReturnDirectCallsites(...)` 改成接收完整 input 列表和 recovered function type，逐个收集 callsite 前寄存器 store value。
- `lib/passes/NativePrototypeRecovery.cpp:575` 将 `rewriteInputMultiReturnDirectCallsites(...)` 改成用完整参数列表创建新 call，并继续为 struct return 生成 `extractvalue`。
- `lib/passes/NativePrototypeRecovery.cpp:1458` 放宽 `rewriteNativeRecoveredPrototypeInputMultiReturn(...)`，允许一个或多个 input 加多个 return。
- `lib/passes/NativePrototypeRecovery.cpp:1483` 按 recovered input 数量逐个校验并替换 external input load。
- `lib/passes/NativePrototypeRecovery.cpp:1515` 复用新的多 input + 多 return callsite 收集逻辑。
- `lib/passes/NativePrototypeRecovery.cpp:1599` 让统一 dispatch 中多 input + 多 return 也进入 input multi-return 重写。
- `tests/native_prototype_recovery_test.cpp:224` 新增 `createTwoInputStoreTwoReturnLoadCallerFunction(...)`，构造 caller 写 RDI/RSI、调用、再读 RAX/RDX。
- `tests/native_prototype_recovery_test.cpp:952` 新增 `createTwoInputTwoOutputReturnStoreFunction(...)`，构造 callee 使用 RDI/RSI 并写 RDX/RAX。
- `tests/native_prototype_recovery_test.cpp:1230` 增加 RDI + RSI input、RAX + RDX return 的样例，并同步 summary 计数。
- `tests/native_prototype_recovery_test.cpp:2602` 添加多 input + 多 return direct callsite 重写断言，验证 `{i64, i64}(i64, i64)`、参数顺序、两个 `extractvalue` 和旧返回寄存器 load 删除。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本步骤完成。

验证：

```sh
cmake --build build --target native_prototype_recovery_test -j2
git diff --check
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
ctest --test-dir build --output-on-failure
```

结果：通过，目标测试 `1/1 Test #4: notdec.native_prototype_recovery.input_candidates ... Passed`；全量测试 `8/8` 通过。

性能：显式签名重写遇到多 input + 多 return 函数时，会对每个 direct caller 按 input 数量查找寄存器 store，再按 return 数量查找返回寄存器 load。默认 metadata recovery 不变。目标测试耗时约 `0.04 sec`，全量测试约 `0.81 sec`。

评分：

- 实现效果：8/10。register input 和 register return 的主要 direct callsite 组合形状都已覆盖。
- 复杂度：6/10。沿用现有 callsite input 和 return load 查找，只把单参数记录扩展成列表。
- 维护成本：6/10。后续需要集中处理栈参数和复杂 CFG，当前 helper 仍保持可读。
