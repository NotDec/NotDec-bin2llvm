# 20260601-106 all-confirmed scale audit

## 背景

`redis:server-symlink` 和 `wolfssl:shared-library` 都在 all-confirmed 阶段表现出明显耗时。它们还没进入 signature rewrite skip reason gate，所以本轮先确认耗时位置，不把它误判成 prototype recovery 功能 blocker。

候选大块任务：

- all-confirmed lift / IR 生成吞吐：本轮处理。
- register SSA / prototype recovery pass 性能：先排除。
- signature rewrite 功能 skip：本轮没有新证据，不处理。

## 定位过程

先关闭后续 pass，只保留 all-confirmed lift：

```bash
timeout 120s /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/redis-server \
  --all-confirmed \
  --no-instcombine-pass \
  --no-register-ssa-pass \
  --no-prototype-recovery-pass \
  -o /tmp/notdec-bin2llvm-redis-server-no-passes.ll
```

结果：

- 120 秒超时，未产生 `.ll`。
- stdout / stderr 为空。

对照小目标：

```bash
timeout 120s /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk \
  --all-confirmed \
  --no-instcombine-pass \
  --no-register-ssa-pass \
  --no-prototype-recovery-pass \
  -o /tmp/notdec-bin2llvm-wrk-no-passes.ll
```

结果：

- `wrk` 无 pass all-confirmed 用时约 31 秒。
- 完整 manifest 审计中 `wrk` all-confirmed 也是约 32 秒。

判断：主要耗时在 all-confirmed lift / IR 生成，不在 instcombine、RegisterSSA 或 PrototypeRecovery。

## 新增脚本

新增 `scripts/bench2-native-scale-audit.sh`：

- 按 manifest `--target PROJECT:ROLE` 选择 Bench2 目标。
- 支持多个 `--limit COUNT`。
- 支持 `--timeout-seconds SECONDS`。
- 固定运行：
  - `--all-confirmed`
  - `--decode-seed-limit`
  - `--no-instcombine-pass`
  - `--no-register-ssa-pass`
  - `--no-prototype-recovery-pass`
- 输出 `metrics.tsv`，记录 target、limit、timeout、rc、elapsed、输出大小和 stderr 大小。

该脚本用于规模诊断，不做语义验证。

## 规模曲线

命令：

```bash
scripts/bench2-native-scale-audit.sh \
  --build-dir /tmp/notdec-bin2llvm-build \
  --out-dir /tmp/notdec-bin2llvm-bench2-redis-server-scale-audit \
  --target redis:server-symlink \
  --timeout-seconds 180
```

结果：

| target | limit | rc | elapsed | output bytes | stderr bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| `redis:server-symlink` | 50 | 0 | 11s | 171928 | 0 |
| `redis:server-symlink` | 100 | 0 | 22s | 322256 | 0 |
| `redis:server-symlink` | 200 | 0 | 44s | 631963 | 0 |
| `redis:server-symlink` | 400 | 0 | 87s | 1293898 | 0 |

短 smoke：

```bash
scripts/bench2-native-scale-audit.sh \
  --build-dir /tmp/notdec-bin2llvm-build \
  --out-dir /tmp/notdec-bin2llvm-bench2-scale-audit-smoke2 \
  --target wrk:executable \
  --limit 5 \
  --limit 10 \
  --timeout-seconds 60
```

结果：

- `wrk:executable limit=5`：2s，stderr 0。
- `wrk:executable limit=10`：2s，stderr 0。

## 结论

`redis:server-symlink` 的耗时和 seed limit 基本线性。按 400 个 seed 87 秒估算，4260 个 confirmed functions 会落在 15 分钟量级，和之前 10 分钟未完成、`wolfssl` 12 分钟被杀的现象一致。

这轮没有发现新的 prototype recovery 语义 blocker。后续如果要推进这个大块任务，应先优化 all-confirmed lift 的吞吐，或者给 Bench2 大目标默认走 seed-limit / 分批审计；不要把它拆成 callsite CFG 小修。
