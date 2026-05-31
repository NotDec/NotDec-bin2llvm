# 83. multi-return callsite 支持单分量 shared successor PHI

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
> - 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
> - 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
> - 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

## 背景

第 81、82 步已经让单返回的 return-only 和 input+return callsite 能处理 shared successor 中的返回寄存器 load。multi-return 现在仍然只支持返回 load 被新 call 支配的场景：rewrite 时在 call 点后 `extractvalue`，再替换旧寄存器 load。如果旧 load 位于 shared successor，直接替换会违反 LLVM SSA 支配关系，所以当前收集阶段仍会保守拒绝。

Ghidra 侧对应点：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionActiveReturn::apply(...)`
    - 对 call output trial 执行 `checkOutputTrialUse(...)`、`deriveOutputMap(...)`、`buildOutputFromTrials(...)`。
  - `ActionReturnRecovery::buildReturnOutput(...)`
    - 对多个返回 trial 会按 ABI 顺序把返回值拼进 `CPUI_RETURN`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::buildOutputFromTrials(...)`
    - 单个 output 直接作为 call output；两个 output 可以通过 join 形成一个整体输出。
  - `FuncCallSpecs::collectOutputTrialVarnodes(...)`
    - 收集每个 output trial 对应的 varnode。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncCallSpecs`
  - `ParamActive`
  - `ParamTrial`
  - `FuncProto`

native 侧用 LLVM struct 返回多返回值。这个阶段只复刻最小可验证部分：无输入 multi-return direct callsite，其中一个返回分量在 shared successor 中被读取，另一个分量未使用。

## 目标

- 只覆盖无输入 multi-return direct callsite。
- 只要求一个返回分量在 shared successor 中被读取，其他分量可以未使用。
- 对 shared successor 中的返回分量 load，先在 call block 生成 `extractvalue`，再在 shared successor 入口建 PHI：call 路径 incoming 是 extractvalue，其他前驱 incoming 是旧寄存器 load。
- 不扩到 input+multi-return，也不处理同一个 shared successor 里多个返回分量都被读取的组合。

## 路线

新增一个 IR 单测：callee 返回 `RAX/RDX`，caller 的 call block 和另一个前驱汇合到 `use_return`，`use_return` 只读取 `RAX`。期望 rewrite 后新 call 返回 struct，call block 里提取第 0 个分量，shared successor 里有 PHI，旧 `RAX` load 被删除，`RDX` 不生成 extract。

实现上扩展 multi-return callsite collection：

1. `findCallsiteReturnLoad(...)` 对 multi-return 收集传入 `allowSharedSuccessorLoad=true`。
2. `MultiReturnCallsiteRewrite` 记录每个分量是否来自 shared successor。
3. `rewriteMultiReturnDirectCallsites(...)` 对 shared successor 分量用 PHI 替换旧 load；非 shared successor 分量保持原来的直接 `extractvalue` 替换。

## 风险

- extractvalue 必须在 call block 中生成，作为 PHI 的 incoming，不能直接在 shared successor 里用新 call。
- 其他前驱要补旧寄存器 load，保留原 shared successor load 的路径语义。
- 本步只做无输入 multi-return，input+multi-return 后续单独处理。

## 判断标准

- 新增 multi-return shared successor PHI 单测通过。
- 现有 multi-return / return-only / input+return 测试继续通过。
- `notdec.native_prototype_recovery` 通过。
- 全量 `ctest` 通过。

## 实现记录

### 改动

- `lib/passes/NativePrototypeRecovery.cpp:565` 的 `MultiReturnCallsiteRewrite` 增加 `ReturnLoadResults` 和 `ReturnRegisterNames`，和 ABI return slot 对齐保存每个返回分量的 load 查找结果。
- `lib/passes/NativePrototypeRecovery.cpp:866` 新增 `replaceSharedSuccessorReturnLoad(...)`，把原来 return-only 的 shared successor PHI 生成逻辑抽出来，允许 call path incoming 使用 `call` 或 `extractvalue`。
- `lib/passes/NativePrototypeRecovery.cpp:910` 的 `rewriteCallsiteReturnLoad(...)` 改为复用 `replaceSharedSuccessorReturnLoad(...)`。
- `lib/passes/NativePrototypeRecovery.cpp:996` 的 `collectMultiReturnDirectCallsites(...)` 对每个返回分量调用 `findCallsiteReturnLoad(..., true)`，允许 shared successor load，并记录对应结果。
- `lib/passes/NativePrototypeRecovery.cpp:1032` 的 `rewriteMultiReturnDirectCallsites(...)` 对 shared successor 分量先在 call block 生成 `extractvalue`，再用 PHI 替换旧 load；普通分量仍按旧逻辑直接替换。
- `tests/native_prototype_recovery_test.cpp:233` 新增 `createSharedSuccessorOneReturnLoadCallerFunction(...)`，构造 `call` 和 `other_pred` 合流到 `use_return` 的测试 CFG。
- `tests/native_prototype_recovery_test.cpp:1864` 新增 `return_rdx_rax_shared` / `call_return_rdx_rax_shared` 测试函数。
- `tests/native_prototype_recovery_test.cpp:1951` 更新 summary 计数。
- `tests/native_prototype_recovery_test.cpp:4260` 新增断言：multi-return shared successor callsite 被重写为 struct return，只生成 1 个 `extractvalue`，shared successor 里生成 PHI，旧 `RAX` load 不再有 use。

### 验证

```bash
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery' --output-on-failure
```

结果：通过。

```bash
ctest --test-dir build --output-on-failure
```

结果：9/9 通过，总耗时 1.34s。

```bash
cmake --build build --target notdec-native-llvm -j2
```

结果：通过。

```bash
OUT_DIR=/tmp/notdec-bin2llvm-bench2-multi-return-shared-phi-smoke \
  scripts/bench2-native-smoke.sh --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-bench2-multi-return-shared-phi-smoke
```

结果：

| 用例 | 结果 | 时间 | rewritten | skipped |
| --- | --- | ---: | ---: | ---: |
| vsftpd | ok | 85s | 139 | 97 |
| libuv | ok | 219s | 338 | 233 |
| memcached | ok | 119s | 188 | 127 |

和上一步同口径 smoke 相比，rewrite 数量没有变化，耗时基本稳定。

### 评分

- 实现效果：8/10。覆盖了无输入 multi-return 中一个返回分量在 shared successor 被读取的 SSA 形状，避免直接替换造成支配关系错误。
- 复杂度：6/10。复用了 return-only 的 PHI 逻辑，新增状态只保存在 multi-return callsite rewrite 结构里，但目前还没有扩到 input+multi-return。
- 维护成本：6/10。后续如果支持多个 shared successor 分量或 input+multi-return，需要继续扩展同一套 `ReturnLoadSearchResult` 记录方式。
