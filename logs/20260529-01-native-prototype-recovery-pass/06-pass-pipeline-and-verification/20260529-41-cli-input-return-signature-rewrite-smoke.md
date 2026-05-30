# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

CLI `.ll` smoke 现在覆盖了 input-only 和 return-only 显式签名重写，但还没有覆盖同一个函数同时恢复参数和返回值的路径。库层已经有 input+return rewrite 测试；CLI 入口还需要一个轻量 smoke，确认文本 IR 路径能把 register input 和 register output 一起转成 LLVM 函数类型。

这一步只补 CLI smoke，不改 recovery 语义。

# Ghidra 实现参考

Ghidra 会在同一轮 prototype recovery 中同时处理输入 trial 和输出 trial，最后写入同一个 `FuncProto`：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`：根据 input storage 使用生成参数 map。
  - `FuncCallSpecs::deriveOutputMap(...)`：根据 output storage 使用生成返回值 map。
  - `FuncProto::updateAllTypes(...)`：把 input/output 结果一起应用到函数原型。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：应用恢复出的 prototype 类型。

native 侧对应路径是：`notdec.register.external_inputs` 生成 input candidate，ABI output register store 生成 return candidate，最后 `notdec.prototype.recovered` 同时带 input 和 return。

# native 侧复刻策略

- 扩展现有 CLI fixture：
  - 增加 `@cli_input_rdi_return_rax`。
  - 函数体读取 RDI external input，做一个简单加法，写入 RAX register store。
- 扩展现有 CLI smoke：
  - 检查输出里有 `define i64 @cli_input_rdi_return_rax(i64 %...)`。
  - summary rewrite seen / rewritten 从 2 调整为 3。
  - 保留现有 transient metadata 负向检查和 LLVM 22 verify。

暂时不做：

- 不覆盖 direct caller 的参数/返回 load 重写。
- 不覆盖多 input 或多 return；这些已有库层测试，CLI 后续可以按需再补。
- 不新增单独 CTest。

# 判断标准

- CLI `.ll` 输入能同时完成 input-only、return-only、input+return 三种显式签名重写。
- 输出 IR 里 `@cli_input_rdi_return_rax` 是 `i64 (i64)`。
- prototype recovery summary 看到 3 个 rewritten 函数。
- 全量测试继续通过。

# 风险

- 仍是手写 metadata fixture，metadata 格式变化时需要同步更新。
- 这个 smoke 不证明 direct callsite 重写；只证明 callee 本体签名和返回值路径。

# 实现记录

## 改动

- `tests/ir/native-prototype/cli-signature-rewrite.ll:19` 添加 `@cli_input_rdi_return_rax`。
  - 函数读取 RDI external input，计算后写入 RAX ABI output register。
  - 这个 fixture 复用现有 RDI input 和 RAX output metadata。
- `tools/CMakeLists.txt:149` 扩展 CLI smoke 检查。
  - 检查输出包含 `define i64 @cli_input_rdi_return_rax(i64 %`。
  - summary expected seen / rewritten 从 2 调整为 3。
  - 继续检查 rewritten IR 不保留 transient prototype metadata，并继续用 LLVM 22 `llvm-as` / `opt -passes=verify` 验证。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本小步完成。

## 验证

已通过：

```sh
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build -R 'notdec.native_llvm.cli_signature_rewrite_ll_smoke' -V
```

结果：目标 smoke `1/1 ... Passed`，约 `0.11 sec`。

## 性能影响

只改测试 fixture 和 CTest 检查，不改运行时代码。没有新增 pass 或算法开销。

## 评分

- 实现效果：6/10。补上 CLI text IR 的 input+return 覆盖，但不是新语义能力。
- 复杂度：2/10。只扩展现有 fixture 和一条 smoke 命令。
- 后期维护成本：2/10。主要成本是手写 metadata 跟随格式变化。
