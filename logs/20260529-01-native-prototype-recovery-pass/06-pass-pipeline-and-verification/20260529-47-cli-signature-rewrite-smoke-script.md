# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

CLI `.ll` 显式签名重写 smoke 已经覆盖多种 input/return 形状、summary、metadata 清理和旧 return store 删除。现在这些检查都塞在 `tools/CMakeLists.txt` 的一条长命令里，继续维护会很难看清每个断言的意图。

这一步只把现有 smoke 命令搬到脚本里，行为保持不变，不改 prototype recovery 算法和 fixture。

# Ghidra 实现参考

Ghidra 的 prototype recovery 验证重点不是命令行脚本形式，而是恢复结果要能稳定落到同一个 `FuncProto`：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`：生成 input storage map。
  - `FuncCallSpecs::deriveOutputMap(...)`：生成 output storage map。
  - `FuncProto::updateAllTypes(...)`：把 input/output 恢复结果应用到函数原型。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：驱动 prototype 类型恢复。

native 侧的 CLI smoke 目标是确认 `notdec-native-llvm` 的 `.ll` 输入路径能稳定执行同一条恢复和重写链路。把断言放到脚本里，可以让后续继续补具体 Ghidra 对应形状时不再污染 CMake。

# native 侧复刻策略

- 新增 `scripts/native-llvm-cli-signature-rewrite-smoke.sh`。
  - 参数：`notdec-native-llvm` 路径、fixture 路径、build 目录、LLVM bin 目录。
  - 保留现有所有检查：签名形状、transient metadata 清理、旧 return register store 删除、summary 计数、LLVM 22 verify。
- `tools/CMakeLists.txt` 中的 CTest 改为调用脚本。
- 不改 fixture，不新增算法逻辑。

暂时不做：

- 不把其它 smoke 一起脚本化。
- 不拆分多个 CTest；保持现有测试名和覆盖面。

# 判断标准

- CTest 仍叫 `notdec.native_llvm.cli_signature_rewrite_ll_smoke`。
- 目标 smoke 和全量测试继续通过。
- CMake 中不再保留整条长 grep 命令。

# 风险

- 脚本参数传错会导致 CTest 找不到 binary、fixture 或 LLVM 22 工具。
- shell 脚本需要保持可执行权限。

# 实现记录

## 改动

- `scripts/native-llvm-cli-signature-rewrite-smoke.sh:1` 新增 CLI `.ll` 显式签名重写 smoke 脚本。
  - `scripts/native-llvm-cli-signature-rewrite-smoke.sh:25` 到 `:55` 添加可执行文件、输入文件和文本断言 helper。
  - `scripts/native-llvm-cli-signature-rewrite-smoke.sh:63` 到 `:69` 调用 `notdec-native-llvm` 的 `.ll` 输入路径，并打开 summary 和显式签名重写。
  - `scripts/native-llvm-cli-signature-rewrite-smoke.sh:71` 到 `:89` 保留原有签名、metadata 清理、旧 return store 删除和 summary 检查。
  - `scripts/native-llvm-cli-signature-rewrite-smoke.sh:91` 到 `:92` 继续使用 LLVM 22 `llvm-as` / `opt -passes=verify` 验证输出 IR。
- `tools/CMakeLists.txt:148` 到 `:152` 将 `notdec.native_llvm.cli_signature_rewrite_ll_smoke` 改为调用脚本。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本小步完成。

## 验证

已通过：

```sh
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build -R 'notdec.native_llvm.cli_signature_rewrite_ll_smoke' -V
```

结果：目标 smoke `1/1 ... Passed`，约 `0.12 sec`。CTest 命令已变成直接调用：

```text
scripts/native-llvm-cli-signature-rewrite-smoke.sh .../notdec-native-llvm .../cli-signature-rewrite.ll .../build .../llvm-22.1.0.obj/bin
```

## 性能影响

只移动测试断言位置，不改运行时代码。没有新增 pass 或算法开销。

## 评分

- 实现效果：5/10。降低 CLI smoke 后续维护成本，但不是新恢复能力。
- 复杂度：2/10。新增一个参数明确的 shell 脚本，CMake 只保留调用。
- 后期维护成本：2/10。后续加断言改脚本即可，不需要继续扩展 CMake 长命令。
