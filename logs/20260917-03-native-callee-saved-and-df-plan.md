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

