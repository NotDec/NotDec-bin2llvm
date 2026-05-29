# 20260529-21 Callsite Input Unique Predecessor

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 在 SSA heritage 后，call 参数可以来自 call 前的唯一数据流来源，不要求写参数的 p-code 和 CALL 在同一个基本块。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritage(...)`：为 varnode 建 SSA，跨 basic block 追踪定义。
  - `Heritage::buildInfoList(...)`：收集 block 内和 block 间的 varnode 信息。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionInputPrototype::apply(...)`：基于 SSA 后的 input/active trial 恢复参数。
  - `ActionPrototypeTypes::apply(...)`：把恢复的 prototype 应用到 callsite。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/funcdata.hh`
  - `FuncCallSpecs`：保存 callsite 使用的参数 storage。

native 侧当前 callsite 参数只查 call 同一 basic block 前面的 store。下一步先支持最小跨块形状：call block 只有一个前驱时，从唯一前驱末尾继续反向查参数 store。

## native 复刻方式

这一步只扩展 callsite input 参数查找：

- 仍优先查 call 所在 basic block 内 call 前的同 register store。
- 如果同 block 找不到，且 call block 只有一个前驱，就从该前驱 terminator 前反向查同 register store。
- 只返回类型等于参数类型的 store value。
- 多前驱、找不到 store、类型不匹配仍保守失败。
- 复用已有 input-only 和 input-return direct callsite rewrite，不改其它行为。

暂不做：

- 多前驱 PHI。
- 跨多层前驱递归查找。
- 栈参数。
- 返回值跨块 load 查找。

## 判断标准

- 同 block 参数 store 的旧行为不变。
- call block 有唯一前驱且参数 store 在前驱时，input-only callsite 能重写。
- 多前驱或类型不匹配仍跳过。
- prototype recovery 单测通过。

## 实现记录

已实现。

### 改动位置

- `lib/passes/NativePrototypeRecovery.cpp:202` 新增 `registerStoreValueInReverseRange(...)`，复用同 register store 反向查找。
- `lib/passes/NativePrototypeRecovery.cpp:227` 调整 `callsiteInputValueBeforeCall(...)`，先查 call 所在 block，再在唯一前驱中查参数 store。
- `tests/native_prototype_recovery_test.cpp:180` 新增 `createInputStoreUniquePredecessorCallerFunction(...)`，构造前驱写 RDI、后继 call 的 caller。
- `tests/native_prototype_recovery_test.cpp:981` 新增 input-only unique predecessor callsite 测试，验证参数来自前驱 store 时也能重写。

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
- 开启 direct callsite rewrite 时，每个 call 最多查本 block 和一个唯一前驱。
- 多前驱仍保守失败，不生成 PHI。
- 不递归跨多层前驱，不处理栈参数，也不处理返回值跨块 load。

### 评分

- 实现效果：7/10。覆盖了常见 split block 参数准备形状。
- 复杂度：8/10。只增加一个范围查找 helper和唯一前驱判断。
- 维护成本：8/10。后续要支持多前驱时可以在这个入口扩展 PHI 逻辑。
