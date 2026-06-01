# 103. manifest 驱动 prototype rewrite 扩展审计脚本

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

第 98 到 102 步已经手工审计了 `lighttpd`、`lighttpd-angel`、`tmux`、`ssh`、`sshd`。手工命令重复，而且每次都要人工检查 LLVM verify stderr、signature rewrite skip reason 和指标。

本轮不复刻新的 Ghidra 数据结构，而是把数据集审计流程沉淀成脚本，方便后续继续扩展 Bench2 目标。

## 候选大块任务

### 1. manifest 驱动 prototype rewrite 审计

- Ghidra 对应：
  - `FuncCallSpecs::buildInputFromTrials(...)`
  - `FuncCallSpecs::deriveOutputMap(...)`
  - `FuncCallSpecs::hasEffect(...)`
- native 当前缺口：
  - 真实目标审计已成为主要推进方式，但还依赖手工命令。
  - 需要稳定记录每个目标的 prototype summary、signature rewrite summary、LLVM verify 结果和 skip reason。
- 本轮选择：
  - 新增脚本，按 manifest 的 `project:role` 选择目标运行同口径审计。

### 2. 继续扩展到 wolfssl / redis

- native 当前缺口：
  - 可能暴露新 blocker。
- 本轮判断：
  - 先做脚本，再跑新目标，避免继续复制长命令。

### 3. 旧三目标 smoke 合并

- native 当前缺口：
  - `bench2-native-smoke.sh` 有 discovery、CFG、heritage 等更强 gate。
- 本轮判断：
  - 不合并。新脚本只负责 prototype/signature 审计，避免影响旧 smoke。

## native 侧计划

新增 `scripts/bench2-native-prototype-audit.sh`：

1. 从 Bench2 manifest 读取 `project:role`。
2. 对每个目标运行：
   - `notdec-native-discover --summary-json`
   - `notdec-native-llvm --all-confirmed --prototype-recovery-summary`
   - LLVM 22 `llvm-as` / `opt -passes=verify`
   - `notdec-native-llvm --rewrite-prototype-signatures`
   - LLVM 22 `llvm-as` / `opt -passes=verify`
3. 生成 `metrics.tsv`，记录 discovery、prototype、signature rewrite 和耗时指标。
4. 默认只允许 `already matches` / `declaration` 两类 skip reason；出现 `unsafe callsite input value`、`unsafe callsite return load`、`function has uses` 等就失败。
5. 输出每个目标的 `.ll`、`.bc`、stderr、summary，方便后续定位。

## 判断标准

- 脚本 `bash -n` 通过。
- 至少用 `lighttpd-angel` 运行一次，确认能通过 LLVM 22 verify 和 skip reason gate。
- 日志和 `PROGRESS.md` 更新。

## 风险

这个脚本不会证明 native IR 语义完整，只是把当前 prototype/signature rewrite 审计自动化。旧三目标的发现质量、CFG、heritage 相关 gate 仍由 `bench2-native-smoke.sh` 负责。

## 实现记录

已实现。

### 改动

- `scripts/bench2-native-prototype-audit.sh`
  - 新增 manifest 目标选择，使用 `--target PROJECT:ROLE` 指定一个或多个 Bench2 目标。
  - 支持 `--build-dir`、`--bench2-root`、`--manifest`、`--out-dir`、`--llvm-bin`。
  - 对每个目标生成：
    - discovery summary；
    - all-confirmed `.ll` / `.bc` / verify 输出；
    - signature-rewrite `.ll` / `.bc` / verify 输出；
    - `metrics.tsv`。
  - 默认只允许 `already matches` / `declaration` 两类 signature rewrite skip reason。
  - 可用 `--allow-skip-reason` 扩展 allowlist，方便临时审计新 blocker。

### 验证

- `bash -n scripts/bench2-native-prototype-audit.sh`：通过。
- 小目标完整链路：

```bash
scripts/bench2-native-prototype-audit.sh \
  --build-dir /tmp/notdec-bin2llvm-build \
  --out-dir /tmp/notdec-bin2llvm-prototype-audit-script-smoke \
  --target lighttpd:helper
```

结果：

| metric | value |
| --- | ---: |
| target | `lighttpd:helper` |
| confirmed functions | 11 |
| basic blocks | 37 |
| instructions | 101 |
| prototype functions | 11 |
| input candidates | 2 |
| return candidates | 1 |
| signature rewrite needed | 2 |
| signature rewrite seen | 32 |
| signature rewrite rewritten | 2 |
| signature rewrite skipped | 30 |

skip reason：

| reason | count |
| --- | ---: |
| already matches | 9 |
| declaration | 21 |

LLVM 22 验证：all-confirmed 和 signature-rewrite 的 `llvm-as` / `opt -passes=verify` stderr 都是 0 字节。

### 评价

- 实现效果：4/5。解决了手工扩展审计命令重复的问题。
- 复杂度：2/5。独立脚本，不影响旧 smoke 和 pass。
- 维护成本：2/5。后续如果需要更强 gate，可以在这个脚本里分层加，不碰旧三目标 smoke。
