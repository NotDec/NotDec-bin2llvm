# Ghidra-style register SSA and call effect plan

用户原始要求：

> 综合考虑Ghidra的方案，以及SSA构建参考的那个参考实现，就按照Ghidra这个思路，总结一下如果要做好这一块应该怎么做，将规划总结到文档。

## 背景

当前 native register SSA 已经能把大部分 register load/store 改成 SSA 值，也已经修掉了 PHI incoming 不全导致 LLVM verifier 不过的问题。

但现在仍有一个更本质的问题：call 对寄存器的影响还没有完整变成数据流。之前的实现里，call clobber 很容易表现成 `nullptr`、裸 `undef`，或者 pass 末尾补洞。这些只能保证 IR 结构合法，不能保证后续 prototype recovery 能知道值从哪里来。

要把这一块做好，应该按 Ghidra 的思路处理：

- call 不是“数据流断点”，而是会产生明确 effect 的 op。
- preserved storage 继续沿用 call 前值。
- return storage 变成 call output。
- killed-by-call storage 变成 call 创建的新未知值。
- unknown effect 也要显式建值，不能悄悄丢语义。
- SSA 构建要按 Braun lazy SSA 的方式，保证临时 PHI 最终补齐所有 predecessor incoming。

## 当前目标和已有 native 状态

当前目标是服务 Bench2 真实 ELF / shared object 的 native 路线，尤其是 `/sn640/NotDec-Exp/Bench2/hexx64.so` 这类目标，生成的 LLVM IR 不只是能被 `llvm-as` 接受，还要让寄存器语义能继续被 prototype recovery 使用。

已有状态：

- `NativeRegisterSSA` 已经按 register backing global 做 SSA，能处理完整寄存器和部分 partial access。
- 现在的 SSA 形态接近 Braun lazy SSA：从使用点按需往前找 reaching definition，必要时在 join block 创建 PHI。
- pending PHI 已经有 pass-end finalize，当前可以保证最终 PHI incoming 数量匹配 LLVM CFG。
- call effect 已经初步显式化：
  - ABI output register 生成 `kind=return`。
  - killed-by-call register 生成 `kind=clobber_unknown`。
  - 其它不确定 call effect 生成 `kind=unknown_effect`。
  - 这些值用 `notdec.register.call_effect` metadata 标记。

还没有完成的关键点：

- call input candidate 还没有在 register SSA 阶段稳定记录。
- direct callee 的 recovered prototype 还没有完整反向精化 caller 侧 call effect。
- PHI finalize 还偏“补齐结构”，不完全等价于 Braun 的 `addPhiOperands + tryRemoveTrivialPhi`。
- callsite 还缺稳定 id。LLVM metadata 不能直接引用 local call instruction，所以现在只能记录 `call_block` / `callee` 字符串。

## Ghidra 相关实现

Ghidra 的核心思路在 heritage 阶段：先给 CALL/CALLIND 加 guard，把 call 对 storage 的影响变成数据流节点，然后再做 renaming。

关键源码：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh:391`
  - `EffectRecord` 定义 call effect 类型：
    - `unaffected`
    - `killedbycall`
    - `return_address`
    - `unknown_effect`

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc:4234`
  - `FuncProto::hasEffect(addr, size)` 查询某个 storage 经过 call 后的 effect。
  - 如果具体 function prototype 没有 effect list，就回退到 prototype model。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc:1443`
  - `Heritage::guardCalls(...)` 遍历当前函数的 callsite。
  - 对每个 storage range 判断它在 call 前后是什么关系。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc:1467`
  - `fc->hasEffect(transAddr, size)` 是 call effect 判定入口。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc:1495`
  - 如果 call input active，并且 storage 可能是参数，Ghidra 会注册 input trial，并把对应 varnode 插到 CALL 输入里。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc:1510`
  - `unaffected` 不加 guard。
  - `unknown_effect` / `return_address` 加普通 `INDIRECT`。
  - `killedbycall` 用 `newIndirectCreation(...)`，表示 call 创建了新值。

- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec:92`
  - `<output>` 定义 ABI 返回 storage，比如 `RAX`、`RDX`、`XMM0_Qa`、`XMM1_Qa`。

- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec:114`
  - `<killedbycall>` 定义默认 killed-by-call storage。

- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec:119`
  - `<unaffected>` 定义 callee-saved / unaffected storage。

Ghidra 给 native 侧的直接启发：

- 不要把 call clobber 当成查找失败。
- 不要把未知值只写成裸 `undef`。
- input / output trial 和 call effect 是同一个 callsite 语义的一部分。
- ABI model 是默认值，具体 callee prototype 可以覆盖默认值。

## Braun SSA 参考对应

参考文档：

- `logs/20260529-01-native-prototype-recovery-pass/08-register-elimination/20260610-02-braun-ssa-reference.md`

Braun lazy SSA 的关键规则：

- `readVariable` 先查当前 block 已知定义。
- 查不到就递归 predecessor。
- 遇到 loop / join 时先创建 incomplete PHI。
- block sealed 后，必须给 incomplete PHI 补齐所有 predecessor incoming。
- 补齐后做 trivial PHI 删除。

当前代码对应关系：

- `variable`
  - 对应 `RegisterUnit` / backing register global。

- `readVariable(variable, block)`
  - 对应 `readRegister(block, unit, before)`。

- `readVariableRecursive(variable, block)`
  - 对应 `readBlockEntry(block, unit)`。

- predecessor value
  - 对应 `readBlockExit(pred, unit)`。

- incomplete PHI
  - 当前是 `PendingPhi[(block, register)]`。

- `sealBlock + addPhiOperands`
  - 当前没有动态 seal，因为 LLVM CFG 已经完整。
  - 等价位置应该是 pass-end `finalizePendingPhis()`。

当前最大偏差：

- `PendingPhi` 现在更像 PHI cache，不是完整的 incomplete PHI 状态机。
- `finalizePendingPhis()` 应该做“重新按 predecessor 查 exit value 并补 incoming”，而不是只发现缺边后补一个 edge unknown。
- trivial PHI 删除应该同步更新 `EntryValue` / `ExitValue` / `Replacement`，并递归处理受影响 PHI。

## 目标设计

目标是把 register SSA 改成下面这个形态：

```text
机器寄存器 load/store
  -> register storage SSA
  -> call effect guard value
  -> PHI merge
  -> prototype recovery 精化 input/output
  -> 删除可证明无用的 register residue
```

其中 call effect guard value 是关键。它要表达“call 之后这个寄存器的值是什么来源”，而不是只表达“这里不知道”。

建议的 value 分类：

- `entry_input`
  - 函数入口已有的寄存器值。

- `local_def`
  - 当前函数内普通 register store 定义。

- `call_input_candidate`
  - call 前传给 callee 的可能参数值。

- `call_return`
  - call 产生的返回值。

- `call_clobber_unknown`
  - call 杀掉旧值，并创建一个与旧值无关的新未知值。

- `call_unknown_effect`
  - ABI / prototype 信息不足，只知道 call 可能影响这个 storage。

- `phi`
  - 控制流合流点。

这几个分类不一定都要变成不同 LLVM instruction，但 metadata 里必须能区分。

## native 侧要复刻的策略

### 1. 先算 call effect，不要先断流

当前 `localValueBefore(...)` 遇到 call 时已经可以返回 `callEffectValue(...)`。后续要把这条路做完整：

- `unaffected`：继续向 call 前查值。
- callee metadata 明确 preserves：继续向 call 前查值。
- ABI output 或 callee recovered return：生成 `kind=return`。
- killed-by-call：生成 `kind=clobber_unknown`。
- unknown：生成 `kind=unknown_effect`。

这对应 Ghidra `guardCalls()` 里先插 `INDIRECT` / `newIndirectCreation()`，再让 heritage rename 接着跑。

### 2. call input 要显式记录

Ghidra 会在 input active 时注册 trial，并把 varnode 插到 CALL 输入。

LLVM 侧短期不要急着改真实 call signature，先做稳定 metadata：

```text
notdec.register.call_input_candidate = {
  callsite_id=...
  slot=...
  register=RDI
  value_kind=...
}
```

候选来源：

- ABI input pentry。
- direct callee recovered prototype。
- call 前 `readRegister(block, unit, call)` 得到的 SSA value。

注意：metadata 不能直接保存 local SSA value 作为 global metadata operand。这里更稳的做法是：

- callsite candidate metadata 只保存 register / ABI slot / callsite id。
- 如果必须保留 value，创建一个轻量 helper intrinsic-like op 或 freeze/copy value，并把 metadata 挂在这个 local instruction 上。

### 3. call output 要从 ABI-level possible return 逐步精化

ABI output 只能说明“这个 storage 可以承载返回值”，不能说明每个函数实际都返回它。

短期：

- 外部 call / unknown callee：ABI output 先建 `kind=return`。
- direct internal callee：优先看 recovered return metadata。

后续：

- 如果 callee prototype 已确认只返回 `RAX`，caller 侧 `RDX` 不应继续当真实 return。
- 如果 call 后某 output 从未被使用，可以降级为无关 clobber 或删除候选。

### 4. PHI finalize 要回到 Braun 语义

`finalizePendingPhis()` 应该成为 native 侧的 `seal all blocks`：

1. 遍历所有 pending PHI。
2. 对每个 CFG predecessor 检查是否已有 incoming。
3. 缺 incoming 时调用统一的 `readBlockExit(pred, unit)`。
4. 如果 exit value 来自 call，就生成对应 call effect value。
5. 只有找不到具体 callsite 时，才生成 `unknown_effect` edge value。
6. 补齐后执行 trivial PHI 删除。

最终要求：

- 没有 operandless PHI。
- 没有 incoming 数量少于 predecessor 数的 PHI。
- PHI incoming 不出现裸 `undef`。
- 每条 incoming 都能追到 entry input、local def、call effect 或明确 unknown edge。

### 5. trivial PHI 删除要维护缓存

当前 `simplifyPhi()` 删除 PHI 时只做 `replaceAllUsesWith` 和 `DeadPhis`。

后续要补：

- `EntryValue` 中指向旧 PHI 的项要替换成 simplified value。
- `ExitValue` 中指向旧 PHI 的项也要替换。
- `Replacement` 里如果有旧 load/store 指向 PHI，也要最终 resolve 到新 value。
- 其它 PHI 因为这个替换变平凡时，要递归简化。

这一步是为了避免 SSA cache 和最终 IR 不一致。

## 保守处理和暂时不做

保守处理：

- ABI cspec 里没有明确 `unaffected` 的寄存器，不要擅自当 preserved。
- ABI cspec 里没有明确 `killedbycall` 的寄存器，也不要硬编码成 clobber。可以先用 `unknown_effect`。
- direct callee metadata 不完整时，先用 ABI fallback。
- callsite id 先用稳定字符串，不把 local call instruction 塞进全局 metadata。

暂时不做：

- 不退回 slot fallback / mem2reg。
- 不把所有 unknown 都当函数入口参数。
- 不一上来重写所有 call signature。
- 不先处理 stack argument / stack output 的复杂精化。
- 不在 register SSA 里硬编码 x86-64 caller-saved 集合来覆盖 cspec。

## 阶段计划

### 阶段 1：补全 call effect value 模型

已有一部分已经完成：

- `return`
- `clobber_unknown`
- `unknown_effect`

下一步要补：

- 稳定 `callsite_id`。
- metadata 字段固定下来。
- 区分 ABI fallback 和 callee prototype 精化来源。

判断标准：

- Bench2 `hexx64.so -f 0x1156e0` 仍通过 LLVM 22 `llvm-as` / `opt -passes=verify`。
- register PHI incoming 没有裸 `undef`。
- 每个 call effect 都能追到 register、kind、callsite。

### 阶段 2：call input candidate

实现：

- 从 ABI input pentry 得到候选 register。
- 对每个 call 读取 call 前 SSA value。
- 给 callsite 或 helper instruction 挂 input candidate metadata。
- prototype recovery 先消费 metadata，不急着改所有 declaration type。

判断标准：

- 典型 `mov rdi, x; call f` 能记录 `RDI` input candidate。
- 如果 direct callee 后续确认有 `RDI` 参数，caller 侧能把该 SSA value 接到 call 参数。

### 阶段 3：callee prototype 反向精化 caller call effect

实现：

- direct internal callee 的 `notdec.register.preserves` / `clobbers` / recovered returns / recovered inputs 优先于 ABI default。
- declaration / indirect call 仍使用 ABI fallback。
- output register 先是 possible return，后续按 callee prototype 缩小。

判断标准：

- 已恢复 prototype 的 direct call，不再把非返回 output register 误标成真实 return。
- caller 和 callee 的 register metadata 语义一致。

### 阶段 4：重写 PHI finalize

实现：

- 把 `PendingPhi` 扩成带状态的数据结构。
- `finalizePendingPhis()` 等价于 `seal_all_blocks`。
- 补 incoming 时重新走 `readBlockExit`，而不是直接补 edge unknown。
- trivial PHI 删除递归更新 cache。

判断标准：

- verifier 通过只是底线。
- PHI incoming 来源可解释。
- 循环和多前驱 join 不依赖结构性补洞。

### 阶段 5：prototype recovery 消费显式数据流

实现：

- register input 从 `call_input_candidate` 来。
- return candidate 从 `kind=return` call effect 来。
- `clobber_unknown` 不参与 return 推断。
- `unknown_effect` 只作为保守阻断，不伪装成有意义参数。

判断标准：

- callsite input store residue 继续下降。
- return rewrite 不把 clobber 当返回值。
- Bench2 固定目标运行时间没有明显回退。

## 验证口径

每个阶段至少验证：

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target notdec-native-llvm native_register_effects_test -j2

/tmp/notdec-bin2llvm-build/bin/native_register_effects_test

/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 \
  -o /tmp/hexx64-1156e0.ll

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0.ll \
  -o /tmp/hexx64-1156e0.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0.bc \
  -o /tmp/hexx64-1156e0.verified.bc
```

涉及性能或 pipeline 行为时，再跑：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --decode-seed-limit 20 --summary-json /bin/ls
```

关注指标：

- `hexx64.so -f 0x1156e0` 总耗时。
- `notdec.register.call_effect` 数量和 kind 分布。
- register PHI 裸 `undef` 数量。
- callsite input store residue。
- declaration / direct call rewrite 数量。

## 风险

- ABI output 偏宽。比如 `RDX` 是 SysV output slot，但很多函数实际只返回 `RAX`。必须靠 callee prototype 和 usage 缩小。
- `unknown_effect` 数量可能较多。这是信息不足，不应该用硬编码 ABI 猜测掩盖。
- helper value 如果用 `freeze undef`，LLVM 优化可能合并或移动。后续如果要强绑定 callsite，需要改成更稳定的 helper intrinsic-like call。
- callsite id 如果只用 block name，不够稳定。要补 module/function 内唯一编号。
- stack 参数和 stack output 比 register 难很多，不能混在第一轮 register SSA 修复里。

## 不做什么

- 不把 `llvm-as` 通过当成语义正确。
- 不用裸 `undef` 补 PHI 语义洞。
- 不回退旧 slot 模式 + mem2reg。
- 不把 Ghidra cspec 没写的寄存器强行归类。
- 不在 register SSA 阶段做大而全的 signature rewrite。
