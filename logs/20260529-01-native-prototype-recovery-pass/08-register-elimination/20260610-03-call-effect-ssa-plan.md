# Call effect aware register SSA plan

用户原始要求：

> 综合考虑Ghidra的方案，以及SSA构建参考的那个参考实现，就按照Ghidra这个思路，总结一下如果要做好这一块应该怎么做，将规划总结到文档。

## 背景

当前 `NativeRegisterSSA` 曾经通过 pass 结束补齐 missing incoming、未知边用 `undef` 来修 PHI 结构。这只能保证 IR 结构合法，不能说明 SSA 构建是对的，也不能完整表达 call 对寄存器值的影响。

真正要做好这一块，需要同时解决两件事：

- SSA 构建要像 Braun lazy SSA 一样，有清楚的 incomplete PHI / finalize 流程。
- call clobber 要像 Ghidra heritage 一样显式化，不能靠 `nullptr` 或 `undef` 抹掉语义。

## Ghidra 参考

Ghidra 的 call effect 模型不是简单“遇到 call 就断开”，而是把 call 对 storage 的影响建进数据流。

关键源码：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh:391`
  - `EffectRecord::unaffected`
  - `EffectRecord::killedbycall`
  - `EffectRecord::return_address`
  - `EffectRecord::unknown_effect`

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc:4234`
  - `FuncProto::hasEffect(addr, size)` 查询某个 storage 过 call 后的 effect。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc:1451`
  - heritage 遍历 callsite，按 `FuncCallSpecs` 和 prototype 判断当前 storage 是 input、output、unaffected、killedbycall 还是 unknown。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc:1510`
  - `unaffected` 不插 guard。
  - `unknown_effect` / `return_address` 插普通 `CPUI_INDIRECT` guard。
  - `killedbycall` 插 `newIndirectCreation(...)`，显式表示 call 创建了新值。

- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec:92`
  - `<output>` 声明返回寄存器，如 `RAX`、`RDX`。

- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec:114`
  - `<killedbycall>` 声明默认 call-killed storage。

- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec:119`
  - `<unaffected>` 声明 callee-saved storage。

对当前 native LLVM 链路的启发：

- call 后寄存器值不是“找不到”，而是有几种明确来源。
- preserved register 继续使用 call 前值。
- return register 使用 call output value。
- killed-by-call register 使用 call-created unknown value。
- possible input/output 应该记录为 callsite candidate，后续给 prototype recovery 用。

### Ghidra trial/use 到 native 的对应

当前 native 的 `strength` 不应该成为最终模型。它只是临时保护：

- 防止 prototype recovery 误吃 entry input、call clobber、默认 ABI input。
- 给日志和统计用，方便看现在哪些 candidate 还不可靠。

长期应该改成 Ghidra 风格：

```text
ABI/callee 发现可能 input
  -> 创建 call input trial
  -> 把 trial 显式插到 call 附近的 IR use
  -> Register SSA 只负责给这个 use 接上正确 reaching definition
  -> trial-use 检查决定 active / inactive / no_use / blocked
  -> prototype recovery 只消费 active trial
```

native 侧不需要照搬 Ghidra 的 p-code CALL operand 结构。可以继续用现在的 helper IR：

- `load @REG` 表示 call 点读 register。
- `@notdec.register.call_input.*(value)` 表示这个 call 可能使用该 register。
- call 后的 `@notdec.register.call_return.*()` / `@notdec.register.call_effect.*()` 加 `store @REG` 表示返回值或 clobber。

但判定层要从 `strength` 迁到 trial state：

- `trial`：ABI/callee 认为该 storage 可能是参数，IR 中已有 helper use。
- `active`：SSA value 能追到 call 前明确准备，或者 PHI 的所有有效 incoming 都能解释为准备路径。
- `inactive`：有候选，但证据不够，例如只来自 function entry input、前一个 call return、或者跨 call 保留下来的 register。
- `no_use`：能证明不是参数，例如 value 来自明确 clobber/unknown effect，或者 ancestor/use 检查失败。
- `blocked`：当前分析还不会判断，例如 stack alias、部分寄存器重叠、条件执行 effect。先不改 signature。

这里有两个边界：

- Register SSA 不看 `active/inactive`。它只保证 value/use/def/PHI 正确。
- Prototype recovery 不直接看 ABI input，也不直接看 `strength`。它最终看 trial state，第一版只消费 `active`。

`strength` 可以暂时保留为 metadata/stat：

- `strong_local_def` / `strong_phi` 先映射成 `active` 的候选证据。
- `weak_entry_input` / `return_forward` 先映射成 `inactive`。
- `blocked_call_effect` 先映射成 `no_use` 或 `blocked`，具体看能否证明来源是 call clobber。

这样做比继续扩大 `strength` 更稳。`strength` 是按当前 native 代码的局部来源分类；Ghidra 的 trial/use 是按“这个候选最后能不能成为参数”分类，后者才是 prototype recovery 需要的结果。

## Braun lazy SSA 参考

参考文档：

- `logs/20260529-01-native-prototype-recovery-pass/08-register-elimination/20260610-02-braun-ssa-reference.md`

核心要求：

- `readVariable` 查当前 block 已知定义。
- `readVariableRecursive` 沿 predecessor 递归查 reaching definition。
- 遇到环时创建 incomplete PHI。
- block sealed 后调用 `addPhiOperands` 补齐所有 predecessor incoming。
- `tryRemoveTrivialPhi` 删除平凡 PHI，并更新所有引用。

对当前代码的要求：

- `PendingPhi` 不能只是 `PHINode *` cache。
- 需要明确 PHI 状态：`Incomplete`、`Completing`、`Complete`。
- 所有 pending PHI 必须有统一 finalize。
- finalize 后仍留在 IR 里的 PHI 必须满足 LLVM CFG predecessor 数。

## 当前问题

当前 `NativeRegisterSSA` 的主要问题不是没有 ABI 信息，而是 ABI 信息没有变成显式数据流。

已有能力：

- `NativeAbi` 已经从 Ghidra cspec 解析 `input` / `output` / `unaffected` / `killedbycall`。
- `NativeRegisterSSA` 已经能识别 `AbiEffects.Unaffected` 和 `AbiEffects.KilledByCall`。
- `NativePrototypeRecovery` 已经能消费部分 register input / return metadata。

缺口：

- `hasCallBefore` / `blockHasClobberingCall` 仍然用 `nullptr` 表示 call 后值不可恢复。
- pending PHI finalize 里的 `undef` 只是结构补洞，没有记录对应哪个 call、哪个 register、哪类 effect。
- call return value 和 call-clobbered unknown value 没有区别。
- callsite 参数和返回值没有在 register SSA 阶段稳定显式化。

## 目标

目标不是一次性把所有 signature rewrite 做完，而是先把 call effect 变成可追踪 SSA 值。

期望效果：

- `NativeRegisterSSA` 不再因为 call clobber 返回裸 `nullptr`。
- call effect 以带 metadata 的 LLVM value 表达。
- PHI incoming 不应该靠 pass 末尾补洞。正确的 SSA 构建应该在 seal/finalize 时按 CFG 补齐每条 predecessor incoming；如果仍缺 incoming，应该让 verifier 暴露问题，而不是补 `undef` 或随手补 unknown。
- prototype recovery 可以区分：
  - function entry input
  - call input
  - call return
  - call clobbered unknown

## 建议设计

### 0. 先显式化 register use/effect，再做 Register SSA

当前 `attachCallInputCandidates()` 是 register SSA 之后的补丁：它重新扫描 call 前局部值，再造 candidate helper。这条路不完整，因为 call input 没有真正进入 SSA use 链。

更合理的结构要贴近 Ghidra heritage：

```text
扫描 register storage access
  -> 给 call 插入显式 register input helper / output effect helper
  -> Register SSA 统一处理 load/store/call-use/call-effect
  -> trial/use 检查消费 SSA 结果
  -> prototype recovery 消费 active trial
```

关键变化：

- call input 不再是 SSA 之后“看一眼同 block store”的 metadata 补丁。
- ABI input register 在 call 点表现成一次 register use。
- 多前驱、循环、trivial PHI 全部交给 Braun-style Register SSA。
- candidate 只负责记录“这个 callsite 的某个 ABI slot 读到了哪个 SSA value”，不自己重新查 CFG。

Ghidra 对应点：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc:1495`
  - `guardCalls()` 发现可能 input 时注册 input trial。
  - 创建对应 `Varnode`。
  - `fd->opInsertInput(op, vn, op->numInput())` 把它插成 CALL 的 input。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh:210`
  - `ParamTrial` 记录一个候选 storage。
  - 关键状态包括 `active`、`used`、`defnouse`、`killedbycall`、`unref`。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc:1963`
  - `ParamActive::registerTrial()` 创建 trial。
  - register trial 默认 `markKilledByCall()`，意思是跨 call 保留下来的寄存器值通常不适合直接当参数证据。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc:5585`
  - `FuncCallSpecs::checkInputTrialUse()` 检查 trial 对应 varnode 的祖先和 use。
  - 能证明是 call 前有效准备的值，标 `active`。
  - 明确不是参数的，标 `defnouse` 并把 CALL input 换成常量，释放数据流。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc:5685`
  - `FuncCallSpecs::buildInputFromTrials()` 只保留最终 `isUsed()` 的 trial，重建 CALL input 列表。
- 后续 heritage SSA 把这个 CALL input 当普通 use rename，不需要单独处理跨 block dominance。

native 侧不一定马上改真实 LLVM call signature，但要达到同样语义：

- 在 Register SSA 内部把 call input 当作 register read。
- 如果不能改真实 call operand，就用 helper call 承载 value，但 helper 的 operand 必须来自统一 SSA 查询结果。
- 不再从另一个 block 随手拿 instruction 当 helper operand。

#### 用 helper/intrinsic-like call 承载 use/effect

这里不建议长期维护一个和 IR 并行的 `Fact` 表。原因很简单：metadata 不是 SSA use，独立表也容易和 IR 失步。真正的数据流应该落在 IR 里，C++ 里的结构体最多作为构造时的临时 descriptor。

call input 推荐形态：

```llvm
%rdi.before.call = load i64, ptr @RDI
call void @notdec.register.call_input.i64(i64 %rdi.before.call),
  !notdec.register.call_input_candidate !{...}
call void @target(...)
```

call effect / return 推荐形态：

```llvm
%rax.after.call = call i64 @notdec.register.call_return.i64(),
  !notdec.register.call_effect !{...}
store i64 %rax.after.call, ptr @RAX
%rbx.after.call = call i64 @notdec.register.call_effect.i64(),
  !notdec.register.call_effect !{...}
store i64 %rbx.after.call, ptr @RBX
```

也就是同时保留 register `load` / `store` 和 helper call：

- input side：`load register` 紧跟 `call_input` helper，然后才是真实 call。
- output side：真实 call 后紧跟 `call_return` / `call_effect` helper，再紧跟写回 register 的 `store`。
- 不要把 load/store 拉到很远的位置。`load` 是 call 点看到的 register 值，`store` 是 call 后 register state 的新定义，二者都应该作为 Register SSA 的普通读写点。
- call 侧只暴露这个 call 自己的 input/effect，不负责回看 call 前已有 store，也不负责匹配 call 后已有 load；这些数据流关系交给 Register SSA。

metadata 只记录解释信息，例如：

- `callsite_id`
- `register`
- `slot`
- `kind = input_use | return | clobber_unknown | unknown_effect | preserved`
- `source = abi_input | abi_output | abi_unknown | callee_recovered_return | callee_clobbers`
- `strength`
- backing global / storage range

生成时机：

1. 扫描函数和 ABI model，给每个非 intrinsic call 建 callsite id。
2. 对 ABI input pentry，在 call 前保留对应 register load，并紧跟插入 `call_input` helper。
3. 对 ABI output / killedbycall / unknown effect，在 call 后插入 `call_return` / `call_effect` helper，并紧跟 store 回对应 register。
4. direct callee 已有 recovered prototype / preserves / clobbers 时，覆盖 ABI fallback。

这样更接近 Ghidra 的 `opInsertInput()`：Ghidra 是把 input varnode 直接插进 CALL p-code op；LLVM 侧暂时不改真实 call signature，所以用紧邻 call 的 helper 代表这个 use。

helper 不能被当成普通可删的纯函数。不要标 `readnone`、`speculatable` 这类属性；必要时用保守 side-effect 属性，保证优化不会随便删除、合并或移动它。metadata 只能说明来源，不能替代 helper operand，因为 metadata 本身不会形成 SSA use，本地 SSA value 也不能安全塞进全局 metadata。

#### candidate 强弱分类到 trial state 的过渡

把 call input 作为 SSA use 后，会自然拿到跨 block PHI，也会自然拿到 function entry input。短期仍可保留 candidate `strength`，但它只是 trial state 的输入证据，不是最终结论：

- `strong_local_def`
  - call 前本函数内明确写入该 register，可作为 `active` 证据。
- `strong_phi`
  - PHI 的 incoming 都能追到本函数内明确写入或其它 strong candidate，可作为 `active` 证据。
- `weak_entry_input`
  - 值只来自 function entry external input。可能是真参数转发，也可能只是寄存器仍然活着，先归 `inactive`。
- `blocked_call_effect`
  - 值来自 `unknown_effect` 或 `clobber_unknown`，先归 `no_use` 或 `blocked`。
- `return_forward`
  - 值来自前一个 call 的 `kind=return`。是否当参数转发，要单独规则，第一版先归 `inactive`。

prototype recovery 第一版只用 `active` trial 推 signature rewrite；`inactive/no_use/blocked` 可以进审计 metadata，不直接改 signature。

这样可以同时满足两点：

- SSA 层不绕开 PHI，不再有跨 block dominance 问题。
- prototype 层不把所有 ABI input / entry input 都误判成真实参数。

### 1. 引入 call effect value

在 `NativeRegisterSSA` 内部新增一个创建 helper value 的接口，例如：

```text
callEffectValue(call, register, kind)
```

`kind` 至少包含：

- `return`
- `clobber_unknown`
- `unknown_effect`

实现形式优先换成 LLVM intrinsic-like helper declaration，并把返回值 store 回对应 register backing global。当前 `freeze undef` 只能算过渡表示，能挂 metadata，但不是理想的数据流节点，后续不应该继续扩大这种用法。

建议 metadata：

```text
notdec.register.call_effect = {
  kind=return | clobber_unknown | unknown_effect,
  register=RCX,
  call=<call instruction>,
  callee=<optional callee function>,
  abi_model=__stdcall
}
```

注意：这里不建议直接继续用裸 `undef`。`undef` 只对 LLVM 合法性有用，对 NotDec 后续恢复没有语义信息。

这里也不建议跳过 store 去匹配后面的 register load。call effect helper 是新值来源，store 是 register state 的定义点；后面的 load 交给 Register SSA 按普通规则消掉或接到这个定义上。

### 2. 把 call effect 接入 readBlockExit

当前逻辑：

```text
localValueBefore(...)
if blockHasClobberingCall(...)
  return nullptr
return readBlockEntry(...)
```

应该改成：

```text
local value before terminator -> return local
last relevant call in block -> return valueAfterCall(call, register)
otherwise -> readBlockEntry
```

`valueAfterCall(call, register)`：

- 如果 register 是 ABI unaffected：返回 call 前值。
- 如果 register 是 callee known preserved：返回 call 前值。
- 如果 register 是 return storage：返回 call return value。
- 如果 register 是 killedbycall：返回 call clobber unknown value。
- 如果 effect unknown：返回 unknown effect value。

这里要复用 Ghidra 的分层：

- 先看具体 callee prototype / metadata。
- 再看 ABI default prototype。
- 外部 call 没有具体信息时，用 ABI。

### 3. 明确 call input candidate

Ghidra 在 heritage 里遇到可能 input 的 storage，会注册 trial 并把 varnode 插到 CALL input。

LLVM 侧可以先不改真实 call signature，但 call input 必须先作为 register SSA use，再记录 metadata：

```text
notdec.register.call_input_candidate
```

触发条件：

- ABI / callee prototype 认为该 callsite 可能读取某个 input register。
- call 前插入对应 register load，并紧跟 `call_input` helper。
- Register SSA 把这个 load rename 成 call 点的 current value。
- rename 后进入 trial/use 检查；现有 strong / weak / blocked 只作为过渡证据。

短期改造目标：

- 对每个 call，查 ABI input pentries。
- 对这些 register 在 call 前插入 load + `call_input` helper，load 不能离 helper 太远。
- 如果 SSA 查询产生 PHI，就让现有 pending PHI / finalize / trivial PHI 逻辑处理。
- metadata 暂时记录 candidate strength；后续 prototype recovery 应改成消费 `active` trial。

### 4. 明确 call output candidate

Ghidra 对 possible output 会 register trial；如果 output storage 需要 guard，会设置 effect。

LLVM 侧：

- 对 ABI output register 创建 call return value。
- 如果后续该 return register 被使用，prototype recovery 可以把它当 call output candidate。
- 如果 callee 是 direct known function，并且已有 recovered return metadata，则按 callee metadata 精化。

### 5. 重整 pending PHI

pass-end finalize 不能作为“补洞兜底”。它只应该完成 Braun SSA 里的 seal / `addPhiOperands` 工作：

1. 对每个 incomplete PHI，按 CFG predecessor 调统一的 `addPhiOperands`。
2. 每条 incoming 都必须来自正常 SSA 查询结果：entry input、local def、call return、call clobber 或明确的 unknown effect。
3. `unknown_effect` 只能来自明确的 call/storage effect，不能用来掩盖 SSA 算法没有补到某条边。
4. finalize 后如果 PHI incoming 数量仍不等于 predecessor 数，不再补 `undef`，也不再补 fake unknown；直接保留错误 IR，让 verifier fail。
5. 最后再 `tryRemoveTrivialPhi`。

这更接近 Braun 的 `addPhiOperands`：PHI 完整性是算法保证的，不是 pass 结束靠默认值补出来的。

### 6. trivial PHI 删除要完整

当前 `simplifyPhi` 只是 `replaceAllUsesWith` 加 `DeadPhis`。

后续应该补：

- 更新 `EntryValue` / `ExitValue` 里指向旧 PHI 的缓存。
- 更新 `Replacement`。
- 对使用该 PHI 的其它 PHI 递归尝试 trivial 删除。

这部分参考 SPIRV-Tools `TryRemoveTrivialPhi` 的思路。

## 分阶段计划

### 阶段 1：只显式化 clobber unknown

范围最小：

- 增加 `notdec.register.call_effect` metadata。
- `blockHasClobberingCall` 不直接返回 `nullptr`。
- 在明确 killedbycall 时生成 `clobber_unknown` value。
- pending PHI missing incoming 不再补裸 `undef`，也不补 fake `unknown_effect`。缺 incoming 说明 SSA 构建仍有 bug，应让 verifier 暴露。

判断标准：

- `hexx64.so -f 0x1156e0` 仍过 `llvm-as` / `opt verify`。
- 输出 IR 不再出现 register PHI incoming 的裸 `undef`。
- 能从 metadata 追到哪个 call 造成未知值。

### 阶段 2：显式化 call return

- 根据 ABI output pentry，为 `RAX/RDX/XMM0...` 创建 call return value。
- direct call 优先读取 callee recovered return metadata。
- prototype recovery 能把 call return value 当返回候选。

判断标准：

- 典型 `call; mov ..., rax` 不再表现成 unknown clobber，而是 call return。

### 阶段 3：显式化 call input helper

- 对 ABI input pentry 读取 call 前值。
- 在 call 前插入 `load @REG` 和紧邻真实 call 的 `@notdec.register.call_input.*` helper。
- 给 helper 写 input candidate metadata。
- prototype recovery 暂时仍可消费 strong candidate，但这只是过渡保护，不是最终方案。

判断标准：

- 典型 `mov rdi, x; call f` 能在 callsite metadata 里看到 `RDI=x`。

### 阶段 3b：重构 call input 为 SSA use

- 按 ABI input / callee metadata 构造 call input helper，不维护长期并行 fact 表。
- helper operand 来自 call 前对应 register load，后续由 Register SSA 把这个 load 接到正确 SSA value。
- 删除 `attachCallInputCandidates()` 里只扫同 block 的 current-value 逻辑；如果还有类似“匹配 call 前 store / call 后 load”的补丁逻辑，也随这次重构去掉。
- candidate helper 只承载 SSA 查询返回的合法 value；不要继续用 `freeze value` 当临时标记。
- helper 使用 dedicated intrinsic-like declaration，例如 `@notdec.register.call_input.*`。
- candidate metadata 暂时保留 `strength`，但只作为 trial/use 检查的输入证据。

判断标准：

- 跨 block 参数准备能形成合法 candidate，不触发 LLVM dominance 错误。
- 多前驱时由 Register SSA 建 PHI，PHI incoming 仍满足阶段 4 的 verifier 要求。
- entry input 不会直接导致 declaration / direct call 被误改 signature。
- `hexx64.so -f 0x1156e0` 的 `llvm-as` / `opt -passes=verify` 继续通过。

### 阶段 3c：引入 Ghidra 风格 trial state

- 为每个 call input helper 创建 trial 记录，字段至少包括 callsite、register、slot、SSA value、effect 来源、当前 state。
- 第一版 state 使用 `trial / active / inactive / no_use / blocked`。
- `strength` 只作为迁移期输入：
  - `strong_local_def` / `strong_phi` 倾向 `active`。
  - `weak_entry_input` / `return_forward` 倾向 `inactive`。
  - `blocked_call_effect` 倾向 `no_use` 或 `blocked`。
- 增加 trial-use 检查，不再让 prototype recovery 直接按 `strength` 改 signature。
- prototype recovery 改成只消费 `active` trial。

判断标准：

- 当前 summary 同时输出 trial state 统计和旧 `strength` 统计，方便对照。
- `active` 数量应小于等于现有 strong 数量，不应突然把 weak entry input 改成真实参数。
- Ghidra/Java 链路已经完全消除寄存器访问的函数，可以作为对照样本，比较 callsite active input。

### 阶段 4：补完整 lazy SSA finalize

- `PendingPhi` 改成状态对象。
- finalize 使用统一 `addPhiOperands`。
- trivial PHI 删除递归更新缓存。

判断标准：

- 不再需要结构性 fallback。
- 每个 PHI 都能追到 entry input、local def、call return、call clobber unknown 或 unknown effect。
- 如果某个 PHI 仍缺 incoming，验证应失败；不要为了让 IR 通过而补 `undef` / fake unknown。

## 风险

- `clobber_unknown` 不能被 LLVM 优化当普通纯函数随意合并，否则会丢掉 callsite 区分。需要 metadata 或 helper 参数绑定 callsite。
- direct callee metadata 可能还没算完，要保持 callee-first 顺序或允许暂时用 ABI fallback。
- x86-64 SysV 的 cspec 默认 `<killedbycall>` 只列了部分 storage，不能误认为没列出的 caller-saved 全都 unaffected。要以 Ghidra cspec 和当前 ABI parser 实际解析结果为准。
- stack 参数和 stack output 比 register 更复杂，第一阶段不要碰 stack。

## 不做什么

- 不退回 slot fallback / mem2reg。
- 不先做大范围 signature rewrite。
- 不把 call clobber 全部当 function entry input。
- 不把所有 unknown 都继续裸 `undef`。

## 2026-06-10 实现记录：阶段 1 clobber unknown 显式化

本次先完成阶段 1，不改真实 call signature，不接 prototype recovery。

### 改动

- `lib/passes/NativeRegisterSSA.cpp:56`
  - 新增 `CallEffectKey` / `EdgeEffectKey`，缓存同一 call/register/effect 和同一 edge/register/effect 的显式 effect value。

- `lib/passes/NativeRegisterSSA.cpp:856`
  - `readRegister` 不再因为 call clobber 直接返回 `nullptr`。
  - call 影响由 `localValueBefore` 统一转换成显式 value。

- `lib/passes/NativeRegisterSSA.cpp:865`
  - `localValueBefore` 遇到 call 时调用 `callEffectKind`。
  - 如果该寄存器受 call 影响，返回 `callEffectValue`。
  - preserved / unaffected register 继续向前查本地定义。

- `lib/passes/NativeRegisterSSA.cpp:920`
  - `callEffectKind` 复用现有 ABI/callee metadata：
    - stack pointer / ABI unaffected / callee preserves：无 effect。
    - callee clobbers 或 ABI killedbycall：`clobber_unknown`。
    - 其它 call effect：`unknown_effect`。

- `lib/passes/NativeRegisterSSA.cpp:1008`
  - `readBlockExit` 遇到 clobbering call 时不缓存 `nullptr`，而是通过 `localValueBefore(... terminator)` 生成显式 call effect value。

- `lib/passes/NativeRegisterSSA.cpp:1089`
  - pending PHI finalize 缺 incoming 时不再补裸 `undef`。
  - 改为 `edgeUnknownEffectValue`，生成带 `notdec.register.call_effect` metadata 的 `unknown_effect` value。

- `lib/passes/NativeRegisterSSA.cpp:1117`
  - `callEffectValue` 用 `freeze undef` 作为 typed placeholder，并挂 `notdec.register.call_effect`。
  - 这里保留了 NotDec 语义信息，后续 pass 能看到这是 call effect，不是普通未知常量。

- `lib/passes/NativeRegisterSSA.cpp:1185`
  - `callEffectMetadata` 记录：
    - `kind=clobber_unknown|unknown_effect`
    - `register=<name>`
    - register backing global
    - `call_block=<block name>`
    - `callee=<function name>`（如果是 direct call）

- `tests/native_register_effects_test.cpp:773`
  - 新增 `countCallEffects`，检查 call effect metadata。

- `tests/native_register_effects_test.cpp:921`
  - 更新测试预期：
    - external call 后 RAX load 被替换为 `clobber_unknown` call effect。
    - 重复 RAX load 复用同一个 call effect。
    - direct callee metadata 标明 clobber 后，caller 里的 RBX load 被替换为 `clobber_unknown` call effect。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target notdec-native-llvm native_register_effects_test -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test

/usr/bin/time -f 'TIME native-llvm-call-effect %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  -o /tmp/hexx64-1156e0-call-effect.ll \
  > /tmp/hexx64-1156e0-call-effect.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-call-effect.ll \
  -o /tmp/hexx64-1156e0-call-effect.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-call-effect.bc \
  -o /tmp/hexx64-1156e0-call-effect.verified.bc

/usr/bin/time -f 'TIME limited-summary-call-effect %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --decode-seed-limit 20 --summary-json /bin/ls \
  > /tmp/binls-native-summary-call-effect.json
```

结果：

- `native_register_effects_test` 通过。
- `hexx64.so -f 0x1156e0` 默认路径成功，时间 `55.17s`。
- `llvm-as` 通过，时间 `1.78s`。
- `opt -passes=verify` 通过，时间 `0.99s`。
- `/bin/ls --decode-seed-limit 20` 时间 `0.23s`，`confirmed_functions=20`，`basic_blocks=41`，`instructions=136`。
- `/tmp/hexx64-1156e0-call-effect.ll` 中：
  - register PHI incoming 裸 `undef` 数量：`0`
  - `notdec.register.call_effect` 数量：`1658`
  - `kind=clobber_unknown` 数量：`504`
  - `kind=unknown_effect` 数量：`1154`

### 遇到的技术判断

1. LLVM metadata 不能直接把 local call instruction 作为普通 metadata operand 保存。
   - 试过 `ValueAsMetadata` / `LocalAsMetadata`，`verifyModule` 都报 `Invalid operand for global metadata`。
   - 当前先记录 `call_block` 和 `callee` 字符串，保证 IR 合法。
   - 后续如果需要精确 callsite id，需要单独生成稳定字符串编号，或者换一种合法的 intrinsic/helper 建模。

2. x86-64 gcc cspec 默认 `<killedbycall>` 只列出 `RAX/RDX/XMM0`。
   - 当前未列入 `killedbycall`、也不是 `unaffected` 的寄存器，如 `RCX/RDI/RSI/R8/R9/R10/R11`，被标成 `unknown_effect`。
   - 这比擅自标成 `clobber_unknown` 更保守，也更贴近当前解析到的 Ghidra facts。
   - 如果后续希望按 ABI caller-saved 全部建模为 clobber，需要明确补充 ABI effect 来源，不能在 register SSA 里硬编码。

### 性能和风险

- `hexx64.so -f 0x1156e0` 从上一轮约 `53.62s` 到 `55.17s`，增加约 `1.55s`。
- call effect value 当前是 `freeze undef`，语义上仍是 unknown placeholder，但 metadata 已经保留了 NotDec 需要的来源信息。
- 还没有实现阶段 2 的 call return value，所以 `RAX/RDX` 目前仍归为 `clobber_unknown`，不是 return。
- 还没有实现阶段 3 的 call input candidate。

评分：

- 实现效果：7/10。阶段 1 目标达成，消除了裸 `undef` PHI，并显式记录 call effect。
- 复杂度：5/10。新增 call/edge effect cache 和 metadata，但仍局限在 register SSA。
- 维护成本：5/10。`call_block` 不是稳定 callsite id，后续接 prototype recovery 前需要再细化。

## 2026-06-10 实现记录：阶段 2 ABI output 显式化为 call return

本次完成阶段 2 的第一步：根据 ABI `<output>` 把 call 后返回寄存器建模为 `kind=return`，不再把 ABI output 和 killed-by-call 混在一起。

### 改动

- `lib/passes/NativeRegisterSSA.cpp:62`
  - `AbiRegisterEffects` 增加 `Outputs`，保存 ABI output register 名。

- `lib/passes/NativeRegisterSSA.cpp:187`
  - `collectAbiRegisterEffects` 解析 `notdec.abi` metadata 的第二个 child list，即 output pentries。
  - 只收 register output，stack output 暂不处理。

- `lib/passes/NativeRegisterSSA.cpp:953`
  - `callEffectKind` 的优先级调整为：
    - stack pointer / callee preserves / ABI unaffected：无 effect。
    - direct callee metadata 明确 clobbers：`clobber_unknown`。
    - ABI output register：`return`。
    - ABI killedbycall：`clobber_unknown`。
    - 其它：`unknown_effect`。

- `tests/native_register_effects_test.cpp:65`
  - 测试 ABI 增加 RAX output pentry。

- `tests/native_register_effects_test.cpp:928`
  - 测试 external call 后 RAX load 被替换成 `kind=return`。
  - 重复 RAX load 复用同一个 `return` call effect，并确认没有误用 `clobber_unknown`。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target notdec-native-llvm native_register_effects_test -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test

/usr/bin/time -f 'TIME native-llvm-call-return %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  -o /tmp/hexx64-1156e0-call-return.ll \
  > /tmp/hexx64-1156e0-call-return.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-call-return.ll \
  -o /tmp/hexx64-1156e0-call-return.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-call-return.bc \
  -o /tmp/hexx64-1156e0-call-return.verified.bc

/usr/bin/time -f 'TIME limited-summary-call-return %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --decode-seed-limit 20 --summary-json /bin/ls \
  > /tmp/binls-native-summary-call-return.json
```

结果：

- `native_register_effects_test` 通过。
- `hexx64.so -f 0x1156e0` 默认路径成功，时间 `56.49s`。
- `llvm-as` 通过，时间 `1.79s`。
- `opt -passes=verify` 通过，时间 `1.03s`。
- `/bin/ls --decode-seed-limit 20` 时间 `0.24s`，`confirmed_functions=20`，`basic_blocks=41`，`instructions=136`。
- `/tmp/hexx64-1156e0-call-return.ll` 中：
  - register PHI incoming 裸 `undef` 数量：`0`
  - `notdec.register.call_effect` 数量：`1658`
  - `kind=return` 数量：`504`
  - `kind=clobber_unknown` 数量：`0`
  - `kind=unknown_effect` 数量：`1154`

### 判断

- direct callee metadata 仍优先于 ABI output。也就是说，如果本模块 callee 分析证明某个 output register 被 clobber 而不是返回值，当前仍会用 `clobber_unknown`。
- 外部 call / 还没有具体 callee metadata 的 call，按 ABI output 建 `return`。
- 这一步还没有区分“函数实际返回了几个 ABI output”。例如 SysV 下 `RDX` 是 ABI output slot，但大多数函数只返回 `RAX`。当前先保留 ABI-level possible return，后续需要 prototype recovery 用实际使用和 callee prototype 精化。

### 性能和风险

- `hexx64.so -f 0x1156e0` 从阶段 1 的 `55.17s` 到 `56.49s`，增加约 `1.32s`。
- `RDX` 现在按 ABI output 标为 `return`，可能对只返回 `RAX` 的普通函数偏宽。后续阶段需要用 callee recovered prototype / callsite usage 缩小。

评分：

- 实现效果：7/10。ABI output 已和 clobber 分开，RAX/RDX 能作为 call return 传播。
- 复杂度：3/10。只复用现有 ABI metadata，没有改 ABI parser。
- 维护成本：4/10。output set 仍是 ABI-level，后续需要 prototype recovery 精化。

## 2026-06-10 实现记录：阶段 3 call input candidate 显式化

本次完成阶段 3 的保守子集：register SSA 阶段给 callsite 产出 ABI register input candidate，并让 prototype recovery 能消费这些 candidate。暂不改 stack input、XMM input，也不把所有 ABI input 都强行变成函数入口 input。

### 改动

- `lib/passes/NativeRegisterSSA.cpp:62`
  - `AbiRegisterEffects` 增加 `Inputs`，保存 ABI input register 顺序。

- `lib/passes/NativeRegisterSSA.cpp:207`
  - `collectAbiRegisterEffects` 解析 `notdec.abi` 的 input pentry child list。
  - 当前只收 register storage 名，保持 ABI 顺序。

- `lib/passes/NativeRegisterSSA.cpp:408`
  - `run()` 在 `rewriteLoads()` 之后调用 `attachCallInputCandidates()`。
  - 这样 call input candidate 看到的是已经 SSA 化后的本地值。

- `lib/passes/NativeRegisterSSA.cpp:958`
  - 新增 `localValueBeforeCallInput()`。
  - 这里只查 call 前本地可见值，不 fallback 到 block entry。
  - 原因是如果 fallback 到 entry，会把每个 call 都建成可能读取所有 ABI input register，制造假的函数入口 input。

- `lib/passes/NativeRegisterSSA.cpp:971`
  - 新增 `attachCallInputCandidates()`。
  - 对每个非 intrinsic call，按 ABI input register 顺序尝试读取 call 前本地 SSA 值。
  - 只处理 64-bit integer register candidate。
  - 对每个 candidate 创建 `freeze value` helper，并挂 `notdec.register.call_input_candidate`。
  - 同时在 call 上挂 `notdec.register.call_input_candidates`，记录 `slot`、`register` 和 backing global。

- `lib/passes/NativePrototypeRecovery.cpp:921`
  - 新增 `declarationInputParamForCandidate()`，把 call input candidate metadata 转成 `NativeRecoveredPrototypeParam`。

- `lib/passes/NativePrototypeRecovery.cpp:942`
  - `declarationInputParamsBeforeCall()` 先读取 `notdec.register.call_input_candidates`。
  - 旧的 call 前 store 回看逻辑保留，作为兼容路径。

- `lib/passes/NativePrototypeRecovery.cpp:1534`
  - 新增 `callInputCandidateValueBeforeCall()`，从 call 前同块最近的 candidate helper 取实际 SSA value。

- `lib/passes/NativePrototypeRecovery.cpp:1186`
  - `declarationCallInputRewriteForCall()` 在旧的 `callsiteInputValueBeforeCall()` 失败后，尝试使用 `callInputCandidateValueBeforeCall()`。

- `tests/native_register_effects_test.cpp:70`
  - 测试 ABI 增加 RDI input pentry。

- `tests/native_register_effects_test.cpp:160`
  - 新增 `createCallInputCandidateFunction()`，构造 `store RDI; call` 场景。

- `tests/native_register_effects_test.cpp:839`
  - 新增 `countCallInputCandidates()` 和 `hasCallInputCandidateMetadata()`，分别检查 helper metadata 和 callsite metadata。

- `tests/native_register_effects_test.cpp:936`
  - 新增阶段 3 断言：
    - RDI call input candidate helper 被创建。
    - call 上有 RDI input candidate metadata。

### 技术判断

- 没有把 local SSA value 直接塞进 call metadata。
  - 之前 call effect 阶段已经确认 LLVM 不接受 local instruction 作为全局 metadata operand。
  - 这次采用本地 helper instruction 承载 value，call metadata 只记录 slot/register/global。

- 没有对所有 ABI input 调完整 `readRegister()`。
  - 完整 `readRegister()` 在找不到本地定义时会创建 function entry external input。
  - 对 call input candidate 来说这会过宽：每个 call 都会看起来读了所有 ABI input。
  - 当前只接受 call 前本地已经能看到的值，后续如果要跨 block，需要单独做“不会制造新 entry input”的 current-value resolver。

- prototype recovery 消费 candidate 仍然保守。
  - candidate 能帮助 declaration input 推断。
  - 但真实 rewrite 仍要求能拿到 64-bit value。
  - stack/XMM/变参暂不处理。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target notdec-native-llvm native_register_effects_test -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test

/usr/bin/time -f 'TIME native-llvm-call-input %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  -o /tmp/hexx64-1156e0-call-input.ll \
  > /tmp/hexx64-1156e0-call-input.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-call-input.ll \
  -o /tmp/hexx64-1156e0-call-input.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-call-input.bc \
  -o /tmp/hexx64-1156e0-call-input.verified.bc

/usr/bin/time -f 'TIME limited-summary-call-input %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --decode-seed-limit 20 --summary-json /bin/ls \
  > /tmp/binls-native-summary-call-input.json
```

结果：

- `native_register_effects_test` 通过。
- `hexx64.so -f 0x1156e0` 默认路径成功，时间 `56.64s`。
- `llvm-as` 通过，时间约 `1.79s`。
- `opt -passes=verify` 通过，时间 `1.05s`。
- `/bin/ls --decode-seed-limit 20` 时间 `0.23s`，`confirmed_functions=20`，`basic_blocks=41`，`instructions=136`。
- `/tmp/hexx64-1156e0-call-input.ll` 中：
  - register PHI incoming 裸 `undef` 数量：`0`
  - `notdec.register.call_input_candidate` 数量：`163`
  - `notdec.register.call_input_candidates` 数量：`61`
  - `notdec.register.call_effect` 数量：`1677`
  - `kind=return` 数量：`508`
  - `kind=clobber_unknown` 数量：`0`
  - `kind=unknown_effect` 数量：`1169`

### 性能和风险

- `hexx64.so -f 0x1156e0` 从阶段 2 的 `56.49s` 到 `56.64s`，变化很小。
- candidate helper 当前是 `freeze value`。这能把 metadata 绑在本地 SSA value 上，但后续如果优化 pass 删除 unused helper，需要在使用前保留或转成更稳定的 helper intrinsic-like op。
- 当前只覆盖同 block call 前本地值。跨 block input candidate 需要一个不会制造 entry input 的 current-value resolver。

评分：

- 实现效果：6/10。call input 已显式可见，并接入 declaration input 推断；但跨 block、stack、XMM 还没做。
- 复杂度：5/10。新增 helper metadata 和 prototype recovery fallback，范围仍局限在 register input。
- 维护成本：5/10。metadata 形态清楚，但后续需要统一 callsite id 和 current-value resolver。

## 2026-06-10 实现记录：阶段 4 小步修复 trivial PHI cache

本次没有直接把 `PendingPhi` 改成完整状态机，只先修阶段 4 里最明显的问题：trivial PHI 删除时，IR use 被替换了，但 `EntryValue` / `ExitValue` / `Replacement` 里可能还缓存旧 PHI。

### 改动

- `lib/passes/NativeRegisterSSA.cpp:1181`
  - `simplifyPhi()` 在 `replaceAllUsesWith()` 之前调用 `replaceCachedValue()`。

- `lib/passes/NativeRegisterSSA.cpp:1204`
  - 新增 `replaceCachedValue()`。
  - 同步更新：
    - `EntryValue`
    - `ExitValue`
    - `Replacement`
  - 避免后续 `resolveValue()` 继续拿到已经被 trivial 化的旧 PHI。

- `tests/native_register_effects_test.cpp:542`
  - 新增 `createTrivialJoinRegisterPhiFunction()`。
  - 构造 diamond CFG，左右两边都写同一个 RAX 常量，join block 读取 RAX。

- `tests/native_register_effects_test.cpp:839`
  - 新增 `countRegisterPhis()`。

- `tests/native_register_effects_test.cpp:966`
  - 新增断言：
    - trivial join 的 RAX load 被替换。
    - trivial RAX PHI 被删除。
    - 返回常量仍是 `0x4242`。

### 判断

- 这是阶段 4 的低风险子集。
- 当前还没有把 `PendingPhi` 改成 `Incomplete/Completing/Complete` 状态对象。
- 当前也还没有把 `finalizePendingPhis()` 改成完整 `seal_all_blocks + addPhiOperands`。
- 这一步只修正 cache 和 IR 不一致的问题，为后续完整重构降低风险。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target native_register_effects_test -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test

/usr/bin/time -f 'TIME native-llvm-phi-cache %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  -o /tmp/hexx64-1156e0-phi-cache.ll \
  > /tmp/hexx64-1156e0-phi-cache.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-phi-cache.ll \
  -o /tmp/hexx64-1156e0-phi-cache.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-phi-cache.bc \
  -o /tmp/hexx64-1156e0-phi-cache.verified.bc
```

结果：

- `native_register_effects_test` 通过。
- `hexx64.so -f 0x1156e0` 默认路径成功，时间 `54.38s`。
- `llvm-as` 通过。
- `opt -passes=verify` 通过，时间 `0.97s`。
- `/tmp/hexx64-1156e0-phi-cache.ll` 中：
  - register PHI incoming 裸 `undef` 数量：`0`
  - `notdec.register.call_input_candidate` 数量：`163`
  - `notdec.register.call_input_candidates` 数量：`61`
  - `notdec.register.call_effect` 数量：`1677`
  - `kind=return` 数量：`508`
  - `kind=clobber_unknown` 数量：`0`
  - `kind=unknown_effect` 数量：`1169`

### 性能和风险

- 这一步只更新缓存指针，不增加新的 CFG/SSA 查询。
- `hexx64.so -f 0x1156e0` 没有性能回退。
- 后续仍需要继续处理完整 `PendingPhi` 状态和 finalize 语义。

评分：

- 实现效果：4/10。修掉了 trivial PHI cache 明显问题，但还没完成阶段 4 全部目标。
- 复杂度：2/10。只新增一个 cache 替换 helper 和一个测试。
- 维护成本：2/10。逻辑局部，后续完整 PHI 状态机仍可复用这一步。

## 2026-06-10 实现记录：阶段 4 missing incoming 重查 predecessor exit

本次继续推进阶段 4，但仍不一次性改完整 `PendingPhi` 状态机。改动点是：pending PHI finalize 发现缺 incoming 时，先按 Braun `addPhiOperands` 的方向重新查询 predecessor exit value，只有查不到时才生成 edge `unknown_effect`。

### 改动

- `lib/passes/NativeRegisterSSA.cpp:1239`
  - `completePhiIncoming()` 缺 incoming 时不再直接调用 `edgeUnknownEffectValue()`。
  - 改为调用 `missingPhiIncomingValue(pred, phi)`。

- `lib/passes/NativeRegisterSSA.cpp:1243`
  - 新增 `missingPhiIncomingValue()`。
  - 通过 `pendingPhiRegister(phi)` 找回 register backing global。
  - 用 `readBlockExit(pred, unit)` 重新查询 predecessor 出口值。
  - 查询成功时补真实 incoming。
  - 只有找不到 register unit 或查询仍失败时，才 fallback 到 `edgeUnknownEffectValue()`。

### 判断

- 这一步让 finalize 更接近 Braun 的 `addPhiOperands`。
- 仍保留现有 recursion guard 和 edge unknown fallback。
- 还没有把 `PendingPhi` 改成带 `Incomplete/Completing/Complete` 的状态对象。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target native_register_effects_test -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test

/usr/bin/time -f 'TIME native-llvm-phi-finalize %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  -o /tmp/hexx64-1156e0-phi-finalize.ll \
  > /tmp/hexx64-1156e0-phi-finalize.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-phi-finalize.ll \
  -o /tmp/hexx64-1156e0-phi-finalize.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-phi-finalize.bc \
  -o /tmp/hexx64-1156e0-phi-finalize.verified.bc
```

结果：

- `native_register_effects_test` 通过。
- `hexx64.so -f 0x1156e0` 默认路径成功，时间 `54.28s`。
- `llvm-as` 通过。
- `opt -passes=verify` 通过，时间 `0.95s`。
- `/tmp/hexx64-1156e0-phi-finalize.ll` 中：
  - register PHI incoming 裸 `undef` 数量：`0`
  - `notdec.register.call_input_candidate` 数量：`163`
  - `notdec.register.call_input_candidates` 数量：`61`
  - `notdec.register.call_effect` 数量：`1677`
  - `kind=return` 数量：`508`
  - `kind=clobber_unknown` 数量：`0`
  - `kind=unknown_effect` 数量：`1169`

### 性能和风险

- 这一步没有性能回退。
- 如果 `readBlockExit()` 在 finalize 阶段触发新的 recursive pending phi，现有 `ResolvingEntry` 仍负责打断递归。
- 完整状态对象仍是后续工作，不在这一步强做。

评分：

- 实现效果：5/10。missing incoming 会优先补真实 predecessor exit，语义比 edge unknown 更好。
- 复杂度：2/10。只新增一个查询 helper。
- 维护成本：3/10。仍依赖当前 pending PHI cache，后续状态机重构时需要纳入统一 `addPhiOperands`。

## 2026-06-10 实现记录：阶段 4 PendingPhi 状态显式化

本次把 `PendingPhi` 从裸 `PHINode *` cache 改成带状态的数据结构。目标是先让 incomplete PHI 的状态可见，后续再继续收敛到完整 `seal_all_blocks + addPhiOperands`。

### 改动

- `lib/passes/NativeRegisterSSA.cpp:74`
  - 新增 `PendingPhiState`：
    - `Incomplete`
    - `Completing`
    - `Complete`

- `lib/passes/NativeRegisterSSA.cpp:84`
  - 新增 `PendingPhiInfo`。
  - 保存 `PHINode *Phi` 和 `PendingPhiState State`。

- `lib/passes/NativeRegisterSSA.cpp:1129`
  - `readBlockEntry()` 取 pending PHI 时改读 `PendingPhiInfo::Phi`。
  - 正常补完 incoming 后调用 `markPendingPhiComplete()`。

- `lib/passes/NativeRegisterSSA.cpp:1176`
  - `ensurePhi()` 创建 `PendingPhiInfo{phi, Incomplete}`。

- `lib/passes/NativeRegisterSSA.cpp:1181`
  - 新增 `markPendingPhiComplete()`，按 PHI 指针把状态改成 `Complete`。

- `lib/passes/NativeRegisterSSA.cpp:1238`
  - `finalizePendingPhis()` 跳过 `Complete` PHI。
  - 对还没完成的 PHI 标成 `Completing`，补 incoming 后标成 `Complete`，再尝试 trivial PHI 删除。

- `lib/passes/NativeRegisterSSA.cpp:1354`
  - `pendingPhiRegister()` 改为从 `PendingPhiInfo::Phi` 查 register。

- `lib/passes/NativeRegisterSSA.cpp:1386`
  - `eraseUnusedPendingPhis()` / `forgetPendingPhi()` 改为使用 `PendingPhiInfo::Phi`。

### 判断

- 这一步满足阶段 4 里“`PendingPhi` 不能只是 `PHINode *` cache”的要求。
- 目前状态还比较轻：
  - `Incomplete`：递归中创建，等待补 incoming。
  - `Completing`：finalize 正在补 incoming。
  - `Complete`：已经补过 incoming，不需要 finalize 再处理。
- 没有引入动态 sealed block API，因为 native LLVM CFG 在 pass 运行时已经完整。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target native_register_effects_test -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test

/usr/bin/time -f 'TIME native-llvm-phi-state %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  -o /tmp/hexx64-1156e0-phi-state.ll \
  > /tmp/hexx64-1156e0-phi-state.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-phi-state.ll \
  -o /tmp/hexx64-1156e0-phi-state.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-phi-state.bc \
  -o /tmp/hexx64-1156e0-phi-state.verified.bc
```

结果：

- `native_register_effects_test` 通过。
- `hexx64.so -f 0x1156e0` 默认路径成功，时间 `54.60s`。
- `llvm-as` 通过。
- `opt -passes=verify` 通过，时间 `0.95s`。
- `/tmp/hexx64-1156e0-phi-state.ll` 中：
  - register PHI incoming 裸 `undef` 数量：`0`
  - `notdec.register.call_input_candidate` 数量：`163`
  - `notdec.register.call_input_candidates` 数量：`61`
  - `notdec.register.call_effect` 数量：`1677`
  - `kind=return` 数量：`508`
  - `kind=clobber_unknown` 数量：`0`
  - `kind=unknown_effect` 数量：`1169`

### 性能和风险

- `hexx64.so -f 0x1156e0` 没有性能回退。
- 当前状态机还没有记录“哪些 predecessor incoming 已经补过”，仍依赖 `completePhiIncoming()` 对现有 incoming block 去重。
- 如果后续要彻底贴近 Braun，可以把 `completePhiIncoming()` 拆成 `addPhiOperands(variable, phi)`，并把 incoming 去重、补边、trivial PHI 删除放进同一个流程。

评分：

- 实现效果：6/10。pending PHI 状态已显式化，finalize 不再把所有 pending PHI 一律当未完成。
- 复杂度：3/10。数据结构变了，但没有改核心递归策略。
- 维护成本：3/10。状态语义明确，后续继续收敛到 Braun 的 `seal_all_blocks` 更容易。

## 2026-06-10 实现记录：阶段 4 trivial PHI 递归简化

本次补齐阶段 4 里 `tryRemoveTrivialPhi` 的另一个明确缺口：一个 PHI 被简化后，使用它的其它 PHI 可能也变成 trivial，需要继续尝试简化。

### 改动

- `lib/passes/NativeRegisterSSA.cpp:1207`
  - `simplifyPhi(phi)` 改成外层入口，创建 `visiting` 集合。

- `lib/passes/NativeRegisterSSA.cpp:1212`
  - 新增递归版 `simplifyPhi(phi, visiting)`。
  - 简化当前 PHI 前，先收集它的 PHI users。
  - 当前 PHI `replaceAllUsesWith(same)` 后，对这些 PHI users 递归调用 `simplifyPhi()`。
  - `visiting` 防止 PHI 环里重复递归。

- `tests/native_register_effects_test.cpp:618`
  - 新增 `createCascadedTrivialRegisterPhiFunction()`。
  - 构造两层 join，第二层 PHI 依赖第一层 PHI。

- `tests/native_register_effects_test.cpp:1000`
  - 新增断言：
    - cascaded trivial RAX load 被替换。
    - cascaded trivial RAX PHI 全部被删除。
    - 返回值仍是 `0x5151`。

### 判断

- 这一步更接近 SPIRV-Tools / Braun 风格的 `TryRemoveTrivialPhi`。
- 当前只递归处理 PHI users，不扩大到任意 instruction users。
- `EntryValue` / `ExitValue` / `Replacement` cache 仍由上一轮 `replaceCachedValue()` 同步。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target native_register_effects_test -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test

/usr/bin/time -f 'TIME native-llvm-phi-recursive %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  -o /tmp/hexx64-1156e0-phi-recursive.ll \
  > /tmp/hexx64-1156e0-phi-recursive.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-phi-recursive.ll \
  -o /tmp/hexx64-1156e0-phi-recursive.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-phi-recursive.bc \
  -o /tmp/hexx64-1156e0-phi-recursive.verified.bc
```

结果：

- `native_register_effects_test` 通过。
- `hexx64.so -f 0x1156e0` 默认路径成功，时间 `54.85s`。
- `llvm-as` 通过。
- `opt -passes=verify` 通过，时间 `0.97s`。
- `/tmp/hexx64-1156e0-phi-recursive.ll` 中：
  - register PHI incoming 裸 `undef` 数量：`0`
  - `notdec.register.call_input_candidate` 数量：`163`
  - `notdec.register.call_input_candidates` 数量：`61`
  - `notdec.register.call_effect` 数量：`1677`
  - `kind=return` 数量：`508`
  - `kind=clobber_unknown` 数量：`0`
  - `kind=unknown_effect` 数量：`1169`

### 性能和风险

- `hexx64.so -f 0x1156e0` 没有性能回退。
- 递归只发生在 PHI users 上，范围受限。
- 后续如果要完全对齐 Braun，还可以把 `completePhiIncoming()` 重命名/拆成显式 `addPhiOperands()`，但当前功能上已覆盖计划里列出的 trivial PHI 删除缺口。

评分：

- 实现效果：6/10。trivial PHI 删除会向后传播到 PHI users。
- 复杂度：3/10。递归范围小，带 visited 防环。
- 维护成本：3/10。逻辑集中在 `simplifyPhi()`，后续完整 SSA 构建重构可继续复用。

## 2026-06-10 实现记录：阶段 2 direct callee recovered return 精化

本次补阶段 2 里还没完成的一点：direct call 优先读取 callee recovered return metadata。之前只要 register 是 ABI output，就会标成 `kind=return`；现在 direct internal callee 如果已经有 recovered returns，就只把 recovered return register 标成 `return`。

### 改动

- `lib/passes/NativeRegisterSSA.cpp:400`
  - 新增 `recoveredPrototypeReturnsRegister()`，读取 callee 的 `notdec.prototype.recovered` metadata return list。

- `lib/passes/NativeRegisterSSA.cpp:433`
  - 新增 `functionHasRecoveredReturns()`，判断 direct callee 是否已经有 recovered return。

- `lib/passes/NativeRegisterSSA.cpp:1081`
  - `callEffectKind()` 对 direct internal callee 先看 recovered return：
    - 如果 callee recovered returns 包含该 register，返回 `kind=return`。
    - 如果 callee 已有 recovered returns 但不包含该 register，不再走 ABI output fallback。
    - 没有 recovered returns 的 call 仍按 ABI output fallback。

- `tests/native_register_effects_test.cpp:77`
  - 测试 ABI 增加 RDX output，覆盖多 ABI output 场景。

- `tests/native_register_effects_test.cpp:284`
  - 新增 `attachRecoveredReturnMetadata()`，给测试 callee 手写 recovered RAX return。

- `tests/native_register_effects_test.cpp:307`
  - 新增 `createDirectRecoveredReturnEffectFunction()`。
  - callee recovered returns 只有 RAX，caller 在 call 后读取 RAX/RDX。

- `tests/native_register_effects_test.cpp:1154`
  - 新增断言：
    - direct recovered RAX return 生成 `kind=return`。
    - direct recovered callee 的非 return RDX 不再被 ABI output fallback 标成 `kind=return`。

### 判断

- 这一步只在 direct internal callee 已有 recovered returns 时收窄 ABI output。
- 外部 declaration、indirect call、还没有 recovered returns 的 direct call，仍保留 ABI output fallback。
- 如果 callee recovered prototype 后续变化，当前 pass 的 callee-first 顺序仍是关键前提。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target native_register_effects_test -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test

/usr/bin/time -f 'TIME native-llvm-direct-return %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  -o /tmp/hexx64-1156e0-direct-return.ll \
  > /tmp/hexx64-1156e0-direct-return.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-direct-return.ll \
  -o /tmp/hexx64-1156e0-direct-return.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-direct-return.bc \
  -o /tmp/hexx64-1156e0-direct-return.verified.bc
```

结果：

- `native_register_effects_test` 通过。
- `hexx64.so -f 0x1156e0` 默认路径成功，时间 `54.90s`。
- `llvm-as` 通过。
- `opt -passes=verify` 通过，时间 `0.94s`。
- `/tmp/hexx64-1156e0-direct-return.ll` 中：
  - register PHI incoming 裸 `undef` 数量：`0`
  - `notdec.register.call_input_candidate` 数量：`163`
  - `notdec.register.call_input_candidates` 数量：`61`
  - `notdec.register.call_effect` 数量：`1677`
  - `kind=return` 数量：`508`
  - `kind=clobber_unknown` 数量：`0`
  - `kind=unknown_effect` 数量：`1169`

### 性能和风险

- `hexx64.so -f 0x1156e0` 没有性能回退。
- 当前只读 register return metadata，不处理 stack/vector return 精化。
- direct callee 没有 recovered returns 时仍使用 ABI output，保持之前保守行为。

评分：

- 实现效果：6/10。direct callee recovered return 能收窄 ABI output，避免 RDX 这类 possible output 被误当真实 return。
- 复杂度：3/10。只读取已有 metadata，没有引入 prototype recovery 依赖。
- 维护成本：3/10。metadata 读取逻辑局部，后续可以替换成共享 helper。

## 2026-06-10 实现记录：稳定 callsite id 和 candidate dominance 修复

本次补上前面阶段 1/3 留下的 callsite 标识问题：call effect 和 call input candidate 都写入稳定的函数内 `callsite_id`。验证时发现 call input candidate helper 可能使用不支配 call 的 SSA value，顺手修掉这个 verifier 问题；同时修掉递归 PHI 简化带来的重复删除问题。

### 改动

- `lib/passes/NativeRegisterSSA.cpp:513`
  - `scanBlock()` 给每个非 intrinsic call 分配函数内顺序编号，保存到 `CallsiteId`。

- `lib/passes/NativeRegisterSSA.cpp:1082`
  - call input candidate helper metadata 增加 `callsite_id=<function>:<index>`。

- `lib/passes/NativeRegisterSSA.cpp:1103`
  - 新增 `valueDominatesCallInput()`。
  - call input candidate 只接受：
    - constant
    - function argument
    - 同 block PHI
    - 同 block 且在 call 前的 instruction
  - 不支配 call 的跨 block instruction 暂时跳过，避免生成非法 IR。

- `lib/passes/NativeRegisterSSA.cpp:1290`
  - `simplifyPhi()` 跳过已经进入 `DeadPhiSet` 的 PHI。
  - 修复递归 trivial PHI 简化后同一个 PHI 被重复加入 `DeadPhis`，最终 `eraseFromParent()` 重复删除崩溃的问题。

- `lib/passes/NativeRegisterSSA.cpp:1503`
  - call effect metadata 增加 `callsite_id=<function>:<index>`。

- `lib/passes/NativeRegisterSSA.cpp:1517`
  - 新增 `callsiteId()`，统一生成 metadata 字符串。

- `tests/native_register_effects_test.cpp:988`
  - 新增 `callEffectHasCallsiteId()`。

- `tests/native_register_effects_test.cpp:1088`
  - 新增 `callInputCandidateHasCallsiteId()`。

- `tests/native_register_effects_test.cpp:1250`
  - 新增断言：
    - RAX call return effect 有 callsite id。
    - RDI call input candidate 有 callsite id。

### 过程中发现的问题

第一次重链 `notdec-native-llvm` 后，`hexx64.so -f 0x1156e0` 崩在：

```text
FunctionPromoter::eraseDeadPhis
llvm::Instruction::eraseFromParent
```

原因是递归 PHI 简化会让同一个 PHI 多次进入 `DeadPhis`。这不是 callsite id 本身的问题，但这次完整重链 CLI 后暴露出来。已用 `DeadPhiSet` 修掉。

第二次运行时 verifier 报：

```text
Instruction does not dominate all uses!
%R8.call_input_candidate = freeze ...
```

原因是 call input candidate helper 插在 call 前，但 candidate value 可能来自不支配该 call 的其它 block。当前先保守过滤；跨 block call input candidate 需要单独的 current-value resolver，不能直接复用会制造 entry input 的 `readRegister()`。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target native_register_effects_test notdec-native-llvm -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test

/usr/bin/time -f 'TIME native-llvm-callsite-id %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  -o /tmp/hexx64-1156e0-callsite-id.ll \
  > /tmp/hexx64-1156e0-callsite-id.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-callsite-id.ll \
  -o /tmp/hexx64-1156e0-callsite-id.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-callsite-id.bc \
  -o /tmp/hexx64-1156e0-callsite-id.verified.bc

ctest --test-dir /tmp/notdec-bin2llvm-build \
  -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' \
  --output-on-failure

/usr/bin/time -f 'TIME limited-summary-callsite-id %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --decode-seed-limit 20 --summary-json /bin/ls \
  > /tmp/binls-native-summary-callsite-id.json
```

结果：

- `native_register_effects_test` 通过。
- 相关 `ctest` 6 项通过。
- `hexx64.so -f 0x1156e0` 默认路径成功，时间 `58.17s`。
- `llvm-as` 通过。
- `opt -passes=verify` 通过，时间 `0.93s`。
- `/bin/ls --decode-seed-limit 20` 时间 `0.24s`，`confirmed_functions=20`，`basic_blocks=41`，`instructions=136`。
- `/tmp/hexx64-1156e0-callsite-id.ll` 中：
  - register PHI incoming 裸 `undef` 数量：`0`
  - `notdec.register.call_input_candidate` 数量：`99`
  - `notdec.register.call_input_candidates` 数量：`39`
  - `notdec.register.call_effect` 数量：`1677`
  - `callsite_id=` 数量：`1737`
  - `kind=return` 数量：`508`
  - `kind=clobber_unknown` 数量：`0`
  - `kind=unknown_effect` 数量：`1169`

### 性能和风险

- `hexx64.so -f 0x1156e0` 本次为 `58.17s`，比上一轮 `54.90s` 慢约 `3.27s`。本次增加了 callsite id 字符串 metadata，同时 call input candidate 做 dominance 过滤；后续如果性能敏感，可以把 `callsiteId()` 的字符串缓存进 map，避免每次构造。
- call input candidate 从 `163` 降到 `99`，这是 dominance 过滤后的保守结果。
- 跨 block call input candidate 仍未解决，需要专门的 current-value resolver。

评分：

- 实现效果：6/10。call effect/input candidate 都有稳定 callsite id，且 IR dominance 合法。
- 复杂度：4/10。新增 callsite id map、dominance 过滤和 dead PHI 去重。
- 维护成本：4/10。metadata 更可追踪，但后续仍需要处理跨 block candidate。

## 2026-06-10 实现记录：call effect source metadata

本次继续补阶段 1 里“区分 ABI fallback 和 callee prototype 精化来源”的缺口。只补 metadata，不改变 call effect 判定顺序。

### 改动

- `lib/passes/NativeRegisterSSA.cpp:90`
  - 新增 `CallEffectInfo`。
  - `Kind` 继续决定 SSA 行为，`Source` 只记录为什么选这个 effect。

- `lib/passes/NativeRegisterSSA.cpp:1148`
  - `callEffectKind()` 改为返回 `CallEffectInfo`。
  - 当前 source 分类：
    - `callee_clobbers`
    - `callee_recovered_return`
    - `abi_output`
    - `abi_killedbycall`
    - `abi_unknown`

- `lib/passes/NativeRegisterSSA.cpp:1434`
  - `callEffectValue()` 接收 `CallEffectInfo`。
  - cache key 仍只用 call/register/kind，不因为 source 增加重复 value。

- `lib/passes/NativeRegisterSSA.cpp:1489`
  - PHI missing incoming fallback 的 edge unknown 标为 `source=phi_missing_incoming`。

- `lib/passes/NativeRegisterSSA.cpp:1504`
  - `callEffectMetadata()` 增加 `source=...` 字段。

- `tests/native_register_effects_test.cpp:988`
  - `callEffectHasCallsiteId()` 抽成通用 `callEffectHasField()`。

- `tests/native_register_effects_test.cpp:1259`
  - 新增断言：
    - external ABI output RAX return 带 `source=abi_output`。
    - direct recovered return RAX 带 `source=callee_recovered_return`。
    - direct callee RBX clobber 带 `source=callee_clobbers`。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target native_register_effects_test -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test

cmake --build /tmp/notdec-bin2llvm-build \
  --target notdec-native-llvm -j2

/usr/bin/time -f 'TIME native-llvm-call-effect-source %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  -o /tmp/hexx64-1156e0-call-effect-source.ll \
  > /tmp/hexx64-1156e0-call-effect-source.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-call-effect-source.ll \
  -o /tmp/hexx64-1156e0-call-effect-source.bc

/usr/bin/time -f 'TIME opt-verify-call-effect-source %e' \
  /sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-call-effect-source.bc \
  -o /tmp/hexx64-1156e0-call-effect-source.verified.bc

ctest --test-dir /tmp/notdec-bin2llvm-build \
  -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' \
  --output-on-failure

/usr/bin/time -f 'TIME limited-summary-call-effect-source %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --decode-seed-limit 20 --summary-json /bin/ls \
  > /tmp/binls-native-summary-call-effect-source.json
```

结果：

- `native_register_effects_test` 通过。
- 相关 `ctest` 6 项通过。
- `hexx64.so -f 0x1156e0` 默认路径成功，时间 `57.68s`。
- `llvm-as` 通过。
- `opt -passes=verify` 通过，时间 `0.93s`。
- `/bin/ls --decode-seed-limit 20` 时间 `0.22s`。
- `/tmp/hexx64-1156e0-call-effect-source.ll` 中：
  - register PHI incoming 裸 `undef` 数量：`0`
  - `notdec.register.call_effect` 数量：`1677`
  - `notdec.register.call_input_candidate` 数量：`98`
  - `notdec.register.call_input_candidates` 数量：`39`
  - `kind=return` 数量：`508`
  - `kind=clobber_unknown` 数量：`0`
  - `kind=unknown_effect` 数量：`1169`
  - `source=abi_output` 数量：`508`
  - `source=abi_unknown` 数量：`1169`
  - `source=callee_recovered_return` 数量：`0`
  - `source=callee_clobbers` 数量：`0`
  - `source=phi_missing_incoming` 数量：`0`

### 判断

- 这一步让 call effect metadata 能说明“来自 ABI fallback”还是“来自 callee metadata”，后续 prototype recovery 可以更保守地消费。
- `hexx64.so -f 0x1156e0` 当前没有 direct callee recovered source，测试里已覆盖 direct callee 两类 source。
- 本次没有处理跨 block call input candidate。那个需要新的 current-value resolver，仍是单独技术决策点。

评分：

- 实现效果：5/10。补齐来源审计信息，但不改变恢复能力。
- 复杂度：2/10。只是在现有 effect 判定上携带 source。
- 维护成本：2/10。字段稳定，后续消费时直接按 `source=` 判断。

## 2026-06-10 实现记录：缓存 callsite id 字符串

本次处理前面记录里的一个性能风险：`callsiteId()` 每次都拼 `function:index` 字符串。改动只缓存字符串，不改变 metadata 内容。

### 改动

- `lib/passes/NativeRegisterSSA.cpp:95`
  - 新增 `CallsiteInfo`，保存 callsite index 和已经拼好的 id 字符串。

- `lib/passes/NativeRegisterSSA.cpp:524`
  - `scanBlock()` 分配 callsite 编号时同步生成 `FunctionName:index`。

- `lib/passes/NativeRegisterSSA.cpp:1536`
  - `callsiteId()` 改为直接返回缓存字符串。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target native_register_effects_test notdec-native-llvm -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test

/usr/bin/time -f 'TIME native-llvm-callsite-cache %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  -o /tmp/hexx64-1156e0-callsite-cache.ll \
  > /tmp/hexx64-1156e0-callsite-cache.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-callsite-cache.ll \
  -o /tmp/hexx64-1156e0-callsite-cache.bc

/usr/bin/time -f 'TIME opt-verify-callsite-cache %e' \
  /sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-callsite-cache.bc \
  -o /tmp/hexx64-1156e0-callsite-cache.verified.bc

ctest --test-dir /tmp/notdec-bin2llvm-build \
  -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' \
  --output-on-failure

/usr/bin/time -f 'TIME limited-summary-callsite-cache %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --decode-seed-limit 20 --summary-json /bin/ls \
  > /tmp/binls-native-summary-callsite-cache.json
```

结果：

- `native_register_effects_test` 通过。
- 相关 `ctest` 6 项通过。
- `hexx64.so -f 0x1156e0` 默认路径成功，时间 `57.81s`。
- `llvm-as` 通过。
- `opt -passes=verify` 通过，时间 `0.95s`。
- `/bin/ls --decode-seed-limit 20` 时间 `0.22s`。
- `/tmp/hexx64-1156e0-callsite-cache.ll` 中：
  - register PHI incoming 裸 `undef` 数量：`0`
  - `notdec.register.call_effect` 数量：`1677`
  - `notdec.register.call_input_candidate` 数量：`99`
  - `notdec.register.call_input_candidates` 数量：`39`
  - `callsite_id=` 数量：`1737`
  - `kind=return` 数量：`508`
  - `kind=unknown_effect` 数量：`1169`

### 判断

- 这一步没有带来明显运行时间改善，`57.81s` 和上一轮 `57.68s` 基本持平。
- 改动仍有价值：metadata 内容稳定，同时避免后续继续增加 callsite metadata 时反复拼字符串。
- 下一步真正有语义收益的方向是跨 block call input candidate，但这需要新的 current-value resolver，不能直接复用会制造 entry input 的 `readRegister()`。

评分：

- 实现效果：2/10。只减少字符串重复构造，不改变 IR 语义。
- 复杂度：1/10。只新增一个缓存结构。
- 维护成本：1/10。`CallsiteInfo` 后续也可承载更多 callsite 事实。

## 2026-06-10 实现记录：prototype recovery 消费 call return effect

本次推进阶段 5 的一个明确缺口：register SSA 后，`call; load RAX` 可能已经变成 `call; %RAX.return.call_effect = freeze undef`。prototype recovery 的 direct callsite return rewrite 之前只认识旧的寄存器 load，现在补上同 block `kind=return` call effect value。

### 改动

- `lib/passes/NativePrototypeRecovery.cpp:4938`
  - `ReturnLoadSearchResult` 增加 `Value`。
  - 旧 `Load` 仍保留给 shared-successor PHI 路径用。

- `lib/passes/NativePrototypeRecovery.cpp:5098`
  - 新增 `isReturnCallEffectValue()`。
  - 只接受 metadata 同时满足 `kind=return` 和 `register=<return register>` 的 instruction。
  - `clobber_unknown` / `unknown_effect` 不会被当成 return load。

- `lib/passes/NativePrototypeRecovery.cpp:5118`
  - `findReturnLoadBeforeStoreInRange()` 在旧 register access load/store 判断前，先识别同 block 的 return call effect value。

- `lib/passes/NativePrototypeRecovery.cpp:5417`
  - `rewriteCallsiteReturnLoad()` 改为替换 `result.Value`。
  - 如果仍是旧 load，行为不变；如果是 call effect instruction，则直接用新 typed call result 替换它。

- `lib/passes/NativePrototypeRecovery.cpp:4979`
  - multi-return / input+multi-return callsite 结构增加 `ReturnValues`。
  - 避免 collection 接受 call effect value 后 rewrite 阶段只看 `Load` 而漏替换。
  - shared-successor 路径仍要求 `Load`，因为 call effect value 没有 register pointer，不能给其它 predecessor 生成 incoming load。

- `tests/native_prototype_recovery_test.cpp:374`
  - 新增 `createReturnEffectCallerFunction()`，构造 direct call 后的 `notdec.register.call_effect kind=return` value。

- `tests/native_prototype_recovery_test.cpp:5997`
  - 新增 return-only rewrite 断言：
    - callee 被改成 `i64()`。
    - caller 里的 call effect value 被新 call result 替换。
    - 新 call result 被使用。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target native_prototype_recovery_test -j2

/tmp/notdec-bin2llvm-build/bin/native_prototype_recovery_test

ctest --test-dir /tmp/notdec-bin2llvm-build \
  -R 'notdec\.native_(register|prototype|instcombine|abi)|native_register_residue' \
  --output-on-failure

cmake --build /tmp/notdec-bin2llvm-build \
  --target notdec-native-llvm -j2

/usr/bin/time -f 'TIME native-llvm-call-effect-return-consume %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  -o /tmp/hexx64-1156e0-call-effect-return-consume.ll \
  > /tmp/hexx64-1156e0-call-effect-return-consume.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-call-effect-return-consume.ll \
  -o /tmp/hexx64-1156e0-call-effect-return-consume.bc

/usr/bin/time -f 'TIME opt-verify-call-effect-return-consume %e' \
  /sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-call-effect-return-consume.bc \
  -o /tmp/hexx64-1156e0-call-effect-return-consume.verified.bc

/usr/bin/time -f 'TIME limited-summary-call-effect-return-consume %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --decode-seed-limit 20 --summary-json /bin/ls \
  > /tmp/binls-native-summary-call-effect-return-consume.json
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 `ctest` 6 项通过。
- `hexx64.so -f 0x1156e0` 默认路径成功，时间 `58.62s`。
- `llvm-as` 通过。
- `opt -passes=verify` 通过，时间 `0.95s`。
- `/bin/ls --decode-seed-limit 20` 时间 `0.21s`。
- `/tmp/hexx64-1156e0-call-effect-return-consume.ll` 中：
  - register PHI incoming 裸 `undef` 数量：`0`
  - `notdec.register.call_effect` 数量：`1677`
  - `notdec.register.call_input_candidate` 数量：`99`
  - `notdec.register.call_input_candidates` 数量：`39`
  - `callsite_id=` 数量：`1737`
  - `kind=return` 数量：`508`
  - `kind=clobber_unknown` 数量：`0`
  - `kind=unknown_effect` 数量：`1169`

### 判断

- 这一步让 prototype recovery 能直接消费 register SSA 产生的 `kind=return` value，而不是只依赖旧的 register load。
- `clobber_unknown` 和 `unknown_effect` 仍不会参与 return rewrite。
- shared-successor return rewrite 暂时仍只支持旧 load 形态。要支持 call effect value 的 shared-successor，需要另一套 incoming value 生成策略，不在本轮硬做。

评分：

- 实现效果：6/10。阶段 5 的 return 消费路径更贴近显式 call effect 数据流。
- 复杂度：4/10。`ReturnLoadSearchResult` 从 load 扩成 value，但旧 load 路径保留。
- 维护成本：4/10。后续如果继续支持 shared-successor call effect，需要扩展当前边界。

## 当前技术决策点

按这个计划继续推进，当前已经不是“补一个判断”能安全解决的状态。剩下两个方向都需要先定语义。

### 1. 跨 block call input candidate

当前 register SSA 只记录同 block、且支配 call 的 input candidate。这个限制来自旧实现，不是 SSA 语义要求。

正确方向是：把 call input 当成 register use：call 前插入 register load，紧跟 `call_input` helper，Register SSA 再把这个 load rename 成 call 点 current value。多前驱时是否建 PHI，完全按 Braun SSA 算法决定：

- 单前驱：递归查 predecessor exit。
- 多前驱：创建 PHI，按 predecessor 补 incoming。
- 循环：先建 incomplete PHI，finalize 时补齐。
- trivial PHI：删除并同步缓存。

旧实现不能直接放宽的原因是：

- 直接用 `readRegister()` 会在找不到本地定义时创建 function entry external input。
- 对 call input 来说，这会把每个 call 都变成“可能读所有 ABI input register”，制造假参数。
- 直接把跨 block value 插到 call 前 helper，又会遇到 LLVM dominance 问题。

所以这里不应该单独设计一个和 SSA 并行的 current-value resolver，也不应该维护一个长期独立的 call input fact 表。应该重构 `attachCallInputCandidates()`：

- 先按 ABI / callee metadata 决定这个 callsite 有哪些 input candidate。
- 每个 candidate 都先表现成 call 前 register load。
- 用 dedicated helper call 消费这个 load。
- Register SSA 负责把 helper operand 接到正确 SSA value。
- metadata 只记录 callsite、register、slot、source 和 strength。
- prototype recovery 根据 strength 决定是否消费。

剩下需要定的是 candidate 语义，不是 PHI 语义：

- function entry input 是 weak candidate 还是直接阻断。
- PHI incoming 全部 strong 时是否把 PHI 标成 strong。
- PHI 混合 strong / entry input 时怎么标。
- 值来自 `kind=return` 时算 weak transfer 还是阻断。
- 值来自 `unknown_effect` / `clobber_unknown` 时默认阻断还是记录审计 metadata。

在这些规则定下来前，不应该让 prototype recovery 消费跨 block / entry-derived candidate 去改 signature。但 Register SSA 层建 PHI 这件事本身不需要再犹豫。

### 2. shared-successor return call effect

当前 prototype recovery 已能消费同 block 的 `kind=return` call effect value。shared-successor return rewrite 仍只支持旧 load 形态。

原因是旧 load 有 register pointer，可以在其它 predecessor 上生成 incoming load：

```text
call path:       new typed call result
other pred path: load @RAX
join:           phi
```

但 call effect value 没有 pointer，只是一个绑定 callsite 的 SSA value。要支持 shared successor，需要先决定其它 predecessor 的 incoming 值来源：

- 重新从 register storage load。
- 复用 register SSA 的 current value。
- 生成 unknown effect。
- 或者暂时禁止 shared-successor call effect rewrite。

这里如果选错，会把不同路径上的返回寄存器值混掉，所以需要先定语义。

## 2026-06-13 实现记录：call helper load/store 形态和 PHI 缺边收紧

本次按上面的新规划推进，不再把 call input 当成 SSA 后的同 block store 回看补丁，也不再用 `freeze undef` 表示 call effect。

### 改动

- `lib/passes/NativeRegisterSSA.cpp:111`
  - 新增 `isNotDecRegisterHelperCall()`，让 `notdec.register.*` helper 不再被当成真实 clobber call。
- `lib/passes/NativeRegisterSSA.cpp:485`
  - 调整 `FunctionPromoter::run()` 顺序：先 `attachCallInputCandidates()` 插 call input load/helper，再 `rewriteLoads()`，最后 `annotateCallInputCandidateStrengths()`。
- `lib/passes/NativeRegisterSSA.cpp:1042`
  - 重写 `attachCallInputCandidates()`：
    - 对 ABI input register 在真实 call 前插入 register load。
    - 紧跟插入 `@notdec.register.call_input.*` helper call。
    - 不再调用旧的 `localValueBeforeCallInput()` 回看 call 前 store。
- `lib/passes/NativeRegisterSSA.cpp:1114`
  - 新增 `annotateCallInputCandidateStrengths()` / `refreshCallInputCandidateList()`。
  - helper operand 经 `rewriteLoads()` 后再分类 strength：
    - `strong_local_def`
    - `strong_phi`
    - `weak_entry_input`
    - `blocked_call_effect`
    - `return_forward`
- `lib/passes/NativeRegisterSSA.cpp:1514`
  - `completePhiIncoming()` 找不到 incoming 时不再造 `phi_missing_incoming` / `undef` fallback；缺边保留给 verifier 暴露。
- `lib/passes/NativeRegisterSSA.cpp:1563`
  - `callEffectValue()` 改成：
    - call 后插入 `@notdec.register.call_return.*` 或 `@notdec.register.call_effect.*`。
    - helper 返回值紧跟 `store` 回对应 register backing global。
- `lib/passes/NativePrototypeRecovery.cpp:633`
  - 新增同名 helper 识别，prototype recovery 的回看逻辑跳过 `notdec.register.*` helper。
- `lib/passes/NativePrototypeRecovery.cpp:927`
  - `declarationInputParamForCandidate()` 只消费 `strong_local_def` / `strong_phi` candidate；无 `strength` 的旧 metadata 仍兼容。
- `lib/passes/NativePrototypeRecovery.cpp:1548`
  - `callInputCandidateValueBeforeCall()` 对 helper call 读取第 0 个参数作为真实 candidate value，不再把 void helper instruction 当 value。
- `tests/native_register_effects_test.cpp:1126`
  - 新增 helper 形态、call effect store、`phi_missing_incoming` 消失等断言。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target native_register_effects_test native_prototype_recovery_test notdec-native-llvm -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test
/tmp/notdec-bin2llvm-build/bin/native_prototype_recovery_test

/usr/bin/time -f 'TIME native-llvm-call-helper-strength %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  -o /tmp/hexx64-1156e0-call-helper-strength.ll \
  > /tmp/hexx64-1156e0-call-helper-strength.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-call-helper-strength.ll \
  -o /tmp/hexx64-1156e0-call-helper-strength.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-call-helper-strength.bc \
  -o /tmp/hexx64-1156e0-call-helper-strength.verified.bc
```

结果：

- 两个单测通过。
- `hexx64.so -f 0x1156e0` 通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
- 本次时间：`68.22s`。
- 上一轮同用例记录为 `58.62s`，本次慢约 `9.60s`。
- `/tmp/hexx64-1156e0-call-helper-strength.ll` 统计：
  - `call_input` helper：`4374`
  - `call_return` helper：`928`
  - `call_effect` helper：`3130`
  - `strength=strong_local_def`：`665`
  - `strength=strong_phi`：`20`
  - `strength=weak_entry_input`：`801`
  - `strength=blocked_call_effect`：`2490`
  - `strength=return_forward`：`398`
  - `source=phi_missing_incoming`：`0`
  - `freeze undef`：`0`

### 判断

- 实现效果：7/10。call input/output 已变成显式 helper 数据流，call effect 也 store 回 register state；PHI 缺边不再被 fake unknown 掩盖。
- 复杂度：6/10。新增 strength 分类和 helper skip，理解成本比旧 metadata 补丁高，但比并行 Fact 表更贴近 IR。
- 维护成本：6/10。后续要继续收敛 strength 规则，尤其是 PHI 混合来源、return forward 是否可消费。

性能上有明显回退。主要原因是现在每个 ABI input register 都显式插 helper，不再只保留同 block strong candidate。语义方向是对的，后续优化不能把 call use 重新藏回审计数据里：

- 先引入 trial state，把 weak / blocked 显式判成 `inactive/no_use/blocked`，让 prototype recovery 不消费。
- 缓存 helper declaration 和常用 metadata，减少字符串和 IR 节点构造成本。
- prototype recovery 当前只消费 strong candidate；迁移后应只消费 `active` trial，避免全量 ABI input 误改 signature。

## 2026-06-13 实现记录：call input helper 统计接入 summary

上一段实现后，helper/strength 分布只能靠 `rg` 扫 IR。为了继续排查性能，先把统计接进 `NativeRegisterSSASummary`，方便每次用 `--register-ssa-summary` 直接看。

### 改动

- `include/notdec-bin2llvm/passes/NativeRegisterSSA.h:19`
  - `NativeRegisterSSAFunctionSummary` / `NativeRegisterSSASummary` 增加：
    - `CallInputHelpers`
    - `CallReturnHelpers`
    - `CallEffectHelpers`
    - `StrongCallInputs`
    - `WeakCallInputs`
    - `BlockedCallInputs`
- `lib/passes/NativeRegisterSSA.cpp:1098`
  - 插入 `@notdec.register.call_input.*` 时累计 `CallInputHelpers`。
- `lib/passes/NativeRegisterSSA.cpp:1128`
  - strength 分类后累计 strong / weak / blocked 数量。
- `lib/passes/NativeRegisterSSA.cpp:1582`
  - 插入 `call_return` / `call_effect` helper 时累计对应计数。
- `lib/passes/NativeRegisterSSA.cpp:1857`
  - `addFunctionSummary()` 汇总新增字段。
- `lib/passes/NativeRegisterSSA.cpp:1951`
  - `printNativeRegisterSSASummary()` 打印新增统计。
- `tests/native_register_effects_test.cpp:1360`
  - 增加 summary 计数非零断言。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target native_register_effects_test native_prototype_recovery_test notdec-native-llvm -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test
/tmp/notdec-bin2llvm-build/bin/native_prototype_recovery_test

/usr/bin/time -f 'TIME native-llvm-call-helper-summary %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  --register-ssa-summary \
  -o /tmp/hexx64-1156e0-call-helper-summary.ll \
  > /tmp/hexx64-1156e0-call-helper-summary.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-call-helper-summary.ll \
  -o /tmp/hexx64-1156e0-call-helper-summary.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-call-helper-summary.bc \
  -o /tmp/hexx64-1156e0-call-helper-summary.verified.bc
```

结果：

- 两个单测通过。
- `hexx64.so -f 0x1156e0` 通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
- 带 summary 本次时间：`69.47s`。
- summary 输出：
  - `call input helpers: 4374`
  - `call return helpers: 929`
  - `call effect helpers: 3159`
  - `call input strong: 685`
  - `call input weak: 1199`
  - `call input blocked: 2490`

### 判断

这一步没有改变 IR 策略，只是让性能排查有稳定计数。当前最明显的问题仍是 `weak + blocked = 3689` 个 candidate 已经显式进入 IR，但 prototype recovery 不消费它们。下一步不应改回“只 materialize strong candidate”；应该先加 Ghidra 风格 trial state，把这些 helper 判成 `inactive/no_use/blocked`，再看是否有纯性能层面的缓存或批量构造优化。

## 2026-06-13 实现记录：call input strength 语义测试

本次先不优化速度，按“功能要一直对”的方向补测试，锁住 call input strength 和 PHI 缺边策略。

### 改动

- `tests/native_register_effects_test.cpp:65`
  - 新增 `attachRaxInputOutputAbi()`，只给单独测试模块使用，用来覆盖 input/output 都是 `RAX` 的 return-forward 场景。
- `tests/native_register_effects_test.cpp:223`
  - 新增 `createWeakCallInputFunction()`，覆盖无本地定义时的 `weak_entry_input`。
- `tests/native_register_effects_test.cpp:242`
  - 新增 `createBlockedCallInputFunction()`，覆盖前一个 call effect 作为输入来源时的 `blocked_call_effect`。
- `tests/native_register_effects_test.cpp:264`
  - 新增 `createStrongPhiCallInputFunction()`，覆盖两条 predecessor 都有本地定义时的 `strong_phi`。
- `tests/native_register_effects_test.cpp:307`
  - 新增 `createReturnForwardCallInputFunction()`，覆盖前一个 call return 继续作为下一个 call input 的 `return_forward`。
- `tests/native_register_effects_test.cpp:329`
  - 新增 `createMissingPhiIncomingModule()`，确认缺 incoming 的 PHI 应被 LLVM verifier 拒绝，而不是被 pass 补洞。
- `tests/native_register_effects_test.cpp:1511`
  - main 里新增对应断言。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target native_register_effects_test native_prototype_recovery_test notdec-native-llvm -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test
/tmp/notdec-bin2llvm-build/bin/native_prototype_recovery_test

/usr/bin/time -f 'TIME native-llvm-call-helper-semantics %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  --register-ssa-summary \
  -o /tmp/hexx64-1156e0-call-helper-semantics.ll \
  > /tmp/hexx64-1156e0-call-helper-semantics.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-call-helper-semantics.ll \
  -o /tmp/hexx64-1156e0-call-helper-semantics.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-call-helper-semantics.bc \
  -o /tmp/hexx64-1156e0-call-helper-semantics.verified.bc
```

结果：

- 两个单测通过。
- `hexx64.so -f 0x1156e0` 通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
- 本次时间：`72.97s`。
- summary 分布和上一轮一致：
  - `call input helpers: 4374`
  - `call return helpers: 929`
  - `call effect helpers: 3159`
  - `call input strong: 685`
  - `call input weak: 1199`
  - `call input blocked: 2490`

### 判断

- 实现效果：8/10。五类 strength 已有 focused 覆盖；PHI 缺 incoming 的策略也有 verifier 负例。
- 复杂度：3/10。只加测试构造，没有改主逻辑。
- 维护成本：3/10。后续调整 strength 规则时，测试会直接指出语义变化。

## 2026-06-13 实现记录：call input trial state 过渡层

本次开始把 call input 从 `strength` 迁到 Ghidra 风格 trial/use。先不改变 helper IR，也不删除 `strength`，而是在现有 candidate 上增加 `trial_state`，让 prototype recovery 优先消费 `active`。

### 改动

- `include/notdec-bin2llvm/passes/NativeRegisterSSA.h:39`
  - `NativeRegisterSSAFunctionSummary` 增加 `ActiveCallInputTrials`、`InactiveCallInputTrials`、`NoUseCallInputTrials`、`BlockedCallInputTrials`。
- `include/notdec-bin2llvm/passes/NativeRegisterSSA.h:65`
  - `NativeRegisterSSASummary` 增加同样的 module 级 trial state 计数。
- `lib/passes/NativeRegisterSSA.cpp:1115`
  - `annotateCallInputCandidateStrengths()` 在写 `strength` 后同步写 `trial_state`。
  - 当前映射：
    - `strong_local_def` / `strong_phi` -> `active`
    - `weak_entry_input` / `return_forward` -> `inactive`
    - `blocked_call_effect` -> `no_use`
- `lib/passes/NativeRegisterSSA.cpp:1233`
  - 新增 `callInputTrialState()`，集中保存这个过渡映射。
- `lib/passes/NativeRegisterSSA.cpp:1287`
  - 新增 `countCallInputTrialState()`，累计 trial state summary。
- `lib/passes/NativeRegisterSSA.cpp:1909`
  - `addFunctionSummary()` 汇总新增 trial state 计数。
- `lib/passes/NativeRegisterSSA.cpp:2013`
  - `printNativeRegisterSSASummary()` 打印 module/function 两级 trial state 统计。
- `lib/passes/NativePrototypeRecovery.cpp:927`
  - `declarationInputParamForCandidate()` 优先读取 `trial_state`。
  - 有 `trial_state` 时只接受 `active`；没有该字段时兼容旧 `strength=strong_local_def|strong_phi`。
- `lib/passes/NativePrototypeRecovery.cpp:1556`
  - `callInputCandidateValueBeforeCall()` 先检查当前 instruction 的 `notdec.register.call_input_candidate` metadata，再按普通 call barrier 处理。
  - 修复之前把 `notdec.register.call_input.*` helper call 提前跳过的问题，让 helper operand 能真正作为 declaration call rewrite 的参数值。
- `tests/native_register_effects_test.cpp:1533`
  - 增加 active/inactive/no-use summary 断言。
- `tests/native_register_effects_test.cpp:1569`
  - 增加 `trial_state=active/inactive/no_use` metadata 断言，覆盖 local def、entry input、call effect、PHI、return forward。
- `tests/native_prototype_recovery_test.cpp:1211`
  - 新增 `createCallInputHelperCallerFunction()`，构造只有 call input helper、没有 register store fallback 的 declaration call input 测试。
- `tests/native_prototype_recovery_test.cpp:7059`
  - 新增 active helper 正例：`trial_state=active` 时 declaration callee 被改成 `void(i64)`。
- `tests/native_prototype_recovery_test.cpp:7092`
  - 新增 inactive helper 负例：即使 metadata 里仍有 `strength=strong_local_def`，`trial_state=inactive` 也不能触发 signature rewrite。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target native_register_effects_test native_prototype_recovery_test notdec-native-llvm -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test
/tmp/notdec-bin2llvm-build/bin/native_prototype_recovery_test

/usr/bin/time -f 'TIME native-llvm-call-input-trial-helper %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  --register-ssa-summary \
  -o /tmp/hexx64-1156e0-call-input-trial-helper.ll \
  > /tmp/hexx64-1156e0-call-input-trial-helper.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-call-input-trial-helper.ll \
  -o /tmp/hexx64-1156e0-call-input-trial-helper.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-call-input-trial-helper.bc \
  -o /tmp/hexx64-1156e0-call-input-trial-helper.verified.bc
```

结果：

- 两个单测通过。
- `hexx64.so -f 0x1156e0` 通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
- 本次时间：`73.11s`。
- summary 输出：
  - `call input helpers: 4374`
  - `call return helpers: 929`
  - `call effect helpers: 3159`
  - `call input strong: 685`
  - `call input weak: 1199`
  - `call input blocked: 2490`
  - `call input trials active: 685`
  - `call input trials inactive: 1199`
  - `call input trials no use: 2490`
  - `call input trials blocked: 0`

### 判断

- 实现效果：7/10。prototype recovery 已从直接看 `strength` 迁到优先看 `trial_state=active`，但 trial-use 检查本身还是复用 strength 结果，还没做真正的 ancestor/use 分析。
- 复杂度：4/10。新增字段和兼容分支较少，IR helper 形态没有变化。
- 维护成本：4/10。后续要把 `callInputTrialState()` 从简单映射替换成真实 trial-use 检查；这次把入口和统计先固定下来。

## 2026-06-13 实现记录：call input trial annotation 收口

本次不改行为，只把 Register SSA 里 call input candidate 的标注入口从 strength 命名收口到 trial 命名。这样后续替换成真正 trial-use 检查时，改动点集中在 `callInputTrialInfo()`。

### 改动

- `lib/passes/NativeRegisterSSA.cpp:101`
  - 新增 `CallInputTrialInfo`，同时保存旧审计字段 `Strength` 和 prototype recovery 消费的 `State`。
- `lib/passes/NativeRegisterSSA.cpp:497`
  - `run()` 改调用 `annotateCallInputTrials()`。
- `lib/passes/NativeRegisterSSA.cpp:1123`
  - `annotateCallInputCandidateStrengths()` 改名为 `annotateCallInputTrials()`。
  - metadata 仍写 `strength` 和 `trial_state` 两个字段，行为不变。
- `lib/passes/NativeRegisterSSA.cpp:1203`
  - 新增 `callInputTrialInfo()`，集中从 value 生成 trial annotation。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target native_register_effects_test native_prototype_recovery_test notdec-native-llvm -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test
/tmp/notdec-bin2llvm-build/bin/native_prototype_recovery_test

/usr/bin/time -f 'TIME native-llvm-call-input-trial-info %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  --register-ssa-summary \
  -o /tmp/hexx64-1156e0-call-input-trial-info.ll \
  > /tmp/hexx64-1156e0-call-input-trial-info.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-call-input-trial-info.ll \
  -o /tmp/hexx64-1156e0-call-input-trial-info.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-call-input-trial-info.bc \
  -o /tmp/hexx64-1156e0-call-input-trial-info.verified.bc
```

结果：

- 两个单测通过。
- `hexx64.so -f 0x1156e0` 通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
- 本次时间：`69.05s`。
- summary 分布不变：
  - `call input trials active: 685`
  - `call input trials inactive: 1199`
  - `call input trials no use: 2490`
  - `call input trials blocked: 0`

### 判断

- 实现效果：5/10。只是把入口整理成 trial info，方便继续替换判定逻辑。
- 复杂度：2/10。新增一个小结构和一次函数改名。
- 维护成本：2/10。行为不变，风险主要是后续真实 trial-use 规则仍未落地。

## 2026-06-13 实现记录：call input trial reason metadata

本次继续补齐 trial metadata。`trial_state` 只说最终状态，不说明为什么；新增 `trial_reason`，让后续对照 Ghidra active/inactive/no-use 时不用反查旧 `strength`。

### 改动

- `lib/passes/NativeRegisterSSA.cpp:101`
  - `CallInputTrialInfo` 增加 `Reason` 字段。
- `lib/passes/NativeRegisterSSA.cpp:1140`
  - `annotateCallInputTrials()` 写入 `trial_reason` metadata。
- `lib/passes/NativeRegisterSSA.cpp:1206`
  - `callInputTrialInfo()` 同时生成 `Strength`、`State`、`Reason`。
- `lib/passes/NativeRegisterSSA.cpp:1263`
  - 新增 `callInputTrialReason()`：
    - `strong_local_def` -> `local_def`
    - `strong_phi` -> `phi`
    - `weak_entry_input` -> `entry_input`
    - `blocked_call_effect` -> `call_effect`
    - `return_forward` -> `return_forward`
- `tests/native_register_effects_test.cpp:1572`
  - 增加 local def、entry input、call effect、PHI、return forward 的 `trial_reason` 断言。
- `tests/native_prototype_recovery_test.cpp:1230`
  - 手写 call input helper 测试 metadata 增加 `trial_reason=local_def`，保持测试 IR 和 Register SSA 输出一致。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target native_register_effects_test native_prototype_recovery_test notdec-native-llvm -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test
/tmp/notdec-bin2llvm-build/bin/native_prototype_recovery_test

/usr/bin/time -f 'TIME native-llvm-call-input-trial-reason %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  --register-ssa-summary \
  -o /tmp/hexx64-1156e0-call-input-trial-reason.ll \
  > /tmp/hexx64-1156e0-call-input-trial-reason.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-call-input-trial-reason.ll \
  -o /tmp/hexx64-1156e0-call-input-trial-reason.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-call-input-trial-reason.bc \
  -o /tmp/hexx64-1156e0-call-input-trial-reason.verified.bc
```

结果：

- 两个单测通过。
- `hexx64.so -f 0x1156e0` 通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
- 本次时间：`69.34s`。
- summary 分布不变：
  - `call input trials active: 685`
  - `call input trials inactive: 1199`
  - `call input trials no use: 2490`
  - `call input trials blocked: 0`

### 判断

- 实现效果：5/10。trial metadata 更完整，但还没有改变 trial-use 判定逻辑。
- 复杂度：2/10。只增加一个解释字段和测试断言。
- 维护成本：2/10。后续真实 use 检查可以继续复用这个字段记录原因。

## 2026-06-13 实现记录：call input trial reason summary

`trial_reason` 已经写进 metadata，但 summary 只能看 state 分布。为了后续和 Ghidra/Java 链路对照，补 reason 计数，直接看 active 里有多少来自 local def / PHI，inactive 里有多少来自 entry input / return forward。

### 改动

- `include/notdec-bin2llvm/passes/NativeRegisterSSA.h:43`
  - `NativeRegisterSSAFunctionSummary` 增加：
    - `LocalDefCallInputTrials`
    - `PhiCallInputTrials`
    - `EntryInputCallInputTrials`
    - `CallEffectCallInputTrials`
    - `ReturnForwardCallInputTrials`
- `include/notdec-bin2llvm/passes/NativeRegisterSSA.h:74`
  - `NativeRegisterSSASummary` 增加同样的 module 级 reason 计数。
- `lib/passes/NativeRegisterSSA.cpp:1150`
  - `annotateCallInputTrials()` 在统计 state 后同步统计 reason。
- `lib/passes/NativeRegisterSSA.cpp:1339`
  - 新增 `countCallInputTrialReason()`。
- `lib/passes/NativeRegisterSSA.cpp:1989`
  - `addFunctionSummary()` 汇总 reason 计数。
- `lib/passes/NativeRegisterSSA.cpp:2108`
  - `printNativeRegisterSSASummary()` 打印 module/function 两级 reason 计数。
- `tests/native_register_effects_test.cpp:1539`
  - 增加 local-def、phi、entry-input、call-effect reason summary 断言。
- `tests/native_register_effects_test.cpp:1764`
  - 增加 return-forward reason summary 断言。

### 验证

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target native_register_effects_test native_prototype_recovery_test notdec-native-llvm -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test
/tmp/notdec-bin2llvm-build/bin/native_prototype_recovery_test

/usr/bin/time -f 'TIME native-llvm-call-input-trial-reason-summary %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  --register-ssa-summary \
  -o /tmp/hexx64-1156e0-call-input-trial-reason-summary.ll \
  > /tmp/hexx64-1156e0-call-input-trial-reason-summary.log 2>&1

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-call-input-trial-reason-summary.ll \
  -o /tmp/hexx64-1156e0-call-input-trial-reason-summary.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-call-input-trial-reason-summary.bc \
  -o /tmp/hexx64-1156e0-call-input-trial-reason-summary.verified.bc
```

结果：

- 两个单测通过。
- `hexx64.so -f 0x1156e0` 通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
- 本次时间：`69.85s`。
- summary 输出：
  - `call input trials active: 685`
  - `call input trials inactive: 1199`
  - `call input trials no use: 2490`
  - `call input trials blocked: 0`
  - `call input trial reasons local def: 665`
  - `call input trial reasons phi: 20`
  - `call input trial reasons entry input: 801`
  - `call input trial reasons call effect: 2490`
  - `call input trial reasons return forward: 398`

### 判断

- 实现效果：5/10。summary 能直接看 reason 分布，但 trial-use 规则仍未改变。
- 复杂度：3/10。新增 5 个计数字段和打印项，结构直接。
- 维护成本：3/10。字段数量增加，但这是后续对照 Ghidra 需要的可观测性。
