# 20260601-121 multi-return second component coverage audit

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

第 120 步已经抽到 Python 的真实多返回 callsite 消费样本：`PyErr_GetRaisedException` 返回 `{ i64, i64 }`，调用方消费第 0 分量。本轮继续检查已有 Bench2 输出里是否也存在第 1 分量消费样本，避免把没有真实证据的路径说成已经覆盖。

这是数据集审计，不新增 Ghidra 数据结构复刻代码。

## 候选大块任务

1. 多返回 callsite 消费覆盖审计：本轮处理。重点是第 1 分量，也就是 `RDX` 对应的返回值是否有真实消费样本。
2. 新的 multi-return rewrite 功能实现：只有发现真实 blocker 或错误改写时再进入功能复刻流程。
3. 继续 seed 加深：可以后续做，但本轮先统计现有输出中的覆盖缺口。

## 搜索范围

搜索当前保留的 Bench2 prototype audit 输出：

```bash
/tmp/notdec-bin2llvm-bench2-*prototype-audit*/*.signature-rewrite.ll
```

统计结果：

| pattern | count | 判断 |
| --- | ---: | --- |
| `call { i64, i64 }` | 21 | 已有真实多返回 callsite |
| `extractvalue { i64, i64 } ..., 0` | 3 | 已有第 0 分量消费样本 |
| `extractvalue { i64, i64 } ..., 1` | 0 | 暂无第 1 分量消费样本 |

注意：输出里有大量 `extractvalue { i64, i1 } ..., 1` / `extractvalue { i32, i1 } ..., 1`，这些来自 LLVM overflow intrinsic，不是 prototype recovery 的 `{ RAX, RDX }` 多返回。

## 已覆盖真实样本

| target | callsite | 消费情况 |
| --- | --- | --- |
| `python:interpreter` seed500 | `call { i64, i64 } @PyErr_GetRaisedException()` | `extractvalue ..., 0` |
| `redis:benchmark` full gate | `call { i64, i64 } @notdec_native_814c0(i64 ...)` | `extractvalue ..., 0` |
| `php:extension-ffi` full gate | `call { i64, i64 } @notdec_native_1c570()` | `extractvalue ..., 0` |

其他多返回 callsite 目前没有消费返回分量，例子包括：

- `ffmpeg:codec-library` 的 `av_packet_alloc()`。
- `vim:executable` 的 `notdec_native_73e00(...)`。
- `lighttpd:executable` 的 `notdec_native_201ff(...)`。
- `tmux:executable` 的 `notdec_native_a3840(...)` / `notdec_native_ad370(...)`。
- `ffmpeg:util-library` 的 `av_get_channel_layout_nb_channels(...)` / `av_frame_alloc()`。

## 判断

- 当前真实输出证明了多返回 callsite 的函数签名改写和第 0 分量消费路径。
- 当前真实输出还不能证明第 1 分量消费路径，因为没有 `extractvalue { i64, i64 } ..., 1` 样本。
- 这不是当前 blocker：已有 gate 没有非合理 skip reason，也没有 LLVM verify 失败。
- 后续不应为了这个缺口单独补一个新的 CFG 排列测试；更合理的做法是继续扩大真实样本或加深 seed，等真实样本出现后再验证。

## 后续

- seed 加深时继续 grep `extractvalue { i64, i64 } ..., 1`。
- 如果真实样本出现第 1 分量消费错误，再归入 multi-return callsite 使用质量这个大块任务处理。
- 如果长期找不到真实样本，可以只保留已有单元测试覆盖，文档里明确真实数据集缺口。
