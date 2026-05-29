# 20260529-16 Input Load Bindings

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的输入参数不是只停在 prototype 列表里。它能把 prototype 里的参数和函数体里的 input varnode 对上。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamActive`：保存 `ParamTrial` 列表。
  - `ParamActive::registerTrial(...)`：把 input varnode 的 storage 加入 trial。
  - `ParamActive::getTrialForInputVarnode(...)`：按 CALL/input slot 找回对应 trial。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionInputPrototype::apply(...)`：遍历 `Varnode::input`，对可能是参数的 varnode 调 `registerTrial(...)`，再 `deriveInputMap(...)`。
  - `ActionPrototypeTypes::apply(...)`：当输入 prototype 已锁定时，用 `setInputVarnode(...)` 强制创建输入 varnode，并标记 locked input。

native 侧现在已经能得到 `NativeRecoveredPrototype::Inputs`，但还没有把这些 input register 和 LLVM IR 里的 `notdec.register.external_input` load 对上。真正改函数签名前，必须先知道哪个 load 后续可以替换成新 argument。

## native 复刻方式

这一步只做只读绑定，不改 IR：

- 新增 `NativePrototypeInputBinding`：
  - recovered input 参数。
  - 对应的唯一 `llvm::LoadInst *`。
- 新增 `getNativePrototypeInputBindings(...)`：
  - 读取 `notdec.prototype.recovered`。
  - 对每个 recovered input register，在函数体里查 `notdec.register.external_input` 且 `name=` 相同的 load。
  - 找不到或找到多个都返回 `std::nullopt`，先保守失败。
  - 全部 input 都能唯一匹配时返回 ordered bindings。

这一步对应 Ghidra “prototype 参数和 input varnode 能对上”的关系。后续签名重写会用这个 binding 把 load 的 uses 替换成 LLVM argument。

## 判断标准

- 带唯一 `RDI.external_input` load 的函数，能返回 1 个 binding。
- binding 的 recovered param 是 `RDI`，slot 是 0。
- binding 指向的 load 是函数体里的同一个 `RDI.external_input` load。
- 没有 `external_input` load 的函数即使有 recovered input，也暂时不返回 binding。
- 同名 `external_input` load 超过一个时暂时不返回 binding。

## 实现记录

### 源码改动

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:10`
  - 前置声明 `llvm::LoadInst`。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:66`
  - 新增 `NativePrototypeInputBinding`。
  - 保存 recovered input 参数和对应的 `external_input` load。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:106`
  - 新增 `getNativePrototypeInputBindings(...)`。
- `lib/passes/NativePrototypeRecovery.cpp:142`
  - 新增 `uniqueExternalInputLoad(...)`。
  - 按 `notdec.register.external_input` 的 `name=` 字段查找唯一 load。
  - 找不到或同名 load 超过一个都返回 `std::nullopt`。
- `lib/passes/NativePrototypeRecovery.cpp:533`
  - 实现 `getNativePrototypeInputBindings(...)`。
  - 读取 `notdec.prototype.recovered`，按 recovered input 顺序绑定每个 input register。
  - 这一步只返回绑定，不替换 load，不改函数签名。
- `tests/native_prototype_recovery_test.cpp:10`
  - 引入 `llvm/IR/Instructions.h`，测试里直接检查 `LoadInst *`。
- `tests/native_prototype_recovery_test.cpp:141`
  - 新增 `createExternalInputLoad(...)`。
- `tests/native_prototype_recovery_test.cpp:157`
  - 新增 `createUsedExternalInputFunction(...)`，构造唯一可绑定的 `RDI.external_input` load。
- `tests/native_prototype_recovery_test.cpp:174`
  - 新增 `createDuplicateExternalInputLoadFunction(...)`，覆盖同名 load 重复时保守失败。
- `tests/native_prototype_recovery_test.cpp:517`
  - 接入 `input_rdi_bindable` 和 `input_rdi_duplicate_load` 两个测试函数。
- `tests/native_prototype_recovery_test.cpp:678`
  - 验证唯一 load 能绑定，参数名是 `RDI`、slot 是 0、load 指针是原 load。
- `tests/native_prototype_recovery_test.cpp:694`
  - 验证没有 load 或重复 load 时不返回 binding。

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

- 性能：helper 显式调用时按函数指令线性扫描，并按 recovered input 数重复扫描。当前 pass 默认不调用，主链路没有额外开销。
- 风险：重复同名 load 先保守失败，后续如果 Register SSA 会产生多个等价入口 load，需要再补更精确的 SSA 绑定或先 canonicalize。
- 实现效果：6/10。已经能把 recovered input 和唯一入口 load 对上，但还没有用 argument 替换 load。
- 复杂度：2/10。只读 metadata 和指令。
- 维护成本：3/10。后续真正签名重写会直接消费 binding；多 load 场景需要再扩展。
