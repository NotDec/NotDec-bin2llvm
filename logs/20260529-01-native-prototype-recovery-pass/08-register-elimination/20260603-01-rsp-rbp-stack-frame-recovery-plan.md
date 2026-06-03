# 原始 prompt

```text
先尝试解决RSP RBP的问题吧。目前我的思路是模仿那边wasm对全局栈指针识别，把栈空间分配转换为开头的alloca，把动态的栈指针调整转换为函数内的动态大小alloca。另外，调研一下Ghidra是怎么做的。然后参考两个的实现，规划一下具体怎么做
```

# 背景

当前固定三目标的寄存器残留已经不是 partial GPR 为主。前面几轮已经处理了 flags、RIP、partial SSA、部分 vector RMW、部分 callsite input store。剩下的大头是：

- `RSP`：入口 external input、栈调整 store、return path restore、call 附近压返回地址/栈调整。
- `RBP`：frame pointer 建立和恢复、callee-saved 保存恢复、少量真实 frame-relative 访问。
- `RBX/R12-R15`：callee-saved 保存恢复，和 `RBP` 有相同的 preserve 语义问题。

这里不能把 `RSP/RBP` 当普通寄存器直接消掉。`RSP` 同时表示栈地址空间的 base、call 边界的 stack effect、函数内动态 alloca；`RBP` 有时是 frame pointer，有时只是普通 callee-saved register。

目标是把能证明为函数本地栈帧的 `RSP/RBP` 流量转成 `alloca` / `notdec_stack`，把只用于 prologue/epilogue 保存恢复的寄存器 store 清掉。不能为了减少 residue 破坏栈参数、返回地址、callee-saved preserve 和栈地址逃逸语义。

# 当前 native 状态

bin2llvm 已经有两套相关基础：

1. `HeritageToLLVM` 已有 `notdec_stack`
   - 文件：`lib/HeritageToLLVM.cpp`
   - `StackFrame` 记录当前函数 stack varnode 覆盖范围。
   - `createStackFrame()` 扫描 Ghidra heritage 输出里的 `space=stack` varnode，在入口创建 byte-addressed `notdec_stack`。
   - `pointerForStackVarnode()` 把 stack varnode 映射到 `notdec_stack` 的 GEP。
   - `readAddressTiedInput()` 给正 offset stack input load 写 `notdec.stack.input` metadata。

2. `NativeRegisterSSA` 已把 ABI stack pointer 从普通 call clobber 里拿出来
   - 文件：`lib/passes/NativeRegisterSSA.cpp`
   - `AbiRegisterEffects::StackPointerRegister` 来自 cspec 的 `stackpointer.register`。
   - `FunctionPromoter::callClobbersRegister()` 对 ABI stack pointer 返回 false，避免普通 call 后错误重载 `RSP`。

所以这次不应该重做一套完全独立的 stack object 模型。优先复用 `notdec_stack`；只有 raw `RSP/RBP` 地址还没进入 Ghidra stack varnode 时，才补一个 native 侧的栈帧识别。

# wasm StackAlloca 参考

主 NotDec 里 wasm 的做法在：

- `/sn640/NotDec/src/Passes/StackPointerFinder.cpp`
  - `StackPointerFinderAnalysis::run()` 优先按 `__stack_pointer` / `env.__stack_pointer` 名字找全局栈指针。
  - 如果名字不够，再按入口块里的 `load sp; add/sub; store sp` 模式投票。
- `/sn640/NotDec/src/Passes/StackAlloca.cpp`
  - `LinearAllocationRecovery::run()` 识别入口栈分配和出口 restore。
  - 固定大小栈分配转成入口 `alloca`。
  - `matchDynamicAllocas()` 识别函数内 `store (sp +/- size), @__stack_pointer`，转成动态大小 `alloca`。
  - 目前主要替换 SP load/store 和相关整数值，动态 alloca 用 `ptrtoint` 接回旧的整数地址流。

可以借鉴的点：

- 先用很窄的入口/出口模式确认这是栈指针，不靠名字硬删。
- 固定栈空间转入口 alloca，动态调整转动态 alloca。
- restore 缺失但函数只有 `unreachable` 出口时可以保守接受。

不能直接照搬的点：

- native 的 `RSP` 不是普通 wasm global；call、return address、栈参数、red zone、stack realign 都混在一起。
- native 已有 Ghidra stack space 和 `notdec_stack`，不能再把同一批 stack varnode 复制成另一套 alloca。
- `RBP` 不能靠名字当第二个 stack pointer，必须先证明它是当前函数 frame base。

# Ghidra 做法

Ghidra 的关键不是“删除 RSP/RBP”，而是把栈建成独立的 spacebase 地址空间：

- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec`
  - `<stackpointer register="RSP" space="ram"/>` 声明正式 stack pointer。
  - `<returnaddress><varnode space="stack" offset="0" size="8"/></returnaddress>` 把返回地址放在 stack space。
  - prototype 使用 `stackshift="8"` / `extrapop="8"` 描述 call 边界的栈变化。
  - `<localrange>` 区分可当本地变量的 stack 范围。
  - `<unaffected>` 包含 `RBX/RBP/RSP/R12-R15`，用于 preserve 语义。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/architecture.cc`
  - `Architecture::decodeStackPointer()` 解析 cspec 的 `<stackpointer>`。
  - `Architecture::addSpacebase()` 创建 formal `stack` space。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/ruleaction.cc`
  - `RuleLoadVarnode::checkSpacebase()` 识别 `spacebase + constant` 的 LOAD/STORE。
  - `RuleLoadVarnode::applyOp()` / `RuleStoreVarnode::applyOp()` 把这类 LOAD/STORE 改成 stack space varnode COPY。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `StackSolver::build()` 跟踪 stack pointer 的 `INT_ADD`、`COPY`、`INDIRECT`、`MULTIEQUAL`、`INT_AND`。
  - `ActionStackPtrFlow::analyzeExtraPop()` 推导 unknown call 的 stack effect。
  - `ActionExtraPopSetup::apply()` 在 call 附近插入 stack pointer effect。
  - `ActionFuncLink::funcLinkInput()` 对 stack 参数插入 stack-relative load 或 placeholder。
- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64.dwarf`
  - `RBP` 不是正式 stack pointer；DWARF 里通过 `<stack_frame register="RBP" offset="-8"/>` 帮助把 frame pointer 引用转成 stack 位置。

对 native 侧的启发：

- `RSP` 应该建成栈地址空间 base，不应该只是一个全局寄存器。
- call effect 要用 `stackshift/extrapop`，不能只看 register clobber。
- `RBP` 要当 frame base 处理，前提是证明它来自稳定的 `RSP` 派生值。
- 本地栈、栈参数、返回地址必须分范围处理，不能全塞进同一个 alloca。

# 规划

## 阶段 0：RSP/RBP 栈残留审计

先补一个审计小步，不改 IR。

统计每个函数：

- 是否已有 `notdec_stack`。
- 是否仍有 raw `RSP/RBP` 派生的 memory access。
- `RSP` 残留 store 是入口分配、return restore、call 前调整、call 后恢复，还是普通路径。
- `RBP` 是否满足 frame pointer 形态：入口由 `RSP` 派生，return path 恢复，函数内有 `RBP + const` 地址使用。
- `RSP/RBP` 的 external input 是否只用于栈帧计算和 return restore。
- call 附近是否存在显式返回地址 store，是否有后续读。
- 是否出现动态栈调整：`RSP = RSP - value`、`RSP = RSP + value`、`RSP = RSP & mask`。

判断标准：

- 固定三目标能列出 `RSP/RBP` residue 的前几类来源。
- 能区分“需要 alloca rewrite 的 raw stack memory access”和“只需要 cleanup 的保存恢复状态”。
- 如果大多数 `RSP/RBP` 只是 return path restore，下一步不要先做动态 alloca。

## 阶段 1：静态 RSP 栈帧 alloca

第一版只处理最保守的 x86-64 SysV 形态：

- 入口有唯一 `RSP.external_input`。
- 入口块内存在 `RSP = RSP0 - constant` 或等价 SSA 值。
- return path 把 `RSP` 恢复到 `RSP0` 或 ABI 允许的返回位置。
- 函数内 raw stack address 都能化成 `RSP0 + constant` 或 `RSP_after_alloc + constant`。
- 没有未知 call 改变 `RSP`，没有无法解释的 `RSP` PHI。

实现策略：

- 如果函数已有 `notdec_stack`，优先扩展或复用它，不新建第二个栈 alloca。
- 如果没有 `notdec_stack`，按负 offset 本地范围在入口创建 byte-addressed `notdec_stack.native`。
- 把能证明落在本地负 offset 范围的 raw stack LOAD/STORE 改成 `notdec_stack` GEP。
- 删除只用于建栈/还栈的 `RSP` store。
- 替换只用于地址计算的 `RSP` load。

暂不处理：

- 正 offset stack 参数和返回地址。
- 栈地址逃逸到外部 call。
- `RSP` 参与比较、哈希、整数返回等非地址用途。
- red zone。

判断标准：

- 小 IR 覆盖 `sub rsp, imm`、本地栈读写、return restore。
- 固定三目标 `stack_pointer` store/external input residue 明显下降。
- LLVM 22 assemble/verify 通过。
- 固定三目标耗时同口径无明显退化。

## 阶段 2：RBP frame base

只在能证明 `RBP` 是 frame pointer 时处理：

- 入口保存旧 `RBP`。
- 新 `RBP` 来自当前函数的 `RSP` 派生值。
- 函数内 `RBP + constant` 地址能映射到同一个 `notdec_stack` 坐标。
- return path 恢复旧 `RBP`。
- `RBP` 没有作为普通整数或普通寄存器值传出。

实现策略：

- 建立 `FrameBase = RSP0 - frameBaseDelta`。
- 把 `RBP + const` 转成 stack offset，再复用阶段 1 的 `notdec_stack` GEP。
- 如果旧 `RBP.external_input` 只用于保存恢复，且函数 preserve metadata 已能表达 `RBP` 不变，则删除旧输入链和 return restore store。

暂不处理：

- 没有 frame pointer 的函数。
- `RBP` 被当普通 callee-saved 临时寄存器使用。
- 多个 frame base 或中途改写 `RBP`。

判断标准：

- `frame_pointer` residue 下降。
- 不把 `RBX/R12-R15` 误当 frame base。
- 抽查 `RBP` 相关函数，栈偏移和 Ghidra stack varnode 方向一致。

## 阶段 3：动态栈调整转动态 alloca

在静态栈帧稳定后再做动态 alloca。

识别形态：

- `RSP_new = RSP_old - size`。
- 后续有配对 restore 或所有出口 unreachable。
- `RSP_new` 的用途只是在一段受控区域内做 stack address base。
- size 是整数 SSA value，未被奇怪修改。

实现策略：

- 在调整点创建 `alloca i8, size`。
- 对该动态区域内以 `RSP_new + const` 访问的内存，改成动态 alloca GEP。
- 对需要保留整数地址流的临时值，第一版可像 wasm 一样用 `ptrtoint` 接回旧 SSA，但只允许内部地址计算使用。
- restore store 删除前必须确认没有后续真实 `RSP` 读。

暂不处理：

- `RSP = (RSP - size) & -16` 这种 realign，先只审计。
- 动态 alloca 地址传给未知外部函数。
- 循环里反复调整栈。

判断标准：

- 小 IR 覆盖动态 `alloca(size)` 和 restore。
- 遇到 stack realign 明确跳过，不误改。

## 阶段 4：callee-saved return-path cleanup

这一步和 `RBP` 相关，但不要塞进 alloca pass。

规则：

- `RBX/RBP/R12-R15` return path restore 只有在函数 effect metadata 已表达 preserve，且函数内没有真实读该 restored value 时，才删除。
- caller 侧 current-value 查询必须相信 callee preserves，而不是依赖 callee body 真的 `store @RBP`。
- 这一步应放在 call-effect resolver 更稳定后做。

判断标准：

- `callee_saved_return_path` residue 下降。
- direct/indirect call 后 preserved register 传播不退化。

# 建议实现顺序

1. 先做阶段 0 审计。
2. 如果审计显示 raw stack memory access 很少，先做阶段 4 的 preserve cleanup，而不是 alloca。
3. 如果 raw `RSP` 本地栈访问仍多，做阶段 1 静态 RSP 栈帧。
4. 再做阶段 2 RBP frame base。
5. 最后做阶段 3 动态 alloca。

这个顺序比直接上动态 alloca 更稳。当前 residue 里很多 `RSP/RBP` 看起来是 return path 和保存恢复，不一定是缺少栈内存 alloca。

# 风险

- `alloca` 地址不是原始进程地址。只要地址不逃逸，替换是合理的；如果地址被传给未知 call 或参与非地址整数运算，必须跳过。
- 正 offset stack 可能是参数或返回地址，不能放进本地 alloca。
- `RBP` 可能只是普通 callee-saved register，不能靠名字处理。
- Ghidra heritage 已经生成的 stack varnode 不要重复建模。
- call 的 `stackshift/extrapop` 不准会让 `RSP` 跨 call 传播错误。
- setjmp/longjmp、手写汇编、栈切换、stack realign、red zone 都要先保守跳过。

# 不做什么

- 不把所有 `RSP/RBP` load/store 直接删掉。
- 不把 `RBP` 默认当 stack pointer。
- 不把正 offset stack 统一当本地变量。
- 不用 slot + mem2reg 兜底。
- 不在第一版处理复杂动态栈、stack realign、栈地址逃逸。
