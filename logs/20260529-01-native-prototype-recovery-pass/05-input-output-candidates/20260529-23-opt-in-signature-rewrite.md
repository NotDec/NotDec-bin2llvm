# 20260529-23 Opt-in Signature Rewrite

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的 prototype recovery 在 action pipeline 中逐步产出 prototype，然后后续 action 使用这个 prototype 更新函数内 varnode 和 return/call 类型。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionInputPrototype::apply(...)`：恢复输入参数。
  - `ActionReturnRecovery::apply(...)`：恢复返回候选。
  - `ActionOutputPrototype::apply(...)`：更新输出 prototype。
  - `ActionPrototypeTypes::apply(...)`：把已经恢复的 prototype 应用到当前函数内的类型和调用相关节点。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto::deriveInputMap(...)` / `FuncProto::deriveOutputMap(...)`：保存最终 prototype。
  - `FuncProto::updateAllTypes(...)`：把 prototype 结果应用到函数分析结果。

native 侧已经能恢复 metadata，也已经有显式 module 级 rewrite helper。下一步先给 `runNativePrototypeRecovery(...)` 加一个默认关闭的 opt-in 选项。这样测试和后续 CLI 可以选择开启，默认 pipeline 仍只写 metadata。

## native 复刻方式

这一步只做库层 opt-in：

- `NativePrototypeRecoveryOptions` 新增 `RewriteSignatures = false`。
- `NativePrototypeRecoverySummary` 增加 module rewrite 的三个计数字段。
- `runNativePrototypeRecovery(...)` 在 metadata 全部写完后，如果 `RewriteSignatures` 为 true，就调用 `rewriteNativeRecoveredPrototypes(module)`。
- 默认 `false`，所以现有 CLI 和默认测试行为不变。

这一步不新增 CLI 参数、不做 callsite rewrite、不改变已有默认输出。

## 判断标准

- 默认 options 下只写 metadata，不改函数签名。
- `RewriteSignatures = true` 时，支持的无调用者函数在同一次 `runNativePrototypeRecovery(...)` 后被改写。
- summary 能记录本次 rewrite seen / rewritten / skipped。
- module 通过 LLVM verifier。

## 实现记录

已实现。

### 改动

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:21`
  - `NativePrototypeRecoveryOptions` 新增 `RewriteSignatures`，默认 `false`。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:116`
  - `NativePrototypeRecoverySummary` 新增签名重写统计：
    - `SignatureRewriteFunctionsSeen`
    - `SignatureRewriteFunctionsRewritten`
    - `SignatureRewriteFunctionsSkipped`
- `lib/passes/NativePrototypeRecovery.cpp:469`
  - `runNativePrototypeRecovery(...)` 在 metadata 写完后，如果 `options.RewriteSignatures` 为 true，调用 `rewriteNativeRecoveredPrototypes(module)`。
  - 把 module rewrite summary 写回 recovery summary。
- `lib/passes/NativePrototypeRecovery.cpp:940`
  - `printNativePrototypeRecoverySummary(...)` 输出新增签名重写计数。
- `tests/native_prototype_recovery_test.cpp:667`
  - 验证默认 options 不会执行签名重写，新增计数保持 0。
- `tests/native_prototype_recovery_test.cpp:1141`
  - 新增 opt-in module，用 `RewriteSignatures = true` 验证同一次 recovery 后完成 `i64(i64)` 签名改写。
  - 验证 opt-in summary 看到 2 个函数、改写 1 个、跳过 1 个。

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

- 默认 `RewriteSignatures = false`，现有 CLI 默认路径不做额外 rewrite。
- opt-in 时在已有 metadata recovery 后多一次 module 函数遍历。
- 测试总耗时 0.07 秒。

### 风险和限制

- 还没有 CLI 参数，当前只暴露库层 opt-in。
- 仍然不处理 callsite rewrite，所以有调用者的函数会被跳过。
- 只支持已有三个最小形状。

### 评分

- 实现效果：7/10。签名重写已能作为 recovery 的显式选项运行。
- 复杂度：3/10。只新增 option、summary 字段和一次 helper 调用。
- 维护成本：4/10。后续需要接 CLI 开关，并把 skip reason 做成可报告结果。
