# 82. input+return callsite 支持 shared successor PHI 返回值

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
> - 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
> - 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
> - 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

## 背景

第 81 步让 return-only direct callsite 在 shared successor 里读取返回寄存器时，可以用 PHI 接新 call 结果。input+return 的单返回形状仍然沿用旧保守路径：一旦返回 load 在 shared successor 里，就报 `unsafe callsite return load`。

Ghidra 侧对应点：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionActiveReturn::apply(...)`
    - 对 call output trial 执行 `checkOutputTrialUse(...)`、`deriveOutputMap(...)`、`buildOutputFromTrials(...)`。
  - `ActionParamDouble::apply(...)`
    - input trial 和 output trial 会在同一条 call prototype recovery 流程里逐步收敛。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::buildInputFromTrials(...)`
    - 把 active input trial 接到 call input。
  - `FuncCallSpecs::buildOutputFromTrials(...)`
    - 把 active output trial 接到 call output。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncCallSpecs`
  - `ParamActive`
  - `ParamTrial`
  - `FuncProto`

native 侧已经有两块能力：`collectMultiInputDirectCallsiteRewrites(...)` 能找 callsite 参数值，`rewriteCallsiteReturnLoad(...)` 能在 shared successor 里为单返回值建 PHI。现在只需要把 input+return 的单返回路径接到这个 PHI 能力。

## 目标

- 只覆盖 input+return direct callsite，返回值是单个 register。
- call 前参数值仍走现有 `collectMultiInputDirectCallsiteRewrites(...)`。
- call 后 shared successor 的返回寄存器 load 用第 81 步的 PHI 方式替换。
- 不扩到 multi-return / input+multi-return。

## 路线

新增一个 IR 单测：callee 需要 `RDI` 输入并返回 `RAX`，caller 在 call 前写 `RDI`，call block 和另一个前驱汇合到 `use_return`，`use_return` 读取 `RAX`。期望 rewrite 后 call 带一个参数，返回值进入 shared successor PHI，旧 `RAX` load 被删除。

实现上改两处：

1. `rewriteInputReturnDirectCallsites(...)` 调用 `rewriteCallsiteReturnLoad(...)` 时允许 shared successor。
2. `rewriteNativeRecoveredPrototypeInputReturn(...)` 做 unsafe return load 检查时同样允许 shared successor。

## 风险

- 新 call 同时有参数和返回值，PHI incoming 必须使用新 call，不要用旧 void call。
- 参数侧仍要先全部收集成功，避免先改一半 callsite。
- 只改单返回 input+return，multi-return 的 extractvalue/PHI 组合更复杂，后续再做。

## 判断标准

- 新增 input+return shared successor PHI 单测通过。
- 既有 return-only PHI 单测继续通过。
- `notdec.native_prototype_recovery` 通过。
- 全量 `ctest` 通过。

## 实现记录

改动文件：

- `lib/passes/NativePrototypeRecovery.cpp:922`
  - `rewriteInputReturnDirectCallsites(...)` 调用 `rewriteCallsiteReturnLoad(...)` 时传入 `allowSharedSuccessorLoad=true`，允许 shared successor PHI 返回值接线。
- `lib/passes/NativePrototypeRecovery.cpp:2005`
  - `rewriteNativeRecoveredPrototypeInputReturn(...)` 的 unsafe return load 检查同样传入 `allowSharedSuccessorLoad=true`。
- `tests/native_prototype_recovery_test.cpp:847`
  - 新增 `createInputStoreSharedSuccessorReturnLoadCallerFunction(...)`，构造 call 前写输入寄存器、call 后合流到 shared successor 读取返回寄存器的 caller。
- `tests/native_prototype_recovery_test.cpp:3853`
  - 新增 input+return shared successor 单测，检查新 call 带参数、返回值进入 PHI、旧返回寄存器 load 被删除。

这一步没有改 multi-return / input+multi-return。它们仍然不允许 shared successor load，避免一次引入 extractvalue + PHI 的组合。

## 验证

```bash
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery' --output-on-failure
```

结果：通过。

```bash
ctest --test-dir build --output-on-failure
cmake --build build --target notdec-native-llvm -j2
```

结果：全量 `ctest` 9/9 通过，总耗时 1.34s；`notdec-native-llvm` 构建通过。

```bash
OUT_DIR=/tmp/notdec-bin2llvm-bench2-input-return-shared-phi-smoke \
  scripts/bench2-native-smoke.sh --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-bench2-input-return-shared-phi-smoke
```

结果：通过。

Bench2 指标：

| target | elapsed | seen | rewritten | skipped | skip reasons |
| --- | ---: | ---: | ---: | ---: | --- |
| vsftpd | 84s | 236 | 139 | 97 | already matches 48, declaration 49 |
| libuv | 220s | 571 | 338 | 233 | already matches 147, declaration 86 |
| memcached | 118s | 315 | 188 | 127 | already matches 71, declaration 56 |

性能：三目标耗时和上一轮同口径 84s / 219s / 118s 基本一致。真实样本 rewrite 数没有变化，本步主要补齐 input+return 的 CFG 正确性边界。

## 评分

- 实现效果：7/10。input+return 单返回 callsite 现在复用第 81 步的 PHI 语义，覆盖了一个原本保守拒绝的 CFG 形状。
- 复杂度：4/10。生产代码只打开已有开关，主要增加单测。
- 维护成本：4/10。后续如果扩到 multi-return，需要单独处理 struct return 的 extractvalue 和 PHI，不能直接套本步。
