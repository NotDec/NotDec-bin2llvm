# 88. 多 input+return callsite 支持 shared successor PHI

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
> - 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
> - 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
> - 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

## 背景

第 82 步覆盖了单 input + 单 return 的 shared successor PHI。当前 `rewriteInputReturnDirectCallsites(...)` 已经使用 `MultiInputCallsiteRewrite`，理论上也支持多个 input；但测试只覆盖了一个 input。为了避免多参数接线和 shared successor PHI 组合以后退化，需要补一个多 input + 单 return 的 shared successor 回归测试。

Ghidra 侧对应点：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionParamDouble::apply(...)`
    - input trial 和 output trial 在同一条 call prototype recovery 流程中一起收敛。
  - `ActionActiveReturn::apply(...)`
    - 检查 call output trial 的真实使用。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::buildInputFromTrials(...)`
    - 按 ABI 顺序把多个 input trial 接到 call input。
  - `FuncCallSpecs::buildOutputFromTrials(...)`
    - 把返回 trial 接到 call output。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncCallSpecs`
  - `ParamActive`
  - `ParamTrial`
  - `FuncProto`

native 侧多 input + 单 return 走 `MultiInputCallsiteRewrite`，返回值重写走 `rewriteCallsiteReturnLoad(...)`。本步验证两者组合时，参数 ABI 顺序和 shared successor PHI 都正确。

## 目标

- 只覆盖两个 input + 单 return direct callsite。
- 返回寄存器 load 位于 shared successor。
- rewrite 后新 call 带两个参数并返回 i64。
- shared successor 生成 PHI，call 路径 incoming 是新 call。
- 不扩展到多 input + multi-return，这部分已经在第 87 步覆盖。

## 路线

新增一个 IR 单测：callee 读取 `RDI/RSI` 并返回 `RAX`；caller 的 call block 写 `RDI/RSI` 后调用 callee，另一个前驱直接汇合到 `use_return`，`use_return` 读取 `RAX`。期望 rewrite 后：

- 新 call 带 2 个参数，参数按 ABI 顺序；
- 新 call 返回 i64；
- shared successor 中有 PHI；
- PHI 的 call 路径 incoming 是新 call；
- 旧 `RAX.return_value` load 不再存在。

如果现有逻辑已经支持，只补测试和记录；如果测试暴露问题，再做最小代码修改。

## 风险

- 参数顺序仍要按 recovered prototype 的 ABI slot 顺序，不按 caller store 顺序。
- shared successor 的其他前驱要保留旧寄存器 load，不能使用 call 返回值。
- 旧 load 可能被删除，测试要扫描 IR，不直接依赖已删除指针。

## 判断标准

- 新增多 input+return shared successor 单测通过。
- 现有 input+return shared successor 单测继续通过。
- `notdec.native_prototype_recovery` 通过。
- 全量 `ctest` 通过。

## 实现记录

本步只补回归测试，没有改生产 pass。现有
`MultiInputCallsiteRewrite`、`rewriteCallsiteReturnLoad(...)` 和
`replaceSharedSuccessorReturnLoad(...)` 已能处理这个 CFG。

源码改动：

- `tests/native_prototype_recovery_test.cpp:437`
  - 新增 `createTwoInputStoreSharedSuccessorReturnLoadCallerFunction(...)`。
  - 构造 caller CFG：`entry` 分到 `call` 和 `other_pred`，两路汇合到 `use_return`。
  - `call` block 写 `RDI/RSI` 后调用 callee；`use_return` 读取 `RAX`，用于验证 shared successor PHI。
- `tests/native_prototype_recovery_test.cpp:4372`
  - 新增 `native-prototype-multi-input-return-shared-successor-callsite-rewrite-test`。
  - 覆盖 callee 读取 `RDI/RSI`、返回 `RAX`，caller 的返回 load 位于 shared successor。
  - 检查 rewrite 后新 call 有 2 个参数、参数顺序为 `RDI/RSI`、返回 `i64`、shared successor 中生成 2 incoming 的 PHI，且旧 `RAX.return_value` load 不再存在。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery' --output-on-failure
```

结果：通过。

```bash
ctest --test-dir build --output-on-failure
cmake --build build --target notdec-native-llvm -j2
```

结果：全量 `ctest` 9/9 通过，`notdec-native-llvm` 构建通过。

Bench2 smoke：

```bash
OUT_DIR=/tmp/notdec-bin2llvm-bench2-multi-input-return-shared-phi-smoke \
  scripts/bench2-native-smoke.sh --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-bench2-multi-input-return-shared-phi-smoke
```

结果：

| 项目 | 状态 | elapsed | seen | rewritten | skipped | already matches | declaration |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| vsftpd | ok | 87s | 236 | 139 | 97 | 48 | 49 |
| libuv | ok | 224s | 571 | 338 | 233 | 147 | 86 |
| memcached | ok | 120s | 315 | 188 | 127 | 71 | 56 |

`metrics.tsv` 的 prototype/signature 数量和前一次 smoke 同口径一致。

评分：

- 实现效果：8/10。覆盖了多 input + 单 return + shared successor PHI 组合，补上此前缺口。
- 复杂度：9/10。只增加测试 helper 和测试块，没有增加生产逻辑理解成本。
- 维护成本：9/10。测试沿用已有 fixture 风格，后续如果 shared successor 重写退化，会直接报错。

更好的方案：后续可以把 shared successor caller 构造 helper 继续收敛，减少测试文件重复；本步先保持小改动，避免重构影响正在验证的 pass 行为。
