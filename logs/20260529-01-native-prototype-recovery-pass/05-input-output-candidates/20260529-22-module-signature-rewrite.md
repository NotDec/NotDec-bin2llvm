# 20260529-22 Module Signature Rewrite

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 在一个函数的 decompile action pipeline 内更新 `FuncProto`，再把恢复出的输入、输出应用到函数和相关调用点。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionInputPrototype::apply(...)`：恢复输入参数候选。
  - `ActionReturnRecovery::apply(...)`：恢复返回候选。
  - `ActionOutputPrototype::apply(...)`：更新输出 prototype。
  - `ActionPrototypeTypes::apply(...)`：在 prototype 已知后把类型应用到 varnode / call / return。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto::deriveInputMap(...)` / `FuncProto::deriveOutputMap(...)`：维护最终输入/输出 storage。
  - `FuncProto::updateAllTypes(...)`：把 prototype 结果应用到当前函数。

native 侧前面已经有单函数统一 rewrite helper。下一步做 module 级显式 helper，把当前 module 里所有支持形状的无调用者函数批量改写，为后续接 pass option 做准备。

## native 复刻方式

这一步仍然不接默认 pass pipeline，只新增显式 API：

- 新增 `NativePrototypeModuleRewriteSummary`。
- 新增 `rewriteNativeRecoveredPrototypes(llvm::Module &module)`。
- 先把 module 里的函数指针收集到 vector，再逐个调用 `rewriteNativeRecoveredPrototype(...)`，避免 rewrite 时删除旧函数影响遍历。
- 统计：
  - `FunctionsSeen`
  - `FunctionsRewritten`
  - `FunctionsSkipped`
- 跳过原因暂时不做详细 map，后续接 CLI 报告时再补。

这一步不做 callsite rewrite、不处理有调用者函数、不接 `runNativePrototypeRecovery(...)` option。

## 判断标准

- module 中多个无调用者、当前支持形状的 recovered prototype 可以一次改写。
- 缺失 recovered prototype、多返回、有调用者等函数被统计为 skipped。
- 改写后 module 通过 LLVM verifier。

## 实现记录

已实现。

### 改动

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:91`
  - 新增 `NativePrototypeModuleRewriteSummary`，记录 `FunctionsSeen`、`FunctionsRewritten`、`FunctionsSkipped`。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:149`
  - 新增 `rewriteNativeRecoveredPrototypes(llvm::Module &module)` 声明。
- `lib/passes/NativePrototypeRecovery.cpp:896`
  - 新增 `rewriteNativeRecoveredPrototypes(...)`。
  - 先把 module 中的函数指针收集到 `std::vector`，再逐个调用 `rewriteNativeRecoveredPrototype(...)`。
  - 改写成功计入 `FunctionsRewritten`，其余计入 `FunctionsSkipped`。
- `tests/native_prototype_recovery_test.cpp:1064`
  - 新增独立 `batchModule`，包含 input-only、return-only、input+return、有调用者 input-only、缺失 recovered prototype 这几类函数。
- `tests/native_prototype_recovery_test.cpp:1094`
  - 验证批量 helper 看到 6 个函数，改写 3 个，跳过 3 个。
  - 验证三类支持形状的函数类型被改成 `void(i64)`、`i64()`、`i64(i64)`。
  - 验证有调用者的函数未被改写。

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

清理一个未使用变量警告后复跑：

```sh
cmake --build build --target native_prototype_recovery_test native_instcombine_metadata_test -j2 &&
ctest --test-dir build -R 'notdec.native_(prototype_recovery.input_candidates|instcombine.metadata)' -V
```

结果：

- `notdec.native_prototype_recovery.input_candidates` 通过。
- `notdec.native_instcombine.metadata` 通过。
- 总计 2 个测试通过。

性能：

- 只新增显式 helper，默认 pass pipeline 还不调用。
- helper 对 module 做一次函数指针收集，再线性调用单函数 rewrite。
- 测试总耗时 0.06 秒。

### 风险和限制

- 仍然不处理 callsite rewrite，所以有 uses 的函数会被跳过。
- skip 原因还没有分项统计，后续接 CLI 报告时再补。
- 还没有接入 `runNativePrototypeRecovery(...)` option。

### 评分

- 实现效果：7/10。批量改写入口已经可用，但还不是默认 pass 行为。
- 复杂度：3/10。只是 vector 收集后调用现有 helper。
- 维护成本：4/10。后续主要扩展点是 skip 原因统计和 pass option。
