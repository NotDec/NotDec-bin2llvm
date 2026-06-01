# 20260601-115 large extra seed-300 prototype audit

## 背景

第 114 步已经把 `php:executable`、`python:interpreter`、`libicu:common-library`、`ffmpeg:codec-library` 提到 seed300，并确认只剩合理 skip reason。本轮补跑第 111 步 seed100 覆盖过、但第 114 步还没升到 seed300 的三个大目标，扩大样本面。

候选大块任务：

- 大目标分批 prototype gate：本轮处理。
- 新的 signature rewrite blocker：如果出现非合理 skip reason，再进入 Ghidra 数据结构复刻流程。
- 全量大库性能边界：本轮只记录同口径时间，不做优化。

## 命令

```bash
scripts/bench2-native-prototype-audit.sh \
  --build-dir /tmp/notdec-bin2llvm-build \
  --out-dir /tmp/notdec-bin2llvm-bench2-large-extra-seed300-prototype-audit \
  --target python:shared-library \
  --target libicu:i18n-library \
  --target ffmpeg:filter-library \
  --decode-seed-limit 300
```

## 结果

| target | limit | all-confirmed | signature-rewrite | needed | rewritten | skipped | skip reason |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `python:shared-library` | 300 | 68s | 68s | 231 | 231 | 108 | `already matches=69`, `declaration=39` |
| `libicu:i18n-library` | 300 | 66s | 66s | 250 | 250 | 95 | `already matches=50`, `declaration=45` |
| `ffmpeg:filter-library` | 300 | 71s | 71s | 254 | 254 | 66 | `already matches=46`, `declaration=20` |

LLVM 22 验证：

- 三个目标的 all-confirmed `.ll` / `.bc` 都生成成功。
- 三个目标的 signature rewrite `.ll` / `.bc` 都生成成功。
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

- 第 114 步的四个 seed300 目标和本轮三个 seed300 目标合起来，覆盖了第 111 步 seed100 的 7 个大目标。
- 7 个大目标到 seed300 都只剩合理 skip reason，说明当前 return binding / callsite rewrite 的已知 blocker 在这批样本里暂时收敛。
- 本轮是数据集审计，不涉及新的 Ghidra 模块或数据结构复刻，因此不写功能实现 plan，不新增代码。

## 后续

- 可以把 7 个目标继续提高到 seed400 / seed500。
- 也可以优先补不同项目类型的 seed300，避免只在 PHP/Python/ICU/ffmpeg 上加深。
- 如果后续再次出现非合理 skip reason，先记录真实函数和 IR 形状，再按 Ghidra 对应数据结构切一个阶段实现。
