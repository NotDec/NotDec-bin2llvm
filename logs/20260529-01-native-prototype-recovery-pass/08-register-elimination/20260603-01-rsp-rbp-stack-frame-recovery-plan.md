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

# 2026-06-03 实现记录：RSP/RBP return-path 死 store 清理

背景：

- 两个小 Bench2 目标里，`RSP/RBP` 残留大多不是 partial access，而是 entry external input、return path store、before-call stack adjustment。
- 其中 return path 上的 `RSP/RBP` store 在后续没有同寄存器 load/call 时，只是在维护全局寄存器状态；栈地址计算本身已经使用 canonical SSA value，不需要再依赖 store 到 `@RSP/@RBP`。
- 这一步先做阶段 4 的安全子集：只删死的 stack pointer / frame pointer store，不做 alloca rewrite，不删 call 前 store。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2119)
  - 新增 `isFramePointerRegisterName()`，只把 `RBP/EBP/BP` 作为第一版 frame pointer 名字。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2124)
  - 新增 `stackFrameRegisterNames()`，从 ABI `StackPointerRegister` 取 stack pointer，并从 ABI unaffected 里取 frame pointer。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2142)
  - 新增 `eraseDeadStackFrameRegisterStores()`。
  - 只处理 rewrite 后已经 `already matches` 的函数。
  - store 必须有 `notdec.register.access`，且匹配 stack/frame register。
  - 复用已有 `storeIsDeadOnAllReturnPaths()`，要求 store 后所有路径到 return 或被同寄存器后续 store 覆盖，中间不能有普通 call 或同寄存器 load。
  - 删除 store 后对 store value 做 `RecursivelyDeleteTriviallyDeadInstructions()`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3530)
  - 在 signature rewrite 后置 cleanup 中，在 killed-by-call cleanup 之后、vector cleanup 之前调用。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:222)
  - 新增 `attachStackFramePreservedTestAbi()`，构造含 `RSP` stack pointer 和 `RBP` unaffected 的测试 ABI。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:1877)
  - 新增 `createPreservedStackFrameStoreFunction()`，构造 canonical store/store/ret 和 store/store/load/ret 形态。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:2604)
  - 新增 `attachRegisterEffectMetadata()`，用于测试里附加 register effect metadata。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8175)
  - 新增 RSP/RBP cleanup 正反例：死的 RSP/RBP store 应删除，后续有 RBP load 的 store 必须保留。

验证：

```bash
cmake --build build --target native_prototype_recovery_test notdec-native-llvm -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
git diff --check

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-rsp-rbp-small-gate2 \
  --target lighttpd:helper \
  --target php:extension-calendar

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-rsp-rbp-small-gate2/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-rsp-rbp-small-gate2/*.signature-rewrite.ll \
  > /tmp/notdec-rsp-rbp-small-details2.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- `git diff --check` 通过。
- Bench2 两个小目标通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `lighttpd:helper` | 3s | 3s |
| `php:extension-calendar` | 12s | 11s |

residue 对比：

```text
before:
gpr load  access          full/full  1
gpr load  external_input  full/full  135
gpr store access          full/full  248
other load access         full/full  8
other load external_input full/full  1

after:
gpr load  access          full/full  1
gpr load  external_input  full/full  135
gpr store access          full/full  158
other load access         full/full  8
other load external_input full/full  1
```

判断：

- 两个小目标上 GPR full store 少了 `90`，主要来自 RSP/RBP return-path store。
- entry external input 没变，因为剩余 `RSP/RBP` 仍被 stack address 计算和 before-call stack adjustment 使用。
- 剩余 `RSP/RBP` 主要是 `entry_external_input` 和 `before_call_path`，这需要后续静态 stack alloca / raw stack address rewrite，不能靠本轮死 store cleanup 删除。
- 耗时同量级，没有看到性能退化。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 4 | 两个小目标少 90 个 GPR store，但还没处理 entry input 和 call 前栈状态。 |
| 理解成本 | 2 | 复用已有 return-path liveness，新增规则只限 ABI stack pointer 和 x86 frame pointer。 |
| 维护成本 | 2 | 后续 alloca rewrite 可以继续复用这个 cleanup；如果扩展到其它架构，需要补 frame pointer 名字来源。 |

有没有更好的方案：

- 更完整方案仍然是阶段 1/2 的静态 stack alloca 和 RBP frame base rewrite。
- 本轮先删 return-path 死 store，是因为它基于 canonical IR 的本地 liveness，风险小，并且两个小目标能直接看到残留下降。

# 2026-06-03 实现记录：静态 RSP 栈内存 alloca 改写

背景：

- return-path 死 store 清理后，两个小 Bench2 目标的 GPR store 从 `248` 降到 `158`，但 `RSP.external_input` 仍有 `135` 个。
- 剩余 `RSP` 里有一部分是 canonical IR 中的 `RSP.external_input + 负常量 -> inttoptr -> load/store`。
- 这类地址如果没有逃逸，可以按 wasm `StackAlloca` 的方向先放进函数入口 alloca。它不再需要用原始进程地址表达，也能让死的 `RSP.external_input` 链被 DCE。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2125)
  - 新增 `hasExistingNotDecStackAlloca()`，已有 `notdec_stack*` alloca 的函数先跳过，避免和 Ghidra/Heritage 已建模的栈空间重复。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2156)
  - 新增 `externalInputLoadForRegister()`，只在函数里存在唯一 `RSP` external input load 时继续处理。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2188)
  - 新增 `stackOffsetFromBase()`，识别 canonical `add/sub` 常量偏移；当前只接受能折成固定 `int64_t` 的 offset。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2256)
  - 新增 `StaticStackMemoryAccess`，记录可改写的 load/store、原始 `inttoptr`、栈 offset 和访问大小。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2263)
  - 新增 `rewriteStaticStackMemoryAccesses()`。
  - 只处理 signature rewrite 后已经 `already matches` 的函数。
  - 只处理 `RSP.external_input + negative constant` 的 direct `inttoptr`。
  - 只接受 pointer 用户全是直接 `load/store`；传给 call、作为别的指针计算、正 offset、跨过 offset 0 的访问都跳过。
  - 按 `[low, high)` 建入口 `notdec_stack.native` alloca，把访问 pointer 改为 alloca 内 byte GEP。
  - 改写后删除无用 `inttoptr`，并让递归 DCE 清掉死的 `RSP.external_input` 链。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3769)
  - 在 signature rewrite cleanup 中，先做静态栈内存改写，再做 RSP/RBP return-path 死 store 清理。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:1907)
  - 新增 `createStaticRspStackMemoryFunction()`，构造非逃逸和逃逸两种 `RSP - 8` raw stack access。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:2791)
  - 新增 `hasRegisterExternalInputLoad()` / `hasIntToPtr()` / `hasAllocaNamed()` 测试辅助。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8302)
  - 新增静态 RSP 栈内存测试：非逃逸 case 应生成 `notdec_stack.native`，删除旧 `inttoptr` 和死 `RSP.external_input`；逃逸 case 必须保持原 IR。

验证：

```bash
cmake --build build --target native_prototype_recovery_test notdec-native-llvm -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
git diff --check

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-rsp-rbp-static-stack-gate \
  --target lighttpd:helper \
  --target php:extension-calendar

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-rsp-rbp-static-stack-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-rsp-rbp-static-stack-gate/*.signature-rewrite.ll \
  > /tmp/notdec-rsp-rbp-static-stack-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- `git diff --check` 通过。
- Bench2 两个小目标通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `lighttpd:helper` | 2s | 3s |
| `php:extension-calendar` | 11s | 12s |

residue 对比：

```text
after return-store cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  135
gpr store access          full/full  158
other load access         full/full  8
other load external_input full/full  1

after static RSP alloca:
gpr load  access          full/full  1
gpr load  external_input  full/full  128
gpr store access          full/full  158
other load access         full/full  8
other load external_input full/full  1
```

判断：

- 两个小目标上 `RSP` external input 残留少了 `7` 个；`notdec_stack.native` 在输出里出现 `238` 次，说明有一批 raw stack access 被替换成了 alloca/GEP。
- GPR store 数没变，符合预期：这轮只改写静态栈内存地址，不处理 before-call 的 `RSP` 状态写。
- 这不是完整 RSP/RBP 消除。剩余主要还是 `RSP/RBP` entry external input、before-call stack pointer、以及 RBP frame-base 相关访问。
- 耗时和上一轮同量级，没有看到性能退化。

剩余问题：

- `RBP` 还没建 frame base。下一步需要识别 `RBP = RSP + const` 或 entry 保存的 frame pointer，再把 RBP-based 负 offset raw stack access 并入同一个 alloca。
- `before_call_path` 上的 `RSP` 调整还没处理。这里可能对应 call frame / return address / outgoing stack args，不能直接删。
- 当前遇到任意逃逸 stack pointer 会整函数跳过，保守但漏掉 mixed safe/unsafe 场景。后续可以改成只跳过逃逸 pointer，不必跳过整函数。

当前两个小目标里 stack/frame residue 前几类：

```text
34 stack_pointer entry_external_input clobbers entry_external_input
30 frame_pointer entry_external_input clobbers entry_external_input
15 stack_pointer before_call_path clobbers stack_pointer
12 stack_pointer entry_external_input clobbers,external_inputs entry_external_input
8  stack_pointer before_call_path clobbers,external_inputs stack_pointer
4  frame_pointer entry_external_input clobbers,external_inputs entry_external_input
```

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | 能消掉一类确定的静态 RSP raw stack access，但两个小目标只减少 7 个 external input，还没处理 RBP 和 call 前栈状态。 |
| 理解成本 | 3 | 增加了 offset 匹配和 alloca 改写，但规则集中在一个 helper，限制条件明确。 |
| 维护成本 | 3 | 保守跳过能降低误改风险；后续扩展到 RBP/dynamic alloca 时需要复用这套 access 收集逻辑，避免重复。 |

有没有更好的方案：

- 更完整的路线仍然是先抽出通用的 stack base/value 识别，再同时支持 RSP、RBP 和动态 alloca。
- 本轮先做 RSP 静态负 offset，是因为它基于优化后的 canonical IR，语义边界清楚，能快速验证 alloca rewrite 对 Bench2 不破坏 assemble/verify。

# 2026-06-03 实现记录：清理未读取的静态栈保存槽

背景：

- 静态 RSP alloca 改写后，很多剩余 `RBP.external_input` 来自函数序言形态：把入口 `RBP` 保存到 `notdec_stack.native`，但后面没有从该槽读回。
- 这些保存槽地址不逃逸，且 alloca 是当前 pass 新建的本地对象；如果没有 overlapping load，这个 store 没有语义消费者。
- 先清掉这类 dead stack store，比直接把 `RBP` 当 frame base 更安全。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2263)
  - 新增 `stackRangesOverlap()`，用 byte range 判断两个静态栈访问是否重叠。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2270)
  - 新增 `staticStackStoreIsLoaded()`，只有存在 overlapping load 时才保留 store。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2285)
  - 新增 `eraseDeadStaticStackStores()`。
  - 删除没有 overlapping load 的 `notdec_stack.native` store。
  - store 删除后递归 DCE store pointer 和 store value，因此未使用的 `RBP.external_input` / `RSP.external_input` 会一起消失。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2427)
  - 在 `rewriteStaticStackMemoryAccesses()` 完成 pointer 改写后调用 dead stack store cleanup。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:1948)
  - 新增 `createStaticRspDeadStackSaveFunction()`，构造 `RSP - 8` 栈槽保存 `RBP.external_input` 但没有 load 的 case。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8341)
  - 新增断言：dead save case 不应保留 `notdec_stack.native`、旧 `inttoptr`、`RSP.external_input`、`RBP.external_input`。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-rsp-rbp-dead-stack-store-gate \
  --target lighttpd:helper \
  --target php:extension-calendar

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-rsp-rbp-dead-stack-store-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-rsp-rbp-dead-stack-store-gate/*.signature-rewrite.ll \
  > /tmp/notdec-rsp-rbp-dead-stack-store-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- Bench2 两个小目标通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `lighttpd:helper` | 3s | 2s |
| `php:extension-calendar` | 11s | 11s |

residue 对比：

```text
after static RSP alloca:
gpr load  access          full/full  1
gpr load  external_input  full/full  128
gpr store access          full/full  158
other load access         full/full  8
other load external_input full/full  1

after dead stack store cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  57
gpr store access          full/full  158
other load access         full/full  8
```

stack/frame residue 前几类：

```text
34 stack_pointer entry_external_input clobbers entry_external_input
15 stack_pointer before_call clobbers stack_pointer
12 stack_pointer entry_external_input clobbers,external_inputs entry_external_input
6  stack_pointer before_call clobbers,external_inputs stack_pointer
6  frame_pointer entry_external_input clobbers entry_external_input
3  frame_pointer entry_external_input clobbers,external_inputs entry_external_input
```

判断：

- GPR external input 从 `128` 降到 `57`，主要清掉了保存但没有读取的 `RBP` / callee-saved register stack save。
- `notdec_stack.native` 出现次数从 `238` 降到 `14`，说明很多 alloca 只服务于死保存槽，已被 DCE。
- GPR store 仍是 `158`，瓶颈已经转向 call 前 `RSP` 状态和真正被保留的寄存器 store。
- 耗时同量级，没有看到性能退化。

剩余问题：

- `RBP` frame-base raw memory access 仍未处理，剩下的 `RBP.external_input` 里还有 `RBP + negative offset -> inttoptr -> load`。
- `RSP` before-call store 仍较多，这需要区分 outgoing stack arg、return address 模拟和真正跨 call 的 stack pointer state。
- 当前 cleanup 只按静态 range 判断 load/store，不做 alias 推理；这是有意保守。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 4 | 两个小目标 external input 大幅下降，但仍没解决 call 前 RSP 和 RBP frame-base。 |
| 理解成本 | 2 | 只在当前 pass 新建的静态 alloca 访问集合里做 range 检查，规则简单。 |
| 维护成本 | 2 | 后续扩展 RBP/dynamic alloca 时可以复用同一 dead stack store cleanup。 |

有没有更好的方案：

- 如果以后统一建 stack object，应把这个 cleanup 放到统一 stack access 收集后执行。
- 当前先放在 RSP 静态改写里，是因为数据来源完整，地址不逃逸，风险低。

# 2026-06-03 实现记录：internal call 前 RSP store 安全清理

背景：

- dead stack store cleanup 后，剩余 `RSP` store 大多在 call 前。
- declaration call 仍可能代表未知 native 边界，不能直接删。
- internal call 可以做更窄判断：如果 callee 不读 `RSP`，store 到 call 之间没有 `RSP` 读/写/其它 call，并且 call 后到 return 或覆盖前也没有 `RSP` 读，这个 call 前 `RSP` store 就只是旧全局寄存器状态维护。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2002)
  - 新增 `storedRegisterValueIsDeadAfterCall()`，检查 call 后路径上是否没有同寄存器读，直到 return 或同寄存器覆盖。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2024)
  - 新增 `canEraseUnusedInternalCallStackPointerStore()`。
  - 要求 call 前局部 store 没有被 intervening instruction 使用。
  - 要求 callee recovered prototype 没有 `RSP` register input，且 callee body 不读 `RSP`。
  - 要求 call 后该 store value 不再被 caller 读取。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2163)
  - 新增 `eraseUnusedInternalCallStackPointerStores()`，只处理 ABI stack pointer，只处理非 declaration direct internal call。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3910)
  - 在 internal killed-input cleanup 后、静态 stack alloca rewrite 前调用。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:1976)
  - 新增 `createInternalRspStoreCallerFunction()`，构造 call 前 `RSP` store 的可删/不可删 case。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8414)
  - 新增三组测试：callee 不读且 caller 后续不读时删除；callee 读 `RSP` 时保留；caller call 后读 `RSP` 时保留。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-rsp-rbp-internal-call-sp-gate \
  --target lighttpd:helper \
  --target php:extension-calendar

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-rsp-rbp-internal-call-sp-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-rsp-rbp-internal-call-sp-gate/*.signature-rewrite.ll \
  > /tmp/notdec-rsp-rbp-internal-call-sp-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- Bench2 两个小目标通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `lighttpd:helper` | 3s | 3s |
| `php:extension-calendar` | 12s | 11s |

residue 汇总和上一轮相同：

```text
gpr load  access          full/full  1
gpr load  external_input  full/full  57
gpr store access          full/full  158
other load access         full/full  8
```

判断：

- 这两个小目标没有命中新规则，残留统计不变。
- 剩余两个 internal before-call/path 样本的 callee 会读 `RSP`，所以当前规则正确保留。
- declaration call 前的 `RSP` store 仍不处理；这需要更完整的 call stack effect / stack argument 语义。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 2 | 补上 internal direct-call 的安全子集，但当前两个小目标没有 residue 下降。 |
| 理解成本 | 2 | 和已有 internal killed-input cleanup 并列，条件集中在 call 前后局部 liveness。 |
| 维护成本 | 2 | 不碰 declaration call，不改变 RBP/alloca 逻辑，后续可扩展到 CFG 前驱等价值。 |

有没有更好的方案：

- 真正能消掉当前 before-call 大头的方案，是引入 Ghidra 式 `stackshift/extrapop` 和 stack argument 建模。
- 本轮只做 internal direct-call 的安全子集，是为了先避免把 declaration/native 边界语义弄错。

# 2026-06-03 实现记录：无用 raw RSP/RBP stack load 清理

背景：

- internal call 前 `RSP` store 清理后，两个小目标仍有 `57` 个 GPR external input，其中 `RSP` entry external input 是最大项。
- 抽查发现其中一部分只是 `RSP.external_input -> inttoptr -> load`，但 load 结果没有任何 use。
- 这类 load 不是 stack 参数，也不是 call frame 状态；非 volatile、非 atomic 且结果无用时，可以直接删掉，并递归 DCE 掉 `inttoptr` 和 `RSP.external_input` 链。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2403)
  - 新增 `eraseUnusedRawStackFrameLoads()`。
  - 只处理 signature rewrite 后已经 `already matches` 的函数。
  - base 必须是唯一 `RSP/RBP` external input load。
  - load 必须是非 volatile、非 atomic、无 use。
  - pointer 必须是 `inttoptr`，且地址能用 canonical `add/sub` 折成 base + 常量。
  - 删除 load 后递归 DCE pointer 链。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3964)
  - 在静态 stack alloca rewrite 后、RSP/RBP return-path store cleanup 前调用。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:1976)
  - 新增 `createRawRspLoadFunction()`，构造 unused / used raw `RSP` load。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8441)
  - 新增测试：unused raw `RSP` load 应删除 `inttoptr` 和死 `RSP.external_input`；used raw `RSP` load 必须保留。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-rsp-rbp-unused-raw-load-gate \
  --target lighttpd:helper \
  --target php:extension-calendar

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-rsp-rbp-unused-raw-load-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-rsp-rbp-unused-raw-load-gate/*.signature-rewrite.ll \
  > /tmp/notdec-rsp-rbp-unused-raw-load-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- Bench2 两个小目标通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `lighttpd:helper` | 3s | 3s |
| `php:extension-calendar` | 12s | 12s |

residue 对比：

```text
after internal call RSP cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  57
gpr store access          full/full  158
other load access         full/full  8

after unused raw stack load cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  45
gpr store access          full/full  158
other load access         full/full  8
```

stack/frame residue 前几类：

```text
26 stack_pointer entry_external_input clobbers entry_external_input
15 stack_pointer before_call clobbers stack_pointer
8  stack_pointer entry_external_input clobbers,external_inputs entry_external_input
6  stack_pointer before_call clobbers,external_inputs stack_pointer
6  frame_pointer entry_external_input clobbers entry_external_input
3  frame_pointer entry_external_input clobbers,external_inputs entry_external_input
```

判断：

- GPR external input 从 `57` 降到 `45`，主要来自 unused raw `RSP` return-address-like load。
- GPR store 仍是 `158`，说明剩余大头还是 call 前 `RSP` 状态和 declaration call 语义。
- 这轮没有处理 used raw load，也没有把 `RBP` entry frame slot 改成 alloca，避免把已有 frame 内容误当本地未初始化 alloca。
- 耗时同量级，没有看到性能退化。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | 两个小目标少 12 个 GPR external input，但 store 不变。 |
| 理解成本 | 2 | 规则只看 unused 普通 load 和 canonical stack/frame base 地址。 |
| 维护成本 | 2 | 不影响 used stack load、call frame、declaration call；后续可和 call stack effect 合并。 |

有没有更好的方案：

- 更完整方案仍然是把 call return address / stackshift 建模起来，而不是只删 unused load。
- 本轮先删 unused raw load，是因为它不需要解释 stack argument 或 call frame 语义。

# 2026-06-03 实现记录：无 stack input 的 declaration call 前 RSP store 清理

背景：

- unused raw load cleanup 后，GPR store 仍是 `158`，其中剩余大头是 declaration call 前的 `RSP` store。
- 不是所有 declaration call 都能删：未知声明可能依赖栈参数或真实栈指针状态。
- 但有一类已经比较明确：声明函数已经带 `notdec.prototype.recovered`，调用也已经改成显式寄存器参数，prototype 里没有 stack input。这时 call 前 `RSP` store 只是 lifted 全局寄存器状态维护；如果 caller 在 call 后也不读 `RSP`，可以删除。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2002)
  - 新增递归 CFG liveness helper：`reachesReturnOrOverwriteWithoutCallOrAccessLoadRecursive()` 和 `allSuccessorsReachReturnOrOverwriteWithoutCallOrAccessLoadRecursive()`。
  - 之前 call 后只看一层 successor；现在可以穿过中间 block，直到 return、同寄存器覆盖、同寄存器读或其它 call。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2056)
  - `storedRegisterValueIsDeadAfterCall()` 改用递归 CFG liveness。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2087)
  - 新增 `prototypeHasStackInput()`，识别 recovered prototype 是否有 stack storage input。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2097)
  - 新增 `canEraseUnusedDeclarationCallStackPointerStore()`。
  - 要求 callee 是非 vararg declaration，call arg 数和 callee arg 数一致。
  - callee 必须有 recovered prototype，且 input 数和 call arg 数一致。
  - prototype 不能包含 stack input，也不能包含 `RSP` register input。
  - call 前到 call 之间不能有 `RSP` 读/写/其它 call，call 后路径不能读 `RSP`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2238)
  - 新增 `eraseUnusedDeclarationCallStackPointerStores()`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:4086)
  - 在 internal killed-input cleanup 后、internal call RSP cleanup 前调用 declaration call RSP cleanup。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:2046)
  - 新增 `createDeclarationRspStoreCallerFunction()`。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8506)
  - 新增 declaration call RSP store 测试：有 recovered prototype 且 call 后不读时删除；call 后经分支到 return 也删除；callee 没 metadata 时保留；caller call 后读 `RSP` 时保留。

验证：

```bash
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-rsp-rbp-decl-call-sp-gate2 \
  --target lighttpd:helper \
  --target php:extension-calendar

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-rsp-rbp-decl-call-sp-gate2/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-rsp-rbp-decl-call-sp-gate2/*.signature-rewrite.ll \
  > /tmp/notdec-rsp-rbp-decl-call-sp-details2.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- Bench2 两个小目标通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `lighttpd:helper` | 2s | 3s |
| `php:extension-calendar` | 11s | 12s |

residue 对比：

```text
after unused raw stack load cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  45
gpr store access          full/full  158
other load access         full/full  8

after declaration call RSP cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  45
gpr store access          full/full  150
other load access         full/full  8
```

stack/frame residue 前几类：

```text
26 stack_pointer entry_external_input clobbers entry_external_input
8  stack_pointer entry_external_input clobbers,external_inputs entry_external_input
8  stack_pointer before_call clobbers stack_pointer
6  frame_pointer entry_external_input clobbers entry_external_input
5  stack_pointer before_call clobbers,external_inputs stack_pointer
3  frame_pointer entry_external_input clobbers,external_inputs entry_external_input
```

判断：

- 两个小目标上删除了 `8` 个 declaration call 前 `RSP` store，GPR store 从 `158` 降到 `150`。
- GPR external input 没变，说明这些 store 的 `RSP.external_input` 仍被其它栈地址或 call frame 语义使用。
- 剩余 declaration call 前 `RSP` store 主要是 callee 没有足够 prototype 信息、caller 后续路径仍读 `RSP`，或 call frame/stack input 语义还不明确。
- 耗时同量级，没有看到性能退化。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | 两个小目标少 8 个 GPR store，但剩余 call 栈语义仍未建模。 |
| 理解成本 | 3 | 增加递归 CFG liveness 和 declaration recovered prototype 条件。 |
| 维护成本 | 3 | 条件偏保守；后续引入 stackshift/stack input 后需要复核 declaration call 的 stack storage 判断。 |

有没有更好的方案：

- 最终仍应按 Ghidra 思路建 call stack effect，而不是只删 declaration call 前的 `RSP` store。
- 本轮先处理无 stack input 的声明，是因为它和现有 declaration call 参数 rewrite 的语义一致，风险可控。

# 2026-06-03 实现记录：call 前 frame register store 清理

背景：

- declaration call 前 `RSP` store 清理后，两个小目标还剩一个明显的 `RBP` before-call 样本：
  `notdec_native_3360` 在调用 `php_info_print_table_start()` 前把 `RSP - 8` 写进 `@RBP`。
- 这个 `RBP` 写不是栈参数，也不是 callee 需要的显式输入；call 后 caller 也不再读取 `RBP`。
- 但不能把规则直接扩大到所有寄存器。这里只处理 ABI stack pointer 和 x86 frame pointer 名字；`RSP` 仍要求 declaration callee 有 recovered prototype，避免未知栈参数/真实 stack pointer 语义被删。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:594)
  - 给 `isFramePointerRegisterName()` 和 `stackFrameRegisterNames()` 增加前置声明，供 call cleanup 逻辑复用。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2071)
  - 将 `canEraseUnusedInternalCallStackPointerStore()` 扩成 `canEraseUnusedInternalCallStackFrameRegisterStore()`。
  - internal direct call 前的 dead store 现在按 `stackFrameRegisterNames(abi)` 同时检查 `RSP/RBP`。
  - 仍要求 callee recovered prototype 不把该寄存器作为 input，callee body 不读该寄存器，caller call 后也不读。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2101)
  - 将 declaration call 前的规则扩成 `canEraseUnusedDeclarationCallStackFrameRegisterStore()`。
  - 对 `RSP`：仍要求 callee 有 recovered prototype，且无 stack input / 无 `RSP` input。
  - 对 frame pointer 名字：callee 没 recovered prototype 时也允许删，但必须确认 caller call 后不读该 frame register。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2246)
  - 将 declaration cleanup 从单一 stack pointer 改成遍历 `stackFrameRegisterNames(abi)`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2297)
  - 将 internal call cleanup 同样改成遍历 `stackFrameRegisterNames(abi)`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:4098)
  - signature rewrite cleanup pipeline 调用新的 stack/frame register cleanup。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:2003)
  - 将 internal call 前 store helper 参数化为 `createInternalStackFrameRegisterStoreCallerFunction()`。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:2050)
  - 将 declaration call 前 store helper 参数化为 `createDeclarationStackFrameRegisterStoreCallerFunction()`。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8512)
  - declaration call 测试补 `RBP` 正反例：无 callee metadata 的 `RBP` call 前 store 可删；无 callee metadata 的 `RSP` 仍保留；call 后读 `RBP` 时保留。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8609)
  - internal call 测试补 `RBP` 正反例：callee/caller 都不读时删除；callee 读或 caller call 后读时保留。

验证：

```bash
git diff --check
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-rsp-rbp-frame-call-store-gate2 \
  --target lighttpd:helper \
  --target php:extension-calendar

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-rsp-rbp-frame-call-store-gate2/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-rsp-rbp-frame-call-store-gate2/*.signature-rewrite.ll \
  > /tmp/notdec-rsp-rbp-frame-call-store-details2.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- Bench2 两个小目标通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `lighttpd:helper` | 3s | 3s |
| `php:extension-calendar` | 12s | 12s |

residue 对比：

```text
after declaration call RSP cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  45
gpr store access          full/full  150
other load access         full/full  8

after frame register call-store cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  45
gpr store access          full/full  149
other load access         full/full  8
```

抽查：

```llvm
define { i64, i64 } @notdec_native_3360() {
bb_3364:
  %0 = add i64 %RSP.external_input, -8
  %1 = add i64 %0, -8
  store i64 %1, ptr @RSP, align 4, !notdec.register.access !91
  call void @php_info_print_table_start()
  ...
}
```

判断：

- 两个小目标只少了 `1` 个 GPR store，命中的是 `php_info_print_table_start()` 前的 `RBP` store。
- `RSP` before-call store 没有继续放宽。剩余 `RSP` 仍要等 call `stackshift/extrapop`、返回地址、outgoing stack arg 建模，不能用 frame register cleanup 硬删。
- 剩余 `RBP.external_input` 多数是 `RBP + negative offset -> inttoptr -> load`。这些更像 frame-relative 访问，不能仅凭 `RBP` 名字改成本地 alloca；要先证明当前函数 frame base。
- 耗时同量级，没有看到性能退化。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 2 | 真实小目标只少 1 个 store，但补上了 RBP call 前 store 的安全规则。 |
| 理解成本 | 3 | 原 RSP cleanup 被泛化成 stack/frame register cleanup，需要区分 RSP 和 frame pointer 的 declaration 条件。 |
| 维护成本 | 3 | 后续引入 call stack effect 后，需要复核 declaration 无 metadata 的 frame pointer 特判。 |

有没有更好的方案：

- 这轮不是 RSP/RBP 的核心突破。真正的关键问题仍是两个：
  1. call 边界的 stack effect：`RSP` before-call store 要按 `stackshift/extrapop`、返回地址、outgoing stack arg 解释。
  2. `RBP` frame base：要证明 `RBP` 来自本函数稳定的 `RSP` 派生值，再把 `RBP + const` 映射进 `notdec_stack`。
- 本轮保留的价值是把一个明确的 RBP call 前状态写清掉，同时没有放宽 RSP 的未知 declaration 语义。

# 2026-06-03 实现记录：noreturn declaration 前 RSP store 清理

背景：

- frame register call-store cleanup 后，两个小目标还剩多处 `__stack_chk_fail` 前的 `RSP` store。
- `__stack_chk_fail` 是已知 no-return、无参数声明。call 前写 `@RSP` 只是 lifted 全局寄存器状态维护，不影响后续返回路径。
- 这条规则不能扩到普通无 metadata declaration；普通外部函数仍可能依赖 stack pointer、stack argument 或返回地址语义。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2101)
  - 新增 `isKnownNoReturnNoArgumentDeclaration()`。
  - 只接受 declaration、非 vararg、0 参数，并且有 LLVM `noreturn` 属性或名字是 `__stack_chk_fail`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2112)
  - 在 callee 没有 recovered prototype 时，`RSP` 只对上述 no-return/no-arg declaration 放开。
  - 普通无 metadata declaration 的 `RSP` store 仍保留。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8535)
  - declaration call 前 store 测试新增 `__stack_chk_fail` case。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8589)
  - 断言普通无 metadata `RSP` declaration 仍保留，`__stack_chk_fail` 前 `RSP` store 和死 `RSP.external_input` 会删除。

验证：

```bash
git diff --check
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-rsp-noreturn-call-store-gate \
  --target lighttpd:helper \
  --target php:extension-calendar

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-rsp-noreturn-call-store-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-rsp-noreturn-call-store-gate/*.signature-rewrite.ll \
  > /tmp/notdec-rsp-noreturn-call-store-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- Bench2 两个小目标通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `lighttpd:helper` | 2s | 3s |
| `php:extension-calendar` | 12s | 12s |

residue 对比：

```text
after frame register call-store cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  45
gpr store access          full/full  149
other load access         full/full  8

after noreturn declaration RSP cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  42
gpr store access          full/full  141
other load access         full/full  8
```

stack/frame residue 前几类：

```text
23 stack_pointer entry_external_input clobbers entry_external_input
8  stack_pointer entry_external_input clobbers,external_inputs entry_external_input
6  frame_pointer entry_external_input clobbers entry_external_input
3  frame_pointer entry_external_input clobbers,external_inputs entry_external_input
2  stack_pointer before_call clobbers,external_inputs stack_pointer notdec_plt0_resolver declaration
2  stack_pointer before_call clobbers,external_inputs stack_pointer __gmon_start__ declaration
```

判断：

- 两个小目标上 GPR store 从 `149` 降到 `141`，GPR external input 从 `45` 降到 `42`。
- `__stack_chk_fail` 前的 `RSP` before-call residue 消失。
- 普通 declaration 的 `RSP` store 仍保留，下一步仍需要 call stack effect / PLT 语义，而不是继续按名字放宽。
- 耗时同量级，没有看到性能退化。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | 清掉 8 个 store 和 3 个 external input，命中明确 no-return 错误路径。 |
| 理解成本 | 2 | 规则很窄，只新增一个 no-return/no-arg declaration 判断。 |
| 维护成本 | 2 | 后续如果导入更完整的函数属性或 libc 知识，可以替换名字特判。 |

有没有更好的方案：

- 更完整的方式是从导入符号、函数属性或 ABI 模型里获得 no-return 信息，而不是只认 `__stack_chk_fail` 名字。
- 当前先做这个特例，是因为它在两个小目标里稳定出现，且语义边界很清楚。

# 2026-06-03 实现记录：known no-stack declaration 前 RSP store 清理

背景：

- noreturn cleanup 后，两个小目标里剩余的 declaration call 前 `RSP` store 主要来自：
  `__gmon_start__`、`notdec_plt0_resolver`、`php_info_print_table_start`。
- 这些调用在当前 IR 里都是 0 参数 declaration；没有 stack input，也没有需要 caller 维护 `@RSP` 全局状态的证据。
- 普通无 metadata declaration 仍不能放开，因为它可能依赖 stack argument 或真实 stack pointer。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:9)
  - 新增 `llvm/ADT/StringSwitch.h`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2102)
  - 将 `isKnownNoReturnNoArgumentDeclaration()` 改成 `isKnownNoStackArgumentDeclaration()`。
  - 仍要求 callee 是 declaration、非 vararg、0 参数。
  - 继续接受 LLVM `noreturn` 属性。
  - 增加白名单：`__gmon_start__`、`__stack_chk_fail`、`notdec_plt0_resolver`、`php_info_print_table_start`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2118)
  - callee 没 recovered prototype 时，`RSP` 只对 known no-stack declaration 放开，并且仍要求 call 后路径不读 `RSP`。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8541)
  - declaration call 前 `RSP` store 测试新增 `__gmon_start__` case。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8604)
  - 断言 known no-stack declaration 前 `RSP` store 和死 `RSP.external_input` 会删除。

验证：

```bash
git diff --check
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-rsp-known-nostack-call-gate \
  --target lighttpd:helper \
  --target php:extension-calendar

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-rsp-known-nostack-call-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-rsp-known-nostack-call-gate/*.signature-rewrite.ll \
  > /tmp/notdec-rsp-known-nostack-call-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- Bench2 两个小目标通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `lighttpd:helper` | 3s | 3s |
| `php:extension-calendar` | 12s | 12s |

residue 对比：

```text
after noreturn declaration RSP cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  42
gpr store access          full/full  141
other load access         full/full  8

after known no-stack declaration RSP cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  39
gpr store access          full/full  138
other load access         full/full  8
```

stack/frame residue 前几类：

```text
22 stack_pointer entry_external_input clobbers entry_external_input
6  stack_pointer entry_external_input clobbers,external_inputs entry_external_input
6  frame_pointer entry_external_input clobbers entry_external_input
3  frame_pointer entry_external_input clobbers,external_inputs entry_external_input
2  stack_pointer ordinary clobbers,external_inputs stack_pointer
2  frame_pointer ordinary clobbers,external_inputs frame_pointer
```

判断：

- 两个小目标上 GPR store 从 `141` 降到 `138`，GPR external input 从 `42` 降到 `39`。
- `__gmon_start__`、`notdec_plt0_resolver`、`php_info_print_table_start` 前的 RSP before-call residue 消失。
- 剩余 RSP/RBP 已经不再主要是 declaration before-call；下一步应回到 RBP frame-base 和 internal call/store path，而不是继续扩大 declaration 名字白名单。
- 耗时同量级，没有看到性能退化。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | 清掉 3 个 store 和 3 个 external input，覆盖当前两个小目标里的 0 参数 no-stack declaration。 |
| 理解成本 | 2 | 条件仍很窄，但引入了名字白名单。 |
| 维护成本 | 3 | 后续最好从导入符号/函数属性/ABI 模型获得 no-stack 信息，替换手写名字。 |

有没有更好的方案：

- 更完整方案是让 declaration import 信息带出参数/返回/noreturn 属性，或者在 prototype metadata 中表达 no-stack-call。
- 当前先用白名单，是因为这些名字在两个小目标里明确命中，且普通无 metadata declaration 仍保留。

# 2026-06-03 实现记录：RSP/RBP dead store 跨 no-stack call 活性

背景：

- known no-stack declaration cleanup 后，仍有一些 `RSP/RBP` store 因为中间经过 call 被 return-path dead-store 规则保守保护。
- 旧规则把任何 call 都当成同寄存器读屏障。这对普通寄存器是安全的，但对已知不读 `RSP` 的 declaration，或已知不读 `RSP/RBP` 的 internal callee，会留下无用的栈/帧寄存器状态写。
- 这一步只放宽 `RSP` 和 x86 frame pointer 名字；其它寄存器仍把 call 当成可能读，避免误删普通 GPR/vector 的 killed-by-call store。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:1880)
  - 新增 `callMayReadRegisterName()`。
  - 非 `RSP/RBP/EBP/BP` 一律保守返回 may-read。
  - declaration 只在 `RSP` 且 `isKnownNoStackArgumentDeclaration()` 成立时认为不读。
  - internal callee 用 recovered prototype 和函数体真实 load 判断是否会读该寄存器。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:1904)
  - 新增 `callMayReadRegisterAccess()`，从 `notdec.register.access` 的 `name/base` 字段取寄存器名。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:1942)
  - 更新 `reachesReturnWithoutCallOrAccessLoad()`、`storeIsDeadOnAllReturnPaths()`、`reachesReturnOrOverwriteWithoutCallOrAccessLoadRecursive()` 等 return-path 活性检查。
  - call 不再固定阻断，只有 `callMayReadRegisterAccess()` 为 true 时才阻断。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:2092)
  - 新增 `createBranchDeclarationRspStoreCallerFunction()`，覆盖跨 basic block 的 call 后 return 场景。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8585)
  - 新增 `branch_known_nostack_rsp_store`，`__gmon_start__` 前的 dead `RSP` store 可删。
  - 新增 `branch_unknown_rsp_store`，普通未知 declaration 前的 `RSP` store 仍保留。

验证：

```bash
git diff --check
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-rsp-call-liveness-gate \
  --target lighttpd:helper \
  --target php:extension-calendar

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-rsp-call-liveness-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-rsp-call-liveness-gate/*.signature-rewrite.ll \
  > /tmp/notdec-rsp-call-liveness-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- Bench2 两个小目标通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `lighttpd:helper` | 3s | 3s |
| `php:extension-calendar` | 12s | 12s |

residue 对比：

```text
after known no-stack declaration RSP cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  39
gpr store access          full/full  138
other load access         full/full  8

after RSP/RBP call liveness cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  37
gpr store access          full/full  134
other load access         full/full  8
```

stack/frame residue 前几类：

```text
22 stack_pointer entry_external_input clobbers entry_external_input
6  frame_pointer entry_external_input clobbers entry_external_input
4  stack_pointer entry_external_input clobbers,external_inputs entry_external_input
3  frame_pointer entry_external_input clobbers,external_inputs entry_external_input
2  stack_pointer ordinary clobbers,external_inputs stack_pointer
1  frame_pointer entry_external_input preserves entry_external_input
```

判断：

- 两个小目标上 GPR store 从 `138` 降到 `134`，GPR external input 从 `39` 降到 `37`。
- 旧的 call barrier 确实留下了跨 block 的 no-stack call 前死 store；现在这类可证明 case 能删除。
- 未知 declaration 仍保留，非 RSP/RBP 寄存器仍不放宽。
- 耗时同量级，没有看到性能退化。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 2 | 两个小目标少 4 个 store、2 个 external input，是 cleanup 小步，不是 RSP/RBP 核心建模。 |
| 理解成本 | 3 | return-path liveness 从“遇 call 停止”变成按寄存器判断 call 是否会读。 |
| 维护成本 | 3 | 规则只放宽 stack/frame register；后续如果有更完整 call effect，可替换 `callMayReadRegisterName()` 的 declaration 特判。 |

有没有更好的方案：

- 更完整的方案仍是按 Ghidra 的 call stack effect 建模 `stackshift/extrapop`、返回地址和 outgoing stack arg。
- 当前这步的价值是先去掉已证明 no-stack/no-read call 保护住的死状态写，不继续扩大 declaration 名字白名单。

# 2026-06-03 实现记录：native stack alloca 后置死 load/save 清理

背景：

- 静态 RSP alloca rewrite 会把 `RSP.external_input + const -> inttoptr -> load/store` 改成 `notdec_stack.native`。
- 第一轮 `eraseDeadStaticStackStores()` 只按当时存在的 load 判断 store 是否活着。后续 cleanup 可能让某些 load 变成无 use，但不会再回头删掉它保护的保存槽。
- 两个小目标里有这种形态：保存旧 `RBP` 到 `notdec_stack.native`，之后从槽里读出来但读值无用，导致 `RBP.external_input` 继续残留。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2617)
  - 新增 `NativeStackAllocaAccess`，只描述已经本地化的 `notdec_stack.native` 直接 load/store。
  - 规则不重新解释 raw `RSP/RBP` 地址，也不把 `RBP.external_input` 当本函数 frame base。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2646)
  - 新增 `nativeStackAllocaAccesses()`，只接受 `notdec_stack.native + 常量 offset` 的直接 load/store。
  - 遇到 alloca 逃逸、非 load/store 用户、非常量 GEP、未知访问大小时跳过整个 alloca。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2696)
  - 新增 `eraseUnusedNativeStackAllocaLoads()`，删除无 use、非 volatile、非 atomic 的 native stack load。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2726)
  - 新增 `eraseDeadNativeStackAllocaStores()`，按剩余 load 覆盖范围删除没有 overlapping load 的 store，并递归删掉由此暴露的 dead value。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2765)
  - 新增 `eraseDeadNativeStackAllocas()`，循环执行 dead load / dead store 清理，最后删除无 use 的 `notdec_stack.native`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:4365)
  - 在 `eraseDeadStackFrameRegisterStores()` 之后、vector store cleanup 之前调用 native stack alloca cleanup。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:1976)
  - 新增 `createStaticRspUnusedSavedFrameLoadFunction()`。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8527)
  - 新增 `static_rsp_unused_saved_frame_load` 测试，覆盖“保存旧 RBP 的槽被无用 load 暂时保护，后置 cleanup 应删除 load/save/alloca/external input”的 case。

验证：

```bash
git diff --check
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-native-stack-cleanup-gate \
  --target lighttpd:helper \
  --target php:extension-calendar

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-native-stack-cleanup-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-native-stack-cleanup-gate/*.signature-rewrite.ll \
  > /tmp/notdec-native-stack-cleanup-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- Bench2 两个小目标通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `lighttpd:helper` | 3s | 3s |
| `php:extension-calendar` | 12s | 12s |

residue 对比：

```text
after RSP/RBP call liveness cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  37
gpr store access          full/full  134
other load access         full/full  8

after native stack alloca cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  35
gpr store access          full/full  134
other load access         full/full  8
```

判断：

- 两个小目标上 GPR external input 从 `37` 降到 `35`，GPR store 不变。
- 命中的是已经 rewrite 成 `notdec_stack.native` 的旧 frame save/load 噪声，不涉及 raw `RBP + const` frame-base 推断。
- 剩余 `RBP.external_input + negative offset` 仍不能直接改本地 alloca，因为当前样本缺少“本函数建立 RBP”的证据。
- 耗时同量级，没有看到性能退化。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 2 | 只少 2 个 external input，但清掉了一类后续 cleanup 暴露出的旧 frame save。 |
| 理解成本 | 3 | 增加了 native stack alloca 内部访问枚举，但边界较窄。 |
| 维护成本 | 3 | 目前只支持 `i8` GEP 常量 offset 的直接 load/store；后续若生成更复杂 GEP，需要扩展解析。 |

有没有更好的方案：

- 更完整的 RBP 方案仍是证明 frame base：入口保存旧 `RBP`，新 `RBP` 来自当前 `RSP`，再把 `RBP + const` 映射进 `notdec_stack`。
- 当前这步先清理已经本地化的 native stack 噪声，避免把缺少 frame-base 证据的 raw `RBP` 访问误改掉。

# 2026-06-03 实现记录：return-path shared successor 活性修正

背景：

- native stack alloca cleanup 后，两个小目标里还剩少量明显的 `RSP` dead store。
- 典型形态是 `notdec_native_1000` / `notdec_native_3000`：`store @RSP` 后进入 diamond CFG，两条分支汇合到同一个 return block。
- 旧的 return-path liveness 在所有 successor 之间共用同一个 `seen` 集合。第一条分支访问 shared successor 后，第二条分支再看到同一个 successor，就被误判为循环，从而保留死 store。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:1942)
  - 给 `allSuccessorsReachReturnWithoutCallOrAccessLoad()` 增加前置声明，让 `reachesReturnWithoutCallOrAccessLoad()` 可以递归穿过后继 block。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:1946)
  - `reachesReturnWithoutCallOrAccessLoad()` 增加当前 block 参数，走到 block 末尾时继续检查 successor。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:1970)
  - `allSuccessorsReachReturnWithoutCallOrAccessLoad()` 对每条 successor path 复制 `seen`，只把单条路径内回到已见 block 当作循环风险。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2069)
  - `allSuccessorsReachReturnOrOverwriteWithoutCallOrAccessLoadRecursive()` 同样改成 per-path `seen`。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:1907)
  - 新增 `createDiamondStackFrameStoreFunction()`，构造 store 后 diamond CFG 共享 return block 的 case。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8522)
  - 新增 `diamond_rsp_restore` 测试，断言 shared-successor CFG 上的 dead `RSP` store 会删除。

验证：

```bash
git diff --check
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-rsp-shared-successor-gate \
  --target lighttpd:helper \
  --target php:extension-calendar

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-rsp-shared-successor-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-rsp-shared-successor-gate/*.signature-rewrite.ll \
  > /tmp/notdec-rsp-shared-successor-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- Bench2 两个小目标通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `lighttpd:helper` | 3s | 3s |
| `php:extension-calendar` | 12s | 12s |

residue 对比：

```text
after native stack alloca cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  35
gpr store access          full/full  134
other load access         full/full  8

after shared-successor liveness fix:
gpr load  access          full/full  1
gpr load  external_input  full/full  35
gpr store access          full/full  128
other load access         full/full  8
```

判断：

- 两个小目标上 GPR store 从 `134` 降到 `128`，GPR external input 不变。
- 命中的是 CFG 活性误判，不是放宽 call/RSP 语义。
- 剩余 stack/frame residue 只剩 external input，没有 `RSP/RBP` store 类 residue。
- 耗时同量级，没有看到性能退化。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | 清掉 6 个 store，并修正了一个 CFG 活性判断 bug。 |
| 理解成本 | 2 | 改动集中在 liveness traversal，规则本身没有新增语义假设。 |
| 维护成本 | 2 | per-path `seen` 是常规 CFG DFS 写法，后续更复杂 CFG 也更合理。 |

有没有更好的方案：

- 如果后续 dead-store cleanup 扩大范围，可以考虑统一成一个小的 CFG liveness walker，减少多处递归函数重复。
- 当前先做局部修正，是因为问题明确命中 RSP/RBP return-path cleanup，改动范围更小。

# 2026-06-03 实现记录：过滤 stack/frame 派生返回候选

背景：

- shared-successor 修正后，两个小 Bench2 目标的 stack/frame store residue 已清零，但还有 `RSP.external_input`。
- 抽查 `php-extension-calendar` 里 `notdec_native_37d0` 一类函数，`RSP.external_input - const` 被写到 ABI output register，prototype recovery 会把它当作返回值。
- 这类值只是当前 native stack 地址，不应恢复成 LLVM ABI return；否则 signature rewrite 后仍会保留 `RSP.external_input` 链。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:55)
  - 给 `accessMatchesEffectRegister()` 和 `stackFrameRegisterNames()` 增加前置声明，供返回候选过滤复用。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:203)
  - 新增 `valueUsesExternalInputRegister()`，沿 `add/sub/and/ptrtoint/inttoptr/phi` 追踪值是否来自指定 register external input。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:244)
  - 新增 `valueIsStackFrameExternalInputDerived()`，用 ABI stack pointer 和 frame pointer 集合判断值是否来自 stack/frame external input。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:4349)
  - 收集 `returnTrialsBefore()` 结果时跳过 stack/frame external input 派生值。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:222)
  - `attachStackFramePreservedTestAbi()` 增加 `RAX` output，让 stack-frame 专用测试能覆盖 return candidate。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:1776)
  - 新增 `createStackDerivedReturnStoreFunction()`，构造 `RSP.external_input - const -> store @RAX`。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8549)
  - 新增 `return_rsp_derived_rax`，断言它不会被标成 `RAX` return candidate。

验证：

```bash
git diff --check
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-stack-derived-return-gate \
  --target lighttpd:helper \
  --target php:extension-calendar

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-stack-derived-return-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-stack-derived-return-gate/*.signature-rewrite.ll \
  > /tmp/notdec-stack-derived-return-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- Bench2 两个小目标通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `lighttpd:helper` | 2s | 3s |
| `php:extension-calendar` | 12s | 12s |

residue 对比：

```text
after shared-successor liveness fix:
gpr load  access          full/full  1
gpr load  external_input  full/full  35
gpr store access          full/full  128
other load access         full/full  8

after stack-derived return filtering:
gpr load  access          full/full  1
gpr load  external_input  full/full  30
gpr store access          full/full  128
other load access         full/full  8
```

判断：

- 两个小目标上 GPR external input 从 `35` 降到 `30`，GPR store 不变。
- 命中的是 `RSP/RBP.external_input + const` 被误当 ABI return 的形态。
- 这一步不改 raw stack memory，也不把 `RBP + const` 当作本函数 frame base。
- 剩余 `RSP/RBP` 主要是 stack 地址值继续写入非返回寄存器，以及少量 `RSP` caller-stack raw load。
- 耗时同量级，没有看到性能退化。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | 少 5 个 external input，并修正一类错误 prototype return。 |
| 理解成本 | 2 | 只在 return candidate 收集处过滤，值追踪范围有限。 |
| 维护成本 | 2 | 规则依赖 ABI stack/frame register 集合，边界清楚；后续可扩展更多无害 cast/op。 |

有没有更好的方案：

- 更完整的做法是先恢复 native stack 地址，再让这些 stack 地址值不要逃到 ABI register global。
- 当前先阻止错误 return 恢复，避免把栈地址当函数返回值继续传播。

# 2026-06-03 实现记录：清理 stack/frame 派生的非返回寄存器 store

背景：

- 过滤 stack/frame 派生返回候选后，剩余一类典型形态是 `RSP.external_input + const` 被写入 `RCX/R15/RDX` 等非返回寄存器，然后函数返回。
- 这类 store 的值不是本函数 LLVM 返回值，也不是后续 call/input 需要的状态；如果目标寄存器之后所有路径都不读，可以删除。
- 规则必须基于 signature rewrite 后的 canonical IR，因为这时 `notdec.prototype.recovered` 已能说明哪些 register 是真实返回。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3125)
  - 新增 `eraseDeadStackFrameDerivedNonReturnStores()`。
  - 只处理 signature rewrite 后已经 eligible 且不需要继续 rewrite 的函数。
  - store 目标如果命中 `notdec.prototype.recovered` 的 register return，则保留。
  - store value 必须能通过 `valueIsStackFrameExternalInputDerived()` 追到 `RSP/RBP.external_input`。
  - 复用 `storeIsDeadOnAllReturnPaths()`，确保后续所有路径没有 register read/call use，或者被 overwrite。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:4479)
  - 在 signature rewrite cleanup 链末尾调用该规则。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8537)
  - stack-frame 测试模块新增 `RDX` global。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8557)
  - 新增 `dead_rsp_derived_rdx`，构造 `RSP.external_input - const -> store @RDX` 的非返回 store。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8586)
  - 断言 `RDX` store 和死 `RSP.external_input` 都被删除。

验证：

```bash
git diff --check
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-stack-derived-store-gate \
  --target lighttpd:helper \
  --target php:extension-calendar

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-stack-derived-store-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-stack-derived-store-gate/*.signature-rewrite.ll \
  > /tmp/notdec-stack-derived-store-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- Bench2 两个小目标通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `lighttpd:helper` | 2s | 3s |
| `php:extension-calendar` | 12s | 12s |

residue 对比：

```text
after stack-derived return filtering:
gpr load  access          full/full  1
gpr load  external_input  full/full  30
gpr store access          full/full  128
other load access         full/full  8

after stack-derived non-return store cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  17
gpr store access          full/full  114
other load access         full/full  8
```

判断：

- 两个小目标上 GPR external input 从 `30` 降到 `17`，GPR store 从 `128` 降到 `114`。
- 命中的是 stack/frame 地址值写入非返回 register global 后直接返回的残留。
- 规则依赖 `notdec.prototype.recovered`，不会删除真实 ABI return register store。
- 剩余 `RSP/RBP` 主要是 raw caller-stack load，以及 `RBP + negative offset` frame-base 访问；这些仍需要更强的 frame base 证据。
- 耗时同量级，没有看到性能退化。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 4 | 少 13 个 external input 和 14 个 store，直接清理一类 stack address escape 残留。 |
| 理解成本 | 3 | 多了一个基于 recovered prototype 的 store cleanup，但复用现有 liveness 和 stack/frame 值追踪。 |
| 维护成本 | 3 | 边界依赖 recovered metadata；后续若 rewrite 顺序调整，需要确认仍在 rewrite 后调用。 |

有没有更好的方案：

- 最终更好的形态是把这些 stack 地址值恢复成局部 stack object pointer，而不是靠 dead store cleanup 删除。
- 当前样本里这些值只写 register global 后返回，没有后续语义使用；先删掉能减少错误外泄和后续 residue。

# 2026-06-03 实现记录：放宽 unused raw stack/frame load 匹配

背景：

- 清理 stack/frame 派生非返回 store 后，剩余 `RSP` 里还有 `phi(RSP - const, RSP - const) -> inttoptr -> load`。
- 旧 `eraseUnusedRawStackFrameLoads()` 只认唯一 external input base 加常量 offset，漏掉了 canonical IR 里经过 phi 的地址。
- 这些 load 无 use、非 volatile、非 atomic，可以直接删除；used raw load 仍必须保留。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2868)
  - `eraseUnusedRawStackFrameLoads()` 不再要求地址折成固定 offset。
  - 对 `inttoptr` 的整数地址 operand 复用 `valueUsesExternalInputRegister()`，只要能追到 ABI stack/frame external input，就允许删除无 use raw load。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:2089)
  - 新增 `createPhiRawRspLoadFunction()`，构造 phi 地址上的 raw `RSP` load。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8707)
  - 新增 unused / used phi raw load 测试：unused case 删除 `inttoptr` 和死 `RSP.external_input`，used case 保留。

验证：

```bash
git diff --check
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-raw-stack-value-load-gate \
  --target lighttpd:helper \
  --target php:extension-calendar

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-raw-stack-value-load-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-raw-stack-value-load-gate/*.signature-rewrite.ll \
  > /tmp/notdec-raw-stack-value-load-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- Bench2 两个小目标通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `lighttpd:helper` | 3s | 2s |
| `php:extension-calendar` | 11s | 11s |

residue 对比：

```text
after stack-derived non-return store cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  17
gpr store access          full/full  114
other load access         full/full  8

after raw stack value load cleanup:
gpr load  access          full/full  1
gpr load  external_input  full/full  15
gpr store access          full/full  114
other load access         full/full  8
```

判断：

- 两个小目标上 GPR external input 从 `17` 降到 `15`，GPR store 不变。
- 命中的是 canonical 后地址里经过 phi 的无用 raw stack load。
- 规则仍只删除无 use、非 volatile、非 atomic 的 load，不删除 used caller-stack/frame load。
- 剩余 `RSP/RBP` 主要是真实使用中的 caller-stack read，以及 `RBP + negative offset` frame-base/canary 读。
- 耗时同量级，没有看到性能退化。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 2 | 只少 2 个 external input，但补上了 canonical phi 地址形态。 |
| 理解成本 | 2 | 复用已有 stack/frame 值追踪，没有新增复杂状态。 |
| 维护成本 | 2 | 规则更少依赖固定 offset，后续地址表达式变化时更稳。 |

有没有更好的方案：

- 对 used raw caller-stack load，不能简单删除；下一步要区分 return address / caller saved restore / frame-base canary 语义。
- `RBP + negative offset` 仍需要先证明当前函数建立了 frame base，不能靠这条 unused-load 规则处理。

# 2026-06-03 测试口径调整：后续小 gate 改用 shared library

背景：

- 之前 RSP/RBP 小 gate 用 `lighttpd:helper` + `php:extension-calendar`。
- `lighttpd:helper` 对应 `/usr/sbin/lighttpd-angel`，作为独立 helper 会带进 `_start`、返回地址读取、PLT resolver、caller stack 等底层入口行为。
- 这些行为不是当前 RSP/RBP 栈帧恢复最想优先处理的函数形态，容易把注意力拉到启动代码和手写底层栈操作。
- 后续这一轮主 gate 改成两个较小的 Bench2 shared object：
  - `php:extension-calendar`：`/usr/lib/php/20230831/calendar.so`，约 39K。
  - `php:extension-sockets`：`/usr/lib/php/20230831/sockets.so`，约 107K。

验证：

```bash
scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-shared-small-rsp-rbp-gate \
  --target php:extension-calendar \
  --target php:extension-sockets

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-shared-small-rsp-rbp-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-shared-small-rsp-rbp-gate/*.signature-rewrite.ll \
  > /tmp/notdec-shared-small-rsp-rbp-details.tsv
```

结果：

- 两个 shared library 都通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `php:extension-calendar` | 11s | 11s |
| `php:extension-sockets` | 40s | 41s |

当前 residue summary：

```text
category access_kind metadata_kind shape value_shape synthetic count
gpr      load        access        full  full        no        6
gpr      load        external_input full  full        no        119
gpr      store       access        full  full        no        505
other    load        access        full  full        no        44
other    load        external_input full  full        no        2
```

判断：

- 旧 `lighttpd:helper` 结果只作为历史记录保留，不再作为这一轮 RSP/RBP 小 gate 的主判断标准。
- 新 gate 更大，残留数会明显高于旧 helper gate，不能和旧数字直接比较。
- `sockets.so` 暴露更多 call 前 `RSP` store、`RBP + const` raw load/store、canary/frame-base 形态，更适合推动后续 frame-base 和 call stack effect。
- 下一步不要为了降低数字直接删除 used raw load。`RBP` frame-base 仍要先证明 prologue/epilogue 和偏移范围；call 前 `RSP` store 仍要确认 stack input / declaration prototype / callee 读写关系。

# 2026-06-03 实现：stored RBP frame-base raw load 清理

背景：

- shared library gate 里 `php:extension-sockets` 的 `notdec_native_10ef0` 有一类窄形态：
  - 入口读 `RSP.external_input`。
  - 本函数内计算 `RSP - 8` 并 `store` 到 `RBP`。
  - call 后再 `load @RBP`，用 `RBP + 8` 做 unused raw load。
- 这不是外部 caller frame，也不是直接读取返回地址；`RBP` 的值来自当前函数刚保存的 local stack pointer 派生值。
- 不能推广到所有 `RBP.external_input + const`。那类仍可能是 caller frame、保存寄存器、canary 或共享 epilogue。

实现：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:1923)
  - 新增 `storeWritesRegisterName`、`functionMayTouchRegisterName`，用于判断 internal callee 是否仍可能读写指定 frame register。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2010)
  - 新增 `callInvalidatesKnownFrameRegisterValue`：未知 call、declaration、callee prototype 显式输入该寄存器、callee 仍读写该寄存器时，清掉已知 frame register 值。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2604)
  - 新增 `replaceStoredFramePointerRegisterLoads`：只传播本函数内 `store @RBP` 的值，且 stored value 必须能追到 `RSP.external_input`。
  - 多前驱 block 只在所有前驱同一个已知值时合并，否则保守跳过。
  - 替换后删除旧 `load @RBP`，后续 raw load/dead store cleanup 继续删除暴露出的无用链。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:4682)
  - pass 顺序调整为先清一轮 raw stack load 和 dead stack/frame store，再做 stored-RBP 替换，然后再清一轮 raw stack load 和 dead stack/frame store。
  - 原因：callee 中间态可能还有马上会被清掉的 dead `store @RBP`，如果过早判断 call effect，会误判 call 会 touch RBP。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:2140)
  - 新增 stored RBP 正例：直线块、merge block、call-return 后继续替换。
  - 新增反例：`RBP.external_input` 派生的 used raw load 必须保留。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8878)
  - 将这些 case 接入 raw stack load 测试模块。

验证：

```bash
git diff --check
cmake --build build --target native_prototype_recovery_test notdec-native-llvm -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-rbp-stored-frame-final-gate \
  --target php:extension-calendar \
  --target php:extension-sockets

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-rbp-stored-frame-final-gate/*.signature-rewrite.ll
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- 两个 shared library 都通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `php:extension-calendar` | 11s | 11s |
| `php:extension-sockets` | 38s | 39s |

residue 对比：

```text
shared gate baseline:
gpr load  access          full/full  6
gpr load  external_input  full/full  119
gpr store access          full/full  505
other load access         full/full  44
other load external_input full/full  2

after stored RBP cleanup:
gpr load  access          full/full  5
gpr load  external_input  full/full  119
gpr store access          full/full  504
other load access         full/full  44
other load external_input full/full  2
```

判断：

- 命中 `php:extension-sockets` 的 `notdec_native_10ef0`：`load @RBP`、`RBP+8` raw load 和对应 dead `store @RBP` 被清掉。
- 改善很小，但它解决了 shared gate 当前 RBP 瓶颈里一个真实、可证明的子集。
- 规则仍保守：external `RBP` frame、callee 读写 `RBP`、declaration call、不同前驱 frame 值，都不替换。
- 当前运行时间和 baseline 同量级，没有看到性能退化。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 2 | shared gate 只少 1 个 GPR load 和 1 个 GPR store，但命中了真实 RBP frame-base 中间态。 |
| 理解成本 | 3 | 新增了一个很小的数据流，规则窄，合并策略简单。 |
| 维护成本 | 3 | 依赖 pass 顺序，已在日志里说明为什么需要先清 dead store。后续若重排 cleanup 要注意这个关系。 |

有没有更好的方案：

- 真正的下一步仍是函数本地 stack frame 建模，把确定的 `RSP/RBP` frame access 统一转 `alloca/notdec_stack`。
- 这次不碰动态 alloca，也不把 used caller-stack/return-address/canary raw load 当垃圾删。

# 2026-06-04 实现：扩展 0 参数 no-stack declaration RSP store 清理

背景：

- shared library gate 里剩余 `RSP` store 有一部分是 call 前栈状态写，目标是 0 参数 declaration：
  - `__errno_location`
  - `strerror`
  - `if_nametoindex`
- 这些 declaration 在当前 IR 中没有参数，不需要 caller stack 参数。
- 普通 unknown declaration 仍不能删，因为可能需要 caller stack 状态。

实现：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2278)
  - 扩展 `isKnownNoStackArgumentDeclaration` 白名单。
  - 新增 `__errno_location`、`strerror`、`if_nametoindex`、`zend_wrong_param_count`。
  - 仍要求 callee 是 declaration、非 vararg、`arg_size() == 0`。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:9031)
  - 补 `__errno_location` no-stack declaration case。
  - 保留普通 no-metadata declaration 的反例，确认 unknown declaration 不会被误删。

验证：

```bash
git diff --check
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-rsp-known-nostack-decl-gate \
  --target php:extension-calendar \
  --target php:extension-sockets

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-rsp-known-nostack-decl-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-rsp-known-nostack-decl-gate/*.signature-rewrite.ll \
  > /tmp/notdec-rsp-known-nostack-decl-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- 两个 shared library 都通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `php:extension-calendar` | 13s | 12s |
| `php:extension-sockets` | 42s | 40s |

residue 对比：

```text
after stored RBP cleanup:
gpr load  access          full/full  5
gpr load  external_input  full/full  119
gpr store access          full/full  504
other load access         full/full  44
other load external_input full/full  2

after known no-stack declaration expansion:
gpr load  access          full/full  5
gpr load  external_input  full/full  117
gpr store access          full/full  497
other load access         full/full  44
other load external_input full/full  2
```

判断：

- 命中 `__errno_location`、`strerror`、`if_nametoindex` 前的 `RSP` store。
- `zend_wrong_param_count` 仍有一条保留，因为 call 前还有 return-address raw store 等 intervening instruction；现有规则保守保留是对的。
- 剩余 stack/frame residue 主要是：
  - `RBP.external_input`：46
  - `RSP.external_input`：43
  - `sockets_strerror` internal call 前 `RSP` store：8
  - `RBP` frame store：2
- 没有看到运行时间明显退化。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 2 | shared gate 少 7 个 GPR store、2 个 external input，收益明确但范围小。 |
| 理解成本 | 1 | 只是扩展已有 0 参数 no-stack declaration 白名单。 |
| 维护成本 | 2 | 白名单需要人工维护，但仍有 declaration/arg/vararg 门槛。 |

有没有更好的方案：

- `sockets_strerror` 是 internal callee，不能按 declaration 白名单删；要么恢复它的 frame 语义，要么证明 caller 传进去的 RSP 状态不被 callee 使用。
- 大量 `RBP.external_input` 仍是外部 frame/canary/saved-register 形态，下一步要做本地 frame tracking，不能直接删。

# 2026-06-04 实现：RSP/RBP 剩余残留语义审计

背景：

- known no-stack declaration cleanup 后，shared library gate 剩余的 `RSP/RBP` 主要是 `entry_external_input`，单看原来的 `residue_reason` 只能知道它们还被使用，不能区分是本地栈、调用栈、canary、保存寄存器恢复，还是函数 chunk。
- 抽查 `php:extension-sockets` 发现大量形态是：
  - `RBP - offset` 读 canary，再和 `FS_OFFSET + 40` 比较。
  - `RBP/RSP + offset` 读 saved register，再写回 `RBX/R12-R15`。
  - `RSP` call 前 store，给内部 helper 保留 call frame state。
- 这些都不能按普通 raw load/store 删除。先把审计脚本分类补清楚，再决定后续 pass 怎么做。

实现：

- [native-register-residue-audit.py](/sn640/NotDec/external/NotDec-bin2llvm/scripts/native-register-residue-audit.py:32)
  - 新增 SSA 定义和 callee-saved store 的文本匹配，用于审计 raw stack/frame use chain。
- [native-register-residue-audit.py](/sn640/NotDec/external/NotDec-bin2llvm/scripts/native-register-residue-audit.py:359)
  - 新增 `values_derived_from()`，追踪 `external_input -> add/phi/...` 的简单 SSA 派生值。
- [native-register-residue-audit.py](/sn640/NotDec/external/NotDec-bin2llvm/scripts/native-register-residue-audit.py:404)
  - 新增 `stack_semantic_for_access()`。
  - 当前只打审计标签，不作为删除依据。
  - 标签包括 `stack_canary`、`saved_register_restore`、`caller_stack`、`call_frame_state`、`frame_base_state`、`caller_frame`、`chunk_phi`。
- [native-register-residue-audit.py](/sn640/NotDec/external/NotDec-bin2llvm/scripts/native-register-residue-audit.py:711)
  - `--details` 输出新增 `stack_semantic` 列。
- [native_register_residue_audit_test.py](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_register_residue_audit_test.py:209)
  - 新增 `test_stack_semantic_labels_frame_and_caller_stack_patterns()`，覆盖 canary、saved-register restore、caller-stack 标签。

验证：

```bash
git diff --check
python3 tests/native_register_residue_audit_test.py
ctest --test-dir build -R native_register_residue --output-on-failure

python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-rsp-known-nostack-decl-gate/*.signature-rewrite.ll \
  > /tmp/notdec-rsp-known-nostack-decl-details-semantic.tsv
```

结果：

- `python3 tests/native_register_residue_audit_test.py` 通过。
- `notdec.native_register_residue_audit.unit` 通过。
- `git diff --check` 通过。
- 重新审计已有 shared gate artifacts 后，当前 `RSP/RBP` 残留都能分类，没有空 `stack_semantic`。

剩余 `RSP/RBP` 语义分类：

```text
39 RBP entry_external_input stack_canary,saved_register_restore,chunk_phi
28 RSP entry_external_input saved_register_restore,caller_stack,call_frame_state
10 RSP entry_external_input saved_register_restore,call_frame_state
8  RSP store before sockets_strerror call_frame_state
4  RBP entry_external_input stack_canary,saved_register_restore
4  RSP entry_external_input caller_stack,call_frame_state
2  RBP entry_external_input saved_register_restore,chunk_phi
2  RBP store frame_base_state
1  RBP entry_external_input stack_canary,chunk_phi
1  RSP store before zend_wrong_param_count call_frame_state
1  RSP store stack_frame_state
1  RSP entry_external_input call_frame_state
```

判断：

- 当前 shared library gate 的 RSP/RBP 瓶颈已经不是“静态本地栈槽转 alloca”。
- 大头是 canary、保存寄存器恢复、caller stack/call frame state，并且很多通过 PHI 落在共享 epilogue/chunk 里。
- 继续靠删除 raw load/store 会破坏语义。下一步应转向：
  - 函数边界/chunk 识别，避免把共享 epilogue 当独立函数入口。
  - call-site stack effect / stack argument / return-address 建模。
  - 对 canary 和 saved-register restore 做明确语义标记，再决定是否能在 prototype rewrite 后清理。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 3 | 没减少 residue，但把剩余 RSP/RBP 全部分到语义类里，明确了下一步不是继续盲删。 |
| 理解成本 | 2 | 只是审计脚本新增一列，不影响 pass 行为。 |
| 维护成本 | 2 | 分类是保守文本识别，后续可替换成真实 IR 数据流；当前不会影响生成 IR。 |

有没有更好的方案：

- 最终应该在 IR/pass 内有正式 stack semantic metadata，而不是只靠审计脚本文本识别。
- 这次先放在审计脚本，是因为它能低风险确认路线：剩余问题已经进入 chunk/call-stack 语义，不适合继续用 raw load/store cleanup 硬削。

# 2026-06-04 实现：noreturn CFG 截断清理 fake chunk PHI

背景：

- 上一轮 `stack_semantic` 审计把很多 `RBP.external_input` 标成 `chunk_phi`，但其中一部分是脚本文本匹配太宽。
- 收窄后，真正的 `chunk_phi` 只剩 `php:extension-sockets` 的 4 个函数：
  - `notdec_native_60f0`
  - `notdec_native_625f`
  - `notdec_native_6379`
  - `notdec_native_6460`
- 抽查发现这些不是正常共享 epilogue，而是 `__stack_chk_fail()` 后还保留 fallthrough/branch，错误路径又回到正常块，导致 `RBP/RSP` 形成 PHI。
- 这类 CFG 本身不对，应先把已知 noreturn call 后的路径截断，再清无前驱死块。

实现：

- [native-register-residue-audit.py](/sn640/NotDec/external/NotDec-bin2llvm/scripts/native-register-residue-audit.py:374)
  - 新增 `derived_values_include_phi()`。
  - `chunk_phi` 只有在 `RBP.external_input` 派生链里真的出现 PHI 时才标记，避免把普通 `RBP + const` canary/restore 误判成 chunk。
- [native_register_residue_audit_test.py](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_register_residue_audit_test.py:263)
  - 新增 `test_stack_semantic_marks_chunk_phi_only_for_phi_derived_frame()`。
  - 覆盖 direct frame 不标 `chunk_phi`，PHI frame 才标。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2055)
  - 新增 `isFunctionExitInstruction()`，让已有 store liveness 同时接受 `ret` 和 `unreachable`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2305)
  - 新增 `isKnownNoReturnDeclaration()`。
  - 第一版只接受 declaration、非 vararg、0 参数，并且有 LLVM `noreturn` 属性或名字是 `__stack_chk_fail`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2318)
  - 新增 `truncateKnownNoReturnDeclarationCalls()`。
  - 对已知 noreturn declaration call 后的下一条指令调用 `llvm::changeToUnreachable()`。
  - 对改动过的函数调用 `llvm::removeUnreachableBlocks()`，清掉截断后留下的无前驱块和死 PHI incoming。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:4725)
  - 在 signature rewrite 后置 cleanup 一开始调用 noreturn CFG 截断。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:2137)
  - 新增 `createNoReturnFallthroughFunction()`。
  - 构造 `call __stack_chk_fail(); br merge`，merge 里有 PHI。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:3375)
  - 新增 `blockEndsWithUnreachable()` 和 `phiHasIncomingFromBlock()` 测试辅助。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8978)
  - raw RSP/RBP cleanup 测试增加 noreturn fallthrough case。
  - 断言 `fail` block 变成 `unreachable`，merge PHI 不再含 `fail` incoming。

验证：

```bash
git diff --check
python3 tests/native_register_residue_audit_test.py
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-rsp-noreturn-unreachable-gate \
  --target php:extension-calendar \
  --target php:extension-sockets

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-rsp-noreturn-unreachable-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-rsp-noreturn-unreachable-gate/*.signature-rewrite.ll \
  > /tmp/notdec-rsp-noreturn-unreachable-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- 两个 shared library 都通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `php:extension-calendar` | 11s | 12s |
| `php:extension-sockets` | 40s | 40s |

residue 对比：

```text
after known no-stack declaration expansion:
gpr load  external_input  full/full  117
gpr store access          full/full  497
other load access         full/full  44
other load external_input full/full  2

after noreturn CFG truncate:
gpr load  external_input  full/full  109
gpr store access          full/full  452
other load access         full/full  44
other load external_input full/full  2
```

RSP/RBP 分类：

```text
43 RBP entry_external_input stack_canary,saved_register_restore
28 RSP entry_external_input saved_register_restore,caller_stack,call_frame_state
4  RSP entry_external_input caller_stack,call_frame_state
2  RBP entry_external_input saved_register_restore
2  RSP entry_external_input saved_register_restore,call_frame_state
2  RBP store frame_base_state
1  RBP entry_external_input stack_canary
1  RSP store stack_frame_state
1  RSP entry_external_input call_frame_state
```

判断：

- `chunk_phi` 从 4 降到 0。
- `__stack_chk_fail` 后的错误 fallthrough 已变成 `unreachable`，截断后留下的无前驱块也被清掉。
- 这轮实际推进的是“函数边界和 chunk 问题”的一个子集：不是合并 chunk，而是先修正 noreturn 造成的假 chunk。
- 剩余 RSP/RBP 已经主要是 canary、saved-register restore、caller-stack/call-frame state。下一步应继续恢复这些语义，而不是按死代码删。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 4 | shared gate 清掉 fake chunk PHI，GPR store 少 45，external input 少 8。 |
| 理解成本 | 2 | 规则很窄，只处理已知 noreturn declaration call 后的 CFG。 |
| 维护成本 | 2 | 后续需要把 noreturn 信息来源扩展到更正式的 import/prototype metadata，但当前白名单很小。 |

有没有更好的方案：

- 更完整方案是在 native lowering 时就给 `__stack_chk_fail` 等 noreturn call 生成正确 CFG，不要等 prototype recovery 后置 cleanup 修。
- 这次先放在 prototype recovery cleanup，是因为当前 fake chunk residue 出现在 signature rewrite 后，能低风险验证真实收益。

# 2026-06-04 实现：清理死 RBP frame-base store 和线性 call-site RSP store

背景：

- 上一轮 gate 后，剩余 RSP/RBP store 只剩：
  - 2 个 `RBP store frame_base_state`，来自 `RBP = RSP - 8` 后没人再读的帧基址状态。
  - 1 个 `RSP store stack_frame_state`，来自 `notdec_native_6b40` 在 `zval_ptr_dtor(i64)` 前设置 call-site 栈状态。
- 这些都不是 canary、saved-register restore 或 stack arg 本体。它们是 signature rewrite 后还没被清掉的旧寄存器状态。

实现：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:1131)
  - 新增 `LocalCallsiteInputStoreLookup`，把“没找到 store”和“中间被 load/call 阻断”分开。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:1139)
  - 新增 `uniqueEdgeTarget()` 和 `uniqueEdgeSource()`。
  - 对 `br i1 false, label %x, label %x` 这种重复边按线性路径处理；真实多分支仍保守放弃。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:1165)
  - 扩展 `localCallsiteInputStoreBeforeCall()`，允许沿唯一前驱向上找旧 call-site input store。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2064)
  - 新增 `callMayReadRegisterNameForDeadFrameStore()`。
  - 死 RBP frame-base store 判断不再把 declaration call 当成会读当前函数的 `@RBP` 状态。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2076)
  - `callMayReadRegisterAccess()` 支持传入不同的 call-read 判断函数。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2207)
  - `storeIsDeadOnAllReturnPaths()` 支持传入不同的 call-read 判断函数。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2236)
  - `valueIsNeededByInterveningInstruction()` 支持沿唯一后继从 store 扫到目标 call。
  - 中途遇到 call、同寄存器读写、走不到目标 call 或 CFG 环时仍保守失败。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3433)
  - `eraseDeadStackFrameRegisterStores()` 对 frame-pointer store 使用新的 dead-frame call-read 判断。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:4849)
  - `rewriteStaticStackMemoryAccesses()` 后再跑一次 `eraseUnusedDeclarationCallStackFrameRegisterStores()`。
  - 这一步会清掉 `notdec_native_6b40` 里静态栈槽转 alloca 后暴露出来的死 RSP call-site store。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:2449)
  - 新增 `createDeclarationFrameBaseStoreCallerFunction()`，覆盖 `RBP = RSP - 8; call declaration; ret`。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:4866)
  - 前驱 call-site input rewrite 测试增加旧寄存器 store 删除断言。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:4929)
  - 线性前驱 call-site input rewrite 测试增加旧寄存器 store 删除断言。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:9151)
  - declaration RSP/RBP store 测试加入死 frame-base store case。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:9226)
  - 断言死 RBP frame-base store 和对应 RSP external input 都被清掉。

验证：

```bash
git diff --check
python3 tests/native_register_residue_audit_test.py
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-rsp-framebase-callsite-final-gate \
  --target php:extension-calendar \
  --target php:extension-sockets

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-rsp-framebase-callsite-final-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-rsp-framebase-callsite-final-gate/*.signature-rewrite.ll \
  > /tmp/notdec-rsp-framebase-callsite-final-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- 两个 shared library 都通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `php:extension-calendar` | 11s | 11s |
| `php:extension-sockets` | 39s | 40s |

residue 对比：

```text
before:
gpr load  external_input  full/full  109
gpr store access          full/full  452
other load access         full/full  44
other load external_input full/full  2

after:
gpr load  external_input  full/full  107
gpr store access          full/full  449
other load access         full/full  44
other load external_input full/full  2
```

RSP/RBP 分类：

```text
43 RBP entry_external_input stack_canary,saved_register_restore
28 RSP entry_external_input saved_register_restore,caller_stack,call_frame_state
4  RSP entry_external_input caller_stack,call_frame_state
2  RBP entry_external_input saved_register_restore
1  RBP entry_external_input stack_canary
1  RSP entry_external_input call_frame_state
```

判断：

- `RBP store frame_base_state` 从 2 降到 0。
- `RSP store stack_frame_state` 从 1 降到 0。
- 当前 shared library gate 里 RSP/RBP 已经没有 store 残留，只剩 entry external input load。
- 剩余 RSP/RBP 主要是 stack canary、saved-register restore、caller-stack/call-frame state；下一步不能按死 store/load 删除，需要继续做明确语义恢复。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 4 | 清掉最后的 RSP/RBP store 残留，且 gate 时间没有明显退化。 |
| 理解成本 | 3 | 增加了线性前驱/后继扫描，逻辑比原来同块查找复杂。 |
| 维护成本 | 3 | 后续最好把重复边处理和线性路径 helper 合并到已有 CFG 工具函数，避免多处各写一份。 |

有没有更好的方案：

- 最终应该在 call-site stack semantic 恢复时直接删除/替换这些旧 RSP 状态 store，而不是靠后置 cleanup 多跑一轮。
- 这次先放在 prototype recovery cleanup，是因为收益明确、规则保守，并且能直接消掉 Bench2 shared library gate 的最后 RSP/RBP store。

# 2026-06-04 实现：saved-register restore 清理

背景：

- 上一轮后，shared library gate 里 `RSP/RBP` store 已清零，但还有一批 `RSP/RBP.external_input`。
- 其中大头是 saved-register restore：从 caller/native stack 读出保存的 `RBX/R12-R15`，返回前写回 preserved register。
- 这类访问不是业务 raw load/store。能证明返回后没人读该 restored value 时，可以按 ABI preserve 噪声清理；但不能碰 stack arg、return address、canary。

实现：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2741)
  - 新增 `preservedNonStackFrameRegisterNames()`。
  - 只选择 ABI `Unaffected` 里的普通 preserved register，排除 `RSP/RBP` 这类 stack/frame register。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3555)
  - 新增 `valueIsNativeStackAllocaPointer()`。
  - 识别已经由静态栈槽改写产生的 `notdec_stack.native` load。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3570)
  - 新增 `valueIsSavedRegisterRestoreLoad()`。
  - 只接受两种来源：raw `RSP/RBP` 派生 `inttoptr` load，或 `notdec_stack.native` load。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3585)
  - 新增 `eraseDeadPreservedRegisterRestoreStores()`。
  - 条件：store 目标是 preserved 非 stack/frame register；store value 是 saved-register restore load；不是 recovered return；沿所有 return path 没有同寄存器读/call 读取。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:4955)
  - 在 signature rewrite 后置 cleanup 中调用 saved-register restore 清理。
  - 清理后再跑一次 `eraseDeadNativeStackAllocas()`，删除因 restore store 被删而空掉的 `notdec_stack.native`。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:234)
  - 测试 ABI 增加 `RBX` preserved。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:1935)
  - 新增 `createSavedRegisterRestoreFunction()`，构造 raw stack load 后写回 preserved register 的 epilogue 形态。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:8942)
  - 增加 saved RBX restore 正反例：
    - raw `RSP + positive offset` restore 可删。
    - `RSP + negative offset` 先转 `notdec_stack.native` 后仍可删，并清掉 alloca。
    - store 后又读 `RBX` 的 live restore 必须保留。

验证：

```bash
git diff --check
python3 tests/native_register_residue_audit_test.py
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-saved-restore-gate \
  --target php:extension-calendar \
  --target php:extension-sockets

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-saved-restore-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-saved-restore-gate/*.signature-rewrite.ll \
  > /tmp/notdec-saved-restore-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- 两个 shared library 都通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `php:extension-calendar` | 11s | 11s |
| `php:extension-sockets` | 39s | 40s |

residue 对比：

```text
before:
gpr load  external_input  full/full  107
gpr store access          full/full  449
other load access         full/full  44
other load external_input full/full  2

after:
gpr load  external_input  full/full  79
gpr store access          full/full  296
other load access         full/full  44
other load external_input full/full  2
```

剩余 `RSP/RBP` external input 分类：

```text
44 RBP entry_external_input stack_canary
4  RSP entry_external_input caller_stack,call_frame_state
2  RBP entry_external_input caller_frame
2  RSP entry_external_input call_frame_state
```

判断：

- saved-register restore 大头已清掉，`RSP/RBP.external_input` 不再主要卡在 callee-saved restore。
- 剩余 RSP/RBP 主要是 canary、caller frame、caller stack 和 call frame state。
- 这些不能继续按 raw load/store 删除；下一步要做 canary 分类保留、caller-stack/return-address/call-frame 语义恢复。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 4 | shared gate 的 GPR external input 和 GPR store 都明显下降，且时间没有退化。 |
| 理解成本 | 3 | 多了一个 preserved-register restore 专用规则，但条件集中，和已有 dead-store 活性判断复用。 |
| 维护成本 | 3 | 后续如果引入正式 stack object / call-frame 模型，这条后置 cleanup 应迁到语义恢复阶段。 |

有没有更好的方案：

- 更完整的做法是先把 saved-register save/restore 建成明确 ABI preserve 事件，再由寄存器状态模型统一消掉。
- 当前先做窄 cleanup，是因为它只处理返回路径上从栈 load 回 preserved register 的明确形态，能避开 canary、stack arg、return address。

# 2026-06-04 实现：raw caller-stack 首槽输入恢复

背景：

- saved-register restore 清理后，`php:extension-sockets` 还剩 4 个 `RSP caller_stack,call_frame_state`。
- 真实形态是入口 `RSP` 指向 return address，代码从 `RSP + 8` raw load 读取第一个 caller-stack 参数，然后把值作为返回值。
- 这类 load 不能按 raw load 删除，应该恢复成函数参数；但同样的 `RSP + 8` 也可能只是 saved-register restore 槽，必须避开。

实现：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:110)
  - 新增 `stackInputMetadata()`，给 raw caller-stack load 补 `notdec.stack.input` 元数据，复用现有 stack input binding/rewrite。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:343)
  - 新增 `rawStackInputOffset()`，用已有 `stackOffsetFromBase()` 判断 raw load 地址是否来自入口 `RSP.external_input`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:354)
  - 新增 `matchingStackInputSpace()`，只接受 ABI stack input pentry 能匹配的 offset/size。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:370)
  - 新增 `loadOnlyFeedsDeadPreservedRegisterRestore()`。
  - 如果 stack load 只写回 ABI `Unaffected` preserved register，且该 store 到返回前不再被读取，就不把它当参数候选。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:410)
  - `stackInputTrials()` 现在接收 ABI。
  - 继续保留 metadata stack input 必须来自当前 `notdec_stack` alloca 的约束。
  - 新增 raw caller-stack 路径：当前只认 `rawOffset == abi.StackShift`，也就是入口 return address 后的第一个 caller-stack slot。
  - raw load 匹配后写入 `notdec.stack.input` 元数据，让后续 rewrite 使用同一套绑定逻辑。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:5146)
  - `buildNativeRecoveredPrototypeFunctionType()` 支持 1 到 8 字节整数输入参数。
  - 这样 4 字节 caller-stack 参数能恢复成 `i32`，不是被硬限制成 `i64`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:5178)
  - 新增 `supportedNativeInputParamType()`，签名 rewrite 输入参数接受 byte-aligned、宽度不超过 64 bit 的整数。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:5250)
  - `getNativePrototypeInputBindings()` 对 stack input 使用 size 精确匹配。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:5268)
  - 新增 `eraseReplacedInputLoad()`，替换 stack input load 后递归删除死掉的 raw pointer 链，避免残留 `inttoptr` 和死 `RSP.external_input`。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:135)
  - `attachRawStackInputTestAbi()` 增加 `RSP` stack pointer 语义、`StackShift=8`、stack input pentry，以及 `RBX` preserved effect。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:1550)
  - 新增 `createRawCallerStackInputReturnFunction()`，覆盖 `RSP+8` raw load 恢复成参数并返回的形态。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:1586)
  - 新增 `createRawCallerStackSavedRegisterRestoreFunction()`，覆盖同样 `RSP+8` 但只恢复 `RBX` 的反例。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:4271)
  - 新增三个测试函数：
    - `raw_caller_stack_input_return` -> `i64(i64)`。
    - `raw_caller_stack_i32_input_return` -> `i64(i32)`。
    - `raw_caller_stack_saved_rbx_restore` 不允许变成 stack input，并且 dead restore store 被清掉。

验证：

```bash
git diff --check
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
python3 tests/native_register_residue_audit_test.py
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-raw-caller-stack-filtered-gate \
  --target php:extension-calendar \
  --target php:extension-sockets

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-raw-caller-stack-filtered-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-raw-caller-stack-filtered-gate/*.signature-rewrite.ll \
  > /tmp/notdec-raw-caller-stack-filtered-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- `native_register_residue_audit_test.py` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- 两个 Bench2 shared library 都通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `php:extension-calendar` | 12s | 11s |
| `php:extension-sockets` | 40s | 41s |

residue：

```text
gpr load  external_input  full/full  75
gpr store access          full/full  296
other load access         full/full  44
other load external_input full/full  2
```

和 saved-register restore 基线比：

- `gpr load external_input`: 79 -> 75，4 个 raw caller-stack `RSP` load 消失。
- `gpr store access`: 296 -> 296，没有新增 preserved restore store。
- normalized store 明细对比：新增 0，删除 0。

真实例子：

- `notdec_native_dc60`
- `notdec_native_dc80`
- `notdec_native_dca0`
- `notdec_native_dcc0`

这 4 个函数从 raw `RSP+8` load 变成 `i64(i32)` 参数函数，旧 `RSP.external_input`、`inttoptr`、`store @RAX` 都被清掉。

判断：

- caller-stack 首槽输入恢复已经能工作，并且没有把 saved-register restore 槽误当成参数。
- 当前规则故意只认 `abi.StackShift`。更远的 stack arg 还没展开，因为真实样例里容易混到 saved-register restore、caller frame 和 chunk 边界。
- 剩余 `RSP/RBP` 主要还是 `RBP` stack canary、caller frame、call frame state。下一步不应该继续扩大 raw stack arg 规则，而是先把 canary/caller-frame/call-frame-state 分类稳定。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 4 | 明确消掉 4 个真实 caller-stack RSP 残留，且没有增加 store residue。 |
| 理解成本 | 3 | 新增了 raw stack input 入口和 restore-only 过滤，但仍复用现有 stack input rewrite。 |
| 维护成本 | 3 | 后续扩大到更多 stack arg 时，`rawOffset == StackShift` 这条临时限制需要替换成更完整的 caller stack range 判断。 |

有没有更好的方案：

- 更完整的方案是先恢复 call frame：return address、caller stack arg、callee-saved restore、dynamic stack 都有明确分类，再统一把 stack input 暴露成参数。
- 当前先做首槽，是因为它能解决已观察到的 4 个 shared library RSP 残留，同时用 preserved restore 过滤避免误改 ABI cleanup。

# 2026-06-04 实现：stack canary raw RBP load 语义化

背景：

- raw caller-stack 首槽恢复后，两个 Bench2 shared library 里还剩 44 个 `RBP stack_canary`。
- 这些不是本地变量 alloca，也不是普通 raw load。典型形态是 `RBP - const` 读取保存的 canary，和 `FS_OFFSET + 40` 当前 canary 比较；失败路径调用 `__stack_chk_fail`。
- 一些函数 chunk 里，`__stack_chk_fail` 前后还混有 call-frame setup 或错误 fallthrough 代码。必须先按 no-return 清掉不可达后继，再匹配 canary。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2184)
  - `callMayReadRegisterName()` 把 `notdec_stack_canary_check` 当成不读取 `RSP/RBP` 的语义调用，避免后续 dead-store 判断被它挡住。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3771)
  - 新增 `getOrCreateStackCanaryCheckDeclaration()`，用 `notdec_stack_canary_check()` 表达已经识别出的 stack protector 检查。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3782)
  - 新增 `loadReadsRegisterExternalInput()`、`pureInstructionOnlyFeedsBlock()`、`storeIsStackChkFailReturnAddress()`、`blockOnlyCallsStackChkFail()`。
  - fail block 只接受 `__stack_chk_fail`，以及它前面的 `RSP` call-frame store、return-address store 和本块内纯地址计算。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3883)
  - 新增 canary compare 识别：支持 `icmp eq` 和 `zext i1 -> i8 -> icmp eq 0` 这种 lowered 条件。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3928)
  - 新增 `loadIsFrameCanarySlot()` 和 `loadIsFsCanary()`。
  - frame canary 只接受 frame pointer 负 offset；当前 canary 只接受 `FS_OFFSET + 40`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3993)
  - 新增 `canReplaceCanaryLoadUses()`，只允许 canary load 除 compare 外还被 `saved - current` 差值使用。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:4022)
  - 新增 `eraseStackCanaryCheck()` / `eraseStackCanaryChecks()`。
  - 成功匹配后插入 `notdec_stack_canary_check()`，把 canary 差值替成 0，删除旧 branch、fail block、raw frame load、FS load 和死 pointer 链。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:5274)
  - 在 prototype candidate 收集前先跑 `truncateKnownNoReturnDeclarationCalls()`，避免 `__stack_chk_fail` 后的错误 fallthrough 污染返回值候选。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:5415)
  - 在 signature rewrite 后置 cleanup 开始处调用 `eraseStackCanaryChecks()`；后面保留一次 no-return truncate，清理后续 rewrite 暴露出的 no-return fallthrough。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:2313)
  - 新增 `createStackCanaryCheckFunction()`，覆盖 `RBP - const` saved canary、`FS_OFFSET + 40` current canary、fail 前 call-frame setup、fail 后错误 fallthrough。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:3619)
  - 新增 `hasCallTo()` 测试辅助。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:9359)
  - 在 raw RSP/RBP cleanup 测试里加入 canary case，要求旧 `__stack_chk_fail`、`RBP.external_input`、`inttoptr`、`store @RAX` 都被清掉，并保留 `notdec_stack_canary_check()`。

验证：

```bash
git diff --check
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
python3 tests/native_register_residue_audit_test.py
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-stack-canary-noreturn-prepass-gate \
  --target php:extension-calendar \
  --target php:extension-sockets

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-stack-canary-noreturn-prepass-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-stack-canary-noreturn-prepass-gate/*.signature-rewrite.ll \
  > /tmp/notdec-bin2llvm-stack-canary-noreturn-prepass-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- `native_register_residue_audit_test.py` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- 两个 Bench2 shared library 都通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `php:extension-calendar` | 12s | 11s |
| `php:extension-sockets` | 39s | 40s |

residue 对比：

```text
before:
gpr load  external_input  full/full  75
gpr store access          full/full  296
other load access         full/full  44
other load external_input full/full  2

after:
gpr load  external_input  full/full  33
gpr store access          full/full  291
other load external_input full/full  2
```

剩余 `RSP/RBP` 分类：

```text
3 RBP caller_frame
1 RSP call_frame_state
```

判断：

- `RBP stack_canary` 从 44 降到 0。
- 生成了 37 个 `notdec_stack_canary_check()` 调用，旧 canary raw load / FS load / `__stack_chk_fail` fail path 在匹配函数里被语义化。
- `other load access` 从 44 降到 0，说明 `FS_OFFSET + 40` canary load 也一起清掉。
- 剩余 `RSP/RBP` 不是 canary，而是 caller frame / call frame state，下一步应继续做函数边界和 call-frame 语义，不应按 canary 规则扩大删除。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 5 | shared gate 上 canary 类 RBP 残留清零，且 `FS_OFFSET` canary load 同步消失。 |
| 理解成本 | 3 | matcher 较多，但每个条件都围绕 canary 固定形态，风险集中。 |
| 维护成本 | 3 | 后续如果引入正式 stack protector 语义节点，可把 `notdec_stack_canary_check()` 替换成更标准的内部 intrinsic。 |

有没有更好的方案：

- 更完整方案是先修函数 chunk 边界，让 `__stack_chk_fail` 后的错误 fallthrough 不进入函数体。
- 当前先做 no-return prepass 加 canary 语义化，是因为它能直接处理真实 shared library 的 canary residue，并且不会把 caller-frame 或 call-frame-state 当成普通栈槽删掉。

# 2026-06-04 实现：caller-frame raw RBP 访问语义化

背景：

- canary 清理后，两个 Bench2 shared library 只剩 3 个 `RBP caller_frame` 和 1 个 `RSP call_frame_state`。
- 3 个 `RBP caller_frame` 都是入口外部 `RBP - const` 直接访问：
  - `notdec_native_65cc`：向父 frame 写 `i32 -1`。
  - `notdec_native_6108`：从父 frame 读 `i32` 后传给 `zend_argument_type_error`。
  - `notdec_native_670c`：从父 frame 读 `i64` 后传给 `freeaddrinfo`。
- 这些不是当前函数本地栈槽，不能转 `alloca`。它们更像 chunk 访问父函数 frame，所以这轮只把 raw RBP 访问分类成显式 caller-frame 语义。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:2187)
  - `callMayReadRegisterName()` 把 `notdec_caller_frame_*` 语义调用当成不读取 `RSP/RBP` 的调用。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3774)
  - 新增 `supportedCallerFrameAccessType()`，当前只支持 `i32/i64`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3780)
  - 新增 `getOrCreateCallerFrameAccessDeclaration()`。
  - load 生成 `notdec_caller_frame_load_i32/i64(offset)`。
  - store 生成 `notdec_caller_frame_store_i32/i64(offset, value)`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3806)
  - 新增 `rewriteCallerFramePointerAccess()`。
  - 只匹配 `RBP.external_input + 负常量 -> inttoptr -> 直接 load/store`。
  - 不接受 volatile/atomic，不接受非直接用户，不接受正 offset。
  - 改写后删除旧 load/store、`inttoptr` 和死的 `RBP.external_input` 链。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3879)
  - 新增 `rewriteCallerFrameAccesses()`，只处理 ABI stack-frame register 里的 frame pointer，不处理 `RSP`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:5588)
  - 在 signature rewrite 后置 cleanup 中，放在 saved-register restore 清理之后调用。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:2572)
  - 新增 `createExternalRbpRawStoreFunction()`，覆盖外部 RBP frame store。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:9377)
  - 原 `external_rbp_raw_load` 从“必须保留 raw RBP”改成 caller-frame load 正例。
  - 新增 caller-frame store 正例，要求旧 `inttoptr` 和 `RBP.external_input` 被清掉。

验证：

```bash
git diff --check
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
python3 tests/native_register_residue_audit_test.py
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-caller-frame-gate \
  --target php:extension-calendar \
  --target php:extension-sockets

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-caller-frame-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-caller-frame-gate/*.signature-rewrite.ll \
  > /tmp/notdec-bin2llvm-caller-frame-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- `native_register_residue_audit_test.py` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- 两个 Bench2 shared library 都通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `php:extension-calendar` | 12s | 12s |
| `php:extension-sockets` | 40s | 41s |

residue 对比：

```text
before:
gpr load  external_input  full/full  33
gpr store access          full/full  291
other load external_input full/full  2

after:
gpr load  external_input  full/full  30
gpr store access          full/full  291
other load external_input full/full  2
```

剩余 `RSP/RBP` 分类：

```text
1 RSP call_frame_state
```

真实语义化结果：

```llvm
call void @notdec_caller_frame_store_i32(i64 -36, i32 %unique_6a80_4)
%0 = call i32 @notdec_caller_frame_load_i32(i64 -68)
%0 = call i64 @notdec_caller_frame_load_i64(i64 -152)
```

判断：

- `RBP caller_frame` 从 3 降到 0。
- 这轮没有把父 frame 当成本地 alloca，也没有扩大到 `RSP`。
- 剩余唯一 `RSP call_frame_state` 是 `notdec_native_10ef0` 里把 `RSP.external_input - 16` 作为参数传给内部函数 `notdec_native_102f0`。这属于 call-site 栈状态传递，不适合继续用 raw load/store cleanup 解决。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 4 | shared gate 上 caller-frame RBP 清零，只剩 call-frame-state RSP。 |
| 理解成本 | 3 | 新增 caller-frame 语义声明，但 matcher 条件很窄。 |
| 维护成本 | 3 | 后续如果修 chunk/function boundary，应把这些 caller-frame 调用替换成真正的父函数 frame 引用或合并后的本地访问。 |

有没有更好的方案：

- 更完整方案是修函数边界，把这些 chunk 合回父函数，或者在 IR 层显式建 chunk parent frame。
- 当前先分类成 `notdec_caller_frame_*`，是因为它不假装这是本地栈，也能去掉最后几个 raw RBP 依赖。

# 2026-06-04 实现：内部 helper 死参数收缩清理 call-frame-state RSP

背景：

- caller-frame 语义化后，两个 Bench2 shared library 只剩 1 个 `RSP call_frame_state`。
- 真实样例在 `php-extension-sockets.signature-rewrite.ll`：
  - `notdec_native_10ef0` 计算 `RSP.external_input - 16`，作为第三个参数传给 `notdec_native_102f0`。
  - `notdec_native_102f0` 只把这个参数写进 `@R9`，函数内没有再读，调用点之后也没有读 `R9`。
- 这不是一个应保留的 stack semantic，而是内部 helper 传播了已经没用的寄存器状态。直接新增一个 `RSP - 16` 语义调用不合适，应该删掉这条死参数链。

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3774)
  - 新增 `callsitePathReadsRegisterAfterCall()`，判断调用点之后是否还会读某个寄存器。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3787)
  - 新增 `argumentOnlyFeedsDeadRegisterStore()`，只接受“参数唯一用途是非 volatile/atomic 的 `store arg, @REG`，且 store 后到 return 不再读该寄存器”的窄形态。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3817)
  - 新增 `directInternalCallsites()`，要求被收缩函数所有 use 都是直接调用，避免改错间接调用或签名不一致的调用。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3841)
  - 新增 `shrinkInternalFunctionArguments()`，重建内部函数签名、更新 `notdec.prototype.recovered` metadata，并同步改写所有直接 callsite。
  - 删除旧 call 后，对被移除实参调用 `RecursivelyDeleteTriviallyDeadInstructions()`，让 `RSP.external_input - const` 这类死链一起清掉。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3933)
  - 新增 `eraseDeadInternalCallArgumentRegisterStores()`，统一找出这种死参数、删掉寄存器 store、再收缩函数参数。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:5798)
  - 在 signature rewrite 后置 cleanup 中，放在 caller-frame access 改写之后执行。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:3660)
  - 新增 `firstCallTo()` 测试 helper。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:9116)
  - 新增 `dead_internal_argument_callee/caller` 正例：callee 的第二个参数只写 `R8`，caller 用 `RSP.external_input - 16` 传入。验证 pass 后 callee/callsite 都只剩 1 个参数，`R8` store 和 caller 里的 `RSP.external_input` 都被清掉。

验证：

```bash
git diff --check
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
python3 tests/native_register_residue_audit_test.py
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-dead-internal-argument-gate \
  --target php:extension-calendar \
  --target php:extension-sockets

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-dead-internal-argument-gate/*.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-dead-internal-argument-gate/*.signature-rewrite.ll \
  > /tmp/notdec-bin2llvm-dead-internal-argument-details.tsv
awk -F'\t' 'NR>1 && $15 ~ /RSP|RBP/ {print}' \
  /tmp/notdec-bin2llvm-dead-internal-argument-details.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- `native_register_residue_audit_test.py` 通过。
- 相关 CTest 6/6 通过。
- `notdec-native-llvm` 构建通过。
- 两个 Bench2 shared library 都通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `php:extension-calendar` | 11s | 11s |
| `php:extension-sockets` | 39s | 40s |

residue：

```text
category	access_kind	metadata_kind	shape	value_shape	synthetic	count
gpr	load	external_input	full	full	no	21
gpr	store	access	full	full	no	193
other	load	external_input	full	full	no	2
```

剩余 `RSP/RBP` 分类：

```text
无
```

判断：

- 两个 shared library 上的 `RSP/RBP` raw residue 暂时清零。
- 这轮没有扩大到删除所有 `R8/R9` store，只处理“内部 helper 参数唯一用途是死寄存器 store”的形态。
- 仍然不能说整个 RSP/RBP 问题结束。当前只证明这两个 shared library 的已知残留清掉了，下一步还要继续做本地栈槽转 `alloca`、动态栈调整、stack arg / return-address 分类和函数 chunk 边界。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 4 | 当前 shared gate 上最后一个 `RSP call_frame_state` 清零。 |
| 理解成本 | 4 | 需要重建函数签名和 callsite，比纯 matcher 删除复杂。 |
| 维护成本 | 3 | 条件很窄，后续如果 prototype rewrite 有统一的函数参数收缩工具，可以把这里合进去。 |

有没有更好的方案：

- 更完整方案是从 call-site 栈语义恢复入手，恢复内部 helper 的真实参数和返回语义。
- 当前先做死参数收缩，是因为这个样例的 `RSP - 16` 没有真实栈访问语义，只是死寄存器状态传播。保守删掉它，比新增一个假的栈语义节点更准确。
