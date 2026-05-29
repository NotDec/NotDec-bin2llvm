# 20260529-18 Input-return Direct Callsite Rewrite

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的 callsite 类型更新不是单独处理输入或输出，而是把 `FuncProto` 同步到每个 call spec。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionInputPrototype::apply(...)`：恢复输入 storage。
  - `ActionReturnRecovery::apply(...)`：恢复返回 storage。
  - `ActionOutputPrototype::apply(...)`：更新输出 prototype。
  - `ActionPrototypeTypes::apply(...)`：把最终 prototype 应用到 call、return 和 varnode 类型。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto::deriveInputMap(...)`：根据 active input trials 形成参数列表。
  - `FuncProto::deriveOutputMap(...)`：根据 output trials 形成返回列表。
  - `FuncProto::updateAllTypes(...)`：把 prototype 传播到函数体和调用点。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/funcdata.hh`
  - `FuncCallSpecs`：保存 callsite 使用的 prototype 和参数信息。

native 侧已经分别支持 input-only 和 return-only 的直接调用点。下一步复刻最小 input + return 形状，让 `void()` callee 改成 `i64(i64)` 时，caller 里的 direct call 也带上参数并返回 i64。

## native 复刻方式

这一步只做单 input + 单 return 的直接调用：

- 只处理 callee recovered prototype 是一个 input、一个 return。
- 只处理 callee 当前类型是 `void()`，恢复类型是 `i64(i64)`。
- 只处理所有 use 都是直接 `CallInst`，且 call 当前无参数、返回 void。
- call 参数复用 input-only 的策略：从 call 前最近的同 register `notdec.register.access` store 取值。
- callee body 仍用已有 input-return rewrite，把 input load 替换成 argument，把 return block 改成返回 recovered value。
- callsite 把旧 void call 替换成新 `i64(i64)` call；返回值暂时允许 unused。

暂不做：

- call 后 RAX store / load 到 LLVM call result 的替换。
- 多参数。
- 多返回值。
- indirect call。

## 判断标准

- 无调用者 input-return rewrite 行为不变。
- 一个直接调用者在 call 前写 RDI 时，callee 能改成 `i64(i64)`，caller 里的 call 带一个参数并返回 i64。
- 找不到参数值或 use 不是直接 call 时仍跳过。
- prototype recovery 单测通过。

## 实现记录

已实现。

### 改动位置

- `lib/passes/NativePrototypeRecovery.cpp:270` 新增 `rewriteInputReturnDirectCallsites(...)`，把旧 void call 替换成调用新 `i64(i64)` callee 的 call。
- `lib/passes/NativePrototypeRecovery.cpp:904` 调整 `rewriteNativeRecoveredPrototypeInputReturn(...)`，callee 有 use 时先尝试收集 direct callsite 的 input 参数；失败仍返回 `function has uses`。
- `lib/passes/NativePrototypeRecovery.cpp:960` 复用 `collectInputOnlyDirectCallsiteRewrites(...)`，从 call 前同 register store 取参数值。
- `lib/passes/NativePrototypeRecovery.cpp:1002` 在函数体重写后同步重写 input-return direct callsite。
- `tests/native_prototype_recovery_test.cpp:1108` 新增独立 input-return callsite 样例，验证 callee 类型变成 `i64(i64)`，caller 里的 call 带一个参数并返回 i64。

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
- 开启 input-return rewrite 且函数有 use 时，会遍历 callee users，并在每个 direct call 所在 basic block 内向前查参数 store。
- 当前只处理同一 basic block 内 call 前最近 store，不跨前驱，不处理 PHI。
- 新 call 的返回值暂时允许 unused，尚未替换 call 后 RAX store/load。
- 多参数、多返回值、indirect call 仍保守跳过。

### 评分

- 实现效果：7/10。打通了最小 input-return 直接调用点重写。
- 复杂度：7/10。复用了 input-only 的参数查找，新增逻辑较少。
- 维护成本：7/10。后续应把 input-only 和 input-return 的 callsite rewrite 合并，避免重复。
