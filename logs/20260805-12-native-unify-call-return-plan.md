# Native SummarySSA：返回信息唯一来源是签名（去掉第一遍 return/clobber 区分）

## 用户原始 prompt

> 思考一下，怎么解决前两个问题（外部 `sqrtl` 原型 arity=0；`stats_stdev` 返回 ST0 变 unknown）。
> …（讨论中逐步收敛）…
> 我觉得是否是返回寄存器的信息直接通过签名获取？或者就和签名信息一起保存？这样所有的静态分析都不需要保留 Return 作为某种结果，这个方向可行性怎么样
> 先形成一个规划文件 然后按照规划开始实现吧，文件里也记录一下本次讨论的过程和思路历程

## 背景

bin2llvm 的 register summary + SummarySSA 链路在 wrk 上留了两个问题：

1. 外部 `sqrtl`（`long double sqrtl(long double)`）原型推断失败：SysV 下 long double 参数走栈（80 位），未知外部 arity 推断只数寄存器前缀（RDI..R9 全 clobber → arity=0），推断产出的参数类型也没有"80 位栈参"。
2. `stats_stdev` 返回链断掉：返回路径 `bb_851a` 调用 `sqrtl`，ST0 被 ABI clobber，`common.ret` 的返回绑定读不到确定值，整体降级成 `notdec.unknown`。

深入讨论后发现这两个问题背后是同一个架构问题：**第一遍 SSA 构建在调用点区分 return/clobber**，而"是不是返回寄存器"本该由签名（`shape.Returns`）决定：

- 外部浮点返回（XMM0/ST0）没有 RAX 那种"整型 ABI 输出默认返回"，导致 `sqrtl`、`round`、`luaL_checknumber` 的浮点返回全部接不住；
- `collectFunctionReturnValues` 用 `mayDependOnSummaryClobberValue` 对"依赖 clobber 占位"的返回绑定整体判死成 unknown，连 phi 里其他分支的确定值一起丢掉；
- 未知外部的返回槽要靠第一遍产生的 `ReturnHelpers`（"返回"语义）才补得进来，存在鸡生蛋：第一遍不知道是不是返回，就无法产生返回占位，也就无法驱动签名补返回槽。

## 讨论过程与思路历程

1. 先定位两个问题的根因：`sqrtl` arity=0（栈参不可见）和 ST0 外部返回链断裂。
2. 提出两个修法：方案 A（long double libm 函数族进已知原型表 + LongDouble 类型）与方案 B（未知外部推断推广到栈证据）。同时提出"浮点 ABI 输出按 RAX 同等待遇"的三处小改（callEffect/callRangeValue/addDemandedExternalReturns）。
3. 用户追问 callEffect 的 return/clobber 区分发生在哪一遍、RAX 是怎么处理的、第一遍为什么区分、以及"本质都是被改动过的寄存器，是否交由后续参数推理阶段判断"。
4. 讨论确认：clobber 占位参与 phi 不会传播假值（占位语义就是显式未知）；phi 层面本来就对缺失分支填 unknown，真正"整体放弃"发生在返回绑定的 `mayDependOnSummaryClobberValue` 判死，这是信息损失最大的点。
5. 收敛到最终方向：**返回信息唯一来源是签名（`shape.Returns`）**。第一遍 SSA 构建只产"调用后状态"占位 + "ABI 输出寄存器被读"证据，不产生任何返回结论；未知外部返回槽由 `addDemandedExternalReturns` 基于读证据补进签名；重写后清理遍查签名，命中返回槽就 `extractReturnRange` 填真实值，否则保持 unknown。
6. 落地路径：返回绑定值（phi/占位）跨函数重写存活不了（`localizeReturnValue` 会把旧函数指令替换成 unknown），因此返回绑定收集从第一遍移到重写后的清理遍，重写时 ret 先建 unknown，清理遍再按最终链值更新。

## 目标

- 第一遍 SSA 构建不再区分 return/clobber：统一"调用后状态"占位。
- 返回槽（`shape.Returns`）成为"是否返回寄存器"的唯一信息源：内部函数/已知外部来自可信原型或 summary facts，未知外部来自读证据推断。
- 外部浮点返回（XMM0/ST0）和 RAX 一样能走通：`sqrtl`/`round`/`luaL_checknumber` 的返回链恢复。
- 返回绑定不再整体判死：重写后按签名解析，确定分支保留真实值，未知分支保持显式 unknown。

## 技术路线（模块级）

1. `callEffect` 第一遍分支去掉 `ReturnValue` 产生（外部声明的 `signatureReturnUsesUnit`/整型 ABI 输出默认、内部函数的 `ExitDemand` 分支都不再产生返回结论），只留 Preserve / Clobber / Unknown；清理遍分支保留（按 shape 判断，供 `extractReturnRange`）。
2. `transferRangeInstruction` / `callRangeValue` 去掉 `kind`（return/clobber）参数：第一遍统一生成 clobber 占位；`ReturnHelpers` 改语义为"调用点后 ABI 输出寄存器被读"的证据（内部函数/已知外部签名已定，不受影响）。
3. `addDemandedExternalReturns`：基于读证据给未知外部补返回槽，浮点输出（XMM0/ST0）补 `double`/`x86_fp80` 返回槽；已知外部仍受原型 + `MaxReturnRegisters` 约束。
4. 返回绑定移到重写后：第一遍不再收集（删 `collectFunctionReturnValues` 调用），重写时 ret 建 unknown，清理遍（`PostSignatureCleanup`）重新读槽位链值（此时已过重写，命中 `extractReturnRange`）并更新 ret operand。
5. 删除/放宽返回绑定的 `mayDependOnSummaryClobberValue` 整体判死（该函数保留，供参数 binding 的前缀保守用）。

## 风险与判断标准

- 风险 1：返回绑定移到清理遍后，ret 更新时序和 warning 生成点变化，需要保证最终 IR 没有未知返回链倒退。
- 风险 2：`native_register_summary_ssa_test` 里大量 `summary_return`/`summary_clobber`/返回绑定断言要跟着更新。
- 风险 3：第一遍不再产生 `summary_return` 占位，`RangeReturnHelpers`/重写时 `extractReturnRange` 通道空转，相关统计（`CallReturnValues`）归零，不影响正确性但调试信息变化。
- 判断标准：wrk 全量 rc=0、`llvm-as` + verify 通过、warning 数量不反弹（ST0 相关 3 条应消除或语义正确）；`sqrtl` 恢复为 `x86_fp80 @sqrtl(x86_fp80)`、`stats_stdev` 返回不再 unknown；native 测试全绿。

## 不做的事

- 未知外部栈参数推断（方案 B，`sqrtl` 的栈参侧）本次不做，仍押后；本次聚焦返回侧。
- 残留 clobber 占位统一替换成 `notdec.unknown` 的清理不做（保持现有 `remaining_summary_clobber_value` warning 行为），避免扩大改动面。
- 不改 `external/binarysub`，不碰顶层其他模块。

## 实现记录（已完成）

### 实现效果

- wrk 全量 `rc=0`，`llvm-as` + `opt -passes=verify` 通过；warning 从基线 1042 条降到 938 条，ST0 相关 3 条清零，`range_return_helper_rewrite_missing_value` 归零。
- `sqrtl` 恢复为 `declare x86_fp80 @sqrtl()`；`stats_stdev` 返回链完整（`common.ret` 的 ST0 phi 直接吃到 `tail call x86_fp80 @sqrtl()` 的结果，不再 unknown）。
- `round` 在 wrk 里被后续 intrinsic 匹配成 `llvm.round.f64`，返回 double 值真实流过 ZMM0 寄存器（修复前是 `notdec.unknown.i64` bitcast 成 double 的垃圾）。
- 未知外部浮点返回不再把 RAX/RDX/ZMM1/ST0 全塞进返回结构体。

### 与计划的偏离（实现中发现并修正）

计划第 3 步"基于读证据补返回槽"落地时发现：第一遍 `callRangeValue` 对**每个** ABI 输出寄存器都生成"调用后状态"占位并记 RangeReturnHelpers，所以"读证据"天然恒真——`round`（RAX+RDX+ST0+ZMM0/ZMM1 全被"读出"）和 `sqrtl`（RAX+ZMM0/ZMM1+ST0）被多推返回槽，重写成 `{ i64, i64, x86_fp80 } @round(double)`、`{ i64, double, double, x86_fp80 } @sqrtl()`。

修正为保守约束（`addDemandedExternalReturns`）：

1. 区分可信返回形状：arity 推断产物 `MaxReturnRegisters = UINT_MAX` 不算可信，和完全未知外部走同一套保守规则；内置/JSON 原型才按 `MaxReturnRegisters` 上限补槽。
2. 读证据收紧为"占位被实际读取（`use_empty()` 为假）"，死值不算。
3. 未知外部返回槽互斥且最多两类：浮点返回（XMM0 优先、有 x87 证据才 ST0）和整型返回（RAX 起，RDX 保守排除，其余整型输出按读证据补，真实 SysV 上就是 RAX 一个）互斥，浮点优先；旧 call 的 LLVM 返回类型（非 void）作为最高优先级证据，void call 才退回收紧后的读证据。ZMM1 等后续 SSE 输出一律当 clobber。

另外把重写时返回槽判定从"按寄存器 unit 匹配"收紧为"按 range 位段覆盖匹配"（与 `extractReturnRange` 一致）：ZMM0[64:64]/[128:384] 这类 keep-high 上层 lane 不在 double 返回槽覆盖范围内，按 clobber 静默转 unknown，不再刷 `range_return_helper_rewrite_missing_value`。

### 修改的文件与函数

- `lib/passes/summary/NativeRegisterSummarySSA.cpp`：
  - `addDemandedExternalReturns`（2535 行起）：重写未知外部返回槽选择逻辑（上述保守约束）；`hasAnyRangeHelper`/`hasLiveReadEvidence` 两个证据 lambda；`trustedReturns` 区分可信原型；删除 `getenv("NRSSA_DEBUG_RET")` 调试块和 `rangeReturnHelpersUseRegister`。
  - `floatReturnSlotForUnit`（1737 行）：新增，按浮点输出槽位宽生成 `double`/`x86_fp80` 返回槽。
  - `isLikelyNonReturnIntegerAbiOutput`（1710 行）：保留恢复，RDX 排除只在未知外部整型循环里用。
  - `callRangeValue`（6414 行）：去掉 `kind` 参数，占位统一 `clobber`；记录 ABI 输出寄存器 RangeReturnHelpers 作为证据。
  - `collectFunctionReturnValues`（6553 行）：`recordBinding` 参数，第一遍 `false`（2926-2936 行调用点），重写后清理遍第一轮 `true` 更新 ret operand。
  - `removeDeadStoresAfterSignatureRewrite`（2959 行）：`collectReturns` 只在重写后第一轮开。
  - `rewriteSignatureShapes`（7724 行附近）：返回槽判定改 range 位段覆盖。
- `tests/native_register_summary_ssa_test.cpp`：
  - `testUnknownExternalFloatReturnPicksSingleXmmSlot`（4773 行，main 9142 行）：新增，锁定"浮点返回优先、RAX 不混入"。
  - 既有断言随新语义更新（`CallReturnValues==0`、RDX 显式 unknown、vararg float 加 `float_write` metadata）；删除 `testLoopPartialWriteUsesNarrowRangePhi` 里残留的调试打印。

### 验证

```bash
cmake --build build --target native_register_summary_ssa_test notdec-native-llvm -j$(nproc)
./build/bin/native_register_summary_ssa_test                 # 全绿
# native_analysis_facts / native_external_call_signature_rewrite /
# native_prototype_recovery / native_register_effects /
# native_register_summary / pcode_to_llvm / native_instcombine_metadata 全绿
# native_abi_cspec_test / native_prototype_model_test 用 x86-64-gcc.cspec 参数跑全绿
./build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk \
  -o /tmp/wrk-fix2.ll --all-confirmed --skip-runtime \
  --register-ssa-warning-out /tmp/wrk-fix2.warn.tsv          # rc=0，938 条 warning
llvm-22.1.0.obj/bin/llvm-as /tmp/wrk-fix2.ll -o /tmp/wrk-fix2.bc
llvm-22.1.0.obj/bin/opt -passes=verify /tmp/wrk-fix2.bc -o /dev/null   # VERIFY-OK
```

### 评分与后续

- 实现效果：外部浮点/整型返回和 RAX 同待遇的目标达成，`sqrtl`/`stats_stdev` 返回链恢复，wrk warning 净减 104 条且不再有 ST0 噪音。
- 复杂度：`addDemandedExternalReturns` 按"可信原型 / 未知外部"分两条路径，证据分"旧返回类型 / 读证据"两类，规则集中在单个函数内，可读性尚可；代价是规则里带了一点 SysV 特化（RDX 排除、XMM0 优先 ST0 次之）。
- 维护成本：后续加已知原型（如把 `round`/`sqrtl` 直接写进原型表）会走可信路径，不受这套启发式影响；未知外部启发式的边界（如"调用后同时读 XMM0 和 ST0 的 long double 函数"会误选 XMM0）已记录在函数注释里。
- 更好的方案：对未知外部做"后向数据流：调用后被读的寄存器值是否到达本函数 ret"再定返回槽，比当前"有 use 就算证据"更准，但需要跨函数重写前跑一遍数据流，改动面大，押后。
