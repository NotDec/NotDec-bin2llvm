# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

CLI smoke 已经覆盖显式签名重写后的二次运行：summary 必须显示所有函数都是 `already matches`，并且不能退化成 `missing recovered prototype`。

但二次运行产物目前只做文本检查，没有用 LLVM 22 重新 assemble 和 verify。这个缺口比较小，但会影响 pass pipeline 的基本稳定性判断。

# Ghidra 实现参考

Ghidra 里 prototype 恢复不是一次性文本转换。恢复出的 `FuncProto` 会继续参与后续 decompiler action：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::updateAllTypes(...)`：更新函数 prototype。
  - `FuncCallSpecs::deriveInputMap(...)` / `deriveOutputMap(...)`：从当前 trial 生成 input/output。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：在 action pipeline 中持续应用 prototype 类型结果。

native 侧对应要求是：显式签名重写后的 IR 再进入同一 CLI pipeline，输出仍必须是 LLVM verifier 接受的 IR。

# native 侧复刻策略

- 不改 pass 逻辑。
- 只扩展 `scripts/native-llvm-cli-signature-rewrite-smoke.sh`。
- 给 rerun 输出增加独立 `.bc` 和 verify `.bc` 路径。
- 在 `run_rerun_check(...)` 末尾使用项目指定 LLVM 22：
  - `llvm-as rerun.ll -o rerun.bc`
  - `opt -passes=verify rerun.bc -o rerun.opt.bc`
- `.ll` 输入和 `.bc` 输入两条 smoke 路径都覆盖。

暂时不做：

- 不增加新 fixture。
- 不扩大到 Bench2 全量。
- 不改变 summary 文本断言。

# 判断标准

- CLI signature rewrite smoke 通过。
- 全量 CTest 通过。
- 二次运行输出能被 LLVM 22 `llvm-as` 和 `opt -passes=verify` 接受。

# 风险

只增加测试脚本命令，风险主要是脚本参数变多后调用点漏改。当前脚本只在 CTest 一个地方调用，保持外部参数不变，新增路径在脚本内部生成。

# 实现记录

## 改动

- `scripts/native-llvm-cli-signature-rewrite-smoke.sh:24` 到 `:36` 增加二次运行产物的 `.bc` 和 verify `.bc` 路径。
  - `.ll` 输入路径使用 `RERUN_BC` / `RERUN_VERIFY_BC`。
  - `.bc` 输入路径使用 `RERUN_BC_FROM_BC` / `RERUN_VERIFY_BC_FROM_BC`。
- `scripts/native-llvm-cli-signature-rewrite-smoke.sh:118` 到 `:144` 扩展 `run_rerun_check(...)`。
  - 继续检查 recovered metadata 和 `already matches` summary。
  - 新增 LLVM 22 `llvm-as` assemble。
  - 新增 LLVM 22 `opt -passes=verify` 校验。
- `scripts/native-llvm-cli-signature-rewrite-smoke.sh:148` 到 `:157` 更新两处 `run_rerun_check(...)` 调用，分别传入独立输出路径。

## 验证

已通过：

```sh
ctest --test-dir build -R 'notdec.native_llvm.cli_signature_rewrite_ll_smoke' -V
git diff --check
ctest --test-dir build --output-on-failure
```

结果：

- `notdec.native_llvm.cli_signature_rewrite_ll_smoke` 通过，约 `0.48 sec`。
- 全量 CTest 通过，`9/9`，约 `1.31 sec`。

## 性能影响

不改生产代码。CLI smoke 多做两次二次运行产物 assemble/verify，本地该测试耗时约 `0.48 sec`，全量 CTest 约 `1.31 sec`。

## 评分

- 实现效果：5/10。补齐二次运行输出的 verifier 证据。
- 复杂度：1/10。只改脚本路径和校验命令。
- 后期维护成本：1/10。保持脚本外部参数不变。
