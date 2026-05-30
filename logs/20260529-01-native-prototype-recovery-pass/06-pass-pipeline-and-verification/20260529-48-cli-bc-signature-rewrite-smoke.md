# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

GOAL 里阶段 6 明确要求 `.ll` / `.bc` 输入也能跑 pass。当前 CLI signature rewrite smoke 只用 `.ll` fixture。工具代码已经把 `.ll` 和 `.bc` 都识别为 IR 输入，但 smoke 还没有固定 `.bc` 路径。

这一步只扩展现有脚本：先用 LLVM 22 `llvm-as` 把 fixture 汇编成 `.bc`，再用同一组断言跑一次 `notdec-native-llvm` 的 `.bc` 输入。

# Ghidra 实现参考

Ghidra 的 prototype recovery 关注的是已经进入中间表示后的 storage 和 prototype，不依赖输入文件是文本还是二进制格式：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`：生成 input storage map。
  - `FuncCallSpecs::deriveOutputMap(...)`：生成 output storage map。
  - `FuncProto::updateAllTypes(...)`：把 input/output 恢复结果写入函数原型。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：应用 prototype 类型恢复结果。

native 侧对应要求是：`tools/notdec-native-llvm.cpp` 的 IR 输入路径读入 `.ll` 或 `.bc` 后，都应运行同一条 prototype recovery / signature rewrite 链路。

# native 侧复刻策略

- 扩展 `scripts/native-llvm-cli-signature-rewrite-smoke.sh`。
  - 把现有 `.ll` 输入检查封成一个小函数。
  - 先跑 `.ll` 输入，保持现有输出文件名和检查。
  - 用 LLVM 22 `llvm-as` 把原始 fixture 汇编成 input `.bc`。
  - 再跑 `.bc` 输入，使用独立输出文件和 summary 文件，复用同一组断言。
- 不改 `notdec-native-llvm` 逻辑，不改 fixture。

暂时不做：

- 不新增单独 CTest；复用现有 CLI signature rewrite smoke。
- 不检查 `.bc` 输出字节级一致；只检查可观察签名、metadata 清理、summary 和 verify。

# 判断标准

- 目标 smoke 同时跑 `.ll` 输入和 `.bc` 输入。
- `.bc` 输入输出满足同样签名、metadata、summary 和 LLVM verify 检查。
- 全量测试继续通过。

# 风险

- 脚本会多跑一次 `notdec-native-llvm`，目标 smoke 时间略增。
- `.bc` 输入由同一个 fixture 汇编而来，只覆盖 IR bitcode 路径，不覆盖 ELF 生成 bitcode 的其它入口。

# 实现记录

## 改动

- `scripts/native-llvm-cli-signature-rewrite-smoke.sh:24` 到 `:28` 新增 `.bc` 输入和 `.bc` 输入输出相关临时文件路径。
- `scripts/native-llvm-cli-signature-rewrite-smoke.sh:68` 到 `:105` 将原有 signature rewrite 断言封成 `run_signature_rewrite_check(...)`。
  - 保留签名形状检查。
  - 保留旧 return register store 和 transient metadata 负向检查。
  - 保留 summary seen / rewritten / skipped 检查。
  - 保留 LLVM 22 `llvm-as` / `opt -passes=verify`。
- `scripts/native-llvm-cli-signature-rewrite-smoke.sh:107` 到 `:112` 先跑 `.ll` 输入，再用 LLVM 22 `llvm-as` 汇编 fixture 成 `.bc` 并复用同一组断言跑 `.bc` 输入。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本小步完成。

## 验证

已通过：

```sh
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build -R 'notdec.native_llvm.cli_signature_rewrite_ll_smoke' -V
```

结果：目标 smoke `1/1 ... Passed`，约 `0.26 sec`。

`.bc` 输入生成的输出确认包含：

```llvm
define { i64, i64 } @cli_input_rdi_rsi_return_rax_rdx(i64 %RDI.external_input1, i64 %RSI.external_input2)
```

`.bc` 输入 summary 确认：

```text
signature rewrite seen functions: 7
signature rewrite rewritten functions: 7
signature rewrite skipped functions: 0
```

## 性能影响

只改测试脚本，不改运行时代码。目标 smoke 多跑一次 `notdec-native-llvm` 和一次 fixture `llvm-as`，本机目标测试耗时约从 `0.12 sec` 增到 `0.26 sec`。

## 评分

- 实现效果：6/10。补上 GOAL 要求里的 `.bc` IR 输入路径 smoke。
- 复杂度：2/10。复用同一组断言，避免复制检查。
- 后期维护成本：2/10。后续改签名断言只需要改一个 helper。
