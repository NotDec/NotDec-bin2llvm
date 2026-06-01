# 96. 空 prototype 原因分类审计

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass。首先将相关的数据结构划分为几个工作量大约相等的部分（目前应该差不多完成了，主要是在数据集上测试并进一步提升），其次，规划一下如何做测试和验证。把进度记录到PROGRESS.md里面。依然要求，
>
> 在数据集上测试并进一步提升时，因为不涉及实现具体的模块或数据结构，可以不需要遵守上面的功能复刻流程。
>
> 1. 每一轮先看 Bench2 当前 skip reason 和真实函数样本，再决定实现点。
> 2. 先按 Ghidra 数据结构和 Bench2 blocker 识别“大块能力”，再从大块能力里切小步。小步服务于大块任务，不允许小步自己变成主线。
> 3. 每次实现不应以“一个极小 CFG 变体”作为默认粒度。应把同一类语义问题合并成一个小阶段处理，同类问题多修复一些再合并commit。

## 背景

第 92 步已经确认 `already matches` 都是零 input/return candidate 的空 recovered prototype。本轮继续做原因分类，判断这些空 prototype 是否集中在合理形状，还是暴露出新的 candidate recovery blocker。

本轮仍使用第 89 步后的完整 smoke：

```bash
/tmp/notdec-bin2llvm-bench2-multi-input-multi-return-partial-shared-smoke
```

之后没有生产代码改动。

## 候选大块任务

### 1. 空 prototype 原因分类 summary

- Ghidra 对应：
  - `fspec.cc`: `FuncCallSpecs::buildInputFromTrials(...)`、`FuncCallSpecs::buildOutputFromTrials(...)`
  - `coreaction.cc`: `ActionPrototypeTypes::apply(...)`
- native 缺口：
  - `already matches` 虽然不是 rewrite 漏处理，但还需要知道空 prototype 主要是什么原因。
- Bench2 影响：
  - 如果发现大量具名函数因 candidate recovery 过保守而空掉，后续应回到 `ParamActive` / `ParamTrial`。
- 收敛标准：
  - 按真实 IR 形状给空 prototype 分类。
  - 分类结果没有指向一类明确误判 blocker。

### 2. declaration effect 精细化

- Ghidra 对应：
  - `FuncCallSpecs::hasEffect(...)`
- native 缺口：
  - 第 94 步确认 declaration 是合理 skip，但外部函数 effect 仍然比较粗。

### 3. multi-return 真实消费样本补充

- Ghidra 对应：
  - `FuncCallSpecs::deriveOutputMap(...)`
- native 缺口：
  - 第 93 步确认当前 Bench2 没有真实 `extractvalue { i64, i64 }` 消费样本。

## 本轮选择

本轮选择 `空 prototype 原因分类 summary`。原因是它是 `already matches` 审计的自然下一步，能判断是否需要继续投入候选恢复逻辑。

## 分类方法

这是启发式分类，不写入代码：

1. 从 `signature-rewrite.native-llvm.stderr` 取 `rewritten=0 reason=already matches` 的函数。
2. 查对应 `.ll` 函数体和 summary 行。
3. 按优先级分类：
   - `init_thunk`: 调用 `__gmon_start__`。
   - `abort_or_fail_stub`: 调用 `abort` 或 `__stack_chk_fail`。
   - `internal_wrapper_or_thunk`: 调用 module 内 native/具名定义函数。
   - `external_call_wrapper`: 调用外部导入函数。
   - `preserved_or_frame_only`: 无真实 call，主要是 preserved register / 栈框架状态。
   - `stack_or_leaf_no_call`: 只剩栈状态、leaf 或无候选短函数。

## 分类结果

| target | total | preserved/frame only | abort/fail stub | external wrapper | internal wrapper/thunk | stack/leaf no call | init thunk |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| vsftpd | 48 | 24 | 0 | 5 | 11 | 7 | 1 |
| libuv | 147 | 51 | 49 | 27 | 2 | 17 | 1 |
| memcached | 71 | 47 | 5 | 7 | 3 | 8 | 1 |

样本：

- vsftpd
  - preserved/frame only: `notdec_native_9110`、`notdec_native_94a0`
  - internal wrapper/thunk: `notdec_native_6720`、`notdec_native_8300`
  - external wrapper: `notdec_native_5020`、`notdec_native_5ba0`
  - init thunk: `notdec_native_5000`
- libuv
  - preserved/frame only: `uv_inet_pton`、`uv_udp_connect`
  - abort/fail stub: `notdec_native_9b40`、`notdec_native_9b4d`
  - external wrapper: `uv_barrier_destroy`、`uv_chdir`
  - stack/leaf no call: `uv_os_getpid`、`uv_os_getppid`
- memcached
  - preserved/frame only: `notdec_native_be50`、`notdec_native_c9a0`
  - external wrapper: `notdec_native_6711`、`notdec_native_6978`
  - internal wrapper/thunk: `notdec_native_66f0`、`notdec_native_b9c0`
  - abort/fail stub: `notdec_native_6740`、`notdec_native_684a`

## 结论

空 prototype 主要集中在合理形状：

- preserved register / frame-only 噪声最多；
- libuv 有大量 abort/fail stub；
- wrapper/thunk 多数是自己没有 ABI input/output，只是在内部写寄存器后调用别的函数；
- init thunk 每个目标各 1 个。

当前没有发现一类明确的 candidate recovery 误判 blocker。尤其是第 92 步已经确认这些函数没有非零 input/return candidate，所以这不是 signature rewrite 的漏处理。

风险：

- 这个分类是启发式审计，不是 pass 输出的一部分。
- 少数具名 `uv_*` 空 prototype 仍值得未来在更大样本里继续抽查，但本轮没有证据说明需要改代码。

后续建议：

1. 不继续围绕 `already matches` 追加实现。
2. 如果要把这个分类产品化，再考虑在 summary 里输出空 prototype 原因，但要先证明它能帮助定位新的 Bench2 blocker。
3. 下一轮更适合做全目标完成度审计，确认哪些阶段只是当前 Bench2 收敛，哪些还不能标成长期完成。
