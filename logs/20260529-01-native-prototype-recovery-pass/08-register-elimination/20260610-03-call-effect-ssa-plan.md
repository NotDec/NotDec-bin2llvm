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
