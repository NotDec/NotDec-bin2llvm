# Native ABI And Prototype Recovery Plan

## 原始 prompt

接下来规划一下如何实现函数的参数识别，以及返回值的识别。首先需要的就是，基于PCode sleigh那边的架构定义，在模块层用metadata标注ABI信息，用作后续其他分析的基础参考。其次就是调研一下Ghidra是怎么做参数和返回值的恢复的。比如，对于这里未知call不知道是否会修改寄存器值的情况，对于有代码的本模块的函数，就按照分析的结果判断。（即可能要拓展一下当前的Pass，拓展覆盖一下对于saved register的分析，然后为每个函数标注clobber信息，即会修改的寄存器集合等其他需要的信息）对于没有代码的外部导入函数，就按照ABI来判断修改的寄存器集合和返回值放在哪。

## 当前目标和已有 native 状态

目标是给 native 链路补上参数、返回值和 call clobber 的基础信息。第一步不急着改函数签名，而是先把 ABI 事实和函数级寄存器效果标出来，给后续恢复 pass 用。

当前已有基础：

- `RegisterStorage` 会把 Sleigh register varnode 映射成 LLVM global，并加 `notdec.register` / `notdec.register.access` metadata。
- `NativeRegisterSSA` 已经能把完整 register unit 的 load 改成 SSA value，并给函数标注 `notdec.register.external_inputs`。
- 当前 call 处理很保守：普通 call / 间接 call 都按可能 clobber 处理，不区分本模块函数、外部导入函数和 ABI preserved register。

下一步需要补的不是“直接猜 C 参数”，而是先形成这些事实：

- 模块默认 ABI：参数寄存器、返回寄存器、callee-saved、caller-saved、栈参数起点、stackshift、extrapop。
- 每个函数实际读到的入口寄存器。
- 每个函数实际写出的返回寄存器候选。
- 每个函数实际 clobber 的寄存器集合。
- 每个函数是否保存并恢复了 ABI 要求 preserved 的寄存器。

## Ghidra 相关实现

Ghidra 的 ABI 信息来自 compiler spec。x86-64 SysV 的例子在：

- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec:34` 的 `<default_proto>`。
- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec:37` 的 `<input>` 定义参数位置，整数参数是 `RDI/RSI/RDX/RCX/R8/R9`，浮点参数是 `XMM0_Qa` 到 `XMM7_Qa`，栈参数从 `stack + 8` 开始。
- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec:92` 的 `<output>` 定义返回位置，整数返回主要是 `RAX/RDX`，浮点返回是 `XMM0_Qa/XMM1_Qa`。
- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec:114` 的 `<killedbycall>` 记录默认 call 会破坏的寄存器。
- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec:119` 的 `<unaffected>` 记录 ABI preserved register，如 `RBX/RSP/RBP/R12-R15`。

Decompiler 侧的核心类是 `ProtoModel` 和 `FuncProto`：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh:726` 说明 `ProtoModel` 表达 ABI，包括输入参数位置、返回值位置、函数副作用和栈行为。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh:787` 的 `deriveInputMap(...)` 用候选输入推导最终参数。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh:794` 的 `deriveOutputMap(...)` 用候选输出推导返回值。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc:4233` 的 `FuncProto::hasEffect(...)` 查询某个存储位置在 call 后是 preserved、killed，还是未知。

参数和返回值恢复的大致流程：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc:4707` 的 `ActionInputPrototype::apply(...)` 遍历 input varnode，把可能是参数的位置注册成 trial，再调用 `deriveInputMap(...)`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc:4765` 的 `ActionOutputPrototype::apply(...)` 从 `RETURN` op 的输入收集返回候选，再更新输出 prototype。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc:1467` 附近在 call 点按 `FuncCallSpecs::hasEffect(...)` 判断寄存器/存储是否被 call 影响。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc:1521` 对 `killedbycall` 创建新的 killed 输出，避免把 call 前的值错误传播到 call 后。

native 侧不用完整复刻 Ghidra 的 prototype engine。当前更实际的路线是：先把 cspec 里的 ABI 事实转成 LLVM module metadata，再用寄存器 SSA 的结果做函数级事实标注。

## native 侧复刻策略

### 1. 模块级 ABI metadata

先在 native lowering 输出的 module 上挂一个 ABI metadata，例如：

```llvm
!notdec.abi = !{!0}
!0 = !{
  !"arch=x86_64",
  !"compiler=x86-64-gcc",
  !"default_proto=__stdcall",
  !"stack_pointer=RSP",
  !"stackshift=8",
  !"extrapop=8",
  !input_regs,
  !output_regs,
  !killed_by_call,
  !unaffected
}
```

metadata 不需要一开始支持全部 cspec 规则。第一版只覆盖 Bench2 当前 x86-64 SysV 需要的事实：

- integer input registers：`RDI, RSI, RDX, RCX, R8, R9`
- float input registers：`XMM0_Qa` 到 `XMM7_Qa`
- integer output registers：`RAX, RDX`
- float output registers：`XMM0_Qa, XMM1_Qa`
- killed-by-call：先按 cspec 记录，再可补全常见 caller-saved 集合
- unaffected：`RBX, RSP, RBP, R12, R13, R14, R15`
- stack argument base：`stack + 8`

实现上不引入新的 XML 依赖。当前 `lib/SleighLift.cpp` 已经用 Ghidra C++ 的 `DocumentStorage` / `XmlDecode` 读取 `.sla` 和 `.pspec`，这套逻辑可以直接复用来读取 `.cspec`。

第一版建议实现一个轻量 cspec 子集解析器，而不是硬编码 SysV：

- 复用 `findSleighSpecPath(...)` / `DocumentStorage::openDocument(...)` 定位和读取 cspec。
- 只解析 `compiler_spec` 下和 ABI 相关的元素：`stackpointer`、`returnaddress`、`default_proto`、`prototype`、`input`、`output`、`pentry`、`register`、`addr`、`unaffected`、`killedbycall`。
- 暂时跳过完整 protorule 类型系统，只保留 register/stack storage 顺序和 min/max size。
- 把解析结果转成 native 自己的轻量 `NativeAbiSpec`，再写入 `!notdec.abi` metadata。

Ghidra C++ decompiler 里也有 `ProtoModel` / `FuncProto` 的完整 decode 逻辑，但直接复用会把 `Architecture`、`AddrSpace`、`TypeFactory`、prototype rule engine 一起带进来，初期成本偏高。更稳的做法是复用 Ghidra 的 XML 基础设施，先解析我们需要的 cspec 子集；等后面要完整支持 hidden return、join、复杂类型规则时，再评估接入完整 `ProtoModel`。

### 2. 扩展 `NativeRegisterSSA` 的 call 处理

当前 pass 把普通 call 当作全寄存器屏障。下一步改成三类：

- 本模块有代码的 direct call：读取 callee 的函数级 metadata，用 callee 实际 clobber 集合判断哪些寄存器被破坏。
- 外部导入函数：按 module ABI metadata 判断 killed-by-call 和返回寄存器。
- 间接 call 或未解析 call：仍保守，按 ABI caller-saved/killed-by-call 处理，无法判断的寄存器不要传播。

这里需要一个小的 fixpoint：

1. 每个函数先用当前保守规则跑出 `external_inputs` 和候选 writes。
2. 标注函数实际 `clobbers` / `preserves` / `return_candidates`。
3. 再用这些信息重跑 register SSA，让本模块 direct call 后的寄存器传播更准。

第一版可以只跑两轮，不做复杂 worklist。

### 3. saved register / preserved register 标注

对 ABI `unaffected` 集合里的寄存器做函数级检查：

- 如果函数入口读了 `RBX`，中间写过 `RBX`，所有 return 前的 `RBX` 都等于入口值，则标注 preserved。
- 如果某个 preserved register 被写过，但不是所有返回路径都恢复成入口值，则标注 clobbered 或 unknown。
- 如果函数从未写这个寄存器，则也可标注 preserved-by-no-write。

这一版不需要识别具体 `push/pop` 或栈槽。基于 SSA 值等价先做更稳：

- 入口值就是 `notdec.register.external_input`。
- return 前值用 `readBlockExit(returnBlock, reg)` 或等价查询得到。
- 所有 return 前值都等于入口值，才算恢复。

栈保存/恢复模式可以作为调试 metadata 补充，但不要依赖它判断正确性。

### 4. 参数识别

参数识别先只基于 register input，不改函数签名：

- 从 `notdec.register.external_inputs` 取函数入口使用过的寄存器。
- 和 ABI input register 顺序相交，得到 `notdec.prototype.input_candidates`。
- 如果某个入口寄存器只是被保存恢复，没有参与真实计算，则不要算参数。
- 栈参数先只标注“未实现”，不要从 RAM load 里硬猜。

候选参数要带理由：

- `source=external_input`
- `abi_slot=0/1/...`
- `register=RDI`
- `used_by_non_preserve_flow=true`

判断标准不是“和 C 原型完全一致”，而是先稳定地区分：

- 真正被函数体使用的入口参数寄存器。
- 只是为了 ABI preserve 被保存恢复的寄存器。
- call 之后来自 callee 返回值的寄存器。

### 5. 返回值识别

返回值识别也先不改函数签名：

- 找每个 return block 前 ABI output register 的值。
- 如果 output register 的值来自函数内计算、load 或本模块 call 返回，标成 `notdec.prototype.return_candidates`。
- 如果输出值等于入口 external input，且该寄存器是 ABI preserved，不当作返回值。
- 如果所有 return path 的同一个 output register 都有一致候选，则置信度高。
- 多返回寄存器先按 metadata 记录，不急着构造 aggregate return。

外部导入函数的返回值位置直接来自 ABI output metadata。本模块函数的返回值位置优先来自函数分析结果；没有结果时再退回 ABI 默认。

## 阶段计划

### 阶段 1：ABI metadata

新增一个 module 级 ABI metadata 生成点。先从 Ghidra `.cspec` 解析 x86-64 SysV 的 ABI 子集，覆盖 Bench2 当前目标。

判断标准：

- `notdec-native-llvm /bin/ls ...` 输出里有 `!notdec.abi`。
- metadata 能映射到已有 `notdec.register` global。
- `llvm-as` 和 `opt -passes=verify` 通过。

### 阶段 2：函数级寄存器效果

扩展 register SSA pass，输出：

- `notdec.register.clobbers`
- `notdec.register.preserves`
- `notdec.register.return_candidates`
- `notdec.prototype.input_candidates`

判断标准：

- `RBX/RBP/R12-R15` 这类 ABI preserved register 不会因为普通 call 被误当成 killed。
- 本模块 direct call 可以使用 callee metadata 约束 clobber。
- 外部 call 使用 ABI metadata 约束 clobber。

### 阶段 3：参数和返回候选稳定化

基于阶段 2 的结果，筛掉保存恢复噪声，给参数和返回值候选加置信度。

判断标准：

- libuv、vsftpd、memcached 至少抽 20 个函数，人工检查候选参数/返回值没有明显把 saved register 当参数。
- 所有 selected native module 通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。

### 阶段 4：后续再考虑改函数签名

只有 metadata 稳定后，才考虑把部分函数从 `void ()` 改成带参数和返回值的 LLVM 函数签名。

这个阶段暂不做，因为一旦改签名，就要同时重写 call site、外部声明、返回指令和 ABI 类型。

## 风险

- x86-64 SysV 的 cspec 里 `<killedbycall>` 只列了部分位置，真实 caller-saved 集合更大。第一版要明确区分“cspec 原文 metadata”和“native 扩展 ABI clobber metadata”。
- `RSP/RBP` 同时涉及栈框架，不适合和普通参数寄存器用同一套规则粗暴处理。
- 子寄存器和 XMM lane 仍然要保守。完整 register unit 之外不要强行判断参数或返回值。
- 本模块 direct call 的 clobber 信息需要 fixpoint。单轮分析容易过保守，两轮一般够用，但要保留 unknown 状态。
- 栈参数恢复需要 stack/RAM 分析，不能和 register 参数混在第一版里做。

## 不做什么

- 第一版不改 LLVM 函数签名。
- 第一版不恢复栈参数。
- 第一版不写完整 cspec XML parser。
- 第一版不做 C 类型恢复。
- 第一版不把 saved register 的栈槽识别作为正确性前提。

## 判断标准

本规划实现后应满足：

- module 有可检查的 ABI metadata。
- 每个 native 函数有寄存器输入、clobber、preserve、返回候选 metadata。
- 外部导入函数按 ABI 判断 clobber 和返回寄存器。
- 本模块 direct call 优先按 callee 分析结果判断 clobber。
- Bench2 selected native 全量 IR 仍通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
- 至少抽查 20 个函数，确认 saved register 不被误标成参数，明显返回寄存器能被标成返回候选。
