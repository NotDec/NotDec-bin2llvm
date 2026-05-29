# 20260529-16 Input-only Direct Callsite Rewrite

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 在恢复 callee prototype 后，会让 call spec 和函数 prototype 一起更新。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionInputPrototype::apply(...)`：恢复当前函数的输入参数。
  - `ActionPrototypeTypes::apply(...)`：把 `FuncProto` 应用到 call、return 和相关 varnode 类型。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto::deriveInputMap(...)`：从 active input trials 得到输入 storage。
  - `FuncProto::updateAllTypes(...)`：把 prototype 结果同步到函数体类型。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/funcdata.hh`
  - `FuncCallSpecs`：保存一个 callsite 使用的 prototype 和参数信息。

native 侧当前已经能把无调用者的 `void()` 改成 `void(i64)`，但有调用者时为了避免 callsite 类型不匹配会保守跳过。下一步先支持最小的直接调用场景。

## native 复刻方式

这一步只做 input-only 直接调用：

- 只处理 callee 的 recovered prototype 是单个 input、无 return。
- 只处理 callee 当前类型是 `void()`，恢复类型是 `void(i64)`。
- 只处理所有 use 都是直接 `CallInst`，且 call 当前无参数、返回 void。
- 重写前先把每个 call 改成带一个参数。参数值从 call 前最近的同 register `notdec.register.access` store 读取。
- 找不到 call 前参数 store、store value 类型不匹配、use 不是直接 call 时仍跳过。
- callee body 仍复用已有 input-only 函数签名重写逻辑。

暂不做：

- 返回值 callsite rewrite。
- 多参数。
- indirect call。
- call 后 register store 到 LLVM 返回值的替换。

## 判断标准

- 旧的无调用者 input-only rewrite 行为不变。
- 一个直接调用者在 call 前写 RDI 时，callee 能改成 `void(i64)`，callsite 能带上该 RDI 值。
- 找不到 callsite 参数值时仍保守跳过。
- prototype recovery 单测通过。

## 实现记录

已实现。

### 改动位置

- `lib/passes/NativePrototypeRecovery.cpp:202` 新增 `callsiteInputValueBeforeCall(...)`，从 call 前反向查同 register 的 `notdec.register.access` store。
- `lib/passes/NativePrototypeRecovery.cpp:228` 新增 `NativeInputOnlyCallsiteRewrite`，保存待改写 call 和实参值。
- `lib/passes/NativePrototypeRecovery.cpp:233` 新增 `collectInputOnlyDirectCallsiteRewrites(...)`，只接受直接 `CallInst`、无参数、void 返回的 callsite。
- `lib/passes/NativePrototypeRecovery.cpp:258` 新增 `rewriteInputOnlyDirectCallsites(...)`，把旧 call 替换成带一个参数的新 call。
- `lib/passes/NativePrototypeRecovery.cpp:774` 调整 `rewriteNativeRecoveredPrototypeInputOnly(...)`，在 callee 有 use 时先尝试最小 direct callsite rewrite；失败仍返回 `function has uses`。
- `tests/native_prototype_recovery_test.cpp:133` 新增 `createInputStoreCallerFunction(...)` 测试辅助函数。
- `tests/native_prototype_recovery_test.cpp:848` 新增独立 callsite rewrite 样例，验证 callee 类型变成 `void(i64)`，callsite 带一个参数。

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

结果：1 个测试通过。

### 性能和风险

- 默认不开签名重写时不走这段逻辑。
- 开启 input-only rewrite 且函数有 use 时，会遍历 callee users，并在每个 call 所在 block 内向前查 store；当前只用于最小 direct callsite 场景。
- 目前只查同一 basic block 内 call 前最近 store，不跨前驱，不处理 PHI、多参数、返回值和 indirect call。
- 找不到参数值、类型不匹配、use 不是 direct call 时仍保守跳过。

### 评分

- 实现效果：7/10。打通了最小直接调用 input-only 重写。
- 复杂度：7/10。新增了 callsite 收集和替换，但还局限在单参数无返回。
- 维护成本：7/10。后续多参数和返回值重写需要把 callsite value 查找抽成更通用的逻辑。
