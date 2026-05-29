# 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

上一小步已经把多个返回寄存器的 recovered prototype 表示成 LLVM struct return，例如 RAX/RDX 是 `{ i64, i64 } ()`。但 rewrite dispatch 仍把这种 shape 直接跳过。

这一步只处理“无调用者”的多返回函数本体重写：把多个 register return store 的 value 聚合成 struct，写入 LLVM `ret`。callsite 的 `extractvalue` 和返回寄存器 load 接线后续再做。

# Ghidra 实现参考

Ghidra 在返回恢复里会把多个 output trial 汇总成一个逻辑返回：

- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionReturnRecovery::apply(...)`：检查每个 return op 上的 output trial，确认 active/used 后调用 `deriveOutputMap(...)`。
  - `ActionReturnRecovery::buildReturnOutput(...)`：按 trial 顺序收集 `CPUI_RETURN` 输入。一个返回值直接接入；两个或多个 piece 时创建 `CPUI_PIECE`，用 join address 表达逻辑返回值。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamActive::joinTrial(...)`：把相邻 trial 合成 join trial，并标记 used/active。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamTrial`：记录 trial 的 slot、storage、used/active 状态。

完整 Ghidra 做法是 join varnode / PIECE。native 侧当前选择 struct return 表示多个独立寄存器返回值，不先合成 i128，因为这更贴近当前 metadata 中的多个 ABI output slot。

# native 侧复刻策略

本小步只做无调用者 callee 本体：

- 读取多返回 `NativePrototypeReturnBinding`。
- 要求 recovered type 是 struct，元素数量等于 return binding 数。
- 每个 return value 类型必须匹配 struct 对应元素。
- 对每个 LLVM `ret void`，用 `insertvalue` 从 `undef` 构造 struct，并改成 `ret { ... }`。
- 函数有调用者时仍保守跳过，避免 callsite result 和 register load 未接线。

不做：

- 不重写 direct callsite。
- 不把 call result 拆成 RAX/RDX `extractvalue`。
- 不处理多 input + 多 return 的组合。

# 判断标准

- 无调用者 RAX/RDX 返回函数能改成 `{ i64, i64 } ()`。
- return 指令返回 struct，两个元素分别来自原 RAX/RDX store value。
- 有调用者的多返回函数仍跳过。
- `native_prototype_recovery_test` 通过。

# 风险

- 当前 struct return 是中间 IR 表达，不代表最终 C 类型一定是 struct。
- 多返回 callsite 未接线前，不能对有调用者的函数开启此 rewrite。

# 实现记录

改动：

- `lib/passes/NativePrototypeRecovery.cpp:1173` 新增 `rewriteNativeRecoveredPrototypeMultiReturn(...)`。
- `lib/passes/NativePrototypeRecovery.cpp:1193` 对有调用者的多返回函数仍返回 `function has uses`，不做 callsite rewrite。
- `lib/passes/NativePrototypeRecovery.cpp:1198` 要求 recovered type 是 struct，字段数等于 return binding 数。
- `lib/passes/NativePrototypeRecovery.cpp:1212` 复用 `getNativePrototypeReturnBindings(...)` 读取每个返回寄存器 store value，并检查 value type 与 struct 字段类型一致。
- `lib/passes/NativePrototypeRecovery.cpp:1242` 对每个 `ret void` 用显式 `InsertValueInst` 聚合多个返回值，再生成 `ret { ... }`。
- `lib/passes/NativePrototypeRecovery.cpp:1285` 在统一 dispatch 中接入无 input、多 return 的 shape。
- `tests/native_prototype_recovery_test.cpp:1877` 把 RAX/RDX 多返回 dispatch 从 unsupported 改为成功重写。
- `tests/native_prototype_recovery_test.cpp:1883` 检查重写后返回类型是两个字段的 struct。
- `tests/native_prototype_recovery_test.cpp:1925` 检查重写后 eligibility 已经 `already matches`。
- `tests/native_prototype_recovery_test.cpp:1934` 检查 return value 是 `insertvalue` 聚合。

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

性能：只在显式 rewrite 时遍历 return binding 和 return blocks，不影响默认 metadata recovery 的 CFG 扫描。目标测试总耗时约 `0.04 sec`，无可见下降。

评分：

- 实现效果：7/10。多返回无调用者函数已经能生成 struct return，为 callsite 接线打基础。
- 复杂度：5/10。新增一个 rewrite helper，但逻辑和现有 return-only helper一致。
- 维护成本：5/10。后续多返回 callsite rewrite 要复用这里的 binding 顺序和 struct 字段顺序。
