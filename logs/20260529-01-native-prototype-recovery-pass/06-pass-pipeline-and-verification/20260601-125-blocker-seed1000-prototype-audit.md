# 20260601-125 blocker seed-1000 prototype audit

## 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass。首先将相关的数据结构划分为几个工作量大约相等的部分（目前应该差不多完成了，主要是在数据集上测试并进一步提升），其次，规划一下如何做测试和验证。把进度记录到PROGRESS.md里面。依然要求，

根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

在数据集上测试并进一步提升时，因为不涉及实现具体的模块或数据结构，可以不需要遵守上面的功能复刻流程。

1. 每一轮先看 Bench2 当前 skip reason 和真实函数样本，再决定实现点。
2. 先按 Ghidra 数据结构和 Bench2 blocker 识别“大块能力”，再从大块能力里切小步。小步服务于大块任务，不允许小步自己变成主线。
3. 每次实现不应以“一个极小 CFG 变体”作为默认粒度。应把同一类语义问题合并成一个小阶段处理，同类问题多修复一些再合并commit。
```

## 背景

Python interpreter 和 ffmpeg codec 曾经分别暴露过 return binding blocker：

- Python seed200 的 `PyStatus_Exit`: `unsafe return value load`，第 112 步修复 register copy binding。
- ffmpeg codec seed200 的 `av_packet_alloc`: `unsafe return value load`，第 113 步修复 declaration call output binding。

第 118 / 122 步已经把它们推进到 seed500 / seed700。本轮继续把这两个 blocker 来源目标加深到 seed1000，优先确认旧 blocker 是否复现。

这是数据集审计，不新增 Ghidra 数据结构复刻代码。

## 候选大块任务

1. 历史 blocker 来源目标 seed1000 prototype gate：本轮处理。
2. 新的 return binding blocker：如果出现非合理 skip reason，再进入 Ghidra 数据结构复刻流程。
3. 多返回第 1 分量真实消费样本：本轮继续搜索和记录。

## 命令

```bash
scripts/bench2-native-prototype-audit.sh \
  --build-dir /tmp/notdec-bin2llvm-build \
  --out-dir /tmp/notdec-bin2llvm-bench2-blocker-seed1000-prototype-audit \
  --target python:interpreter \
  --target ffmpeg:codec-library \
  --decode-seed-limit 1000
```

## 结果

| target | limit | all-confirmed | signature-rewrite | needed | rewritten | skipped | skip reason |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `python:interpreter` | 1000 | 229s | 222s | 800 | 800 | 256 | `already matches=200`, `declaration=56` |
| `ffmpeg:codec-library` | 1000 | 234s | 230s | 905 | 905 | 127 | `already matches=95`, `declaration=32` |

LLVM 22 验证：

- 两个目标的 all-confirmed `.ll` / `.bc` 都生成成功。
- 两个目标的 signature rewrite `.ll` / `.bc` 都生成成功。
- 所有 `llvm-as.stderr` 和 `opt.stderr` 都是 0 字节。

## skip reason 判断

本轮没有出现新的非合理 skip reason：

- 没有 `unsafe return value load`。
- 没有 `unsafe callsite input value`。
- 没有 `unsafe callsite return load`。
- 没有 `return value type mismatch`。
- 没有 `function has uses`。

旧 blocker 没有复现：

- Python 的 return register copy binding 在 seed1000 仍稳定。
- ffmpeg 的 declaration call output binding 在 seed1000 仍稳定。

## 多返回消费检查

在本轮 seed1000 两目标输出中搜索：

| pattern | count |
| --- | ---: |
| `call { i64, i64 }` | 11 |
| `extractvalue { i64, i64 } ..., 0` | 1 |
| `extractvalue { i64, i64 } ..., 1` | 0 |

结论：

- seed1000 仍只有第 0 分量消费样本。
- 第 1 分量真实消费样本仍未出现。

## 判断

- 两个历史 blocker 来源目标加深到 seed1000 后，signature rewrite 仍是 `needed == rewritten`。
- skip reason 仍只剩 `already matches` / `declaration`。
- 本轮没有新的实现点，不补小 CFG 测试。

## 后续

- 可以把 Redis server / wolfSSL 也从 seed700 加深到 seed1000，补齐关键压力目标的同口径深度。
- 也可以对 seed1000 新增 rewritten 函数做语义抽查。
