# 20260601-108 ffmpeg executable / ICU audit

## 背景

上一轮 PHP extension 和 ffmpeg 小/中型库没有暴露新的 signature rewrite blocker。本轮继续扩展 manifest 中还没覆盖的目标，但避免直接全量跑明显过大的库。

候选大块任务：

- 小型 executable 覆盖：`ffmpeg:executable`。
- 数据型 shared object 覆盖：`libicu:data-library`。
- 大 ICU 库规模边界：`libicu:common-library` / `libicu:i18n-library`。

本轮是数据集审计，不新增 Ghidra 数据结构复刻代码。

## prototype gate

命令：

```bash
scripts/bench2-native-prototype-audit.sh \
  --build-dir /tmp/notdec-bin2llvm-build \
  --out-dir /tmp/notdec-bin2llvm-bench2-ffmpeg-icu-data-prototype-audit \
  --target ffmpeg:executable \
  --target libicu:data-library
```

结果：

| target | all-confirmed | signature-rewrite | functions | needed | rewritten | skipped | skip reason |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `ffmpeg:executable` | 58s | 60s | 265 | 200 | 200 | 96 | `already matches=65`, `declaration=31` |
| `libicu:data-library` | 1s | 1s | 5 | 0 | 0 | 15 | `already matches=5`, `declaration=10` |

LLVM 22 验证：

- 两个目标的 all-confirmed `.ll` 均通过 `llvm-as` / `opt -passes=verify`。
- 两个目标的 signature rewrite `.ll` 均通过 `llvm-as` / `opt -passes=verify`。
- 所有 `*.llvm-as.stderr` 和 `*.opt.stderr` 都是 0 字节。

判断：

- `ffmpeg:executable` 没有新的非合理 skip reason。
- `libicu:data-library` 基本是数据库形状，只有 5 个 confirmed functions，没有 rewrite 需求。

## ICU 规模判断

命令：

```bash
scripts/bench2-native-scale-audit.sh \
  --build-dir /tmp/notdec-bin2llvm-build \
  --out-dir /tmp/notdec-bin2llvm-bench2-libicu-scale-audit \
  --target libicu:common-library \
  --target libicu:i18n-library \
  --limit 50 \
  --limit 100 \
  --timeout-seconds 180
```

结果：

| target | limit | elapsed | output bytes | stderr bytes |
| --- | ---: | ---: | ---: | ---: |
| `libicu:common-library` | 50 | 11s | 155167 | 0 |
| `libicu:common-library` | 100 | 22s | 309584 | 0 |
| `libicu:i18n-library` | 50 | 11s | 142568 | 0 |
| `libicu:i18n-library` | 100 | 22s | 268444 | 0 |

`notdec-native-discover --summary-json`：

| target | confirmed functions | basic blocks | instructions | unresolved indirect call | unresolved indirect branch |
| --- | ---: | ---: | ---: | ---: | ---: |
| `libicu:common-library` | 4333 | 10309 | 40731 | 27 | 81 |
| `libicu:i18n-library` | 8974 | 16944 | 73459 | 68 | 191 |

判断：

- 两个 ICU 大库和 `redis:server-symlink` 一样，seed-limit 曲线接近 50 个约 11 秒、100 个约 22 秒。
- 直接全量 prototype gate 预计会落在十几到三十分钟级，本轮不强跑。
- 这属于 all-confirmed lift / IR 生成吞吐问题，不是 prototype recovery skip reason。

## 后续

- 继续扩展时应优先选择 confirmed functions 数量较小的目标。
- 对 `redis:server-symlink`、`wolfssl`、`libicu:common-library`、`libicu:i18n-library` 这类目标，先做 all-confirmed 性能任务或分批审计策略。
