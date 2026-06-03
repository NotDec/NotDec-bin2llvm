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
