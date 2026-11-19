# 寄存器需求两层定点（DCE × 寄存器分析结合）

> 用户原始 prompt：
>
> 启动一个goal尝试实现一下DCE和当前寄存器分析的结合吧
>
> （上文是 `20261119-01-hexx64-native-handoff.md` 的交接：§4 已讨论一致的方向是
> 把寄存器层的按位活跃信息反馈给值层初始需求、做最大不动点收窄；§5 讨论了与
> LLVM DCE 合并成统一 worklist、以及"寄存器感知 DCE"的三步走。）

## 计划

- [x] **P1 寄存器写 → 值层初始需求收窄 + 两层定点**（窄改动）
- [x] **P2 回归与指标**：fortune 19/19；hexx64 全量 A/B（同一输入、同一台机器）。
- [x] **P3 寄存器感知 DCE（实验，默认关）**：观察位为 0 的寄存器写把值换成 0、
      递归删掉死链；实现 + 单测 + 收益统计，但发现死写判定还不够 sound，默认关。
- [x] **P4 记录与提交**。

结论先说：

1. 两层定点**是对的**（结构上消除了"寄存器写 = 满宽 seed"这个放大器），在
   hexx64 上指标小幅改善：**unknown 515 → 514**，指令数 1,131,043 → 1,129,415，
   IR 86.27 → 86.19 MB，本地裸读 2213 → 2204，clobber 占位 8628 → 8603，
   LLVM 22 verify ok，fortune 19/19。
2. **R10/R11/R8 的需求链不是 liveness 假需求**：它们是"setcc 只写低字节 + 整
   寄存器读 + 返回值把高位垃圾带出去"的真实 IR 数据流。`FUN_dd270` 的
   `mov r15d,r11d`（0xdd2da）→ `mov eax,r15d`（0xdd2a4）→ `ret` 说明 R11 的
   高 24 位确实进了 EAX；只有把"整数返回的 ABI 未定义高位"建模出来才能切断，
   属于交接文档 §7 的待办，不是本轮的 DCE 能解决的。
3. **DCE 本身可行但前提未满足**：需求分析的死写判定只对"签名要不要这几位"负责，
   还没证明是"值可以删"的严格上界。实验里发现了一个**真 bug**（noreturn 调用的
   实参入口需求被整个跳过，已修），修完 fortune 上仍有 callee-saved 返回槽的
   差异，所以 `value_dce` 默认关，等需求侧做成严格上界再打开。

## 实现记录

### P1 两层定点

| 文件 | 内容 |
|---|---|
| `lib/passes/register/RegisterSummaryInternal.h` | `computeValueDemands` 增加 `observedWrites` 参数；`liveBeforeBlock` 增加观察输出参数与注释 |
| `lib/passes/register/RegisterSummarySupport.cpp` | 新增 `observedValueMask`（寄存器坐标 → 值坐标）；寄存器 store / partial_write 的值按观察位 seed（第一轮/未记录仍满宽）；`icmp`/`fcmp` 从"无条件 seed 操作数"改成 transfer（结果有需求才要操作数全位）；`select` 条件补需求 |
| `lib/passes/register/RegisterSummarySignature.cpp` | 走查时在 `eraseDemand` 之前记录 `live ∩ 写入位段`；32 位 GPR 写只观察低 32 位；`reportWriteObservations` 诊断 |
| `lib/passes/register/RegisterSummaryRewrite.cpp` | `analyzeCallerDemand` 两层交替到不动点；收集死写；`NOTDEC_TRACE_DEMAND` 按轮输出 |

语义要点：

- 观察位 = "该写之后、被下一次写覆盖之前"仍被读的位：走查在 `eraseDemand`
  之前取 `live ∩ 写入位段`，天然就是这个集合。整寄存器 store 记整寄存器；
  partial_write 记写入位段；x86-64 的 32 位 GPR 写只记低 32 位（高位是清零，
  不是这次写提供的值）。
- 单调性：观察位越窄 → 值层 seed 越窄 → 读的位需求越窄 → 走查 live 越窄，
  每轮都是上界；从满宽下降，停在最大不动点，不会比真实需求少。
- 三道保险不变：表里查不到的值仍按全宽（`valueDemand` 默认全 1）、未记录的写
  按满宽、未知调用/别名走查给宽解。
- 内层块级走查只在"这一遍没有改动任何块"时采用该遍的观察结果（否则观察位是
  中途状态的欠近似）。
- 轮数上限 8（一般 2 轮稳定：第二轮收窄、第三轮确认）。

### 诊断

`NOTDEC_TRACE_DEMAND=<寄存器名>` 现在会额外输出：

```
DEMANDROUND fn=... round=N
  READ bb=... off=... width=... demand=...
  LIVEMASK bb=... in=... out=...
  WRITE bb=... observed=...
  ENTRY round=N demand=...
```

`--register-ssa-summary` 会多一行：

```
Native register write observations: dead=62032 live=400223 RAX:98128 ...
```

`NOTDEC_DUMP_DEAD_WRITES=1` 打印死写明细（DCE 的输入）。

### P3 寄存器感知 DCE（`NativeRegisterValueDce`，`value_dce` 默认关）

- `lib/passes/register/NativeRegisterValueDce.cpp`：对观察位为 0 的寄存器写，
  把写入值换成 `0`（写本身保留：它仍把寄存器定义成 non-entry，影响签名事实），
  然后从旧值出发用 worklist 删除没人用的纯值链（算术/比较/cast/phi、ctpop 等
  算值 intrinsic、`notdec.partial_read.*`、`notdec.unknown.*`、位段抽取插入）。
  绝不删 store、真实调用、终结指令、alloca、`notdec.register.*` 里的有状态 helper。
- `RegisterSummaryAnalyzer::run` 增加 `PruneDeadRegisterValues`：DCE 之后**重跑
  一遍完整分析**（Effects/ReadEntry 依赖这些读），然后才 attach metadata。
- pipeline 配置新增 `register-elimination.value_dce`（默认 false）。

**为什么默认关**：拿 fortune 做实验（172 个死写、清 164 个值、删 104 条指令），
`register-elimination` 之后的 IR 出现新的 callee-saved 入口裸读、`FUN_4db0` 的
返回类型从 `void` 变成 `i64`（多出 RBP 返回槽）。排查出第一个根因是：

- `applyBackwardCallDemand` 对 noreturn callee 直接 `live.clear(); return;`，
  **跳过了 callee 的入口需求**。于是 `FUN_4db0` 在 `call @FUN_27a0()`（noreturn）之前
  给 RDI 准备的实参被当成死写，DCE 把值换成 0 → 实参丢了。已修：noreturn 只清
  返回后的 live，调用点读的入口位继续往下走（`DEMANDEXIT` 语义不变）。

修完后 fortune 仍不通过（callee-saved 返回槽差异），说明还有别的地方"需求没覆盖
= 没人用"不成立（例如 ABI 边界/返回槽的闭环依赖签名决策，而不是纯 liveness）。
结论：**DCE 的删除判据需要比签名需求更强的 soundness 论证**；当前保留实现和
单测（`testRegisterValueDceClearsDeadWriteAndPrunesChain`），默认关，等需求侧
收口后再评估。

### 顺带修掉的 bug：noreturn 调用的入口需求

`applyBackwardCallDemand` 原来：

```cpp
if (effect.NoReturn) { live.clear(); return; }   // 入口需求也被跳过了
```

现在清完 live 继续走入口需求循环。hexx64 指标不变（说明这类调用点的入口需求
本来就由别处覆盖），但语义上必须修，否则任何基于需求做删除的变换都会踩到。

## 指标（hexx64 全量，同一输入、同一台机器）

| 指标 | 35b3855（base） | 本轮 | 变化 |
|---|---|---|---|
| `unknown.calls` | 515 | **514** | −1 |
| 指令数 | 1,131,043 | **1,129,415** | −1,628 |
| IR 大小 | 86,266,787 B | **86,189,341 B** | −77 KB |
| 本地裸读 | 2213 | 2204 | −9 |
| clobber 占位 | 8628 | 8603 | −25 |
| `abi_undefined.calls` | 0 | 0 | — |
| verify（LLVM 22） | ok | ok | — |
| ctest | 19/19 | 19/19 | — |

unknown 细分：R10 33→31、RDX 9→8、RAX 82→83（+1），其余不变；R8=36、R9=96、
R11=30、RCX=11、RDI=3、RSI=12。
需求图规模：R10 487→481 节点，R8 545→544，R11 478 不变，R9 1305 不变。

## R10/R11 链的结论（为什么这轮没有大收益）

以 `FUN_dd270` 为例（0xdd2ca–0xdd2dd）：

```asm
dd2cd: 41 0f 95 c3   setne  r11b          ; 只写低 8 位
dd2d7: 41 08 c3      or     r11b,al
dd2da: 45 89 df      mov    r15d,r11d     ; 读整 32 位（高 24 位是垃圾）
...
dd2a4: 44 89 f8      mov    eax,r15d      ; 返回值 EAX = r15d
```

lifted IR 里 `%R11D = partial_read.i64.i32(@R11)` 的值确实被写进 R15、再经
`zext`/`store @RAX` 进入返回值。也就是说 **32 位读不是死读**：高 24 位是调用者
的 R11 垃圾，但 IR 语义上会被返回。需求分析要求 R11 入口位是"忠实"的，
要切断只能把"整数返回的高位是 ABI 未定义"建模出来（交接文档 §7 的待办），
不能靠收窄/删除。`FUN_1d01c0` 的 `setcc/and/test` 那条链在单函数视角下已经能
收到 8 位（`DEMANDROUND` round 2 起 `demand=FF`，入口需求为空），但全量里它的
`read_entry_mask` 仍是 `0xFFFFFF00`——那是 forward 分析的"读过哪些入口位"，
不是收窄后的 demand；两者的口径差异见 `RegisterSignatureSlots.cpp` 的
ReadEntryMask/EntryDemandMask 注释。

## 下一步（按收益排序）

1. **整数返回的 ABI 未定义高位**（交接 §7 第一条）：把 `int`/`bool` 返回的
   RAX 高位标成 `notdec.abi_undefined`，并让需求不在这些位上闭环。这是切断
   R10/R11/R8/R9 链的关键，预计能消掉 30+31+36+96 个 `return_binding` unknown
   里的一部分。
2. **可观察性阶段 2**：用 `UsedInputs` 裁剪 `ReadEntry`（RDI 144 / RAX 181 /
   RSI 38 个 cuttable_callsites），把"读了但没人观察"的入口需求从源头去掉。
3. **统一 worklist**：把 `computeValueDemands` 和 `liveBeforeBlock` 合并成
   一个工作项分 `ValueNeeds`/`RegisterNeeds` 的定点，sink 判定换成 LLVM 的
   `wouldInstructionBeTriviallyDead`。本轮的两层交替已经把这个定点的语义固定
   下来，合并主要是省一层循环和让删除判据更硬。
4. **DCE 重新打开**：等 1/2 做完、需求变成严格上界后，再评估
   `value_dce=true`（单测和实现已就绪）。

## 涉及文件

| 文件 | 内容 |
|---|---|
| `lib/passes/register/RegisterSummarySupport.cpp` | `computeValueDemands` 观察位 seed、icmp/fcmp/select transfer |
| `lib/passes/register/RegisterSummarySignature.cpp` | `liveBeforeBlock` 观察位、noreturn 入口需求修复、死写诊断 |
| `lib/passes/register/RegisterSummaryRewrite.cpp` | `analyzeCallerDemand` 两层定点、死写收集、按轮诊断 |
| `lib/passes/register/RegisterSummaryCore.cpp` | DCE 钩子（PruneDeadRegisterValues） |
| `lib/passes/register/NativeRegisterValueDce.cpp` / `include/.../NativeRegisterValueDce.h` | 寄存器感知 DCE（默认关） |
| `lib/passes/register/NativeRegisterElimination.cpp`、`lib/core/NativePipeline*.{cpp,h}` | `value_dce` 选项接线（默认 false） |
| `tests/native_register_core_test.cpp` | DCE 机制单测 |
