# 92. Bench2 空 prototype 质量审计

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass。首先将相关的数据结构划分为几个工作量大约相等的部分（目前应该差不多完成了，主要是在数据集上测试并进一步提升），其次，规划一下如何做测试和验证。把进度记录到PROGRESS.md里面。依然要求，
>
> 在数据集上测试并进一步提升时，因为不涉及实现具体的模块或数据结构，可以不需要遵守上面的功能复刻流程。
>
> 1. 每一轮先看 Bench2 当前 skip reason 和真实函数样本，再决定实现点。
> 2. 先按 Ghidra 数据结构和 Bench2 blocker 识别“大块能力”，再从大块能力里切小步。小步服务于大块任务，不允许小步自己变成主线。
> 3. 每次实现不应以“一个极小 CFG 变体”作为默认粒度。应把同一类语义问题合并成一个小阶段处理，同类问题多修复一些再合并commit。

## 背景

第 90 步确认 Bench2 signature rewrite 的非合理 skip reason 已清零，第 91 步抽查 rewritten 函数语义合理。本轮继续看剩余 `already matches`，确认它们是合理的空 recovered prototype，还是候选恢复过保守导致的漏改。

本轮仍使用第 89 步后的完整 smoke：

```bash
/tmp/notdec-bin2llvm-bench2-multi-input-multi-return-partial-shared-smoke
```

之后只改过文档和审计记录，没有改生产代码。

## 候选大块任务

### 1. already matches / 空 prototype 质量审计

- Ghidra 对应：
  - `fspec.cc`: `FuncCallSpecs::buildInputFromTrials(...)`、`FuncCallSpecs::buildOutputFromTrials(...)`
  - `coreaction.cc`: `ActionPrototypeTypes::apply(...)`
- native 缺口：
  - 剩余 skipped 里 `already matches` 数量较多。
  - 需要确认这些函数是否真无 ABI 参数/返回候选。
- Bench2 影响：
  - 如果发现误空 prototype，后续会回到 `ParamActive` / `ParamTrial` 侧补候选筛选。
- 收敛标准：
  - 所有 `already matches` 都能对应到空候选，样本没有明显 ABI input/output 漏标。

### 2. multi-return callsite 使用质量审计

- Ghidra 对应：
  - `fspec.cc`: `FuncCallSpecs::deriveOutputMap(...)`
  - `coreaction.cc`: `ActionActiveReturn::apply(...)`
- native 缺口：
  - 第 91 步抽到的真实 multi-return callsite 多数没有使用 struct result。
  - 后续需要确认真实 `extractvalue` 使用是否稳定，或者为什么很多返回分量被丢弃。
- Bench2 影响：
  - 不一定改变 rewrite 数量，但影响多返回语义质量判断。

### 3. declaration / 外部 call effect 边界审计

- Ghidra 对应：
  - `fspec.cc`: `FuncCallSpecs::hasEffect(...)`
- native 缺口：
  - `declaration` 本身不应 rewrite，但外部函数副作用会影响 register SSA 和候选恢复。
- Bench2 影响：
  - 可能影响后续候选精度，不直接减少 declaration skip。

## 本轮选择

本轮选择 `already matches / 空 prototype 质量审计`。原因是它直接对应第 90 步剩余最多的 skip reason，也能判断第 6 阶段是否还需要继续实现候选恢复逻辑。

## 统计结果

空 recovered prototype metadata：

| target | metadata |
| --- | --- |
| vsftpd | `!89 = !{!"model=__stdcall", !"input_count=0", !"return_count=0", !90, !90}` |
| libuv | `!91 = !{!"model=__stdcall", !"input_count=0", !"return_count=0", !92, !92}` |
| memcached | `!94 = !{!"model=__stdcall", !"input_count=0", !"return_count=0", !95, !95}` |

`already matches` 数量和候选情况：

| target | already matches | nonzero input candidates | nonzero return candidates |
| --- | ---: | ---: | ---: |
| vsftpd | 48 | 0 | 0 |
| libuv | 147 | 0 | 0 |
| memcached | 71 | 0 | 0 |

结论：所有 `already matches` 都是 `input_candidates=0 return_candidates=0` 的空 recovered prototype。没有发现“已有候选但 signature rewrite 没处理”的情况。

## 样本抽查

### vsftpd

`notdec_native_5000`：

- `external_inputs=1 input_candidates=0 return_candidates=0`。
- 只读取 `RSP.external_input`，做 init thunk 形状的栈调整和可选 `__gmon_start__` 调用。
- `RAX` 来自 GOT/内存读取后只用于条件，不形成本函数 ABI 返回。

`notdec_native_6720`：

- `external_inputs=1 input_candidates=0 return_candidates=0`。
- 写 `RDI=130842` 后调用已改写的 `notdec_native_8410(i64 130842)`。
- 本函数自己的入口寄存器没有作为 ABI input 使用。

`notdec_native_9110`：

- `external_inputs=5 input_candidates=0 return_candidates=0`。
- 入口 `R12/R13/R14/RSP/RBP` 主要用于保存、栈框架和 preserved register 相关流动。
- 当前不把这些 preserved/栈状态当作函数参数是合理的。

### libuv

`notdec_native_8000`：

- `external_inputs=1 input_candidates=0 return_candidates=0`。
- 和 vsftpd 的 init thunk 类似，主要是 `RSP` 栈调整和可选 `__gmon_start__`。

`notdec_native_9b40`：

- `external_inputs=2 input_candidates=0 return_candidates=0`。
- 栈框架后直接调用 `abort()`，没有真实 ABI 参数和返回值。

`uv_inet_pton`：

- `external_inputs=7 input_candidates=0 return_candidates=0`。
- 当前样本只看到 preserved register / frame 保存形状。
- 因为它是具名 `uv_*` 函数，后续如果继续做候选质量，可以单独扩大抽样；本轮没有证据说明 rewrite 阶段漏处理。

### memcached

`notdec_native_5000`：

- `external_inputs=1 input_candidates=0 return_candidates=0`。
- init thunk 形状，只用入口 `RSP` 做栈调整。

`notdec_native_66f0`：

- `external_inputs=2 input_candidates=0 return_candidates=0`。
- 先调用 `perror()`，后续根据内部加载值调用已改写的 `notdec_native_bc60(...)`。
- 入口 `RBX/RSP` 是控制状态和栈状态，不是 ABI input。

`notdec_native_6740`：

- `external_inputs=4 input_candidates=0 return_candidates=0`。
- 调用 `perror()`、`bind()`、`__errno_location()`，入口外部值主要是 preserved/control state。
- 本轮没有看到应改成显式参数或返回值的证据。

## 结论

`already matches` 不是 signature rewrite 的剩余 blocker。它们目前都对应空 recovered prototype，也就是函数类型本来就是 `void()`，不需要改签名。

样本里的空 prototype 大多能解释为：

- init thunk；
- abort stub；
- wrapper/control-flow 函数；
- preserved register 或栈状态噪声；
- 内部调用已改写函数，但自己没有 ABI input/output。

风险：

- libuv 里有一批具名 `uv_*` 函数也是空 prototype。本轮抽样没有证明它们有问题，但它们比 `notdec_native_*` 更值得后续扩大抽查。
- 如果后续要继续提升，不应直接补 CFG 变体，而应做“空 prototype 原因分类报告”，把 init/thunk/abort/wrapper/preserved-only/no ABI use 拆开统计。

下一步建议：

1. 做 multi-return callsite 使用质量审计，确认真实 `extractvalue` 使用情况。
2. 或者增加空 prototype 原因分类 summary，便于后续判断是否还要投入 candidate recovery。
