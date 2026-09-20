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

