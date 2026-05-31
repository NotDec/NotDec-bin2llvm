# 81. shared successor 返回值 load 用 PHI 重写

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
> - 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
> - 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
> - 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

## 背景

现在 callsite return rewrite 已经能处理线性后继、直接多后继、未使用 shared successor、shared successor 里先 clobber 的情况。但如果 call 路径和非 call 路径汇合到同一个 block，然后在这个 shared successor 里读取返回寄存器，当前实现会保守拒绝。

Ghidra 侧对应点：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionActiveReturn::apply(...)`
    - 对每个 call 的 active output 运行 `checkOutputTrialUse(...)`、`deriveOutputMap(...)`、`buildOutputFromTrials(...)`。
  - `ActionReturnRecovery::buildReturnOutput(...)`
    - 把 recovered output 追加到 `CPUI_RETURN`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::checkOutputTrialUse(...)`
    - 判断 call output trial 是否真的被使用。
  - `FuncCallSpecs::buildOutputFromTrials(...)`
    - 把 active output varnode 移到 call output 上。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto`
  - `FuncCallSpecs`
  - `ParamActive`
  - `ParamTrial`

native 侧目前没有 P-code call output varnode，只能在 LLVM SSA 上复刻。shared successor 里读取返回寄存器时，call 结果不支配 shared successor，所以不能直接 `replaceAllUsesWith(newCall)`。需要在 shared successor 入口创建 PHI：call 路径 incoming 是新 call 结果，其他前驱 incoming 保留旧 load 的值。

## 目标

- 只覆盖 return-only direct callsite。
- 对一个 shared successor 中的返回寄存器 load，如果这个 load 支配了来自 call 路径和非 call 路径的使用点，则允许 return-only rewrite。
- rewrite 时在 shared successor 入口插入 PHI，用 PHI 替换旧 load。call 路径 incoming 是新 call 结果，其他前驱 incoming 是在前驱 terminator 前补的旧寄存器 load。
- input+return 和 multi-return 暂时不打开这个路径，避免一次扩大太多。

## 路线

先给 `native_prototype_recovery_test.cpp` 加一个窄测试：`call` block 和 `other_pred` 汇合到 `use_return`，`use_return` 里读取 `RAX`。期望 return-only rewrite 成功，旧 load 被删除，新 call 结果通过 PHI 传给使用点。

实现上扩展 callsite return load 收集结果，不只返回 `LoadInst`，还记录 shared successor 的前驱信息。普通线性场景继续直接替换；shared successor 场景改为插 PHI。

## 风险

- PHI 插入点必须在 shared successor 的 PHI 之后、普通指令之前。
- old call 替换成 new call 后，PHI 的 incoming block 仍然是旧 call 所在 block，不能弄错。
- 这一步只做 return-only direct callsite，不扩到 input+return 和 multi-return，避免一次改太大。

## 判断标准

- 新增 shared successor PHI 单测通过。
- 既有 unsafe multi-predecessor 测试仍拒绝。
- `notdec.native_prototype_recovery` 通过。
- 全量 `ctest` 通过。

## 实现记录

改动文件：

- `lib/passes/NativePrototypeRecovery.cpp:534`
  - `ReturnLoadSearchResult` 增加 `SharedSuccessor` 和 `CallPredecessor`，用于标出返回寄存器 load 位于 shared successor 的场景。
- `lib/passes/NativePrototypeRecovery.cpp:542`
  - 新增 `foundReturnLoad(...)`、`blockedReturnLoadSearch()`、`clobberedReturnLoadSearch()`，避免结构字段增加后旧 `{...}` 初始化错位。
- `lib/passes/NativePrototypeRecovery.cpp:789`
  - `findCallsiteReturnLoad(...)` 增加 `allowSharedSuccessorLoad` 参数。默认仍保守；只有 return-only rewrite 传 `true`。
- `lib/passes/NativePrototypeRecovery.cpp:864`
  - `rewriteCallsiteReturnLoad(...)` 在 shared successor 场景创建 PHI。call 路径 incoming 使用新 call 结果；其他前驱在 terminator 前补一个旧寄存器 load，作为 PHI incoming。
- `lib/passes/NativePrototypeRecovery.cpp:947`
  - `collectReturnOnlyDirectCallsites(...)` 对 return-only direct callsite 允许 shared successor load。
- `tests/native_prototype_recovery_test.cpp:3388`
  - 原 multi-predecessor return callsite 用例改为正例，检查 rewrite 成功、旧 load 删除、shared successor 里生成含新 call incoming 的 PHI。

计划阶段的“其他前驱没有同寄存器来源才允许”实现时改成“其他前驱补旧寄存器 load”。原因是 shared successor 的旧 load 原本在所有路径上都执行；PHI 只有 call 路径应该换成新 call 结果，非 call 路径应保持原寄存器值。

## 验证

```bash
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery' --output-on-failure
```

结果：通过。

```bash
ctest --test-dir build --output-on-failure
```

结果：9/9 通过，总耗时 1.35s。

```bash
OUT_DIR=/tmp/notdec-bin2llvm-bench2-shared-successor-phi-smoke \
  scripts/bench2-native-smoke.sh --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-bench2-shared-successor-phi-smoke
```

结果：通过。

Bench2 指标：

| target | elapsed | seen | rewritten | skipped | skip reasons |
| --- | ---: | ---: | ---: | ---: | --- |
| vsftpd | 84s | 236 | 139 | 97 | already matches 48, declaration 49 |
| libuv | 219s | 571 | 338 | 233 | already matches 147, declaration 86 |
| memcached | 118s | 315 | 188 | 127 | already matches 71, declaration 56 |

性能：三目标耗时和上一轮同口径 86s / 219s / 118s 基本一致。真实样本 rewrite 数没有变化，本步主要补齐一个 CFG 正确性边界。

## 评分

- 实现效果：7/10。return-only shared successor 返回值 load 现在能保持 SSA 支配关系并完成 rewrite。
- 复杂度：6/10。新增 PHI 路径会增加一点理解成本，但范围只限 return-only。
- 维护成本：6/10。后续 input+return / multi-return 要复用这套 PHI 逻辑时，可能需要把 return load rewrite 结果结构再整理一次；现在先不抽象，避免过早扩大。
