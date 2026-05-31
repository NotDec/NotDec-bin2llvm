# 84. input+multi-return callsite 支持单分量 shared successor PHI

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
> - 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
> - 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
> - 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

## 背景

第 81、82、83 步已经分别覆盖 return-only、input+return、无输入 multi-return 的 shared successor 返回值 PHI。剩下的相邻缺口是 input+multi-return：callsite 既要收集 call 前输入寄存器值，又要把 struct return 的某个分量接到 shared successor 中的旧返回寄存器 load。

Ghidra 侧对应点：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionParamDouble::apply(...)`
    - input/output trial 会在同一轮 prototype recovery 中一起更新。
  - `ActionActiveReturn::apply(...)`
    - 对 call output trial 做使用检查，并调用 `deriveOutputMap(...)` / `buildOutputFromTrials(...)`。
  - `ActionReturnRecovery::buildReturnOutput(...)`
    - 多个 output trial 会按 ABI 顺序组合成返回输出。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::buildInputFromTrials(...)`
    - 把 active input trial 接到 call input。
  - `FuncCallSpecs::buildOutputFromTrials(...)`
    - 把 active output trial 接到 call output，必要时组合多个返回 storage。
  - `FuncCallSpecs::collectOutputTrialVarnodes(...)`
    - 收集每个 output trial 对应的 varnode。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncCallSpecs`
  - `ParamActive`
  - `ParamTrial`
  - `FuncProto`

native 侧现在已经有两块可复用逻辑：input+multi-return direct callsite 会收集参数并生成 struct return call；shared successor 返回 load 可以通过 `replaceSharedSuccessorReturnLoad(...)` 接 PHI。本步只把这两块接起来。

## 目标

- 只覆盖单 input + multi-return direct callsite。
- 只要求一个返回分量在 shared successor 中被读取，其他返回分量可未使用。
- call 前输入值仍走现有 `callsiteInputValueBeforeCall(...)`。
- shared successor 中的返回分量 load 用 `extractvalue + PHI` 替换。
- 不扩展到多个返回分量同时位于 shared successor 的组合。

## 路线

新增一个 IR 单测：callee 读取 `RDI`，返回 `RDX/RAX`；caller 在 call block 写 `RDI` 后调用 callee，另一个前驱直接汇合到 `use_return`，`use_return` 只读取 `RAX`。期望 rewrite 后：

- 新 call 带一个参数；
- 新 call 返回 struct；
- call block 中只生成一个 `extractvalue`；
- shared successor 中有 PHI，call 路径 incoming 是 `extractvalue`；
- 旧 `RAX` load 不再有 use。

实现上扩展 input+multi-return callsite collection：

1. `InputMultiReturnCallsiteRewrite` 记录每个返回分量的 `ReturnLoadSearchResult` 和 register name。
2. `collectInputMultiReturnDirectCallsites(...)` 查返回 load 时传入 `allowSharedSuccessorLoad=true`。
3. `rewriteInputMultiReturnDirectCallsites(...)` 对 shared successor 分量复用 `replaceSharedSuccessorReturnLoad(...)`。

## 风险

- `extractvalue` 必须在 call block 中生成，再作为 PHI incoming；不能直接在 shared successor 中使用新 call。
- 输入参数收集和返回 load 收集必须都成功后才改 IR。
- 本步只补 input+multi-return 的单 shared successor 分量，多个分量组合后续再单独验证。

## 判断标准

- 新增 input+multi-return shared successor PHI 单测通过。
- 现有 return-only / input+return / multi-return PHI 单测继续通过。
- `notdec.native_prototype_recovery` 通过。
- 全量 `ctest` 通过。

## 实现记录

### 改动

- `lib/passes/NativePrototypeRecovery.cpp:582` 的 `InputMultiReturnCallsiteRewrite` 增加 `ReturnLoadResults` 和 `ReturnRegisterNames`，按 ABI return slot 保存返回 load 查找结果。
- `lib/passes/NativePrototypeRecovery.cpp:1067` 的 `collectInputMultiReturnDirectCallsites(...)` 查找每个返回分量时传入 `allowSharedSuccessorLoad=true`，并记录对应 register name。
- `lib/passes/NativePrototypeRecovery.cpp:1118` 的 `rewriteInputMultiReturnDirectCallsites(...)` 对 shared successor 返回分量用 `replaceSharedSuccessorReturnLoad(...)`，PHI 的 call 路径 incoming 使用 call block 中的 `extractvalue`。
- `tests/native_prototype_recovery_test.cpp:1883` 新增 `input_rdi_return_rdx_rax_shared` 和 `call_input_rdi_return_rdx_rax_shared` 测试函数。
- `tests/native_prototype_recovery_test.cpp:1962` 更新 summary 计数：新增 2 个函数、1 个 input candidate、2 个 return candidate、1 个需要签名重写的函数。
- `tests/native_prototype_recovery_test.cpp:4377` 新增 input+multi-return shared successor 断言：新 call 带 1 个参数，返回 struct，只生成 1 个 `extractvalue`，shared successor PHI 含 `extractvalue` incoming，旧 `RAX` load 不再有 use。

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

结果：`notdec-native-llvm` 构建通过；全量 `ctest` 9/9 通过，总耗时 1.34s。

第一次并行跑 `ctest` 和 `notdec-native-llvm` 构建时，`ctest` 先启动遇到正在重链的可执行文件，报过一次 `Permission denied` / `missing executable`。构建结束后重跑全量 `ctest` 已通过。

```bash
OUT_DIR=/tmp/notdec-bin2llvm-bench2-input-multi-return-shared-phi-smoke \
  scripts/bench2-native-smoke.sh --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-bench2-input-multi-return-shared-phi-smoke
```

结果：

| 用例 | 结果 | 时间 | seen | rewritten | skipped | skip reasons |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| vsftpd | ok | 88s | 236 | 139 | 97 | already matches 48, declaration 49 |
| libuv | ok | 226s | 571 | 338 | 233 | already matches 147, declaration 86 |
| memcached | ok | 121s | 315 | 188 | 127 | already matches 71, declaration 56 |

和第 83 步同口径 smoke 相比，rewrite 数量不变，耗时略有波动但没有明显退化。

### 评分

- 实现效果：8/10。补齐了 input+multi-return 的相邻 shared successor 形状，保持参数收集和 struct return extract 的 SSA 语义正确。
- 复杂度：5/10。只是在 input+multi-return callsite rewrite 里保存并使用已有 `ReturnLoadSearchResult`，没有新建一套逻辑。
- 维护成本：5/10。后续如果支持多个返回分量同时在 shared successor 中读取，需要继续补专门测试；当前实现结构可以承接。
