# 20260601-120 seed-limited rewritten semantics audit

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

第 119 步确认 32 个 manifest 目标都有 full gate 或 seed-limited gate 证据。本轮不继续扩大覆盖面，而是从 seed500 的大目标里抽查 rewritten 函数和 callsite 语义，补强“只看 skip reason 不够”的部分。

这是数据集审计，不新增 Ghidra 数据结构复刻代码。

## 候选大块任务

1. seed-limited rewritten 语义抽查：本轮处理。看函数签名、recovered metadata、callsite 参数和返回值是否一致。
2. 新的 signature rewrite blocker：本轮没有新的非合理 skip reason，暂不进入功能复刻流程。
3. 继续加深 seed：可以后续做，但这轮优先检查已改写结果质量。

## 抽查对象

输出目录：

- `/tmp/notdec-bin2llvm-bench2-key-seed500-prototype-audit/python-interpreter.signature-rewrite.ll`
- `/tmp/notdec-bin2llvm-bench2-key-seed500-prototype-audit/ffmpeg-codec-library.signature-rewrite.ll`

两个目标的 gate 状态：

| target | needed | rewritten | skipped | skip reason | LLVM 22 verify |
| --- | ---: | ---: | ---: | --- | --- |
| `python:interpreter` | 400 | 400 | 138 | `already matches=100`, `declaration=38` | pass |
| `ffmpeg:codec-library` | 445 | 445 | 82 | `already matches=55`, `declaration=27` | pass |

`llvm-as` / `opt -passes=verify` stderr 都是 0 字节。

## 函数签名抽查

| target | function | summary | rewritten signature | 判断 |
| --- | --- | --- | --- | --- |
| Python | `PyNumber_MatrixMultiply` | 2 input, 1 return | `i64 @PyNumber_MatrixMultiply(i64, i64)` | 参数和返回数量一致 |
| Python | `PyFrame_GetCode` | 1 input, 1 return | `i64 @PyFrame_GetCode(i64)` | 参数和返回数量一致 |
| Python | `PyErr_Restore` | 3 input, 2 return | `{ i64, i64 } @PyErr_Restore(i64, i64, i64)` | 多返回 struct 化一致 |
| Python | `PyInterpreterState_New` | 0 input, 2 return | `{ i64, i64 } @PyInterpreterState_New()` | return-only 多返回一致 |
| Python | `PyStatus_Ok` | 1 input, 1 return | `i64 @PyStatus_Ok(i64)` | 参数和返回数量一致 |
| ffmpeg | `av_packet_clone` | 1 input, 1 return | `i64 @av_packet_clone(i64)` | 参数和返回数量一致 |
| ffmpeg | `av_bsf_iterate` | 1 input, 2 return | `{ i64, i64 } @av_bsf_iterate(i64)` | 多返回 struct 化一致 |
| ffmpeg | `avcodec_parameters_alloc` | 0 input, 1 return | `i64 @avcodec_parameters_alloc()` | return-only 一致 |
| ffmpeg | `avpriv_h264_has_num_reorder_frames` | 1 input, 2 return | `{ i64, i64 } @avpriv_h264_has_num_reorder_frames(i64)` | 多返回 struct 化一致 |
| ffmpeg | `avcodec_pix_fmt_to_codec_tag` | 0 input, 2 return | `{ i64, i64 } @avcodec_pix_fmt_to_codec_tag()` | return-only 多返回一致 |

metadata spot check：

- Python `!notdec.prototype.recovered` 里 `input_count=2` / `return_count=1` 对应 `RDI`、`R9`、`RAX` 时，函数签名是 `i64 @PyUnicode_EncodeFSDefault(i64, i64)`。
- Python `input_count=0` / `return_count=2` 对应 `RAX`、`RDX` 时，函数签名是 `{ i64, i64 } @PyErr_GetRaisedException()`。
- ffmpeg `input_count=1` / `return_count=1` 对应 `RDI`、`RAX` 时，函数签名是 `i64 @av_packet_free_side_data(i64)`。
- ffmpeg `input_count=0` / `return_count=2` 对应 `RAX`、`RDX` 时，函数签名是 `{ i64, i64 } @av_packet_alloc()`。

## callsite 抽查

| target | caller shape | rewritten callsite | 判断 |
| --- | --- | --- | --- |
| Python | return-only direct call | `%6 = call i64 @PyDict_New()` | 返回值直接从 call result 取，不再从 `RAX` load |
| Python | two-input direct call | `%16 = call i64 @PyUnicode_EncodeFSDefault(i64 %RDI.callsite_input, i64 %R9.callsite_input)` | callsite 参数来自当前寄存器值，顺序跟 metadata 一致 |
| Python | multi-return direct call | `%4 = call { i64, i64 } @PyErr_GetRaisedException()` 后 `extractvalue ..., 0` | struct result 第 0 分量用于原 `RAX` 语义 |
| Python | input+return direct call | `%5 = call i64 @_PyDict_HasOnlyStringKeys(i64 %RDI.external_input1)` | input 直接传入，返回值直接使用 |
| ffmpeg | input+return direct call | `%14 = call i64 @av_packet_free_side_data(i64 %RDI.external_input1)` | input 和返回签名一致 |
| ffmpeg | multi-return direct call | `%14 = call { i64, i64 } @av_packet_alloc()` | 调用方没有消费返回分量，保持未用结果 |
| ffmpeg | return-only direct call | `%13 = call i64 @avcodec_descriptor_get()` | 返回值直接驱动后续 flag 计算 |

## 判断

- 抽查的函数签名都和 summary 的 input / return candidate 数量一致。
- ABI slot 顺序符合当前 recovered metadata：input 仍按 ABI input slot，return 仍按 `RAX` / `RDX` output slot。
- Python 的 `PyErr_GetRaisedException` 提供了真实多返回 callsite 消费样本，当前能把 `{ i64, i64 }` 第 0 分量接到原 `RAX` 使用处。
- ffmpeg 的 `av_packet_alloc` 仍是多返回调用但返回分量未消费，这属于合理未用结果，不是 rewrite blocker。
- 本轮没有发现新的 `unsafe callsite input value`、`unsafe callsite return load` 或返回类型不一致问题。

## 后续

下一步可以继续做两类审计：

1. 对 seed-limited 大目标继续抽查更多真实 callsite，尤其是多返回第 1 分量被消费的样本。
2. 把关键目标从 seed500 加深到 seed700 / seed1000，看是否出现新的非合理 skip reason。
