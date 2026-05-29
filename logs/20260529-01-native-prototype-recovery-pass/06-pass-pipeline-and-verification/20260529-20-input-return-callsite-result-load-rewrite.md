# 20260529-20 Input-return Callsite Result Load Rewrite

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 在一个 callsite 上同时应用输入参数和返回值 prototype。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionInputPrototype::apply(...)`：恢复输入 storage。
  - `ActionReturnRecovery::apply(...)`：恢复返回 storage。
  - `ActionOutputPrototype::apply(...)`：更新输出 prototype。
  - `ActionPrototypeTypes::apply(...)`：把输入和返回类型应用到 callsite。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto::deriveInputMap(...)`：形成输入参数列表。
  - `FuncProto::deriveOutputMap(...)`：形成返回参数列表。
  - `FuncProto::updateAllTypes(...)`：把 prototype 同步到函数体和调用点。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/funcdata.hh`
  - `FuncCallSpecs`：保存每个 callsite 的参数和返回约定。

native 侧当前 input-return direct callsite 已能把旧 void call 改成 `i64(i64)`，但返回值还 unused。上一小步已经为 return-only 接上了 call 后 RAX load；这一步把同样策略接到 input-return。

## native 复刻方式

这一步只做 input-return direct callsite 的最小结果接线：

- 复用 return-only 的策略：在旧 void call 后、同一 basic block 内向后找第一个同 register 的 `notdec.register.access` load。
- 如果 load 类型等于新 call result 类型，就把 load uses 替换成新 call result，并删除空 load。
- call 参数仍来自 call 前最近的 input register store。
- 找不到返回值 load 时仍允许重写 callsite，保持上一小步行为。

暂不做：

- 跨 basic block 查找返回值使用。
- 多参数或多返回值。
- indirect call。
- call 后 store 到 RAX 的替换。

## 判断标准

- 旧 input-return direct callsite rewrite 行为不变。
- call 后有同 block RAX load 时，该 load 的使用改用新 `i64(i64)` call result。
- 找不到 RAX load 时仍能生成返回 i64 的 call。
- prototype recovery 单测通过。

## 实现记录

已实现。

### 改动位置

- `lib/passes/NativePrototypeRecovery.cpp:270` 新增 `rewriteCallsiteReturnLoad(...)`，把 call 后同 block 的同 register load uses 替换成新 call result。
- `lib/passes/NativePrototypeRecovery.cpp:297` 的 `rewriteInputReturnDirectCallsites(...)` 增加 `returnRegisterName`，并调用 `rewriteCallsiteReturnLoad(...)`。
- `lib/passes/NativePrototypeRecovery.cpp:325` 的 `rewriteReturnOnlyDirectCallsites(...)` 改为复用同一个 `rewriteCallsiteReturnLoad(...)`。
- `lib/passes/NativePrototypeRecovery.cpp:937` 的 `rewriteNativeRecoveredPrototypeInputReturn(...)` 把 recovered return register 传给 callsite rewrite。
- `tests/native_prototype_recovery_test.cpp:180` 新增 `createInputStoreReturnLoadCallerFunction(...)`，构造 call 前写 RDI、call 后读 RAX 的 caller。
- `tests/native_prototype_recovery_test.cpp:1196` 更新 input-return callsite 测试，验证旧 RAX load 被删除，新 call result 有 use。

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
- 开启 input-return direct callsite rewrite 时，只在每个 direct call 所在 basic block 内向后扫描到第一个匹配 load。
- 找不到 load 时保持上一小步行为：仍生成返回 i64 的 call。
- 当前不跨 basic block，不处理 PHI，不处理 call 后 store 到 RAX，不处理多参数或多返回值。

### 评分

- 实现效果：8/10。input-return callsite 的参数和返回值使用都接到了 LLVM call。
- 复杂度：7/10。抽出了返回值 load 重写 helper，减少 return-only 和 input-return 重复。
- 维护成本：7/10。后续跨 block 和 PHI 场景仍需要更完整的数据流处理。
