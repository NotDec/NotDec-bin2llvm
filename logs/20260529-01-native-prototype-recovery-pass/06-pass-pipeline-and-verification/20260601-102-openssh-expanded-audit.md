# 102. OpenSSH ssh / sshd 扩展 Bench2 审计

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

第 101 步确认 `lighttpd-angel` 和 `tmux` 没有新的 signature rewrite blocker。本轮继续按 Bench2 manifest 扩展到 OpenSSH client/server：

- `/sn640/NotDec-Exp/Bench2/rootfs/usr/bin/ssh`
- `/sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/sshd`

这两个目标比 helper 更接近真实应用，但规模低于 `tmux`。本轮仍是数据集审计，不新增功能复刻 plan，也不改代码。

输出目录：

- `/tmp/notdec-bin2llvm-bench2-ssh-prototype-audit`
- `/tmp/notdec-bin2llvm-bench2-sshd-prototype-audit`

## 候选大块任务

### 1. 扩展 Bench2 真实样本，寻找新 blocker

- Ghidra 对应：
  - `FuncCallSpecs::buildInputFromTrials(...)`
  - `FuncCallSpecs::deriveOutputMap(...)`
  - `FuncCallSpecs::hasEffect(...)`
- native 当前缺口：
  - 已有样本的 direct signature rewrite 非合理 skip reason 已清零。
  - 需要继续看真实项目是否暴露 `unsafe callsite input value`、`unsafe callsite return load`、`function has uses`、间接调用等新问题。
- 本轮选择：
  - 继续扩大样本，不先做新实现。

### 2. manifest 驱动 smoke 分层

- native 当前缺口：
  - 大目标手工审计成本变高。
  - 旧 smoke 只覆盖 `vsftpd/libuv/memcached`，扩展目标还没有自动化 gate。
- 本轮判断：
  - OpenSSH 仍然干净，说明下一轮可以考虑把扩展审计沉淀成脚本，而不是继续手工跑。

### 3. indirect branch / indirect call 后续范围

- native 当前缺口：
  - `ssh` / `sshd` 都没有 unresolved indirect call，但仍有少量 unresolved indirect branch。
  - 这影响 native IR 语义完整性，但不直接表现为 prototype recovery signature rewrite skip reason。

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

## ssh 结果

基础指标：

| metric | value |
| --- | ---: |
| confirmed functions | 712 |
| basic blocks | 1255 |
| instructions | 6777 |
| call xrefs | 97 |
| unresolved indirect calls | 0 |
| unresolved indirect branches | 1 |

prototype recovery：

| metric | value |
| --- | ---: |
| functions | 712 |
| external inputs | 3880 |
| input candidates | 695 |
| return candidates | 207 |
| rewrite eligible functions | 712 |
| signature rewrite needed functions | 538 |

signature rewrite：

| metric | value |
| --- | ---: |
| seen | 787 |
| rewritten | 538 |
| skipped | 249 |

skip reason：

| reason | count |
| --- | ---: |
| already matches | 174 |
| declaration | 75 |

耗时：

| step | seconds |
| --- | ---: |
| discover | 0.66 |
| all-confirmed | 159.55 |
| signature-rewrite | 159.89 |

LLVM 22 验证：all-confirmed 和 signature-rewrite 的 `llvm-as` / `opt -passes=verify` stderr 都是 0 字节。

## sshd 结果

基础指标：

| metric | value |
| --- | ---: |
| confirmed functions | 836 |
| basic blocks | 1432 |
| instructions | 7885 |
| call xrefs | 119 |
| unresolved indirect calls | 0 |
| unresolved indirect branches | 1 |

prototype recovery：

| metric | value |
| --- | ---: |
| functions | 836 |
| external inputs | 4392 |
| input candidates | 743 |
| return candidates | 286 |
| rewrite eligible functions | 836 |
| signature rewrite needed functions | 633 |

signature rewrite：

| metric | value |
| --- | ---: |
| seen | 910 |
| rewritten | 633 |
| skipped | 277 |

skip reason：

| reason | count |
| --- | ---: |
| already matches | 203 |
| declaration | 74 |

耗时：

| step | seconds |
| --- | ---: |
| discover | 0.72 |
| all-confirmed | 189.59 |
| signature-rewrite | 189.31 |

LLVM 22 验证：all-confirmed 和 signature-rewrite 的 `llvm-as` / `opt -passes=verify` stderr 都是 0 字节。

## 结论

OpenSSH client/server 没有暴露新的 direct signature rewrite blocker：

- `ssh`：538 个需要 rewrite 的函数全部改写。
- `sshd`：633 个需要 rewrite 的函数全部改写。
- 两个目标都只剩 `already matches` / `declaration` 两类合理 skip reason。
- 没有 `unsafe callsite input value`、`unsafe callsite return load`、`function has uses`、`missing recovered prototype`。

当前 direct signature rewrite 已在 `vsftpd/libuv/memcached/lighttpd/lighttpd-angel/tmux/ssh/sshd` 上没有非合理 skip reason。下一步建议优先做两件事之一：

1. 写 manifest 驱动的扩展 smoke，把已审计目标分层纳入自动验证，避免继续手工复制命令。
2. 继续扩到 `wolfssl` 或 `redis-server`，寻找不同项目类型的新 blocker。

如果后续扩展目标仍然干净，就应做一次阶段性收敛审计，明确长期目标还剩 indirect control flow、栈参数、类型细化、函数指针 use 和更完整的 Ghidra `ParamActive` 复刻。
