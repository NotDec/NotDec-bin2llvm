# 20260529-17 Return Store Bindings

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 会把恢复出的 output trial 接回 `CPUI_RETURN`，而不是只记录候选。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ProtoModel::deriveOutputMap(...)`：从 output trials 里选择最终返回值。
  - `FuncProto::deriveOutputMap(...)`：把当前函数的 active output 交给 model 处理。
  - `FuncCallSpecs::initActiveOutput()`：开启返回值恢复。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：输出未锁定时调用 `data.initActiveOutput()`。
  - `ActionReturnRecovery::apply(...)`：检查 `CPUI_RETURN` 的输出 trial，最终调用 `deriveOutputMap(...)`。
  - `ActionReturnRecovery::buildReturnOutput(...)`：把恢复出的返回 varnode 加到 `CPUI_RETURN` 输入里。
  - `ActionOutputPrototype::apply(...)`：根据 `CPUI_RETURN` 上的返回输入更新输出类型。

native 侧已经能把 return candidate 收进 `NativeRecoveredPrototype::Returns`。下一步真正改 LLVM 返回类型时，需要知道这个 recovered return 对应哪条寄存器 store，以及 store 的 value operand 是什么。

## native 复刻方式

这一步只做只读绑定，不改 IR：

- 新增 `NativePrototypeReturnBinding`：
  - recovered return 参数。
  - 对应唯一 `llvm::StoreInst *`。
  - store 的 value operand。
- 新增 `getNativePrototypeReturnBindings(...)`：
  - 读取 `notdec.prototype.recovered`。
  - 对每个 recovered return register，在函数体里查 `notdec.register.access` 且 `name=` 相同的 store。
  - 找不到或找到多个都返回 `std::nullopt`，先保守失败。
  - 全部 return 都能唯一匹配时返回 ordered bindings。

这一步对应 Ghidra “recovered output trial 能接回 RETURN varnode”的关系。后续签名重写会用这个 binding 改 `ret` 指令。

## 判断标准

- 单返回 `RAX` 的函数能返回 1 个 binding。
- binding 的 recovered return 是 `RAX`，slot 是 0。
- binding 指向的 store 是函数体里的唯一 `RAX` store。
- binding 的 return value 是该 store 的 value operand。
- 同一返回 register 有多条 store 时暂时不返回 binding。

## 实现记录

### 源码改动

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:14`
  - 前置声明 `llvm::StoreInst` 和 `llvm::Value`。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:76`
  - 新增 `NativePrototypeReturnBinding`。
  - 保存 recovered return 参数、对应 register store 和 store 的 value operand。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:120`
  - 新增 `getNativePrototypeReturnBindings(...)`。
- `lib/passes/NativePrototypeRecovery.cpp:171`
  - 新增 `uniqueRegisterAccessStore(...)`。
  - 按 `notdec.register.access` 的 `name=` 字段查找唯一 store。
  - 找不到或同名 store 超过一个都返回 `std::nullopt`。
- `lib/passes/NativePrototypeRecovery.cpp:587`
  - 实现 `getNativePrototypeReturnBindings(...)`。
  - 读取 `notdec.prototype.recovered`，按 recovered return 顺序绑定每个 return register。
  - 这一步只返回绑定，不改 `ret` 指令，不改函数签名。
- `tests/native_prototype_recovery_test.cpp:191`
  - `createReturnStoreFunction(...)` 支持可选返回创建的 `StoreInst *`。
- `tests/native_prototype_recovery_test.cpp:554`
  - 保存 `return_rax` 的唯一 `RAX` store，用于检查 binding 是否指向同一条 store。
- `tests/native_prototype_recovery_test.cpp:749`
  - 验证单返回 `RAX` 能绑定，参数名是 `RAX`、slot 是 0、store 和 value operand 都正确。
- `tests/native_prototype_recovery_test.cpp:767`
  - 验证同一返回 register 有多条 store 时不返回 binding。

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

- 性能：helper 显式调用时按函数指令线性扫描，并按 recovered return 数重复扫描。当前 pass 默认不调用，主链路没有额外开销。
- 风险：多 return path 或同一寄存器多 store 暂时保守失败。后续真正改返回类型时，需要先处理按 return block 绑定 value 的场景。
- 实现效果：6/10。已经能把单返回 recovered return 和唯一 register store 对上，但还没改 LLVM `ret`。
- 复杂度：2/10。只读 metadata 和指令。
- 维护成本：3/10。后续 return rewrite 可以复用 binding；多路径返回需要扩展。
