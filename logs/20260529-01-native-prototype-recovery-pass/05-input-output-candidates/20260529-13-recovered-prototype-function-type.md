# 20260529-13 Recovered Prototype Function Type

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的 `FuncProto` 最终会变成一个可用于调用和函数头的 prototype，而不是只停在 storage candidate。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto`：保存输入参数、返回值、prototype model、锁定状态等信息。
  - `ProtoParameter`：保存一个参数或返回值的 storage 和 type。
  - `FuncProto::numParams()`、`FuncProto::getParam(...)`、`FuncProto::getOutput()`：后续 action 读取最终 prototype。
  - `ProtoModel::deriveInputMap(...)` / `ProtoModel::deriveOutputMap(...)`：把 `ParamActive` trial 收敛成 `FuncProto` 的参数/返回列表。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：从 trial 派生 prototype 后，继续更新输入和输出类型。
  - `ActionInputPrototype::apply(...)`、`ActionOutputPrototype::apply(...)`：根据已知 prototype 调整函数体中的参数/返回相关 varnode。

native 侧已经有 `NativeRecoveredPrototype`，但还没有把它翻译成 LLVM 函数类型。没有这个步骤，后续签名重写会直接揉在 IR 改写里，风险较高。

## native 复刻方式

这一步只做类型形状，不改 IR：

- 新增 `buildNativeRecoveredPrototypeFunctionType(...)`。
- 当前只支持已实现的 register 场景：
  - 每个 input register 先映射为 `i64` 参数。
  - 0 个 return register 映射为 `void`。
  - 1 个 return register 映射为 `i64`。
  - 多 return register 暂时返回 `std::nullopt`，后续再决定 struct return 或 metadata-only。
- 这一步不替换函数、不改 `ret` 指令、不改 callsite。

这样后续重写函数签名时，可以先判断当前 recovered prototype 是否已经能形成一个安全 LLVM `FunctionType`。

## 判断标准

- input-only prototype 能得到 `void(i64)`。
- return-only prototype 能得到 `i64()`。
- input + return prototype 能得到 `i64(i64)`。
- 多 return prototype 当前返回 `std::nullopt`，避免先乱造 ABI。

## 实现记录

### 源码改动

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:10`
  - 前置声明 `llvm::FunctionType` / `llvm::LLVMContext`。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:77`
  - 新增 `buildNativeRecoveredPrototypeFunctionType(...)`。
- `lib/passes/NativePrototypeRecovery.cpp:10`、`lib/passes/NativePrototypeRecovery.cpp:15`
  - 引入 LLVM `DerivedTypes` / `Type`。
- `lib/passes/NativePrototypeRecovery.cpp:439`
  - 实现 `buildNativeRecoveredPrototypeFunctionType(...)`。
  - 目前 register input 统一映射成 `i64` 参数。
  - 无返回寄存器映射成 `void`，单返回寄存器映射成 `i64`。
  - 多返回寄存器返回 `std::nullopt`，先不猜 struct return ABI。
- `tests/native_prototype_recovery_test.cpp:6`
  - 引入 LLVM `DerivedTypes`。
- `tests/native_prototype_recovery_test.cpp:418`
  - 新增 `functionTypeShape(...)`，检查生成的 `FunctionType`。
- `tests/native_prototype_recovery_test.cpp:575`
  - 覆盖 input-only prototype 生成 `void(i64)`。
- `tests/native_prototype_recovery_test.cpp:601`
  - 覆盖 return-only prototype 生成 `i64()`。
- `tests/native_prototype_recovery_test.cpp:627`
  - 覆盖多返回 prototype 暂不生成 `FunctionType`。

### 验证

```sh
cmake -S . -B build \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
cmake --build build --target native_prototype_recovery_test native_instcombine_metadata_test -j2
ctest --test-dir build -R 'notdec.native_(prototype_recovery.input_candidates|instcombine.metadata)' -V
```

结果：`notdec.native_prototype_recovery.input_candidates` 和 `notdec.native_instcombine.metadata` 通过。

### 性能和风险

- 性能：helper 只按输入参数数量创建 `FunctionType`，默认 pass 当前还不调用它，没有运行时影响。
- 风险：当前把 register storage 固定映射为 `i64`，只适合当前 x86-64 register 小样例。后续需要按 storage size/type 扩展。
- 实现效果：6/10。已经能从 recovered prototype 得到基础 LLVM 类型，但还没重写函数签名。
- 复杂度：2/10。只做类型形状，不做 IR 改写。
- 维护成本：3/10。后续多返回值需要明确 struct return 或 metadata-only 策略。
