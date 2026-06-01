# 101. lighttpd-angel / tmux 扩展 Bench2 审计

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass。首先将相关的数据结构划分为几个工作量大约相等的部分（目前应该差不多完成了，主要是在数据集上测试并进一步提升），其次，规划一下如何做测试和验证。把进度记录到PROGRESS.md里面。依然要求，
>
> 根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：
>
> - 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
> - 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
> - 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
>
> 在数据集上测试并进一步提升时，因为不涉及实现具体的模块或数据结构，可以不需要遵守上面的功能复刻流程。
>
> 1. 每一轮先看 Bench2 当前 skip reason 和真实函数样本，再决定实现点。
> 2. 先按 Ghidra 数据结构和 Bench2 blocker 识别“大块能力”，再从大块能力里切小步。小步服务于大块任务，不允许小步自己变成主线。
> 3. 每次实现不应以“一个极小 CFG 变体”作为默认粒度。应把同一类语义问题合并成一个小阶段处理，同类问题多修复一些再合并commit。

## 背景

第 100 步清零 `lighttpd` 的 `unsafe callsite input value` 和 `unsafe callsite return load` 后，本轮继续按 Bench2 manifest 扩展真实目标，确认修复是否只适合 `lighttpd`，以及是否会暴露新的大块能力缺口。

本轮是数据集审计，不新增功能复刻 plan，也不改代码。

目标：

- `/sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/lighttpd-angel`
- `/sn640/NotDec-Exp/Bench2/rootfs/usr/bin/tmux`

输出目录：

- `/tmp/notdec-bin2llvm-bench2-lighttpd-angel-prototype-audit`
- `/tmp/notdec-bin2llvm-bench2-tmux-prototype-audit`

## 候选大块任务

### 1. 扩展 Bench2 真实样本并寻找新 blocker

- Ghidra 对应：
  - `FuncCallSpecs::buildInputFromTrials(...)`
  - `FuncCallSpecs::deriveOutputMap(...)`
  - `FuncCallSpecs::hasEffect(...)`
- native 缺口：
  - 旧样本和 `lighttpd` 当前已经没有非合理 signature rewrite skip reason。
  - 需要继续扩展真实目标，判断下一块实现是否来自 callsite、函数指针 use、indirect call、stack 参数或其他能力。
- 收敛标准：
  - all-confirmed 和 signature rewrite IR 通过 LLVM 22 assemble/verify。
  - signature rewrite skip reason 只剩合理分类，或明确记录新 blocker。

### 2. manifest 驱动 smoke

- native 缺口：
  - 当前 `scripts/bench2-native-smoke.sh` 仍硬编码旧三目标。
  - 后续如果继续扩大样本，应考虑 manifest 选择和分层 gate。
- 本轮判断：
  - 先不实现。需要更多目标数据后再决定 smoke gate 怎么分层。

### 3. callsite rewrite 计划结构化

- native 缺口：
  - callsite input / return 的 helper 已经较多。
  - 后续可能需要统一成显式 rewrite plan，降低维护成本。
- 本轮判断：
  - 没有新 skip reason 逼迫立即重构，先继续数据集审计。

## 验证命令

每个目标都按同口径执行：

```bash
BUILD=/tmp/notdec-bin2llvm-build
LLVM=/sn640/NotDec/llvm-22.1.0.obj/bin

$BUILD/bin/notdec-native-discover --summary-json "$TARGET" > "$OUT/summary.json"
$BUILD/bin/notdec-native-llvm "$TARGET" --all-confirmed --prototype-recovery-summary -o "$OUT/all-confirmed.ll"
$LLVM/llvm-as "$OUT/all-confirmed.ll" -o "$OUT/all-confirmed.bc"
$LLVM/opt -passes=verify "$OUT/all-confirmed.bc" -o "$OUT/all-confirmed.opt.bc"
$BUILD/bin/notdec-native-llvm "$TARGET" --all-confirmed --prototype-recovery-summary --rewrite-prototype-signatures -o "$OUT/signature-rewrite.ll"
$LLVM/llvm-as "$OUT/signature-rewrite.ll" -o "$OUT/signature-rewrite.bc"
$LLVM/opt -passes=verify "$OUT/signature-rewrite.bc" -o "$OUT/signature-rewrite.opt.bc"
```

实际命令额外用 `/usr/bin/time` 记录 discover / all-confirmed / signature-rewrite 时间。

## lighttpd-angel 结果

基础指标：

| metric | value |
| --- | ---: |
| confirmed functions | 11 |
| basic blocks | 37 |
| instructions | 101 |
| call xrefs | 2 |
| unresolved indirect calls | 0 |
| unresolved indirect branches | 0 |

prototype recovery：

| metric | value |
| --- | ---: |
| functions | 11 |
| external inputs | 15 |
| input candidates | 2 |
| return candidates | 1 |
| rewrite eligible functions | 11 |
| signature rewrite needed functions | 2 |

signature rewrite：

| metric | value |
| --- | ---: |
| seen | 32 |
| rewritten | 2 |
| skipped | 30 |

skip reason：

| reason | count |
| --- | ---: |
| already matches | 9 |
| declaration | 21 |

耗时：

| step | seconds |
| --- | ---: |
| discover | 0.24 |
| all-confirmed | 2.63 |
| signature-rewrite | 2.78 |

LLVM 22 验证：all-confirmed 和 signature-rewrite 的 `llvm-as` / `opt -passes=verify` stderr 都是 0 字节。

## tmux 结果

基础指标：

| metric | value |
| --- | ---: |
| confirmed functions | 1307 |
| basic blocks | 3520 |
| instructions | 15145 |
| call xrefs | 510 |
| unresolved indirect calls | 0 |
| unresolved indirect branches | 20 |

prototype recovery：

| metric | value |
| --- | ---: |
| functions | 1307 |
| external inputs | 6767 |
| input candidates | 1446 |
| return candidates | 364 |
| rewrite eligible functions | 1307 |
| signature rewrite needed functions | 1082 |

signature rewrite：

| metric | value |
| --- | ---: |
| seen | 1348 |
| rewritten | 1082 |
| skipped | 266 |

skip reason：

| reason | count |
| --- | ---: |
| already matches | 225 |
| declaration | 41 |

耗时：

| step | seconds |
| --- | ---: |
| discover | 1.28 |
| all-confirmed | 296.71 |
| signature-rewrite | 300.26 |

LLVM 22 验证：all-confirmed 和 signature-rewrite 的 `llvm-as` / `opt -passes=verify` stderr 都是 0 字节。

额外观察：

- `tmux` 有 2 个 `{ i64, i64 }` struct-return direct call。
- 没有发现 `.old` 残留。
- 没有 `unsafe callsite input value`、`unsafe callsite return load`、`function has uses` 这类非合理 skip reason。

## 结论

本轮两个新目标没有暴露新的 prototype recovery blocker：

- `lighttpd-angel` 很小，只能说明 helper 目标没有回退。
- `tmux` 规模更大，1307 个 confirmed functions，1082 个需要 rewrite 的函数全部改写，并通过 LLVM 22 verify。

当前 direct signature rewrite 在 `vsftpd/libuv/memcached/lighttpd/lighttpd-angel/tmux` 上的非合理 skip reason 已经清零。下一轮更有价值的方向不是继续在这些目标里补 CFG 小形状，而是：

1. 继续扩展到 manifest 后面的 `ssh` / `sshd`，寻找新的真实 blocker。
2. 或者开始整理 manifest 驱动 smoke，把旧三目标和扩展审计目标分层纳入自动验证。
3. 如果后续目标仍然干净，再做一次阶段性收敛审计，明确长期 Goal 还剩 indirect call、栈参数、类型细化、函数指针 use 等未覆盖范围。
