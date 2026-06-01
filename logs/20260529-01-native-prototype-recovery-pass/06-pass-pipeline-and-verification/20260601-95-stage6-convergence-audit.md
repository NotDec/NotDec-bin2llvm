# 95. 第 6 阶段收敛审计

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass。首先将相关的数据结构划分为几个工作量大约相等的部分（目前应该差不多完成了，主要是在数据集上测试并进一步提升），其次，规划一下如何做测试和验证。把进度记录到PROGRESS.md里面。依然要求，
>
> 在数据集上测试并进一步提升时，因为不涉及实现具体的模块或数据结构，可以不需要遵守上面的功能复刻流程。
>
> 1. 每一轮先看 Bench2 当前 skip reason 和真实函数样本，再决定实现点。
> 2. 先按 Ghidra 数据结构和 Bench2 blocker 识别“大块能力”，再从大块能力里切小步。小步服务于大块任务，不允许小步自己变成主线。
> 3. 每次实现不应以“一个极小 CFG 变体”作为默认粒度。应把同一类语义问题合并成一个小阶段处理，同类问题多修复一些再合并commit。

## 背景

第 90-94 步已经分别审计：

- signature rewrite skip reason；
- rewritten 函数语义样本；
- `already matches` / 空 prototype；
- multi-return callsite 使用；
- `declaration` / 外部 call effect 边界。

本轮不实现新代码，只按 `GOAL.md` 的第 6 阶段停止标准做逐项审计，判断当前阶段是否还需要继续追零散 CFG 测试。

本轮仍使用第 89 步后的完整 smoke：

```bash
/tmp/notdec-bin2llvm-bench2-multi-input-multi-return-partial-shared-smoke
```

之后没有生产代码改动。

## Bench2 基础结果

| target | elapsed | confirmed functions | `.ll/.bc/opt.bc` | signature rewrite `llvm-as` stderr | signature rewrite `opt` stderr |
| --- | ---: | ---: | --- | ---: | ---: |
| vsftpd | 85s | 187 | yes | 0 | 0 |
| libuv | 221s | 485 | yes | 0 | 0 |
| memcached | 118s | 259 | yes | 0 | 0 |

三目标都生成了：

- `signature-rewrite.ll`
- `signature-rewrite.bc`
- `signature-rewrite.opt.bc`
- `all-confirmed.ll`
- `all-confirmed.bc`
- `all-confirmed.opt.bc`

## rewrite 指标

| target | prototype functions | input candidates | return candidates | seen | rewritten | skipped |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| vsftpd | 187 | 174 | 56 | 236 | 139 | 97 |
| libuv | 485 | 414 | 157 | 571 | 338 | 233 |
| memcached | 259 | 247 | 94 | 315 | 188 | 127 |

skip reason：

| target | already matches | declaration | unsafe input | unsafe return | function uses | missing recovered prototype |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| vsftpd | 48 | 49 | 0 | 0 | 0 | 0 |
| libuv | 147 | 86 | 0 | 0 | 0 | 0 |
| memcached | 71 | 56 | 0 | 0 | 0 | 0 |

## 停止标准对照

### 1. Bench2 selected 目标稳定完成

状态：满足。

证据：

- 第 90 步记录三目标 smoke 均完成。
- 本轮复核三目标 `signature-rewrite.ll/.bc/.opt.bc` 和 `all-confirmed.ll/.bc/.opt.bc` 都存在。
- `signature-rewrite.llvm-as.stderr` 和 `signature-rewrite.opt.stderr` 都是 0 字节。

说明：

- 本轮没有重新跑完整 Bench2，因为第 89 步后没有生产代码改动。
- 后续只要改生产代码，必须重新跑同口径 smoke。

### 2. signature rewrite skip reason 已分类

状态：满足。

证据：

- 第 90 步记录 skip reason 只剩 `already matches` 和 `declaration`。
- 当前没有 `unsafe callsite input value`、`unsafe callsite return load`、`function has uses`、`missing recovered prototype`。
- 第 92 步确认 `already matches` 都是零 input/return candidate 的空 recovered prototype。
- 第 94 步确认 `declaration` 都是 LLVM declaration，不是本地函数漏改。

### 3. 非合理 skip reason 有处理结论

状态：满足。

证据：

- 第 90 步确认非合理 skip reason 已清零。
- 之前第 16-89 步已经按同类能力处理 direct callsite input、return、multi-input、multi-return、shared successor、PHI、unused return component 等问题。
- 剩余 `already matches` 和 `declaration` 都是合理 skip，不要求继续 rewrite。

### 4. rewritten 函数真实样本抽查

状态：满足当前阶段。

证据：

- 第 91 步抽查 vsftpd、libuv、memcached 的 input-only、return-only、input+return、input+multi-return 样本。
- 抽查结论是 metadata、函数签名、函数体参数替换、返回值和 callsite 一致。
- 第 93 步补充 multi-return callsite 使用审计：当前 Bench2 没有真实 `extractvalue { i64, i64 }` 消费样本，memcached 的 3 个 struct-return callsite 都未使用返回分量。

限制：

- multi-return 的“调用方消费返回分量”路径还缺真实 Bench2 样本。
- 这个限制不构成当前 Bench2 blocker，但后续如果找到真实样本，应归入 multi-return callsite 使用质量这个大块任务。

### 5. 后续不再追逐零散 CFG 组合

状态：满足。

证据：

- `GOAL.md` 已新增大块任务识别、实现粒度和阶段停止标准。
- 第 90-95 步都按 Bench2 skip reason 和真实样本先审计，再决定是否实现。
- 当前没有新的 Bench2 blocker 支持继续补单个 shared successor / multi-return / input 数量排列组合。

## 阶段结论

第 6 阶段的 direct signature rewrite 可以视为阶段性收敛：

- Bench2 三个目标当前通过 LLVM 22 汇编和 verify。
- signature rewrite 非合理 blocker 已清零。
- `already matches` 和 `declaration` 都有合理解释。
- rewritten 函数样本没有发现签名、参数、返回值或 callsite 语义错误。
- 继续补零散 CFG 组合不会明显推进 Bench2 主线。

这不是整个 native prototype recovery 长期目标完成。它只是说明第 6 阶段的 direct call signature rewrite 子目标，在当前 Bench2 样本上已经没有明确 blocker。

## 后续建议

后续工作应从新证据出发：

1. 如果改生产代码，先重新跑 Bench2 同口径 smoke。
2. 如果继续提升候选质量，优先做空 prototype 原因分类 summary，而不是补 CFG 变体。
3. 如果找到真实 multi-return 返回分量消费样本，再回到 multi-return callsite 使用质量任务。
4. 如果 Bench2 暴露外部 call effect 造成的候选误判，再按 Ghidra `FuncCallSpecs::hasEffect(...)` 复刻更细的 declaration effect。
