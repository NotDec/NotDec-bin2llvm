# 94. Bench2 declaration / call effect 边界审计

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass。首先将相关的数据结构划分为几个工作量大约相等的部分（目前应该差不多完成了，主要是在数据集上测试并进一步提升），其次，规划一下如何做测试和验证。把进度记录到PROGRESS.md里面。依然要求，
>
> 在数据集上测试并进一步提升时，因为不涉及实现具体的模块或数据结构，可以不需要遵守上面的功能复刻流程。
>
> 1. 每一轮先看 Bench2 当前 skip reason 和真实函数样本，再决定实现点。
> 2. 先按 Ghidra 数据结构和 Bench2 blocker 识别“大块能力”，再从大块能力里切小步。小步服务于大块任务，不允许小步自己变成主线。
> 3. 每次实现不应以“一个极小 CFG 变体”作为默认粒度。应把同一类语义问题合并成一个小阶段处理，同类问题多修复一些再合并commit。

## 背景

第 90 步确认 signature rewrite 只剩 `already matches` 和 `declaration`。第 92 步已经确认 `already matches` 不是 rewrite 漏处理。本轮审计剩余的 `declaration`，确认它们是否都是外部声明 / LLVM intrinsic，并记录它们和 call effect 的边界。

本轮仍使用第 89 步后的完整 smoke：

```bash
/tmp/notdec-bin2llvm-bench2-multi-input-multi-return-partial-shared-smoke
```

之后没有生产代码改动。

## 候选大块任务

### 1. declaration / 外部 call effect 边界审计

- Ghidra 对应：
  - `fspec.cc`: `FuncCallSpecs::hasEffect(...)`
  - `fspec.hh`: `FuncCallSpecs`
  - `cspec.cc`: compiler spec 里的 `unaffected` / `killedbycall`
- native 缺口：
  - declaration 本身不应 rewrite，但外部函数副作用会影响 register SSA、返回 load 查找和候选恢复。
  - 需要确认当前 `declaration` skip 不是本地函数漏改。
- Bench2 影响：
  - 不直接减少 rewrite skipped 数量。
  - 如果外部 call effect 过粗，后续可能影响 candidate recovery 精度。
- 收敛标准：
  - declaration skip 都对应 LLVM declaration。
  - 不存在 declaration 带 input/return candidate 却被漏 rewrite 的情况。
  - 当前 call effect 边界有明确结论。

### 2. 空 prototype 原因分类 summary

- Ghidra 对应：
  - `FuncCallSpecs::buildInputFromTrials(...)`、`FuncCallSpecs::buildOutputFromTrials(...)`
- native 缺口：
  - 空 prototype 已确认不是 rewrite 漏处理，但原因还没有自动分类。

### 3. multi-return 真实消费样本补充

- Ghidra 对应：
  - `FuncCallSpecs::deriveOutputMap(...)`
- native 缺口：
  - 第 93 步确认当前 Bench2 没有真实 `extractvalue { i64, i64 }` 消费样本。

## 本轮选择

本轮选择 `declaration / 外部 call effect 边界审计`。原因是它是第 90 步剩余的另一个 skip reason，也是第 6 阶段停止标准里必须分类清楚的合理 skip。

## 统计结果

| target | declaration | LLVM intrinsic | notdec helper | external import |
| --- | ---: | ---: | ---: | ---: |
| vsftpd | 49 | 13 | 1 | 35 |
| libuv | 86 | 15 | 1 | 70 |
| memcached | 56 | 14 | 1 | 41 |

抽查前 20 个 `declaration` skip，IR 中均为 `declare`，不是本地 `define`：

- LLVM intrinsic：`llvm.ctpop.*`、`llvm.ssub.with.overflow.*`、`llvm.uadd.with.overflow.*`、`llvm.sadd.with.overflow.*`、`llvm.readcyclecounter`。
- NotDec helper：`notdec_plt0_resolver`。
- 外部导入：`__gmon_start__`、`__cxa_finalize`、`malloc`、`free`、`memcpy`、`bind`、`abort`、`pthread_mutex_lock`、`perror` 等。

没有发现 declaration 带非零 prototype candidate：

| target | declaration with input candidates | declaration with return candidates |
| --- | ---: | ---: |
| vsftpd | 0 | 0 |
| libuv | 0 | 0 |
| memcached | 0 | 0 |

## 高频外部调用样本

| target | 高频样本 |
| --- | --- |
| vsftpd | `fork` 3 次，`memcmp` 2 次，其余多为 1 次 |
| libuv | `abort` 49 次，`__assert_fail` 18 次，`__errno_location` 10 次 |
| memcached | `perror` 15 次，`__stack_chk_fail` 8 次，`pthread_mutex_lock` 4 次 |

这些函数保持 declaration 是合理的：native signature rewrite 只处理当前 module 内有函数体和 recovered prototype 的函数，不应给外部导入创建替代函数体。

## 当前 native 边界

相关代码：

- `lib/passes/NativePrototypeRecovery.cpp`
  - `runNativePrototypeRecovery(...)` 遇到 declaration 直接跳过候选恢复。
  - `getNativePrototypeRewriteEligibility(...)` 对 declaration 返回 reason `declaration`。
  - `callClobbersRegister(...)` 对 intrinsic 返回不 clobber；对本地定义优先看 `notdec.register.preserves` / `notdec.register.clobbers`；对 declaration / unknown call 回落 ABI unaffected 规则。
- `lib/passes/NativeRegisterSSA.cpp`
  - `callClobbersRegister(...)` 同样对本地定义使用 callee metadata，对 declaration 回落 ABI unaffected 规则。
  - direct callee-first 排序跳过 declaration。
- `include/notdec-bin2llvm/NativeAbi.h`
  - `NativeAbiEffect` 保留 Ghidra `unaffected` / `killedbycall` 结构，供 call effect 查询。

这个边界和 Ghidra 思路一致：`FuncCallSpecs::hasEffect(...)` 会基于已知 function prototype / compiler spec 判断 call 对 varnode 的影响。native 侧目前没有完整外部函数 prototype database，所以 declaration 只能按 ABI effect 保守处理。

## 结论

`declaration` 是合理 skip，不是 signature rewrite 剩余 blocker：

- 它们都是 LLVM declaration，没有本地函数体可 rewrite。
- 它们没有 recovered input/return candidate。
- 当前 rewrite 只处理 module 内定义函数，边界正确。

外部 call effect 仍是后续可能提升的方向，但不是第 6 阶段 direct signature rewrite 的阻塞项。只有当 Bench2 后续出现明确 candidate recovery 问题，并且证据指向外部函数 effect 过粗时，才应回到 `FuncCallSpecs::hasEffect(...)` 对应的大块能力继续实现。

下一步建议：

1. 做第 6 阶段停止标准汇总审计，逐项对照 `GOAL.md` 判断是否阶段性收敛。
2. 如果继续实现，优先从真实 Bench2 新 blocker 出发，不继续补零散 CFG 组合。
