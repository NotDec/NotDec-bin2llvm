# 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

当前 direct callsite 签名重写已经支持单 input + 单 return，也支持多 input、无 return。真实 native 函数常见形状是多个寄存器参数加一个 RAX 返回值，比如 `i64(i64, i64)`。这一步补这个小形状，让 callee 和 caller 的参数、返回值一起改成 LLVM 显式签名。

# Ghidra 实现参考

Ghidra 的 call prototype 恢复会把 input trial 和 output trial 分开归类，最后统一更新 call op：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamActive::sortTrials()`：按 prototype storage 顺序整理 input/output trial。
  - `FuncCallSpecs::buildInputFromTrials(...)`：把 used input trial 写回 CALL 输入。
  - `FuncCallSpecs::deriveInputMap(...)`：形成最终输入参数 storage。
  - `FuncCallSpecs::deriveOutputMap(...)`：形成最终返回值 storage。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionFuncLink::funcLinkInput(...)`：为 callsite 建立 input trial。
  - `ActionFuncLink::funcLinkOutput(...)`：为 callsite 建立 output trial。

native 侧已经用 `NativeRecoveredPrototype::Inputs` / `Returns` 保存 ABI 顺序的结果。这一步复刻的是最小组合效果：按 input 顺序收集 callsite 前寄存器 store，把 call 后 RAX load 替换成新 call 的返回值。

# native 侧复刻策略

先支持窄形状：

- 原始函数仍必须是 `void()`。
- recovered prototype 必须是一个或多个 i64 input，加一个 i64 return。
- callee 内每个 input 都必须绑定唯一 external input load。
- callee return 必须绑定唯一 ABI return store value。
- direct caller 必须是无参数 void call。
- callsite input 继续复用 `callsiteInputValueBeforeCall(...)`。
- callsite return load 继续复用 `findCallsiteReturnLoad(...)`，遇到 CFG 不确定或类型不匹配就跳过。

暂时不做：

- 多 input + 多 return。
- 栈参数。
- 部分 callsite 可改、部分不可改的混合重写。

# 判断标准

- RDI + RSI input、RAX return 的函数能重写成 `i64(i64, i64)`。
- callee 内两个 external input load 都替换成 LLVM 参数。
- direct caller 中新 call 带两个参数，顺序是 RDI、RSI。
- direct caller 中旧 RAX load 被新 call 返回值替换。
- 现有单 input + 单 return、多 input 无 return、多 return 测试继续通过。

# 风险

- callsite input 和 return load 查找仍只覆盖简单 CFG；复杂 caller 保守跳过。
- 这一步会让 `rewriteNativeRecoveredPrototypeInputReturn(...)` 名字继续沿用，但实际支持多 input + 单 return。后续如果继续扩展，可以再统一命名，当前先不做无关改名。

# 实现记录

改动：

- `lib/passes/NativePrototypeRecovery.cpp:252` 保留 `MultiInputCallsiteRewrite` 作为单 input 和多 input 的统一 callsite 参数列表记录，删除已无使用者的单 input 专用 helper。
- `lib/passes/NativePrototypeRecovery.cpp:423` 将 `rewriteInputReturnDirectCallsites(...)` 改为接收 `MultiInputCallsiteRewrite`，创建 call 时传入完整参数列表，并继续替换 call 后返回寄存器 load。
- `lib/passes/NativePrototypeRecovery.cpp:1223` 放宽 `rewriteNativeRecoveredPrototypeInputReturn(...)`，允许一个或多个 i64 input 加一个 i64 return。
- `lib/passes/NativePrototypeRecovery.cpp:1244` 按 recovered input 数量逐个校验 external input load binding。
- `lib/passes/NativePrototypeRecovery.cpp:1271` 复用 `collectMultiInputDirectCallsiteRewrites(...)` 收集 direct callsite 参数列表，并保留 return load 安全检查。
- `lib/passes/NativePrototypeRecovery.cpp:1306` 逐个把 callee 的 external input load 替换为新函数参数。
- `lib/passes/NativePrototypeRecovery.cpp:1581` 让统一 dispatch 中多 input + 单 return 也进入 input-return 重写。
- `tests/native_prototype_recovery_test.cpp:516` 新增 `createTwoInputStoreReturnLoadCallerFunction(...)`，构造 caller 写 RDI/RSI、调用、再读 RAX。
- `tests/native_prototype_recovery_test.cpp:700` 新增 `createTwoInputReturnFunction(...)`，构造 callee 使用 RDI/RSI 并写 RAX。
- `tests/native_prototype_recovery_test.cpp:2171` 添加 RDI + RSI input、RAX return 的 direct callsite 重写测试，验证 `i64(i64, i64)`、参数顺序、input load 替换和旧 RAX load 删除。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本步骤完成。

验证：

```sh
cmake --build build --target native_prototype_recovery_test -j2
git diff --check
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
ctest --test-dir build --output-on-failure
```

结果：通过，目标测试 `1/1 Test #4: notdec.native_prototype_recovery.input_candidates ... Passed`；全量测试 `8/8` 通过。

性能：显式签名重写遇到多 input + 单 return 函数时，会对每个 direct caller 按 input 数量查找寄存器 store，再查一次返回寄存器 load。默认 metadata recovery 不变。目标测试耗时约 `0.04 sec`，全量测试约 `0.80 sec`。

评分：

- 实现效果：7/10。多寄存器参数加单寄存器返回的直接调用点已能一起重写。
- 复杂度：6/10。复用已有多 input helper 和 return load helper，没有新增大结构。
- 维护成本：6/10。后续多 input + 多 return 还需要把 `InputMultiReturnCallsiteRewrite` 也改成参数列表版本。
