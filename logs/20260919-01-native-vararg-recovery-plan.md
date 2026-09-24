# native 变参函数识别与恢复（va_start/va_arg/va_end）—— plan

## 用户原始 prompt

> 变参函数确实，我看LLVM IR里面似乎是用va_start va_end以及va_arg 等表示的。能否尝试识别变参这一块的逻辑，
> 并且也恢复成这种形式？同理，也可以参考一下，LLVM ir 遇到va_start va_end以及va_arg等后端是怎么lowering的，
> 针对性地用最少的代码识别编译器会生成的模式即可

## 已有 native 状态（本轮之前的基线）

- **外部变参调用已经能表示**：只要 external prototype 里标了 `VarArg`，签名/调用点就是
  `declare i64 (i64, i64, ...) @__snprintf_chk(...)`；调用点走
  `NativeRegisterSummarySSA.cpp:8480` 的 `shape->VarArg` 分支，未证明的实参用
  `notdec.register.vararg_unknown.i64` 占位（`shouldPoisonUnprovenVarArg` :8195），最后
  `lowerVarArgUnknownHelpers` 降级。浮点计数用调用点前 RAX 低字节（AL = 向量寄存器个数）。
- **内部变参函数完全没有识别**：lift 把手写寄存器保存区 + 手写 va_list 原样留下，summary 只看到
  "入口读了 R8/R9"，于是把变参寄存器当成**固定参数**。memcached `FUN_293a0` 就是这样变成
  `(i64 %RDI.arg, ..., i64 %R9.arg)`，调用方为了凑参数不得不给 R8/R9 造值（当下 4 条 R9 unknown
  的来源）。
- 管线位置：`runNativeRegisterSummarySSA` 开头 `truncateKnownNoReturnCalls` →
  **`runNativeStackFrameRewrite`(:8795)** → canary → `canonicalizeRegisterPointerPhiLoads` →
  pre-summary peephole(:8806) → base/final summary → shape → builder(SSA) → rewrite → cleanup。
  变参识别要放在 peephole 之后、base summary 之前：此时帧访问已是 alloca+GEP 形式（好匹配），
  而且删掉保存区读取后，summary 自然不会再记 ReadEntry、不会再生成变参寄存器参数。

## 参考实现（ground truth，本地可复现）

LLVM 22 源码：
- `llvm/lib/Target/X86/X86ISelLowering.cpp:26203` `X86TargetLowering::LowerVASTART`：
  `__va_list_tag` = { i32 gp_offset(0..48); i32 fp_offset(48..176); ptr overflow_arg_area; ptr reg_save_area }。
- 同文件 `:26261` `LowerVAARG`：ArgMode=2（float ≤16B，用 fp_offset，步长 16）/
  ArgMode=1（integer ≤32B，用 gp_offset，步长 8）；`f80` 直接 assert 不支持。
- 保存区大小：GP 6×8=48B，FP 8×16=128B（`VASTART_SAVE_XMM_REGS`，由 AL 决定是否保存）。

实测（本机 LLVM 22）：
- `/tmp/va/va_test.c` + `clang -O2 -S`（注意 -O2 会跳过固定参数的保存槽）：
  `subq $88,%rsp; movq %rsi,-88(%rsp)..movq %r9,-56(%rsp); testb %al,%al; je; movaps %xmm0,-48(%rsp)..;
  leaq -96(%rsp),%rax; movq %rax,-112(%rsp); leaq 96(%rsp),%rax; movq %rax,-120(%rsp);
  movabsq $0x3000000008,%rax; movq %rax,-128(%rsp)`（gp_offset=8、fp_offset=48）。
- `/tmp/va/va_test.ll`（手写 `llvm.va_start`+`va_arg`）+`llc -O2` 的形状与上面一致，
  va_arg(GP) 展开为：`movl gp_offset,%ecx; cmpl $48,%ecx; jae overflow; movq reg_save_area,%rax;
  addq %rcx,%rax; addl $8,%ecx; movl %ecx,gp_offset; jmp done; overflow: movq overflow_arg_area,%rax;
  addq $8,..; done: movslq (%rax),%rax`。
- 结论：**我们要识别的模式与 LLVM 自己的 lowering 一一对应**，识别出来以后可以直接用
  `llvm.va_start/va_arg/va_end` 表达，不需要自己造 ABI。

## 在 lift 里的对应模式（memcached `FUN_293a0`，raw lift `mc.pressa.ll`，行号见文件）

```llvm
%unique_4800_8  = add i64 %RBP, -176          ; 保存区基址 = RBP-208，R8 落在 +32
%unique_6b00_8  = load i64, ptr @R8
%notdec_ram_ptr25 = inttoptr i64 %unique_4800_8 to ptr
store i64 %unique_6b00_8, ptr %notdec_ram_ptr25
%unique_4800_827 = add i64 %RBP26, -168       ; R9 落在 +40（与 R8 相邻 8 字节）
...
%AL = call i8 @notdec.partial_read.i64.i8(ptr @RAX, i64 0)   ; test al, al
... 条件分支到 XMM 保存块（movaps 8 条）...
```

帧重写之后（pass 输入，`pre-summary-mc.dump` FUN_293a0）同样的东西变成：

```llvm
store i64 %unique_6b00_8,   ptr %notdec_stack.native.ptr184   ; alloca+208 = 保存区+32 (R8)
store i64 %unique_6b00_828, ptr %notdec_stack.native.ptr185   ; alloca+216 = 保存区+40 (R9)
%AL = call i8 @notdec.partial_read.i64.i8(ptr nonnull @RAX, i64 0)
store i32 32, ptr %notdec_stack.native.ptr196                 ; va_list+0  gp_offset = 32 → 4 个固定 GP
store i32 48, ptr %notdec_stack.native.ptr197                 ; va_list+4  fp_offset = 48 → 0 个固定 FP
store i64 <reg_save_area>, ptr %notdec_stack.native.ptr198    ; va_list+16 = alloca+176
store i64 <overflow_arg_area>, ptr %notdec_stack.native.ptr195; va_list+8
```

识别特征（最小集）：
1. **保存区**：≥2 个不同 ABI 参数寄存器的入口值被 store 到同一 alloca 的连续 8 字节槽，
   且 `slot_offset - 8*reg_index` 对所有寄存器一致（= 保存区基址）。
2. **va_list**：同一函数里有 `store i32 48` 和 `store i32 8k`（相邻 4 字节）——k 就是固定 GP 个数；
   配套 `reg_save_area`/`overflow_arg_area` 两个指针字段。
3. **AL 测试**：函数里有 `partial_read.i64.i8(@RAX,0)` 及其条件分支（XMM 保存块）。

原型验证（Python，`/tmp/va/detect3.py`，输入 `--no-register-ssa-pass` 的 raw lift）：
- memcached：命中 3 个真变参函数 —— `FUN_293a0`(gp_fixed=4, 保存 R8/R9)、
  `FUN_208d0`(gp_fixed=3, 保存 RCX/R8/R9)、`FUN_6a2d`(gp_fixed=1, 保存 RSI/RDX/RCX/R8/R9)；
- wrk：0 个内部变参函数（它的变参都是外部 printf 族，已由 prototype 覆盖）；
- 单寄存器"命中"（36 个）是保存槽算术偶然相等造成的假阳性，用 "≥2 个不同寄存器 + gp/fp 对 + AL"
  三重条件即可排除。

## 阶段、判断标准

### Stage 0：检测 + 元数据（零语义变化）
- 新增 `NativeVarArgDetect`（`include/notdec-bin2llvm/passes/summary/NativeVarArgDetect.h` +
  `lib/passes/summary/NativeVarArgDetect.cpp`），在 peephole 之后运行；识别上面三条特征，
  给函数挂 `!notdec.vararg = !{fixed_gp=k, fixed_fp=m, save_base=.., va_list=.., regs=..}`，
  并把统计写进 summary（可选打印）。
- 判断标准：memcached 命中 3 个且 k 与手工核对一致；wrk 0 个；IR 只有 metadata 变化；
  单测/ctest/fortune/verify 全绿。

### Stage 1：callee 重写 + 签名变参（本轮的正题）
- 同一个 pass 里在识别成功后做重写：
  1. 删掉保存区的 store（以及只喂给它们的 `load @R8/@R9/...`）和 AL 测试块（含 XMM 保存块）；
  2. 把手写 va_list 的 4 个字段 store 换成 `call void @llvm.va_start(ptr %ap)`，`%ap` 就放在
     原来的 va_list 帧地址上（同一 24 字节），**手工 va_arg 代码不用动**：它原本就读
     va_list 内存里的 gp_offset/reg_save_area，va_start 之后这些字段由后端保存区填好，
     语义等价（地址从"自己的保存区"变成"后端在函数入口保存的寄存器区"）；
  3. 记录 `fixed_gp/fixed_fp` 供 shape 用：`shapeForInternalFunction` 只把"被本函数直接读"
     的寄存器当固定参数，其余变参位 + `shape.VarArg = true`；调用点直接复用已有的
     `shape->VarArg` 路径（未证明的实参 → `vararg_unknown` 占位，不再当固定参数）。
- 判断标准：
  - 3 个函数签名变成 `(...)`；FUN_293a0 的 `%R9.arg` 消失；
  - 预期 memcached unknown 9 → 7 或更少（FUN_294b0 里为 FUN_293a0 补 R8/R9 的 4 条 range_unknown
    会因值不再被使用而被 `eraseDeadUnknownValueCalls` 清掉；FUN_15330 的 2 条不是变参问题，保留）；
  - `llvm-as` + `opt -passes=verify` 通过；ctest 12/12；fortune x86_64/i386 不回归；
  - vararg 相关 warning（`vararg_arg_uses_unknown_value` 等）有统计，便于盘点。

### Stage 2（可选，之后再说）：把手工 va_arg 折叠成 `va_arg`
- 只对"自己消费变参"的函数（如 `FUN_208d0`/`FUN_6a2d` 若确有 gp_offset 比较 + 双路径 + load），
  按 ground truth 展开形状折成 `%v = va_arg ptr %ap, i32` + `va_end`。
- 判断标准：这些函数里 va_list 手工代码消失、IR 形如 C 变参函数；签名/调用点不变。

## 风险

1. **调用点 AL 语义**（最大风险）：SysV 要求"调用变参函数前 AL = 使用的向量寄存器个数"。
   callee 侧重写成 va_start 后，保存由后端负责；但如果调用点原来是靠"手写保存区"隐式满足的，
   可能出现 AL 未设置/错的形态。lift 里调用点本来就有 `partial_write @RAX, <n>`（wrk `main`
   可见），需要先做实验确认调用点是否总能提供 AL，缺的话要在调用点补。
2. **删除的边界**：保存区的 store 必须确认"只被保存区使用"（有函数会把参数再存别处），
   只删"值只喂给保存区"的那些；AL 测试块要整块删（含 XMM movaps 链），别留下悬空分支。
3. **浮点变参**：只识别不重写 XMM 保存部分，交给 va_start；但 `fp_offset` 与 AL 的组合要在
   有浮点变参的函数上验证（memcached 这 3 个 fp_fixed 都是 0，需要构造用例或找其它样本）。
4. **fortune（i386）不在射程**：i386 的 va_list 是 `char*`，LowerVASTART 走另一条分支；本轮
   只做 x86-64 SysV，i386 检测到也不动。

## 不做什么

- 不动外部变参推断（`inferExternalCallShapes`）与 `vararg_unknown` 降级路径。
- 不支持 Win64、i386、`f80`/结构体传参变参（LLVM 自己也不支持 f80 va_arg）。
- 不引入新的"调用点 AL 推断模型"（先用 lift 里已有的 AL 写值）。
- 不做 va_copy、不做把 va_list 当普通结构体搬来搬去的形态。

## 实现记录（2026-09-19 续，Stage 0+1 合并落地）

用户确认："对，按这个推进试试吧"。

### 落地内容

- 新 pass：`include/notdec-bin2llvm/passes/summary/NativeVarArgRewrite.h` +
  `lib/passes/summary/NativeVarArgRewrite.cpp`（`lib/CMakeLists.txt` 注册），在
  `runNativeRegisterSummarySSA` 的 pre-summary peephole 之后、base summary 之前运行。
  - 识别（x86-64 SysV；任一条不满足就保守跳过、不半改）：≥2 个 ABI 参数寄存器的连续
    8 字节保存槽在同一 alloca 上（`offset - 8*regIndex` 一致）；同块内相邻 4 字节的
    `i32 48` / `i32 8k`（k = 固定 GP 数）；从 `partial_read.i64.i8(@RAX,0)` 出发的 AL
    测试传递闭包；FP 保存区（base+48..base+176）每个 store 都必须是可删的
    `partial_read.i512.i128(@ZMMi,0)` 且偏移与 ZMM 序号一致；va_list 4 个字段 store
    必须在同一个块里。
  - 重写：删保存区 store（GP/XMM）→ 折叠"AL 守卫侧变空"的条件分支 → 在 va_list 原位置
    插 `llvm.va_start(ptr %ap)`（复用原 GEP）→ 删手写 va_list 字段 store → 挂
    `!notdec.vararg`（fixed_gp/fixed_fp/save_area/va_list/saved）。手工 va_arg 代码不动：
    它读 va_list 内存，va_start 之后 `reg_save_area` 指向后端在入口保存的真实寄存器区，
    语义等价。
- 签名：`shapeForInternalFunction` 见 `!notdec.vararg` 就 `shape.VarArg = true`；固定参数
  仍由 ReadEntry 决定（保存区读删掉后，变参寄存器不再是入口参数）。
- 调用点：`callParamSlots` 在"变参 callee + 无 external callsite shape"时按 ABI 把固定参数
  之后的输入槽也当变参传下去（GP `IntegerInputsInOrder`；FP `FloatInputsInOrder` +
  `floatSlotForDemand` 全掩码）。未证明的实参交给已有的 `shouldPoisonUnprovenVarArg`
  → `vararg_unknown` → `lowerVarArgUnknownHelpers` 降级成 poison。
- CLI：`--no-vararg-recovery`（A/B 对照用）。
- 单测：`testVarArgPrologueBecomesVaStart`（识别 → va_start → 无残留变参读 → 签名带 `...`
  且只剩固定参数）。

### 实现中确认的两件计划外的事

1. **AL 守卫分支不是 SSA 依赖**：lift 里 `test al,al` 的标志经 `@ZF/@SF` 等 global 传递，
   分支条件读的是 `@ZF`。改成通用（且语义等价）的变换：折叠"一侧为空块、该块单前驱单后继"
   的条件分支，顺带收掉 XMM 保存块。
2. **PF 由 `llvm.ctpop.i8` 计算**：AL 闭包里会出现 `call range(i8 0, 9) i8 @llvm.ctpop.i8(...)`，
   闭包遍历必须允许纯 intrinsic，否则 3 个函数全部放弃。另外 `llvm.va_start` 是
   `llvm_anyptr_ty` 重载 intrinsic，`getOrInsertDeclaration` 必须带指针类型实参，否则
   LLVM 内部 `DecodeFixedType` 空指针崩。

### 结果

| 指标 | 改前（`--no-vararg-recovery`） | 改后 |
|---|---|---|
| memcached 识别 / 改写 | 3 / 0 | **3 / 3**（`FUN_6a2d`、`FUN_208d0`、`FUN_293a0`） |
| memcached 保存区 store / va_list 字段 / va_start | 0 / 0 / 0 | 34 / 12 / 3 |
| memcached unknown | 9 | **3**（剩 `entry_range_rejected:RAX`(FUN_1a380) 1 + CALLOTHER RDX 2） |
| memcached IR bytes / warning 行 / defines | 4,021,006 / 825 / 230 | 4,029,337 / 815 / 231 |
| wrk 识别 / 改写 | 0 / 0 | 0 / 0（无内部变参函数） |
| wrk unknown | 37 | **37**（分组不变） |
| 单测 / ctest / fortune x2 / `verify` | — | 全绿（ctest 12/12，含 fortune x86_64 + i386） |

改后的三个函数签名（`i8 %RAX.arg` 是改前就有的 AL 隐藏参数建模，见下）：

```llvm
define internal void @FUN_6a2d(i64 %RDI.arg, i8 %RAX.arg, ...) !notdec.vararg ...
define internal void @FUN_208d0(i64 %RDI.arg, i64 %RSI.arg, i64 %RDX.arg, i8 %RAX.arg, ...) !notdec.vararg ...
define internal void @FUN_293a0(i64 %RDI.arg, i64 %RDX.arg, i64 %RCX.arg, i8 %RAX.arg, ...) !notdec.vararg ...
```

调用点变成真变参，原来为 R8/R9 凑固定参数值的 6 条 `notdec.unknown` 全部消失：

```llvm
call void (i64, i64, i64, i8, ...) @FUN_293a0(i64 207008, i64 %RDI.arg, i64 205150, i8 0,
                                              i64 ptrtoint (ptr @FUN_c4c0 to i64), i64 %14, i64 %R9.clobber2065)
```

### 两个要留意的点

1. **raw 入口 load 6 → 7**：新增的是 `FUN_c4c0` 里 `bb_c4c9` 的 `%RAX.entry = load @RAX`
   （该函数在一条路径上直接返回入口 RAX，是真实的入口读；由既有的"MayEntryMask 允许、
   但没有对应参数"分支产生，不是这次新引入的建模错误）。A/B 也确认：`--no-vararg-recovery`
   下 RAX 入口 load 2 条，改后 3 条。
2. **`i8 %RAX.arg`（AL 隐藏参数）仍在签名里**：改前就有（RAX 作为返回寄存器被 ReadEntry
   加进参数）。现在函数已是变参，LLVM 在调用点自己会设置 AL，这个显式 i8 参数冗余但无害
   （callee 的 va_start 用的是后端保存的 AL）。清掉它属于另一件事（"返回寄存器不该因为
   入口读而变成参数"）。

## Stage 2："自己取变参"的折叠（2026-09-19 续）

用户要求："自己创建一些测试用例，把目前测不到的'自己取'这一块逻辑再完善；注释里说明设计
理念；并在 AGENTS.md 记录'参考编译器代码、用最少的代码匹配编译器会生成的模式'。"

- 实现 `foldManualVaArgSequences()`（`NativeVarArgRewrite.cpp`，声明在同名头文件）：
  按 LLVM `LowerVAARG`（`X86ISelLowering.cpp:26261`）的菱形匹配并折成 `va_arg`：
  `load i32 off(+0 GP/+4 FP)` → `icmp ult 48/176` → 双路径（`reg_save_area+off` / 
  `overflow_arg_area`）+ 偏移推进 → `phi`。折成 `%v = va_arg ptr %ap, T`，
  删掉两块并直接跳 join。在签名重写之后调用（那时 lift 的寄存器中转已展开，菱形是干净 SSA）。
- 关键约束：offset load 使用者、两块内所有非终结指令都必须在匹配集合里；两块必须单前驱、
  同一个 join/phi、值类型一致；溢出区步长只要求是常量（FP 是 16 vs 8）。
  任一条不满足整体跳过。
- 单测 `testManualVaArgDiamondBecomesVaArgInstruction`：GP(i32)/FP(double) 各折 1 条，
  外加"少了偏移推进"的反例必须原样保留。
- 设计理念写进 `NativeVarArgRewrite.cpp` 顶部注释与 `AGENTS.md` 新增的
  "模式匹配类 pass 的设计理念"一节。

验证：单测绿；ctest **12/12**（含 fortune x86_64/i386）；memcached 重跑：识别/改写仍 3/3、
unknown 仍 **3**、**0 处 va_arg 折叠**（样本里确实没有"自己取"的函数）——说明新增折叠对
现有样本无副作用，能力由单测覆盖。

## 多处 va_list 初始化：每个初始化点各还原一个 llvm.va_start（2026-09-23 续）

起因是交接文档 `logs/20260923-01-native-handover.md` 的 P1：同一个 va_list 在多条
路径上各初始化一次时，plan 只记最后一个 (gp,fp) 对，step 5 只删那个块的字段 store，
applyPlan 只插一个 `llvm.va_start` —— 另一条路径的手工 va_list 还在，但它指向的寄存器
保存区已经被删掉，是半改写（语义 bug）。实例：`wrk aprintf`（entry 里手工 va_list 之后
立刻 `__vsnprintf_chk(0,0,2,-1,fmt,ap)` 做长度探测）、`redis-cli FUN_20df0`
（`clusterManagerLog.lto_priv.0`，同形，用 `__vfprintf_chk`）。
用户要求："把背后的设计考虑写到注释和文档里"。

### 先确定"编译器会生成什么"（不猜，查源码）

- **标准**：C11 §7.16.1 要求每次 va_start/va_copy 在同函数里配一个 va_end；va_end 的
  规定里明确 va_list 之后可以 "reinitialized by the va_start or va_copy macro"。标准
  没有"一个函数只能 va_start 一次"或"一个函数只有一个 va_list"的限制。
- **clang**：对每个 `__builtin_va_start` 都发一个 `llvm.va_start`，没有去重、没有
  状态跟踪（`clang/lib/CodeGen/CGBuiltin.cpp` `EmitVAStartEnd`，调用点在 3179-3188）。
  `va_end` 同样发 intrinsic（x86-64 后端展开成 no-op）；`va_copy` 发 `llvm.va_copy`，
  后端在 LP64 上是 24 字节 memcpy（`LowerVACOPY`，`X86ISelLowering.cpp:26319-26342`）
  ——所以 va_copy 在 lift 里是一段结构体拷贝，不会被常量对 matcher 当成初始化点。
- **LLVM X86 后端**：每个 `llvm.va_start` 独立展开成"以 ap 为 base 的固定 4 个字段
  store"：`i32 gp_offset@+0`、`i32 fp_offset@+4`、`overflow_arg_area@+8`、
  `reg_save_area@+16`（`LowerVASTART`，`X86ISelLowering.cpp:26203-26258`，
  注释里就写着 `__va_list_tag` 的布局）。多个 va_start 不合并。
- gp_offset/fp_offset 是**函数级**常量：`setVarArgsGPOffset(NumIntRegs*8)`、
  `setVarArgsFPOffset(48 + NumXMMRegs*16)`，reg save frame index 也是函数级
  （`X86ISelLoweringCall.cpp:1561-1563`）——同一函数里每个真 va_start 写的 gp/fp
  常量必然相同。
- verifier 对 `llvm.va_start` 只查"所在函数是 vararg"
  （`llvm/lib/IR/Verifier.cpp:6396`），没有"只能在 entry 块 / 只能一个"的限制
  （现有实现本来就插在非 entry 块）。

⇒ **一个 gp_offset store = 一次源 va_start**，是机器码↔IR 的 1:1 映射；多处初始化不是
编译器重复生成，而是多次合法 va_start，必须逐一还原。

### 实现（`lib/passes/summary/NativeVarArgRewrite.cpp`）

- **数据结构**：`FunctionPlan` 里的 `VaListFieldStores`/`VaListInitBlock` 换成
  `std::vector<VaListWindow>`。窗口 = (基本块, va_list 槽)，装该槽的字段 store；窗口里
  再记 `GpStores` = 这个槽上的初始化点（一个 gp_offset store 一个）。两级分开的原因：
  同一个栈槽可能被两个 va_list 复用（标准允许，编译器照发两段完整序列），此时槽只该删
  一次、`llvm.va_start` 却要插两次，且第二次必须插在中间那次调用之后。
- **step 2**（~282-333）：每个匹配的 gp store 收进所在窗口的 `GpStores`，按指令去重
  （外层 fp、内层 gp 是笛卡尔积）；站点间 `fixed_gp` 一致性检查保留——gp_offset 是
  函数级常量，对不上说明其中一对是普通常量、不是 va_start 序列。
- **step 5**（~486-541）：按窗口收集字段 store（同块 + `[GpOffset, GpOffset+24)`；指针
  字段允许缺失，值可能经过寄存器中转）；每个窗口 <2 个字段 store → 整函数放弃；第 2 个
  及以后的窗口额外要求 gp+16（reg_save_area）处有一个 64 位、非常量的 store，普通结构体
  那种 {8k, 48} 常量对挡在这里。**为什么不比对它等于哪个保存区**：这个阶段 lift 的指针值
  还可能经过寄存器中转（实测 store 的值是 `load i64, ptr @RAX`），比对就要追寄存器
  数据流——本 pass 不做通用数据流推断，所以只认形态。第一个窗口沿用整函数证据（保存区 +
  AL 测试 + gp/fp 相邻），不额外收紧，避免把现在能识别的单点函数挡掉。
- **applyPlan step 3**（~616-645）：每个初始化点插一个 `llvm.va_start`，ap 指针取该点
  gp_offset store 的 pointer operand（编译器 4 个字段 store 共用同一个 ap）；窗口第一个
  点仍插在该窗口最早的字段 store 前（保持旧行为），同窗口后续点插在自己的 gp store 前。
  metadata `va_list=` 记第一个点（全仓库只有 `nativeVarArgInfo()` 读它，测试只看
  FixedGp）。
- 删掉了 `vaListPointer == nullptr` 时的 entry-GEP 兜底：每个站点必然有 gp store，
  它是死代码。

### 单测（`tests/native_register_summary_ssa_test.cpp`）

三个 helper（`emitManualVaListInit` / `emitVarArgAlTestAndXmmSave` /
`buildVarArgTestFixture`）+ 四条用例：

1. `testVarArgPrologueWithTwoInitsInDifferentBlocksIsRewritten`：两个块各一次
   （wrk/redis 的真实形态）→ 2 个 va_start、字段 store 全删、
   `VaListFieldStoresRemoved == 6`。
2. `testVarArgPrologueWithTwoInitsInOneBlockIsRewritten`：同一块里隔着一次调用两次
   初始化（两个 va_list 复用一个栈槽）→ 2 个 va_start，且断言第二个在第一次调用之后。
3. `testVarArgPrologueWithTwoVaListObjectsIsRewritten`：一个函数两个 va_list 对象
   （不同槽）→ 2 个 va_start，参数分别是对应的 ap 指针。
4. `testVarArgPrologueWithConflictingInitsIsRejected`：两个点 gp_offset 不同 →
   整函数放弃，手工 va_list 一个 store 都不动。

（顺带修了测试计数器的一个坑：`stripPointerCasts()` 会把全零 GEP 折成 alloca，偏移 0
的 gp_offset store 要按"alloca = offset 0"处理，否则 `== 0` 的断言会空过。）


### 验证

- 单测全绿（4 条新用例）。把 pass 改动 stash 掉重跑做对照：前 3 条按预期失败
  （"did not insert two llvm.va_start" / "did not keep two va_start"），第 4 条
  （冲突拒绝）本来就该绿。
- ctest **12/12**（含 fortune x86_64/i386）。
- 7 目标同条件重跑（`/tmp/fix14` vs `/tmp/fix13`，命令同 `/tmp/fix13b_run.sh`
  换成 fix14；lighttpd 单独一次 506s）：
  - `notdec.unknown` 全部持平 **48 / 7 / 8 / 8 / 0 / 0 / 0**
    （lighttpd / redis-cli / libuv / ffmpeg / vsftpd / memcached / wrk）；
  - 7 个 IR 全部 `llvm-as` + `opt -passes=verify` **OK**；
  - vararg 统计只有两处变化，正好是这次修的两个函数：redis-cli
    `va_list_fields_removed 36→40`、`va_start_inserted 9→10`（FUN_20df0）；
    wrk `3→7`、`1→2`（aprintf）；其余目标逐项相同；
  - `aprintf` / `FUN_20df0` 最终 IR：各 **2 个 `llvm.va_start`**、va_list
    窗口里 0 个手工常量 store、`!notdec.vararg` 在。
- va_list 半改写扫描（`/tmp/va_scan6.py` 指向 fix14）：7 个目标 **half-rewritten 全空**；
  `undetected` 仍是已知三条：lighttpd `log_pri_multiline`（P2，fixed_gp=6）、
  vsftpd `FUN_1c630` / ffmpeg `FUN_3c030`（交接文档里 objdump 确认过的误报）。
- 度量注意：这套 pipeline 本身有 run-to-run 非确定性（同一个二进制连跑两遍 vsftpd，
  IR 大小 1708019 vs 1708028 字节，diff 是 metadata 编号/SSA 重命名级别；在 vsftpd
  这种 0 个变参改写、pass 完全不改写的目标上也出现）。所以跨 run 比较只看
  unknown/warn/verify/vararg 计数和定点 IR 检查，不要拿 IR 字节数当判据。

### 留给后面的（本次不做）

- `fixed_gp=6`（lighttpd `log_pri_multiline`）：已在下面的 P2 一节修掉；base 从 XMM
  保存组反推，而不是从 reg_save_area 字段（那个值可能经过寄存器中转）。
- 同一个初始化点的字段 store 被 lift 分到不同基本块：现在仍按"gp store 所在块"收集，
  另一个块的字段 store 不会删（matcher 的跨块接受行为没动，收紧怕回归）。
- gp_offset 不是常量（走寄存器中转）的初始化点：本来就识别不到，不上数据流。

## P2：零 GP 保存槽（fixed_gp=6，lighttpd log_pri_multiline）（2026-09-23 续）

### 目标与 pass-time 证据

- DWARF：`void log_pri_multiline(log_error_st*, const char* restrict, unsigned, int,
  const char* restrict, size_t, const char* restrict, ...)` —— 7 个固定参数（前 6 个
  在 GP 寄存器、第 7 个在栈上），变参只在 XMM/栈。
- 汇编：`test %al,%al; je; movaps %xmm0..7 → -0xb0..-0x40(%rbp)`，没有任何 GP 保存。
- 临时 dump pass-time IR（getenv 钩子，查完已删）：AL 测试在 entry；8 个 XMM 保存落在
  frame+112..224；va_list 窗口在 frame+32（+32 gp=48、+36 fp=48、+40 overflow、
  +48 reg_save=`ptrtoint(frame+64)`）；XMM 组自洽推出 base=64，GP 区 [64,112) 无 store。
- 旧实现 step 1 要求至少一个 GP 保存槽，这类函数直接放弃（这正是它漏掉的原因）。

### 实现（`lib/passes/summary/NativeVarArgRewrite.cpp`）

- 新增 `deriveSaveAreaFromXmmSaves()`：GP 分组为空时，用"XMMk 存在 base+48+16k"
  反推 base——每个候选独立算一个 base，要求 ≥2 个候选、同一帧、base 全部一致，并且 GP 区
  `[base, base+48)` 一个 store 都没有。保存区本身仍由 step 4 的 FP 区间检查兜底
  （区间内每个 store 必须是对应槽位的 XMM 保存），推导只负责给出 base 假设。
  **为什么不用 va_list 的 reg_save_area 字段**：那个值在这个阶段可能还是
  `load i64, ptr @RAX` 这种寄存器中转形态（aprintf 就是），比对它要追寄存器数据流。
- 新守卫：零 GP 槽时必须 `fixed_gp == 6`，和已有的"单 GP 槽必须 fixed_gp == 5"
  对称——只有 6 个 GP 参数寄存器全是固定参数，编译器才会一个 GP 变参寄存器都不保存。
- 误报挡板已核：两个已知误报（vsftpd `FUN_1c630`、ffmpeg `FUN_3c030`）
  pass-time 一个 ZMM 保存都没有（dump 过），进不了这条路。

### 单测（`tests/native_register_summary_ssa_test.cpp`）

`testVarArgPrologueWithNoGpSavesIsRewritten`：fixture 加 `withGpSaves=false`
（一个 GP 槽都不存），尾部的 XMM 保存块固定存两个（+256/+272），va_list 写 gp_offset=48。
断言 detected/rewritten=1、`fixed_gp=6`、1 个 `llvm.va_start`、
`RegisterSavesRemoved==2`、窗口常量 store 清空。去掉 pass 改动这条会失败。

### 验证

- 单测绿；ctest **12/12**。
- 7 目标 `/tmp/fix15` vs `/tmp/fix14`：unknown 全持平 **48/7/8/8/0/0/0**；
  7 个 IR `llvm-as` + `opt -passes=verify` 全 OK；vararg 统计只有 lighttpd
  变化（detected 4→5、`register_saves_removed` 39→47、
  `va_list_fields_removed` 16→20、`va_start_inserted` 4→5），其余逐项相同。
- `log_pri_multiline` 最终 IR：1 个 `llvm.va_start`、无手工窗口 store、签名
  `(i64 %RDI.arg, ..., i64 %R9.arg, i8 %RAX.arg, i64 %"stack+8.arg", ...)`
  ——和 DWARF 的 7 个固定参数一致。
- 扫描器（fix15）：half-rewritten 全空；undetected 只剩 vsftpd/ffmpeg 两个已知误报
  （`log_pri_multiline` 已从 undetected 消失）。

### 已知缺口（这次没解决，单独记）

- 3 个调用点（`h1_send_headers` / `h1_recv_headers` /
  `http_request_headers_process`）**没有把变参实参传进来**：二进制里
  `0x254cd` 有 `push %rdx`（第 8 个参数，也就是第一个变参），但 IR 调用只传
  7 个固定参数 + AL；fix14/fix15 的调用点逐行相同，不是这次引入的。
  机制：调用点的变参实参证据在识别之前就没了——callee 的 XMM 保存读被
  `partialReadFeedsOnlyDeadFrameSlots`（NativeRegisterSummary.cpp:98）判成"只喂没人
  本地读的帧槽"而不产生寄存器需求；栈上的变参则通过逃逸的 va_list 被外面读，同样看不见。
  要补上得让这条规则考虑"帧地址被拿出去用"（发现 2 的 demand 侧），见下一节。
- 度量补充：warning 计数也会 run-to-run 抖动（libuv 在 fix14 那次是 68，用 P1/P2 两个
  二进制各重跑两次都是 66），跨 run 只比 unknown/verify/vararg 计数。

### 追加：调用点的栈上变参（P2 的配套，2026-09-23 续）

P2 认出 `log_pri_multiline` 之后做 A/B 发现：它的 3 个调用点仍然只传 7 个固定参数
+ AL，而汇编里这几个调用点明明在 push 变参实参（`0x254cd: push %rdx` 等）。原因不在
callee：`callParamSlots` 对内部变参 callee 只补"剩下的 ABI 输入寄存器槽"
（`appendAbiVarArgSlots`），**没有栈槽**；fixed_gp=6 时变参只能走栈，于是没有槽可以
绑，调用点的 outgoing store 被当死值删掉。

**实现**（`NativeRegisterSummarySSA.cpp`，`callParamSlots` 的 vararg 分支）：
新增 `appendAbiStackVarArgSlots(call, slots)`。从 shape 固定栈参数覆盖到的最后一个
偏移往后，按 ABI 的 stack 输入槽（cspec：`stack+8` 起、maxsize 500、align 8）逐个
补槽：每个候选先用 i64 探针定位该位置的最近一次 outgoing store，找不到就停；找到就按 store
的值类型建槽（浮点 → FloatStack，否则 8 字节 IntegerStack）。寄存器尾和栈尾分别在两趟
collection（`stackSlotsOnly`）里收集，第二趟只看栈槽，所以寄存器槽读不到值不会挡住
栈槽。

**验证**：
- lighttpd 的 3 个 `log_pri_multiline` 调用点都多出了那个变参 operand
  （`i64 %2` / `i64 %9` / `i64 poison`——第三个的值在上游已经不可证，走
  既有的 vararg_unknown→poison 路径）。
- 7 目标 `/tmp/fix16` vs `/tmp/fix15`：unknown 全持平 **48/7/8/8/0/0/0**、
  `llvm-as`+`opt -passes=verify` 全 OK；内部变参 callee 的调用点 operand 总数
  lighttpd 1898→1946、redis-cli 1645→1718、memcached 1301→1331，其余目标不变（即找回了
  151 个之前被丢掉的栈上变参 operand）；poison 只多 4 个（lighttpd）。
- 半改写扫描（fix16）仍全空、undetected 只剩两个已知误报。
- **没有单测**：手写 IR 里 `NativeStackAddressAnalysis` 不把 outgoing/incoming 栈槽判成
  lift 形态的 EntryStackPointer/相对组（push 形态和 reload 形态都试过，固定栈参数也绑不上），
  所以这条路径现在只由真实二进制的调用点验收。要补单测得用一段真实 lift 片段做 fixture。

**那次 poison 的根因（2026-09-23 追查，已更正）**：第三个调用点
（`http_request_headers_process`）在机器码里除了压格式串，还多压了一个 RDX——但它
**不是变参实参，是栈对齐的填充**。对照 lighttpd **1.4.74** 源码（DWARF 记的行号 1366 与源码
逐行对得上；注意本地 `source-ir/src/lighttpd-1.4.82-new` 是另一个版本，行号差 47）：

    log_debug_multiline(r->conf.errh, __FILE__, __LINE__,
                        hdrs, r->rqst_header_len, "rqst: ");

后面没有实参；编译器为了 16 字节对齐多压了一个已经死掉的寄存器（同一个源码里别处用的是
`sub $8,%rsp`，那种不会被 lift 成 store，所以只有 push 形态的填充会多出一个槽）。lift 把
这个 push 建模成"往 outgoing 区写值"，栈尾规则就把它当成了一个变参槽；绑上去的值是前面几个
parse 调用的出口 RDX——工具把这三个 callee（`FUN_23a73`/`FUN_428d0`/`FUN_23a0d`，
DWARF 里都是 `int` 单返回值）的出口 RDX 过推断成了第二个返回值，调用点侧这个值又已经是
unknown（reason=`call_effect_mixed`），于是变成 poison。
对比：`h1.c:248` / `h1.c:497` 两处**确实是真变参**（`r->con->fd` / `con->fd`），
栈尾规则把它们正常恢复了（这也是 fix16 里 lighttpd +48 的主要来源）。
结论：栈尾规则目前分不出"真变参"和"对齐填充"，填充会在调用点多出一个（无害的）幽灵 operand
——callee 只按格式串取它需要的个数，永远不会读到填充槽。可选收紧：栈尾只传"值能证明"的槽，
遇到第一个不可证明的槽就停（真实例 147 个能保住，lighttpd 那 4 个 poison 会掉）。
另：那三个 callee 的"第二个返回值"是过推断（源码/DWARF 都是 int 单返回），如果哪天要收
`call_effect_mixed`，可以从这里入手评估。

### 追加 2：Mixed effect 也走统一占位（2026-09-23 续）

**问题**：第三个调用点压进栈的 RDX，来源是前面三个 parse 调用（`FUN_23a73` /
`FUN_428d0` / `FUN_23a0d`）出口的 RDX。这些 callee 把 RDX 当第 3 个参数读、又只在
部分路径写它 → summary 的出口 provenance 是 Mixed（`MayEntry && MayNonEntry`）。按位 SSA 的
调用点分派里只有 `Clobber`/`ReturnValue` 才生成"调用后状态"占位（`callRangeValue`），
Mixed 走的是 `markBlockRangeUnknown`：直接判丢，后面物化成不可替换的
`notdec.unknown`（`NativeRegisterSummarySSA.cpp` 的 6007 起）。于是签名里明明有 RDX
返回槽，调用点却拿不到 `extractvalue`，变参规则再把它换成 poison。

**背景**：`logs/20260805-12-native-unify-call-return-plan.md` 已经把"第一遍不再区分
return/clobber、统一生成占位、是不是返回由签名决定"落地了，但那次只把占位给了 Clobber，
Mixed 仍是旧路——这一条是把那次统一补完整。

**实现**（只动分派，不动 `callEffect` 的分类）：effect 是 Mixed 且
`signatureReturnUsesUnit(call, unit)` 为真（签名说这个寄存器在 callee 的返回槽里）时，
也走 `callRangeValue` 占位通道；不在返回槽里才保持原来的"判丢 + `call_effect_mixed`
归因"路径，省掉一次无意义的占位。
不动分类的原因：`callEffect` 还被区间事件统计（4721-4728）和调用点活跃性（5503-5506）
消费——Clobber 会擦掉"入口值可能活过这次调用"那条活跃性，Mixed 语义上不该擦。

**验证**（同一二进制，`/tmp/fix17` vs `/tmp/fix16`）：
- lighttpd 第三个调用点的变参 operand 从 `i64 poison` 变成真实值
  （`%notdec.reg.insert.lower339`，叶子是那三个 callee 的 `extractvalue …, 1`）。
- unknown 计数：lighttpd 48→48、redis-cli 7→7、libuv 8→8、**ffmpeg 8→5**
  （`call_effect_mixed:FUN_38200:ZMM0=3` 消失）、vsftpd/memcached/wrk 0→0；
  7 目标 `llvm-as`+verify 全 OK；ctest 12/12；半改写扫描仍全空。
- warning：lighttpd `vararg_arg_uses_unknown_value` 51→48、`return_binding_missing` 1→0；
  redis-cli 同项 1→0；libuv 2→0；没有新增 warning 类型。
- **没有单测**：手写小 fixture 里改前改后结果一样——清理遍会把调用后的读重新按签名解析，
  小 fixture 因此没有牙（实测：把 pass 改动 stash 掉重跑，那条用例照样绿）。差异只在"值在
  重写过程中就被抓走"的场合（变参 operand 绑定）出现，要复现得同时搭内部变参 callee + 栈参数，
  而手写 IR 触发不了栈参数的 lift 形态识别（见上一节）。


