# 20260529-20 Input Return Signature Rewrite

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的完整 prototype 会同时影响输入参数和返回值。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamActive`：保存 input/output trial。
  - `ProtoModel::deriveInputMap(...)` / `FuncProto::deriveInputMap(...)`：从 input trials 派生输入参数。
  - `ProtoModel::deriveOutputMap(...)` / `FuncProto::deriveOutputMap(...)`：从 output trials 派生返回值。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionInputPrototype::apply(...)`：从 input varnode 注册 trial，并更新 input prototype。
  - `ActionPrototypeTypes::apply(...)`：输入锁定时创建 input varnode；输出未锁定时开启 active output。
  - `ActionReturnRecovery::apply(...)`：检查 `CPUI_RETURN` 上的 output trial，并调用 `deriveOutputMap(...)`。
  - `ActionReturnRecovery::buildReturnOutput(...)`：把恢复出的返回 varnode 接到 `CPUI_RETURN`。
  - `ActionOutputPrototype::apply(...)`：根据 `CPUI_RETURN` 的返回输入更新返回类型。

native 侧已经分别实现了 input-only 和 return-only 的最小签名重写。下一步做一个组合场景：单 input register + 单 return register，把 `void()` 改成 `i64(i64)`，同时替换 input load 和 `ret`。

## native 复刻方式

这一步只处理最小安全场景：

- recovered prototype 必须能形成 `i64(i64)`。
- 原函数必须是 `void()`。
- recovered input 必须只有一个。
- recovered return 必须只有一个。
- 必须能唯一绑定 input load 和 return store。
- 函数必须没有 uses，先不做 callsite 重写。

实现方式：

- 新增 `rewriteNativeRecoveredPrototypeInputReturn(...)`。
- 创建一个新函数，类型用 recovered `FunctionType`。
- 把原函数 basic blocks 移到新函数。
- 用新函数第 0 个 argument 替换绑定的 `external_input` load uses。
- 删除无 uses 的旧 input load。
- 把所有 `ret void` 改成 `ret <return binding value>`。
- 复制函数级 metadata。
- 新函数接管原函数名字，删除旧函数。

这一步不处理 callsite、多参数、多返回寄存器、stack 参数、多路径不同返回值。

## 判断标准

- `void @prototype_rdi_to_rax()` 改成 `i64 @prototype_rdi_to_rax(i64)`。
- 函数体里原 `RDI.external_input` load 的 uses 被新 argument 替换。
- 原 input load 被删除。
- `ret void` 改成 `ret i64 <RAX store value>`。
- 有调用者、缺少 input/return binding、多个参数或多个返回值都不改写。

## 实现记录

已实现。

### 改动

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:135`
  - 新增 `rewriteNativeRecoveredPrototypeInputReturn(...)` 声明。
- `lib/passes/NativePrototypeRecovery.cpp:770`
  - 新增 `rewriteNativeRecoveredPrototypeInputReturn(...)`。
  - 只接受无 uses 的 `void()` 原函数。
  - 只接受 recovered prototype 为单 input + 单 return，且能构造成 `i64(i64)`。
  - 复用 `getNativePrototypeInputBindings(...)` 和 `getNativePrototypeReturnBindings(...)` 做唯一绑定。
  - 创建新函数后 splice 原 basic blocks，用新 argument 替换 `external_input` load，删除无 uses 的旧 load，把 `ret void` 改成 `ret i64 <return value>`。
- `tests/native_prototype_recovery_test.cpp:230`
  - 新增 `createInputReturnFunction(...)` 小样例：读取 RDI external input，计算后写 RAX。
- `tests/native_prototype_recovery_test.cpp:626`
  - 加入 `input_rdi_return_rax` 用例，并更新 summary 计数。
- `tests/native_prototype_recovery_test.cpp:905`
  - 验证组合 rewrite 后函数类型是 `i64(i64)`，旧 input load 被删除，load user 改用新 argument，`ret` 返回原 RAX store value。

### 验证

命令：

```sh
cmake -S . -B build \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON &&
cmake --build build --target native_prototype_recovery_test native_instcombine_metadata_test -j2 &&
ctest --test-dir build -R 'notdec.native_(prototype_recovery.input_candidates|instcombine.metadata)' -V
```

结果：

- `notdec.native_prototype_recovery.input_candidates` 通过。
- `notdec.native_instcombine.metadata` 通过。
- 总计 2 个测试通过。

性能：

- 本次只增加一个显式 helper 和一个单测，不接入默认 pass pipeline，不会影响当前正常 prototype recovery 路径。
- 测试总耗时 0.06 秒，未见性能风险。

### 风险和限制

- 仍然不处理有调用者的函数，callsite rewrite 后再开放。
- 只处理一个输入寄存器和一个返回寄存器。
- 返回值直接来自唯一 RAX store 的 value；多路径、多返回寄存器、stack 参数仍按已有保守逻辑拒绝。

### 评分

- 实现效果：8/10。覆盖了当前最小组合场景。
- 复杂度：7/10。没有新增数据结构，但和现有两个 helper 有重复，后续可以在支持更多场景时再抽公共函数。
- 维护成本：7/10。逻辑直接，限制明确；主要维护点是后续 callsite rewrite 接入时统一入口。
