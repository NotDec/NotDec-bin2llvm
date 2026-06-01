# 20260601-124 seed700 skip quality audit

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

第 122 步确认 key seed700 gate 只剩 `already matches` / `declaration`。第 123 步抽查了新增 rewritten 函数。本轮继续看 skipped 函数质量，确认 `already matches` 里没有非空 recovered prototype 被漏改。

这是数据集审计，不新增 Ghidra 数据结构复刻代码。

## 候选大块任务

1. seed700 skip quality audit：本轮处理。
2. 新的 signature rewrite blocker：如果 `already matches` 中发现非空 recovered prototype 或 declaration 不是声明，再进入功能修复。
3. 空 prototype 质量提升：如果空 prototype 里出现明显参数/返回漏恢复，再另开数据结构复刻任务。

## 统计

| target | already matches | empty prototype | non-empty prototype | declaration |
| --- | ---: | ---: | ---: | ---: |
| `python:interpreter` | 142 | 142 | 0 | 52 |
| `ffmpeg:codec-library` | 77 | 77 | 0 | 30 |
| `redis:server-symlink` | 127 | 127 | 0 | 43 |
| `wolfssl:shared-library` | 167 | 167 | 0 | 55 |

判定方式：

- 从 `*.signature-rewrite.native-llvm.stderr` 读取 skip reason。
- 对 `already matches` 函数读取 summary 里的 `input_candidates` / `return_candidates`。
- `input_candidates=0` 且 `return_candidates=0` 视为 empty prototype。

## already matches 抽查

| target | sample | summary | IR 形态 |
| --- | --- | --- | --- |
| Python | `PyMapping_HasKey` | `external_inputs=2 input_candidates=0 return_candidates=0` | `define void @PyMapping_HasKey()` |
| Python | `PyBuffer_SizeFromFormat` | `external_inputs=6 input_candidates=0 return_candidates=0` | `define void @PyBuffer_SizeFromFormat()` |
| Redis | `updateDictResizePolicy` | `external_inputs=0 input_candidates=0 return_candidates=0` | `define void @updateDictResizePolicy()` |
| Redis | `commandCommand` | `external_inputs=5 input_candidates=0 return_candidates=0` | `define void @commandCommand()` |
| wolfSSL | `wc_HmacInit_Label` | `external_inputs=7 input_candidates=0 return_candidates=0` | `define void @wc_HmacInit_Label()` |
| wolfSSL | `wc_GenerateSeed` | `external_inputs=6 input_candidates=0 return_candidates=0` | `define void @wc_GenerateSeed()` |

对应 recovered metadata 是空 prototype，例如：

- Python `!notdec.prototype.recovered`: `input_count=0`, `return_count=0`。
- Redis `!notdec.prototype.recovered`: `input_count=0`, `return_count=0`。
- wolfSSL `!notdec.prototype.recovered`: `input_count=0`, `return_count=0`。

## declaration 抽查

| sample | IR 形态 | 判断 |
| --- | --- | --- |
| `llvm.ctpop.i64` | `declare i64 @llvm.ctpop.i64(i64)` | LLVM intrinsic |
| `__gmon_start__` | `declare void @__gmon_start__()` | 外部声明 |
| `av_free` | `declare void @av_free()` | 外部声明 |
| `strlen` | `declare void @strlen()` | 外部声明 |
| `notdec_native_43640` | `declare void @notdec_native_43640()` | 未定义目标 |
| `notdec_native_53bb20` | `declare void @notdec_native_53bb20()` | 未定义目标 |

## 判断

- seed700 的 `already matches` 没有发现非空 recovered prototype 漏 rewrite。
- `already matches` 当前实际含义仍是空 prototype：没有 input candidate，也没有 return candidate。
- `declaration` 样本都是声明，不应在当前 module 内重写函数体。
- 本轮没有新的非合理 skip reason，不需要进入 Ghidra 功能复刻流程。

## 后续

- 如果继续加深到 seed1000，仍应复查 `already matches` 是否全为空 prototype。
- 空 prototype 的质量问题要单独看真实函数语义；不能把它和 signature rewrite 漏改混在一起。
