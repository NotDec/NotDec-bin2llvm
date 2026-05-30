# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

CLI `.ll` smoke 已覆盖 input-only、return-only、单 input + 单 return。库层已经有多 input + 单 return 的签名和 direct callsite 重写测试，但 CLI 入口还没有覆盖 ABI input 顺序在文本 IR 路径里能稳定落成函数参数列表。

这一步只补 CLI smoke，不改 prototype recovery 算法。

# Ghidra 实现参考

Ghidra 的多参数恢复不是按使用出现顺序直接生成参数，而是由 prototype model 的 storage 规则排序和筛选：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamListStandard::assignMap(...)`：根据 `ParamEntry` 规则把候选 storage 分配为参数列表。
  - `ParamEntry::justifiedContain(...)`：判断某个 varnode storage 是否落在参数 entry 内。
  - `FuncCallSpecs::deriveInputMap(...)`：从 input trial 推出最终 input map。
  - `FuncProto::updateAllTypes(...)`：把 input 和 output 类型写回函数原型。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：运行 prototype 类型恢复动作。

native 侧对应关系是：`NativePrototypeModel` 用 `!notdec.abi` 的 input pentry 顺序判断 RDI、RSI 等 ABI input slot，`NativePrototypeRecovery` 将 external input trial 按 slot 排序后写入 `notdec.prototype.recovered`，显式签名重写再用 recovered prototype 生成 LLVM `FunctionType`。

# native 侧复刻策略

- 扩展现有 CLI fixture：
  - 增加 `@RSI` register global 和对应 register metadata。
  - ABI input pentry 从只含 RDI 扩展为 RDI、RSI。
  - 增加 `@cli_input_rdi_rsi_return_rax`，函数读取 RDI/RSI，写 RAX。
- 扩展现有 CLI smoke：
  - 检查输出里有 `define i64 @cli_input_rdi_rsi_return_rax(i64 %, i64 %`。
  - summary rewrite seen / rewritten 从 3 调整为 4。
  - 保留 transient metadata 负向检查和 LLVM 22 verify。

暂时不做：

- 不补 direct caller 的 CLI `.ll` 覆盖；库层已经覆盖 callsite 参数和返回 load 重写。
- 不覆盖多返回寄存器 CLI smoke；后续可单独补。
- 不拆 CMake 长命令，避免把测试覆盖小步变成测试框架重构。

# 判断标准

- CLI `.ll` 输入能把 RDI、RSI 两个 input candidate 按 ABI slot 顺序改成两个 LLVM 参数。
- 输出 IR 里 `@cli_input_rdi_rsi_return_rax` 是 `i64 (i64, i64)`。
- prototype recovery summary 看到 4 个 rewritten 函数。
- 全量测试继续通过。

# 风险

- 手写 metadata fixture 变复杂，后续 metadata 格式调整时需要同步维护。
- 这个 smoke 只证明 CLI text IR 的 callee 本体签名重写，不证明真实 ELF 里多参数识别质量。

# 实现记录

## 改动

- `tests/ir/native-prototype/cli-signature-rewrite.ll:2` 添加 `@RSI` register global。
- `tests/ir/native-prototype/cli-signature-rewrite.ll:28` 添加 `@cli_input_rdi_rsi_return_rax`。
  - 函数读取 RDI、RSI 两个 external input。
  - 函数把两者相加后写入 RAX ABI output register。
- `tests/ir/native-prototype/cli-signature-rewrite.ll:45` 将 ABI input pentry 扩展为 RDI、RSI。
  - `tests/ir/native-prototype/cli-signature-rewrite.ll:52` 到 `:56` 添加 RSI register、external input 和 ABI storage metadata。
- `tools/CMakeLists.txt:149` 扩展 CLI smoke。
  - 检查输出包含 `define i64 @cli_input_rdi_rsi_return_rax(i64 %` 和第二个 `, i64 %` 参数。
  - summary expected seen / rewritten 从 3 调整为 4。
  - 保留 transient metadata 负向检查和 LLVM 22 verify。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本小步完成。

## 验证

已通过：

```sh
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build -R 'notdec.native_llvm.cli_signature_rewrite_ll_smoke' -V
```

结果：目标 smoke `1/1 ... Passed`，约 `0.11 sec`。

中间有一次目标 smoke 失败。原因是 CTest 仍使用重新生成前的旧命令，且最初 grep 把两个参数之间的参数名漏掉了。重新构建并放宽为函数签名前缀 + 第二参数检查后通过。

## 性能影响

只改测试 fixture 和 CTest 检查，不改运行时代码。没有新增 pass 或算法开销。

## 评分

- 实现效果：6/10。补上 CLI text IR 的多 input + 单 return 覆盖，但不是新恢复能力。
- 复杂度：3/10。metadata fixture 多了第二个 ABI input slot，仍局限在一个文件。
- 后期维护成本：3/10。手写 metadata 稍复杂，后续格式变动时要同步维护。
