# 93. Bench2 multi-return callsite 使用审计

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass。首先将相关的数据结构划分为几个工作量大约相等的部分（目前应该差不多完成了，主要是在数据集上测试并进一步提升），其次，规划一下如何做测试和验证。把进度记录到PROGRESS.md里面。依然要求，
>
> 在数据集上测试并进一步提升时，因为不涉及实现具体的模块或数据结构，可以不需要遵守上面的功能复刻流程。
>
> 1. 每一轮先看 Bench2 当前 skip reason 和真实函数样本，再决定实现点。
> 2. 先按 Ghidra 数据结构和 Bench2 blocker 识别“大块能力”，再从大块能力里切小步。小步服务于大块任务，不允许小步自己变成主线。
> 3. 每次实现不应以“一个极小 CFG 变体”作为默认粒度。应把同一类语义问题合并成一个小阶段处理，同类问题多修复一些再合并commit。

## 背景

第 91 步抽查 rewritten 函数时发现 multi-return 函数能改成 `{ i64, i64 }` 返回，但真实 callsite 多数没有消费 struct result。本轮继续按 Bench2 结果做质量审计，确认多返回 callsite 当前是真实未使用，还是 rewrite 后少了 `extractvalue`。

本轮仍使用第 89 步后的完整 smoke：

```bash
/tmp/notdec-bin2llvm-bench2-multi-input-multi-return-partial-shared-smoke
```

之后没有生产代码改动。

## 候选大块任务

### 1. multi-return callsite 使用质量审计

- Ghidra 对应：
  - `fspec.cc`: `FuncCallSpecs::deriveOutputMap(...)`
  - `coreaction.cc`: `ActionActiveReturn::apply(...)`
- native 缺口：
  - 需要确认 `{ i64, i64 }` 返回值在真实 callsite 是否被消费。
  - 如果被消费，需要确认分量顺序和 `extractvalue` 位置合理。
- Bench2 影响：
  - 不改变 rewrite 数量，但影响第 6 阶段语义质量判断。
- 收敛标准：
  - 统计真实 struct-return 定义、direct call、`extractvalue { i64, i64 }`。
  - 对真实 callsite 样本判断返回结果是否被使用。

### 2. 空 prototype 原因分类 summary

- Ghidra 对应：
  - `fspec.cc`: `FuncCallSpecs::buildInputFromTrials(...)`、`FuncCallSpecs::buildOutputFromTrials(...)`
- native 缺口：
  - 第 92 步确认 `already matches` 都是空候选，但还没有自动分类原因。
- Bench2 影响：
  - 有助于后续判断是否继续做 candidate recovery。

### 3. declaration / 外部 call effect 边界审计

- Ghidra 对应：
  - `fspec.cc`: `FuncCallSpecs::hasEffect(...)`
- native 缺口：
  - declaration 不 rewrite，但外部 call effect 会影响候选恢复。

## 本轮选择

本轮选择 `multi-return callsite 使用质量审计`。原因是它直接对应第 91 步留下的语义风险：多返回函数已经被表达成 struct return，但真实调用方是否消费返回分量还不清楚。

## 统计结果

| target | `{ i64, i64 }` defs | `{ i64, i64 }` direct calls | `extractvalue { i64, i64 }` |
| --- | ---: | ---: | ---: |
| vsftpd | 7 | 0 | 0 |
| libuv | 23 | 0 | 0 |
| memcached | 21 | 3 | 0 |

说明：

- `extractvalue` 总体并不为 0，但都是 `{ i64, i1 }` 或 `{ i32, i1 }` 这类溢出 intrinsic 的结果，不是 prototype recovery 产生的 `{ i64, i64 }` 返回值。
- vsftpd/libuv 的 multi-return 函数目前没有 direct struct-return callsite。
- memcached 有 3 个 direct struct-return callsite，但没有一个消费 struct result 分量。

## 样本抽查

memcached `notdec_native_bc60` 有两个 direct callsite：

```llvm
%12 = call { i64, i64 } @notdec_native_bc60(i64 %unique_df00_8, i64 18)
...
%21 = call { i64, i64 } @notdec_native_bc60(i64 %unique_df00_8, i64 0)
```

两个 call 后都继续从内存和寄存器状态取值，没有 `extractvalue { i64, i64 }`。这说明当前调用方没有使用返回寄存器分量，rewrite 保留未使用 struct result 是合理的。

memcached `notdec_native_1d760` 有一个 direct callsite：

```llvm
%2 = call { i64, i64 } @notdec_native_1d760(i64 365248, i64 8192)
```

call 后立即继续写 `RSI/RDI` 并返回，也没有消费 struct result。

## 结论

当前 Bench2 三个目标里没有真实 `extractvalue { i64, i64 }` 使用。multi-return rewrite 已经把函数签名和 callsite 类型改出来，但真实调用方没有使用返回分量，所以看不到分量级 callsite 替换效果。

这不是当前代码错误，也不是新的 rewrite blocker。它说明第 6 阶段对 multi-return 的真实验证范围有限：

- 函数体里 `{ i64, i64 }` 构造和 metadata 顺序已经在第 91 步抽查过。
- callsite 侧只验证到“能生成 struct-return call 且未使用结果时 verifier 通过”。
- 还没有 Bench2 真实样本证明“调用方消费某个返回分量”这条路径。

后续如果要增强验证，应优先找真实样本或增加一组高质量 IR 测试，覆盖：

1. 调用方只消费第 0 个返回分量；
2. 调用方只消费第 1 个返回分量；
3. 调用方同时消费两个返回分量；
4. shared successor + PHI 中消费返回分量。

这些测试必须归到 multi-return callsite 使用质量这个大块任务里，不应再拆成无边界的 CFG 排列组合。
