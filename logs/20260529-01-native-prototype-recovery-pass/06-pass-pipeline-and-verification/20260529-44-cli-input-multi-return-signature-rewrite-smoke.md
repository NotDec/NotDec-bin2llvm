# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

CLI `.ll` smoke 已覆盖多 input + 单 return、return-only 多返回，但还没有覆盖 input + 多返回。库层已经支持 RDI input 加 RAX/RDX output 转成 `{ i64, i64 } (i64)`。CLI 入口需要一个最小 smoke，确认文本 IR 路径能同时处理 input candidate 和多个 return candidate。

这一步只补 CLI smoke，不改 prototype recovery 算法。

# Ghidra 实现参考

Ghidra 的 prototype recovery 会在同一个函数原型里同时更新输入和输出 storage：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`：把 input trial 分配到 prototype input map。
  - `FuncCallSpecs::deriveOutputMap(...)`：把 output trial 分配到 prototype output map。
  - `ParamListStandard::assignMap(...)`：按 `ParamEntry` 顺序分配 input/output storage。
  - `FuncProto::updateAllTypes(...)`：把 input 和 output 的最终结果一起写入函数 prototype。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：应用 prototype recovery 的类型结果。

native 侧对应关系是：`NativePrototypeRecovery` 从 `notdec.register.external_inputs` 得到 input candidates，从 return 前 ABI output register store 得到 return candidates，统一写入 `notdec.prototype.recovered`。显式签名重写再用 recovered prototype 生成 LLVM `FunctionType`。

# native 侧复刻策略

- 扩展现有 CLI fixture：
  - 增加 `@cli_input_rdi_return_rax_rdx`。
  - 函数读取 RDI，分别写 RDX 和 RAX。
  - 复用现有 RDI input、RAX/RDX output ABI metadata。
- 扩展现有 CLI smoke：
  - 检查输出里有 `define { i64, i64 } @cli_input_rdi_return_rax_rdx(i64 %`。
  - summary rewrite seen / rewritten 从 5 调整为 6。
  - 保留 transient metadata 负向检查和 LLVM 22 verify。

暂时不做：

- 不补 direct caller 的 CLI `.ll` extractvalue 覆盖；库层已有 callsite 测试。
- 不补多 input + 多返回 CLI smoke；后续可单独补。
- 不拆 CMake 长命令。

# 判断标准

- CLI `.ll` 输入能把 RDI input 和 RAX/RDX return 一起改成 LLVM 函数签名。
- 输出 IR 里 `@cli_input_rdi_return_rax_rdx` 是 `{ i64, i64 } (i64)`。
- prototype recovery summary 看到 6 个 rewritten 函数。
- 全量测试继续通过。

# 风险

- 现有 CLI smoke 命令已经较长，继续追加覆盖会降低可读性。
- 这个 smoke 只证明 callee 本体签名重写，不证明 direct caller 的参数和 `extractvalue` 重写。

# 实现记录

## 改动

- `tests/ir/native-prototype/cli-signature-rewrite.ll:45` 添加 `@cli_input_rdi_return_rax_rdx`。
  - 函数读取 RDI external input。
  - 函数先写 RDX，再写 RAX，复用现有 RDI input 和 RAX/RDX output metadata。
- `tools/CMakeLists.txt:149` 扩展 CLI smoke。
  - 检查输出包含 `define { i64, i64 } @cli_input_rdi_return_rax_rdx(i64 %`。
  - summary expected seen / rewritten 从 5 调整为 6。
  - 继续使用 LLVM 22 `llvm-as` / `opt -passes=verify` 验证输出 IR。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本小步完成。

## 验证

已通过：

```sh
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build -R 'notdec.native_llvm.cli_signature_rewrite_ll_smoke' -V
```

结果：目标 smoke `1/1 ... Passed`，约 `0.12 sec`。

生成 IR 里确认：

```llvm
define { i64, i64 } @cli_input_rdi_return_rax_rdx(i64 %RDI.external_input1)
```

summary 里确认：

```text
signature rewrite seen functions: 6
signature rewrite rewritten functions: 6
signature rewrite skipped functions: 0
function cli_input_rdi_return_rax_rdx: external_inputs=1 input_candidates=1 return_candidates=2 rewrite_eligible=1 needs_signature_rewrite=1
```

## 性能影响

只改测试 fixture 和 CTest 检查，不改运行时代码。没有新增 pass 或算法开销。

## 评分

- 实现效果：6/10。补上 CLI text IR 的 input + multi-return 覆盖，但不是新恢复能力。
- 复杂度：3/10。复用已有 metadata 和 smoke，只新增一个函数形状。
- 后期维护成本：3/10。CTest 命令更长，后续继续加形状时应考虑拆成脚本。
