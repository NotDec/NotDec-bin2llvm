# 20260529-18 Return-only Signature Rewrite

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的返回值恢复最终会改到函数返回语义上。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：输出没有锁定时调用 `data.initActiveOutput()`，开始收集返回值。
  - `ActionReturnRecovery::apply(...)`：遍历 `CPUI_RETURN`，检查 output trial 是否真实参与返回，最后调用 `deriveOutputMap(...)`。
  - `ActionReturnRecovery::buildReturnOutput(...)`：把恢复出的返回 varnode 加到 `CPUI_RETURN` 上。
  - `ActionOutputPrototype::apply(...)`：从 `CPUI_RETURN` 的返回输入更新输出类型。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ProtoModel::deriveOutputMap(...)`：从 output trials 里选择最终返回值。
  - `FuncProto::deriveOutputMap(...)`：把 active output 收敛到当前函数 prototype。

native 侧已经有 recovered return metadata，也能绑定唯一的返回寄存器 store。现在可以做最小闭环：把 `void()` 函数改成 `i64()`，并把 `ret void` 改成 `ret <store value>`。

## native 复刻方式

这一步只处理最小安全场景：

- recovered prototype 必须能形成 `i64()`。
- 原函数必须是 `void()`。
- recovered input 必须为空。
- recovered return 必须只有一个。
- 必须能唯一绑定 return store。
- 函数必须没有 uses，先不做 callsite 重写。

实现方式：

- 新增 `NativePrototypeRewriteResult`，记录是否改写和原因。
- 新增 `rewriteNativeRecoveredPrototypeReturnOnly(...)`。
- 创建一个新函数，类型用 recovered `FunctionType`。
- 把原函数 basic blocks 移到新函数。
- 把所有 `ret void` 改成 `ret <return binding value>`。
- 复制函数级 metadata。
- 新函数接管原函数名字，删除旧函数。

这一步不处理参数、不处理调用点、不处理多返回路径不同值、不处理多返回寄存器。

## 判断标准

- `void @return_rax()` 改成 `i64 @return_rax()`。
- 函数体里的 `ret void` 改成 `ret i64 <RAX store value>`。
- 原函数没有 uses 时可以改写。
- 有 input 参数候选、多个返回 store、已有 uses 的函数都不改写。

## 实现记录

### 源码改动

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:85`
  - 新增 `NativePrototypeRewriteResult`，记录是否改写、原因和改写后的函数。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:129`
  - 新增 `rewriteNativeRecoveredPrototypeReturnOnly(...)`。
- `lib/passes/NativePrototypeRecovery.cpp:13`
  - 引入 `IRBuilder`，用于替换 `ret void`。
- `lib/passes/NativePrototypeRecovery.cpp:614`
  - 实现 `rewriteNativeRecoveredPrototypeReturnOnly(...)`。
  - 只允许无 uses、原型为 `void()`、recovered prototype 为 `i64()`、无 recovered input、单 recovered return、且唯一 return binding 的函数改写。
  - 创建新函数并接管原函数名，用 `Function::splice(...)` 搬 basic blocks。
  - 将 `ret void` 改成 `ret i64 <return store value>`。
  - 复制函数属性和函数级 metadata，最后删除旧函数。
- `tests/native_prototype_recovery_test.cpp:118`
  - 新增 `createCallerFunction(...)`，构造有调用者的负例。
- `tests/native_prototype_recovery_test.cpp:573`
  - 新增 `return_rax_used` 和调用者，验证有 uses 时不改写。
- `tests/native_prototype_recovery_test.cpp:788`
  - 验证有调用者的 return-only 函数不改写，原因是 `function has uses`。
- `tests/native_prototype_recovery_test.cpp:795`
  - 验证无调用者的 `return_rax` 能改成 `i64()`。
  - 验证新 `ret` 返回的是原 `RAX` store 的 value operand。
  - 改写后再次运行 LLVM verifier。

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

- 性能：helper 只有显式调用时才做检查、创建新函数和搬 basic blocks，当前默认 pass 不调用，主链路没有额外开销。
- 风险：这一步故意拒绝有 uses 的函数，callsite 重写还没接上；也不处理 input 参数、多返回寄存器、多路径不同返回值。
- 实现效果：7/10。已经打通 return-only 的真实 LLVM 函数签名和 `ret` 改写，但还不是完整 prototype rewrite。
- 复杂度：4/10。开始改 IR 结构，但范围只限无调用者 return-only 函数。
- 维护成本：4/10。后续需要把参数替换和 callsite 重写纳入同一套 rewrite 流程。
