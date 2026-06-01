# 89. 多 input+multi-return callsite 覆盖 shared successor 部分返回分量

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
> - 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
> - 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
> - 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

## 背景

第 87 步覆盖了多 input + multi-return 且两个返回分量都在 shared successor 使用的情况。真实代码里也会出现只使用一个返回寄存器、另一个返回寄存器未使用的 callsite。第 70 步已经允许 multi-return 的部分返回分量未使用，本步把这个能力和多 input + shared successor PHI 组合起来固定成回归测试。

Ghidra 侧对应点：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionParamDouble::apply(...)`
    - input/output trial 在同一轮 prototype recovery 中一起处理。
  - `ActionActiveReturn::apply(...)`
    - 根据 call output trial 是否有真实使用决定返回输出。
  - `ActionReturnRecovery::buildReturnOutput(...)`
    - 把保留下来的 output trial 组装成返回输出。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::buildInputFromTrials(...)`
    - 按 ABI 顺序构造多个 input。
  - `FuncCallSpecs::buildOutputFromTrials(...)`
    - 按 output trial 构造返回输出。
  - `FuncCallSpecs::collectOutputTrialVarnodes(...)`
    - 收集每个 output trial 对应的 varnode，未使用分量不会强行生成使用。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncCallSpecs`
  - `ParamActive`
  - `ParamTrial`
  - `FuncProto`

native 侧 `InputMultiReturnCallsiteRewrite` 已经用 null `ReturnLoads` 表示未使用返回分量；`replaceSharedSuccessorReturnLoad(...)` 负责把已使用分量替换成 PHI。本步验证这两者同时出现时不会互相影响。

## 目标

- 覆盖两个 input + 两个 return 的 direct callsite。
- caller 的返回 load 位于 shared successor。
- 只使用一个返回分量，另一个返回分量保持未使用。
- rewrite 后新 call 带两个参数，返回 struct。
- 只为已使用返回分量生成一个 `extractvalue` 和一个 PHI。
- 不扩展到间接 call、已有参数 call 或更复杂多层 CFG。

## 路线

新增一个独立 module 单测：callee 读取 `RDI/RSI` 并返回 `RDX/RAX`；caller 在 call block 写 `RDI/RSI` 后调用 callee，另一个前驱直接汇合到 `use_return`，`use_return` 只读取 `RAX`。期望 rewrite 后：

- 新 call 参数仍按 ABI 顺序；
- 新 call 返回 struct；
- 只有一个 `extractvalue`；
- shared successor 中只有一个 PHI 接入该 `extractvalue`；
- 旧 `RAX.return_value` load 不再存在；
- 不要求为未使用的 `RDX` 生成 `extractvalue`。

如果现有逻辑已经支持，只补测试和记录；如果测试暴露问题，再做最小代码修改。

## 风险

- 未使用返回分量不能因为 struct return 存在而强行 `extractvalue`。
- shared successor 的非 call 前驱仍要保留旧寄存器 load。
- 参数顺序必须继续按 ABI slot，而不是按 caller store 或返回分量顺序。

## 判断标准

- 新增单测通过。
- `notdec.native_prototype_recovery` 通过。
- 全量 `ctest` 通过。

## 实现记录

本步只补回归测试，没有改生产 pass。现有
`InputMultiReturnCallsiteRewrite` 已经用空返回 load 表示未使用分量，
`replaceSharedSuccessorReturnLoad(...)` 已能处理已使用分量的 shared successor PHI。

源码改动：

- `tests/native_prototype_recovery_test.cpp:4478`
  - 新增独立 module
    `native-prototype-multi-input-multi-return-partial-shared-successor-test`。
  - 构造 callee `input_rdi_rsi_return_rdx_rax_partial_shared`：读取
    `RDI/RSI`，写回 `RDX/RAX` 两个返回寄存器。
  - 构造 caller `call_input_rdi_rsi_return_rdx_rax_partial_shared`：
    call block 写 `RDI/RSI` 后调用 callee，shared successor 只读取 `RAX`。
- `tests/native_prototype_recovery_test.cpp:4516`
  - 跑 `runNativePrototypeRecovery(...)` 后调用
    `rewriteNativeRecoveredPrototype(...)`。
  - 断言新 call 带两个参数，参数按 ABI 顺序，返回 struct。
  - 断言只生成 1 个 `extractvalue` 和 1 个 PHI。
  - 断言旧 `RAX.return_value` load 被删除，未使用的 `RDX` 没有被额外读取。

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

结果：全量 `ctest` 9/9 通过，总耗时 1.37s；`notdec-native-llvm` 构建通过。

Bench2 smoke：

```bash
OUT_DIR=/tmp/notdec-bin2llvm-bench2-multi-input-multi-return-partial-shared-smoke \
  scripts/bench2-native-smoke.sh --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-bench2-multi-input-multi-return-partial-shared-smoke
```

结果：

| 项目 | 状态 | elapsed | seen | rewritten | skipped | already matches | declaration |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| vsftpd | ok | 85s | 236 | 139 | 97 | 48 | 49 |
| libuv | ok | 221s | 571 | 338 | 233 | 147 | 86 |
| memcached | ok | 118s | 315 | 188 | 127 | 71 | 56 |

本步没有改生产代码，Bench2 rewrite 指标和前一轮一致。

评分：

- 实现效果：7/10。覆盖多 input + multi-return + shared successor 中“只使用部分返回分量”的组合。
- 复杂度：9/10。复用已有 helper 和重写逻辑，只增加一个独立 module 测试。
- 维护成本：8/10。断言数量较多，但测试形状明确，能防止未使用返回分量被误 `extractvalue`。

更好的方案：后续可以把 caller IR 扫描断言抽成小 helper，减少测试主体里的重复循环；本步先保持局部改动，避免顺手重构。
