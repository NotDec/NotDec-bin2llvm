# Call effect aware register SSA plan

用户原始要求：

> 综合考虑Ghidra的方案，以及SSA构建参考的那个参考实现，就按照Ghidra这个思路，总结一下如果要做好这一块应该怎么做，将规划总结到文档。

## 背景

当前 `NativeRegisterSSA` 已经修掉了 pending PHI incoming 不全的问题，但修法是 pass 结束补齐 missing incoming，未知边用 `undef`。这只能保证 IR 结构合法，不能完整表达 call 对寄存器值的影响。

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
- PHI incoming 缺失时优先来自明确 call effect value，而不是 fallback `undef`。
- prototype recovery 可以区分：
  - function entry input
  - call input
  - call return
  - call clobbered unknown

## 建议设计

### 1. 引入 call effect value

在 `NativeRegisterSSA` 内部新增一个创建 helper value 的接口，例如：

```text
callEffectValue(call, register, kind)
```

`kind` 至少包含：

- `return`
- `clobber_unknown`
- `unknown_effect`

实现形式先用 LLVM intrinsic-like helper declaration 或 freeze/undef 包装都可以，但必须带 metadata。

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

LLVM 侧可以先不改真实 call signature，但要记录 metadata：

```text
notdec.register.call_input_candidate
```

触发条件：

- call 前某个 ABI input register 的值被读取，并且该 call 的 prototype 可能使用它。
- 或者 prototype recovery 已经认为 callee 需要这个 input。

短期可以先只记录 direct use：

- 对每个 call，查 ABI input pentries。
- 对这些 register 调 `readRegister(block, unit, call)`。
- 如果值非空，记录 call input candidate。

### 4. 明确 call output candidate

Ghidra 对 possible output 会 register trial；如果 output storage 需要 guard，会设置 effect。

LLVM 侧：

- 对 ABI output register 创建 call return value。
- 如果后续该 return register 被使用，prototype recovery 可以把它当 call output candidate。
- 如果 callee 是 direct known function，并且已有 recovered return metadata，则按 callee metadata 精化。

### 5. 重整 pending PHI

保留当前 pass-end finalize，但把它从“补 `undef`”升级成：

1. 对每个 missing incoming edge，重新查询 edge predecessor 的 exit value。
2. 如果失败是 call effect，生成 call effect value。
3. 只有真正无法定位来源时，才用 `unknown_effect` value。
4. 最后再 `tryRemoveTrivialPhi`。

这更接近 Braun 的 `addPhiOperands`，也更接近 Ghidra 的 explicit indirect effect。

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
- pending PHI missing incoming 不再补裸 `undef`，改补 `clobber_unknown` 或 `unknown_effect`。

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

### 阶段 3：显式化 call input

- 对 ABI input pentry 读取 call 前值。
- 给 callsite 写 input candidate metadata。
- prototype recovery 消费这些 callsite candidates。

判断标准：

- 典型 `mov rdi, x; call f` 能在 callsite metadata 里看到 `RDI=x`。

### 阶段 4：补完整 lazy SSA finalize

- `PendingPhi` 改成状态对象。
- finalize 使用统一 `addPhiOperands`。
- trivial PHI 删除递归更新缓存。

判断标准：

- 不再需要结构性 fallback。
- 每个 PHI 都能追到 entry input、local def、call return、call clobber unknown 或 unknown effect。

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
