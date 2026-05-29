# 20260529-14 Prototype Rewrite Eligibility

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 不会在 trial 刚出现时就直接改函数头。它先把 trial 收敛成 `FuncProto`，再由后续 action 按已经锁定或已经恢复的 prototype 修改输入和返回相关 varnode。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ProtoModel`：描述 ABI model，注释里明确 `deriveInputMap()` / `deriveOutputMap()` 用数据流恢复函数 prototype。
  - `FuncProto`：保存最终 prototype，包括输入参数、返回值、model 和锁定状态。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::isInputLocked()`：判断输入 prototype 是否已经固定。
  - `FuncProto::setInputLock(...)` / `FuncProto::setOutputLock(...)`：锁定输入和输出后，也会锁定 model。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：根据当前 `FuncProto` 处理 RETURN 输出和锁定输入。
  - `ActionInputPrototype::apply(...)`：未锁定时从 input varnode 注册 trial，再派生输入 prototype。

native 侧现在已经有 `NativeRecoveredPrototype` 和 `buildNativeRecoveredPrototypeFunctionType(...)`。真正重写 LLVM 函数签名前，还缺少一层明确判断：这个函数是否有 recovered metadata，是否能转成当前支持的 LLVM `FunctionType`，是否确实需要改。

## native 复刻方式

这一步只做判断，不改 IR：

- 新增 `NativePrototypeRewriteEligibility`，记录：
  - `Eligible`：是否可以进入后续签名重写。
  - `NeedsRewrite`：当前函数类型和 recovered 类型是否不同。
  - `Reason`：不可重写或无需重写的简单原因。
  - `RecoveredType`：当前支持的 recovered LLVM 函数类型。
- 新增 `getNativePrototypeRewriteEligibility(...)`：
  - 没有 `notdec.prototype.recovered`：不可重写。
  - 多返回等当前无法形成 `FunctionType`：不可重写。
  - declaration 暂时不重写，因为当前恢复来自函数体分析。
  - vararg 原函数暂时不重写，避免 callsite 规则还没接上时破坏语义。
  - 类型相同则 eligible 但 `NeedsRewrite=false`。

这相当于先复刻 Ghidra 在 `FuncProto` 固定后才进入 `ActionPrototypeTypes` 的边界。后续真正替换函数和 callsite 时，只消费这个判断结果。

## 判断标准

- input-only recovered prototype 对原 `void()` 函数应标记 eligible 且 needs rewrite。
- return-only recovered prototype 对原 `void()` 函数应标记 eligible 且 needs rewrite。
- 多 return recovered prototype 当前不可重写，原因是 unsupported prototype type。
- 没有 recovered metadata 的函数不可重写。

## 实现记录

### 源码改动

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:55`
  - 新增 `NativePrototypeRewriteEligibility`。
  - 记录是否可进入后续 IR 签名重写、是否需要重写、原因和 recovered `FunctionType`。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:90`
  - 新增 `getNativePrototypeRewriteEligibility(...)` 声明。
- `lib/passes/NativePrototypeRecovery.cpp:460`
  - 实现 `getNativePrototypeRewriteEligibility(...)`。
  - declaration、vararg、缺少 recovered metadata、多返回等不能形成 `FunctionType` 的情况先返回不可重写。
  - 可形成 `FunctionType` 时，比较当前 `FunctionType`，给出 `needs rewrite` 或 `already matches`。
- `tests/native_prototype_recovery_test.cpp:104`
  - 新增 `createFunctionWithType(...)`，构造已经匹配 recovered 类型的测试函数。
- `tests/native_prototype_recovery_test.cpp:466`
  - 新增 `input_rdi_already_typed`，覆盖 `Eligible=true` 但 `NeedsRewrite=false`。
- `tests/native_prototype_recovery_test.cpp:603`
  - 覆盖 input-only recovered prototype 可进入重写，且需要重写。
- `tests/native_prototype_recovery_test.cpp:612`
  - 覆盖已经匹配的 recovered prototype 不需要重写。
- `tests/native_prototype_recovery_test.cpp:645`
  - 覆盖 return-only recovered prototype 可进入重写，且需要重写。
- `tests/native_prototype_recovery_test.cpp:675`
  - 覆盖多 return recovered prototype 当前不可重写。
- `tests/native_prototype_recovery_test.cpp:689`
  - 覆盖缺少 recovered metadata 时不可重写。

### 验证

```sh
cmake -S . -B build \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON
cmake --build build --target native_prototype_recovery_test native_instcombine_metadata_test -j2
ctest --test-dir build -R 'notdec.native_(prototype_recovery.input_candidates|instcombine.metadata)' -V
```

结果：`notdec.native_prototype_recovery.input_candidates` 和 `notdec.native_instcombine.metadata` 通过。

### 性能和风险

- 性能：只在显式调用 eligibility helper 时读取函数 metadata 并构造一个 `FunctionType`，当前 pass 默认不调用，没有主链路运行时影响。
- 风险：当前判断只支持 register 参数和 0/1 个 register 返回值。多返回、vararg、declaration 先保守拒绝，避免后续签名重写时破坏 callsite。
- 实现效果：5/10。补上了签名重写前的 gate，但还没有真正改 IR。
- 复杂度：2/10。只是读 metadata、构造类型、比较类型。
- 维护成本：2/10。后续 rewrite pass 可以直接复用这个判断；多返回支持需要扩展这里。
