# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

CLI `.ll` smoke 目前只覆盖 input-only 签名重写：`void @cli_input_rdi()` 变成 `void @cli_input_rdi(i64)`。库层已经覆盖 return-only rewrite，但工具入口还没有文本 IR smoke 证明 `.ll` 路径会把 ABI output register store 改成 LLVM return value。

这一步只补 CLI smoke，不改 prototype recovery 语义。

# Ghidra 实现参考

Ghidra 的返回值恢复从 return 点附近的 output storage 推出最终函数返回：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveOutputMap(...)`：根据 output storage 使用生成返回值 map。
  - `FuncProto::updateAllTypes(...)`：把恢复出的 output map 应用到函数原型。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionReturnRecovery::apply(...)`：从 return 相关 varnode 收集返回 trial。
  - `ActionPrototypeTypes::apply(...)`：把恢复出的类型应用到函数。

native 侧对应路径是：`notdec.register.access` store 到 ABI output register -> `notdec.prototype.return_candidates` -> `notdec.prototype.recovered` -> 显式签名重写生成 LLVM return type。

# native 侧复刻策略

- 扩展现有 CLI fixture `tests/ir/native-prototype/cli-signature-rewrite.ll`：
  - 增加 `@RAX` register global。
  - 增加 `@cli_return_rax`，函数体写常量到 `@RAX`，store 带 `notdec.register.access` metadata。
  - ABI metadata 的 output 列表加入 RAX。
- 扩展现有 CTest：
  - 检查输出里有 `define i64 @cli_return_rax()`。
  - 继续检查输出能被 LLVM 22 `llvm-as` 和 `opt -passes=verify` 接受。
  - summary 统计从 1 个 rewrite 调整为 2 个 rewrite。

暂时不做：

- 不覆盖 direct caller 的 return load 重写。
- 不覆盖多返回 struct return。
- 不新增单独 CTest，先复用当前 CLI `.ll` smoke。

# 判断标准

- CLI `.ll` 输入能同时完成 input-only 和 return-only 显式签名重写。
- 输出 IR 里 `@cli_return_rax` 是 `i64 ()`。
- prototype recovery summary 看到 2 个 rewritten 函数。
- 全量测试继续通过。

# 风险

- 手写 metadata 容易和 reader 不一致。控制方法是复用现有 metadata 形状，只增加 RAX output 和 `notdec.register.access` store。
- 这个 smoke 不证明 caller return load 重写；那是另一个已有库层能力。

# 实现记录

改动：

- `tests/ir/native-prototype/cli-signature-rewrite.ll:2` 增加 `@RAX` register global。
- `tests/ir/native-prototype/cli-signature-rewrite.ll:13` 增加 `@cli_return_rax`，向 `@RAX` 写常量并带 `notdec.register.access` metadata。
- `tests/ir/native-prototype/cli-signature-rewrite.ll:26` 把 ABI metadata 的 output 列表改成包含 RAX。
- `tools/CMakeLists.txt:149` 扩展 `notdec.native_llvm.cli_signature_rewrite_ll_smoke`：
  - 检查输出包含 `define i64 @cli_return_rax()`。
  - summary rewrite seen / rewritten 期望从 1 调整为 2。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本步骤完成。

验证：

```sh
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build -R 'notdec.native_llvm.cli_signature_rewrite_ll_smoke' -V
```

结果：通过，目标测试 `1/1 Test #9: notdec.native_llvm.cli_signature_rewrite_ll_smoke ... Passed`，耗时约 `0.10 sec`。

性能：只扩展一个 CLI smoke fixture，不改 runtime 代码。对 pass 运行性能无影响。

评分：

- 实现效果：6/10。CLI `.ll` 路径现在同时覆盖 input-only 和 return-only rewrite。
- 复杂度：2/10。只加一个最小 fixture 函数和文本检查。
- 维护成本：2/10。手写 metadata 需要随 ABI metadata 格式同步。
