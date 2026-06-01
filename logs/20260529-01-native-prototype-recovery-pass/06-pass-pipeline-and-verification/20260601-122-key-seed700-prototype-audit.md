# 20260601-122 key seed-700 prototype audit

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

第 118 步把四个关键压力目标推进到 seed500。第 120 / 121 步做了 rewritten 语义和多返回消费覆盖抽查。本轮继续加深同一批关键目标到 seed700，看更深样本是否出现新的非合理 skip reason，或者出现多返回第 1 分量消费样本。

这是数据集审计，不新增 Ghidra 数据结构复刻代码。

## 候选大块任务

1. 关键压力目标 seed700 prototype gate：本轮处理。
2. 新的 signature rewrite blocker：如果出现非合理 skip reason，再进入 Ghidra 数据结构复刻流程。
3. 多返回第 1 分量真实消费样本：如果出现，再补语义抽查；本轮只做搜索和记录。

## 命令

```bash
scripts/bench2-native-prototype-audit.sh \
  --build-dir /tmp/notdec-bin2llvm-build \
  --out-dir /tmp/notdec-bin2llvm-bench2-key-seed700-prototype-audit \
  --target python:interpreter \
  --target ffmpeg:codec-library \
  --target redis:server-symlink \
  --target wolfssl:shared-library \
  --decode-seed-limit 700
```

## 结果

| target | limit | all-confirmed | signature-rewrite | needed | rewritten | skipped | skip reason |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `python:interpreter` | 700 | 157s | 158s | 558 | 558 | 194 | `already matches=142`, `declaration=52` |
| `ffmpeg:codec-library` | 700 | 163s | 160s | 623 | 623 | 107 | `already matches=77`, `declaration=30` |
| `redis:server-symlink` | 700 | 156s | 157s | 573 | 573 | 170 | `already matches=127`, `declaration=43` |
| `wolfssl:shared-library` | 700 | 158s | 157s | 533 | 533 | 222 | `already matches=167`, `declaration=55` |

LLVM 22 验证：

- 四个目标的 all-confirmed `.ll` / `.bc` 都生成成功。
- 四个目标的 signature rewrite `.ll` / `.bc` 都生成成功。
- 所有 `llvm-as.stderr` 和 `opt.stderr` 都是 0 字节。

## skip reason 判断

本轮没有出现新的非合理 skip reason：

- 没有 `unsafe return value load`。
- 没有 `unsafe callsite input value`。
- 没有 `unsafe callsite return load`。
- 没有 `return value type mismatch`。
- 没有 `function has uses`。

剩余 skip reason 只有：

- `already matches`。
- `declaration`。

## 多返回消费检查

在本轮 seed700 四目标输出中搜索：

| pattern | count |
| --- | ---: |
| `call { i64, i64 }` | 9 |
| `extractvalue { i64, i64 } ..., 0` | 1 |
| `extractvalue { i64, i64 } ..., 1` | 0 |

结论：

- seed700 仍只有第 0 分量消费样本。
- 第 1 分量真实消费样本仍未出现。
- 这不是当前 blocker，但仍是后续真实样本覆盖缺口。

## 判断

- `python:interpreter` 和 `ffmpeg:codec-library` 曾经暴露 return binding blocker；seed700 没有复现。
- `redis:server-symlink` 和 `wolfssl:shared-library` 作为规模压力目标，seed700 仍只剩合理 skip reason。
- 和 seed500 相比，本轮加深了 200 个 seed，rewrite needed / rewritten 数同步增长，没有出现 rewrite 安全性回退。
- 本轮不实现新功能，不补小 CFG 测试。

## 后续

- 可以把关键目标继续加深到 seed1000。
- 也可以优先对 seed700 新增 rewritten 函数做语义抽查。
- 多返回第 1 分量消费仍需要继续在真实输出里找，不建议只为它补单个 CFG 变体。
