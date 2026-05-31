# 87. 多 input+multi-return callsite 支持 shared successor 双返回分量 PHI

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
> - 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
> - 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
> - 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

## 背景

第 84、86 步覆盖了单 input + multi-return 的 shared successor 返回值 PHI。当前 `InputMultiReturnCallsiteRewrite` 本身支持多个 input，但 shared successor 双返回分量组合还没有覆盖到多 input 场景。真实 callsite 常见的是多个参数和多个返回寄存器同时出现，所以需要把这个边界固定下来。

Ghidra 侧对应点：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionParamDouble::apply(...)`
    - input/output trial 会在同一条 call prototype recovery 流程中一起更新。
  - `ActionActiveReturn::apply(...)`
    - 检查 call output trial 的使用，并构建返回输出。
  - `ActionReturnRecovery::buildReturnOutput(...)`
    - 多个返回 trial 按 ABI 顺序组合成返回输出。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::buildInputFromTrials(...)`
    - 按 ABI 顺序把多个 active input trial 接到 call input。
  - `FuncCallSpecs::buildOutputFromTrials(...)`
    - 把多个 output trial 接到 call output。
  - `FuncCallSpecs::collectOutputTrialVarnodes(...)`
    - 收集每个 output trial 对应的 varnode。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncCallSpecs`
  - `ParamActive`
  - `ParamTrial`
  - `FuncProto`

native 侧同一个 `InputMultiReturnCallsiteRewrite` 同时承载参数列表和返回 load 列表。前几步已经证明单 input 和双返回分量的 shared successor PHI 可以工作。本步验证多 input 时参数顺序不会被返回 PHI 逻辑影响。

## 目标

- 只覆盖两个 input + multi-return direct callsite。
- 两个返回分量都在同一个 shared successor 中被读取。
- rewrite 后新 call 带两个参数并返回 struct。
- call block 生成两个 `extractvalue`。
- shared successor 生成两个 PHI，分别接对应 `extractvalue`。
- 不扩展到更复杂的多前驱参数等价场景。

## 路线

新增一个 IR 单测：callee 读取 `RDI/RSI` 并返回 `RDX/RAX`；caller 的 call block 写 `RDI/RSI` 后调用 callee，另一个前驱直接汇合到 `use_return`，`use_return` 同时读取 `RAX/RDX`。期望 rewrite 后：

- 新 call 带 2 个参数，参数仍按 ABI 顺序；
- 新 call 返回 struct；
- 生成 2 个 `extractvalue`；
- 生成 2 个 PHI，两个 PHI 都有 `extractvalue` incoming；
- 旧 `RAX.return_value` / `RDX.return_value` load 不再存在。

如果现有逻辑已经支持，只补测试和记录；如果测试暴露问题，再做最小代码修改。

## 风险

- 参数顺序必须保持 ABI slot 顺序，不能被 caller 中 store 顺序影响。
- 两个 PHI 的 incoming 必须和对应返回分量匹配。
- shared successor 的其他前驱仍然保留旧寄存器 load，不能错误使用 call 返回值。

## 判断标准

- 新增多 input+multi-return 双分量 shared successor 单测通过。
- 现有 input+multi-return shared successor 测试继续通过。
- `notdec.native_prototype_recovery` 通过。
- 全量 `ctest` 通过。

## 实现记录

### 改动

- `tests/native_prototype_recovery_test.cpp:434` 新增 `createTwoInputStoreSharedSuccessorTwoReturnLoadCallerFunction(...)`，构造 call block 写 `RDI/RSI`、调用 callee，再和另一个前驱汇合到 shared successor 读取 `RAX/RDX`。
- `tests/native_prototype_recovery_test.cpp:2108` 新增 `input_rdi_rsi_return_rdx_rax_shared_both` / `call_input_rdi_rsi_return_rdx_rax_shared_both` 测试函数。
- `tests/native_prototype_recovery_test.cpp:2165` 更新 summary 计数：新增 2 个函数、2 个 external input、2 个 input candidate、2 个 return candidate、1 个需要签名重写的函数。
- `tests/native_prototype_recovery_test.cpp:4876` 新增多 input+multi-return 双分量 shared successor 断言：新 call 带 2 个参数并保持 ABI 顺序，返回 struct，生成 2 个 `extractvalue`，shared successor 生成 2 个 PHI，两个 PHI 都有 `extractvalue` incoming，旧 `RAX.return_value` / `RDX.return_value` load 不再存在。

这一步没有改生产代码。现有 `InputMultiReturnCallsiteRewrite` 已经同时保存 ABI 顺序参数列表和每个返回 slot 的 `ReturnLoadSearchResult`，本步把多 input + 双返回分量 shared successor 的组合固定成回归测试。

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
OUT_DIR=/tmp/notdec-bin2llvm-bench2-multi-input-multi-return-shared-all-phi-smoke \
  scripts/bench2-native-smoke.sh --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-bench2-multi-input-multi-return-shared-all-phi-smoke
```

结果：

| 用例 | 结果 | 时间 | seen | rewritten | skipped | skip reasons |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| vsftpd | ok | 88s | 236 | 139 | 97 | already matches 48, declaration 49 |
| libuv | ok | 220s | 571 | 338 | 233 | already matches 147, declaration 86 |
| memcached | ok | 118s | 315 | 188 | 127 | already matches 71, declaration 56 |

本步没有改生产代码，Bench2 rewrite 指标和前一轮一致；耗时在同口径波动范围内。

### 评分

- 实现效果：7/10。补上多 input + multi-return 双返回分量 shared successor 的回归覆盖，确认参数顺序和两个返回分量 PHI 能同时成立。
- 复杂度：3/10。只增加测试 helper 和断言，没有改 pass 代码。
- 维护成本：3/10。测试形状明确，后续如果扩多前驱参数等价或更复杂 CFG，可以沿用这组断言。
