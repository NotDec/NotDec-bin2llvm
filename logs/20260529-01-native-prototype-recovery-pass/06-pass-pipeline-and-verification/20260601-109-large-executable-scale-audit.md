# 20260601-109 large executable scale audit

## 背景

当前多数中小目标的 signature rewrite gate 已通过，只剩 manifest 里的大 executable / 大 shared object。按前几轮结果，大目标的主要问题是 all-confirmed lift / IR 生成吞吐，不是 prototype recovery skip reason。

本轮先做规模审计，不直接全量跑明显过大的目标。

候选大块任务：

- 大 executable / runtime library 规模边界：本轮处理。
- prototype recovery 功能 blocker：本轮没有新的 skip reason 证据，不处理。
- all-confirmed lift 性能优化：后续单独作为性能任务。

## discovery 规模

`notdec-native-discover --summary-json`：

| target | confirmed functions | basic blocks | instructions | unresolved indirect call | unresolved indirect branch |
| --- | ---: | ---: | ---: | ---: | ---: |
| `vim:executable` | 5309 | 17862 | 72451 | 19 | 34 |
| `php:executable` | 10629 | 35036 | 124435 | 90 | 1201 |
| `python:interpreter` | 11422 | 58158 | 205342 | 337 | 175 |
| `python:shared-library` | 7343 | 15527 | 72520 | 28 | 112 |
| `ffmpeg:format-library` | 2906 | 4598 | 26701 | 0 | 8 |
| `ffmpeg:codec-library` | 10779 | 13873 | 92252 | 3 | 22 |
| `ffmpeg:filter-library` | 12392 | 21979 | 109506 | 65 | 118 |
| `python:debug-interpreter` | 12525 | 25349 | 125479 | 55 | 29 |
| `python:debug-shared-library` | 12853 | 26380 | 129071 | 55 | 19 |

## scale audit

命令：

```bash
scripts/bench2-native-scale-audit.sh \
  --build-dir /tmp/notdec-bin2llvm-build \
  --out-dir /tmp/notdec-bin2llvm-bench2-executable-scale-audit \
  --target vim:executable \
  --target php:executable \
  --target python:interpreter \
  --target python:shared-library \
  --target ffmpeg:format-library \
  --limit 50 \
  --limit 100 \
  --timeout-seconds 180
```

结果：

| target | limit | elapsed | output bytes | stderr bytes |
| --- | ---: | ---: | ---: | ---: |
| `vim:executable` | 50 | 11s | 172503 | 0 |
| `vim:executable` | 100 | 22s | 339928 | 0 |
| `php:executable` | 50 | 12s | 166971 | 0 |
| `php:executable` | 100 | 23s | 333757 | 0 |
| `python:interpreter` | 50 | 11s | 147726 | 0 |
| `python:interpreter` | 100 | 22s | 311133 | 0 |
| `python:shared-library` | 50 | 11s | 159387 | 0 |
| `python:shared-library` | 100 | 23s | 309737 | 0 |
| `ffmpeg:format-library` | 50 | 11s | 223649 | 0 |
| `ffmpeg:format-library` | 100 | 23s | 441081 | 0 |

## 判断

- 这些目标的 seed-limit 曲线仍然接近 50 个 seed 约 11 秒、100 个 seed 约 22 秒。
- 直接全量 prototype gate 预计会落在十几分钟到几十分钟级。
- 这类目标当前应归为 all-confirmed lift / IR 生成吞吐边界，不应继续在 prototype recovery 里追零散 CFG 形状。

## 后续

可选路线：

- 给大目标做分批 prototype gate：按 seed limit 分段生成 / rewrite / verify，先观察功能 skip reason。
- 优化 all-confirmed lift 的吞吐，再恢复大目标全量 gate。
- 继续选 manifest 中已证明可控的中小目标扩展覆盖；但目前主线功能 blocker 已没有新证据。
