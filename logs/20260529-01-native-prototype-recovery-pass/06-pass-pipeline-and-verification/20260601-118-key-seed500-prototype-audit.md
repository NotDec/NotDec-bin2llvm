# 20260601-118 key seed-500 prototype audit

## 背景

第 114 到 117 步已经覆盖了多批 seed300 目标。本轮把曾经暴露过 blocker 或规模边界的关键目标继续加深到 seed500，看更深的真实函数样本是否出现新的 skip reason。

候选大块任务：

- 关键压力目标 seed500 prototype gate：本轮处理。
- 新的 signature rewrite blocker：如果出现非合理 skip reason，再进入 Ghidra 数据结构复刻流程。
- 大目标全量性能边界：本轮只记录 seed-limited 同口径时间，不做优化。

## 命令

```bash
scripts/bench2-native-prototype-audit.sh \
  --build-dir /tmp/notdec-bin2llvm-build \
  --out-dir /tmp/notdec-bin2llvm-bench2-key-seed500-prototype-audit \
  --target python:interpreter \
  --target ffmpeg:codec-library \
  --target redis:server-symlink \
  --target wolfssl:shared-library \
  --decode-seed-limit 500
```

## 结果

| target | limit | all-confirmed | signature-rewrite | needed | rewritten | skipped | skip reason |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `python:interpreter` | 500 | 113s | 111s | 400 | 400 | 138 | `already matches=100`, `declaration=38` |
| `ffmpeg:codec-library` | 500 | 114s | 114s | 445 | 445 | 82 | `already matches=55`, `declaration=27` |
| `redis:server-symlink` | 500 | 111s | 112s | 409 | 409 | 124 | `already matches=91`, `declaration=33` |
| `wolfssl:shared-library` | 500 | 110s | 111s | 375 | 375 | 170 | `already matches=125`, `declaration=45` |

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

- `python:interpreter` 和 `ffmpeg:codec-library` 曾经在 seed200 暴露 return binding 问题；提高到 seed500 后没有复现。
- `redis:server-symlink` 和 `wolfssl:shared-library` 曾经主要是规模边界目标；seed500 prototype gate 仍只剩合理 skip reason。
- 本轮是数据集审计，不涉及新的 Ghidra 模块或数据结构复刻，因此不写功能实现 plan，不新增代码。

## 后续

- 可以继续把关键目标提高到 seed700 / seed1000。
- 也可以补一个 manifest 覆盖审计，总结哪些目标是 full gate，哪些是 seed300/500 gate。
- 如果后续再次出现非合理 skip reason，先记录真实函数和 IR 形状，再按 Ghidra 对应数据结构切一个阶段实现。
