# 86. input+multi-return callsite 支持 shared successor 双返回分量 PHI

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
> - 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
> - 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
> - 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

## 背景

第 84 步覆盖了 input+multi-return callsite 中一个返回分量在 shared successor 中读取的情况。第 85 步确认无输入 multi-return 中两个返回分量都在 shared successor 读取时，现有 slot 级 PHI 重写能正确工作。还剩一个相邻形状：callsite 同时有输入参数，且两个 struct return 分量都在 shared successor 读取。

Ghidra 侧对应点：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionParamDouble::apply(...)`
    - input trial 和 output trial 会在同一条 call prototype recovery 流程里收敛。
  - `ActionActiveReturn::apply(...)`
    - 对 call output trial 做使用检查，并交给 call spec 构建返回输出。
  - `ActionReturnRecovery::buildReturnOutput(...)`
    - 多个返回 trial 按 ABI 顺序组合成返回输出。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::buildInputFromTrials(...)`
    - 把 active input trial 接到 call input。
  - `FuncCallSpecs::buildOutputFromTrials(...)`
    - 把 active output trial 接到 call output，支持多个返回 storage。
  - `FuncCallSpecs::collectOutputTrialVarnodes(...)`
    - 收集每个 output trial 对应的 varnode。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncCallSpecs`
  - `ParamActive`
  - `ParamTrial`
  - `FuncProto`

native 侧用 `InputMultiReturnCallsiteRewrite` 同时保存 callsite 参数值和每个返回分量的 load 查找结果。第 84 步已经让它记录 `ReturnLoadSearchResult`。本步验证两个返回分量都需要 shared successor PHI 时，参数和两个返回分量能一起正确改写。

## 目标

- 只覆盖单 input + multi-return direct callsite。
- 两个返回分量都在同一个 shared successor 中被读取。
- rewrite 后新 call 带一个参数并返回 struct。
- call block 生成两个 `extractvalue`。
- shared successor 生成两个 PHI，分别接对应 `extractvalue`。
- 不扩展到多 input + multi-return 的 shared successor 双分量组合。

## 路线

新增一个 IR 单测：callee 读取 `RDI` 并返回 `RDX/RAX`；caller 的 call block 写 `RDI` 后调用 callee，另一个前驱直接汇合到 `use_return`，`use_return` 同时读取 `RAX` 和 `RDX`。期望 rewrite 后：

- 新 call 带 1 个参数；
- 新 call 返回 struct；
- 生成 2 个 `extractvalue`；
- 生成 2 个 PHI，两个 PHI 都有 `extractvalue` incoming；
- 旧 `RAX.return_value` / `RDX.return_value` load 不再存在。

实现上优先复用现有 `InputMultiReturnCallsiteRewrite`。如果现有逻辑已经支持，只补测试和记录；如果测试暴露问题，再做最小代码修改。

## 风险

- 参数收集必须仍然在旧 call 前完成，不能被 shared successor 返回值逻辑影响。
- 两个 PHI 的 incoming 要分别对应两个 `extractvalue`，不能混 slot。
- 本步不处理多 input 的组合，避免一次扩大范围。

## 判断标准

- 新增 input+multi-return 双分量 shared successor 单测通过。
- 现有单分量 shared successor 测试继续通过。
- `notdec.native_prototype_recovery` 通过。
- 全量 `ctest` 通过。

## 实现记录

### 改动

- `tests/native_prototype_recovery_test.cpp:975` 新增 `createInputStoreSharedSuccessorTwoReturnLoadCallerFunction(...)`，构造 call block 写输入寄存器、调用 callee，再和另一个前驱汇合到 shared successor 读取两个返回寄存器。
- `tests/native_prototype_recovery_test.cpp:1999` 新增 `input_rdi_return_rdx_rax_shared_both` / `call_input_rdi_return_rdx_rax_shared_both` 测试函数。
- `tests/native_prototype_recovery_test.cpp:2080` 更新 summary 计数：新增 2 个函数、1 个 input candidate、2 个 return candidate、1 个需要签名重写的函数。
- `tests/native_prototype_recovery_test.cpp:4615` 新增 input+multi-return 双分量 shared successor 断言：新 call 带 1 个参数并返回 struct，生成 2 个 `extractvalue`，shared successor 生成 2 个 PHI，两个 PHI 都有 `extractvalue` incoming，旧 `RAX.return_value` / `RDX.return_value` load 不再存在。

这一步没有改生产代码。第 84 步扩展的 `InputMultiReturnCallsiteRewrite` 已经按 ABI return slot 保存 `ReturnLoadSearchResult`，现有 `rewriteInputMultiReturnDirectCallsites(...)` 可以逐个返回分量生成 `extractvalue + PHI`。本步把双分量形状固定成回归测试。

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
OUT_DIR=/tmp/notdec-bin2llvm-bench2-input-multi-return-shared-all-phi-smoke \
  scripts/bench2-native-smoke.sh --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-bench2-input-multi-return-shared-all-phi-smoke
```

结果：

| 用例 | 结果 | 时间 | seen | rewritten | skipped | skip reasons |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| vsftpd | ok | 86s | 236 | 139 | 97 | already matches 48, declaration 49 |
| libuv | ok | 225s | 571 | 338 | 233 | already matches 147, declaration 86 |
| memcached | ok | 121s | 315 | 188 | 127 | already matches 71, declaration 56 |

本步没有改生产代码，Bench2 rewrite 指标和前一轮一致；耗时在同口径波动范围内。

### 评分

- 实现效果：7/10。补上 input+multi-return 双返回分量 shared successor 的回归覆盖，证明参数接线和两个返回分量 PHI 能同时成立。
- 复杂度：3/10。只增加测试 helper 和断言，没有改 pass 代码。
- 维护成本：3/10。测试形状明确，后续多 input + multi-return 可按同样方式扩展。
