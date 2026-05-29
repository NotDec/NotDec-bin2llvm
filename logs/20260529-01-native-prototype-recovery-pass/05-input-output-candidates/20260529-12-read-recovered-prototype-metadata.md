# 20260529-12 Read Recovered Prototype Metadata

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的 recovered prototype 是可被后续 action 直接读取的函数状态，不是只写给 UI 的文本。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto`：保存函数 prototype，后续 action 通过 `getParam(...)`、`numParams(...)`、`getOutput(...)` 读取。
  - `ProtoParameter` / `ProtoStore`：保存单个输入参数或返回值的 storage。
  - `ParamActive` / `ParamTrial`：候选 trial 被 `deriveInputMap(...)` / `deriveOutputMap(...)` 消费后，结果进入 `FuncProto`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：写入 `FuncProto` 后，继续用 `updateInputTypes(...)` / `updateOutputTypes(...)` 等函数消费 prototype。
  - `ActionInputPrototype::apply(...)`、`ActionOutputPrototype::apply(...)`：会根据已知 prototype 插入或调整参数/返回相关 varnode。

native 侧上一小步已经写出 `notdec.prototype.recovered`，但还缺一个读回接口。后续签名重写、callsite 重写如果都手写 metadata parser，会很容易不一致。

## native 复刻方式

这一步只补读回 API：

- 新增 `readNativeRecoveredPrototypeMetadata(const llvm::Function &)`。
- 从 `notdec.prototype.recovered` 读取：
  - `model=...`
  - 第 4 个 operand 作为 inputs 列表。
  - 第 5 个 operand 作为 returns 列表。
  - 每个参数读取 `name=` 和 `slot=`。
- 如果 metadata 缺失或字段不完整，返回 `std::nullopt`。

这一步不改变 metadata 形状，不改现有推断，不做 LLVM 函数签名重写。

## 判断标准

- `native_prototype_recovery_test` 能读回 input-only、return-only、多 return 的 recovered prototype。
- 空 recovered prototype 仍返回 `std::nullopt`。
- 原有 candidate metadata 测试继续通过。

## 实现记录

### 源码改动

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:9`
  - 前置声明 `llvm::Function`。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:72`
  - 新增 `readNativeRecoveredPrototypeMetadata(const llvm::Function &)`。
- `lib/passes/NativePrototypeRecovery.cpp:17`
  - 引入 `<exception>`，用于解析失败时保守返回 `std::nullopt`。
- `lib/passes/NativePrototypeRecovery.cpp:233`
  - 新增 `parseUint64Field(...)`，读取 `slot=` 这类整数字段。
- `lib/passes/NativePrototypeRecovery.cpp:251`
  - 新增 `readRecoveredParamList(...)`，读取 inputs / returns 子节点。
- `lib/passes/NativePrototypeRecovery.cpp:404`
  - 实现 `readNativeRecoveredPrototypeMetadata(...)`。
  - 缺少 `notdec.prototype.recovered`、缺少 `model=`、inputs/returns 子节点不是 `MDNode`、参数缺少 `name=` 或 `slot=` 时，都返回 `std::nullopt`。
- `tests/native_prototype_recovery_test.cpp:411`
  - 新增 `recoveredPrototypeParamAt(...)`，检查读回后的参数顺序。
- `tests/native_prototype_recovery_test.cpp:548`
  - 覆盖 input-only recovered prototype 读回。
- `tests/native_prototype_recovery_test.cpp:568`
  - 覆盖 return-only recovered prototype 读回。
- `tests/native_prototype_recovery_test.cpp:584`
  - 覆盖多返回寄存器的读回顺序。
- `tests/native_prototype_recovery_test.cpp:602`
  - 覆盖空 recovered prototype 返回 `std::nullopt`。

### 验证

```sh
cmake -S . -B build \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
```

结果：`notdec.native_prototype_recovery.input_candidates` 通过。

### 性能和风险

- 性能：读回 API 只在调用方需要时解析函数 metadata，本次不会增加默认 pass 成本。
- 风险：当前读回依赖 `notdec.prototype.recovered` 的 operand 位置。后续如果 metadata 形状升级，需要同步这个 API，而不是让外部调用方直接解析。
- 实现效果：7/10。recovered prototype 已经能写回和读回，但还没重写 LLVM 函数签名。
- 复杂度：2/10。只加保守 parser，不引入新推断规则。
- 维护成本：2/10。后续统一通过这个 API 读 recovered prototype，可减少重复 parser。
