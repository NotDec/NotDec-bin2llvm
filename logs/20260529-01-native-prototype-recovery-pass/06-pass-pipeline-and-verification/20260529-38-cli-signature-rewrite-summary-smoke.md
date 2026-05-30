# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

当前 native prototype recovery 已经能在显式 opt-in 时重写 `.ll` 输入里的函数签名。上一小步只检查输出 IR 形态和 LLVM verify，没有检查 CLI summary 是否把这次 rewrite 统计出来。

这一步补 CLI 层面的 summary smoke。目标是确认工具入口 `--prototype-recovery-summary --rewrite-prototype-signatures` 能把 `NativePrototypeRecoverySummary` 里的签名重写统计打印出来，方便后续 Bench2 运行时直接看 rewrite 数量和 skip reason。

# Ghidra 实现参考

Ghidra decompiler 不只修改内部 prototype，也会保留 action 执行状态，方便调试和追踪 prototype 推导：

- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：把恢复出的 prototype 类型应用到当前函数和 call/return 相关 varnode。
  - `ActionFuncLink::apply(...)`：把函数间 prototype/call 关系接入 decompiler action 流程。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::updateAllTypes(...)`：按恢复结果更新函数原型。
  - `FuncCallSpecs::deriveInputMap(...)` / `deriveOutputMap(...)`：根据 callsite storage 使用生成 input/output map。

native 侧没有 Ghidra action trace。当前对应的可观测结果是 `NativePrototypeRecoverySummary` 和 `printNativePrototypeRecoverySummary(...)`。

# native 侧复刻策略

- 复用现有 `.ll` fixture `tests/ir/native-prototype/cli-signature-rewrite.ll`。
- 在 CLI smoke 命令里同时打开：
  - `--prototype-recovery-summary`
  - `--rewrite-prototype-signatures`
- 把 stderr 重定向到一个 summary 文件。
- 检查 summary 里出现：
  - `signature rewrite seen functions: 1`
  - `signature rewrite rewritten functions: 1`
  - `signature rewrite skipped functions: 0`

暂时不做：

- 不改 summary 文本格式。
- 不新增 JSON summary。
- 不扩大到 ELF smoke。

# 判断标准

- 新检查能证明 CLI opt-in rewrite 发生时 summary 也同步反映。
- 输出 IR 仍通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
- 全量测试继续通过。

# 风险

- 这是文本 smoke，summary 文案变化会导致测试失败。当前 summary 是面向人读的调试输出，这个风险可以接受。
- 只覆盖 `.ll` 路径，不覆盖 ELF 路径；ELF 路径后续 Bench2 smoke 单独补。

# 实现记录

改动：

- `tools/CMakeLists.txt:149` 扩展 `notdec.native_llvm.cli_signature_rewrite_ll_smoke`：
  - 增加 `--prototype-recovery-summary`。
  - 将 stderr 写到 `notdec-native-llvm-cli-signature-rewrite-summary.txt`。
  - 检查 summary 中的 `signature rewrite seen functions: 1`、`signature rewrite rewritten functions: 1`、`signature rewrite skipped functions: 0`。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本步骤完成。

验证：

```sh
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build -R 'notdec.native_llvm.cli_signature_rewrite_ll_smoke' -V
```

结果：通过，目标测试 `1/1 Test #9: notdec.native_llvm.cli_signature_rewrite_ll_smoke ... Passed`，耗时约 `0.10 sec`。

性能：只扩展一个 CTest smoke 的文本检查，不改 runtime 代码。对 pass 运行性能无影响。

评分：

- 实现效果：5/10。补上 CLI opt-in rewrite 的 summary 可观测性。
- 复杂度：1/10。只改测试命令。
- 维护成本：2/10。summary 文案变化时需要同步更新 smoke。
