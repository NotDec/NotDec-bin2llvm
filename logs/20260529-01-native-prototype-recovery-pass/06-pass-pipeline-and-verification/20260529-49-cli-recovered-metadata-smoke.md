# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

CLI signature rewrite smoke 已检查中间 trial metadata 会被清掉，例如 `notdec.prototype.input_candidates` 和 `notdec.prototype.return_candidates`。但最终恢复结果 `notdec.prototype.recovered` 仍应保留，因为后续 pass 或调试工具需要知道这个函数是从哪些 ABI storage 恢复出的原型。

这一步只补 CLI 输出检查，不改 prototype recovery 算法。

# Ghidra 实现参考

Ghidra 在 prototype recovery 结束后会把最终结果保留在函数原型对象里，而不是只留下临时 trial：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`：从 input trial 得到最终 input storage。
  - `FuncCallSpecs::deriveOutputMap(...)`：从 output trial 得到最终 output storage。
  - `FuncProto::updateAllTypes(...)`：把最终 input/output 结果写入 `FuncProto`。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：应用恢复出的 prototype 类型。

native 侧对应关系是：`notdec.prototype.input_candidates` / `return_candidates` 是 trial 阶段 metadata，显式签名重写后应该清理；`notdec.prototype.recovered` 对应最终 `FuncProto` 结果，应该保留。

# native 侧复刻策略

- 扩展 `scripts/native-llvm-cli-signature-rewrite-smoke.sh`。
  - 在 `.ll` 和 `.bc` 输入两条路径的 rewritten 输出上检查 `notdec.prototype.recovered` 仍存在。
  - 检查输出中仍有 `input_count=2` 和 `return_count=2`，固定多 input + 多 return 的 recovered prototype 记录。
- 不改 fixture，不改 CMake，不改恢复逻辑。

暂时不做：

- 不逐个检查每个 metadata 节点的 RDI/RSI/RAX/RDX 顺序；脚本目前只固定存在性和最大组合形状。
- 不新增单独 CTest。

# 判断标准

- `.ll` 输入 rewritten 输出保留 `notdec.prototype.recovered`。
- `.bc` 输入 rewritten 输出保留 `notdec.prototype.recovered`。
- 输出中能看到 `input_count=2` 和 `return_count=2`。
- 全量测试继续通过。

# 风险

- 这是文本层检查，不能替代 metadata reader 的结构化单元测试。
- 如果 LLVM metadata 打印格式变化，检查字符串可能需要同步调整。

# 实现记录

## 改动

- `scripts/native-llvm-cli-signature-rewrite-smoke.sh:91` 到 `:93` 扩展通用输出断言。
  - 检查 rewritten 输出保留 `notdec.prototype.recovered`。
  - 检查输出中存在 `input_count=2`。
  - 检查输出中存在 `return_count=2`。
- 因为这些断言位于 `run_signature_rewrite_check(...)`，所以 `.ll` 输入和 `.bc` 输入都会执行同一组检查。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本小步完成。

## 验证

已通过：

```sh
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build -R 'notdec.native_llvm.cli_signature_rewrite_ll_smoke' -V
```

结果：目标 smoke `1/1 ... Passed`，约 `0.28 sec`。

`.ll` 和 `.bc` 输出都确认包含 `notdec.prototype.recovered`，并且包含 `input_count=2` / `return_count=2` 的 recovered prototype metadata。

## 性能影响

只增加三条文本检查，不改运行时代码。目标 smoke 耗时仍约 `0.28 sec`。

## 评分

- 实现效果：5/10。固定最终 recovered metadata 保留结果，但不是新恢复能力。
- 复杂度：1/10。只新增三个通用脚本断言。
- 后期维护成本：2/10。依赖 LLVM metadata 文本格式，和现有 CLI smoke 断言一致。
