# 91. Bench2 rewritten 函数语义抽查

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass。首先将相关的数据结构划分为几个工作量大约相等的部分（目前应该差不多完成了，主要是在数据集上测试并进一步提升），其次，规划一下如何做测试和验证。把进度记录到PROGRESS.md里面。
>
> 在数据集上测试并进一步提升时，因为不涉及实现具体的模块或数据结构，可以不需要遵守上面的功能复刻流程。
>
> 1. 每一轮先看 Bench2 当前 skip reason 和真实函数样本，再决定实现点。
> 2. 先按 Ghidra 数据结构和 Bench2 blocker 识别“大块能力”，再从大块能力里切小步。小步服务于大块任务，不允许小步自己变成主线。
> 3. 每次实现不应以“一个极小 CFG 变体”作为默认粒度。应把同一类语义问题合并成一个小阶段处理，同类问题多修复一些再合并commit。

## 背景

第 90 步确认 Bench2 signature rewrite 的非合理 blocker 已经清零。本轮按 `GOAL.md` 的阶段停止标准，抽查 rewritten 函数的语义质量：签名、metadata、函数体参数替换、返回值和 callsite 是否一致。

本轮仍使用第 89 步后的完整 smoke：

```bash
/tmp/notdec-bin2llvm-bench2-multi-input-multi-return-partial-shared-smoke
```

之后只改过文档，没有改生产代码。

## 抽查范围

覆盖三类目标和四类签名形状：

- vsftpd
  - input+return：`notdec_native_17960`
  - input-only：`notdec_native_8410`
- libuv
  - return-only：`uv_library_shutdown`
  - input+return：`notdec_native_9e70`
  - input+multi-return：`notdec_native_9e50`
- memcached
  - input+multi-return：`notdec_native_bc60`
  - return-only：`notdec_native_f3f0`

## 样本结论

### vsftpd

`notdec_native_17960`：

- 签名：`i64 @notdec_native_17960(i64 %RDI.external_input1)`。
- metadata：`input_count=1`、`return_count=1`，input 为 `RDI`，return 为 `RAX`。
- 函数体中原入口 `RDI` 值已使用参数 `%RDI.external_input1`。
- 返回值直接 `ret i64 0`，没有旧的 `RAX` return load 参与 callsite。
- callsite 示例：`%15 = call i64 @notdec_native_17960(i64 0)`。

`notdec_native_8410`：

- 签名：`void @notdec_native_8410(i64 %RDI.external_input1)`。
- metadata：`input_count=1`、`return_count=0`，input 为 `RDI`。
- 函数体中 `RDI` 参数写入 `RBX`，符合 input-only rewrite。
- callsite 示例：`call void @notdec_native_8410(i64 130842)`。

### libuv

`uv_library_shutdown`：

- 签名：`i64 @uv_library_shutdown()`。
- metadata：`input_count=0`、`return_count=1`，return 为 `RAX`。
- 函数体两个 return path 都返回 `i64 1`，原 `RAX` return store 已转成 LLVM return。

`notdec_native_9e70`：

- 签名：`i64 @notdec_native_9e70(i64 %RSI.external_input1, i64 %RDX.external_input2)`。
- metadata：`input_count=2`、`return_count=1`，input 为 `RSI/RDX`，return 为 `RAX`。
- 函数体使用 `RSI` 作为源地址、`RDX` 作为目标地址，最后返回从 `RDX` 指向内存读取的值。
- callsite 示例：
  - `%216 = call i64 @notdec_native_9e70(i64 %unique_df00_8198, i64 %RCX308)`
  - `%254 = call i64 @notdec_native_9e70(i64 %RCX357, i64 %RDX.regssa)`
- 第二个 callsite 证明 RegisterSSA PHI 值能作为参数传递。

`notdec_native_9e50`：

- 签名：`{ i64, i64 } @notdec_native_9e50(i64 %RDI.external_input1)`。
- metadata：`input_count=1`、`return_count=2`，input 为 `RDI`，return 为 `RAX/RDX`。
- 函数体用两个 `insertvalue` 构造 `{ i64, i64 }`，分量顺序和 metadata 一致。

### memcached

`notdec_native_bc60`：

- 签名：`{ i64, i64 } @notdec_native_bc60(i64 %RDI.external_input1, i64 %RSI.external_input2)`。
- metadata：`input_count=2`、`return_count=2`，input 为 `RDI/RSI`，return 为 `RAX/RDX`。
- 函数体在两个 return path 都构造 `{ 1, value }`。
- callsite 示例：
  - `%12 = call { i64, i64 } @notdec_native_bc60(i64 %unique_df00_8, i64 18)`
  - `%21 = call { i64, i64 } @notdec_native_bc60(i64 %unique_df00_8, i64 0)`
- 当前样本里 struct result 未使用，符合“未使用返回分量不强行 extract”的策略。

`notdec_native_f3f0`：

- 签名：`i64 @notdec_native_f3f0()`。
- metadata：`input_count=0`、`return_count=1`，return 为 `RAX`。
- 函数体返回从 `FS_OFFSET + 40` 读取的值。
- callsite 示例：`%2 = call i64 @notdec_native_f3f0()`。

## 额外检查

- 三个目标的 signature rewrite `.ll` 都没有 `.old` 函数定义残留。
- 三个目标的 signature rewrite `.ll` 都没有对 `.old` 函数的调用。
- 三个目标的 signature rewrite `llvm-as` / `opt` stderr 为空。
- 三个目标的 all-confirmed `llvm-as` / `opt` stderr 为空。

类型形状计数：

| target | i64 return defs | void with i64 params defs | struct return defs |
| --- | ---: | ---: | ---: |
| vsftpd | 42 | 90 | 7 |
| libuv | 111 | 204 | 23 |
| memcached | 52 | 115 | 21 |

## 风险和后续任务

没有发现明显 rewrite 语义错误，但仍有两个后续审计方向：

1. `already matches` / 空 prototype 质量审计。
   - 当前很多 `void()` native 函数带空 recovered prototype，应该确认是真无候选，还是 candidate 筛选过保守。
2. multi-return callsite 使用质量审计。
   - 当前抽到的真实 multi-return callsite 多数未使用 struct result。
   - 后续可以专门找真实 `extractvalue` 或确认为什么 Bench2 中多返回分量常未被使用。

## 结论

当前 direct call signature rewrite 在抽查样本中表现合理：

- recovered metadata 和 LLVM 签名一致；
- input 参数顺序符合 ABI slot；
- return-only 和 input+return 都把 register return 改成 LLVM return；
- multi-return 用 `{ i64, i64 }` 和 `insertvalue` 表达；
- callsite 参数来源能对应 rewrite 前寄存器值；
- 没有 `.old` 残留调用。

下一步不应回到零散 CFG 测试，建议做 `already matches` / 空 prototype 质量审计。
