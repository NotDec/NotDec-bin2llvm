# 20260601-105 redis benchmark / wrk expanded audit

## 背景

上一轮 `redis:cli` 暴露的 `return value type mismatch=3` 已通过 return storage slice 修复。本轮继续按 `GOAL.md` 的规则，从 Bench2 manifest 里扩展真实样本，先看 skip reason 和性能边界，再决定是否实现。

本轮候选大块任务：

- Redis 同族样本覆盖：确认 `redis:cli` 的 storage slice 修复是否也覆盖 `redis:benchmark`。
- 新项目小样本覆盖：用 `wrk` 检查是否出现新的 callsite / return binding skip reason。
- 大样本规模边界：用 `redis:server-symlink` 试探 all-confirmed / signature rewrite 耗时。

本轮先做数据集审计，不新增 Ghidra 数据结构复刻代码。

## 审计结果

命令：

```bash
scripts/bench2-native-prototype-audit.sh \
  --build-dir /tmp/notdec-bin2llvm-build \
  --out-dir /tmp/notdec-bin2llvm-bench2-redis-benchmark-wrk-prototype-audit \
  --target redis:benchmark \
  --target wrk:executable
```

结果：

| target | all-confirmed | signature-rewrite | functions | needed | rewritten | skipped | skip reason |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `redis:benchmark` | 103s | 103s | 446 | 376 | 376 | 119 | `already matches=70`, `declaration=49` |
| `wrk:executable` | 32s | 31s | 142 | 124 | 124 | 49 | `already matches=18`, `declaration=31` |

LLVM 22 验证：

- `redis-benchmark.llvm-as.stderr` / `redis-benchmark.opt.stderr`：0 字节。
- `redis-benchmark.signature-rewrite.llvm-as.stderr` / `redis-benchmark.signature-rewrite.opt.stderr`：0 字节。
- `wrk-executable.llvm-as.stderr` / `wrk-executable.opt.stderr`：0 字节。
- `wrk-executable.signature-rewrite.llvm-as.stderr` / `wrk-executable.signature-rewrite.opt.stderr`：0 字节。

结论：

- `redis:benchmark` 没有复现 `redis:cli` 的 `return value type mismatch`，说明上一轮 storage slice 修复覆盖了 Redis 同族当前样本。
- `wrk:executable` 没有新的非合理 skip reason。
- 本轮不需要新增功能实现。

## 规模边界

补充试跑：

```bash
scripts/bench2-native-prototype-audit.sh \
  --build-dir /tmp/notdec-bin2llvm-build \
  --out-dir /tmp/notdec-bin2llvm-bench2-redis-server-prototype-audit \
  --target redis:server-symlink
```

结果：

- `notdec-native-discover` 完成，summary 显示：
  - confirmed functions: 4260
  - basic blocks: 10527
  - instructions: 47553
  - unresolved indirect call: 33
  - unresolved indirect branch: 39
- all-confirmed native LLVM 阶段运行约 10 分钟仍未结束，手动中断。
- `redis-server-symlink.native-llvm.stderr` 为空，`metrics.tsv` 只有表头。

判断：

- 这不是 signature rewrite 的功能 skip，而是大样本生成/分析性能边界。
- 和此前 `wolfssl:shared-library` 超过 12 分钟被杀属于同一类规模问题。

## 后续判断

当前候选大块任务变成：

- 性能/规模任务：定位 `redis:server-symlink` / `wolfssl:shared-library` 在 all-confirmed 阶段耗时的主要 pass 或函数。
- 继续扩展中等样本：优先选 manifest 中体量可控的 shared object / extension，观察是否出现新的非合理 skip reason。
- 暂不继续追小 CFG 变体；只有 Bench2 暴露新的功能 blocker 时再写功能复刻 plan。
