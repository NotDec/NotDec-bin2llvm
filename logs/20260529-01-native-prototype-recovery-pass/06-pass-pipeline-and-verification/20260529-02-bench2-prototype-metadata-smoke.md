# 20260529-02 Bench2 Prototype Metadata Smoke

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的 prototype recovery 结果会随 decompiler pipeline 固定产出，不需要调用方额外手动跑候选分析。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionActiveParam::apply(...)`：从 active input trials 派生参数。
  - `ActionActiveReturn::apply(...)`：从 call output trials 派生调用返回值。
  - `ActionReturnRecovery::apply(...)`：从当前函数 return 前状态派生返回值。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamActive::registerTrial(...)`：登记候选 varnode。
  - `ParamListStandard::fillinMap(...)`：按 input storage 规则筛参数。
  - `ParamListStandardOut::fillinMap(...)`：按 output storage 规则筛返回值。

对应到 native 侧，Bench2 smoke 不复刻算法，只检查 pipeline 的可观察结果：真实目标生成的 IR 里必须有 ABI metadata、register SSA metadata、prototype input/return candidate metadata。

## native 复刻方式

现有 `scripts/bench2-native-smoke.sh` 已经跑 `vsftpd`、`libuv`、`memcached`，并用 LLVM 22 做 `llvm-as` 和 `opt -passes=verify`。这一步只加最小检查：

- 在脚本中新增 `check_prototype_metadata(...)`。
- 对 all-confirmed IR 检查：
  - 有 `!notdec.abi`。
  - 有 `!notdec.register.external_inputs`。
  - 有 `!notdec.prototype.input_candidates`。
  - 有 `!notdec.prototype.return_candidates`。
- 在 `check_ir_features(...)` 后调用，避免默认 pipeline 悄悄不跑 prototype recovery。

暂不做：

- selected targets 全量导出。
- 统计每个函数的候选数量。
- 抽查 20 个函数的语义正确性。
- instcombine 接入。

## 判断标准

- `bash -n scripts/bench2-native-smoke.sh` 通过。
- `scripts/bench2-native-smoke.sh` 三个固定 Bench2 目标通过。
- 不新增源码逻辑，只增强验证脚本。

## 实现记录

### 改动

- `scripts/bench2-native-smoke.sh:163` 到 `scripts/bench2-native-smoke.sh:175`：新增 `check_prototype_metadata(...)`，检查 all-confirmed IR 中的 ABI、external input、input candidate、return candidate metadata。
- `scripts/bench2-native-smoke.sh:567`：在已有 `check_ir_features(...)` 后调用 `check_prototype_metadata(...)`。

### 验证

- `bash -n scripts/bench2-native-smoke.sh`
  - 结果：通过。
- `cmake -S . -B build-native -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON`
  - 结果：通过。
- `cmake --build build-native --target notdec-native-llvm notdec-native-discover notdec-heritage-module-check -j2`
  - 结果：通过。
- `scripts/bench2-native-smoke.sh --build-dir build-native --out-dir /tmp/notdec-bin2llvm-bench2-smoke-prototype-20260529-02`
  - 结果：未通过。失败发生在 native LLVM 之前，`vsftpd.summary.json` 里 `unresolved_indirect_flows.indirect branch=2`，脚本原有上限是 0。本次没有放宽旧基线。
- 手工跑三目标 all-confirmed prototype metadata 检查：
  - `vsftpd`：`notdec-native-llvm --all-confirmed`、LLVM 22 `llvm-as`、`opt -passes=verify`、四类 metadata grep 均通过。
  - `libuv`：同上，通过。
  - `memcached`：同上，通过。

手工验证命令输出目录：

```text
/tmp/notdec-bin2llvm-prototype-metadata-manual-20260529-02
```

metadata grep 计数：

```text
vsftpd.all-confirmed.ll 186
libuv.all-confirmed.ll 482
memcached.all-confirmed.ll 258
```

### 性能影响

本次只新增 smoke 脚本里的 `grep -Fq` 检查，不影响生成 IR 的性能。手工三目标 all-confirmed 验证主要耗时仍在 native lowering 和 LLVM verify。

### 评分

- 实现效果：7/10。脚本已能防止默认 pipeline 丢失 prototype metadata，但完整 smoke 被既有 discovery 基线挡住。
- 复杂度：1/10。只新增四个固定 pattern 检查。
- 维护成本：1/10。复用现有 `require_ir_pattern(...)`。

### 后续

- 单独处理 `vsftpd` 当前 2 个 unresolved indirect branch，或者明确更新 Bench2 discovery 基线。
- 再跑完整 `scripts/bench2-native-smoke.sh`。
