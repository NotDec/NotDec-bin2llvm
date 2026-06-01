# 98. lighttpd 扩展 Bench2 样本审计

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass。首先将相关的数据结构划分为几个工作量大约相等的部分（目前应该差不多完成了，主要是在数据集上测试并进一步提升），其次，规划一下如何做测试和验证。把进度记录到PROGRESS.md里面。依然要求，
>
> 在数据集上测试并进一步提升时，因为不涉及实现具体的模块或数据结构，可以不需要遵守上面的功能复刻流程。
>
> 1. 每一轮先看 Bench2 当前 skip reason 和真实函数样本，再决定实现点。
> 2. 先按 Ghidra 数据结构和 Bench2 blocker 识别“大块能力”，再从大块能力里切小步。小步服务于大块任务，不允许小步自己变成主线。
> 3. 每次实现不应以“一个极小 CFG 变体”作为默认粒度。应把同一类语义问题合并成一个小阶段处理，同类问题多修复一些再合并commit。

## 背景

第 97 步确认长期 Goal 还不能标完成。第 6 阶段在 `vsftpd/libuv/memcached` 上收敛后，继续在同一批样本上做审计收益不大。本轮扩大到 Bench2 manifest 里的下一个真实目标 `lighttpd`，寻找新的真实 blocker。

目标：

```text
/sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/lighttpd
```

输出目录：

```text
/tmp/notdec-bin2llvm-bench2-lighttpd-prototype-audit
```

说明：开始时 `/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm` 是旧构建，不支持 `--prototype-recovery-summary`。已先执行 `cmake --build /tmp/notdec-bin2llvm-build -j$(nproc)` 更新构建，再重跑验证。

## 候选大块任务

### 1. 扩大 Bench2 真实样本，寻找新 blocker

- Ghidra 对应：
  - `FuncCallSpecs::buildInputFromTrials(...)`
  - `FuncCallSpecs::deriveOutputMap(...)`
  - `FuncCallSpecs::hasEffect(...)`
- native 缺口：
  - 旧三目标没有非合理 skip reason，无法继续定位真实问题。
  - 需要更大目标暴露新的 callsite input / return load / function use 形状。
- Bench2 影响：
  - 直接决定下一轮实现点。
- 收敛标准：
  - 新目标能生成 IR 并通过 LLVM 22 assemble/verify。
  - 记录 signature rewrite skip reason 和真实样本。

### 2. manifest 驱动 smoke

- native 缺口：
  - `scripts/bench2-native-smoke.sh` 当前硬编码 `vsftpd/libuv/memcached`。
  - 后续如果要系统扩大样本，应支持从 manifest 选目标。

### 3. callsite input / return load 能力补强

- native 缺口：
  - 只有新目标暴露出非合理 skip reason 后，才值得回到实现。

## 本轮选择

本轮选择 `扩大 Bench2 真实样本，寻找新 blocker`。这是数据集验证，不新建功能复刻 plan。

## 验证命令

手动执行同口径流程：

```bash
OUT=/tmp/notdec-bin2llvm-bench2-lighttpd-prototype-audit
BUILD=/tmp/notdec-bin2llvm-build
LLVM=/sn640/NotDec/llvm-22.1.0.obj/bin
TARGET=/sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/lighttpd

$BUILD/bin/notdec-native-discover --summary-json "$TARGET" > "$OUT/lighttpd.summary.json"
$BUILD/bin/notdec-native-llvm "$TARGET" --all-confirmed --prototype-recovery-summary -o "$OUT/lighttpd.all-confirmed.ll"
$LLVM/llvm-as "$OUT/lighttpd.all-confirmed.ll" -o "$OUT/lighttpd.all-confirmed.bc"
$LLVM/opt -passes=verify "$OUT/lighttpd.all-confirmed.bc" -o "$OUT/lighttpd.all-confirmed.opt.bc"
$BUILD/bin/notdec-native-llvm "$TARGET" --all-confirmed --prototype-recovery-summary --rewrite-prototype-signatures -o "$OUT/lighttpd.signature-rewrite.ll"
$LLVM/llvm-as "$OUT/lighttpd.signature-rewrite.ll" -o "$OUT/lighttpd.signature-rewrite.bc"
$LLVM/opt -passes=verify "$OUT/lighttpd.signature-rewrite.bc" -o "$OUT/lighttpd.signature-rewrite.opt.bc"
```

## 基础指标

| metric | value |
| --- | ---: |
| confirmed functions | 893 |
| basic blocks | 2175 |
| instructions | 9560 |
| call xrefs | 349 |
| unresolved indirect calls | 5 |
| unresolved indirect branches | 5 |

prototype recovery：

| metric | value |
| --- | ---: |
| functions | 893 |
| external inputs | 4048 |
| input candidates | 984 |
| return candidates | 317 |
| rewrite eligible functions | 893 |
| signature rewrite needed functions | 702 |

signature rewrite：

| metric | value |
| --- | ---: |
| seen | 967 |
| rewritten | 691 |
| skipped | 276 |

LLVM 22 验证：

| artifact | stderr size |
| --- | ---: |
| all-confirmed `llvm-as` | 0 |
| all-confirmed `opt -passes=verify` | 0 |
| signature-rewrite `llvm-as` | 0 |
| signature-rewrite `opt -passes=verify` | 0 |

## skip reason

| reason | count |
| --- | ---: |
| already matches | 191 |
| declaration | 74 |
| unsafe callsite input value | 7 |
| unsafe callsite return load | 4 |

这和旧三目标不同：`lighttpd` 重新暴露了非合理 skip reason。

## 新 blocker 样本

`unsafe callsite input value`：

- `log_perror`
- `log_debug`
- `log_error`
- `buffer_extend`
- `notdec_native_3acd0`
- `http_chunk_append_file_ref_range`
- `http_chunk_append_file_fd_range`

这些样本多为具名函数，并且常涉及 ABI 后段寄存器或类似 vararg/logging 的调用形状。例如 `log_debug` / `log_error` 的 recovered prototype 包含 `R8/R9` 输入，callsite 侧不是简单同块 store 可取值。

`unsafe callsite return load`：

- `notdec_native_f558`
- `notdec_native_1c6d8`
- `http_method_buf`
- `http_request_parse_target`

这些样本暴露的是返回寄存器 load 查找边界。例如 `http_method_buf` 在调用后读取 `RAX`，但 recovered prototype 有两个返回分量，第二个返回分量的使用/未使用路径需要更精确判断。

额外观察：

- signature rewrite 输出没有 `.old` 残留。
- `{ i64, i64 }` struct return 定义有 52 个，direct struct call 有 2 个，但仍没有 `extractvalue { i64, i64 }`。
- `lighttpd` 有 unresolved indirect call 5 个，不满足当前 smoke 脚本对旧三目标的 “no unresolved indirect calls” gate；本轮只作为扩展审计样本，不改 smoke gate。

## 结论

`lighttpd` 是有效的新压力样本：

- all-confirmed 和 signature rewrite IR 都能通过 LLVM 22 assemble/verify。
- signature rewrite 能改写 691 个函数。
- 同时暴露 11 个非合理 skip reason，给后续实现提供真实 blocker。

下一轮不应继续做文档审计。应从 `lighttpd` 的两类 blocker 中选择一个大块能力实现：

1. callsite input value 查找补强：
   - 对应 Ghidra `FuncCallSpecs::buildInputFromTrials(...)`。
   - 目标是解释 `log_perror/log_debug/log_error/buffer_extend` 等真实样本。
2. callsite return load 查找补强：
   - 对应 Ghidra `FuncCallSpecs::deriveOutputMap(...)`。
   - 目标是解释 `http_method_buf/http_request_parse_target` 等真实样本。

优先建议先做 input blocker，因为它有 7 个样本，且包含多个具名 logging/helper 函数。
