# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

当前库层已经在显式签名重写后清理 rewritten 函数上的中间 metadata：

- `notdec.register.external_inputs`
- `notdec.prototype.input_candidates`
- `notdec.prototype.return_candidates`

CLI `.ll` smoke 现在只检查函数签名被改写、summary 统计正确、输出能通过 LLVM verify。还没有确认工具入口输出的 IR 也不再带这些 trial 阶段 metadata。

# Ghidra 实现参考

Ghidra 会把 prototype trial 收敛成最终 `FuncProto`，trial 不会作为最终函数语义继续挂在函数上：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamActive::sortTrials()`：整理当前 trial。
  - `FuncCallSpecs::deriveInputMap(...)` / `deriveOutputMap(...)`：把 trial 转成最终 input/output map。
  - `FuncProto::updateAllTypes(...)`：把恢复结果应用到函数原型。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：应用恢复出的 prototype 类型。

native 侧的 `input_candidates` / `return_candidates` 是 trial 阶段结果；显式签名重写成功后，输出 IR 应该只保留真正有用的函数签名和必要 metadata。

# native 侧复刻策略

- 复用现有 CLI `.ll` smoke fixture。
- 在 `notdec.native_llvm.cli_signature_rewrite_ll_smoke` 里增加负向检查：
  - 输出 IR 不包含 `notdec.register.external_inputs`
  - 输出 IR 不包含 `notdec.prototype.input_candidates`
  - 输出 IR 不包含 `notdec.prototype.return_candidates`
- 仍保留现有函数签名、summary、LLVM 22 verify 检查。

暂时不做：

- 不清理或检查 `notdec.prototype.recovered`。
- 不新增新的 fixture。
- 不扩大到 ELF smoke。

# 判断标准

- CLI 输出的 rewritten `.ll` 不再带 trial 阶段 metadata。
- 现有 CLI rewrite smoke 仍通过。
- 全量测试继续通过。

# 风险

- 这是文本检查，metadata 名字变化时需要同步更新。
- 当前 fixture 是 input-only，只能实际覆盖 `external_inputs` 和 `input_candidates`；`return_candidates` 检查是防止该文件意外出现同类中间 metadata。

# 实现记录

改动：

- `tools/CMakeLists.txt:149` 扩展 `notdec.native_llvm.cli_signature_rewrite_ll_smoke`，在输出 `.ll` 上增加负向检查：
  - 不包含 `notdec.register.external_inputs`
  - 不包含 `notdec.prototype.input_candidates`
  - 不包含 `notdec.prototype.return_candidates`
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本步骤完成。

验证：

```sh
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build -R 'notdec.native_llvm.cli_signature_rewrite_ll_smoke' -V
```

结果：通过，目标测试 `1/1 Test #9: notdec.native_llvm.cli_signature_rewrite_ll_smoke ... Passed`，耗时约 `0.10 sec`。

性能：只扩展一个 CTest smoke 的文本检查，不改 runtime 代码。对 pass 运行性能无影响。

评分：

- 实现效果：5/10。CLI 输出也覆盖 rewritten 函数不保留 trial metadata。
- 复杂度：1/10。只改测试命令。
- 维护成本：2/10。metadata 名字变化时需要同步 smoke。
