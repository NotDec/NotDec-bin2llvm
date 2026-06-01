# 90. Bench2 signature rewrite skip reason 审计

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass。首先将相关的数据结构划分为几个工作量大约相等的部分（目前应该差不多完成了，主要是在数据集上测试并进一步提升），其次，规划一下如何做测试和验证。把进度记录到PROGRESS.md里面。依然要求，
>
> 在数据集上测试并进一步提升时，因为不涉及实现具体的模块或数据结构，可以不需要遵守上面的功能复刻流程。
>
> 1. 每一轮先看 Bench2 当前 skip reason 和真实函数样本，再决定实现点。
> 2. 先按 Ghidra 数据结构和 Bench2 blocker 识别“大块能力”，再从大块能力里切小步。小步服务于大块任务，不允许小步自己变成主线。
> 3. 每次实现不应以“一个极小 CFG 变体”作为默认粒度。应把同一类语义问题合并成一个小阶段处理，同类问题多修复一些再合并commit。

## 背景

第 81-89 步已经补齐 direct callsite rewrite 的 shared successor、multi-input、multi-return、部分返回分量使用等边界。按新的 `GOAL.md` 规则，本轮不继续补单个 CFG 变体，先审计 Bench2 最新 skip reason 和真实函数样本。

本轮使用最近一次完整 smoke：

```bash
/tmp/notdec-bin2llvm-bench2-multi-input-multi-return-partial-shared-smoke
```

这次 smoke 发生在第 89 步后；之后只改了目标文档，没有改代码，所以仍代表当前代码状态。

## Bench2 指标

| target | elapsed | prototype_functions | input_candidates | return_candidates | seen | rewritten | skipped |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| vsftpd | 85s | 187 | 174 | 56 | 236 | 139 | 97 |
| libuv | 221s | 485 | 414 | 157 | 571 | 338 | 233 |
| memcached | 118s | 259 | 247 | 94 | 315 | 188 | 127 |

skip reason：

| target | already matches | declaration | unsafe input | unsafe return | function uses |
| --- | ---: | ---: | ---: | ---: | ---: |
| vsftpd | 48 | 49 | 0 | 0 | 0 |
| libuv | 147 | 86 | 0 | 0 | 0 |
| memcached | 71 | 56 | 0 | 0 | 0 |

当前 signature rewrite 的非合理 blocker 已经清零：没有 `unsafe callsite input value`、`unsafe callsite return load`、`function has uses`。

## 真实样本抽查

抽查 rewritten 函数：

- vsftpd
  - `notdec_native_17960`: `i64 @notdec_native_17960(i64 %RDI.external_input1)`
  - callsite 示例：`%15 = call i64 @notdec_native_17960(i64 0)`
  - `notdec_native_8410`: `void @notdec_native_8410(i64 %RDI.external_input1)`
  - callsite 示例：`call void @notdec_native_8410(i64 130842)`
- libuv
  - `uv_library_shutdown`: `i64 @uv_library_shutdown()`
  - `notdec_native_9e70`: `i64 @notdec_native_9e70(i64 %RSI.external_input1, i64 %RDX.external_input2)`
  - callsite 示例：`%216 = call i64 @notdec_native_9e70(i64 %unique_df00_8198, i64 %RCX308)`
  - callsite 示例：`%254 = call i64 @notdec_native_9e70(i64 %RCX357, i64 %RDX.regssa)`
- memcached
  - `notdec_native_bc60`: `{ i64, i64 } @notdec_native_bc60(i64 %RDI.external_input1, i64 %RSI.external_input2)`
  - callsite 示例：`%12 = call { i64, i64 } @notdec_native_bc60(i64 %unique_df00_8, i64 18)`
  - `notdec_native_f3f0`: `i64 @notdec_native_f3f0()`
  - callsite 示例：`%2 = call i64 @notdec_native_f3f0()`

LLVM 22 `llvm-as` / `opt` stderr 为空，说明 signature rewrite 产物通过汇编和 verify。

## 候选大块任务

### 1. 真实函数语义抽查

- Ghidra 对应：
  - `coreaction.cc`: `ActionParamDouble::apply(...)`、`ActionActiveReturn::apply(...)`
  - `fspec.cc`: `FuncCallSpecs::buildInputFromTrials(...)`、`FuncCallSpecs::buildOutputFromTrials(...)`
- native 缺口：
  - 当前 verifier 通过，但还缺系统性的人工语义抽查记录。
  - 需要确认参数顺序、返回值分量、callsite 参数来源不是偶然匹配。
- Bench2 影响：
  - 不一定改变 rewrite 数量，但直接决定第 6 阶段能否收敛。
- 收敛标准：
  - 每个 target 至少抽查若干 rewritten 函数，覆盖 input-only、return-only、input+return、multi-return。
  - 对明显可疑样本记录到后续实现任务。

### 2. already matches / 空 prototype 质量审计

- Ghidra 对应：
  - `fspec.cc`: `FuncCallSpecs::buildInputFromTrials(...)`、`FuncCallSpecs::buildOutputFromTrials(...)`
  - `coreaction.cc`: `ActionPrototypeTypes::apply(...)`
- native 缺口：
  - 当前剩余 skipped 大量是 `already matches`。
  - 其中很多是空 recovered prototype 的 `void()` native 函数，需要确认是真无候选，还是候选筛选过保守。
- Bench2 影响：
  - 如果发现误空 prototype，会增加 candidate 和 rewritten 数量。
- 收敛标准：
  - 对每个 target 抽查若干 `already matches` native 函数。
  - 分类为空函数/无真实 use/候选缺失/ABI 模型限制。

### 3. declaration 处理边界记录

- Ghidra 对应：
  - `FuncCallSpecs::hasEffect(...)`
  - 外部函数 prototype / effect 信息。
- native 缺口：
  - 当前 declaration 是合理 skip，但外部函数 effect 仍影响 register SSA 和参数传播。
- Bench2 影响：
  - 不直接减少 signature rewrite skipped，因为 declaration 不应 rewrite。
  - 可能影响后续 call effect 精度。
- 收敛标准：
  - 明确 declaration 保持不 rewrite。
  - 只在 call effect 影响真实候选时另开任务。

## 本轮选择

本轮不实现代码。原因：

- signature rewrite 非合理 blocker 已清零。
- 继续补单个 CFG 变体不会改变 Bench2 结果。
- 下一步应先做真实函数语义抽查，确认 rewritten 结果质量；再决定是否回到实现。

推荐下一块任务：`真实函数语义抽查`。它比继续扩展 callsite rewrite 测试更接近阶段停止标准。

## 结论

当前第 6 阶段的 direct call signature rewrite blocker 已经阶段性收敛：

- Bench2 三个目标 smoke 通过。
- rewrite skipped 只剩 `already matches` 和 `declaration`。
- 没有 unsafe input、unsafe return、function uses。

后续重点应从“补 rewrite CFG 组合”切换为：

1. rewritten 函数语义抽查；
2. `already matches` / 空 prototype 质量审计；
3. 只有发现真实问题后，再回到 Ghidra 数据结构或 native 实现补强。
