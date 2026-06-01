# 20260601-119 manifest prototype coverage audit

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

第 114 到 118 步把多个大目标推进到 seed300 / seed500，并且没有新的非合理 skip reason。本轮不继续盲目加深单个目标，先对 `Bench2/manifest/benchmark-targets.tsv` 的 32 个目标做覆盖汇总，确认当前 prototype gate 的证据边界。

这是数据集审计，不新增 Ghidra 数据结构复刻代码。

## 候选大块任务

1. manifest 覆盖汇总：本轮处理。目标是确认每个 manifest 目标是否至少有 full gate 或 seed-limited gate 证据。
2. 新的 signature rewrite blocker：本轮没有新运行结果暴露 blocker，暂不进入功能复刻流程。
3. 大目标全量 gate：seed-limited 目标仍可继续加深，但先记录当前覆盖面，避免只围绕少数大库反复迭代。

## 覆盖汇总

| target | 当前证据 | 最新记录 | 结论 |
| --- | --- | --- | --- |
| `vsftpd:executable` | full gate | `20260601-95-stage6-convergence-audit.md` | 只剩 `already matches` / `declaration` |
| `libuv:shared-library` | full gate | `20260601-95-stage6-convergence-audit.md` | 只剩 `already matches` / `declaration` |
| `memcached:executable` | full gate | `20260601-95-stage6-convergence-audit.md` | 只剩 `already matches` / `declaration` |
| `lighttpd:executable` | full gate | `20260601-100-callsite-return-unused-cfg.md` | 第 98 步 blocker 已清零 |
| `lighttpd:helper` | full gate | `20260601-101-lighttpd-angel-tmux-expanded-audit.md` | 只剩 `already matches` / `declaration` |
| `tmux:executable` | full gate | `20260601-101-lighttpd-angel-tmux-expanded-audit.md` | 只剩 `already matches` / `declaration` |
| `openssh:client` | full gate | `20260601-102-openssh-expanded-audit.md` | 只剩 `already matches` / `declaration` |
| `openssh:server` | full gate | `20260601-102-openssh-expanded-audit.md` | 只剩 `already matches` / `declaration` |
| `wolfssl:shared-library` | seed500 gate | `20260601-118-key-seed500-prototype-audit.md` | 只剩 `already matches` / `declaration` |
| `redis:server-symlink` | seed500 gate | `20260601-118-key-seed500-prototype-audit.md` | 只剩 `already matches` / `declaration` |
| `redis:cli` | full gate | `20260601-104-return-binding-storage-slice.md` | storage slice blocker 已清零 |
| `redis:benchmark` | full gate | `20260601-105-redis-benchmark-wrk-expanded-audit.md` | 只剩 `already matches` / `declaration` |
| `libicu:data-library` | full gate | `20260601-108-ffmpeg-executable-icu-audit.md` | 只剩 `already matches` / `declaration` |
| `libicu:i18n-library` | seed300 gate | `20260601-115-large-extra-seed300-prototype-audit.md` | 只剩 `already matches` / `declaration` |
| `libicu:common-library` | seed300 gate | `20260601-114-large-seed300-prototype-audit.md` | 只剩 `already matches` / `declaration` |
| `vim:executable` | seed300 gate | `20260601-116-diverse-seed300-prototype-audit.md` | 只剩 `already matches` / `declaration` |
| `python:interpreter` | seed500 gate | `20260601-118-key-seed500-prototype-audit.md` | seed200 blocker 未复现 |
| `python:shared-library` | seed300 gate | `20260601-115-large-extra-seed300-prototype-audit.md` | 只剩 `already matches` / `declaration` |
| `python:debug-interpreter` | seed300 gate | `20260601-117-python-debug-seed300-prototype-audit.md` | 只剩 `already matches` / `declaration` |
| `python:debug-shared-library` | seed300 gate | `20260601-117-python-debug-seed300-prototype-audit.md` | 只剩 `already matches` / `declaration` |
| `wrk:executable` | full gate | `20260601-105-redis-benchmark-wrk-expanded-audit.md` | 只剩 `already matches` / `declaration` |
| `ffmpeg:executable` | full gate | `20260601-108-ffmpeg-executable-icu-audit.md` | 只剩 `already matches` / `declaration` |
| `ffmpeg:codec-library` | seed500 gate | `20260601-118-key-seed500-prototype-audit.md` | seed200 blocker 未复现 |
| `ffmpeg:format-library` | seed300 gate | `20260601-116-diverse-seed300-prototype-audit.md` | 只剩 `already matches` / `declaration` |
| `ffmpeg:util-library` | full gate | `20260601-107-php-ffmpeg-expanded-audit.md` | 只剩 `already matches` / `declaration` |
| `ffmpeg:filter-library` | seed300 gate | `20260601-115-large-extra-seed300-prototype-audit.md` | 只剩 `already matches` / `declaration` |
| `ffmpeg:scale-library` | full gate | `20260601-107-php-ffmpeg-expanded-audit.md` | 只剩 `already matches` / `declaration` |
| `ffmpeg:resample-library` | full gate | `20260601-107-php-ffmpeg-expanded-audit.md` | 只剩 `already matches` / `declaration` |
| `php:executable` | seed300 gate | `20260601-114-large-seed300-prototype-audit.md` | 只剩 `already matches` / `declaration` |
| `php:extension-calendar` | full gate | `20260601-107-php-ffmpeg-expanded-audit.md` | 只剩 `already matches` / `declaration` |
| `php:extension-ffi` | full gate | `20260601-107-php-ffmpeg-expanded-audit.md` | 只剩 `already matches` / `declaration` |
| `php:extension-sockets` | full gate | `20260601-107-php-ffmpeg-expanded-audit.md` | 只剩 `already matches` / `declaration` |

## 当前判断

- 32 个 manifest 目标都已有 prototype gate 证据。
- 小到中型目标多数已有 full gate。
- 大目标主要用 seed300 / seed500 gate 覆盖；这是覆盖率限制，不是当前已知 prototype rewrite blocker。
- 最新覆盖中没有 `unsafe callsite input value`、`unsafe callsite return load`、`unsafe return value load`、`return value type mismatch`、`function has uses`。
- 剩余 skip reason 只有 `already matches` 和 `declaration`。

## 不标长期完成的原因

这个结论只能说明当前 Bench2 manifest prototype rewrite gate 覆盖面已经比较干净，不能说明整个 native prototype recovery Pass 完成：

- seed-limited 大目标还不是全量证明。
- 参数/返回恢复的语义正确性仍只做了样本抽查，不是全函数证明。
- 栈参数、复杂 alias、间接调用、更完整的 Ghidra protorule/type propagation 仍不在当前完成范围内。

## 后续

下一步不应再为单个 CFG 形状单独迭代。更合理的方向是：

1. 把关键 seed500 目标继续加深到 seed700 / seed1000。
2. 对 seed-limited 大目标挑少量 rewritten 函数做语义抽查。
3. 如果出现新的非合理 skip reason，再按 Ghidra 对应数据结构切功能实现阶段。
4. 如果继续追求全量 gate，应先把 lift / IR 生成吞吐作为性能任务处理。
