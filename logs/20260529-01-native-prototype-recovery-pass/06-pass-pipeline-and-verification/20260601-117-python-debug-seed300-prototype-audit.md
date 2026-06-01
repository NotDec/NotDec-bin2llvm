# 20260601-117 python debug seed-300 prototype audit

## 背景

第 116 步覆盖了 vim、ffmpeg format、redis server、wolfssl 等不同项目类型。本轮继续补一个不同二进制形态：Python debug interpreter 和 debug shared library。它们此前只做过规模审计，没有进入 seed-limited prototype gate。

候选大块任务：

- Python debug 目标 seed300 prototype gate：本轮处理。
- 新的 signature rewrite blocker：如果出现非合理 skip reason，再进入 Ghidra 数据结构复刻流程。
- debug build 特有符号/函数形态：本轮只通过 skip reason 观察，不单独实现。

## 命令

```bash
scripts/bench2-native-prototype-audit.sh \
  --build-dir /tmp/notdec-bin2llvm-build \
  --out-dir /tmp/notdec-bin2llvm-bench2-python-debug-seed300-prototype-audit \
  --target python:debug-interpreter \
  --target python:debug-shared-library \
  --decode-seed-limit 300
```

## 结果

| target | limit | all-confirmed | signature-rewrite | needed | rewritten | skipped | skip reason |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `python:debug-interpreter` | 300 | 67s | 67s | 209 | 209 | 183 | `already matches=91`, `declaration=92` |
| `python:debug-shared-library` | 300 | 68s | 69s | 204 | 204 | 194 | `already matches=96`, `declaration=98` |

LLVM 22 验证：

- 两个目标的 all-confirmed `.ll` / `.bc` 都生成成功。
- 两个目标的 signature rewrite `.ll` / `.bc` 都生成成功。
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

- Python debug 目标没有暴露新的 prototype recovery / signature rewrite blocker。
- debug build 的额外符号和函数体没有导致新的非合理 skip reason。
- 本轮是数据集审计，不涉及新的 Ghidra 模块或数据结构复刻，因此不写功能实现 plan，不新增代码。

## 后续

- 可以继续把主要大目标从 seed300 提到 seed400 / seed500。
- 也可以补跑全 manifest 中剩余较小目标，确认是否还有未记录的二进制形态。
- 如果后续再次出现非合理 skip reason，先记录真实函数和 IR 形状，再按 Ghidra 对应数据结构切一个阶段实现。
