# 20260601-116 diverse seed-300 prototype audit

## 背景

第 114 / 115 步把 PHP、Python、ICU、ffmpeg codec/filter 等大目标推进到 seed300。本轮不继续只加深同一类库，而是选择不同项目类型的真实目标，继续按 GOAL 要求先看 Bench2 skip reason，再决定是否需要实现。

候选大块任务：

- 不同项目类型的 seed300 prototype gate：本轮处理。
- 新的 signature rewrite blocker：如果出现非合理 skip reason，再进入 Ghidra 数据结构复刻流程。
- 大目标全量性能边界：本轮只记录 seed300 同口径时间，不做优化。

## 命令

```bash
scripts/bench2-native-prototype-audit.sh \
  --build-dir /tmp/notdec-bin2llvm-build \
  --out-dir /tmp/notdec-bin2llvm-bench2-diverse-seed300-prototype-audit \
  --target vim:executable \
  --target ffmpeg:format-library \
  --target redis:server-symlink \
  --target wolfssl:shared-library \
  --decode-seed-limit 300
```

## 结果

| target | limit | all-confirmed | signature-rewrite | needed | rewritten | skipped | skip reason |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `vim:executable` | 300 | 67s | 67s | 222 | 222 | 112 | `already matches=78`, `declaration=34` |
| `ffmpeg:format-library` | 300 | 68s | 68s | 278 | 278 | 49 | `already matches=22`, `declaration=27` |
| `redis:server-symlink` | 300 | 67s | 67s | 245 | 245 | 81 | `already matches=55`, `declaration=26` |
| `wolfssl:shared-library` | 300 | 67s | 66s | 220 | 220 | 112 | `already matches=80`, `declaration=32` |

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

- `redis:server-symlink` 和 `wolfssl:shared-library` 之前主要是规模边界目标；本轮 seed300 prototype gate 正常通过，说明它们当前没有暴露新的 prototype rewrite blocker。
- `vim:executable`、`ffmpeg:format-library` 从 seed100 提到 seed300 后仍只剩合理 skip reason。
- 本轮是数据集审计，不涉及新的 Ghidra 模块或数据结构复刻，因此不写功能实现 plan，不新增代码。

## 后续

- 可以继续把已覆盖目标提高到 seed400 / seed500。
- 也可以跑 PHP extensions、Python debug、ffmpeg util/scale/resample 这类较小目标的 seed300，补不同二进制形态。
- 如果后续再次出现非合理 skip reason，先记录真实函数和 IR 形状，再按 Ghidra 对应数据结构切一个阶段实现。
