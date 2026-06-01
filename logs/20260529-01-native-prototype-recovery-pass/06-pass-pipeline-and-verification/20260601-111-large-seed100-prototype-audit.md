# 20260601-111 large seed-100 prototype audit

## 背景

`bench2-native-prototype-audit.sh` 已支持 `--decode-seed-limit`。本轮用它对前几轮判定为全量过慢的大目标做 seed-limited signature rewrite gate，目标是继续找真实 prototype recovery blocker。

候选大块任务：

- 大目标分批 prototype gate：本轮处理。
- 新的 signature rewrite blocker：如果出现非合理 skip reason，再进入功能复刻流程。
- all-confirmed 性能优化：本轮不处理。

## 命令

```bash
scripts/bench2-native-prototype-audit.sh \
  --build-dir /tmp/notdec-bin2llvm-build \
  --out-dir /tmp/notdec-bin2llvm-bench2-large-seed100-prototype-audit \
  --target php:executable \
  --target python:interpreter \
  --target python:shared-library \
  --target libicu:common-library \
  --target libicu:i18n-library \
  --target ffmpeg:codec-library \
  --target ffmpeg:filter-library \
  --decode-seed-limit 100
```

## 结果

| target | limit | all-confirmed | signature-rewrite | needed | rewritten | skipped | skip reason |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `php:executable` | 100 | 23s | 23s | 79 | 79 | 40 | `already matches=21`, `declaration=19` |
| `python:interpreter` | 100 | 22s | 22s | 75 | 75 | 44 | `already matches=25`, `declaration=19` |
| `python:shared-library` | 100 | 23s | 24s | 77 | 77 | 49 | `already matches=23`, `declaration=26` |
| `libicu:common-library` | 100 | 23s | 23s | 81 | 81 | 39 | `already matches=19`, `declaration=20` |
| `libicu:i18n-library` | 100 | 22s | 23s | 81 | 81 | 44 | `already matches=19`, `declaration=25` |
| `ffmpeg:codec-library` | 100 | 24s | 24s | 92 | 92 | 20 | `already matches=8`, `declaration=12` |
| `ffmpeg:filter-library` | 100 | 24s | 24s | 78 | 78 | 41 | `already matches=22`, `declaration=19` |

LLVM 22 验证：

- 所有目标的 all-confirmed `.ll` 均通过 `llvm-as` / `opt -passes=verify`。
- 所有目标的 signature rewrite `.ll` 均通过 `llvm-as` / `opt -passes=verify`。
- 所有 `*.llvm-as.stderr` 和 `*.opt.stderr` 都是 0 字节。

## 判断

- 7 个大目标的前 100 seed 都没有新的非合理 skip reason。
- 当前新增样本继续支持这个判断：prototype recovery / signature rewrite 的主要功能 blocker 暂未再出现；大目标问题主要还是全量 lift 吞吐。
- 本轮不写功能复刻 plan，不新增 CFG 小测试。

## 后续

- 可以继续把 seed limit 提到 200 / 400，或者对大目标做分段 seed 审计。
- 若后续出现 `unsafe callsite input value`、`unsafe callsite return load`、`return value type mismatch` 等非合理 skip，再回到对应 Ghidra 数据结构复刻流程。
