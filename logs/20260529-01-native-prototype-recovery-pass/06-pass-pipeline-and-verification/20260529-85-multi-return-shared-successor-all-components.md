# 85. multi-return callsite 支持 shared successor 双返回分量 PHI

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
> - 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
> - 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
> - 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

## 背景

第 83 步覆盖了无输入 multi-return callsite 中“一个返回分量在 shared successor 里读取，另一个未使用”的情况。真实 CFG 中也可能两个返回寄存器分量都在同一个汇合块中读取。这个时候每个返回分量都需要自己的 `extractvalue` 和 PHI，不能只处理第一个分量。

Ghidra 侧对应点：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionActiveReturn::apply(...)`
    - 对 call output trial 做使用检查，并把可用 output trial 汇总给 call spec。
  - `ActionReturnRecovery::buildReturnOutput(...)`
    - 多个返回 trial 按 ABI 顺序组合成返回输出。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::buildOutputFromTrials(...)`
    - 处理多个 output trial，并在可组合时形成整体 call output。
  - `FuncCallSpecs::collectOutputTrialVarnodes(...)`
    - 收集每个 output trial 的 varnode，后续按 trial 列表处理。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncCallSpecs`
  - `ParamActive`
  - `ParamTrial`
  - `FuncProto`

native 侧已经把多返回值映射为 LLVM struct return。每个返回分量对应一个 ABI slot，一个 shared successor load 需要一个 PHI。这个阶段先固定无输入 multi-return 的双分量 shared successor 形状。

## 目标

- 只覆盖无输入 multi-return direct callsite。
- 两个返回分量都在同一个 shared successor 中被读取。
- rewrite 后 call block 生成两个 `extractvalue`。
- shared successor 生成两个 PHI，每个 PHI 的 call 路径 incoming 分别是对应的 `extractvalue`。
- 不扩到 input+multi-return 的双分量 shared successor。

## 路线

新增一个 IR 单测：callee 返回 `RDX/RAX`，caller 的 call block 和另一个前驱汇合到 `use_return`，`use_return` 同时读取 `RAX` 和 `RDX`。期望 rewrite 后：

- 新 call 返回 struct；
- 生成 2 个 `extractvalue`；
- shared successor 中生成 2 个 PHI；
- 两个 PHI 都有 `extractvalue` incoming；
- 两个旧返回寄存器 load 都不再有 use。

实现上优先检查现有 `MultiReturnCallsiteRewrite` 是否已经按返回 slot 保存 `ReturnLoadSearchResult`。如果现有逻辑已经支持，只补测试和记录；如果测试暴露问题，再做最小代码修改。

## 风险

- 两个 PHI 都插在 shared successor 入口，顺序不重要，但 incoming 必须来自对应返回分量的 `extractvalue`。
- 其他前驱仍要保留旧寄存器 load，避免把未走 call 的路径错误改成 call 返回值。
- 本步不处理 input+multi-return 的双分量组合，避免一次扩大测试面。

## 判断标准

- 新增双分量 shared successor multi-return 单测通过。
- 现有 shared successor 单分量测试继续通过。
- `notdec.native_prototype_recovery` 通过。
- 全量 `ctest` 通过。

## 实现记录

### 改动

- `tests/native_prototype_recovery_test.cpp:271` 新增 `createSharedSuccessorTwoReturnLoadCallerFunction(...)`，构造 call block 和另一个前驱汇合到 `use_return`，并在 shared successor 中读取两个返回寄存器。
- `tests/native_prototype_recovery_test.cpp:1914` 新增 `return_rdx_rax_shared_both` / `call_return_rdx_rax_shared_both` 测试函数。
- `tests/native_prototype_recovery_test.cpp:2014` 更新 summary 计数：新增 2 个函数、2 个 return candidate、1 个需要签名重写的函数。
- `tests/native_prototype_recovery_test.cpp:4378` 新增双分量 shared successor multi-return 断言：新 call 返回 struct，生成 2 个 `extractvalue`，shared successor 生成 2 个 PHI，两个 PHI 都有 `extractvalue` incoming，旧 `RAX.return_value` / `RDX.return_value` load 不再存在。

这一步没有改生产代码。第 83 步引入的 `MultiReturnCallsiteRewrite` 已经按 ABI return slot 保存 `ReturnLoadSearchResult`，现有 `rewriteMultiReturnDirectCallsites(...)` 可以逐个返回分量生成 `extractvalue + PHI`。本步把这个行为固定成回归测试。

实现中发现一个测试写法问题：直接对可能已被 `eraseFromParent()` 删除的旧 `LoadInst *` 调 `use_empty()` 不稳。双分量测试改为扫描 caller IR，确认原旧 load 名称不再存在。

### 验证

```bash
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery' --output-on-failure
```

结果：通过。

```bash
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
```

结果：`notdec-native-llvm` 构建通过；全量 `ctest` 9/9 通过，总耗时 1.39s。

```bash
OUT_DIR=/tmp/notdec-bin2llvm-bench2-multi-return-shared-all-phi-smoke \
  scripts/bench2-native-smoke.sh --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-bench2-multi-return-shared-all-phi-smoke
```

结果：

| 用例 | 结果 | 时间 | seen | rewritten | skipped | skip reasons |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| vsftpd | ok | 85s | 236 | 139 | 97 | already matches 48, declaration 49 |
| libuv | ok | 220s | 571 | 338 | 233 | already matches 147, declaration 86 |
| memcached | ok | 119s | 315 | 188 | 127 | already matches 71, declaration 56 |

本步没有改生产代码，Bench2 指标和第 84 步同口径一致，耗时没有明显变化。

### 评分

- 实现效果：7/10。补上了双返回分量都在 shared successor 中读取的回归覆盖，证明现有 slot 级 PHI 重写能处理这个形状。
- 复杂度：3/10。只增加测试 helper 和断言，没有改 pass 代码。
- 维护成本：3/10。测试形状清晰，后续扩 input+multi-return 双分量时可以复用思路。
