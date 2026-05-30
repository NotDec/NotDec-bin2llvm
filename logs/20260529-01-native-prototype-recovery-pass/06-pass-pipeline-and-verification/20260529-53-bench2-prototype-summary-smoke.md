# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

Bench2 smoke 已经检查 native lowering 产物能被 LLVM 22 assemble/verify，也会 grep IR 里的 ABI、register SSA、prototype candidate metadata。

但它没有打开 `--prototype-recovery-summary`，所以缺少一层直接证据：真实目标上 prototype recovery pass 确实看到了函数、external input、input candidate、return candidate，而不是只靠 IR 文本存在某些 metadata。

# Ghidra 实现参考

Ghidra prototype 恢复会在 action pipeline 中持续统计和使用当前函数的 prototype 状态：

- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：驱动函数 prototype 类型恢复。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`：从 input trial 生成参数映射。
  - `FuncCallSpecs::deriveOutputMap(...)`：从 output trial 生成返回映射。
  - `FuncProto::updateAllTypes(...)`：把恢复结果写回最终 prototype。

native 侧当前没有完整 Ghidra action trace。`NativePrototypeRecoverySummary` 是当前阶段最直接的 pipeline 可观测结果，应纳入 Bench2 smoke。

# native 侧复刻策略

- 不改 pass 逻辑，只改 `scripts/bench2-native-smoke.sh`。
- `notdec-native-llvm --all-confirmed` 增加 `--prototype-recovery-summary`。
- 从 `native_stderr` 解析 summary 总数：
  - `functions`
  - `external inputs`
  - `input candidates`
  - `return candidates`
- 对 Bench2 三个 selected native 目标，只要求这些总数存在且大于 0。
- 不固定每个目标的精确数量，避免把当前启发式输出变成僵硬基线。

暂时不做：

- 不开启签名重写。
- 不解析每个函数的 summary 行。
- 不把 Bench2 summary 数量写入长期 golden 文件。

# 判断标准

- Bench2 smoke 脚本能在当前 selected native 目标上通过。
- 脚本会在 prototype recovery 没运行、没看到候选，或 summary 格式丢失时失败。
- 全量 CTest 继续通过。

# 风险

Bench2 full smoke 比普通 CTest 慢。这里只增加 stderr 解析和已有 pass 的 summary 打印，不增加额外 lowering 次数。summary 数字大于 0 的门槛比精确基线更稳。

# 实现记录

## 改动

- `scripts/bench2-native-smoke.sh:409` 到 `:429` 增加 prototype recovery summary 解析 helper。
  - `parse_prototype_metric(...)` 从 `native_stderr` 读取 summary 总数。
  - `require_positive_prototype_metric(...)` 要求指标存在且大于 0。
- `scripts/bench2-native-smoke.sh:539` 到 `:563` 扩展 selected native full lowering 检查。
  - `notdec-native-llvm --all-confirmed` 增加 `--prototype-recovery-summary`。
  - 对 `functions`、`external inputs`、`input candidates`、`return candidates` 四个总数做非零检查。
  - 继续保留 LLVM 22 assemble/verify 和 IR metadata grep 检查。

## 验证

已通过：

```sh
bash -n scripts/bench2-native-smoke.sh
git diff --check
OUT_DIR=/tmp/notdec-bin2llvm-bench2-prototype-summary-smoke \
  scripts/bench2-native-smoke.sh --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-bench2-prototype-summary-smoke
ctest --test-dir build --output-on-failure
```

Bench2 selected native 结果：

| target | elapsed | functions | external inputs | input candidates | return candidates |
| --- | ---: | ---: | ---: | ---: | ---: |
| vsftpd | 43s | 187 | 924 | 163 | 59 |
| libuv | 112s | 485 | 1739 | 321 | 165 |
| memcached | 60s | 259 | 1196 | 224 | 99 |

全量 CTest 通过，`9/9`，约 `1.30 sec`。

## 性能影响

不改生产代码。Bench2 smoke 没有增加额外 lowering，只打开 summary 打印和 stderr 解析；本次三目标总耗时约 `215s`。

## 评分

- 实现效果：6/10。Bench2 smoke 现在能直接证明 prototype recovery summary 在真实目标上有非零结果。
- 复杂度：2/10。只增加 shell 解析和检查。
- 后期维护成本：2/10。不固定具体数量，只检查 pipeline 真实跑出结果。
