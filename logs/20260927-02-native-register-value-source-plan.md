# native 寄存器值的来源完整性（不许裸寄存器读，也不许“建模缺口 unknown”）

## 0. 用户原始 prompt

> 返回unknown感觉也不对。为什么总是喜欢用 unknown 呢？那个只是当前代码修复过程中的临时产物。
> 你在底层汇编里面，再怎么值都有来源的除非是什么奇怪的脏值，一般也很难真的是 unknown 的。
> 现在 unknown 被滥用了，重新写一个规划吧。我上面说的三点中，除了都不应该产生裸寄存器访问外，
> 也都不应该产生 unknown 值。

（“上面说的三点”指上一条消息里的：①不把 RDX 当参数；②把 RDX 当返回值；③把 RDX 当返回值的同时把入口 RDX 当参数。）

## 1. 背景与现状

产物里目前有两条“值丢了”的出口，都不该存在：

**（a）裸寄存器读**（`load iN, ptr @REG` / `notdec.partial_read.*(ptr @REG, off)`）。寄存器 global 在产物里全是无初值 `external global`（`lib/RegisterStorage.cpp:158`），没有任何定义或 store，读到的值没有来源。HEAD（`/tmp/fix29`，10 目标）：

| 目标 | 整寄存器 entry load | partial_read（其中 range_entry） | unknown |
|---|---|---|---|
| lighttpd | 54 | 76（71） | 26 |
| redis-cli | 45 | 97（94） | 7 |
| libuv | 6 | 17（14） | 8 |
| ffmpeg | 1 | 6（4） | 5 |
| vsftpd | 1 | 5（4） | 0 |
| memcached | 10 | 13（9） | 0 |
| wrk | 0 | 0（0） | 0 |
| tmux | 473 | 272（267） | 25 |
| ssh | 16 | 81（75） | 65 |
| wolfssl | 138 | 1319（1311） | 556 |
| **合计** | **744** | **1886（1849）** | **692** |

**（b）unknown**。10 目标总共 692 个，只有六个 reason，逐个查证都是**流水线自己没把值接上**，没有一个是“机器上真的没有来源”：

| reason | 个数 | 主要目标 | 含义 | 判定 |
|---|---|---|---|---|
| `call_effect_mixed` | 534 | wolfssl 478 / ssh 48 / ffmpeg 4 / tmux 4 | 调用点需要某寄存器的值，而 callee 的出口 provenance 是 Mixed，调用点直接“判丢” | 缺口 |
| `call_arg_rewrite_missing_binding` | 61 | wolfssl 43 / tmux 14 / ssh 4 | 实参改写只认 store 绑定，找不到就当 unknown | 缺口 |
| `return_binding_uses_clobber_value` | 52 | wolfssl 14 / ssh 11 / lighttpd 10 / libuv 8 | 返回绑定用了 clobber 占位（值是“调用后寄存器”占位，不是真值） | 缺口 |
| `entry_range_rejected` | 39 | lighttpd 16 / wolfssl 16 / tmux 6 / ssh 1 | 按位 SSA 走到函数入口要入口值，却被“出口级”的 MayEntry 闸门拒掉 | 缺口（判据粒度错） |
| `range_return_rewrite_missing_value` | 5 | wolfssl 4 / ssh 1 | 返回重写时找不到要返回的值 | 缺口 |
| `foreign_call_argument_replacement` | 1 | wolfssl 1 | 给未建模调用传参时造 unknown | 缺口 |

**（c）当前“三个选项”的落地状态**：pass 是混着的——返回槽按 `MayNonEntry && ExitDemand` 建（`NativeRegisterSummarySSA.cpp:1806`、出口活跃性 `:5616`），入口形参按 `ReadEntryMask` 建（`shapeForInternalFunction` `:1676` / `addParamForUnit` `:1713`）。两者不闭合的地方就掉进裸读或 unknown。这正是“混合的错误情况”。

## 2. 目标：把“值的来源”变成不变量

- **I1**：接口可改的内部函数（内部链接、地址不逃逸、调用点由本 pass 改写）的产物里，**寄存器 global 的读 = 0**。
- **I2**：**建模缺口型 unknown = 0**，即 §1(b) 表里六个 reason 全部归零。
- **I3**：只允许两类“外部来源”的值，且必须显式标注、单独计数、不进 unknown 指标：
  - **边界机器状态**：程序入口 / 导出函数 / 地址逃逸函数的入口寄存器（`!notdec.register.boundary`）；
  - **不透明 callee 的 clobber**：被未知外部或未知间接 callee 改写的寄存器（`!notdec.register.opaque_clobber`）。
  并且要用手段把这两类压到最小（见阶段 4），每一条都要能说出“具体是哪个外部来源”。
- **I4（闭合性）**：对每个函数，
  - 对外要的出口位（`ExitDemand ∩ MayNonEntry`）在入口需要的位（`∩ MayEntry`）必须出现在形参里；
  - 需要从 callee 拿的出口位必须出现在该 callee 的返回槽里。
  两条任一不满足，就是 I1/I3 的违规点，不允许再出现第三种出口。

判断标准：产物里不再出现“裸读”，unknown 计数只剩 I3 的两类（且带标注），签名只增不减（或有据可查）。

## 3. 值的来源分类（规划的骨架）

| 来源 | 机器上是什么 | 产物里应该怎么表示 |
|---|---|---|
| ABI / 边界入口 | 程序或外部调用者放进寄存器的值 | 入口形参（接口可改时）；否则 **边界读**（I3，带标注） |
| 指令写 | p-code 里有明确定义 | 按位 SSA 值（现状已正确） |
| 内部 callee 的出口 | callee 的按位 SSA / 形参链算出来的值 | 返回槽（聚合返回）+ 该 callee 需要的入口形参（I4 闭合） |
| 不透明 callee 的出口 | 外部实现留下的值 | **opaque clobber 标注**（I3），并优先通过 promotion / external 事实消掉 |
| 环境（标志位、段寄存器、计数器……） | 硬件 / 环境 | 专门的 helper + 标注（DF、ST0 现在就是这么做的） |

**没有“unknown”这一格。** 任意值只在“不透明 callee 的出口”和“边界机器状态”里出现，而这两格都要带来源标注。

## 4. 六个缺口的根因与修法

1. **`call_effect_mixed`（534）**
   根因：调用点分派对 Mixed（`MayEntry ∧ MayNonEntry`）只有“判丢”一条路（`unknownCallEffectCause` / `callRangeValue` 的 Mixed 分支）。
   修法：**按位合并**。callee 的返回槽覆盖的位取 callee 的返回；`~MayNonEntryMask` 的位（callee 任何路径都没写过）取调用者自己的调用前值——这个值调用者的按位 SSA 一定有。分界线用 callee summary 的 `MayNonEntryMask`（`callEffect` `:6749` 现在已经在读 callee facts，成本可控）。
2. **`call_arg_rewrite_missing_binding`（61）**
   根因：实参改写（`:8886-8902`）只认 `bindingForIndex` 给的 store 绑定，绑定缺失就造 unknown。
   修法：绑定缺失时取**调用点该寄存器的按位 SSA 值**（和 `entryRangeInput` 同一套 range 机制）；只有当该值在模块内确实无定义（走不到入口、也走不到任何写）时，才轮到 I3 的外部来源。
3. **`return_binding_uses_clobber_value`（52）**
   根因：返回绑定（`:7740-7780`）在缺少真值时用 clobber 占位顶上。
   修法：返回绑定优先取“按位 SSA 值 / 形参链 / callee 返回槽”，clobber（`notdec.register.summary_clobber`）只允许出现在不透明 callee 处，并且要走 I3 标注；这条和 I4 的闭合检查是同一件事的两面。
4. **`entry_range_rejected`（39）**
   根因：`rangeMayComeFromEntry`（`:6597`）拿**出口级**的 `MayEntryMask` 去回答“这个 range 在函数入口是不是入口值”这个**点**问题，粒度不对。
   修法：按位 SSA 的 range 走查本身已经知道“这个 range 在当前路径上有没有被写过、是否一路走到函数入口”——走到入口就说明它的来源就是入口值（入口值本来就是合法来源）。把这道闸门改成点敏感（或直接让走查结果说话），它现在只会把正确结果拒掉、把值推给裸读/unknown。
5. **`range_return_rewrite_missing_value`（5）**
   根因：返回重写（`:7620-7630`）找不到值。
   修法：先按 I4 做闭合检查（返回槽该有的位必须在），再重写；缺的位置应该由阶段 2/3 的合并补上，不允许再造 unknown。
6. **`foreign_call_argument_replacement`（1）**
   根因：给未建模调用（foreign）传参时造 unknown（`:8140`）。
   修法：参数值同样取调用者 SSA 值 / I3 外部来源；foreign 调用只影响“callee 的出口”，不影响“调用者已算出的实参”。

## 5. 阶段、验收与顺序

- **阶段 0（度量尺子，必须先做）**
  加“寄存器访问审计”：扫产物（或 pass 末尾自查），按函数分类统计 裸读 / 边界读 / opaque clobber / unknown（按 reason）。
  验收：任意目标上能输出完整清单；§1 的基线数字可复现；清单进 summary 输出，并配一个单测。
- **阶段 1（入口侧闭合）**
  `entry_range_rejected` 归零；入口形参规则补齐（保留“中段 mask 不建形参”“浮点/变参先跳过”这些既有取舍，但每一类都要在审计里作为**已知边界**列出来，且不再掉进裸读）。
  上一轮的 `/tmp/fix-entry-param.patch` 是这一步的一部分。
- **阶段 2（调用点侧闭合）**
  `call_arg_rewrite_missing_binding` + `call_effect_mixed` 归零（按位合并）。
- **阶段 3（返回侧闭合）**
  `return_binding_uses_clobber_value` + `range_return_rewrite_missing_value` + `foreign_call_argument_replacement` 归零。
- **阶段 4（外部来源最小化）**
  间接调用 promotion（lighttpd 日志现在是 `seen=117 promoted=0 slots=9 escaped=4`，一个都没提升）+ 已知外部函数按 ABI 事实；剩下的边界读 / opaque clobber 单独计数。
  验收：每一类都能逐条对上“哪个外部来源”。
- **每阶段验收**：10 目标 `llvm-as` + `opt -passes=verify` OK；`ctest` 12/12；审计里该阶段负责的类 = 0；unknown 总数不高于上一阶段；签名只增不减（或有据可查）。
- **终结标准**：I1/I2 达成；I3 两类计数给出并与基线对比；六个 reason 在审计输出里消失。

## 6. 风险、成本、不做的事

- 不做：把裸读换个名字（变 unknown / 变 clobber / 变 `machine_state`）当作修好；不加新的兜底 fallback；不为指标好看而收紧/放宽判据；不动导出函数 ABI 和地址逃逸函数；不放宽“中段 mask/浮点/变参”的既有取舍而不给量化。
- 风险：阶段 2 的按位合并依赖调用点能读到 callee 的 `MayNonEntryMask`（现在 `callEffect` 已读，风险低）；阶段 4 的 promotion 有多目标/逃逸风险，必须保守（宁可留 opaque clobber 也不能猜）。
- 理解成本：把“值来自哪里”写成一张表 + 四条不变量，比现在散落的 fallback 好维护；代价是调用点/返回点要显式做按位合并，代码会多一点。
- 维护成本：审计脚本和 summary 计数要跟着判据一起改（规则变化时同步）。

## 7. 与既有工作的关系

- `logs/20260927-01-native-return-entry-param-plan.md`（入口形参实验，未落地）是**阶段 1 的一种手段**，不是全部；它的量测数据（unknown 会先涨）正好说明为什么必须先做阶段 0 的审计、再做阶段 1。
- 交接文档 `logs/20260923-01-native-handover.md` §6 的“不要重试”清单继续有效。
- 本文只写规划，没有改产品代码；工作区干净（HEAD `c07aa7d` + 本文档提交）。
