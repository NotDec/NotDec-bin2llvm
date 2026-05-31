# 80. CLI smoke 覆盖空 recovered prototype

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
> - 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
> - 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
> - 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

## 背景

第 79 步已经让 native pass 把没有输入、没有返回的函数记录成合法的空 recovered prototype。现在 C++ 单测覆盖了这个语义，但 CLI smoke 还只覆盖有参数或有返回值的函数。

Ghidra 侧对应的是 `FuncProto` 可以表达 0 参数、0 返回：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto`
  - `FuncProto::numParams()`
  - `FuncProto::deriveInputMap(...)`
  - `FuncProto::deriveOutputMap(...)`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionParamDouble::apply(...)`
  - `ActionReturnRecovery::apply(...)`

这里不新增恢复逻辑，只把命令行验证补上，确认 `.ll` 和 `.bc` 输入路径都保留空 recovered prototype。

## 目标

- 在 CLI signature rewrite fixture 里加入一个 `void()` 函数。
- smoke 脚本检查输出里有 `input_count=0` / `return_count=0`。
- 首次 rewrite 时，空 `void()` 函数应作为 `already matches` 跳过，其余 7 个函数继续 rewrite。
- 二次运行时，8 个函数都应是 `already matches`。

## 路线

改 `tests/ir/native-prototype/cli-signature-rewrite.ll` 加一个无候选函数。改 `scripts/native-llvm-cli-signature-rewrite-smoke.sh` 的计数和字符串检查。这个测试已经同时覆盖 `.ll` 和 `.bc` 输入，所以不需要另建脚本。

## 风险

这一步只改测试，不改 pass。主要风险是 summary 计数写错，导致 smoke 和真实语义不一致。

## 判断标准

- `notdec.native_llvm.cli_signature_rewrite_ll_smoke` 通过。
- 全量 `ctest` 通过。

## 实现记录

改动文件：

- `tests/ir/native-prototype/cli-signature-rewrite.ll:7`
  - 新增 `cli_empty_recovered`，函数体是原始 `void()`，没有 input candidate，也没有 return candidate。
- `scripts/native-llvm-cli-signature-rewrite-smoke.sh:92`
  - `run_signature_rewrite_check` 检查输出保留 `define void @cli_empty_recovered()`。
- `scripts/native-llvm-cli-signature-rewrite-smoke.sh:101`
  - 检查输出 metadata 包含 `input_count=0` / `return_count=0`。
- `scripts/native-llvm-cli-signature-rewrite-smoke.sh:113`
  - 首次 rewrite 的 summary 改为 `seen=8`、`rewritten=7`、`skipped=1`，并检查 skip reason 是 `already matches`。
- `scripts/native-llvm-cli-signature-rewrite-smoke.sh:139`
  - 二次运行 summary 改为 `seen=8`、`rewritten=0`、`skipped=8`，并检查 8 个函数都是 `already matches`。

这一步没有改 pass 逻辑，只补 CLI smoke 覆盖，所以不单独跑 Bench2 性能对比。

## 验证

```bash
ctest --test-dir build -R 'notdec.native_llvm.cli_signature_rewrite_ll_smoke' -V
```

结果：通过。

```bash
ctest --test-dir build --output-on-failure
```

结果：9/9 通过，总耗时 1.34s。
