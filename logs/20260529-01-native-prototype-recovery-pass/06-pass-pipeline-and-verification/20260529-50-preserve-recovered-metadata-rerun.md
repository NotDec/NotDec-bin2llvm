# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

CLI rewritten 输出已经保留 `notdec.prototype.recovered`。但如果把 rewritten IR 再喂给 `notdec-native-llvm --rewrite-prototype-signatures`，当前 prototype recovery 会因为没有旧的 register external input / return store trial，把 recovered metadata 清掉。随后 module rewrite 只能报 `missing recovered prototype`，而不是 `already matches`。

这会破坏一个很重要的性质：显式签名重写后的 IR 应该能再次通过同一条 pass pipeline，并保持已经恢复出的原型信息。

# Ghidra 实现参考

Ghidra 的最终 prototype 不依赖每次都重新从 trial 推出来；一旦 `FuncProto` 已经持有恢复出的 input/output storage，后续动作会围绕这个 prototype 继续工作：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`：从 trial 推导 input map。
  - `FuncCallSpecs::deriveOutputMap(...)`：从 trial 推导 output map。
  - `FuncProto::updateAllTypes(...)`：把最终 input/output 结果写入 `FuncProto`。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：应用 prototype 类型恢复结果。

native 侧 `notdec.prototype.recovered` 对应最终 `FuncProto` 结果。trial metadata 可以清理，但 recovered metadata 在函数类型已经匹配时应该保留。

# native 侧复刻策略

- 修改 `runNativePrototypeRecovery(...)` 的每函数逻辑。
  - 如果本轮没有产生新的 input/return candidate，但函数已有可读的 `notdec.prototype.recovered`，并且当前 LLVM 函数类型已经匹配 recovered prototype，则保留该 metadata。
  - 这种函数的 rewrite eligibility 应该是 `already matches`，module rewrite summary 也应该计入 `already matches`。
  - 如果没有 ABI，仍按现有逻辑清理 stale metadata。
  - 如果函数类型不匹配旧 recovered prototype，仍允许本轮无候选时清掉它，避免保留 stale 结果。
- 补库层测试：rewritten 函数再次跑 recovery + rewrite 时，summary 里是 `already matches`，且 recovered metadata 仍在。
- 扩展 CLI smoke：对 rewritten `.ll` 再跑一次，检查 `already matches` skip reason，而不是 `missing recovered prototype`。

暂时不做：

- 不引入新的 metadata schema。
- 不尝试从已经重写的函数体反推出 recovered metadata；只保留可信且类型匹配的已有 metadata。

# 判断标准

- rewritten IR 二次运行不会丢失 `notdec.prototype.recovered`。
- 二次运行 summary 报 `signature rewrite skipped reason already matches`。
- 无 ABI stale metadata 清理测试继续通过。
- 全量测试继续通过。

# 风险

- 如果旧 recovered metadata 可读但本轮真实 ABI/函数语义已变化，类型匹配可能不足以证明它一定正确。这里先只保留“已重写输出再次进入同一 pipeline”的稳定场景。

# 实现记录

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:773` 到 `:776` 在每个函数开始 recovery 前读取旧的 `notdec.prototype.recovered`。
- `lib/passes/NativePrototypeRecovery.cpp:879` 到 `:889` 在本轮没有新 recovered prototype 时保留旧 metadata。
  - 旧 metadata 必须可读。
  - 旧 metadata 的 model 必须等于当前 ABI prototype name。
  - 当前 LLVM `FunctionType` 必须已经等于旧 recovered prototype 对应的类型。
  - 不满足这些条件时仍清掉 recovered metadata，避免保留 stale 结果。
- `tests/native_prototype_recovery_test.cpp:2937` 到 `:2956` 增加 opt-in rewrite 二次运行测试。
  - 第一次 rewrite 后再次运行 `runNativePrototypeRecovery(... RewriteSignatures=true)`。
  - 验证 rewritten 函数不会再次重写。
  - 验证 skip reason 中 `already matches` 为 2，`missing recovered prototype` 为 1。
  - 验证已重写函数仍保留 `notdec.prototype.recovered`。
- `scripts/native-llvm-cli-signature-rewrite-smoke.sh:24` 到 `:32` 增加二次运行输出路径。
- `scripts/native-llvm-cli-signature-rewrite-smoke.sh:114` 到 `:135` 增加 `run_rerun_check(...)`。
  - 检查二次运行输出保留 `notdec.prototype.recovered`。
  - 检查二次运行 summary 是 `rewritten=0`、`skipped=7`、`already matches=7`。
  - 检查不再出现 `missing recovered prototype` skip reason。
- `scripts/native-llvm-cli-signature-rewrite-smoke.sh:139` 和 `:144` 分别对 `.ll` 重写输出和 `.bc` 重写输出做二次运行检查。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本小步完成。

## 验证

已通过：

```sh
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
ctest --test-dir build -R 'notdec.native_llvm.cli_signature_rewrite_ll_smoke' -V
```

结果：

- `notdec.native_prototype_recovery.input_candidates` 通过，约 `0.04 sec`。
- `notdec.native_llvm.cli_signature_rewrite_ll_smoke` 通过，约 `0.34 sec`。

CLI 二次运行 summary 确认 `.ll` 和 `.bc` 两条路径都是：

```text
signature rewrite seen functions: 7
signature rewrite rewritten functions: 0
signature rewrite skipped functions: 7
signature rewrite skipped reason already matches: 7
```

中间验证时误用了不存在的构建 target：

```sh
cmake --build build --target notdec-native-llvm notdec-native-prototype-recovery-test -j2
```

其中 `notdec-native-llvm` 已构建成功，`notdec-native-prototype-recovery-test` 这个 target 不存在。随后改用正确的 CTest 测试名验证通过。

## 性能影响

运行时代码只在每个函数 recovery 时读取旧 recovered metadata，并在无新候选时做一次函数类型比较。CLI smoke 多跑两次 rerun 检查，目标测试耗时约从 `0.28 sec` 增到 `0.34 sec`。

## 评分

- 实现效果：7/10。修复已重写 IR 二次进入 pipeline 时丢失 recovered prototype 的问题。
- 复杂度：3/10。只在无新候选时保留类型匹配的旧 metadata。
- 后期维护成本：3/10。保留条件依赖 recovered metadata reader 和 function type 构造逻辑，和现有 rewrite eligibility 共享判断。
