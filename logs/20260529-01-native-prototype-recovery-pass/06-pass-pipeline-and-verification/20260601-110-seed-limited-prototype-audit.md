# 20260601-110 seed-limited prototype audit

## 背景

前几轮确认 `vim`、`php`、`python`、`libicu`、大 ffmpeg 库等目标主要受 all-confirmed lift / IR 生成吞吐限制。全量 prototype gate 会很慢，但只做 scale audit 又看不到 signature rewrite skip reason。

本轮做一个测试/验证能力的小步：让 manifest prototype audit 支持 `--decode-seed-limit`，这样可以对大目标分批跑完整 signature rewrite gate。

这不是新的 Ghidra 数据结构复刻，所以不写功能复刻 plan。

## 改动

文件：`scripts/bench2-native-prototype-audit.sh`

- 增加 `--decode-seed-limit COUNT` 参数。
- all-confirmed 和 signature rewrite 两次 `notdec-native-llvm` 调用都传入该参数。
- `metrics.tsv` 增加 `decode_seed_limit` 列。
- 未指定时写 `all`，保持原来的全量行为。

## 验证

帮助信息：

```bash
scripts/bench2-native-prototype-audit.sh --help
```

输出已包含 `--decode-seed-limit COUNT`。

seed-limited prototype gate：

```bash
scripts/bench2-native-prototype-audit.sh \
  --build-dir /tmp/notdec-bin2llvm-build \
  --out-dir /tmp/notdec-bin2llvm-bench2-seed-limited-prototype-audit \
  --target vim:executable \
  --target ffmpeg:format-library \
  --decode-seed-limit 100
```

结果：

| target | limit | all-confirmed | signature-rewrite | prototype functions | needed | rewritten | skipped | skip reason |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `vim:executable` | 100 | 22s | 23s | 100 | 69 | 69 | 56 | `already matches=31`, `declaration=25` |
| `ffmpeg:format-library` | 100 | 23s | 23s | 100 | 90 | 90 | 24 | `already matches=10`, `declaration=14` |

LLVM 22 验证：

- 两个目标的 all-confirmed `.ll` 均通过 `llvm-as` / `opt -passes=verify`。
- 两个目标的 signature rewrite `.ll` 均通过 `llvm-as` / `opt -passes=verify`。
- 所有 `*.llvm-as.stderr` 和 `*.opt.stderr` 都是 0 字节。

## 判断

- seed-limited prototype gate 能在大目标上给出真实 skip reason，且不会等待全量十几分钟。
- 当前两个样本仍没有新的非合理 skip reason。
- 后续可以用这个脚本对 `php` / `python` / `libicu` / 大 ffmpeg 库做分批功能审计。
