# 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

当前无调用者的多返回函数已经能重写为 struct return，例如 RAX/RDX -> `{ i64, i64 } ()`。但有 direct caller 时仍跳过，因为 caller 侧旧 IR 是 call 后分别读取 RAX/RDX。

这一步只做 return-only 多返回 direct callsite 的最小接线：把新 struct call result 用 `extractvalue` 拆开，分别替换旧的 RAX/RDX load。

# Ghidra 实现参考

Ghidra 在 callsite 输出上会把多个 output trial 接成一个 call output，再拆回各 piece：

- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionFuncLink::funcLinkOutput(...)`：为单个 sub-function call 设置 output recovery，必要时初始化 `FuncCallSpecs` 的 active output。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::buildOutputFromTrials(...)`：如果只有一个 output trial，直接把它设为 CALL output；如果有两个 trial，构造 join varnode，并在 CALL 后插入 `CPUI_SUBPIECE` 拆回低/高 piece。
  - `FuncCallSpecs::findPreexistingWhole(...)`：如果已有 `CPUI_PIECE` whole，就复用已有 whole。

native 侧 struct call result 对应 Ghidra 的 join varnode；`extractvalue` 对应 `CPUI_SUBPIECE`。

# native 侧复刻策略

这一步只支持当前测试需要的窄形状：

- callee 是 return-only，多返回，且每个 return slot 是 i64。
- 每个 user 必须是 direct `void` call，参数个数为 0。
- 对每个 return register，复用现有 `findCallsiteReturnLoad(...)`，找到 call 后线性路径上的寄存器 load。
- 新 call 返回 struct 后，为每个字段创建 `extractvalue`，替换对应旧 load。
- 如果任一返回 load 缺失、被 clobber、类型不匹配，整个函数仍保守跳过。

不做：

- 不支持 input + 多 return。
- 不支持多分支 / PHI 返回 load。
- 不支持缺少某个 return register load 时部分重写。

# 判断标准

- return-only RAX/RDX callee 有 direct caller 时能重写为 `{ i64, i64 } ()`。
- caller 里的旧 RAX/RDX register load 被删除。
- 新 call result 的两个字段通过 `extractvalue` 被使用。
- 现有 unsafe return load 负例不回退。
- `native_prototype_recovery_test` 通过。

# 风险

- 这一步要求所有返回 register load 都安全可替换，真实 CFG 中会保守跳过不少 case。
- 后续扩展 input + 多 return 时，需要把 input callsite 参数查找和本步骤的返回值拆分合并。

# 实现记录

改动：

- `lib/passes/NativePrototypeRecovery.cpp:311` 新增 `MultiReturnCallsiteRewrite`，记录一个 direct call 和每个返回 slot 对应的旧 register load。
- `lib/passes/NativePrototypeRecovery.cpp:316` 新增 `MultiReturnCallsiteCollectionResult`，携带收集结果和失败原因。
- `lib/passes/NativePrototypeRecovery.cpp:459` 新增 `collectMultiReturnDirectCallsites(...)`，要求 user 是 direct void call，并且每个返回寄存器都能用现有 `findCallsiteReturnLoad(...)` 找到安全 load。
- `lib/passes/NativePrototypeRecovery.cpp:490` 新增 `rewriteMultiReturnDirectCallsites(...)`，创建新 struct-return call，用 `extractvalue` 替换旧 RAX/RDX load。
- `lib/passes/NativePrototypeRecovery.cpp:1283` 的多返回 rewrite 不再一看到 uses 就跳过，而是先尝试安全 direct callsite 收集。
- `lib/passes/NativePrototypeRecovery.cpp:1325` 在 callee 本体重写后同步重写 multi-return direct callsite。
- `tests/native_prototype_recovery_test.cpp:157` 新增 `createTwoReturnLoadCallerFunction(...)`，构造 call 后读取两个返回寄存器的 caller。
- `tests/native_prototype_recovery_test.cpp:940` 新增有 direct caller 的 RAX/RDX callee。
- `tests/native_prototype_recovery_test.cpp:2000` 验证 caller 中新 call 返回 struct，旧 RAX/RDX load 被删除，并生成两个 `extractvalue`。

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

性能：只在显式签名重写多返回函数时遍历 direct users，并复用已有线性 return-load 查找。默认 recovery 不增加额外扫描。目标测试总耗时约 `0.04 sec`，无可见下降。

评分：

- 实现效果：7/10。return-only 多返回 direct callsite 已能接到 struct call result。
- 复杂度：6/10。新增 callsite 收集和 extractvalue 重写，但沿用了已有 return load 安全边界。
- 维护成本：6/10。后续 input + 多返回需要合并 input 参数收集和本步骤的返回拆分。
