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

