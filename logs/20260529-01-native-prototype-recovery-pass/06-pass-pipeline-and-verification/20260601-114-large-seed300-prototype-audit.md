# 20260601-114 large seed-300 prototype audit

## 背景

第 112 步修复 Python seed200 的 return register copy blocker。第 113 步修复 ffmpeg codec seed200 的 declaration call output blocker。按 GOAL 的规则，本轮继续先跑 Bench2 真实目标，看新增函数是否暴露新的 skip reason，再决定是否需要实现。

候选大块任务：

- 大目标分批 prototype gate：本轮处理。
- 新的 signature rewrite blocker：如果出现非合理 skip reason，再进入 Ghidra 数据结构复刻流程。
- 全量大库性能边界：本轮只记录同口径时间，不做优化。

## 命令

```bash
scripts/bench2-native-prototype-audit.sh \
  --build-dir /tmp/notdec-bin2llvm-build \
  --out-dir /tmp/notdec-bin2llvm-bench2-large-seed300-prototype-audit \
  --target php:executable \
  --target python:interpreter \
  --target libicu:common-library \
  --target ffmpeg:codec-library \
  --decode-seed-limit 300
```

## 结果

| target | limit | all-confirmed | signature-rewrite | needed | rewritten | skipped | skip reason |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `php:executable` | 300 | 69s | 71s | 241 | 241 | 89 | `already matches=59`, `declaration=30` |
| `python:interpreter` | 300 | 66s | 67s | 236 | 236 | 92 | `already matches=64`, `declaration=28` |
| `libicu:common-library` | 300 | 66s | 66s | 250 | 250 | 84 | `already matches=50`, `declaration=34` |
| `ffmpeg:codec-library` | 300 | 68s | 70s | 267 | 267 | 53 | `already matches=33`, `declaration=20` |

LLVM 22 验证：

- 四个目标的 all-confirmed `.ll` / `.bc` 都生成成功。
- 四个目标的 signature rewrite `.ll` / `.bc` 都生成成功。
- 脚本 gate 已跑 `llvm-as` 和 `opt -passes=verify`，没有 verify 失败。

## skip reason 判断

本轮没有出现新的非合理 skip reason：

- 没有 `unsafe return value load`。
- 没有 `unsafe callsite input value`。
- 没有 `unsafe callsite return load`。
- 没有 `return value type mismatch`。
- 没有 `function has uses`。

剩余 skip reason 只有：

- `already matches`：函数当前签名已经和 recovered prototype 一致，或者 recovered prototype 为空。
- `declaration`：外部声明或 LLVM intrinsic，不应在当前 module 内改函数体。

## 判断

- seed300 相比 seed200 多覆盖了一批真实函数，四个大目标仍然只剩合理 skip reason。
- 第 112 / 113 步修过的两类 return binding blocker 没有在 seed300 里复现。
- 本轮是数据集审计，不涉及新的 Ghidra 模块或数据结构复刻，因此不写功能实现 plan，不新增代码。

## 后续

- 可以继续把同一组目标提高到 seed400 / seed500，或补跑 `python:shared-library`、`libicu:i18n-library`、`ffmpeg:filter-library` 的 seed300。
- 如果后续再次出现非合理 skip reason，先记录真实函数和 IR 形状，再按 Ghidra 对应数据结构切一个阶段实现。
