# native summary SSA 阶段性能：tmux/ssh 跑不出 IR 的原因分析

## 0. 用户原始 prompt

> 对 在研究仔细分析一下下一个问题的原因吧

承接交接 `logs/20260923-01-native-handover.md` 的 P3：tmux/ssh（以及 wolfssl）在 summary 阶段
`timeout 1500` 内不产出 IR。交接里的判断是"base summary 的 `Analyzer::run` 占 90% 时间、
疑似 `solveFunction` 轮转定点超线性"，要求先量化再决定。本轮就是量化。

本轮只做测量和分析，没有改产品代码；临时插桩已回退（patch 存在
`/tmp/perf_instrumentation.patch`），工作区干净（HEAD `a830244`）。产物在
`/tmp/perf1`..`/tmp/perf4`、`/tmp/lighttpd.perf`。

## 1. 结论摘要

1. **瓶颈不在 base summary**。实测两遍 base summary（self-bottom-up + demand）合计：
   lighttpd 41s、ssh 155s、tmux 231s；而 lighttpd 整条 SSA pass 里光是逐函数重写循环
   就 **682s**（占该次运行 ~80%）。交接里"90% 在 base summary"的 perf 结论与当前代码不符
   （应基于更早 revision，或 profiler 归因错了）。
2. **真正的热点是 SSA pass 的逐函数重写循环（`FunctionBuilder::run`）**，其中两个
   **与规模成平方关系**的操作吃掉绝大部分时间：
   - **A. 死代码消除里的"记录值"查询**：`isRecordedCallArgValue` / `isRecordedCallArgStore` /
     `isRecordedFunctionReturnValue` 对每个候选值线性扫**模块级** `SignatureState.CallArgs`，
     每行还要 `resolve()`。CallArgs 随模块推进增长（lighttpd 首个函数 406 条 → 末尾 3720 条；
     tmux 处理到 400 号函数时已 4654 条），所以越到后面每个函数越慢。
     perf（200s 采样）：`isRecordedCallArgValue` 占 **37.9%**（其中 `resolve` 33.0%，
     背后是 `std::map<Value*,Value*>` 查找 30.8%）。
   - **B. `rewritePartialWrites` 的 per-store 值图递归**：每个寄存器 store 都新建一个
     `visiting` 集合、把该值的操作数 DAG 重走一遍；DAG 是共享的，等于 O(store 数 × DAG)。
     perf：`rewritePartialWrites` 占 **24.5%**；步内计时：store 循环 ~8ms/store
     （burl_normalize 2031 个 store 花 17.1s，compute-demands 只花 20ms）。
3. 次要：`removeDeadStoresByLiveness`（每函数反向 liveness 定点，~5%）、`rewrite-loads`（~4%）。
4. 重写后的清理循环（最多 10 轮全模块、每轮完整重写管线 + InstCombine）在 lighttpd 上
   **2 轮就收敛**（8.2s + 3.0s），不是主因；但如果某目标的 dead store 每轮都剩一点，
   它会把 A/B 的逐函数成本再乘轮数（tmux/ssh 的轮数本轮没测到，因为函数循环没跑完）。
5. 规模效应：perf4 里 tmux 20 分钟处理到 401/1280 个函数、ssh 283/699，按这个速率
   tmux 光逐函数循环就要 ~60min —— 与"1 小时没 IR"吻合。也就是说它**不是死循环，
   是慢**（CPU 一直 95%+，无输出）。

## 2. 测量方法

临时 getenv 门控插桩（改完已 `git checkout` 回退）：

- base summary（`NativeRegisterSummary.cpp`）：`solveFunction` 的 solves / 内部轮数 /
  `transferBlock` 次数 / 耗时；SCC 组大小与轮数。
- SSA（`NativeRegisterSummarySSA.cpp`）：各阶段耗时（`ssa-stage`）、每函数耗时
  （`function-enter` / `function-slow`）、`FunctionBuilder::run` 每步耗时（`ssa-steps`）、
  `rewritePartialWrites` 的 compute-demands / store-loop 拆分（`ssa-pw`）、
  `isRecordedCallArgValue` 的调用次数与被扫行数。
- 命令统一为 `build/bin/notdec-native-llvm <bin> --all-confirmed --skip-runtime
  --register-ssa-summary`，LLVM 22 工具验证。
- perf：`perf record -q -g -o /tmp/lighttpd.perf -- timeout 200 <同上 lighttpd>`
  （`perf_event_paranoid=0`，200s 采样 799K samples）。

## 3. 证据

### 3.1 lighttpd 阶段分解（perf4，插桩版）

| 阶段 | 耗时 |
|---|---|
| truncate-noreturn | 0.2s |
| stack-frame-rewrite | 1.9s |
| canary / peephole / x87 / vararg | 0.02 / 0.12 / 0.11 / 0.13s |
| base-summary-1（self-bottom-up） | 15.8s |
| infer-external-call-shapes | 0.13s |
| base-summary-2（demand） | 25.3s |
| **function-loop（722 个函数）** | **682.5s** |
| 签名精化 + markSignatureCallArgStores | ~1.2s |
| rewrite-signature-shapes | 1.9s |
| cleanup-iteration 0 / 1 | 8.2s（删 2613 条死 store）/ 3.0s（删 0，收敛） |

### 3.2 lighttpd 函数内分解（266 个 >500ms 的函数，sum(total)=622s）

| 步骤 | 合计 |
|---|---|
| rewrite-partial-writes | 346.6s（56%） |
| dead-partial-reads | 119.6s（19%） |
| dead-replaced-loads | 108.6s（17%） |
| dead-stores-liveness | 31.1s（5%） |
| rewrite-loads | 5.3s（0.9%） |
| call-args-reg / plan-ranges | 3.4s / 1.6s |
| `isRecordedCallArgValue` 扫描行数 | **7.79 亿行** |

`ssa-pw` 拆分（2166 次 `rewritePartialWrites` 调用）：pw-store-loop 合计 **378.5s**，
pw-compute-demands 合计只有 **1.78s**（平均 0.8ms/次）→ **热点在 store 循环，不在算需求**。

### 3.3 单函数样本（都是"不大但极慢"）

| 目标 | 函数 | blocks | total | pw-store-loop | callarg-scans | callarg-rows | 当时的 callargs |
|---|---|---|---|---|---|---|---|
| lighttpd | burl_normalize | 238 | 34.4s | 17.1s | 3712 | 3086 万 | 3038 |
| lighttpd | http_response_parse_headers | 271 | 27.8s | 16.3s | 3166 | 2952 万 | 3379 |
| lighttpd | http_date_if_modified_since | 127 | 23.6s | 13.3s | 2324 | 2320 万 | 3609 |
| ssh | FUN_17a60 | 651 | **100.0s** | 52.5s | 10900 | 7492 万 | 2410 |
| ssh | FUN_1ae20 | 246 | 47.9s | 28.6s | 4221 | 4293 万 | 3564 |
| tmux | FUN_892c0 | 243 | 33.7s | 18.6s | 3294 | 3481 万 | 3896 |
| tmux | FUN_3fac0 | 255 | 27.8s | — | 2255 | 2849 万 | 4654 |

一个函数的总耗时和它的 blocks/指令数只有弱相关（`http_date_if_modified_since` 127 blocks
就要 23.6s），和 store 数、callarg 扫描量强相关。

### 3.4 模块级增长（平方项的来源）

`SignatureState.CallArgs` 是**整个模块**的 map（`NativeRegisterSummarySSA.cpp:339`），
在逐函数循环里由 `collectSignatureCallArgs`（6697）不断追加。`isRecordedCallArgValue`（5341）
对**每个候选值**遍历整张表并对每行 `resolve()`：

```cpp
bool isRecordedCallArgValue(llvm::Value *value) const {
  value = resolve(value);
  for (const auto &[call, bindings] : SignatureState.CallArgs) {   // 模块级
    for (const CallArgStoreBinding &binding : bindings) {          // 每行两次 resolve
      if (resolve(binding.Value) == value || resolve(binding.RegisterValue) == value) return true;
    }
  }
  return false;
}
```

实测：lighttpd 第一个被打印的函数扫描时 CallArgs=406 行源、末尾 3720；tmux 已到 4654。
所以"每个候选值的检查成本"随模块推进线性增长 → 全模块总成本 O(模块²)。
`isRecordedCallArgStore`（5326）和 `isRecordedFunctionReturnValue`（5355）是同样的形状。

### 3.5 perf 归因（lighttpd，非插桩版，200s 采样）

| 符号（children） | 占比 |
|---|---|
| FunctionBuilder::run | 47.1% |
| **FunctionBuilder::isRecordedCallArgValue** | **37.9%** |
| FunctionBuilder::resolve | 33.0% |
| `std::map<Value*,Value*>` 查找（Replacement） | 30.8% |
| rewritePartialWrites | 24.5% |
| Analyzer::run（base summary 两遍） | 24.3%（其中 runBottomUp 15.4%） |

`resolve()` 背后是 `std::map<llvm::Value*,llvm::Value*>`（Replacement）的链式查找：
7.79 亿行 × 每行 2 次 resolve ⇒ 十亿级 map 查找，这正是 self 时间最大的项。

### 3.6 运行时长

- lighttpd（perf4，带插桩）：约 14 分钟（其中 function-loop 682s）。
- tmux/ssh（perf4）：20 分钟时分别走到 401/1280、283/699 个函数，仍在 function-loop；
  外推 tmux 单 function-loop ~60 分钟。与交接的"1h 无 IR"一致。
- 进程 CPU 一直 95%+，内存 1-1.2GB，不是死锁/OOM。

## 4. 根因

1. **`isRecordedCallArgValue` 每次调用扫全模块 CallArgs**（5341/5326/5355）。
   调用点在 `removeDeadPartialReads`（5139）、`removeDeadReplacedLoads`（5309），
   两者对"每个被替换/部分读候选"都查一遍；候选数随函数大小增长，表随模块增长 ⇒ 平方。
2. **`rewriteUndemandedRegisterStoreOperands`（3920）每个 store 重走一遍操作数 DAG**：
   `visiting` 集合在 `rewritePartialWrites`（4425/4443）每次 store 新建，跨 store 不共享；
   `demands` 在同一次 `rewritePartialWrites` 里是常量，因此同一个 DAG 被重复走
   O(store 数) 次 ⇒ 平方。
3. 另有 `removeDeadStoresByLiveness`（5330）每函数反向 liveness 定点（次要，~5%）。
4. 清理循环（9330，最多 10 轮全模块）会把上述逐函数成本乘轮数；本轮 lighttpd 收敛在
   2 轮，但 tmux/ssh 未测到（函数循环没跑完）——修完 1/2 之后要重测轮数。

## 5. 建议的修法（待确认后再动）

按性价比排序，两处都只动 SSA pass 内部，不改判据语义：

1. **把"记录值"查询做成 O(1)**：在 `FunctionBuilder` 里维护一张"已记录值"集合
   （`llvm::DenseSet<llvm::Value*>`，值都先 `resolve()` 一次），内容 = 所有 CallArgs 的
   `Value`/`RegisterValue` + 本函数的 FunctionReturns 值；`isRecordedCallArgStore` 用
   单独的 store 集合。集合可在 `collectSignatureCallArgs`/`collectFunctionReturnValues`
   写入时增量维护（或每次 `run()`/`removeDeadStoresAfterSignatureRewrite()` 开始时重建一次，
   仍是 O(模块) 一次而不是 O(候选 × 模块)）。
   预期：干掉 perf 里那 37.9% 和十亿级 map 查找。
2. **把 per-store DAG 重走改成一次遍历**：`rewritePartialWrites` 里共享一个
   `std::map<llvm::Value*, bool>`（值 → 处理结果）作为 memo，配一个防环 visiting；
   `demands` 在一次调用内不变、poison 替换幂等，所以结果可复用。
   预期：store 循环 378s → 几十秒量级。
3. 修完 1/2 再量测：如果 tmux/ssh 的 cleanup 轮数 >2，再考虑"只对本轮有变化的函数重跑
   清理"（记录每个函数上轮是否删过 dead store / 是否被调用者影响），而不是全模块重跑。
4. 明确不做：重写 base summary 的 worklist（不是瓶颈）、直接砍 `maxPostRewriteCleanupIterations`
   （没证据）、动 `removeDeadStoresByLiveness`（收益小、风险大）。

## 6. 判断标准

- 语义不变：7 目标 `llvm-as` + `opt -passes=verify` OK；`notdec.unknown` 计数不升
  （当前 lighttpd 26 / redis-cli 7 / libuv 8 / ffmpeg 5 / 其余 0）；`ctest` 12/12；
  fortune x86_64/i386 回归通过；函数签名集合与改前一致（用 `/tmp/sig_diff.py` 对比）。
- lighttpd function-loop 从 682s 降到 <200s；tmux 能在 `timeout 1500` 内产出 IR
  且 verify OK（这是 P3 的原始目标）。
- 量测表（时间/unknown/warnings/verify/IR bytes）和实现记录追加到本文件。

## 7. 不做什么

- 不为了性能放宽或跳过"记录值"检查（那会改语义，可能删掉还要用的值）。
- 不引入跨 pass 的全局缓存（失效时机不可控）。
- 不顺手重构 `FunctionBuilder` 的其他步骤。

## 8. 实施记录（2026-09-24）

### 8.1 改动

`lib/passes/summary/NativeRegisterSummarySSA.cpp`：

1. **记录值查询 O(1)**：`FunctionBuilder` 增加
   `RecordedCallArgValues` / `RecordedCallArgStores` / `RecordedFunctionReturnValues`
   三个 `DenseSet` + 脏标记；`ensureRecordedValueCache()` 把模块级
   `SignatureState.CallArgs` / 本函数 `FunctionReturns` 里的值放进集合，
   `isRecordedCallArgValue` / `isRecordedCallArgStore` /
   `isRecordedFunctionReturnValue` 改成一次哈希查询。
   失效点：`collectSignatureCallArgs` / `collectFunctionReturnValues`（表内容变化）
   和四处 `Replacement[...] = ...` 写入（链变长）；**擦除 `Replacement` 不失效**。
2. **一次遍历值图**：`rewriteUndemandedRegisterStoreOperands` 增加共享的
   `UndemandedWalkClean`（"这棵子树本次已处理完"），每次 `rewritePartialWrites`
   开始时清空；配合原有的 per-store `visiting` 防环。

### 8.2 第一版错在哪（记下来免得重走）

第一版把缓存做成"每次擦除 `Replacement` 也失效"，结果 `removeDeadPartialReads` /
`removeDeadReplacedLoads` 里每删一个候选就重建一次缓存（每次 O(模块行数)），
而原来的线性扫描遇到命中会提前退出——lighttpd 从 504s 变成 661s。
第二版改成：集合里存每个记录值的**整条 Replacement 链**；链被擦短后该值只会解析到
链上更早的成员（仍在集合里，不会漏判），链被加长才需要靠写入点失效重建。
代价是可能多返回几次 true（保守：少删一条死指令），实测计数器完全一致（见 8.4）。

### 8.3 效果（lighttpd，同命令）

| 指标 | 改前 | 改后 |
|---|---|---|
| 整次运行 | 504s | **138s**（另一次带插桩的测量 173s） |
| function-loop（722 个函数） | 682s | **53.8s** |
| `rewritePartialWrites` 的 store 循环合计 | 378.5s | **10.3s** |
| pw-compute-demands 合计 | 1.8s | 2.2s |
| unknown | 26 | 26 |

### 8.4 正确性核对

- `/tmp/sig_diff.py`（fix19 vs fix21）：7 个目标**函数签名零变化**（defs 数、形参列表全同）。
- SSA pass summary 计数完全一致（逐目标比对）：`dead_loads_removed` / `dead_stores_removed` /
  `partial_demand_matched` / `partial_demand_rejected` 全部相同。
- unknown 计数相同；IR bytes 只有 ±0.02% 的噪声差（libuv/vsftpd 完全相同）。
- 7 目标 `llvm-as` + `opt -passes=verify` 全 OK；`ctest` 12/12。

### 8.5 各目标量测（/tmp/fix21，改动后；括号内为改动前）

| 目标 | time | unknown | verify |
|---|---|---|---|
| lighttpd | 173s（504s） | 26（26） | OK |
| redis-cli | 146s（351s） | 7（7） | OK |
| libuv | 69s（100s） | 8（8） | OK |
| vsftpd | 47s（87s） | 0（0） | OK |
| memcached | 116s（264s） | 0（0） | OK |
| wrk | 29s（35s） | 0（0） | OK |
| ffmpeg | 159s（367s） | 5（5） | OK |
| **tmux** | **591s（timeout 1500s 无 IR）** | 25 | OK，IR 21.3MB |
| **ssh** | **378s（timeout 1500s 无 IR）** | 65 | OK，IR 12.1MB |

tmux/ssh 都在 sweep 的 `timeout 1500` 内产出 IR 并通过验证，P3 的目标达成。
| **wolfssl** | **1782s（timeout 1500s 无 IR）** | 556 | OK，IR 37.4MB |

tmux/ssh 都在 sweep 的 timeout 1500 内产出 IR 并通过验证，P3 的主要目标达成。
wolfssl（3964 个函数、IR 37MB）现在能跑完并验证通过，但 1782s 仍略高于 sweep 的
1500s 上限——它体量是 tmux 的 3 倍，按同一量级外推仍差 ~20%。如果还要继续压，
下一层热点看 base summary（3964 个函数的 SCC/定点）和 removeDeadStoresByLiveness。

### 8.6 残留与后续

- 缓存是"链成员"语义，理论上可能多返回 true（保守），实测无差异；如果将来发现
  `dead_loads_removed` 异常下降，先查这里。
- 临时插桩（`NOTDEC_DEBUG_SSA_TIMING` 那套）已删除，代码里没有 getenv。
- 下一步可选：`removeDeadStoresByLiveness`（改后占比上升）、base summary 的
  demand 定点（tmux 44s / ssh 105s，波动大）都不再是瓶颈，但可以作为后续优化。

## 9. wolfssl 为什么还要 ~1350s（2026-09-24 追加测量）

先把探针加回来（`NOTDEC_DEBUG_SUMMARY_TIMING` / `NOTDEC_DEBUG_SSA_TIMING`，测完已回退），
量的是改动后的代码（HEAD `2878c06`）。wolfssl：3964 个函数、485k loads / 667k stores、
IR 37MB（lighttpd 是 505 函数 / 69k loads / 102k stores / 4.6MB）。

| 阶段 | 耗时 | 说明 |
|---|---|---|
| pre-SSA（stack-frame-rewrite 11s + 其他） | ~15s | |
| base-summary-1（self-bottom-up） | **102s** | 537k transferBlock 调用 |
| base-summary-2（demand） | **238s** | 其中 solveFunction 只占 ~88s，其余在 demand 机制本身 |
| SSA function-loop（3964 个函数） | **449s** | 111ms/函数；最慢 FUN_5b610 12.5s、FUN_931d0 8.9s |
| shape-refine + rewrite-signature-shapes | ~19s | |
| cleanup-iteration 0（删 18238 条死 store） | **212s** | 含一次全模块 InstCombine+SimplifyCFG + 每函数完整清理管线 |
| cleanup-iteration 1（删 0 条，收敛） | **54s** | 明知删不到东西也要跑完整轮 |
| 未计时的余量（最终 cleanup/输出等） | ~260s | 按 lighttpd 的余量（~24s）× IR 体量（8×）外推一致 |

合计 ~1350s（本次插桩运行；非插桩的 /tmp/fix21 是 1782s，机器/指针序有波动）。

结论：不存在单一热点，是四块各占 7-33%：
1. **SSA function-loop 449s**：修完 A/B 之后每函数成本已经和函数指令数近似线性，
   要继续只能压常数（当前 ~111ms/函数，其中最大头是 `rewritePartialWrites` 的
   per-store 处理、`removeDeadStoresByLiveness` 的每函数反向定点）。
2. **demand pass 238s**：`runTopDownDemand` 是「每轮遍历全部函数 ⇒ `analyzeCallerDemand`」，
   而 `analyzeCallerDemand(F)` 只依赖 F 自己的 IR 和 F 自己的 ExitDemand（callee 需求是
   它的输出），所以「本轮 F 的 ExitDemand 没变就跳过 F」可以严格保语义地把
   轮数 × 全部函数 降到 只算有变化的函数（同一处还能把 `computeValueDemands(F)` 跨轮缓存）。
3. **cleanup 两轮 266s**：第 0 轮里全模块 InstCombine 与每函数清理管线各占一半左右（未细分）；
   第 1 轮删 0 条却仍要跑完整轮（54s）。可做的是「用 InstCombine 是否改动过来决定要不要
   再跑一轮」——注意这只能省掉死指令的清理（语义无害），要确认收益再改。
4. **base-summary-1 102s**：`transferBlock(State state)` 按值拷贝整个 State
   （Cells/Origins/StackSlots/StackSlotStores/ValueOrigins 六张 map，大的函数上千条），
   每块每轮拷一次；sp_mul 实测 1.5ms/transfer（lighttpd 平均 0.14ms）。可以改成
   传引用 + 只在需要保留输入状态时拷一次，或换 worklist 减少 transfer 次数。

如果要让 wolfssl 也进 1500s：优先做 2（改动小、语义等价、预计省 100-150s）和 3 的
InstCombine 短路（预计省 ~54s+），就能压到 ~1150s；再要快就得动 1 或 4。

## 10. 第二轮优化：demand 跳过 + cleanup 短路（2026-09-24，commit `2577874`）

### 10.1 改动

1. `runTopDownDemand`（`NativeRegisterSummary.cpp`）：给每个函数的 ExitDemand 记版本号，
   只有它自己变过才重算 `analyzeCallerDemand`。依据：该函数只读 F 自己的 IR 和 F 自己的
   `Demands[&function].ExitDemand`（`liveBeforeBlock` 里唯一的 `Demands[...]` 读），
   callee 的需求是它的**输出**。原来每轮遍历全部函数，等于重复 `computeValueDemands`
   加一次块级定点，规模是 轮数 × 全部函数。
2. `runPostRewriteInstCombine`（`NativeRegisterSummarySSA.cpp`）返回「IR 是否被改动」
   （InstCombine/SimplifyCFG 无改动时返回 `PreservedAnalyses::all()`）；cleanup 循环在
   `iteration > 0` 且两个 pass 都没改过 IR 时直接结束。这一轮面对的 IR 与上一轮结束时相同，
   上一轮「改写完再删死值」已经是同一个 IR 上的定点；剩下的死 store 不影响语义。

### 10.2 量测（/tmp/fix23 vs /tmp/fix21，同命令，10 个目标）

| 目标 | fix21 | fix23 | unknown | verify |
|---|---|---|---|---|
| lighttpd | 173s | **140s** | 26 | OK |
| redis-cli | 146s | 143s | 7 | OK |
| libuv | 69s | 74s | 8 | OK |
| vsftpd | 47s | 42s | 0 | OK |
| memcached | 116s | 95s | 0 | OK |
| wrk | 29s | 22s | 0 | OK |
| ffmpeg | 159s | 157s | 5 | OK |
| tmux | 591s | **474s** | 25 | OK |
| ssh | 378s | **330s** | 65 | OK |
| wolfssl | 1782s | **1385s** | 556 | OK |

10 个目标现在全部在 sweep 的 1500s 以内（libuv 的 69→74s 是噪声）。

### 10.3 等价性

- `/tmp/sig_diff2.py`（fix21 vs fix23）：9 个目标函数签名零变化（tmux/ssh 也纳入对比）。
- unknown 计数逐目标不变；SSA 计数 `dead_loads_removed` / `dead_stores_removed` 完全一致
  （lighttpd 32/50、tmux 77/139、ssh 332/543、wolfssl 190/225）。
- 全部 `llvm-as` + `opt -passes=verify` OK；`ctest` 12/12；单测通过。

## 11. 第三轮优化：工作队列 + 记录值集合改成模块级增量（2026-09-24，commit `2b1de93`）

### 11.1 怎么找到这两处的

对整次 wolfssl 运行做了一次低频 perf 采样（`-F 199`，22 分钟），发现**上一轮加的**
`FunctionBuilder::ensureRecordedValueCache` 自己占了全程序 **33.5%** 采样（其中
`insertChain` 22.8%、`DenseSet` 12.7%）：它按设计是「每个 FunctionBuilder 重建一次」，
而每次重建要扫**全模块**的 `SignatureState.CallArgs`；wolfssl 有 3964 个函数、每个函数
还要跑 run + 两轮清理，于是变成 函数数 × 全模块行数 × 3。lighttpd 上只有 3720 条所以
看不出来，大模块上就成了最大头。

### 11.2 改动

1. `Analyzer::solveFunction`（`NativeRegisterSummary.cpp`）：`while(changed){for each block}`
   改成工作队列（FIFO），只有前驱出口变了的块才重新 `transferBlock`。转移函数对各 Cell 位
   是单调 gen/kill，工作队列在同一个格上做混沌迭代，收敛到同一不动点。
2. 记录值集合（`NativeRegisterSummarySSA.cpp`）：`SignatureState` 增加
   `CallArgRecordedValues` / `CallArgRecordedStores`（记录绑定时增量插入）和
   `CallArgRecordedValuesByFunction`（按函数名分组的绑定值）。查询 = 模块级集合一次哈希
   （跨函数记录值的 `resolve` 恒等，直接命中）+ 本函数自己那几个记录值按解析结果再比一次。
   用函数名分组而不是 `Function*`：签名重写会 `takeName` 换新对象，清理遍按名字仍能取到
   主循环记录的那批值。`isRecordedFunctionReturnValue` 保留每函数小集合（返回值很少）。

### 11.3 量测（/tmp/fix24 vs /tmp/fix23 vs /tmp/fix21，同命令，10 个目标）

| 目标 | fix21 | fix23 | fix24 | unknown | verify |
|---|---|---|---|---|---|
| lighttpd | 173s | 140s | **90s** | 26 | OK |
| redis-cli | 146s | 143s | **87s** | 7 | OK |
| libuv | 69s | 74s | **40s** | 8 | OK |
| vsftpd | 47s | 42s | **32s** | 0 | OK |
| memcached | 116s | 95s | **73s** | 0 | OK |
| wrk | 29s | 22s | **19s** | 0 | OK |
| ffmpeg | 159s | 157s | **100s** | 5 | OK |
| tmux | 591s | 474s | **310s** | 25 | OK |
| ssh | 378s | 330s | **273s** | 65 | OK |
| wolfssl | 1782s | 1385s | **809s** | 556 | OK |

相对本轮动工前（fix19/fix21）：lighttpd 504→90s、tmux 591→310s、ssh 378→273s、
wolfssl 1782→809s（wolfssl 之前还会被 1500s 超时杀掉）。

### 11.4 等价性

- `/tmp/sig_diff3.py`（fix23 vs fix24）：10 个目标函数签名零变化。
- unknown 计数逐目标不变；SSA 计数 `dead_loads_removed`/`dead_stores_removed` 完全一致
  （lighttpd 32/50、tmux 77/139、ssh 332/543、wolfssl 190/225）。
- 10 个目标 `llvm-as` + `opt -passes=verify` 全 OK；`ctest` 12/12；单测通过。

### 11.5 残留

- 记录值集合是「只增不减」的，签名重写后里面可能留着已删除 Value 的指针；查询只用活值，
  与改动前的行为一致（原来也是拿 `CallArgs` 里的值比）。
- 工作队列假定转移函数单调（原实现的不动点也依赖同一假设）；如果将来出现非单调规则，
  这里要一起看。
- 现在最大的剩余成本已经不是单点：wolfssl 809s 里 base summary 两遍 + demand、SSA 逐函数
  循环、清理轮各占一块，需要重新量一遍才知道下一刀切哪里。

