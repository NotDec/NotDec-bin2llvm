# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

CLI `.ll` smoke 已覆盖单返回和多 input，但还没有覆盖多返回寄存器。库层已经支持 RAX/RDX 这类多 output slot 转成 LLVM struct return。CLI 入口需要一个最小 smoke，确认文本 IR 路径也能把多个 ABI output register 合成 `{ i64, i64 }` 返回类型。

这一步只补 return-only 多返回 CLI smoke，不改 recovery 算法。

# Ghidra 实现参考

Ghidra 的返回值恢复会把多个 output storage 放进同一个 prototype output map，而不是只保留第一个寄存器：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveOutputMap(...)`：根据 output trial 和 prototype model 生成返回 storage map。
  - `ParamListStandard::assignMap(...)`：按 `ParamEntry` 顺序分配 output storage。
  - `FuncProto::updateAllTypes(...)`：把 input/output map 一起写入函数 prototype。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：触发 prototype 类型恢复。

native 侧对应关系是：`NativePrototypeRecovery` 从 return 前 ABI output register store 收集 `notdec.prototype.return_candidates`，按 ABI slot 排序写入 `notdec.prototype.recovered`。显式签名重写遇到多个 return slot 时，用 LLVM struct return 表达多寄存器返回。

# native 侧复刻策略

- 扩展现有 CLI fixture：
  - 增加 `@RDX` register global 和 ABI output pentry。
  - 增加 `@cli_return_rax_rdx`，函数写 RDX 和 RAX。
  - 故意按 RDX、RAX 的 store 顺序写，检查 recovered output 仍按 ABI slot 生成 `{ RAX, RDX }`。
- 扩展现有 CLI smoke：
  - 检查输出里有 `define { i64, i64 } @cli_return_rax_rdx()`。
  - summary rewrite seen / rewritten 从 4 调整为 5。
  - 保留 transient metadata 负向检查和 LLVM 22 verify。

暂时不做：

- 不补 direct caller 的 CLI `.ll` 多返回 extractvalue 覆盖；库层已有 direct callsite 测试。
- 不补 input + multi-return CLI smoke；后续可单独做。
- 不拆 CMake 长命令。

# 判断标准

- CLI `.ll` 输入能把 RAX、RDX 两个 return candidate 改成 LLVM struct return。
- 输出 IR 里 `@cli_return_rax_rdx` 是 `{ i64, i64 } ()`。
- prototype recovery summary 看到 5 个 rewritten 函数。
- 全量测试继续通过。

# 风险

- 手写 metadata fixture 继续变长，后续可以在覆盖更多 CLI 形状前考虑拆脚本。
- 这个 smoke 只证明 callee 本体多返回签名重写，不证明 callsite extractvalue 路径。

# 实现记录

## 改动

- `tests/ir/native-prototype/cli-signature-rewrite.ll:4` 添加 `@RDX` register global。
- `tests/ir/native-prototype/cli-signature-rewrite.ll:38` 添加 `@cli_return_rax_rdx`。
  - 函数先写 RDX，再写 RAX，确认 rewrite 不依赖 store 出现顺序。
- `tests/ir/native-prototype/cli-signature-rewrite.ll:56` 将 ABI output pentry 扩展为 RAX、RDX。
  - `tests/ir/native-prototype/cli-signature-rewrite.ll:65` 到 `:68` 添加 RDX register access 和 output storage metadata。
- `tools/CMakeLists.txt:149` 扩展 CLI smoke。
  - 检查输出包含 `define { i64, i64 } @cli_return_rax_rdx()`。
  - summary expected seen / rewritten 从 4 调整为 5。
  - 继续使用 LLVM 22 `llvm-as` / `opt -passes=verify` 验证输出 IR。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本小步完成。

## 验证

已通过：

```sh
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build -R 'notdec.native_llvm.cli_signature_rewrite_ll_smoke' -V
```

结果：目标 smoke `1/1 ... Passed`，约 `0.11 sec`。

生成 IR 里确认：

```llvm
define { i64, i64 } @cli_return_rax_rdx()
```

summary 里确认：

```text
signature rewrite seen functions: 5
signature rewrite rewritten functions: 5
signature rewrite skipped functions: 0
function cli_return_rax_rdx: external_inputs=0 input_candidates=0 return_candidates=2 rewrite_eligible=1 needs_signature_rewrite=1
```

## 性能影响

只改测试 fixture 和 CTest 检查，不改运行时代码。没有新增 pass 或算法开销。

## 评分

- 实现效果：6/10。补上 CLI text IR 的 return-only 多返回覆盖，但不是新恢复能力。
- 复杂度：3/10。多了 RDX output slot 和 struct return 检查，仍局限在现有 smoke。
- 后期维护成本：3/10。fixture metadata 更长，后续最好避免无限扩展同一条 CTest 命令。
