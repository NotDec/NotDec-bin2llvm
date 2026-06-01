# 20260601-123 seed700 new rewritten semantics audit

## 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass。首先将相关的数据结构划分为几个工作量大约相等的部分（目前应该差不多完成了，主要是在数据集上测试并进一步提升），其次，规划一下如何做测试和验证。把进度记录到PROGRESS.md里面。依然要求，

根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

在数据集上测试并进一步提升时，因为不涉及实现具体的模块或数据结构，可以不需要遵守上面的功能复刻流程。

1. 每一轮先看 Bench2 当前 skip reason 和真实函数样本，再决定实现点。
2. 先按 Ghidra 数据结构和 Bench2 blocker 识别“大块能力”，再从大块能力里切小步。小步服务于大块任务，不允许小步自己变成主线。
3. 每次实现不应以“一个极小 CFG 变体”作为默认粒度。应把同一类语义问题合并成一个小阶段处理，同类问题多修复一些再合并commit。
```

## 背景

第 122 步把四个关键压力目标从 seed500 加深到 seed700，并且 gate 全部通过。本轮不继续只看数量，而是对 seed700 相比 seed500 新增的 rewritten 函数做语义抽查。

这是数据集审计，不新增 Ghidra 数据结构复刻代码。

## 候选大块任务

1. seed700 新增 rewritten 函数质量抽查：本轮处理。
2. 新的 signature rewrite blocker：seed700 没有暴露非合理 skip reason，暂不进入功能复刻流程。
3. 多返回第 1 分量消费样本：继续观察，本轮新增样本仍没有出现。

## 新增 rewritten 统计

| target | seed500 rewritten | seed700 rewritten | 新增 rewritten |
| --- | ---: | ---: | ---: |
| `python:interpreter` | 400 | 558 | 158 |
| `ffmpeg:codec-library` | 445 | 623 | 178 |
| `redis:server-symlink` | 409 | 573 | 164 |
| `wolfssl:shared-library` | 375 | 533 | 158 |

## 函数签名抽查

| target | function | summary | rewritten signature | 判断 |
| --- | --- | --- | --- | --- |
| Python | `_PyErr_BadInternalCall` | 2 input, 2 return | `{ i64, i64 } @_PyErr_BadInternalCall(i64, i64)` | 多返回 struct 化一致 |
| Python | `PyUnstable_Eval_RequestCodeExtraIndex` | 0 input, 2 return | `{ i64, i64 } @PyUnstable_Eval_RequestCodeExtraIndex()` | return-only 多返回一致 |
| Python | `Py_AddPendingCall` | 2 input, 1 return | `i64 @Py_AddPendingCall(i64, i64)` | 参数和返回数量一致 |
| Python | `_Py_hashtable_new` | 0 input, 1 return | `i64 @_Py_hashtable_new()` | return-only 一致 |
| Python | `Py_IsNone` | 1 input, 1 return | `i64 @Py_IsNone(i64)` | input+return 一致 |
| Redis | `dictAddRaw` | 2 input, 0 return | `void @dictAddRaw(i64, i64)` | input-only 一致 |
| Redis | `RM_ReplyWithMap` | 0 input, 1 return | `i64 @RM_ReplyWithMap()` | return-only 一致 |
| Redis | `moduleZsetAddFlagsFromCoreFlags` | 1 input, 2 return | `{ i64, i64 } @moduleZsetAddFlagsFromCoreFlags(i64)` | 多返回 struct 化一致 |
| Redis | `raxFindParentLink` | 1 input, 2 return | `{ i64, i64 } @raxFindParentLink(i64)` | 多返回 struct 化一致 |
| Redis | `lua_pushlightuserdata` | 2 input, 1 return | `i64 @lua_pushlightuserdata(i64, i64)` | 参数和返回数量一致 |
| wolfSSL | `wc_Sha3_224_GetHash` | 0 input, 1 return | `i64 @wc_Sha3_224_GetHash()` | return-only 一致 |
| wolfSSL | `wc_Sha3_512_Update` | 3 input, 0 return | `void @wc_Sha3_512_Update(i64, i64, i64)` | 多 input 无返回一致 |
| wolfSSL | `wc_RsaPrivateKeyValidate` | 2 input, 1 return | `i64 @wc_RsaPrivateKeyValidate(i64, i64)` | 参数和返回数量一致 |
| wolfSSL | `wc_Sha384SetFlags` | 2 input, 1 return | `i64 @wc_Sha384SetFlags(i64, i64)` | 参数和返回数量一致 |
| wolfSSL | `wolfSSL_ERR_peek_error` | 0 input, 1 return | `i64 @wolfSSL_ERR_peek_error()` | return-only 一致 |

## callsite 抽查

Python seed700 新增 `_PyErr_BadInternalCall` 后，出现真实多返回 direct callsite：

```llvm
%13 = call { i64, i64 } @_PyErr_BadInternalCall(i64 7356369, i64 55)
ret void
```

判断：

- 参数已经从原来的 `RDI` / `RSI` register store 变成 call 参数。
- 返回值未消费，因此没有生成 `extractvalue`。
- 这和第 121 步结论一致：真实数据集仍没有多返回第 1 分量消费样本。

Python seed700 仍保留之前已验证的第 0 分量消费：

```llvm
%4 = call { i64, i64 } @PyErr_GetRaisedException()
%5 = extractvalue { i64, i64 } %4, 0
```

## 判断

- seed700 新增 rewritten 函数覆盖了更多真实项目和更多签名形状。
- 抽查样本中 summary 的 input / return candidate 数量和 rewritten LLVM 签名一致。
- 多返回函数定义和 direct callsite 可以继续通过 LLVM 22 verify。
- 新增样本没有暴露 `unsafe callsite input value`、`unsafe callsite return load` 或返回类型不一致。
- 多返回第 1 分量消费仍未出现，继续作为真实样本覆盖缺口记录。

## 后续

- 可以继续把关键目标加深到 seed1000。
- 也可以转向 seed700 新增函数的空 prototype / `already matches` 质量抽查。
- 如果后续出现第 1 分量真实消费样本，再回到 multi-return callsite 使用质量任务。
