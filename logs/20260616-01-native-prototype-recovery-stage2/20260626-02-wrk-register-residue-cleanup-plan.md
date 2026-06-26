# 原始 prompt

接下来以wrk为目标，调研一下wrk的结果中剩余的寄存器情况，规划一下该怎么处理，写成一个新的plan文件

# 补充 prompt

把顺序改一下吧，处理FS_Offset以及canary的问题提前，作为第一点。同时改进一下这步：首先，调研LLVM源码中插入canary的pass，看它会对函数做什么样的变换，其次，考虑在当前Pass链路里找合适的位置创建一个canary消除的Pass，专门匹配这种模式，然后删除掉canary相关的判断和check fail分支。注意必须要直接做rewrite这一步，而不是先纯标注。

# 背景

当前默认 native 链路是 summary：

- `notdec-native-llvm` 默认运行 `NativeRegisterSummarySSA`。
- 新的寄存器消除、signature rewrite 和 residue cleanup 都应继续放在 `lib/passes/summary/`。
- 旧 heritage 链路只保留对照，这次不碰。

最新可用 `wrk` 输出：

- `/tmp/notdec-wrk-float-libm.ll`
- `/tmp/notdec-wrk-float-libm.summary.json`
- `/tmp/notdec-wrk-float-libm.bc`
- `/tmp/notdec-wrk-float-libm.opt.bc`

这份输出来自最近的 fixed libm float ABI 变更后验证。LLVM 22 `llvm-as` 和 `opt -passes=verify` 已通过，生成耗时 `wrk elapsed=45.42 user=45.35 sys=0.06 maxrss=184536`。

# 当前 wrk 残留

命令：

```bash
scripts/native-register-residue-audit.py /tmp/notdec-wrk-float-libm.ll
scripts/native-register-residue-audit.py --details /tmp/notdec-wrk-float-libm.ll
```

汇总：

| category | access_kind | count |
| --- | --- | --- |
| flags | store | 2 |
| other | load | 10 |
| other | store | 22 |

按寄存器名：

| name | count |
| --- | ---: |
| ST0 | 4 |
| ST1 | 4 |
| ST2 | 4 |
| ST3 | 4 |
| ST4 | 4 |
| ST5 | 4 |
| ST6 | 4 |
| ST7 | 3 |
| OF | 2 |
| FS_OFFSET | 1 |

按函数：

| function | count |
| --- | ---: |
| notdec_native_8530 | 14 |
| main | 10 |
| notdec_native_88c0 | 7 |
| sock_read | 1 |
| notdec_native_6cc0 | 1 |
| aeCreateTimeEvent | 1 |

结论：

- `wrk` 当前没有 GPR 残留。
- `wrk` 当前没有 ZMM/XMM 残留。
- 剩余问题集中在一个 `FS_OFFSET` TLS/canary 读、x87 `ST*`、两个 `OF` store。

# Ghidra 相关依据

## x86-64 SysV cspec

文件：`/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec`

关键点：

- `:30`：stack pointer 是 `RSP`。
- `:37-82`：默认 input 先列 `XMM0_Qa..XMM7_Qa`，再列 `RDI/RSI/RDX/RCX/R8/R9`，再列 stack `addr offset="8" space="stack"`。
- `:92-103`：默认 output 包括 `XMM0_Qa/XMM1_Qa` 和 `RAX/RDX`。
- `:114-118`：默认 killedbycall 只列 `RAX/RDX/XMM0`。
- `:119-127`：默认 unaffected 只列 `RBX/RSP/RBP/R12/R13/R14/R15`。
- `:16`：`long_double_size value="10"`，这和当前残留里的 `ST* i80` 对上。

这里没有把 `ST0..ST7` 放进 x86-64 SysV default prototype 的 killedbycall/unaffected。native 侧不能简单把所有 x87 都按 call-clobber 删除。

## Ghidra decompiler 方向

相关文件：

- `Features/Decompiler/src/decompile/cpp/architecture.cc`
  - `Architecture::getSegmentOp()`
  - `Architecture::initializeSegments()`
  - `SegmentedResolver::resolve()`
- `Features/Decompiler/src/decompile/cpp/ruleaction.cc`
  - `RuleSegment::applyOp()`
- `Features/Decompiler/src/decompile/cpp/printc.hh`
  - `PrintC::opSegmentOp()`

Ghidra 对 segment base / TLS 不是把 `FS_OFFSET` 当普通参数寄存器消掉，而是有 segment op / resolver 这条机制。native 侧现在只是把 `FS_OFFSET` 当 register global，所以 stack canary 路径还会留下 `load @FS_OFFSET`。

# LLVM StackProtector 相关依据

文件：`/sn640/NotDec/llvm-source/llvm/lib/CodeGen/StackProtector.cpp`

关键点：

- `:117-149`：new PM 的 `StackProtectorPass::run()` 判断需要保护后调用 `InsertStackProtectors()`。
- `:171-204`：legacy `StackProtector::runOnFunction()` 也是同一路线。
- `:527-550`：`getStackGuard()` 从 target TLS guard 或 `llvm.stackguard` intrinsic 取当前 guard。
- `:562-571`：`CreatePrologue()` 在入口插入 `StackGuardSlot` alloca 和 `llvm.stackprotector`。
- `:574-719`：`InsertStackProtectors()` 在 return 或必要的 noreturn call 前插入 epilogue 检查。
- `:690-713`：inline 检查形状是“取当前 guard、取保存的 guard、`icmp`、条件跳转”，成功分支回原返回路径，失败分支到 fail block。
- `:721-751`：`CreateFailBB()` 创建 `__stack_chk_fail` 或目标平台的 stack smash handler，然后 `unreachable`。

对应到 x86-64 native lifted IR，当前 guard 通常表现为 `FS_OFFSET + 40`，即 `FS:0x28`。所以 summary 链路要识别的是已经 lower 之后的 canary epilogue：从本地栈 slot 读保存值，从 `FS_OFFSET + 40` 读当前值，比较后失败分支调用 `__stack_chk_fail`。

旧 heritage 链路的 `lib/passes/heritage/NativePrototypeRecovery.cpp` 里已有 `eraseStackCanaryCheck()`、`loadIsFsCanary()` 等代码，可读作模式参考。但这次不能依赖 heritage，也不应把新功能写回 `NativePrototypeRecovery`。

# 残留分类

## 1. FS_OFFSET TLS/canary 读

位置：

- `notdec_native_6cc0:3353`：`%FS_OFFSET256 = load i64, ptr @FS_OFFSET`

周边 IR 是 stack canary 检查：

- 先从本地栈 slot 取保存的 canary。
- `load @FS_OFFSET`
- 加 `40`
- 从 `FS_OFFSET + 40` 读当前 canary。
- 比较，不等则走 `__stack_chk_fail`。

这条不是寄存器传参残留，而是编译器插入的 stack protector epilogue。它的处理目标不是先标注 TLS/segment 后继续保留，而是直接 rewrite：匹配 canary 检查后，删除比较、失败分支和只服务于 canary 的 `FS_OFFSET + 40` 读。

处理判断：

- 不能把 `FS_OFFSET` 当常量删。TLS base 是运行时状态。
- 不能删除所有 `FS_OFFSET` 读。只删能证明是 stack canary epilogue 的模式。
- 不能只改 residue audit 分类。第一步必须在当前 summary pass 链路里实际 rewrite IR。
- 不能直接复用旧 heritage pass；可以参考旧 matcher 的形状，但实现要放在 summary 链路。

## 2. x87 ST 栈残留

位置：

- `main`
  - `:682`：`load i80 @ST7`
  - `:1050`、`:1055-1061`：`store i80 0 @ST7/@ST0..@ST6`
  - `:1071`：`load i80 @ST7`
- `notdec_native_88c0`
  - `:2351-2357`：`store i80 0 @ST0..@ST6`
- `notdec_native_8530`
  - `:5852-5858`：`store i80 %ST* @ST0..@ST6`
  - `:5897-5902`：`load i80 @ST5..@ST0`
  - `:5911`：`load i80 @ST6`

局部 IR 显示这些值混在 `x86_fp80` 浮点计算、保存旧 x87 栈值、恢复/搬移 `ST*` 的序列里。它们不是当前 fixed libm XMM ABI 的问题，也不是普通整数 register SSA 问题。

处理判断：

- 不能按“x87 都是 caller-saved”删。x86-64-gcc.cspec 没有这么说。
- 不能只做死 store 删除。`notdec_native_8530` 的 `ST*` load/store 形成跨 block 的 x87 栈保存/恢复。
- 需要单独建 x87 栈模型，至少识别 `ST0..ST7` 的 push/pop/rotate/restore 模式，或者先做非常窄的 wrk pattern cleanup。

## 3. OF flag store

位置：

- `sock_read:3174`：`store i8 %1, ptr @OF`，后面调用 `read`。
- `aeCreateTimeEvent:7264`：`store i8 0, ptr @OF`，后面调用 `gettimeofday`。

这两条都是 call 前的 flag 写入，当前审计归类为 `callsite_input_store`。外部调用不会读取 CPU flags。理论上 residue cleanup 可以删掉这类 call 前 flags store。

风险：

- flags 也可能被后续条件跳转读取，不能全局按“call 前 flags”删除。
- 需要用现有 backward demand 或局部 use 检查，只删到下一个真实 flags observer 之前没有被读的 store。

# 目标

以 `wrk` 为第一目标，把当前 residue 从“普通 register access”里继续收敛：

1. 先处理 `FS_OFFSET` / canary：调研 LLVM StackProtector 形状，在 summary 链路里新增 canary cleanup，直接 rewrite 掉 canary 判断和 `__stack_chk_fail` 失败分支。
2. 再消掉确定无语义的 `OF` flag store。
3. 最后对 x87 `ST*` 做保守方案：先分类和建最小测试，再决定是局部 cleanup 还是真正的 x87 stack model。

成功标准不是盲目让 audit 为 0，而是让剩余每一类都有清楚语义：

- GPR / XMM / ZMM 继续为 0。
- `FS_OFFSET + 40` canary 检查被 rewrite，相关 `__stack_chk_fail` 失败分支被删；如果还有 `__stack_chk_fail`，必须能说明不是已支持 canary 模式。
- `OF` 两条 store 删除，且 `wrk` verifier 通过。
- x87 `ST*` 如果暂不删除，要能列出保存/恢复链，不误删。

# 阶段计划

## 阶段 1：FS_OFFSET / canary rewrite

范围：

- 新增 summary 链路 pass/helper，建议命名为 `NativeStackCanaryCleanup`。
- `include/notdec-bin2llvm/passes/summary/`
- `lib/passes/summary/`
- `lib/CMakeLists.txt`
- `lib/passes/summary/NativeRegisterSummarySSA.cpp`
- `tests/native_register_summary_ssa_test.cpp`

路线：

- 先按 LLVM 22 的 `StackProtector.cpp` 固定 canary 形状：
  - prologue 保存 guard。
  - epilogue 重新读取 guard。
  - 比较保存值和当前值。
  - 失败分支调用 `__stack_chk_fail`，然后 `unreachable`。
- 实现形态先按当前 native 链路风格做独立 helper pass，不先接 LLVM PassManager：
  - 头文件声明 `runNativeStackCanaryCleanup(llvm::Module &module)`。
  - cpp 内部返回 cleanup summary，至少记录扫描函数数、rewrite 次数、删除 fail block 数。
  - 由 `runNativeRegisterSummarySSA()` 串起来调用，而不是把 matcher 继续塞进 `FunctionBuilder`。
- 在 `runNativeRegisterSummarySSA()` 里找插入点。优先放在 `runNativeStackFrameRewrite(module)` 之后、`runNativeRegisterSummary(module, summaryOptions)` 之前：
  - stack/frame 访问已经过一轮规整。
  - `FS_OFFSET` 还没有被 summary SSA 改成 entry load / phi，模式更直接。
  - canary 分支不会继续污染后面的 register demand 和 signature rewrite。
- 暂不放到更晚的位置。晚于 summary SSA 后，`FS_OFFSET` 可能已经变成 entry load / phi，CFG 和 PHI 也更容易被 canary fail path 污染，matcher 会复杂很多。
- 如果早期位置覆盖不了某些 wrk 形状，再补一个晚期 cleanup，但第一版先避免支持两套 matcher。
- matcher 只处理能证明的 stack protector epilogue：
  - 条件分支的一边只调用 `__stack_chk_fail` 或平台 stack smash handler，然后 `unreachable`。
  - 条件来自 `icmp eq/ne`，允许外层 `zext` 再和 0 比较。
  - 一个比较输入是本地栈 slot 里的 saved canary。
  - 另一个比较输入是 `load (inttoptr (load @FS_OFFSET + 40))` 形状的 current canary。
  - 两个 canary load 和地址计算链除这个检查外没有真实用户。
- rewrite 动作必须直接改 IR：
  - 把条件分支改成无条件跳到成功分支。
  - 删除 fail block，或者在不可达后由 `removeUnreachableBlocks()` 清掉。
  - 删除 compare、`FS_OFFSET + 40` 地址链、saved/current canary load 等死指令。
  - 不插入 `notdec_stack_canary_check` 语义 helper。
- 负例必须保守：
  - fail block 有额外副作用不删。
  - `FS_OFFSET` 读被其他逻辑使用不删。
  - 偏移不是 `40` 不删。
  - 条件不是 canary equality 不删。

验证：

- 单测新增：
  - 正常 `eq`/`ne` canary check 被 rewrite。
  - `zext` 后和 0 比较的 lifted 形状被 rewrite。
  - fail block 有额外副作用时不 rewrite。
  - `FS_OFFSET + 非 40` 时不 rewrite。
- `wrk` 重新生成并过 LLVM 22 verifier。
- residue audit 中 `FS_OFFSET` 普通残留目标为 0。
- `wrk` 中已匹配 canary check 的 `__stack_chk_fail` 失败分支消失。
- `fortune` 同口径耗时不明显退化。

风险：

- lifted IR 可能已经把 `FS_OFFSET` 改成 summary SSA phi；第一版如果只放早期位置，要确认 wrk 目标形状是否都在早期可见。
- 部分函数可能把 canary 差值用于额外计算，不能只因为看到 `__stack_chk_fail` 就删。
- 删除 CFG 分支后要维护 verifier，特别是 PHI incoming 和 unreachable block 清理。

## 阶段 2：flag store cleanup

范围：

- `lib/passes/summary/NativeRegisterSummarySSA.cpp`
- `tests/native_register_summary_ssa_test.cpp`

路线：

- 复用现有 dead store / partial demand 思路，只针对 flags register。
- 规则要保守：store 后到下一个可能读 flags 的指令之间，如果只有 call 或普通不读 flags 的操作，且该 flags 值不进入内存或返回，就删除。
- 第一版只覆盖 `OF`，或者按 flags 集合 `CF/PF/AF/ZF/SF/OF` 做同一套逻辑，但测试必须覆盖“后续 branch 读取 flags 时不能删”。

验证：

- 单测新增：
  - call 前 dead `OF` store 被删。
  - branch/icmp 真实使用时不删。
- `wrk` 重新生成并过 LLVM 22 verifier。
- residue audit 中 `flags store` 从 2 变 0。

风险：

- flags 在 lifted IR 里可能通过 load 后参与 select/branch，不一定直接接 branch。
- 不能把所有 intrinsic 前 flags store 都删，先按 demand 证明。

## 阶段 3：x87 ST 残留分类和最小模型

范围：

- 先写测试和审计，不急着大改。
- 可能涉及 `NativeRegisterSummarySSA` 的 register set 分类、x87 stack-specific cleanup。

路线：

- 先从 wrk 抽三个最小模式：
  - `main` 中 `ST7` 保存到内存后恢复。
  - `notdec_native_88c0` 中 call 前清 `ST0..ST6`。
  - `notdec_native_8530` 中 `ST0..ST6` 跨 block 保存/恢复后继续 x87 计算。
- 对每个模式判断：
  - 是否只是死的 x87 stack cleanup。
  - 是否是需要保留的 `long double` 计算状态。
  - 是否可以转成普通 SSA 值，不再经 register global。
- 第一版只删除能证明死的 store；跨 block 保存/恢复不急删。

验证：

- residue audit 能单独报告 x87。
- 若实现 cleanup，`wrk` 中 `ST*` 残留下降，LLVM 22 verifier 通过。
- `fortune`、`memcached`、`redis-cli` 至少跑 verifier，避免 x87 规则误伤。

风险：

- x87 是栈机器，不是普通独立寄存器。`ST0..ST7` 的编号随 push/pop 变化。
- 当前 IR 里同时有 `x86_fp80` store 和 `i80` store，类型混合；简单 RAUW 容易破坏 verifier。
- x87 结果可能和 `long double` 或 libc printf 路径有关，误删会改变语义。

# 不做什么

- 不回退旧 heritage 链路。
- 不只把 `FS_OFFSET` 标成 TLS/segment 就结束。
- 不把 `FS_OFFSET` 当常量删。
- 不删除非 canary 语义的 `FS_OFFSET` 读。
- 不无条件删除所有 `__stack_chk_fail` 调用。
- 不把所有 x87 `ST*` 都按 caller-saved 删除。
- 不在这一步处理 stack 参数 rewrite。
- 不扩大 fixed external prototype 表，除非 wrk 残留明确指向某个缺失原型。

# 判断标准

短期：

- `wrk` 仍通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
- `fortune` 同口径耗时不明显退化。
- `scripts/native-register-residue-audit.py /tmp/notdec-wrk-*.ll` 中：
  - GPR / vector 仍为 0。
  - canary rewrite 后，`FS_OFFSET` 普通残留目标为 0。
  - flags store 目标为 0。
  - x87 残留数量和位置可解释。

长期：

- 对简单项目，普通寄存器残留可以接近 0。
- 对复杂项目，剩余项不再混成“寄存器没消掉”，而是分成 flags、segment/TLS、x87、真实 vector state 等明确类别。
