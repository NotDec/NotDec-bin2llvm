# 20260601-107 php / ffmpeg expanded audit

## 背景

上一轮确认大目标慢点主要在 all-confirmed lift / IR 生成。这轮继续从 Bench2 manifest 扩展真实样本，优先选体量较小但来源不同的 shared object / extension，观察是否出现新的 signature rewrite skip reason。

候选大块任务：

- PHP extension 样本覆盖：检查小型动态库是否有新的 declaration / empty prototype 之外的问题。
- ffmpeg 中型库覆盖：检查多媒体库是否暴露新的 return binding / callsite rewrite blocker。
- 性能/规模边界：记录中型库 all-confirmed / signature rewrite 耗时。

本轮是数据集审计，不新增 Ghidra 数据结构复刻代码。

## 审计命令

```bash
scripts/bench2-native-prototype-audit.sh \
  --build-dir /tmp/notdec-bin2llvm-build \
  --out-dir /tmp/notdec-bin2llvm-bench2-php-ffmpeg-small-prototype-audit \
  --target php:extension-calendar \
  --target php:extension-ffi \
  --target php:extension-sockets \
  --target ffmpeg:util-library \
  --target ffmpeg:resample-library \
  --target ffmpeg:scale-library
```

## 结果

| target | all-confirmed | signature-rewrite | functions | needed | rewritten | skipped | skip reason |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `php:extension-calendar` | 11s | 11s | 52 | 42 | 42 | 31 | `already matches=10`, `declaration=21` |
| `php:extension-ffi` | 73s | 75s | 316 | 246 | 246 | 124 | `already matches=70`, `declaration=54` |
| `php:extension-sockets` | 41s | 41s | 184 | 134 | 134 | 94 | `already matches=50`, `declaration=44` |
| `ffmpeg:util-library` | 287s | 289s | 1266 | 1119 | 1119 | 177 | `already matches=147`, `declaration=30` |
| `ffmpeg:resample-library` | 34s | 33s | 148 | 132 | 132 | 38 | `already matches=16`, `declaration=22` |
| `ffmpeg:scale-library` | 177s | 177s | 785 | 678 | 678 | 128 | `already matches=107`, `declaration=21` |

LLVM 22 验证：

- 所有目标的 all-confirmed `.ll` 均通过 `llvm-as` / `opt -passes=verify`。
- 所有目标的 signature rewrite `.ll` 均通过 `llvm-as` / `opt -passes=verify`。
- 所有 `*.llvm-as.stderr` 和 `*.opt.stderr` 都是 0 字节。

## 判断

- 6 个新增目标没有新的非合理 skip reason。
- PHP extension 覆盖了小型动态库场景，当前 signature rewrite 闭环稳定。
- ffmpeg util / swscale 暴露的是耗时问题，不是 prototype recovery 语义 blocker。
- 本轮不实现新功能，不补小 CFG 测试。

## 后续

下一步可以继续沿两个方向推进：

- 继续扩展中等样本，优先挑 manifest 中还没覆盖、但不会明显超过 5 分钟的目标。
- 如果要处理大样本，先围绕 all-confirmed lift 吞吐做性能任务，而不是在 prototype recovery 里追小形状。
