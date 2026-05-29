# 20260529-17 Return-only Direct Callsite Rewrite

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 在 return prototype 明确后，会把 callee prototype 和 callsite 同步更新。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionReturnRecovery::apply(...)`：从输出 trial 恢复返回 storage。
  - `ActionOutputPrototype::apply(...)`：把返回 storage 写回函数 prototype。
  - `ActionPrototypeTypes::apply(...)`：让 call、return 和 varnode 类型使用最终 prototype。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto::deriveOutputMap(...)`：根据输出 trial 形成返回参数列表。
  - `FuncProto::updateAllTypes(...)`：更新函数体和调用点类型。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/funcdata.hh`
  - `FuncCallSpecs`：保存每个 callsite 对 callee prototype 的使用方式。

native 侧目前 return-only helper 只支持无调用者。下一步先支持最小的直接调用点，让 `void()` callee 改成 `i64()` 时，caller 里的直接 call 也能改成返回 i64 的 call。

## native 复刻方式

这一步只做 return-only 直接调用：

- 只处理 callee recovered prototype 是无 input、单 return。
- 只处理 callee 当前类型是 `void()`，恢复类型是 `i64()`。
- 只处理所有 use 都是直接 `CallInst`，且 call 当前无参数、返回 void。
- callee body 仍使用已有 return-only 重写，把 return block 改成返回 recovered value。
- callsite 只把旧 void call 替换成新 i64 call；返回值暂时允许 unused。

暂不做：

- call 后 RAX store / load 到 LLVM call result 的替换。
- input-return callsite rewrite。
- 多返回值。
- indirect call。

## 判断标准

- 无调用者 return-only rewrite 行为不变。
- 一个直接调用者调用 return-only callee 时，callee 能改成 `i64()`，caller 里的 call 也改成返回 i64。
- use 不是直接 call 时仍跳过。
- prototype recovery 单测通过。

## 实现记录

已实现。

### 改动位置

- `lib/passes/NativePrototypeRecovery.cpp:270` 新增 `collectReturnOnlyDirectCallsites(...)`，只接受直接 `CallInst`、无参数、void 返回的 callsite。
- `lib/passes/NativePrototypeRecovery.cpp:284` 新增 `rewriteReturnOnlyDirectCallsites(...)`，把旧 void call 替换成调用新 `i64()` callee 的 call。
- `lib/passes/NativePrototypeRecovery.cpp:720` 调整 `rewriteNativeRecoveredPrototypeReturnOnly(...)`，callee 有 use 时先尝试最小 direct callsite rewrite；失败仍返回 `function has uses`。
- `tests/native_prototype_recovery_test.cpp:965` 把已有直接调用者样例改成应成功重写。
- `tests/native_prototype_recovery_test.cpp:977` 新增独立 return-only callsite 样例，验证 callee 类型变成 `i64()`，caller 里的 call 也返回 i64。

### 验证

命令：

```sh
cmake -S . -B build \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON &&
cmake --build build --target native_prototype_recovery_test -j2 &&
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
```

结果：初次运行旧断言失败，因为旧测试仍要求 return-only 有直接调用者时跳过；更新为新预期后，1 个测试通过。

### 性能和风险

- 默认不开签名重写时不走这段逻辑。
- 开启 return-only rewrite 且函数有 use 时，会遍历 callee users；当前只接受无参数、void 返回的 direct call。
- 新 call 的返回值暂时允许 unused，尚未替换 call 后寄存器 store/load。
- indirect call、多返回值、input-return callsite 仍保守跳过。

### 评分

- 实现效果：7/10。打通了最小 return-only 直接调用点重写。
- 复杂度：8/10。逻辑比 input-only 更简单，只改 call 类型。
- 维护成本：7/10。后续仍需把 call result 接到 RAX 使用点，才能更接近 Ghidra 的 call spec 类型传播。
