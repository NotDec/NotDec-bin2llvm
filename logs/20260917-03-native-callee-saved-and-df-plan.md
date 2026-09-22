# native 剩余 unknown：callee-saved 与 DF（plan + 记录）

## 用户原始 prompt

> 接下来再看其他的unknown吧

## 现状（上一轮之后）

- memcached：86 条 unknown。按寄存器：DF 22、RBP 20、RBX 18、R9 6、RDI 4、RDX 4、ZMM0 2、
  RSI 2、R8 1、RCX 1、RAX 1，其余 6 条是调用点 clobber；集中在 FUN_16bb0(34)、FUN_1c010(7)。
- wrk：57 条，主要是 RDX（28 条来自间接调用）+ x87 窗口槽 21 条。
- 证据（memcached FUN_16bb0 → FUN_15820）：

```llvm
%73 = call { i64, i64 } @FUN_15820()      ; callee 把 RBX/RBP 出口值当返回值带回
%RBP.range_unknown = call i64 @notdec.unknown.i64()
%RBX.range_unknown = call i32 @notdec.unknown.i32()
```

FUN_15820 自己只读入口 @RBX/@RBP、存进 native frame、在出口 phi 里读回来（= 原值），但它的
summary `modifies` 仍列出 RBX/RBP（存了再读回来证不出相等）。调用方于是把这两个 callee-saved
寄存器当成“调用后有了新值”，为它们造 unknown。

## 目标与判断标准

1. callee-saved（ABI unaffected）寄存器在内部调用后不再被当成被改写；memcached RBP/RBX 38 条
   unknown 消失，wrk 不回归；
2. DF：SysV 要求函数入口/返回时方向标志清零，入口与调用后的 DF 读应当解析成 0（memcached
   22 条）；
3. verify / ctest / fortune 全部保持通过；
4. 不改变任何寄存器在“真的被 callee 改写”时的行为（caller-saved 照旧）。

## 路线与取舍

- 方案 A（本轮做）：`applyFunctionEffect`（内部 callee 的总结效果合并进调用方状态）跳过
  `Abi.Unaffected` 里的寄存器——这与 `applyAbiCallClobbers` 已有的做法一致；ABI 保证 callee
  原样保留这些寄存器，callee 内部的 may_non_entry 不应外泄。
- 方案 B（候选）：DF 按 ABI 建模为“入口/调用后为 0”。入口值可以直接用常量 0 物化；调用后
  的 DF 仍按 ABI unaffected 处理（callee 必须清零返回）。
- 不动：caller-saved 寄存器的调用后 unknown（RDX/R9/RDI/RSI 等，是诚实结果）、x87 窗口、
  wrk 的间接调用返回模型。

## 风险

- 如果某个内部函数违反 ABI（真的把 RBX/RBP 改掉且不恢复），方案 A 会让调用方以为值没变；
  这与项目既有的 ABI 假设（x87 栈为空、callee-saved 保留）一致，且违反 ABI 的函数本来就会
  破坏外部调用方。
- DF 置 0 只在函数边界；函数内部 `std` 之后不跨调用的使用仍然按数据流走。

## 不做什么

- 不引入新的逃逸/别名分析；不改 unknown-external 返回模型；不动 x87。

## 实现与结果

三处改动：

1. `NativeRegisterSummary.cpp` `applyFunctionEffect`：合并内部 callee 的 summary 效果时跳过
   `Abi.Unaffected`（callee-saved）寄存器——与 `applyAbiCallClobbers` 对调用的做法一致；
2. `NativeRegisterSummarySSA.cpp` `callEffect`：把 `callPreservesRegisterByAbi` 提到 callee
   summary 事实之前，ABI 的 callee-saved 保证优先于 callee 的 `may_non_entry`；
3. DF：`isPostCallZeroRegister` 改名 `isAbiBoundaryZeroRegister`，并补两处——`entryRangeInput`
   里 DF 的入口值直接给常量 0；`callEffect` 里 DF 返回 `Clobber`（`callRangeValue` 已有
   “调用后为 0”的规则），不再走 `Unknown` 占位。

### 验证

| 指标 | 改前 | 改后 |
|---|---|---|
| memcached unknown | 86 | **26** |
| memcached IR bytes | 4,069,860 | 4,037,772 |
| memcached warning 行 | 829 | 822 |
| memcached defines | 230 | 230 |
| wrk unknown / bytes / warn / defines | 57 / 1,022,548 / 311 / 87 | 同左（不变） |
| fortune i386 unk / bytes / warn | 2 / 248,050 / 9 | 2 / 248,998 / 8 |
| fortune x86_64 unk / bytes / warn | 0 / 293,746 / 34 | 0 / 293,789 / 34 |
| verify（LLVM 22） | OK | OK |
| ctest / 单测 | 12/12 / 通过 | 12/12 / 通过 |

消失的 60 条：RBP 20 + RBX 18（callee-saved 被 callee summary 误标改写的级联）+ DF 22。
单测新增 `testCalleeSavedRegisterSurvivesInternalCall`（RBX 被 callee 读写过，调用方调用后
再读不能变 unknown）和 `testDirectionFlagIsZeroAtBoundaries`（DF 入口读、调用后读都是 0）。

### 剩下的 26（memcached）

- R9 6、RDI 4、RDX 4、RSI 2、R8 1、RCX 1、RAX 1：caller-saved 寄存器在调用后被读，值本来
  就是 callee 留下的未知数（诚实）；
- ZMM0 2：同类，浮点 caller-saved；
- 6 条调用点 clobber 残留：`notdec_pcode_CALLOTHER_i64` 2、fopen/fclose/strcmp 等。

### 下一步候选

1. `notdec_pcode_CALLOTHER_*` 的符号化（2 条 + 相关警告）；
2. wrk 的 unknown-external 返回模型：RDX 34 条（28 条来自间接调用）——收紧“未知外部调用
   返回寄存器”的判定，或给 http_parser 回调补原型；
3. wrk 的 x87 窗口 21 条（入口/调用后真实未定义，抽查后确认是否还有可恢复的）。

## 尝试：窄段入口值退回整寄存器（已量、已回退）

按用户要求试了“窄段自己的入口事实被否掉时，退回整寄存器入口值 + 取段”：在
`entryRangeInput` 里把 `rangeMayComeFromEntry` 从“直接 return nullptr”改成“整寄存器走
`entryRegisterLoad`、窄段走 `materializeCoveredAccess(整寄存器入口值, 段)`”。

**效果（memcached）**：unknown 26 → **12**，`main` 的 3 条 RDI 段 unknown 消失、改成 1 条
`load i64, ptr @RDI` + 取段；wrk 57 → 55；两个 IR verify OK。剩下的 12 条是 R9 6（内部
调用后读 caller-saved）和 4 条 `fopen`/`fclose`/`strcmp`/`CALLOTHER` 的 RDX clobber 残留。

**但单测挂了 2 个**（`raw RAX load remained after partial read rewrite`、
`branch full load left raw RAX load`）：这些用例明确要求“段级入口事实被否时不要合成
整寄存器 raw load”。也就是说 `rangeMayComeFromEntry` 这道闸门是**刻意的**：它宁可给
unknown/走 clobber，也不让 pass 凭空多出一条寄存器 global 读。

**结论与后续**：值本身是对的（memcached 上确实恢复了 argc），但绕过闸门会破坏
“partial read 消费后不应残留 raw 寄存器 load”的既有约定。正解应当是把**事实**修正对，
而不是在入口兜底处绕过它：

1. `rangeMayComeFromEntry` 用的是**整函数**级的 must 事实（“这些位在函数里被写过”），
   但挡住的是**当前位置**的判断；应改成按读取点判断（本块/前驱有没有这段的定义）；
2. 段级入口需求要升格成整寄存器需求，让 `main` 拿到 `%RDI.arg`（源码 IR 是
   `i32 @main(i32 %argc, ptr %argv)`）；
3. 6 条调用后 RDX 另查。

本轮不改代码（回退到 `3104cf7` 的状态：memcached 26、wrk 57），把测到的数据和
闸门原因记在这里。

## 下一步调查（已做一半，2026-09-17）

- 事实层面：当前 IR 里**有 5 个节点** `name=RDI, read_entry=true`（例 !136/!324/!354），说明
  “RDI 入口读记入需求”这条机制本身是通的；`main` 的 `read_entry=false, mask=0x` 是个案。
- `main` 原始 lift 里，第一次读 RDI 是 `partial_read.i64.i32(ptr @RDI, 0)`（argc），紧跟着就
  出现 `store i64 48, ptr @RDI`（RDI 被当 scratch 复用），说明 main 里 RDI 既有“入口读”又
  有“本函数写”，summary 的按位 `ReadEntry |= readMask & MayEntry` 在这条链上没记到入口读。
- 待做：对 `main` 的 RDI cell 做一次定点 trace（第一个 RDI 读之前到底有没有 @RDI 写、是
  `store` 还是 `partial_write`、在哪个块），确认是事实计算把它标成 non-entry，还是读取点
  判定（①）需要先修；随后按“① 按读取点判定 + ② 按位记需求”落地，验收标准：内部函数不得
  残留入口 raw 寄存器读，`main` 形如 `(i32 %argc, ptr %argv)`。


## 调查结论与落地：帧指针匹配（2026-09-18）

本轮用户要求（原话摘录）：

> 在 memcached 剩 26 条、wrk 剩 57 条 notdec.unknown 的前提下，先查清 main 的 RDI（argc）
> 为什么 read_entry=false，再修 entryRangeInput；不要绕过 rangeMayComeFromEntry 闸门。
> 验收：内部函数无入口 raw 读、main 形如 (i32 %argc, ptr %argv)、单测/ctest/fortune/verify 全绿。
> 补充要求：只针对编译器一般产生的 RBP 形态做简单匹配（"要么整函数是栈指针，要么完全不是"），
> 代码注释和文档要说清楚编译器对 RBP 的使用规律和当前代码的假设。

### 根因：事实计算错了，不是读取点判定

1. main 原始 lift 第一条 RDI 访问是 `partial_read.i64.i32(ptr @RDI, i64 0)`（argc），
   此前没有任何 @RDI 写，`store i64 48, ptr @RDI` 在其后——是真入口读。
2. 该读结果存进 `%notdec_stack.native.ptr10742`（alloca+280）。在 summary 看到的 IR 里
   这个 GEP 只有 store、没有 load，于是 `partialReadFeedsOnlyDeadFrameSlots` 判成
   "死溢出"：前向不记 `ReadEntry`、反向不产生需求。
3. 同一内存的读在 `bb_6e98`，拼写是早期帧重写没折过来的
   `inttoptr(add (%RBP268 = load @RBP), -1992)`（RBP=入口SP-8，-1992-8+2280=280）。
   晚期帧清理（SSA 重写之后）才把它折成 ptr10742（旧指标 `stack_frame_accesses_rewritten=591`）。
4. 早期没折的原因：`replaceFramePointerLoads` 的数据流把"前驱还没扫到"和"前驱已知没有值"
   都当成 `nullptr`，第一轮扫描时循环头被自己的回边毒化，结论自我确认、修不回来。
   用 Python 精确复刻 C++ 语义：main 821 个块只有 entry 一个块知道 RBP 的值，
   `bb_6e98` 的 39 个前驱全是"没有值"。
5. 因果链：丢读 → `addParamForUnit` 要求 `ReadEntry` → 不加 `%RDI.arg`；同时 range 重写的
   `rangeMayComeFromEntry` 用 exit 级 `MayEntryMask`（main 的 RDI 全 0）→ 3 条 unknown
   （offset 0/8/16）。所以修 `entryRangeInput` 是治标；闸门本身不用动。

### 中间实验（已量、已弃用）：帧指针数据流三值格

给数据流加 TOP（还没算）/ None / Value 三态，从全 TOP 单调下降迭代到最大不动点。
效果与最终方案相同（memcached 26→12、raw 入口 load 15→6、晚期帧清理 591→0），
但概念上要解释"为什么 TOP 不能当成 None"。补丁保留在
`/tmp/nb-frame/exp-frame-pointer-lattice.patch`。

### 采用方案：按编译器模型整函数匹配

LLVM 依据（`/sn640/NotDec/llvm-source`）：

- `X86RegisterInfo.cpp` `getFrameRegister`：`return TFI->hasFP(MF) ? FramePtr : StackPtr;`
  ——"哪个寄存器当帧基址"是整函数属性。
- `X86FrameLowering.cpp` `hasFPImpl` 全是整函数条件。
- 同文件 `assert(MF.getRegInfo().isReserved(MachineFramePtr) && "FP reserved")`：用帧指针时
  RBP 被保留，不参与寄存器分配。

机器码形态：序言 `push %rbp`（保存调用方值）+ `mov %rbp, %rsp`（或 `lea`）；收尾
`mov %rsp, %rbp; pop %rbp`（或 `leave`）。不用帧指针的函数没有"设置"，RBP 是普通
callee-saved 寄存器。

实测 memcached 的 pass 输入（201 个有 @RBP 写的函数）：恰好一次设置 191（187 有恢复 +
4 无恢复），完全没有设置 10，怪形态 0。

实现（`NativeStackFrame.cpp` `replaceFramePointerLoads`）：

1. 分类写：值由 RSP 派生的 store 是"设置"，其余（收尾恢复、`partial_write`、认不出的
   store）是"其它写"。写识别用"指针就是帧指针全局"+"访问元数据"两条判据，并把
   `partial_write` 也算写——堵住旧实现"看不见的写 → 拿过期值替换"的洞。
2. 设置必须唯一；不唯一整函数不折。
3. "其它写"之后可达的块用一次 BFS 标出来，这些块不折；写所在块内按指令顺序判断。
4. 其余位置只要唯一设置支配这条 load（`llvm::DominatorTree`），就把 `load @RBP` 替换成
   设置时存进去的值。

假设（每条不成立时只是少折，不会折错）：一个函数只用一种帧基址；调用不改帧指针
（ABI callee-saved）；认不出的写一律算写；毒化是块级的。

### 验证

| 指标 | 改前（89deb08） | 改后 |
|---|---|---|
| memcached unknown | 26 | **12**（重复 3 次一致） |
| memcached IR bytes | 4,037,772 | 4,025,704~4,025,706 |
| memcached warning / defines | 822 / 230 | 822 / 230 |
| memcached raw 入口寄存器 load | 15 | **6**（余下 6 条 pre-existing） |
| wrk unknown / bytes / warn / defines | 57 / 1,022,548 / 311 / 87 | 同左 |
| main 签名 | `i64 @main(i64 %RSI.arg)` | `i64 @main(i32 %RDI.arg, i64 %RSI.arg)` |
| 单测（含 3 个新用例） | 通过 | 通过 |
| ctest | 12/12 | 12/12 |
| fortune i386 | 2 / 248,998 / 8 | 2 / 248,998 / 8 |
| fortune x86_64 | 0 / 293,789 / 34 | 0 / 293,754 / 34 |
| `opt -passes=verify` | OK | OK |

消失的 14 条：main 3 条 RDI + `FUN_2d310`/`FUN_18bd0`/`FUN_176e0`/`FUN_18130`/
`FUN_e460`/`FUN_14030` 等函数的入口段读；这些函数各自长出正确参数（如 `FUN_18130` 加
`i32 %RDI.arg`、`FUN_18bd0` 加 `double %ZMM0.arg`）。顺带消掉 9 条本来违反红线的
`%RSP.entry`/`%RBP.entry` raw 入口 load（FUN_de20/e460/f780/300c0/f920/15820/2f2e0）。

新增单测（`tests/native_register_summary_ssa_test.cpp`，都走
`runNativeStackFrameRewrite` 公开入口）：

- `testFramePointerLoadFoldsAcrossLoop`：循环体里的 `load @RBP` 必须折掉（原始 bug）。
- `testFramePointerLoadAfterRestoreIsKept`：收尾恢复之后的 `load @RBP` 不能折。
- `testTwoFramePointerSettersAreNotFolded`：两次互不支配的设置 → 整函数不折。

### 代价与遗留

- 毒化是块级的：收尾恢复块可达的块里，即使某条具体路径不经过恢复也不折（实测
  memcached/wrk 无代价）。收益是代码里没有不动点、没有值身份比较。
- 不匹配编译器形态的函数保持原样，只是少折。
- memcached 剩余 12 条 = R9 6（`FUN_294b0` 入口段）+ RDX 5（CALLOTHER×2 / fopen / fclose /
  strcmp）+ RAX 1（`FUN_1a380` 只有一句 `ret unknown`）；wrk 57 条不变。
- 另有 6 条 pre-existing raw 入口 load（FUN_19ac0/19b10 RAX、FUN_22b30 RSP、FUN_2be30
  R14/R15、main RBX）不在本轮射程。


## 死 unknown 的来龙去脉（2026-09-18 续）

本轮用户问题（原话）：

> 看看那个死代码真的是死代码吗，那段部分没被用到为什么会创建对应 unknown?

调查发现两类，都是清理时序问题，不是"为死代码创建 unknown"：

### 一类：创建时活着，之后才变死（strcmp 那条 + wrk 20 条）

- unknown 代表的读是真实的。`FUN_1e900` 里 strcmp 之后原始 lift 读
  `%EDX = partial_read.i64.i32(ptr @RDX, 0)`（本块只写了 DL，高 24 位追溯到调用点），
  值一路走 `partial_write @R15` - 读回 - zext - store @R15。
- 后续签名重写 / 死 store 删除 / InstCombine 把消费者链删掉，unknown 变成没人用。
- tool 的 pipeline 顺序是关键：`SSA 重写 - InstCombine - peephole - final cleanup
  （内部有 eraseDeadUnknownValueCalls）- 再一次 InstCombine - 写文件`。最后一次
  InstCombine 跑在死 unknown 清理**之后**（它用来折叠 final cleanup 降级出的
  insert/extract 链），它删掉的最后一个使用者没人再扫。wrk 的 57 条里有 20 条属于
  这种残留（`unknowns.py` 标 dead/unused=20）。
- 修法：把 `eraseDeadUnknownValueCalls` 从匿名命名空间提出来（声明进
  `NativeRegisterFinalCleanup.h`），在 tool 两条 pipeline 的最后一次 InstCombine
  之后各补一次。

### 二类：创建时就死了（fopen/fclose 的 RDX 两条）

- 加 trace 后看到 `[helper-trace] uses=0 reg=RDX type=i24`：`rewriteSignatureShapes`
  解析 helper 时它在 IR 里已经没有使用者，代码不看 `use_empty()` 就物化
  `notdec.unknown` 并写进内部 valueMap；后续链通过 valueMap 引用到它，于是"为死
  helper 造的 unknown"反而被用了。
- 分类本身也不对：RDX 在 fopen 之后是 clobber（fopen 只返回 RAX），本该像同一调用
  点的 RCX/R8/R9 一样保留 `summary_clobber` 占位，却被当成"返回槽解析失败"。
- 修法：解析失败且 `helper->use_empty()` 时删 helper、不造 unknown；能解析出真实
  返回值的死 helper 仍照常解析（避免丢返回值）。

### 结果

| 指标 | 改前 | 改后 |
|---|---|---|
| memcached unknown | 12 | **9** |
| memcached IR bytes | 4,025,704 | 4,021,243 |
| memcached warning | 822 | 828 |
| wrk unknown | 57 | **37**（剩余全部 dead=0；删掉的 20 条正是原 dead 集合） |
| wrk IR bytes | 1,022,621 | 1,019,316 |
| wrk warning | 311 | 311 |
| 单测 / ctest / fortune x2 / verify | — | 全绿（ctest 12/12，fortune 两个退出 0） |

warning 822 增加到 828 的 +6 全是 `remaining_summary_clobber_value`（原来被 unknown 掩盖的
clobber 占位现在如实报告；同一调用点其它寄存器本来就有同类 warning），另有
`call_arg_uses_unknown_value` 变成更准确的 `call_arg_uses_clobber_value`。

单测新增 `testEraseDeadUnknownValueCalls`（无使用者的 `notdec.unknown` 被清掉、
有使用者的保留）。二类修复需要 pass 内部 valueMap 链才能构造，单测不易复现，由 CLI
层的 unknown 计数与 fortune residue audit 覆盖。

### 剩余

- memcached 9 条 = R9 6（`FUN_294b0` 循环内 range phi/段解析）+ CALLOTHER 2
  （`FUN_1fa40`）+ RAX 1（`FUN_1a380` 整函数 `ret unknown`）。
- wrk 37 条 = 间接调用返回模型 14（`callee:<indirect>`）+ ST0/ST1 窗口 15 +
  `__printf_chk` 2 + strcmp 2 + gettimeofday 1 + 其它。
- 另有 6 条 pre-existing raw 入口 load 不在射程。

## unknown 归因与成因盘点（2026-09-19 续）

本轮用户要求（原话）：

> 为什么是创建 unknown 值？而不是保留原始的寄存器操作呢？现在的 unknown 调用的方式感觉
> 有点太偏离语义了

> 对 先把参数用起来吧，有个参数的语义还是稍微清楚一些

> 对 归因这步做一下吧，然后再盘点一下当前的问题到底是什么，如果要修复的话，最好怎么做

### 上一轮结论更正：4 条 R9 不是"虚报"

上一轮根据事实表面（`!271` 有 `read_entry=true/read_entry_mask=全 1` 但 `may_entry_mask=0`）判断
memcached `FUN_294b0` 的 4 条 R9 unknown（`bb_29500`/`bb_29c30` 各 2）是 `rangeMayComeFromEntry`
口径不一致造成的虚报，**这是错的**：漏看了 `FUN_294b0` entry 块里的 `call void @FUN_17e10()`
（pre-SSA 275160，entry 块里唯一的非 helper 调用）。加 trace 跑完整 memcached 后的真实链路：

```text
[read-null]      blk=entry      reg=R9 off=0 w=32 phase=build reason=unknown-marked
[exit-resolved]  blk=entry      reg=R9 off=0 w=32 value=null cached=0
[entry-resolved] blk=bb_29c30   reg=R9 off=0 w=32 value=null
[read-null]      blk=bb_29c30   reg=R9 off=0 w=32 reason=entry-null
[phi-unknown]    fn=FUN_294b0   reg=R9 off=0 w=32 pred=bb_29c30 exit_null=1
```

- entry 块自己被 `markBlockRangeUnknown`：FUN_17e10 的 R9 summary 是 Mixed
  （`!250 = name=R9, may_entry=true/mask 全 1, may_non_entry=true/mask 全 1`；FUN_17e10 的签名只有
  `%RDI.arg`，说明它内部把 R9 当 caller-saved 临时寄存器写）→ `callEffect` 走 Unknown →
  标记 entry 的 R9[0,32)/[32,64)。
- entry 被标记后 `readRangeBefore(entry, ...)` 在 `UnknownCurrentDef` 闸门直接得 null，于是
  bb_29c30/bb_29500/bb_29542 的 exit 解析全为 null，`completeRangePhi` 兜底物化 unknown。
  trace 里根本没有 `[entry] fn=FUN_294b0 reg=R9` 这一行 —— 怀疑的 mask 口径问题在这 6 条上
  没被走到。
- 所以 6 条 R9 全是"调用边界的真实丢失"；`%R9.arg` 是死参数（唯一需要入口 R9 的读是
  `call @FUN_293a0(...)` 的第 5 个参数，它在 FUN_17e10 之后，入口值不能顶替）。
- "把参数用起来"这条路线对本例不适用，已放弃。

### 实现：unknown 成因归因

目标：`notdec.unknown` 的 `!notdec.unknown.source` 不再只有一个笼统的 `range_unknown`，而是能说出
"是哪个 callee 在调用边界把它弄丢的"，并能在批跑输出里按成因盘点。

- `lib/passes/summary/NativeRegisterSummarySSA.cpp`
  - `:3518` `struct RangeLossCause {Reason, Callee}` + `:3522` `RangeLossCauses`
    （与 `UnknownCurrentDef` 同 key；只做诊断归因，不参与任何值决策）。
  - `:5814` `markBlockRangeUnknown(block, range, cause)` 记录成因；`writeSegment`/
    `writeCoveredValue`/call-value 写入处分三处 `RangeLossCauses.erase`。
  - `:6456` `unknownCallEffectCause()`：`callee_summary_missing`（内部 callee 无 facts）vs
    `call_effect_mixed`（Mixed；带 callee 名，间接调用记 `<indirect>`）。
  - `:5990` `readRangeEntry` 结果为 null 时沿 pred 继承成因；`:6418` 闸门拒绝记
    `entry_range_rejected`，入口值取不到记 `entry_value_unavailable`。
  - `:5613` `attributedRangeUnknown()`：phi 兜底/缺段兜底统一走它，把 reason+callee 写进 metadata；
    `unknownRangeBefore` 增加 callee 参数。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:430` `countRemainingUnknownValues()`：
  按 `reason:callee:register` 统计最终 IR 里剩下的 `notdec.unknown.*`；声明在
  `include/notdec-bin2llvm/passes/summary/NativeRegisterFinalCleanup.h:52`。
- `tools/notdec-native-llvm.cpp:559` `reportRemainingUnknownValues()`：在两个 pipeline 的最后一次
  `eraseDeadUnknownValueCalls` 之后打印一行直方图，**只在 `--register-ssa-summary` 下打印**
  （默认 stderr 必须干净：`scripts/native-llvm-cli-signature-rewrite-smoke.sh` 用 `require_empty`）。
- `tests/native_register_summary_ssa_test.cpp:10770`
  `testMixedCallBoundaryAttributesUnknownToCallee`：造一个"一条路径写 RDI、另一条不动"的 Mixed
  callee，caller 在调用后跨块读 RDI，断言 unknown 带 `reason=call_effect_mixed` +
  `callee=mixed_rdi_callee`，且计数按该 key 归类。
  坑：测试里两个分支不能都是空块，否则 SimplifyCFG 折掉 phi，InstCombine 重建 unknown 时会丢
  metadata（归因只能算 best-effort）。

### 验证

| 指标 | 改前 | 改后 |
|---|---|---|
| memcached unknown | 9 | **9**（不变，原因见盘点） |
| wrk unknown | 37 | **37**（不变） |
| 单测 / ctest | 绿 / 12-12 | 绿 / **12/12**（含 fortune x86_64+i386） |
| memcached/wrk `llvm-as` + `opt -passes=verify` | OK | OK |

新增归因输出（`--register-ssa-summary`，两次运行一致；unknown 计数与分组集合一致，IR 字节有
已知 ±百字节级指针序抖动）：

```text
memcached total=9  [call_effect_mixed:FUN_15330:R9=2, call_effect_mixed:FUN_17e10:R9=4,
                    entry_range_rejected:RAX=1,
                    range_return_helper_clobbered_register:notdec_pcode_CALLOTHER_i64:RDX=2]
wrk       total=37 [entry_range_rejected:ST0=7, entry_range_rejected:ST1=8,
                    range_return_helper_clobbered_register:<indirect>:RDX=14,
                    range_return_helper_clobbered_register:__printf_chk:ST0=2,
                    range_return_helper_clobbered_register:strcmp:ST0=2,
                    range_return_helper_clobbered_register:gettimeofday:ST0=1,
                    range_return_helper_clobbered_register:luaL_checknumber:ST0=1,
                    range_return_helper_clobbered_register:luaL_checkudata:RDX=1,
                    range_return_helper_clobbered_register:memchr:RDX=1]
```

### 盘点：剩下的 unknown 到底是什么（memcached 9 + wrk 37 = 46）

| 组 | 数量 | 性质 | 建议 |
|---|---|---|---|
| `call_effect_mixed:<callee>:R9` | 6（mc） | 调用边界真实丢失：callee 的出口 provenance 是 Mixed（可能保留、可能改写），调用方拿不到 | **不改**；要消除只能让 Mixed callee 把该寄存器当返回值带回来（P3） |
| `range_return_helper_clobbered_register` | 24（mc 2 + wrk 22） | 被请求的寄存器在该调用点**不是返回值而是 clobber**：`__printf_chk`/`strcmp`/`gettimeofday` 的 ST0、间接调用的 RDX、CALLOTHER 的 RDX。同一调用点的其它 clobber 寄存器已经有 `notdec.register.summary_clobber` 占位（wrk `gettimeofday` 处 ST0=unknown 与 ST1=clobber 并排，就是证据） | **P1：接到 call-tied clobber 占位** |
| `entry_range_rejected:ST0/ST1` | 15（wrk） | x87 窗口临时值：入口 ST0/ST1 本来就不是"调用方传进来的值"（见 `logs/20260917-01-native-x87-window-shift-load-plan.md`） | **暂不动**；要动属于表示层或窗口模型扩展 |
| `entry_range_rejected:RAX` | 1（mc `FUN_1a380`） | `%RAXentry_range_rejected = call @notdec.unknown.i64(); ret`：返回槽解析失败落到入口 range 路径 | 单独小调查（根因在返回绑定，不在入口闸门） |

### 如果要修，最好怎么做

- **P1（建议下一轮，收益最大、语义最干净）**：`range_return_helper_clobbered_register` 的兜底改成
  与调用点绑定的 clobber 占位。改点在 `NativeRegisterSummarySSA.cpp:8622` 附近（`RangeReturnHelpers`
  解析失败且"不在返回槽内"的分支，当前直接 `unknownValueAt`），复用同一调用点已有的
  clobber 占位工厂（`notdec.register.summary_clobber.<ty>` + `!notdec.register.summary_ssa.call_value`）。
  预计 memcached 9→7、wrk 37→15，之后 unknown 计数基本只反映真正的建模缺口。风险：warning 口径
  变化（`call_arg_uses_unknown_value` → `call_arg_uses_clobber_value`、`remaining_summary_clobber_value`
  增加）要同步期望值；间接调用点的 clobber 占位唯一性要确认。
- **P2**：ST0/ST1 入口窗口那 15 条另立策略（`entry_range_rejected` 这个 reason 已经把"窗口拒绝"
  和"调用边界丢失"分开，先留作基线）。
- **P3（不建议现在做）**：让 Mixed callee 返回被改写的寄存器（调用点对 Mixed 位也产生 exit
  demand、callee 加返回槽）。能消掉 memcached 6 条，但那是把机器级临时值如实带回，签名/调用点
  改动面大、语义收益低；真要做，先用 P1 把"占位与调用点绑定"铺好。
- **P4**：`FUN_1a380` 的 1 条 RAX 单独看。

### 本轮不做什么

- 不改 `rangeMayComeFromEntry` 的 mask 选择：怀疑的口径问题在本例没被走到，改了消不掉这 6 条，
  反而会把"入口值被拒绝"与"调用边界丢失"混在一起；现在 `entry_range_rejected` 已能把两者分开。
- 不动 x87 窗口建模和间接调用返回模型（P1 先把表示层统一，再谈精度）。

## P1 落地：clobbered 返回 helper 改用 call-tied clobber 占位（2026-09-19 续）

盘点里的 P1：\`range_return_helper_clobbered_register\`（被请求的寄存器在该调用点不是返回值
而是 clobber）以前直接物化 \`notdec.unknown\`，而同一调用点其它 clobber 寄存器已经有
\`notdec.register.summary_clobber.<ty>\` 占位。

- 改点：\`lib/passes/summary/NativeRegisterSummarySSA.cpp\`（\`rewriteSignatureShapes\` 里
  \`RangeReturnHelpers\` 解析失败且不在返回槽内的分支）。现在创建与调用点绑定的 clobber
  占位（同名 helper + \`!notdec.register.summary_ssa.call_value\` 的 name/kind=clobber/
  call_index 元数据），不再造 unknown；在返回槽里却解析失败的情况保持 unknown 不变。
- 效果（A/B，同一命令）：

| 指标 | 改前 | 改后 |
|---|---|---|
| memcached unknown | 3 | **1**（只剩 \`entry_range_rejected:RAX\`，FUN_1a380） |
| wrk unknown | 37 | **15**（只剩 \`entry_range_rejected:ST0/ST1\` 窗口） |
| memcached warning 行 | 815 | 821 |
| wrk warning 行 | 312 | 352 |
| 单测 / ctest / fortune x2 / verify | — | 全绿（ctest 12/12） |

warning 增加的都是原来被 unknown 掩盖的 \`remaining_summary_clobber_value\`（clobber 占位
如实报告），和上一轮 dead-unknown 清理时的口径变化一致。

剩余：memcached 1 条（FUN_1a380 的返回槽解析落到入口 range，属于返回绑定问题）、
wrk 15 条（x87 窗口 ST0/ST1 入口值，属于窗口建模/表示层话题）。

## 常数除法魔数识别（设计，待实现；2026-09-19 续）

背景：memcached 最后 1 条 unknown 是 \`FUN_1a380\` —— raw lift 里是编译器生成的
"除以常数"（魔数乘法）helper，寄存器消除后 body 被清空、返回槽退化成入口 RAX 兜底、
被 \`rangeMayComeFromEntry\` 拒绝 → \`entry_range_rejected:RAX\`。用户提议：在最早
（寄存器没消除时）按编译器生成的魔数模式做匹配，把它折成普通 SSA/除法。

对照结论（已实测确认）：

- raw lift 里是 \`store i64 -6148914691236517205 (=0xAAAAAAAAAAAAAAAB) → @RAX\`，
  后接 \`shl %RSI, 2\` + \`zext i64→i128\` + \`mul\` + \`lshr 128,64\` + \`trunc\`，即
  \`mulhi\` 形态；\`0xAAAAAAAAAAAAAAAB\` 正是 **无符号除以 3** 的魔数（注：\`0xAAAA...AA\`
  是它减一，别混）。
- \`clang -O2\` ground truth：\`x/3\`→\`0xAAAAAAAAAAAAAAAB\`+\`mulq\`+\`shrq $1\`；
  \`x/7\`→\`0x2492492492492493\`；\`x/512\`/\`x/4096\` 走纯 shift；\`x*0xAAAA...AA\` 才会
  出现 \`0xAAAAAAAAAAAAAA\`。
- LLVM 源码位置：\`lib/CodeGen/SelectionDAG/TargetLowering.cpp:6705 BuildUDIV\`、
  \`:6537 BuildSDIV\`、\`:6471 BuildSDIVPow2\`，ISD 组合入口 \`:6421 BuildUDIVPattern\`/
  \`:6576 BuildSDIVPattern\`；魔数算法在这些函数里用 APInt 现算，没有单独常量表。

实现方案（最小）：

1. 位置：\`runNativeRegisterPreSummaryPeephole\`（summary/SSA 之前，正好是"寄存器还没消除"）。
2. 形状：同一基本块内、无 call/无内存副作用；常量装进寄存器（或 \`zext\` 常量）→
   \`mul i128 (zext A),(zext B)\`（其中一个是常量）→ \`lshr i128,64\`（+可选 add/shr 修正）
   → 结果写回被函数返回的寄存器；允许成对寄存器搬运噪声。
3. 两条落法，**先做 (a)**：
   (a) 去寄存器化：只把这条链改写成等价 SSA（保留 \`mul/lshr/trunc\` 结构），不判定除法——
       最小、最安全，目的是让返回绑定不再踩"partial write 边界被提前删"的坑；
   (b) 反解成 \`udiv/sdiv\`：用"有界搜索 + 复用 BuildUDIV 的生成算法"对拍 (魔数, shift)，
       命中才改写；建议先只做无符号 + 2 的幂，\`d\` 上限 4096，超出跳过。
4. 判据：整块命中且（若走 (b)）反解成功才改写，否则原样保留；clang ground truth 单测；
   memcached 上 \`FUN_1a380\` 应变成正常函数、unknown 归零；ctest/fortune 不回归。
5. 注意：这条 pass 只治这类函数；\`NativeRegisterSummarySSA.cpp:3454\` 记的"返回绑定依赖
   partial-write 边界、helper 被提前删就退化成 unknown"的通用机制仍要单独查。


## 魔数方案实测作废：FUN_1a380 的真实根因是同宽 mul 的 lane demand（2026-09-20 续）

用户原始 prompt：

> 这个是之前的对话，接下来按照这个继续推进，但是不要直接就弄成 SSA 吧，因为当前在链路里面比较早
> 不如弄成，使用寄存器 但是比原先更简单地使用的方式

### 先做的实测：改写魔数乘法链消不掉这条 unknown

工具支持 .ll 输入，于是直接在 `-f 0x1a380` 的 raw lift 上做 A/B，只改 IR、只看
`notdec.unknown remaining`：

| 变体 | 改法 | unknown |
|---|---|---|
| 对照 | 不改 | 1（`entry_range_rejected:RAX`） |
| R1 | 常量与乘法结果都去掉寄存器往返（`store C→@RAX` + `load @RAX` 合成 SSA 值），保留 `mul/lshr/trunc` 结构和写回 | 1 |
| R2 | 只去常量往返（`store MAGIC1/MAGIC2 → @RAX` + 对应 load） | 1 |

结论：这条 unknown 跟魔数链"用不用寄存器、怎么用寄存器"无关，(a) 消不掉它。继续按
魔数匹配做下去只是多一条不被需要的编译器模式 matcher。

### 真实根因

`rewritePartialWrites()`（`lib/passes/summary/NativeRegisterSummarySSA.cpp`，约 4273 行）
对每个 register store 的数据流跑 `rewriteUndemandedRegisterStoreOperands()`，把 demand
为零的整数 operand 换成 `poison` + `!notdec.register.summary_ssa.zero_demand_operand`
（20260911-01 的"用 poison 暴露漏掉的 observer"设计）。FUN_1a380 的 `mulq` 被 lift 成：

```llvm
%unique_4c80_8 = shl i64 %RSI, 2
store i64 %unique_4c80_8, ptr @RDX      ; 随即被 RDX 的新定义覆盖，这条 store 没有 demand
%a = zext i64 0xAAAAAAAAAAAAAAAB to i128
%b = zext i64 %unique_4c80_8 to i128
%m = mul nuw i128 %a, %b
%hi = lshr i128 %m, 64
```

partial demand 的 Add/Sub/Mul lane 规则（同文件约 4199 行）是：

```cpp
unsigned srcWidth = valueBitWidth(inst->getOperand(0));
llvm::APInt lowDemand  = trimmedMask(inputDemand, srcWidth);
llvm::APInt highDemand = inputDemand & ~maskForLowBits(valueBitWidth(inst), srcWidth);
```

`mul nuw i128` 两个操作数都是 i128，`srcWidth == resultWidth == 128`，
`~maskForLowBits(128, 128)` 恒为空 → 永远走 lane 分支：只有 `%hi` 需求的 bit[64,128)
被原样传给 i128 操作数，`%b = zext i64 ...` 再被 `trimmedMask(..., 64)` 一裁剪就剩零
demand → `%unique_4c80_8` 被换成 poison → mul 变 poison → 由它算出的分支条件、RDX、
返回值全部 poison，LLVM 按 UB 删掉函数体。签名重写后的 cleanup 读不到任何 RAX 定义，
退回入口 range，被 `rangeMayComeFromEntry`（RAX MayEntry=0）拒绝——就是那条
`entry_range_rejected:RAX`。

定位手段（一次性，已删除不入库）：在 `attributedRangeUnknown()` 与 `entryRangeInput()`
的拒绝分支加 `getenv` 打印，得到 `ENTRYREJECT block=entry detail=offset=0,width=64` 和
`RANGELOSS read=ret reason=entry_range_rejected`；再在
`removeDeadStoresAfterSignatureRewrite()` 开头 dump 函数，看到 cleanup 第一轮时函数只剩
`entry` + 两个 RDX entry read + `ret i64 poison`，而 pass A 结束时的 dump 是完整的 13 个
block。两者一对比就把"值被 poison"和"返回绑定读不到定义"接上了。

### 修复

同宽乘法没有"低位段"可依赖——乘积的每一位都依赖两个操作数的全部位——任何一位需求都
必须 demand 全部操作数：

```cpp
bool sameWidthMul = inst->getOpcode() == llvm::Instruction::Mul &&
                    srcWidth >= resultWidth;
llvm::APInt highDemand = sameWidthMul ? inputDemand
                                      : inputDemand & ~maskForLowBits(resultWidth, srcWidth);
```

Add/Sub 不动：它们的 lane demand 只是保守程度问题（进位链），一起改会把全程序的需求面
放大。改动 15 行，理解成本低；维护上把"lane 规则只对窄操作数成立"写进了注释。

### 验证

- 单测：`tests/native_register_summary_ssa_test.cpp` 新增
  `testPartialDemandKeepsSameWidthMulOperand`（dead register store + `mul i128` + 调用点
  读 RAX，断言这条函数里没有 zero-demand poison）。把修复 revert 后该断言失败
  （`same-width mul operand was replaced with poison`），恢复修复后通过。
- `-f 0x1a380`：改前 `total=1 [entry_range_rejected:RAX=1]`、函数体被清空；改后
  `total=0`，函数是 237 行的正常函数，base64 解码循环与
  `mul 0xAAAAAAAAAAAAAAAB` 链都在（InstCombine 顺手把 `lshr 64` + `lshr 1` 折成 `lshr 65`）。
- 全量 memcached（`--all-confirmed --register-ssa-summary`）：unknown 1 → **0**，
  `FUN_1a380` 是 237 行的正常函数；`llvm-as` + `opt -passes=verify` 通过。
  耗时 4m57s（改前那次 5m32s 与后台编译并行，只能算同档）。
- wrk（`--all-confirmed --skip-runtime`）：unknown 15 条不变（仍是 x87 窗口 ST0/ST1），
  `llvm-as` + `opt -passes=verify` 通过，35s。
- `ctest --test-dir build`：12/12 通过，含 `realworld_fortune_x86_64`/`i386`。

### 魔数识别 (b) 与返回绑定通用机制

- (b)（有界搜索 `d` 反解成 `udiv/sdiv`）对这条 unknown 不是必需的，暂不做；上文
  关于 `0xAAAAAAAAAAAAAAAB`/`BuildUDIV` 的 ground truth 仍然有效，但它是"可读性"
  话题而不是正确性话题。
- 返回绑定"延迟到签名重写后的 cleanup + 依赖 residue removal 保活"的通用机制没有改：
  这次修的是它的上游（真值被 poison 掉、定义提前消失）。再遇到"返回绑定读到
  入口/unknown"的函数，先查 demand 链上有没有 zero-demand poison，而不是先怪
  partial-write 边界。



## 常数除法魔数识别（实现；2026-09-20 续）

用户原始 prompt：

> 对 还是单独做一轮识别吧 还是能够提升可读性的

### 位置与形态（相对上一节设计的调整）

落在 `runNativeRegisterPostRewritePeephole`（寄存器重写之后、final cleanup 之前），
不是上一节设想的 pre-summary peephole。理由：识别只需要值图；pre-summary 那时链上
还缠着 register store / partial write，post-rewrite 时链已经是干净 SSA。上一节选
pre-summary 的动机（"寄存器还没消除时治返回绑定"）已经被同宽 mul 那个修复取代，
换到后面只减少匹配噪声。

匹配的形态（`x` 是 i64；魔数在前在后都认；链上还有别的使用者不影响改写）：

```llvm
%w = zext i64 %x to i128
%m = mul i128 %w, <magic>
%h = lshr i128 %m, K            ; K >= 64
%t = trunc i128 %h to i64       ; 形态 A：root = %t
%q = lshr i64 %t, s             ; 形态 B：root = %q，总移位 K = 64 + s
```

### 反解：Granlund-Montgomery，证明而不是抽样

`M == ceil(2^K/d)` 且 `(M*d - 2^K) * 2^L <= 2^K` ⇒ `floor(x*M/2^K) == floor(x/d)`
对全部 `x < 2^L` 成立（Hacker's Delight 4-1）。两条都验证过才改写，所以是等价变换，
不是"看着像除法"。

- `L` 取被除数本身的值上界：普通 i64 值 `L = 64`；被除数是 `lshr i64 %y, k` 时
  `L = 64 - k`。clang 就是按这个更窄的范围挑魔数的：`x/10^9` 是 `(x>>9)/1953125`，
  `M = 0x44B82FA09B5A53`、`K = 75`，只有把 L 收到 55 才验证得过（`e = 399807 <= 2^20`）。
- 反解不搜索：`M == ceil(2^K/d)` 等价于 `d ∈ [2^K/M, 2^K/(M-1))`，这个区间宽度只有
  约 `d^2/2^K`（远小于 1），所以 `d = ceil(2^K/M)` 直接算出来再验证。上一节设计里的
  "d ∈ [3,4096] 有界搜索"因此不需要了，除数不设上界。

不认的形态（整体跳过，不半改）：带 add 修正的魔数形态（`x/7`、`x/4095` 是
`mulhi; sub; shr; add; shr`）、有符号除法（sext/ashr 链）、i32 版本（`mul i64` 形态）。
`x/512` 这类 2 的幂本来就是纯 shift，没有魔数可认。

### clang ground truth

`clang -O2 -S`（llvm-22.1.0.obj，x86-64）生成的 (d, M, s) 直接抄进单测表：

| d | M | s |
|---|---|---|
| 3 | 0xAAAAAAAAAAAAAAAB | 1 |
| 5 | 0xCCCCCCCCCCCCCCCD | 2 |
| 6 | 0xAAAAAAAAAAAAAAAB | 2 |
| 9 | 0xE38E38E38E38E38F | 3 |
| 10 | 0xCCCCCCCCCCCCCCCD | 3 |
| 72 | 0xE38E38E38E38E38F | 6 |
| 4097 | 0xFFF000FFF000FFF1 | 12 |
| 10^9 | 0x44B82FA09B5A53（先 >>9） | 11 |

### 单测（`tests/native_register_summary_ssa_test.cpp`）

- `testConstantDivisionMagicFold`：上表每项 ×（形态 A、形态 B）都断言折成 `udiv x, d`，
  且乘法链被收干净（`!functionHasMul`）；
- `testConstantDivisionAddCorrectionFormIsLeftAlone`：x/7 的 add 修正形态原样保留；
- `testConstantDivisionUnknownMagicIsLeftAlone`：不对应任何除数的魔数原样保留。

### 效果与验证

- `FUN_1a380`（base64 解码的长度检查）现在读作
  `%0 = udiv i64 %unique_4c80_8, 3` / `%1 = udiv i64 %unique_4700_8, 72`，
  和源码里的 `srclen*4/3` 对得上。
- memcached：识别 7 处，最终 IR 里留下 6 条 `udiv`（除数 3 / 72 / 100×3 / 10^9，
  分布在 FUN_1a380、FUN_1b9e0、FUN_1c010、FUN_1d670）；unknown 仍是 0；
  `llvm-as` + `opt -passes=verify` 通过；耗时 4m53s（改动前同机 4m57s～5m32s，同档）。
- wrk：识别 1 处（`udiv ..., 1000`）；unknown 15 条不变（仍是 x87 窗口 ST0/ST1）；
  verify 通过；35s（改动前 35.4s）。
- `ctest --test-dir build`：12/12 通过，含 fortune x86_64/i386。
- 覆盖检查：memcached 产物里剩下的 12 条 `mul i128` 两个操作数都是运行时值的 zext
  （真正的 64×64→128 乘法），没有漏认的魔数除法。

### 代价与后续

- 只扫 i64 的 trunc/lshr 根，反解 O(1)，代价可以忽略；产物里 `udiv` 更短。
- 还没认的：add 修正形态（`x/7`、`x/4095` 这类）、有符号除法、i32 形态。
  都是独立的形态和验证，真要做应该各自一轮，不要塞进同一个 matcher。



## i386（32 位）常数除法识别（2026-09-20 续）

用户原始 prompt：

> 对，要不针对 32 位的 I386 binary 搞点测试用例，然后支持一下 I386 的情形？

### 改动：把匹配做成按位宽参数化

识别器原来把 W = 64 写死在类型和移位上界里。现在按"乘积位宽的一半"取 W：

- `mulq`（x86-64）：`zext i64 → mul i128 → lshr i128, K → trunc i128 → i64`，W = 64；
- `mull`（i386）：`zext i32 → mul i64 → lshr i64, K → trunc i64 → i32`，W = 32。

匹配里所有常量改成由 W 推出：`mul 类型 == 2W`、`K ∈ [W, 2W]`、魔数有效位 `<= W`、
`udiv` 的位宽 = W、被除数范围上界默认 W（`lshr x, k` 时收到 W-k）。顺带收紧了一处：
`trunc` 必须来自 `lshr`（取乘积高半段），裸的 `trunc(mul)` 是低半段，不是除法。

### i386 ground truth

`clang -m32 -O2 -S`（llvm-22.1.0.obj）的实际输出（`mull` 后把 edx 右移 s）：

| d | M | s |
|---|---|---|
| 3 | 0xAAAAAAAB | 1 |
| 5 | 0xCCCCCCCD | 2 |
| 6 | 0xAAAAAAAB | 2 |
| 9 | 0x38E38E39 | 1 |
| 10 | 0xCCCCCCCD | 3 |
| 100 | 0x51EB851F | 5 |
| 1000 | 0x10624DD3 | 6 |
| 10^9 | 0x44B83（先 shrl $9） | 7 |

（x/7、x/4095 在 i386 上同样是带 add 修正的形态，x/512 是纯 shift，与 64 位一致。）

### 单测

`testConstantDivisionMagicFoldI386`：上表每项 ×（形态 A、形态 B），在 `mul i64` 的
32 位形态上断言折成 `udiv i32 x, d` 且链被收干净；`buildMagicDivisionFunction` 加了
`width` 参数（默认 64，不改变原有 64 位用例）。

### 端到端（真 i386 binary）

`clang -m32 -O2 -static` 编了个只含 `x/3`、`x/100`、`x/1000`、`x/10^9` 的 i386 可执行
文件，用 `-f <addr>` 逐个提升：

| 函数 | 输出 |
|---|---|
| `div3` | `%0 = udiv i32 %stack+4.arg, 3` |
| `div100` | `%0 = udiv i32 %stack+4.arg, 100` |
| `div1e9` | `%0 = udiv i32 %stack+4.arg, 1000000000`（预移位被 InstCombine 折回整除数） |

整个静态二进制（含 i386 libc，`--all-confirmed`）：`constant_divisions_recognized=12`、
unknown 0；`llvm-as` + `opt -passes=verify` 通过（`e-p:32:32`）。

### 回归

- memcached x86-64：识别 7 处不变、unknown 0、4m50s；wrk：1 处不变、unknown 15 不变、35s。
- `ctest --test-dir build`：12/12 通过（含 `realworld_fortune_x86_64`/`i386`）。
- 三个 i386 与两个 x86-64 产物全部 `llvm-as` + `opt -passes=verify` 通过。

### 下一轮：i386 的有符号除法（本轮**没**做）

fortune-i386 样本里的常数除法魔数链**全是有符号**的（`imull` → `sarl` → 加符号位），
所以它的 `constant_divisions_recognized=0`、5 条 `mul nsw i64` 原样保留：

```llvm
%s = sext i32 %x to i64
%m = mul nsw i64 %s, <M>
%.neg = lshr i32 <high 或 x>, 31
%q = add i32 (… >>_arith s), %.neg
```

clang -m32 的有符号 ground truth（`int f(int x)`）：`x/100` → M = 0x51EB851F、`sarl $5`；
`x/13` → M = 0x4EC4EC4F、`sarl $2`；都在 `imull` 的高半段上做算术右移，再加高半段的
符号位。要做得另外一轮：魔数生成用 Hacker's Delight 的有符号版本（`magic()`）对拍，
验证方式也和这里不同，不能直接复用无符号的 GM 条件。

（顺带记录：fortune-i386 产物里剩 1 条 unknown，是 `notdec_native_43c0` 的
`return_binding_uses_clobber_value`——间接调用返回的纯 clobber 图，属 20260911-01
的既有行为，与本轮改动无关；`ctest` 不校验该计数。）



## 有符号常数除法识别（2026-09-20 续）

用户原始 prompt：

> 对，把有符号的也尝试，这相关的都加上吧

### 匹配的形态

有符号除法（`int x/d`）和它的模运算在 lift 之后是：

```llvm
%wide = sext iW %x to i(2W)
%m    = mul nsw i(2W) %wide, <M>       ; 正魔数
%h    = lshr i(2W) %m, W
%t    = trunc i(2W) %h to iW
%q    = ashr iW %t, s                  ; s >= 1；s == 0 时没有这一条
%sign = lshr iW %t, W-1                ; 高半段符号位（写 x 的符号位也认）
%r    = add iW %q, %sign
```

它算的是 `floor_s(x*M / 2^(W+s)) + [x<0]`：x >= 0 时就是 GM 的 `floor(x/d)`；
x < 0 时把 `x = -y`、`y = q*d + r` 代进去逐项展开，需要 `0 < q*e + r*M <= 2^K`
（`e = M*d - 2^K`），它在 `e * 2^(W-1) <= 2^K` 时成立——和无符号的 GM 条件是
同一个式子，只是 L 从 W 换成 W-1。所以两种除法共用一套 O(1) 反解，只换 L，
改写时用 `sdiv` 而不是 `udiv`。

**没做**：负魔数的 `x + mulhi` 形态（clang -O2 x86-64 的 `int64_t x/100` 是
`mul nsw i128 %sext, -6640827866535438581` + `add x` + `ashr 6` + 符号位），
它需要自己的等价性证明，本轮整体跳过（有单测钉住"不认"）。

### ground truth（clang -O2 实际输出）

i386（`imull` + `sarl`）：

| d | M | s |
|---|---|---|
| 3 | 0x55555556 | 0 |
| 10 | 0x66666667 | 2 |
| 13 | 0x4EC4EC4F | 2 |
| 100 | 0x51EB851F | 5 |
| 1000 | 0x10624DD3 | 6 |

x86-64（`imulq` + `sarq`）：

| d | M | s |
|---|---|---|
| 3 | 0x5555555555555556 | 0 |
| 5 | 0x6666666666666667 | 1 |
| 7 | 0x4924924924924925 | 1 |
| 10 | 0x6666666666666667 | 2 |
| 13 | 0x4EC4EC4EC4EC4EC5 | 2 |

（注意 `x/7` 有符号的魔数是 0x4924924924924925，不是无符号的 0x2492492492492493
——后者对应 d = 14，在 L = W-1 下验证不过，正好说明两套条件不能混用。）

### 测试与结果

- 单测：`testSignedConstantDivisionMagicFoldI386`、`...FoldX86_64`（上表全部命中，
  且断言没有被记成无符号）、`testSignedConstantDivisionAddFormIsLeftAlone`
  （负魔数 `+x` 形态必须原样保留）。
- i386 实测：`-f` 提升自建 `-m32` 静态二进制——`int32_t x/3` → `sdiv i32 %arg, 3`，
  `x/100` → `sdiv i32 %arg, 100`，`x%100` → `srem i32 %arg, 100`
  （先折出 sdiv，InstCombine 再把 `x - 100*(x/100)` 认成 srem）。
- x86-64 实测：`int64_t x/3` → `sdiv i64 %RDI.arg, 3`；`x/100` 仍是未识别的
  `+x` 形态（本轮不做）。
- **fortune-i386：5 处有符号魔数全部折掉**（`constant_divisions_recognized=0`、
  `signed_constant_divisions_recognized=5`），产物里 `mul nsw i64` 从 5 条降到 0 条，
  读作 `srem i32 %x, 100` 和 4 条 `srem i32 %x, 26`；unknown 仍是那 1 条
  `return_binding_uses_clobber_value`（与本次无关）。
- x86-64 回归：memcached 无符号 7 处、有符号 0 处、unknown 0、4m47s；
  wrk 无符号 1 处、**有符号 5 处（都是 /1000）**、unknown 15 不变、34s。
- `ctest --test-dir build`：12/12 通过；memcached/wrk/i386/x86-64 产物全部
  `llvm-as` + `opt -passes=verify` 通过。

### 覆盖边界

现在还认不出来的常数除法形态只有两类：无符号的 add 修正魔数（x/7、x/4095）和有符号的
负魔数 `+x` 形态（x86-64 的 int64 x/100）。两者都需要各自的等价性证明，不是换个
条件就能扩的；真要做应该各自一轮。



## 剩下两种形态：等价性证明怎么做、难度如何（2026-09-20 续）

用户原始 prompt：

> 那按这个方向再继续推进试试？等价性证明具体是怎么做的？是否存在较大的难度？

### 证明的固定套路

四类形态用的是同一套流程，不是"看着像就折"：

1. **写闭式**：把匹配到的 IR 写成输入 x 的函数。例如简单无符号是
   `q(x) = floor(x*M / 2^K)`；add 修正是 `q(x) = floor((x + h) / 2^extra)`，
   `h = floor(x*M/2^N)`；有符号是 `q(x) = floor_s(x*M/2^K) + [x<0]`。
2. **代入 x = d*q + r**（0 <= r < d）：闭式里所有除法和取整都作用在
   `q*d + r` 上，展开后 `q` 单独出来，剩下一个只依赖 (q, r) 的误差项 E。
   - 简单形态：`h = q + floor((q*e + r*M)/2^K)`，于是
     `q_out = q + floor((q*e + r*M)/2^K)`，要它等于 q 就得
     `0 <= q*e + r*M < 2^K`，闭式化就是 **`e * 2^L <= 2^K`**（GM 条件）。
   - add 修正形态：`q_out = q + floor((r + floor((q*e + r*M)/2^N)) / 2^extra)`，
     要它等于 q 就得 **`E(q,r) = r + floor((q*e + r*M)/2^N) < 2^extra`**。
   - 有符号：x >= 0 时退化成 GM；x < 0 时令 `x = -y`、`y = q*d + r`，
     `floor_s(-y*M/2^K) = -ceil(y*M/2^K)`，配上 `+1` 的符号修正后需要
     `0 < q*e + r*M <= 2^K`，和上面同一个式子，只是 L 换成 W-1。
3. **证明 E ≡ 0**：E 对 q、r 都单调，可行域是 `q*d + r <= 2^L - 1` 的阶梯，
   所以最大值只可能在两个角点 `(q_max, r_max)` 和 `(q_max-1, d-1)` 取到。
   于是"证明"就是 **两次 APInt 算术检查**，不是抽样：代码里
   `solveMagicDivisor` 是一个不等式，`solveUnsignedAddMagicDivisor` 是两次
   `worstError` 求最大。
4. **反解 d**：`M == ceil(2^K/d)` 把 d 夹在宽度 < 1 的区间里，
   `d = ceil(2^K/M)` 直接算出来再回代验证，不搜索、不猜。

### 新增的两类

**无符号 add 修正形态**（`x/7`、`x/4095`）：
`h = trunc(lshr(mul(zext x, M), W))`、`q = ((x - h) >> 1 + h) >> s`。
它等于 `floor((x + h) / 2^(s+1))`（因为
`floor((x-h)/2) + h = floor((x+h)/2)`），按上面第 3 步验证。实测 `x/7` 的最坏
误差项是 7、上限是 8，`x/4095` 是 4095 < 4096——很紧但成立。

**有符号负魔数 `+x` 形态**（clang x86-64 的 `int64_t x/100`）：
机器常量是负的 `M_neg`，序列是 `t = x + mulhi_s(x, M_neg)`、
`q = (t >>_arith s) + [t<0]`。因为
`floor(x*M_neg/2^W) = floor(x*M_u/2^W) - x`（`M_u = M_neg + 2^W > 0`），
外面 `+ x` 正好抵消，所以 **它本来就是正魔数简单形态**，取常量的低 W 位当
`M_u` 就能用已有的有符号条件直接验证，不用新的数学。

### 难度

不大，但每类都要单独推一遍、单独钉测试。这次两类加起来：
无符号 add 是"多推一个误差项 + 两次 APInt 检查"；有符号 `+x` 是"认出它其实是
简单形态"（推导 5 行）。真正花时间的是**形态匹配**（谁减谁、符号位从哪个值取、
InstCombine 有没有把中间步骤折掉）和 ground truth 对拍，不是数学。
之前没做是因为在"一次只加一类、先证明再折"的节奏下没必要一次铺开。

### 验证

- 单测：新增 `testUnsignedAddCorrectionMagicFold`（x86-64 的 x/7、x/4095）和
  `testSignedNegativeMagicAddXFormFold`（int64 x/100 的负魔数 `+x` 形态）；
  原来两条"这两种形态必须原样保留"的负向测试因为现在支持了而删除。
- 端到端（自建静态二进制，按地址提升）：
  `uint64_t x/7` → `udiv i64 %RDI.arg, 7`；
  `uint64_t x/4095` → `udiv i64 %RDI.arg, 4095`；
  `int64_t x/100` → `sdiv i64 %RDI.arg, 100`（之前是原样的 `+x` 链）。
- 回归：memcached 无符号 7 / 有符号 0 / unknown 0 / 4m50s；wrk 无符号 1 /
  有符号 5 / unknown 15 / 36s；fortune-i386 有符号 5 不变；
  `ctest --test-dir build` 12/12；四个产物 `llvm-as` + `opt -passes=verify` 通过。

### 现在还剩什么

四类形态（无符号/有符号 × 简单/add 修正）都认了。还差的是**除数带预移位且
InstCombine 没折回整除数**的边角、16 位 i386 形态（`mul i32` 的 lift），
以及浮点除法——都不是当前样本里出现的东西，等有真实用例再说。



## 边角形态：窄乘积（16 位）与窄源加宽（2026-09-20 续）

用户原始 prompt：

> 剩余的这些边角，要不创建一些对应的测试用例，然后也实现一下试试吧

### 先扫真实产物，别凭印象列边角

把 memcached / wrk / fortune-i386 产物里剩余的常量乘法按形态分类：

- 绝大多数是**哈希/定点常数**（`mul i64 %x, 1609587791953885689`、`mul i64 %x, 1000000`、
  `mul i32 %x, -862048943` 之类），不是除法，本来就该原样留着。
- 真正的除法缺口只有 **1 处**：wrk 里一个 `gettimeofday` 附近的
  `int64` 值其实是 `sext i32` 来的，编译器用了 32 位魔数放进 64 位乘法里——**窄源加宽**。
- `and(ashr(x, W-1), mask)` 和 `select` 那些看着像"2 的幂除法修正"，逐个查上下文后
  都不是（是符号扩展/掩码 idiom），所以**没做**有符号 2 的幂除法。
- 16 位形态在样本里没有，但 i386 上有真实的 `uint16_t/int16_t` 除法，clang 会把它
  提升到 32 位，于是有第三种乘积形态。

### 形态一：窄乘积（16 位提升到 32 位）

clang -m32 -O2 对 `uint16_t x/3` 生成
`movzwl x; imull $43691; shrl $17`，lift 出来是：

```llvm
%v = zext i16 %x to i32
%p = mul nuw i32 %v, 43691
%q = lshr i32 %p, 17
```

乘积**自身宽**（不是 2W），没有高半段，移位 N (=17) 还小于 W (=32)。数学没变：
还是 GM 的 `floor(x*M/2^N) = floor(x/d)`，只是把 L 取成源的位宽（16）。
额外必须验证**乘积不溢出**：`max|x| * M < 2^(W-1)`（有符号）/`2^W`（无符号），
否则机器码是回绕值、公式不成立——这一步在 matcher 里用 APInt 检查。

有符号 16 位（`int16_t x/3`）是 `%p = mul nsw i32 (sext i16), 21846`、
`%q = add (lshr %p, 16), (lshr %p, 31)`——注意符号位取自**乘积本身**（不是高半段），
正好就是已经证明过的有符号公式 `floor_s(x*M/2^N) + [x*M<0]`。

实现：`matchNarrowProductMagicChain`，求解器给 `solveMagicDivisor` 加了
`allowShortShift`（窄乘积的移位可以 < W）。

### 形态二：窄源加宽

wrk 那处：

```llvm
%83 = sext i32 %v to i64           ; 被除数（i64）
%w  = sext i32 %v to i128
%p  = mul nsw i128 %w, 2361183241434822607   ; ceil(2^71/1000)
%h  = lshr i128 %p, 64
%t  = trunc i128 %h to i64
%.neg = lshr i64 %83, 63           ; 符号位
%q  = add i64 (ashr i64 %t, 7), %.neg
```

乘法的源比 W 窄（i32 而不是 i64），但被除数是同一个值的 i64 扩展。证明不变，
只是范围收到 i32：`inputBits = 32 - 1 = 31`，条件更松。改写用那个 i64 被除数。
实现上把 `parseMagicProduct` 的"源必须正好 W 位"放宽成"源 <= W 位并记下位宽"，
符号位来源在那个 iW 值上时就走窄源分支。

### 验证

- 单测：`testNarrowProductMagicFold16Bit`（16 位无符号/有符号各一）、
  `testNarrowSourceWideningFold`（wrk 那处 i64/i32 形态）。
- 端到端（自建 i386 静态二进制，按地址提升）：
  `uint16_t x/3` → `udiv i16 %stack+4.arg, 3`；
  `int16_t x/3` → `sdiv i16 %stack+4.arg, 3`。
- wrk：有符号识别 5 → **6**（窄源那处折成 `sdiv i64 ..., 1000`），unknown 15 不变。
- memcached：无符号 7 → **19**（窄乘积形态额外认出了 12 处 `udiv i32`，除数
  60/100/10000/10^6/10^8 —— 32 位时间/单位换算，魔数放在 64 位乘法里的那类），
  有符号 0、unknown 0；哈希/定点常数照旧不动。
- fortune-i386：有符号 5 不变；`ctest --test-dir build` 12/12；
  产物 `llvm-as` + `opt -passes=verify` 通过。

### 仍然不做

16 位 **add 修正**形态（`uint16_t x/7`：sub/lshr 在 i16、add/lshr 在 i32，混宽度）——
样本里没有、匹配要跨宽度，投入产出不划算，等有真实用例再说；
16 位以下的 8 位形态同理。有符号 2 的幂除法（clang 用 `cmov`/`and` 修正序列）
在三个样本里一处都没有，也先不做。



## 常量乘法盘点 + peephole 阶段的 extract 噪声（2026-09-20 续）

用户原始 prompt：

> 类似的乘法是不是还没做，如果是的话，也做一下匹配吧？

### 先回答"乘法还有没有没做的"：没有

写了代数扫描器，把产物里的 `shl/add/sub` 链按 `C*x` 还原（递归到同一个基值），
只统计系数不是 0/1/2 的幂、也不是已有 `mul` 的：
memcached / wrk / fortune-i386 **三份产物全是 0 处**。原因是这类强度削减
（`x*3` → `lea`）在 lift 出来的 IR 里本来就已经是 `mul`，InstCombine 也会折回去，
没有"移位加法汤"要还原。所以这一轮没有乘法 matcher 可加。

顺手做的覆盖率检查反而发现了**除法的真缺口**：编了个 40 个除法点（u32/s32/u64/s64 ×
10 个除数）的 x86-64/i386 二进制对拍，x86-64 只折了 27/40：

- `s32_*`（`int32_t x/d`）**全部没折**；
- `u32_1`（`uint32_t x/7` 的 add 修正形态）没折；
- `u32_9`/`s32_9`（除以 10^12，源码直接是常量 0）是假缺口，忽略。

### 根因：peephole 阶段还在用 `notdec.reg.extract` 表示位段

把 peephole 输入的 IR dump 出来（临时 getenv 开关，看完已删）才看清：
`s32_0` 在这个阶段是

```llvm
%1 = mul nsw i64 %0, 1431655766
%2 = lshr i64 %1, 63
%3 = lshr i64 %1, 32
%a = call i32 @notdec.reg.extract.i64.i32(i64 %3, i64 0)   ; 不是 trunc
%b = call i32 @notdec.reg.extract.i64.i32(i64 %2, i64 0)
%4 = add i32 %a, %b
```

识别器只认 `trunc`，于是整条链匹配不上；而 final cleanup 之后它才变成 `trunc`，
所以"最终 IR 看着完全该折"却折不了。

### 本轮改动

在 `runNativeRegisterPostRewritePeephole` 开头先把 `notdec.reg.extract.iFULL.iW(v, off)`
降成显式 `lshr`+`trunc`（final cleanup 本来就会做同样的事），再跑识别和原有的
value-range 化简。匹配器另外放宽了两处：符号位接受
`trunc iW (lshr i(2W) %m, 2W-1)`（乘积符号位），以及"是不是同一个值"改成按
(基值, 位偏移, 位宽) 归一化比较，不再被一层 `trunc`/extract 包装挡住。

### 结果与仍缺的部分

- 覆盖率对拍：x86-64 27/40 → **29/40**（`s32_0` 折了；peephole 的 value-range 化简
  在同一份 IR 上从 0 处变成 19 处）。
- 仍缺：`u32_1`（32 位 add 修正）和 `s32_1..s32_8`（d=7 起的 32 位有符号除法）。
  这些在 peephole 阶段是**另一类形态**（还没逐个 dump 确认），需要单独一轮：
  先 dump 它们的 peephole 输入、按同一套流程推导、再实现。
- 回归：memcached unknown 0、wrk unknown 15（不变）、`ctest --test-dir build` 12/12、
  单测全过；wrk 的 value-range 化简从 0 处涨到 53 处（同一份 IR 上识别和化简都变好）。
- 中途踩到一个自己引入的崩溃：移位量用 `APInt(width, ...)` 按类型比较，遇到
  "移位量比被除数宽"（`lshr i64` 用在 i32 形态里）会触发宽度断言，wrk 直接 abort；
  改成按值比较（`shiftAmountIs`）后消失。这类形态在 lowering 之后才被暴露出来。

（这一轮的定位经验：以后"最终 IR 该折没折"先怀疑 **peephole 阶段的值图形态**，
dump 一眼比猜快得多；`notdec.reg.*` helper 是这一层最常见的噪声。）

## x86-64 32 位除法边角：宽度往返 + ashr-before-trunc（2026-09-21 续）

用户原始 prompt：

> 阅读 docs/analysis/native-compiler-idiom-matching.md，
>
> 下一步（很具体，不用再推导数学）：在 runNativeRegisterPostRewritePeephole 里重新加上
> NOTDEC_DEBUG_PEEPHOLE 开关（我这两轮用的那几行），dump s32_1 在往返剥离之后的形态；
> 对着形态修 matchSignedMagicMulChain 的 +x 分支（大概率是剥离只做了一半，或
> sign->getOperand(0) != sum / product->Scaled != dividend 这两处比较）；u32_1（32 位
> add 修正）同样 dump 一次，多半也是同一类包装；覆盖率 fixture 从 29/40 打到 37/40
> （剩下 3 个是 x/10^12 的假缺口），补单测、跑 memcached/wrk/ctest、记录提交。
> 这个 fixture（40 个除法点）和 dump 开关的用法我都写在
> logs/20260917-03-native-callee-saved-and-df-plan.md 里了，下一轮可以直接接着做。

### 覆盖率 fixture 与结果

fixture 源码（`/tmp/cov.c`，重建时直接抄回）：

```c
#include <stdint.h>
__attribute__((noinline)) uint32_t u32_0(uint32_t x){return x/3;}
__attribute__((noinline)) uint32_t u32_1(uint32_t x){return x/7;}
__attribute__((noinline)) uint32_t u32_2(uint32_t x){return x/9;}
__attribute__((noinline)) uint32_t u32_3(uint32_t x){return x/11;}
__attribute__((noinline)) uint32_t u32_4(uint32_t x){return x/13;}
__attribute__((noinline)) uint32_t u32_5(uint32_t x){return x/60;}
__attribute__((noinline)) uint32_t u32_6(uint32_t x){return x/1000;}
__attribute__((noinline)) uint32_t u32_7(uint32_t x){return x/65535;}
__attribute__((noinline)) uint32_t u32_8(uint32_t x){return x/1000000000;}
__attribute__((noinline)) uint32_t u32_9(uint32_t x){return x/1000000000000;}
__attribute__((noinline)) int32_t s32_0(int32_t x){return x/3;}
__attribute__((noinline)) int32_t s32_1(int32_t x){return x/7;}
__attribute__((noinline)) int32_t s32_2(int32_t x){return x/9;}
__attribute__((noinline)) int32_t s32_3(int32_t x){return x/11;}
__attribute__((noinline)) int32_t s32_4(int32_t x){return x/13;}
__attribute__((noinline)) int32_t s32_5(int32_t x){return x/60;}
__attribute__((noinline)) int32_t s32_6(int32_t x){return x/1000;}
__attribute__((noinline)) int32_t s32_7(int32_t x){return x/65535;}
__attribute__((noinline)) int32_t s32_8(int32_t x){return x/1000000000;}
__attribute__((noinline)) int32_t s32_9(int32_t x){return x/1000000000000;}
__attribute__((noinline)) uint64_t u64_0(uint64_t x){return x/3;}
__attribute__((noinline)) uint64_t u64_1(uint64_t x){return x/7;}
__attribute__((noinline)) uint64_t u64_2(uint64_t x){return x/9;}
__attribute__((noinline)) uint64_t u64_3(uint64_t x){return x/11;}
__attribute__((noinline)) uint64_t u64_4(uint64_t x){return x/13;}
__attribute__((noinline)) uint64_t u64_5(uint64_t x){return x/60;}
__attribute__((noinline)) uint64_t u64_6(uint64_t x){return x/1000;}
__attribute__((noinline)) uint64_t u64_7(uint64_t x){return x/65535;}
__attribute__((noinline)) uint64_t u64_8(uint64_t x){return x/1000000000;}
__attribute__((noinline)) uint64_t u64_9(uint64_t x){return x/1000000000000;}
__attribute__((noinline)) int64_t s64_0(int64_t x){return x/3;}
__attribute__((noinline)) int64_t s64_1(int64_t x){return x/7;}
__attribute__((noinline)) int64_t s64_2(int64_t x){return x/9;}
__attribute__((noinline)) int64_t s64_3(int64_t x){return x/11;}
__attribute__((noinline)) int64_t s64_4(int64_t x){return x/13;}
__attribute__((noinline)) int64_t s64_5(int64_t x){return x/60;}
__attribute__((noinline)) int64_t s64_6(int64_t x){return x/1000;}
__attribute__((noinline)) int64_t s64_7(int64_t x){return x/65535;}
__attribute__((noinline)) int64_t s64_8(int64_t x){return x/1000000000;}
__attribute__((noinline)) int64_t s64_9(int64_t x){return x/1000000000000;}
int main(void){ return (int)(u32_0(3)+u32_1(3)+u32_2(3)+u32_3(3)+u32_4(3)+u32_5(3)+u32_6(3)+u32_7(3)+u32_8(3)+u32_9(3)+
  s32_0(3)+s32_1(3)+s32_2(3)+s32_3(3)+s32_4(3)+s32_5(3)+s32_6(3)+s32_7(3)+s32_8(3)+s32_9(3)+
  u64_0(3)+u64_1(3)+u64_2(3)+u64_3(3)+u64_4(3)+u64_5(3)+u64_6(3)+u64_7(3)+u64_8(3)+u64_9(3)+
  s64_0(3)+s64_1(3)+s64_2(3)+s64_3(3)+s64_4(3)+s64_5(3)+s64_6(3)+s64_7(3)+s64_8(3)+s64_9(3)); }
```

fixture 在 `/tmp/cov.c`（40 个除法点：u32/s32/u64/s64 × 10 个除数，全部 noinline，
main 里逐个调用），`/tmp/cov64`、`/tmp/cov32` 是 clang -m64/-m32 -O2 的产物；
逐点检查就是把产物按函数名切开数 `udiv/sdiv`（脚本 `/tmp/cov_check.py`）：

```bash
build/bin/notdec-native-llvm /tmp/cov64 --all-confirmed --skip-runtime \
  --register-ssa-summary -o /tmp/cov64.ll
python3 /tmp/cov_check.py /tmp/cov64.ll   # 打印 recognized: N/38 + missing
```

- x86-64：**29/40 → 38/40**。缺的 11 个里，`s32_1..s32_8`、`u32_1` 是真缺口
  （本轮修掉）；`u32_9`/`s32_9`（除以 10^12）在产物里根本不存在——`objdump -d`
  是 `xor %eax,%eax; ret`，clang 已经折成常量 0，是假缺口。
  **用户预估的"3 个假缺口"实际是 2 个**（u64_9/s64_9 是真实 64 位除法，一直能折），
  所以 38/40 就是全部真实点。
- i386（/tmp/cov32）：18/18 个真实 32 位点都折了；u64/s64 在 i386 上是
  `__udivdi3`/`__divdi3` 库调用，没有魔数，属预期，无回归。

### dump：peephole 输入的真实形态（开关已按惯例删除）

在 `runNativeRegisterPostRewritePeephole` 的 extract 降级之后加回
`NOTDEC_DEBUG_PEEPHOLE`（docs §1.3 的那几行），`NOTDEC_DEBUG_PEEPHOLE=s32_` 跑一次
fixture 就能看到。三类形态：

**s32_1（负魔数 `+x`，x86-64 i32）**——`+x` 分支的每个环节都被 `trunc(zext(...))`
包住，指针比较全部落空：

```llvm
%0 = sext i32 %RDI.arg to i64
%1 = mul nsw i64 %0, -1840700269
%2 = lshr i64 %1, 32
%3 = trunc i64 %0 to i32          ; trunc(sext %RDI.arg) = %RDI.arg
%4 = trunc i64 %2 to i32
%5 = add i32 %3, %4               ; sum
%6 = zext i32 %5 to i64
%7 = trunc i64 %6 to i32          ; trunc(zext %5) = %5
%8 = zext i32 %7 to i64
%9 = trunc i64 %8 to i32          ; 又一层
%10 = lshr i32 %9, 31             ; 符号位：操作数是 %9，不是 %5
%11 = zext nneg i32 %10 to i64
%12 = ashr i32 %7, 2              ; shifted：操作数也是 %7，不是 %5
%13 = zext i32 %12 to i64
%14 = trunc i64 %13 to i32
%15 = trunc i64 %11 to i32
%16 = add i32 %14, %15            ; root
```

**s32_2（正魔数，ashr 作用在 2W 位积上）**：

```llvm
%1 = mul nsw i64 %0, 954437177
%2 = lshr i64 %1, 63              ; 乘积符号位
%3 = ashr i64 %1, 33              ; sarq $0x21,%rax：移位在 64 位积上做
%4 = trunc i64 %3 to i32          ; 截断在 ashr 之后
%5 = trunc i64 %2 to i32
%6 = add i32 %4, %5
```

**u32_1（无符号 add 修正）**——同一类包装：
root `%13 = lshr i32 %12, 2`；`%12 = trunc(zext(%10))`、`%10 = add i32 %3, %9`
（`%9 = trunc(zext(%7))`）、`%7 = lshr i32 %6, 1`（`%6 = trunc(zext(%4))`）、
`%4 = sub i32 %RDI.arg, %3`。

### 根因（三处，都不是数学）

1. **宽度往返没看穿**：`trunc(zext(x))`/`trunc(sext(x))` 和 `x` 是同一个值，但
   `sign->getOperand(0) != sum`、`product->Scaled != dividend` 以及 root 操作数的
   分类都按 `Value*` 直接比，`+x` 分支和 add 修正分支的每个环节都断链。
2. **ashr 的位置**：i386 是 `trunc(lshr(m, W))` 之后在 iW 上 `ashr s`；x86-64 的
   32 位除法是 `trunc(ashr i64 %m, W+s)`（`sarq` 直接作用在 64 位积上）。
   匹配器只认前者，后者的 trunc 操作数是 `ashr` 不是 `lshr`，`parseHighHalf`
   直接拒绝。
3. **`lowBitsView` 的盲区**：原来的 trunc 处理会跳到更宽的操作数上，于是
   `%RDI.arg`（i32）和 `trunc(sext %RDI.arg)`（i32）仍被当成两个值。

### 改动（`lib/passes/summary/NativeRegisterPeephole.cpp`）

- `:254` 新增 `stripWidthRoundTrip`：把 `trunc (zext|sext %v)`（结果类型 == %v
  类型）折成 `%v`，最多 4 层。`lowBitsView`（`:272`）每轮先过它，`sameLowBits`
  因此也能看穿。
- `matchSignedMagicMulChain`（`:644`）：root 操作数、ashr 的操作数、`+x` 分支 sum
  的两个操作数都先过 `stripWidthRoundTrip`；`sign->getOperand(0) != sum` 和
  `product->Scaled != dividend` 改成 `sameLowBits`。
- 同函数 simple 分支接受 `trunc(ashr i(2W) %m, K)`（W ≤ K ≤ 2W）：
  `extraShift = K - W`，其余（正魔数、`parseMagicProduct`、符号位来源）不动。
  等价性：截断后低 W 位和"先取高半段再 ashr"相同，闭式仍是
  `floor_s(x*M/2^K) + [x<0]`。
- `matchUnsignedAddMagicMulChain`（`:864`）：root 的 add、sum 的操作数、`sub`
  同样过 `stripWidthRoundTrip`。

数学一行没改：新形态复用同一个 `solveMagicDivisor`/`solveUnsignedAddMagicDivisor`
证明。dump 开关按惯例在提交前删除（`git grep getenv` 复核过）。

### 单测（`tests/native_register_summary_ssa_test.cpp`）

- `:11010` `testX86_64Signed32BitAshrBeforeTruncFold`：d = 3/9/11/13/1000/10^9，
  (M, sarq 移位量) 抄自 clang -m64 真实输出；断言折成 sdiv、乘法链收干净。
- `:11075` `testX86_64Signed32BitNegativeMagicAddXWithRoundTrips`：d = 7/60/65535
  的负魔数 `+x` 形态，按 peephole 输入原样构造 `trunc(zext)` 往返。
- `:11142` `testUnsigned32BitAddCorrectionWithRoundTrips`：u32 x/7、x/4095。
- `:11207` `testWrappedRoundTripUnknownMagicIsLeftAlone`：同样的往返形态配未知
  魔数（0x12345678）必须原样保留。
- A/B：把 pass 改动 stash 掉重建后跑，前三条分别报
  "chain was not folded back to sdiv/udiv" 和计数不符；恢复后全过。

### 验证

| 指标 | 改前 | 改后 |
|---|---|---|
| 覆盖率 fixture x86-64 | 29/40 | **38/40**（余下 2 个是假缺口） |
| 覆盖率 fixture i386 | 18/18 真实点 | 18/18 |
| memcached unknown | 0 | **0** |
| memcached 识别（无符号/有符号） | 19 / 0 | **20 / 0** |
| memcached IR bytes / warning / defines | — | 3,767,398 / 823 / 231 |
| wrk unknown | 15（ST0=7, ST1=8） | **15**（同左） |
| wrk 识别（无/有）/ value_range | 1 / 6 / 53 | 1 / 6 / 53 |
| wrk IR bytes / defines / warning | — | 949,730 / 89 / 318 |
| `llvm-as` + `opt -passes=verify`（memcached/wrk） | OK | OK |
| `ctest --test-dir build` | 12/12 | **12/12** |

耗时：memcached 586s、wrk 61s。**这两个数不可比**——跑的时候机器上有别的项目在
跑（一批 `microsub2.exe` worker），16 核 load average 23~29，是改前
4m50s/35s 的 ~1.8~2 倍；但和改动无关（同负载下 fixture 还是 2.6s，改动是每个候选
根 O(1) 的 dyn_cast，没有新增循环）。要复现耗时对比得等机器空下来重跑。

### 剩余

- `u32_9`/`s32_9` 是假缺口（clang 折成常量 0），不再计。
- 16 位 add 修正、有符号 2 的幂、8 位形态仍不做（样本里没有）。

## 取模残形：`sub` 写法的有符号商（2026-09-21 续）

用户原始 prompt：

> 这个"取模残形"可以补一下，补完再详细分析一下那个 CALLOTHER 问题具体是什么，
> 用容易懂的方式介绍一下

### 先纠一个自己的误判：peephole 看到的不是折叠后的形态

上一轮扫描在**最终 IR** 里看到 fortune-x86_64 的 4 处 `(c - 52) % 26` 是
`lshr i64 %m, 35` + `mul i32 %q, 230`（230 ≡ -26 (mod 256)），据此写了一版认
"demand 折叠后的 add + 230"的 matcher，结果一处也没折。把 peephole 输入 dump 出来
才看清（`NOTDEC_DEBUG_PEEPHOLE=4db0`）：

- peephole 阶段是**原始形态**：
  `%102 = sub i32 (trunc i64 (ashr i64 %m, 35)), (ashr i32 %x, 31)`，
  余数是 `%107 = sub i32 %x, mul i32 %q, 26`；
- 最终 IR 里的 `lshr` + `230` 是 peephole **之后**最后一次 InstCombine 的
  demanded-bits 化简：只有低 8 位被用到，`ashr` 与 `lshr` 在 bit[0,8) 上相同，
  负的 `-26*q` 也就换成了 `+230*q`。

第一版 matcher 整个撤掉（没进提交），改在**商的写法**上补：现有
`matchSignedMagicMulChain` 只认 `add iW %shifted, %sign`（sign = `lshr x, W-1`，
0/1），而 clang 在"只要余数"的路径上用 `sub iW %shifted, %sign`（sign =
`ashr x, W-1`，0/-1）。两者都等于 `floor_s(x*M/2^K) + [x<0]`，closed form 和
`solveMagicDivisor` 的验证条件完全相同。

**教训**：`docs §1.3` 说"dump 目标 pass 那一刻的 IR"，这次指的是**两种方向**——
最终 IR 既可能比 peephole 脏（helper 还没降级），也可能比 peephole **干净**
（InstCombine 事后做了等价的 demand 改写）。按最终 IR 反推 matcher 会写错形态。

### 改动（`lib/passes/summary/NativeRegisterPeephole.cpp`）

- `matchSignedMagicMulChain`：根接受 `Sub`；符号项和根**配对**——Add 配
  `lshr x, W-1`（0/1），Sub 配 `ashr x, W-1`（0/-1）；其余 AShr 仍按 i386 的
  `ashr iW (trunc(lshr(m, W))), s` 处理。`+x` 负魔数分支保持只认 Add。
- 候选根扫描加上 `Sub`（原来只扫 trunc/lshr/add）。
- 取模本身**不写 matcher**：折出 `sdiv` 之后 `x - d*(x/d)` 由 InstCombine 认成
  `srem`（文档 §2.3 的既定路线）。i386 的 fortune 一直这么折，这次只是让 x86-64
  的 `sub` 写法也能走到这一步。

### 验证

- fortune-x86_64：4 处全部变成 `srem i32`，产物里 `1321528399` 从 4 → **0**：

  ```llvm
  %48 = add nsw i32 %notdec.reg.insert.lower.wide158, -52
  %49 = srem i32 %48, 26
  %50 = trunc nsw i32 %49 to i8
  %51 = add i8 %50, 65
  ```

- 单测 `testSignedDivisionSubFormFold`（按真实形态构造，含 `trunc(zext)` 往返）：
  断言折成 sdiv 26、magic 的 i64 mul 收干净；把 pass 改动 stash 掉重建后报
  "sub-form signed quotient was not recognized"，恢复后通过。
- `ctest --test-dir build`：12/12（含 fortune x86_64/i386）。

| 指标 | 改前 | 改后 |
|---|---|---|
| fortune-x86_64 `(c - 52) % 26` ×4 | magic `mul lshr/ashr` + `-26*q` 链 | **`srem i32, 26`**，`1321528399` 归零 |
| 单测 + A/B | 新用例失败 | 通过 |
| ctest | 12/12 | 12/12 |
| memcached unknown | 0 | **0** |
| memcached 识别（无符号/有符号） | 20 / 0 | 20 / **1**（`FUN_f920` 的 d=1392，`e*2^31=1.7e11 <= 2^40` 验证过） |
| memcached IR bytes / warning / defines | 3,767,398 / 823 / 231 | 3,766,810 / 821 / 231 |
| wrk unknown | 15（ST0=7, ST1=8） | **15**（同左） |
| wrk 识别（无/有）/ value_range | 1 / 6 / 53 | 1 / 6 / 53 |
| wrk IR bytes / defines / warning | 949,730 / 89 / 318 | 949,714 / 89 / 318 |
| `llvm-as` + `opt -passes=verify` | OK | OK |
| 耗时 memcached / wrk | 586s / 61s（外部 load 23~29） | **289.6s / 34.9s**（机器空下来，回到 4m50s/35s 基线档） |

上一轮那两个耗时数字是外部负载下的，不可比；这轮恢复正常档，和更早的基线一致，
说明改动没有性能影响。

## Sleigh userop：名字接进来 + 每个 userop 一个专用占位函数（2026-09-21 续）

用户原始 prompt：

> 感觉这里应该针对对应的user op，针对性地创建intrinsic函数。那就是把遇到的这种
> userop号都识别一下吧。在原来的这种创建notdec_pcode_CALLOTHER_i64的地方加一些
> warning，提示后面开发的时候应该增加对这种user op号的支持。顺手把名字接进来
> 这一步也做吧。考虑和其他的intrinsic一致，即用notdec.register.开头，如果我没记错的话

### 名字接进来

- `PcodeProgram` 加 `UserOpNames`（下标就是 CALLOTHER 第一个常量操作数里的
  userop 号）；`SleighLift` 三处产 program 的地方（decoder 的 Impl、
  `collectSleighPcode`、`collectSleighPcodeRanges`）都用
  `SleighBase::getUserOpNames` 填。
- `PcodeToLLVM::lowerCallOther` 改成**按名字分派**（不依赖 .sla 编译期的编号
  顺序），只有名字表缺失（手工构造的 program）时才退回编号。

### 专用占位函数 + warning

- 未建模的 userop 不再落到笼统的 `notdec_pcode_CALLOTHER_i64`，而是每个 userop
  一个 `notdec.register.userop.<name>.<返回类型>.<参数类型…>`，例如
  `notdec.register.userop.crc32.i64.i64.i8` / `…i64.i64.i64`（同一 userop 的
  不同操作数宽度用类型后缀区分）。
- `notdec.register.` 前缀是关键：summary 把这个命名空间当内置 intrinsic
  （`isUnknownExternalFunction` 和 `buildInitialSignatureShapes` 都显式排除），
  所以**不做 ABI 签名重写**——旧路径的 variadic CALLOTHER 就是在这一步把
  `(i32 215, i64 %acc, i64 %data)` 全丢光的；现在实参原样保留，也不会按未知
  外部调用生成 clobber。
- 每个 userop 打一条 warning（进程内去重，避免大二进制刷屏）：

  ```text
  warning: unsupported Sleigh userop 215 (crc32) at 0x1fa80; emitted
  notdec.register.userop.crc32.i64.i64.i8 placeholder - add semantic support in
  PcodeToLLVM.cpp lowerCallOther
  ```

- helper 声明清理（`eraseUnusedRegisterHelperDeclarations`）加上
  `notdec.register.userop.` 前缀，并像 `eraseDeadUnknownValueCalls` 一样导出、
  在 tool 的最后一次 InstCombine+SimplifyCFG 之后再补一次：那次化简可能才删掉
  死块里的调用。没补这一步时 memcached 留下了 **17 条没人用的 cpuid 声明**。

### 盘点：三个样本里遇到的 userop

| 号 | 名字 | 处理 |
|---|---|---|
| 17/18 | LOCK / UNLOCK | 忽略（读写 p-code 已表达语义） |
| （按名字） | rdtsc | `llvm.readcyclecounter` |
| 128 | MOVMSKPD | 显式逐 lane 取符号位 |
| 215 | crc32 | 占位（memcached `FUN_1fa40`，11 个调用点：8×i64 + 3×i8） |
| 44–60 | cpuid / cpuid_* | 占位（memcached 一条 `0f a2`，Ghidra 展开成 17 个 CALLOTHER；所在函数最后被 DCE） |
| 77 | invalidInstructionException | 占位（memcached / wrk 的 `ud2`） |

顺带发现/修掉的两件事：

1. **旧代码 `Offset == 77 → readcyclecounter` 是错的**：这个 .sla 里 77 =
   `invalidInstructionException`，memcached 0xb8b3 正是 `ud2`（`0f 0b`）。
   以前 `ud2` 被静默当成 rdtsc。改成按名字分派后，真实 rdtsc（另一个号）由名字
   命中 `readcyclecounter`——用 `__rdtsc()` 自建二进制验证：输出里 2 条
   readcyclecounter、没有 userop warning；`ud2` 则明确变成
   `notdec.register.userop.invalidInstructionException.i64` 占位。
2. 旧路径把每个 userop 都当"未知外部调用"，产生大量 clobber：memcached 的
   `remaining_summary_clobber_value` 438 → 382、
   `unknown_external_arity_stopped_at_call_clobber` 27 → 5。占位调用把读写放在
   实参/返回值里（cpuid 的 17 个 leaf 各自带输出 varnode），不再无差别毒化
   caller-saved。

### 单测

- `tests/pcode_to_llvm_test.cpp`：`testUnknownUserOpGetsNamedPlaceholder`
  （命名 + 类型后缀、实参保留、不再出现笼统名）、
  `testUnknownUserOpWithoutNamesUsesIndex`（无名字表退回 `unknown_<号>`）。
- `tests/native_register_summary_ssa_test.cpp`：
  `testFinalCleanupDropsUnusedUserOpPlaceholders`（关掉 GlobalDCE，
  确认未用声明被清掉且计数 +1）。

### 验证

| 指标 | 改前 | 改后 |
|---|---|---|
| 笼统 `notdec_pcode_CALLOTHER_*` | memcached 9 处 | **0**（全部命名占位） |
| memcached userop 占位 | — | 13 个调用（crc32 11 + invalidInstructionException 4）、3 条声明；17 条未用 cpuid 声明被清掉 |
| memcached warning 行 | 821 | **760**（clobber 类减少；新增 19 条 userop warning） |
| memcached unknown / IR bytes / defines | 0 / 3,766,810 / 231 | 0 / 3,782,229 / 231 |
| wrk unknown / IR bytes / defines / warning | 15 / 949,714 / 89 / 318 | 15 / 949,686 / 89 / 319 |
| 单测（pcode_to_llvm / summary_ssa） | 过 | 过（+3 用例） |
| `ctest --test-dir build` | 12/12 | 12/12 |
| `llvm-as` + `opt -passes=verify` | OK | OK（memcached / wrk） |
| 耗时 memcached / wrk | 289.6s / 34.9s | 288.0s / 34.4s |

### 占位的定位：够类型推理用就行，不做语义展开

这条链路的产物是喂类型推理（htype / microsub2）的，占位函数不需要"算出真值"，
只要满足：

1. **宽度正确**：返回类型 = p-code 输出 varnode 的位宽，实参类型 = 各输入位宽；
   同一 userop 的不同宽度用类型后缀区分（crc32 的 `i8`/`i64` 两个签名）。
2. **访存还在**：内存操作数在 p-code 里本来就是独立的 LOAD，占位调用拿到的是
   已加载的值，类型推理仍能看到那次访存和它的宽度。例（memcached crc32q）：

   ```llvm
   %unique_df00_8 = load i64, ptr %notdec_ram_ptr29, align 1
   %35 = call i64 @notdec.register.userop.crc32.i64.i64.i64(
             i64 %notdec.reg.insert.lower116, i64 %unique_df00_8)
   ```

3. **有具体原型**：不再是 `i64 (...)` variadic，消费者按普通函数签名处理即可；
   `notdec.register.userop.` 前缀就是"这是未建模指令"的可机器识别标记。
4. 需要真语义（比如以后要重编译执行）时再展开；现在没有这个消费方，不做。
   cpuid 的各 leaf 同理。

- warning 是进程内去重、打到 stderr；IR 输入路径不进 lifter，所以
  `cli-signature-rewrite-ll` 那条"默认 stderr 必须为空"的红线不受影响。

## wrk IR 盘点：没有新 bug，但记两条结论（2026-09-21 续）

用户要求"看看哪里还有明显的问题值得修的，比如说 WRK 的 IR"。盘了一遍，结论：

### 1. 间接调用/函数指针提升的低命中率是**设计使然**，不是漏提升

数字：wrk `seen=50 promoted=2 slots=15`，memcached `seen=54 promoted=0 slots=45`。
单看像 bug，在 promotion pass 里临时打印槽状态（已撤）后实测：

- 目标是**未定义外部声明**的槽（wrk 的 `ssl_read/ssl_write/ssl_close/ssl_connect`、
  `sock_*`）→ `target->isDeclaration()`，本来就不提升；
- `0x142c0 sock_connect`、`0x142d0 sock_read`、`0x14100`（zmalloc 的 OOM handler，
  重定位初值 `FUN_7b30`）在 IR 里有真实 store：`main` 里
  `store ... inttoptr(82624)`、`zmalloc_set_oom_handler` 里
  `store i64 %RDI.arg, ptr inttoptr(82176)`——运行时可写，拒绝提升是对的；
- `0x142e0` 有 2 个目标（sock_readable），也拒绝；
- 其余 `FPCALL unresolved` 都是运行时算出来的指针（`inttoptr %unique_...`），
  静态提不了。

结论：这条路没有"明显 bug"，别再去追低提升率。

### 2. wrk 的 15 条 unknown 全是 x87 入口 ST0/ST1（已细化，更正上一版"死值"的粗话）

6 个函数全是 internal：`format_time_us/s`、`stats_mean/stdev/percentile/within_stdev`、
`FUN_81d0`。形态：窗口模型在入口读 `@ST0`/`@ST1`（load 上带
`!notdec.x87.window.shift`），当作"被挤出窗口的旧值"传给 `notdec.x87.push`；
range 侧 `rangeMayComeFromEntry` 用**函数出口级**的 MayEntry=0 拒绝入口值
（`NativeRegisterSummarySSA.cpp:6435`，和当初 main 的 RDI 是同一类"口径按整函数
而不是按读取点"的问题），于是 phi 兜底物化成 `notdec.unknown.i80`。

**这些值到底有没有被用**（对 CFG 做 LIFO 栈内容模拟，再按使用点分类；不是只看
min-depth，也不是只看"有没有 use"）：

| 函数 | 入口值经 pop 回到的值 | 结论 |
|---|---|---|
| format_time_us / stats_mean / stats_percentile / FUN_81d0 | pop 结果没人用，或只喂 `push` | 死 |
| stats_stdev | `%23/%29/%30` 只喂 `push`，`%34` 没人用 | 死 |
| stats_within_stdev | `%25/%37` 进 ST1 的 phi，但该 phi **只喂 push**；`%26/%38` 只喂 `push` | 死 |

- 真正被算术/比较消费的 `peek(i)`（stats_stdev 的 peek(3)、stats_within_stdev 的
  4 个 peek(2)）都发生在 **depth > i** 的位置（模拟深度 3/4），读的是真实槽位，
  不可能读到入口值；`poke` 同理。
- **更正**：不能说"没人用"。准确说法是：只参与 x87 窗口记账（push/poke），不进入
  任何算术/比较/存储/调用/返回；LLVM 意义上不是死值（消费者是对外不可见的
  `notdec.x87.*`），所以不能靠 DCE 掉。

**修法候选**：

- (A) 最小：在 `entryRangeInput` 里给空槽一条规则，和 DF 的
  `isAbiBoundaryZeroRegister` 并列——x87 窗口搬移读（`!notdec.x87.window.shift`）
  在本路径上没有定义时返回 **poison**，不要退化成 unknown。前提是"函数入口 x87
  栈为空"。改动局部、不影响真实数据流：poison 只会流进记账调用。
- (B) 彻底：给窗口模型加**深度跟踪**（入口深度 0）。push 在 depth<2 时不读旧槽、
  也不往库里 spill；pop 在 depth≤2 时新 ST1 就是空（poison），不调 `pop()`。
  这样入口寄存器根本不会被读，`format_time_us` 这种深度从不超过 2 的函数的
  push/pop 记账会全部消失。代价是跨分支/循环的深度需要数据流，无法证明时回退到
  今天的形态（保留 unknown）。

**ABI / linkage 的边界**：

- 需要的假设只有一条：**函数入口 x87 栈逻辑为空**。SysV ABI 把它写进调用约定，
  对 public/导出函数是硬保证；对 internal 函数，编译器理论上能自定义约定，但
  clang/GCC 不会拿 x87 栈跨调用传值（x87 不参与参数传递，唯一接口是 long double
  返回值、调用方立刻消费；LLVM/GCC 也没有跨函数的 x87 深度优化）。
- 本项目已经在 internal 函数上依赖同类 ABI 假设：callee-saved 保留、DF 边界为 0、
  栈参数布局（见 2026-09-17/18 的记录）。把"入口 x87 深度 0"并入同一类假设是一致的，
  但要在代码注释和文档里写明。
- 不想靠假设也行：`--all-confirmed` 已经 lift 了整个二进制，可以用同一个深度数据流
  算出**每个调用点上 x87 深度是否为 0**；"所有调用点都可验证为 0"的 internal 函数
  直接成立，没有内部调用者的导出函数用 ABI 兜底。这是 (B) 的副产品。

### 3. 方法论坑：objdump 的 x87 `fdivp/fdivrp` 助记符是互换的

差点据此误报"format_time_us 的除法反了"：`DE F1` 在 objdump（AT&T）里显示
`fdivp %st,%st(1)`，而 Ghidra spec（`ia.sinc`）里 `DE F1` 是 **FDIVRP**
（`ST1 = ST0 / ST1`，`:FDIVRP is ... byte=0xF1`），`DE F9` 才是 FDIVP。
按 spec 核对后 IR 的 `x / C` 是对的。已写进 `docs §1.2`。

### 其他扫描结果（都不是问题）

- 无 `poison`/`undef` 返回；60 处 `unreachable` 是 `ud2`/abort 路径；
- 75 个 `summary_clobber` 都是"调用后读 caller-saved"的诚实结果；
- 最大的函数 `http_parser_execute`（6670 行）是大 switch，无结构异常。

## x87 入口窗口修掉：空槽 poison + 深度精确模型（2026-09-21 续）

用户原始 prompt：

> 对，直接都做吧。确实符合的话，也可以都假设每个函数都符合这个约定。这个倒是可以
> 看看LLVM源码里面的代码生成是不是这么假设的

### 1. 先查 LLVM 源码：假设只在 x86-64 成立

/sn640/NotDec/llvm-source/llvm/lib/Target/X86/X86CallingConv.td：

- x86-64 SysV：long double 参数走**栈**（CCIfType<[f80, f128],
  CCAssignToStack<0, 0>，约 :598 / :836），返回值在 FP0（"Long double types are
  always returned in FP0"，约 :289）→ **入口 x87 栈一定为空**。
- i386：CCIfType<[f80], CCAssignToReg<RC.FP_CALL>>（约 :144）——long double 参数
  按 ABI **就在 x87 寄存器里**，入口栈非空。所以这个假设**不能推广到 32 位**。

实现分别用 isLikelyX86_64SysVAbi(Abi)（A）和 pointerByteSize() == 8（B）守卫；
i386 完全保持旧行为（fortune-i386 产物只有编译器已知的指针序 metadata 抖动，值一致）。

### 2. (A) 空槽 = poison（NativeRegisterSummarySSA.cpp）

entryRangeInput 里加一条和 DF 的 isAbiBoundaryZeroRegister 并列的规则：x86-64 SysV
下 ST0..ST7 的入口读按 ABI 就是空槽，返回 **poison**，不要落到 entry_range_rejected
→ notdec.unknown。这条对所有 ST 入口读生效（不只窗口搬移读），是 (B) 证不出深度时
的兜底。

### 3. (B) 深度精确的窗口模型（PcodeToLLVM.cpp）

- computeX87BlockDepths：块内按 X87WindowSpec 累加 push/pop（Library、未识别的
  x87 操作、间接跳转、空 native 块、非末块无终结符都判失败），块间按 CFG 不动点
  传播；入口深度 0，**汇合深度不一致或某条路径弹空就整函数放弃**。
- 深度可证（X87Depth 有值）时：
  - push 在 depth<2 不读/不写不存在的旧槽，也不往库 spill；
  - pop 在 depth<=2 不调库，新 ST1 只在 depth>=3 时从库取；
  - 库深度从"机器深度"变成 max(depth-2, 0)，**栈顶仍然是 ST(2)**，peek/poke 的
    槽位语义不变（runtime 的 peek(i) 只能按"相对库顶"解释，否则旧协议在 depth=3
    时也对不上）。
- 证不出来整函数退回旧模型；即使退回，(A) 保证不再产生入口 unknown。
- 效果（wrk）：notdec.x87.push 55 → **19**、pop 61 → **22**；format_time_us /
  format_time_s / stats_mean / stats_percentile 的记账调用**全消失**，算术/比较/
  访存逐函数 diff 一致（只有 SSA 重编号）。

### 4. 单测

- tests/pcode_to_llvm_test.cpp 新增 testX87ShallowWindowHasNoLibraryTraffic
  （depth 0/1 完全不碰库）、testX87DeepWindowSpillsToLibrary（第三次 push 溢出 1 次、
  depth 3 的 pop 取回 1 次）；更新 testX87FildlFoldsToWindowPush /
  testX87FldzFoldsToWindowPushConstant 为 depth-0 语义（不再 spill、ST1 留空）。
- (A) 没有单独立用例：要手工搭 !notdec.abi 元数据才能让 isLikelyX86_64SysVAbi
  成立；由 wrk e2e 覆盖（unknown 15 → 0）。

### 5. 验证

| 指标 | 改前 | 改后 |
|---|---|---|
| wrk unknown | 15（ST0=7, ST1=8） | **0** |
| wrk notdec.x87.push / pop | 55 / 61 | **19 / 22** |
| wrk IR bytes / warning / defines | 949,686 / 319 / 89 | **937,141** / 306 / 89 |
| memcached unknown / bytes / warning | 0 / 3,782,229 / 760 | 0 / 3,782,233 / 760 |
| 单测 pcode_to_llvm / summary_ssa | 过 | 过（+2 用例） |
| ctest --test-dir build | 12/12 | **12/12** |
| fortune x86_64 unknown / residue audit | 0 / header-only | 0 / header-only |
| llvm-as + opt -passes=verify | OK | OK（memcached / wrk） |
| 耗时 wrk / memcached | 34.4s / 289.6s | 34.0s / 285.9s |

### 顺带：乘法的盘点结论（回答"乘法还有没有类似魔数模式"）

四份产物里的剩余常量乘（memcached 157 / wrk 17 / fortune-x64 9 / fortune-i386 1）
按形态分：

- **哈希/定点常数**（真乘法，什么都不用做）：memcached `FUN_13a60` 的 xxHash
  （`0x165667B19E3779F9`、`0x85EBCA6B` 等 14 条 `mul + lshr + xor`）、wrk 的
  时间换算（`*1000`/`*1000000`）、`*40/*120/*536` 的结构体步长；
- **取模残形** 4 处（本轮修掉）；
- **除法型 `mulhi + 修正` 链**：其余产物 0 处。

所以乘法本身没有新的 matcher 可加；剩下的都在除法/取模的"写法变体"里。


## 调用点形参槽位跨分区段：按重叠段拼接（2026-09-21 续）

用户原始 prompt：

> 当前的问题是都完全修好了吗。是的话可以找下一个值得修的地方

### 1. 先把"还有没有别的问题"变成数据：Bench2 light sweep

上一轮之后 memcached / wrk / fortune-x86_64 的 unknown 都是 0，fortune-i386 剩 1 个
（死函数、无调用者）。为回答"下一个值得修的地方"，跑了一轮
`/tmp/sweep/run.sh`（`--all-confirmed --skip-runtime --register-ssa-summary`，
单目标 1500s 上限）：

| 目标 | unknown | 主要 reason（修复前） |
|---|---|---|
| lighttpd-angel | 0 | - |
| vsftpd | 5 | call_arg_rewrite_missing_binding ×5 |
| libuv | 16 | call_arg_rewrite_missing_binding ×8 + return_binding_uses_clobber_value ×8 |
| lighttpd | 783 | call_effect_mixed ×663 + call_arg_rewrite_missing_binding ×89 + type_mismatch ×4 |
| ffmpeg | 4920 | call_arg_rewrite_missing_binding 为主 |

### 2. 根因：callee 形参槽位宽度不一定落在 caller 的分区边界上

libuv `uv_udp_set_membership` → `FUN_26a50` 的 RDX/RCX 槽位是 i32，而 caller 对
@RDX 的计划分区是 [0,16)+[16,64)：i16 partial read 贡献 16 这个边界，i64 存取贡献
0/64。于是对 [0,32) 的请求：

- `plannedRangesCovering(0,32)` 因为最后一段 [16,64) 越过请求右端而返回空；
- 原来的兜底只找"单个包含段"，[0,16) 与 [16,64) 都不包含 [0,32) → nullptr；
- `callArgStoreBindings` 在第一个失败槽位就 break，同一个调用点的 RDX、RCX 一起
  报 `call_arg_rewrite_missing_binding`。

raw lift（`--no-register-ssa-pass --no-instcombine-pass`）里
`store i64 16, ptr @RDX` / `store i64 4, ptr @RCX` 都在，值并没有丢，只是读不出来。

### 3. 修法：composePlannedRangeRead（NativeRegisterSummarySSA.cpp）

`readSlotRangeBefore` 的 `ranges.empty()` 兜底从"找单个包含段"改成按**重叠分段**
拼接：

- 按顺序取与请求相交的计划分段，要求从请求左端开始无空洞地覆盖到右端，否则放弃；
- 每段用 `readRangeBefore` 读出整段值（要求类型匹配 + 支配调用点），
  `extractBitsFromIntegerValue` 截出相交位，`insertBitsIntoInteger` 插回请求宽度；
- 单段就能覆盖整个请求时直接返回该段（保持旧行为）；
- 任一段缺失/未知就整体返回 nullptr，仍然保守。

旧兜底是它的子集（单包含段 = 一个覆盖全请求的段），所以是补全而不是新开一条路。

### 4. 验证

| 指标 | 改前 | 改后 |
|---|---|---|
| libuv unknown | 16 | **8**（8 个 missing_binding 全部消失，剩 8 个纯 clobber 返回） |
| libuv IR bytes / warning | 2,772,152 / 74 | 2,770,747 / 66 |
| vsftpd unknown | 5 | **4**（FUN_18120 的 i16 RSI 槽修掉，剩 FUN_17f60 RCX ×4） |
| vsftpd IR bytes / warning | 1,709,556 / 167 | 1,709,291 / 166 |
| memcached unknown / bytes / warning | 0 / 3,782,229 / 760 | 0 / 3,782,437 / 755 |
| wrk unknown / bytes / warning | 0 / 937,141 / 306 | 0 / 937,161 / 306 |
| 单测 native_register_summary_ssa_test | 过 | 过（+testCallArgSlotSpansPlannedRangeSegments） |
| ctest | 12/12 | **12/12** |
| fortune x86_64 / i386 regression | 过 | 过（EXIT=0） |
| llvm-as + opt -passes=verify | OK | OK（libuv/vsftpd/memcached/wrk） |

libuv 修完后的调用点（原先两个 `arg_unknown`）：

    call i64 @FUN_26a50(i64 %RDI.arg, i64 %notdec_stack.native.int531, i32 16, i32 4)

单测用 RDX 的 i16 partial read + i64 存取造出 [0,16)+[16,64) 分区，callee 只要
i32 RDX，断言最终调用参数是常量 16 且没有 missing_binding 告警。

lighttpd 用修复后的二进制重跑仍是 783、reason 分布一模一样（missing_binding 还是
89（去重后 77 组）、call_effect_mixed 还是 663）：这类与"分区拼接"无关，正是下面
(A) 的伪形参问题。


### 5. 顺带盘出来的下一批目标（按证据强度）

修完后剩下的 unknown 分三类，只有第一类是真 bug。

**(A) 伪形参：`push %rcx` 栈对齐（vsftpd 4 个；也是 lighttpd 89 个里的一部分）**

vsftpd FUN_17f60（内部函数，真实签名 2 个参数）的实测汇编：

    17f60: endbr64
    17f64: cmp    %esi,%edi
    17f66: je     17f8c
    17f68: push   %rbp
    17f69: mov    %rsp,%rbp
    17f6c: push   %rbx
    17f6d: mov    %esi,%ebx
    17f6f: push   %rcx        <-- 只为把栈对齐到 16 字节
    17f70: call   6190        (dup2)
    ...
    17f79: pop    %rax        <-- 弹出的值没人用
    17f7a: pop    %rbx
    17f7b: pop    %rbp
    17f7c: ret

lighttpd array_insert_unique（真实签名也是 2 个参数）是同一种写法：

    20383: push   %rbp
    20384: mov    %rsp,%rbp
    20387: push   %rbx
    20388: mov    %rsi,%rbx
    2038b: push   %rcx        <-- 对齐
    2038c: call   FUN_202c2
    ...
    203db: pop    %rax        <-- 丢弃
    203dc: pop    %rbx
    203dd: pop    %rbp
    203de: ret

lift 之后 push 变成"读入口 @RCX + 写 native frame 槽"，pop 变成"读该槽 + 写 @RAX"。
summary 的入口读判定是前向的：只要入口 @RCX 被读到就记 read_entry，于是 RCX 变成
形参。真实调用点（如 lighttpd 1a4d9 调 array_insert_unique）根本不设 RCX，于是
call_arg_rewrite_missing_binding；更麻烦的是为了让这个不存在的形参有值，调用点会
跨调用去读 RCX，产生 call_effect_mixed 的 unknown —— lighttpd 这 663 个里很大
一部分是它的下游。

量化（脚本扫 lighttpd 最终 IR）：模块里有 **104** 个函数的某个 `.arg` 只被
native frame store 使用；77 个 missing_binding 告警里 **19** 个属于这种函数。

修法候选（要先定一个再动手）：

1. **后向 demand / 活跃性**：void 函数里 @RAX 没有 exit demand → `store @RAX` 死
   → pop load 死 → frame 槽没有活 load → push store 死 → RCX 入口读消失。
   最根本，等于给 summary 补一条"寄存器 store 看 exit demand、frame store 看槽的
   活 load"的后向活跃性；改动最大，但一次能扫掉一大片。
2. **frame 层结构性匹配**：push 的源寄存器与 pop 的目标寄存器不同（`push %rcx` /
   `pop %rax`），且弹出值在块内没有被读 → 判为对齐填充，删掉这一对。
   代码小；"没被读"可以先做块内近似（到本块下一次写该寄存器之间没有 load）。
3. **shape 精化**：对内部函数，若**所有**调用点都没有给出该寄存器形参的非 clobber
   绑定，就把它从形参里去掉（类似已有的 refineInternalStackParamShapes）。
   调用点侧立刻干净；callee body 里那次入口读要替换成 poison/unknown。

**(B) 纯 clobber 返回（libuv 8 个）——保守正确，不建议为好看而改**

uv_free_cpu_info / uv_os_free_environ / uv_free_interface_addresses 这类：函数尾是
间接调用（free），@RAX 的出口值只能来自 clobber，于是签名给了 RAX 返回槽、值降级
成 notdec.unknown。C ABI 下 void 函数的 RAX 本就是未定义值，调用点拿到"有值但
不知道"比强行改成 void 更准确。除非有别的证据（比如外部原型表确实知道它返回
void），这一类别算预期内的保守占位。

**(C) call_effect_mixed（lighttpd 663 个、ffmpeg 更多）——多数是 (A) 的下游**

已追过一条链：lighttpd configparser 里 `call @array_get_element_klen` 之后
RSI/R8/R9 的 range 值变成 unknown，phi 一路汇到 `call void %84(...)` 的实参。
这些寄存器在该调用点并不是真实参数（compiler 只把 SIL 等低位当 scratch 用），
所以按 (A) 把伪形参去掉后，这一层会跟着大幅下降；剩下真正路径不敏感的部分再单独
评估（需要 per-path demand，属于大改）。

### 6. 下一步

下一个动手目标定为 (A)，三个候选里先选 1（后向 demand）还是 2（结构性匹配）需要
先定：1 更根本、覆盖面大但风险高；2 小、能立刻消掉这批，但只治 push/pop 这一种
写法。

### 7. 调用边界也进分区（planRegisterRanges，2026-09-21 续）

用户 prompt：

> 对，在 planRegisterRanges 里也考虑一下调用边界吧。应该还好

做法：新增 `addCallParamRangeBoundaries`（NativeRegisterSummarySSA.cpp，紧挨
addSummaryDemandRangeBoundaries 之后调用）。遍历本函数里 `isAnalyzableCall` 的
调用点，对拿得到 shape 的（直接调用；间接调用的 shape 是规划之后
addIndirectCallsiteShapes 现推的，这里拿不到）把 `callParamSlots()` 里每个带
Unit 的槽位 [offset, size) 加成边界。这样 callee 形参槽位请求天然整段平铺，走
assembleRangeReadIfDominating 的常规路径；按重叠段拼接（composePlannedRangeRead）
从此只作为"规划时拿不到 shape"（间接调用、晚期精化）的兜底。

效果（对比只保留拼接兜底的上一版，同机同期跑）：

| 目标 | unknown | IR bytes（前 → 后） | warning | verify |
|---|---|---|---|---|
| libuv | 8 → 8 | 2,770,747 → 2,771,321（+0.02%） | 66 → 66 | OK |
| vsftpd | 4 → 4 | 1,709,291 → 1,712,704（+0.20%） | 166 → 166 | OK |
| memcached | 0 → 0 | 3,782,437 → 3,799,177（+0.44%） | 755 → 757 | OK |
| wrk | 0 → 0 | 937,161 → 937,812（+0.07%） | 306 → 306 | OK |

memcached 多的 2 条 warning 不是新缺口：同一批 clobber 占位从 R8[0:64) 变成
R8[0:32)+R8[32:64)（更细的粒度）。单测 testCallArgSlotSpansPlannedRangeSegments 加了
一条断言，caller 的 range_segments_planned >= 3（callee i32 槽位的边界确实进了分区），
最终实参仍是常量 16。ctest 12/12。

## 未知外部元数：不一致时按变参建模（2026-09-21 续）

用户 prompt：

> 两个都试试吧

### 1. 先确认证据来源（临时打印，已删）

给 inferExternalCallShapes 的 UnknownExternal 分支加 `NOTDEC_DEBUG_EXTARITY=<callee>`
的临时打印，跑 ffmpeg `-n main` 看 av_log 各调用点的证据：每点候选是 6 个整型寄存器
+ 6 个浮点 + 16 个栈槽，栈槽只有真写过才是 LocalDefinition（origin=0），否则
Unknown。也就是**每个点自己推出来的元数是可信的**（3/4/7…各不相同），问题在聚合：
aritiesByCallee 取 min..max，然后把 max 当成整个 callee 的固定元数。

ffmpeg 里元数不一致的未知外部有 282 个：av_log 3..14、av_bprintf 2..9、
av_add_q 1..2、av_buffer_unref 0..1…… 结果 `declare double @av_log(i64 ×14)`，
573 个调用点全部传 14 个参数；只传 3 个参数的点也去凑 8 个栈槽，于是 4563 个
missing_binding unknown（占 ffmpeg 全部 unknown 的 93%）。

### 2. 修法：固定 min 前缀 + 每点自己的尾巴

inferExternalCallShapes 在 `minArity != maxArity` 时：

- 原型改成 `VarArg=true, FixedArgs=minArity, MaxArgs=maxArity`，声明变成
  `(i64, i64, i64, ...)`；元数一致时保持原来的固定元数模型（非变参函数在所有
  调用点元数相同，不一致基本只能是变参）；
- 同时给每个调用点发一个 NativeExternalCallShape：Inputs = 该点自己的证据前缀
  （整数序列在前、浮点序列在后），callParamSlots 只取 Params.size() 之后的尾巴，
  于是每个点只传自己证据支持的实参；
- 尾巴上取不到值的槽位走已有的 shouldPoisonUnprovenVarArg → poison，不再算 unknown。

单测：testUnknownExternalArityUsesMaxCallsitePrefix 改名
testUnknownExternalArityUsesVarArgTail，断言声明是变参、固定前缀 1、两个调用点分别
传 1/3 个实参（顺带把测试里的常量真分支改成函数参数，否则 SimplifyCFG 会把长调用点
折掉）。

### 3. 效果（对比只加边界的上一版）

| 目标 | unknown 前 → 后 | IR bytes 前 → 后 | warning 前 → 后 |
|---|---|---|---|
| ffmpeg | 4920 → **8** | 7,373,354 → 4,689,227（−36%） | 11,942 → 2,073 |
| lighttpd | 787 → 754 | 5,305,599 → 5,257,857 | 2,648 → 2,553 |
| redis-cli | 129 → 128 | 4,044,258 → 4,087,405 | 1,530 → 1,523 |
| wrk | 0 → 0 | 937,812 → 927,133 | 306 → 275 |
| memcached | 0 → 0 | 3,799,177 | 757 |
| libuv / vsftpd | 8 / 4 | 不变 | 不变 |

ffmpeg 剩下 8 个：call_effect_mixed（FUN_38200:ZMM0 ×3、FUN_3e3b0:R9 ×4）+ 1 个
clobber 返回。av_log 调用点现在按点给参数（main：3×7、4×12、5×1、6×3、7×4），
与源码一致；llvm-as + verify 全 OK。

### 4. 顺带：调用边界"只收本块写过的槽位"收窄实验（不采用）

试过在 addCallParamRangeBoundaries 里只给"本块内调用点之前写过该寄存器"的槽位加
边界（理由：编译器会在调用前 materialize 实参，没写过就不是这次传的实参）。lighttpd
上省了 0.46% IR，但 4 个 call_arg_rewrite_type_mismatch 全部回来了（有些调用点没在
本块写寄存器，却要靠细分区才能取出类型正确的值），unknown 754 → 755。结论：保持
全收，代码里留了注释记录这次实验。

## 栈对齐 push/pop 被当成保存：和 callee-saved save/restore 一起认（2026-09-21 续）

用户 prompt：

> 对，按这个推进试试吧

### 1. 先看编译器源码：push 的值是 undef、pop 的是死寄存器

llvm/lib/Target/X86/X86FrameLowering.cpp 的 BuildStackAdjustment（while (Offset) 循环）：

    if (Offset == SlotSize) {
      // Use push / pop for slot sized adjustments as a size optimization. We
      // need to find a dead register when using pop.
      unsigned Reg = isSub ? (unsigned)(Is64Bit ? X86::RAX : X86::EAX)
                           : TRI->findDeadCallerSavedReg(MBB, MBBI);
      ...
        BuildMI(MBB, MBBI, DL, TII.get(Opc))
            .addReg(Reg, getDefRegState(!isSub) | getUndefRegState(isSub))

调整量正好一个槽（x86-64 的 8 字节）时 LLVM 用 push/pop 代替 sub/add：push 的
寄存器带 getUndefRegState（值不存在，固定 RAX），pop 用 findDeadCallerSavedReg 挑
一个死寄存器。GCC 的 `push %rcx` + `pop %rax` 是同一手法（vsftpd FUN_17f60、
lighttpd array_insert_unique 实测）。也就是编译器自己认为这次 push 的值是 undef、
pop 的结果是死的，而我们的 lift 只看到普通 load/store。

### 2. 位置：就是 callee-saved save/restore 那套代码

NativeRegisterSummary.cpp 里早就有保存/恢复建模：

- push 侧（store 进固定栈槽）：存的若是寄存器 R 的入口值且
  canTrackSavedEntryRegister(R)，就记 StackSlots[slot] = R、**不计入口读**；
  否则 markEntryValueRead → R 变 read_entry。
- canTrackSavedEntryRegister（:2125 附近）**只认 Abi.Unaffected（callee-saved）**，
  所以 `push %rcx` 直接落进"读"分支，把 scratch 变成形参。

按"和 callee-saved 一起识别、且发生在寄存器消除之前"的思路，在这里加第三种分类。

### 3. 实现（NativeRegisterSummary.cpp）

新增 stackAdjustmentSlots / collectStackAdjustmentSlots：

1. 候选：store 进固定栈槽、值是该寄存器的**直接入口 load**、寄存器是 caller-saved
   （非 Abi.Unaffected、非 RSP、非 ignored）；
2. 该槽的每个 load 都只被搬进别的寄存器（写回同一个寄存器 = 真 save/restore，排除）；
3. 每个搬进去的寄存器"搬完就死"：registerValueDiesAfter 从那条 store 往后走
   （块内 + 后继块，最多 64 块），期间没有显式读、也没被会消费它的调用读掉。

命中后：push 侧不计入口读、不登记保存槽；load 侧也不挂"保存值来源"。

调试中踩的两个坑（都会把真实 spill 误判成对齐填充）：

- 一开始把**任何**显式读/写当判据、忽略调用：lift 的 IR 里实参是"store 进实参
  寄存器 + call"，调用隐式消费寄存器，于是 vsftpd FUN_13d70 的 RSI spill、
  FUN_14880/FUN_13ee0 的 RDX spill 被误判，新增 5 个 entry_range_rejected。
- 改成"一切调用都算读"又太粗：FUN_17f60 错误分支的 pop 走
  `pop %rdx; jmp die`，被当成活，修复失效。
- 最终按**逐寄存器**判断：callMayReadRegister 对外部调用看 shape 的输入槽、对内部
  调用看 bottom-up effect 的 ReadEntry，未知外部/间接/effect 未算出才保守当读；
  callee 不碰这个寄存器就继续往后走。这样 FUN_17f60 的两条 pop 路径都判死，
  真实 spill 又保住。

### 4. 单测（native_register_summary_test.cpp）

- testStackAdjustmentPushIsNotEntryRead：`push RCX（入口值）→ call → pop 进
  RAX → ret`，断言 RCX.ReadEntry == false；
- testCallerSavedSaveRestoreIsEntryRead：同样 push，但 pop 写回 RCX，断言
  ReadEntry == true（真 save/restore 不能被吃掉）；
- testStackAdjustmentPopUseKeepsEntryRead：pop 进 RAX 后又有 `load @RAX`，
  断言 ReadEntry == true（保守不放行）。

### 5. 效果

vsftpd：4 个 `call_arg_rewrite_missing_binding:FUN_17f60:RCX` → **0**。
其余目标见 summary（libuv / memcached / wrk / lighttpd / redis-cli / ffmpeg）。

### 5. 效果（相对只做了 vararg 元数修复的上一版）

| 目标 | unknown 前 → 后 | IR bytes 前 → 后 | warning 前 → 后 |
|---|---|---|---|
| vsftpd | 4 → **0** | 1,712,854 → 1,710,770 | 166 → 158 |
| lighttpd | 754 → **274**（−64%） | 5,257,857 → 4,721,632（−10%） | 2,553 → 1,179 |
| libuv | 8 → 8 | 2,771,321 → 2,771,338 | 66 → 66 |
| redis-cli | 128 → 128 | 4,087,405 → 4,086,172 | 1,523 |
| memcached / wrk / ffmpeg | 0 / 0 / 8 | 基本不变 | 不变 |

ctest 12/12，fortune x86_64 / i386 回归 EXIT=0，所有目标 llvm-as + opt -passes=verify OK。

lighttpd 剩下的 274 个：call_effect_mixed 238（其中 `FUN_26313` 的 R8/R9/RSI
各 40，占 120）、entry_range_rejected 19、clobber 返回 10、missing_binding 7。
redis-cli 的 128 没动（`FUN_1e950` / `hi_sdscat*` 家族不是这个 idiom）。

## 间接调用元数被"判空/寻址读"误清零：只有"搬到别处"的读才算消费（2026-09-22 续）

用户 prompt：

> 对，详细查一下下一批这一块，然后用容易懂的方式介绍一下

### 1. lighttpd `FUN_26313` 那 120 个 unknown 的完整链条

- `FUN_26313` 是按 kind 分派的小函数：有的路径直接 `ret`（R8/R9/RSI 保持调用
  方原值），有的路径尾跳 `jmp buffer_free` / `jmp *0x8(%rax)`（控制权交给别的
  函数，寄存器被改写）。所以 summary 判它对这三个寄存器 Mixed 是**对的**。
- configparser（yacc 主循环）把这些寄存器当循环携带值用；循环里有一条路径经过
  FUN_26313，汇合点 SSA 把 range 值变成 unknown（20 个 FUN_26313 调用点 × 3 寄存器
  × 2 个 range = 120）。
- 这些 unknown 唯一的消费者是 configparser 里 5 个 vtable 间接调用的第 2~6 个实参。
  5 个调用点的汇编都只设了 RDI（对象指针），实际就是 1 个参数。
- 为什么被推成 6 个参数：debug 打出的证据是
  `idx=0 unit=RDI origin=LocalDefinition consumed=1`，其余 origin=Mixed。
  RDI 明明是实参，但同块内它被读过，ConsumedLocal 规则把它从连续证据尾部扣掉 →
  参数个数 0；0 证据时 addIndirectCallsiteShapes 走兜底 addAbiInputParamSlots，
  把 6 个 SysV 整型入参寄存器全当参数，其中 5 个是循环携带的残留 → unknown 顺着
  它们流进实参。

### 2. 第一次修法（不够）：只豁免"只置标志位"的读

先加了 loadFeedsOnlyFlags（load 的值只喂 icmp/fcmp 不算消费），build 后全量重跑，
lighttpd 仍是 274。加临时打印看 registerValuePendingAtCall 从调用点往回扫到的
load：`users=1 [add]` —— 紧挨着调用的那条 `mov 0x10(%rdi),%rax`（把 RDI 当地址
基址用）先被扫到，它喂的是 add，不是 icmp，于是规则没触发。

### 3. 最终修法：只有"原样搬到别处"的读才算消费

把判据改成 loadValueIsMovedAway：读出来的值直接存进另一个寄存器/内存（或只经过一层
cast 再存）才算"值已被物化到别处、寄存器可能只是残留"（对应
`lea name,%rcx; mov %rcx,%rdx; call f` 那种）；比较（判空）、算术/寻址都说明这个值
仍然要用，寄存器还要带着它进调用。

效果：RDI 的 ConsumedLocal 变 0 → 证据 int=1 → 间接调用形状 1 个参数 → 后 5 个假实参
连需求都不存在，120 个 unknown 一起消失。

### 4. 单测

- testFlagOnlyRegisterReadKeepsArgumentArity：四个寄存器本地定义，RCX 只被 icmp 读，
  断言未知外部被改写成 4 个参数（旧行为算 3）；
- 已有的 testConsumedScratchRegisterIsNotAnExternalArgument（RCX 被存进内存）继续
  通过，保证"真消费"没被放宽。

### 5. 效果

| 目标 | unknown 前 → 后 | IR bytes 前 → 后 |
|---|---|---|
| vsftpd | 0 → 0 | 持平 |
| libuv | 8 → 8 | 持平 |
| lighttpd | 274 → **?** | ? |
| redis-cli | 128 → ? | ? |
| memcached / wrk / ffmpeg | 0 / 0 / 8 | 持平 |

### 5. 效果（相对上一版）

| 目标 | unknown 前 → 后 | IR bytes 前 → 后 | warning 前 → 后 |
|---|---|---|---|
| lighttpd | 274 → **115**（−58%） | 4,721,632 → 4,647,952 | 1,179 → 1,064 |
| redis-cli | 128 → **110** | 4,086,172 → 4,070,497 | 1,523 → 1,490 |
| vsftpd | 0 → 0 | 1,710,770 → 1,710,774 | 158 |
| libuv | 8 → 8 | 2,771,338 → 2,771,819 | 66 → 68（clobber 占位粒度变细） |
| memcached / wrk / ffmpeg | 0 / 0 / 8 | 基本持平 | 不变 |

ctest 12/12，fortune x86_64 / i386 回归 EXIT=0，7 个目标 llvm-as + opt -passes=verify 全 OK。

lighttpd 剩下的 115：call_effect_mixed 约 90（FUN_26313 那 120 已经清零）、
entry_range_rejected 十几、clobber 返回 10、missing_binding 7。

## 变参保存区识别：混入同寄存器普通 spill 时不再整函数放弃（2026-09-22 续）

用户 prompt：

> 再看下一个明显的问题吧

### 1. 下一批的分布

lighttpd 115（mixed 79 + entry_range_rejected 19 + clobber 返回 10 + missing 7）；
redis-cli 110（mixed 83 + missing 20 + clobber 7），其中 mixed 有 **69 个**来自
`hi_sdscatfmt` / `hi_sdscatlen`，missing 20 个来自 `FUN_1e950` —— 都是 hiredis
的变参函数（`sdscatfmt(sds, fmt, ...)` 那一族）。

### 2. 现象：变参函数没被 va_start 恢复识别

- 这些函数 lift 后 summary 把 6 个 GP 寄存器 + AL 都算成形参，调用点要传 7 个实参；
- 变参重写（NativeVarArgRewrite）本该把它们改写成`(固定参数, ...)` + llvm.va_start，
  redis-cli 一共识别出 7 个（redisAppendCommand / FUN_15510 / FUN_20df0 /
  hi_sdscatprintf / redisCommand / redisFormatCommand / redisAsyncCommand），
  但 `hi_sdscatfmt` / `FUN_1e950` 漏了。

### 3. 原因 (a)：保存区里混入同寄存器普通 spill → 整函数放弃（本次修）

识别第 1 步把"load @REG + store 到帧槽（load 只有一个使用者）"都当保存区候选，
并要求**所有**候选共用一个 base = offset - 8*regIndex，否则 return nullopt。
hi_sdscatfmt 有 8 个候选：

    idx=2 off=112  idx=3 off=120  idx=4 off=128  idx=5 off=136   ← 真保存区 base=96
    idx=2 off=8    idx=4 off=16   idx=4 off=24   idx=3 off=24    ← 局部变量 spill

后 4 个是编译器把同几个入口值再存到局部变量，base 各不相同（-8/-16/0），旧逻辑直接
整函数放弃。

改成按 base 分组、取**最大的一致子集**（组内寄存器下标不重复、≥2 个）作为保存区。
注意只按 base 分组、不额外要求同一 alloca：先试过按 (帧, base) 分组，结果误伤了
redisAppendCommand / redisFormatCommand / redisAsyncCommand（它们的保存槽跨帧比较
offset），detected 从 7 掉到 5。

效果：`FUN_1e950` 现在识别成 `(i64, i64, i8, ...)`，调用点的 20 个
missing_binding 消失；redis-cli unknown 110 → **87**。

### 4. 原因 (b)：AL 链闭包被格式解析的 cmp 污染（留作下一刀）

第 3 步把**所有** `partial_read.i64.i8(@RAX)`（test al,al 的 lift 形态）当 AL 测试
根，沿使用者做闭包，遇到"存进非标志寄存器"的用途就整函数放弃。hi_sdscatfmt 的格式
解析循环里有 **92** 个 `cmp al,k`，闭包把它们全卷进来，最后撞到非标志用途 → 放弃
（临时用 __LINE__ 探针定位到第 3 步的这条检查）。

正确做法是只把"守着 XMM 保存区的那个 AL 测试"算作 va_start 的 AL 链（例如要求该链
到达的分支支配 XMM 保存 store），其余 AL 链不参与闭包、也不因其放弃。这需要给识别加
一条支配关系检查，比 (a) 大，留作下一刀。

### 5. 效果

| 目标 | unknown 前 → 后 | IR bytes 前 → 后 | warning 前 → 后 |
|---|---|---|---|
| redis-cli | 110 → **87** | 4,070,497 → 4,064,293 | 1,490 → 1,473 |
| vsftpd | 0 → 0 | 1,710,774 → 1,710,777 | 158 |
| libuv | 8 → 8 | 2,771,819 → 2,771,361 | 68 → 66 |
| memcached / wrk | 0 / 0 | 基本持平 | 755 / 276 |
| lighttpd | 115 → 115 | 4,647,952 → 4,647,536 | 1,064 |
| ffmpeg | 8 → 8 | 4,693,934 → 4,693,940 | 2,082 |

ctest 12/12，fortune x86_64 / i386 回归 EXIT=0，所有目标 verify OK。

补充单测：testVarArgPrologueWithOrdinarySpillIsDetected —— 在真保存区旁边加一份同
寄存器的普通 spill（base 不一致、槽位避开 FP 保存区和 va_list 窗口），断言仍然
detected/rewritten 且插入 llvm.va_start。旧的"所有候选共用一个 base"实现在这条用例
上会失败。

## 变参 AL 测试识别：只认"守着 XMM 保存区"的那条链（2026-09-22 续）

用户 prompt：

> 对，按这个继续推进试试吧

### 1. 现象

上一轮修掉保存区 spill 之后，`hi_sdscatfmt` 还是没被识别。用 __LINE__ 探针定位到
识别第 3 步（AL 测试闭包）：某条 AL 链的使用者里出现"存进非标志寄存器"就 return
nullopt。hi_sdscatfmt 的格式解析循环里有 **92** 个 `cmp al,k`，全都是
`partial_read.i64.i8(@RAX)`，全被当作 AL 测试根卷进闭包，最后撞到一个非标志用途
（把 AL 当数据搬走），整个变参函数被放弃。

### 2. 修法（NativeVarArgRewrite.cpp）

- 先把第 4 步（FP/XMM 保存区）挪到 AL 判定之前，收集 `xmmSaveBlocks`（含 XMM 保存
  store 的块）；
- AL 判定改成逐根独立：对每个 `partial_read.i64.i8(@RAX)` 做**标志用途**的传递闭包
  （碰到别的用途只停在那条边上，不再整函数放弃），只有当这条链经过的块里存在一个
  **条件分支直接跳到 XMM 保存块**时，这条链才算数；一条都没有才放弃。
- 无关的 AL 用途（cmp 链、把 AL 当数据搬走）既不进闭包，也不会导致放弃。

注：`plan.AlTestInstructions` 只用于记录/校验（applyPlan 不删它，清理交给后续
SSA 死代码消除），所以这次改动只影响"识别/放弃"的判定。

### 3. 效果（相对上一版）

| 目标 | unknown 前 → 后 | IR bytes 前 → 后 | warning 前 → 后 |
|---|---|---|---|
| redis-cli | 87 → **7** | 4,064,293 → 4,019,841 | 1,473 → 1,437 |
| vsftpd | 0 → 0 | 1,710,777 → 1,710,770 | 158 |
| libuv | 8 → 8 | 2,771,361 | 66 |
| memcached | 0 → 0 | 3,797,987 | 755 |
| wrk | 0 → 0 | 930,419 → 930,436 | 276 |
| lighttpd | 115 → 115 | 4,647,536 → 4,648,828 | 1,064 → 1,066 |
| ffmpeg | 8 → 8 | 4,693,940 → 4,693,934 | 2,082 |

redis-cli 只剩 7 个 `return_binding_uses_clobber_value:RAX`（纯 clobber 返回，保守
占位）。`hi_sdscatfmt` 现在识别成变参（`(i64, i64, i8, ...)`），它和
`hi_sdscatlen` 的 69 个 call_effect_mixed、以及 7 个 missing_binding 一起消失。

ctest 12/12，fortune x86_64 / i386 回归 EXIT=0，7 个目标 llvm-as + opt -passes=verify
全 OK。

### 4. 单测

testVarArgPrologueWithSpillAndAlNoiseIsDetected：在真保存区旁边同时放
(a) 同寄存器的普通 spill（base 不一致）和 (b) 把 AL 当数据存到内存的无关用途，
断言仍然 detected/rewritten 且插入 llvm.va_start。旧实现分别在 (a)(b) 两处放弃。

## 间接调用无证据时的"6 个 ABI 入参"兜底：去掉（2026-09-22 续）

用户 prompt：

> 对继续查一下吧

### 1. 下一批：entry_range_rejected（lighttpd 19 个）

分布：plugins_call_init 9（RSI/RDX/R8/R9 的 i32 range）、FUN_263db 3、FUN_f8e6 2、
FUN_3acd0 2、FUN_e78d / FUN_23c64 / buffer_copy_string_len_lc 各 1。

查最大的 `plugins_call_init`（形参只有 RDI）：

- 入口块里出现 9 个 unknown，唯一消费者是 `%13 = call i64 %12(6 个实参)`（一个
  间接调用）；
- 它的汇编（0x2403b）就是 `call *%rax`，调用点前**没有任何实参寄存器设置**；
  同一个函数里另一个间接调用点 0x240d2 设了 `mov (%r12),%rsi; mov %rbx,%rdi`，
  有 LocalDefinition 证据，于是没走兜底、也没出 unknown；
- 无证据时 addIndirectCallsiteShapes 走 addAbiInputParamSlots 兜底：把 6 个 SysV
  整型入参寄存器全当形参 → 调用点凭空要 5 个值 → range SSA 沿循环回边把需求拉回
  函数入口 → 这些寄存器在 summary 里 MayEntry=0（被调用 clobber 过），
  entryRangeInput 拒绝 → `notdec.unknown`。

### 2. 修法

证据机制的前提是：真实参要么是调用点前显式写进实参寄存器（LocalDefinition），要么
是入口透传（ForwardedEntry）。两者都没有时，寄存器里只是 caller-saved 的残留，兜底
成 6 个实参只会凭空造需求。改成**无证据就不加形参**，并删掉随之无用的
addAbiInputParamSlots。

### 3. 效果（7 个目标全量对比）

| 目标 | unknown 前 → 后 | IR bytes 前 → 后 | warning 前 → 后 |
|---|---|---|---|
| lighttpd | 115 → **106** | 4,648,828 → 4,628,926 | 1,066 → 1,039 |
| vsftpd | 0 → 0 | 1,710,770 → 1,708,034 | 158 |
| libuv | 8 → 8 | 2,771,361 | 66 |
| memcached | 0 → 0 | 3,797,987 → 3,797,881 | 755 |
| wrk | 0 → 0 | 930,436 → 930,349 | 276 |
| redis-cli | 7 → 7 | 4,019,841 → 4,019,295 | 1,437 → 1,332 |
| ffmpeg | 8 → 8 | 4,693,934 → 4,693,908 | 2,082 |

所有目标 llvm-as + opt -passes=verify OK，ctest 12/12，fortune x86_64 / i386 回归
EXIT=0。单测 testIndirectCallWithoutEvidenceHasNoArguments：没写任何实参寄存器的
间接调用，断言改写后 0 个实参。

## 部分入口读不再建窄形参（2026-09-22 续）

用户 prompt：

> 按这个感觉可以试一试

### 1. 剩余 mixed 里的一个具体子机制

lighttpd 的 config_plugin_values_init / network_init 签名里挂着 `i24 %R8.arg`
（第 5 个参数 R8 的 bit 8..32）。facts 里 `read_entry_mask=0xFFFFFF00`：

- 函数在某处读了 R8 的 32 位（`partial_read.i64.i32(@R8, 0)`）；
- 但低字节在那之前被**无条件**写过（`partial_write.i64.i8(@R8, ..., 0)`）→
  MayEntry 只剩 bit 8..32；readRegister 记 `ReadEntry = MayEntry & readMask =
  0xFFFFFF00`；
- buildInitialSignatureShapes 把这个连续段变成 i24 形参槽 [8:32) → 所有调用点都要给
  R8 补这 24 位；调用点那边 R8 又来自别的 Mixed 调用 → unknown。

ABI 参数永远从 bit 0 开始；这种"入口读只覆盖寄存器中段"是"部分重读局部写过的值 +
死 scratch 位"的组合，不是真参数。

### 2. 修法

addParamForUnit 里：paramMask 非零且 `countr_zero() != 0`（不从 bit 0 开始）就不建
形参、直接 return。调用点不再需要这几位；函数体内那一次入口读退化成
entry_range_rejected 占位（1 个，比每个调用点各一个 unknown 划算）。

### 3. 效果

| 目标 | unknown 前 → 后 | IR bytes 前 → 后 |
|---|---|---|
| lighttpd | 106 → **97** | 4,628,926 → 4,607,985 |
| vsftpd / memcached / wrk | 0 / 0 / 0 | 持平 |
| libuv | 8 | 2,771,361 → 2,765,013 |
| redis-cli / ffmpeg | 7 / 8 | 持平 |

ctest 12/12，所有目标 llvm-as + opt -passes=verify OK。单测
testPartialEntryMaskDoesNotCreateNarrowParam：先无条件写 R8 低字节再读 32 位，
断言改写后没有 R8 形参（旧行为会生成 i24 R8.arg）。
