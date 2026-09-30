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
- 本文档记录规划与后续实现记录（阶段 0 已落地，见 §8）。
## 8. 阶段 0 实现记录（已落地）

- `include/notdec-bin2llvm/passes/summary/NativeRegisterSummarySSA.h`：新增
  `NativeRegisterValueSourceAudit` 结构与 `auditNativeRegisterValueSources()` 声明。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp`：实现放匿名 namespace
  （`registerValueSourceIsControlled` = 本地链接且函数地址没被取；
  `pointerIsRegisterGlobal`；`auditRegisterValueSourcesImpl`），公开 wrapper 在匿名
  namespace 之后。分类：`load iN, ptr @REG` 与 `notdec.partial_read.*(@REG, …)` 按
  “接口可改 / 边界”分别计数；`notdec.partial_write.*`、`notdec.register.summary_clobber.*`、
  `notdec.unknown.*`（按 `!notdec.unknown.source` 的 `reason`）各一列。
- `tools/notdec-native-llvm.cpp`：`reportRemainingUnknownValues` 里在
  `notdec.unknown remaining` 之后打印 `Native register value sources: …`（只在
  `--register-ssa-summary` 下打印，默认路径 stderr 仍为空）。
- `tests/native_register_summary_ssa_test.cpp`：
  `testRegisterValueSourceAuditClassifiesReads`（内部读→local、外部可见读→boundary、
  unknown 按 reason 归类）。

命令：

```bash
make -C build -j16 notdec-native-llvm native_register_summary_ssa_test
./build/bin/native_register_summary_ssa_test            # exit 0
build/bin/notdec-native-llvm <target> --all-confirmed --skip-runtime --register-ssa-summary -o out.ll
```

基线（**最终产物**、审计口径）：

| 目标 | local_register_reads | boundary_register_reads | clobber_placeholders | unknowns |
|---|---|---|---|---|
| vsftpd | 7 | 6 | 23 | 0 |
| lighttpd | 120 | 6 | 519 | 26（`entry_range_rejected` 16、`return_binding_uses_clobber_value` 10） |
| tmux | 308 | 473 | 6856 | 25（`call_arg_rewrite_missing_binding` 14、`entry_range_rejected` 6、`call_effect_mixed` 4、`return_binding` 1） |

注意：§1 表里用脚本数的 `partial_read` 把 helper 的 `declare` 也算进去了，实际以本表为准
（lighttpd 旧口径 76 里有 9 条是声明）。

## 9. 阶段 1：现状、修正后的工作项

### 9.1 入口形参（原 1c）已实现并量测：单独落地不达标

patch：`/tmp/fix-entry-param.patch`（收窄版：跳过浮点返回寄存器与变参函数）。

| 目标 | local_register_reads | unknowns |
|---|---|---|
| vsftpd | 7 → 9 | 0 → 0 |
| lighttpd | 120 → **78** | 26 → **84**（`call_effect_mixed` +47、`entry_range_rejected` +6、`call_arg_rewrite_missing_binding` +5） |
| tmux | 308 → 291 | 25 → 30 |

1c 只覆盖“返回槽需要的入口位”，裸读主体（填充槽、边界、跳过的那几类）还在；
涨的 unknown 全部落在阶段 2/3 负责的类里。按 §5 判据**先不落地**，只留 patch。

### 9.2 `entry_range_rejected` 的成因分类（本轮查清）

抽查 lighttpd 16 个（FUN_f108 RAX×3、FUN_f147 RAX×3、FUN_f8e6 RCX×2、FUN_263db RDX×2、
FUN_3acd0 RAX×2、FUN_e78d RCX、FUN_23a73 RAX、FUN_23c64 R10、buffer_copy_string_len_lc RAX）
与 wolfssl/tmux 的同名项，分两类：

1. **编译器填充/保存区（junk）**。例 `FUN_f108`（0xf108）：汇编是
   `push rax`（对齐 8 字节）→ `call` → `pop r9`（R9 是后随变参调用的寄存器保存槽），
   源码对应 connections.c 的 `log_pri(...)`。summary 的 `collectStackAdjustmentSlots`
   已判定它是“栈对齐 push，值 undef”（`NativeRegisterSummary.cpp:2076`），所以
   `read_entry` 里没有 RAX；但按位 SSA 仍把 `push` 的**值**（入口 RAX）物化出来，
   出口级 `MayEntryMask` 闸门再拒掉 → 3 个 unknown。同型：FUN_f147、FUN_f8e6、
   FUN_e78d（`push rcx … pop rdx`）、FUN_23c64（`push r10`）。
   **修法**：在“已判定为填充/保存区”的槽位 store 处直接写 poison（IR 侧的
   `getUndefRegState`），不给它找值 —— 裸读和 unknown 一起消失。
2. **真入口值**：请求发生在函数入口、这些位确实还没被写过，只是出口级 `MayEntryMask`
   为 0（函数在别的路径上覆盖过）。点查询用了出口级 mask。
   **修法**：按位 SSA 走到函数入口就说明来源就是入口值，闸门只留诊断（warning 类），
   值走形参 / 边界读。

### 9.3 修正后的阶段 1 工作项

- **1a**：填充/保存区槽位 poison 化（覆盖第 1 类，预计消掉 lighttpd 16 里的大半）。
- **1b**：入口闸门改点敏感（只告警不拒），第 2 类走形参/边界读。
- **1c**：入口形参补齐（patch 已有，等 1a/1b 一起量）。

## 10. 决策与阶段 2/3 的切入点

**用户决策（填充槽 / 栈对齐 push 的值）**：不要用 poison —— 后续优化对 poison 的行为可能超出
预期；这类"编译器自己都当 undef 用"的槽位，值用 `notdec.unknown` 是对的；如果消不掉，后面
再设计**专门的优化 Pass** 来消除（例如只喂给死槽/变参保存槽的 unknown）。

对本规划的修改：

- 阶段 1a 从"poison 化"改为"**保持 unknown + 归因到填充槽**"：现状（lighttpd 16 个）已经是
  unknown，但 reason 是 `entry_range_rejected`；应改成 `padding_slot_value` 这类一眼能看出
  "这是编译器填充值"的 reason，并把"消除"拆成后续独立 Pass。
- 阶段 1b（入口闸门点敏感）只对**真入口值**那一类生效；填充槽那类继续走 unknown。

**阶段 2/3 代码切入点（本轮已定位）**：

| 目标 reason | 位置 | 现状与修法 |
|---|---|---|
| `call_effect_mixed`（534） | 分派 `NativeRegisterSummarySSA.cpp:6167-6216`；返回槽条件 `:1806` 一带 | Mixed 时若 `signatureReturnUsesUnit` 为真已走 `callRangeValue`（正确）；为假才 `markBlockRangeUnknown`。根因是 **callee 的返回槽没覆盖该寄存器**（`ExitDemand` 没算上这个调用点的需求）。修法：(i) demand 侧让 callers 的需求进 `ExitDemand`；(ii) 分派侧第一遍对 Mixed 生成可替换占位，交给 cleanup 按签名解析（cleanup 的 `callEffect` 已经分 ReturnValue/Preserve/Clobber）。两条都做才闭合：只做 (ii) 会退化成 clobber 占位（ABI 上不算错，但不是真值） |
| `call_arg_rewrite_missing_binding`（61） | `:8943` 一带 | 绑定缺失时先试若干 fallback，最后造 unknown；要补"取调用点该寄存器按位 SSA 值" |
| `return_binding_uses_clobber_value`（52） | `:7825` 一带 | 返回绑定直接用 clobber 占位；改成优先取按位 SSA 值 / 形参链 |
| `range_return_rewrite_missing_value`（5） | `:7693` | 返回重写找不到值，靠上面的闭合修 |
| `foreign_call_argument_replacement`（1） | `:8200` 一带 | 传参取调用者 SSA 值 / I3 外部来源 |

**建议顺序**：先返回侧（`return_binding_uses_clobber_value` + `range_return_rewrite_missing_value`，
局部、可单测），再 `call_effect_mixed` 的 (ii)+(i)，最后 `call_arg_rewrite_missing_binding`；
每步按 §5 判据量测（审计行 + unknown 直方图 + verify + ctest）。


## 11. 返回侧试改记录：折叠是**有意的**，得从 demand 侧修

按 §10 的顺序先动返回侧：把 `collectFunctionReturnValues`（`NativeRegisterSummarySSA.cpp`
`:7812` 一带）里"出口值全是 clobber 占位 → 折叠成 `return_binding_uses_clobber_value`
unknown"的 `else` 分支删掉，让占位留给重写/清理遍按签名解析。

结果：单测三处失败 ——

- `unknown external RDX value was not materialized as unknown`
- `internal return exposed external clobber helper`
- `sqrtl left a register SSA warning`

说明这个折叠是**有意的设计**：内部函数的出口值里不允许暴露 clobber helper 占位，出口处宁可
给显式 unknown。已回退（工作区干净，单测恢复通过）。

结论（修正 §10 的顺序）：**返回侧的 52 个 unknown 不能就地修**，得先修 demand 侧 ——
只要 callee 的返回槽覆盖了调用者需要的寄存器，clobber 图在到达 `ret` 之前就已经被解析成
真实返回值，折叠根本不会触发。所以顺序改为：

1. `call_effect_mixed` 的 (i) demand 侧：让 `analyzeCallerDemand` / `ExitDemand` 把
   "调用点之后仍被读"的 Mixed 寄存器算进去（callee 的 `shape.Returns` 因此覆盖它）；
2. 再看 (ii) 分派侧是否还需要"第一遍可替换占位"；
3. 返回侧与 `call_arg_rewrite_missing_binding` 顺带消失或大幅下降。


## 12. 阶段 2 (i) 第一步：partial_read 的 demand 位偏移（已修，量测中性）

**改动**：`NativeRegisterSummary.cpp` 的 `liveBeforeBlock` 里，partial_read 分支原来把
`valueDemand(call, valueDemands)`（读结果坐标，从 bit 0 起）直接 `addDemand` 到寄存器上，
没有按 `partial->BitOffset` 移位。改成先 `zext+shl(BitOffset)` 再 add。理由：读 `DH`
（offset=8）的需求会记到 DL 上，调用点之后真正被读的位进不了 callee 的 ExitDemand。

**量测**（lighttpd 97s、libuv 49s，与基线逐项对比）：

| 目标 | local | boundary | clobber | unknowns |
|---|---|---|---|---|
| lighttpd | 120（不变） | 6（不变） | 519（不变） | 26（不变：entry_range_rejected 16、return_binding 10） |
| libuv | 4 | 18 | 8 | 8（不变） |

单测 `native_register_summary_ssa_test` / `native_register_summary_test` 通过。

**结论**：这个移位是**对的**（坐标语义），但**不是** 534 个 `call_effect_mixed` 的成因
（很多消费方按 mask 非零判断，位偏移不影响"有没有需求"）。下一步要查的是需求链上的另外两处：

1. `applyBackwardCallDemand` 只在 `Effects.count(callee) != 0` 时把 `MayNonEntry & mask`
   加进 callee 的 ExitDemand；effect 没算出来的 callee 会走到"不透明调用"分支，需求被擦掉
   而不是传下去（`NativeRegisterSummary.cpp:3025/3053`）。
2. 返回槽/`signatureReturnUsesUnit` 的**range 覆盖**：slot 类型由 `ExitDemandMask` 经
   `integerSlotForSingleDemandRange` 决定，若 mask 落在非连续区间会退回整寄存器，而调用点
   需要的 range 可能仍不在覆盖范围内。


## 13. "禁止裸读→没载体就给 unknown" 试改失败：入口读是**真来源**

**试改**：在 `entryRangeInput` 末尾，对"接口可改"的函数（本地链接且地址没被取）直接删掉
裸读兜底，改成 `entry_value_without_carrier` 的 unknown（边界函数保留裸读）。

**结果**：单测两处失败 ——

- `dead frame spill created an unknown vector value`
- `callee-saved register after an internal call became unknown`

**含义（关键）**：入口读（`load @REG` / `partial_read(@REG,…)`）**不是假值**，它承载的是
"这个函数自己的入口值"——一个**真实来源**（被调用者/机器状态确实在入口放了值）。把它换成
unknown 等于**把已经知道的值扔掉**，比裸读更糟。所以：

- 出口 4（裸读）的正解是**出口 3：形参**，不是 unknown；
- "全化"必须**两边一起做**，否则只会把问题从一个地方挪到另一个地方：
  - **callee 侧**：入口需要的 range 全部由形参承载（现在的判据只覆盖"返回槽需要的那部分位"）；
  - **caller 侧**：调用点要能给出这个形参的值（先取调用者自己的按位 SSA 值；现在只认 store 绑定）。
  只做 callee 侧就是 §9.1 量到的现象：callee 的裸读少了，调用点冒出一堆 unknown。

已回退（测试恢复通过，工作区干净）。

**修正后的下一步（一个组合改动）**：

1. callee 侧：入口形参判据从 `ReadEntryMask | returnEntryMask` 扩到"按位 SSA 在入口要过的
   所有 range"（出口 3 成为默认载体）；
2. caller 侧：`call_arg_rewrite_missing_binding` 与调用点分派改成"先取调用者自己的按位
   SSA 值"，确实没有才落到 unknown / 边界读；
3. 两边一起量：`local_register_reads` 应显著下降，且 unknown **不净增**（原来由裸读承载的
   值改由形参承载）；
4. 之后才是策略：出口 1（剪掉调用者并不需要的返回槽）vs 出口 2/3。


## 14. 优先级确认（用户）：形参 > 裸读 > unknown；两边一起改的依赖顺序

用户决策：**裸寄存器读仍然比 unknown 更优先**（已知来源 > 未知）。所以优先级是

`形参（出口③） > 裸读（出口④，机器状态/入口值） > unknown（出口⑤）`

这条直接决定了"两边一起改"的**依赖顺序**（否则会先制造 unknown）：

1. **先做 caller 侧（使能半边）**：调用点必须能给出形参值。现在 `call_arg_rewrite_missing_binding`
   只认 store 绑定，而改写阶段（`rewriteSignatureShapes`）已经拿不到按位 SSA 状态，所以做法是
   **在 FunctionBuilder 构建期把"该调用点每个寄存器 range 的 SSA 值"记录下来**（沿用已有的
   recorded-value 机制：`CallArgRecordedValues/…ByFunction`），改写阶段缺绑定时直接取它；
   取不到（确实无定义）才允许落到裸读/边界读，最后才是 unknown。
2. **再做 callee 侧（扩容半边）**：入口形参判据从 `ReadEntryMask | returnEntryMask` 扩到
   "按位 SSA 在入口要过的 range"（候选：`ReadEntryMask | returnEntryMask | (EntryDemandMask & MayEntryMask)`；
   注意 `EntryDemandMask` 含写保留位，注释里明确说会撑大参数，所以必须量测）。
3. **顺序反了会先涨 unknown**：只扩 callee 侧（§9.1 已量到 lighttpd 26 → 84），因为调用点还不知道
   该给什么。caller 侧先做好，扩容才不会引入新 unknown。

填充槽（`read_entry=false` 那类）不受这两步影响，继续按 §10 保持 unknown + 后续专门 Pass 消除。


## 15. 结论（用户确认）：签名改写要能查"值表"，不能只认 store 绑定

诊断已确认：问题在**改写签名阶段只能看 store 绑定**，而机器传寄存器参数根本不需要 store。
修法就是让改写阶段能查到构建期那张"某调用点某寄存器 range 的值"表。

**实现规格（下一轮照此执行）**

1. **记录（构建期）**：在 `SignatureRewriteState`（`NativeRegisterSummarySSA.cpp:345-351` 一带，
   现有 `CallArgRecordedValues / …ByFunction` 旁边）新增
   ```
   // 调用点每个参数槽在"调用前那一刻"的按位 SSA 值：改写阶段缺 store 绑定时用它，
   // 而不是造 unknown。键 = (call, param index)；值 = range SSA 值。
   std::map<llvm::CallBase *, std::map<unsigned, llvm::Value *>> CallArgRangeValues;
   ```
   填充点：FunctionBuilder 处理调用（`transferRangeBlockUntil` 的 call 分支，`:6167-6216`
   一带）时，对 callee shape 的每个 `NativeSignatureSlot`（含 `slot.Unit`/`OffsetBits`/`SizeBits`），
   用当前 `CurrentDef` 查该寄存器 range 的值（若跨 range 需按 `extractRangeValue` 拼装，
   和 `entryRangeArgument` 的做法一致），存进 `CallArgRangeValues[call][index]`。
2. **消费（改写期）**：`rewriteSignatureShapes` 的实参循环（`:8943` 前后，
   `bindingForIndex` 返回 nullptr 时）改为
   ①取 `CallArgRangeValues[call][index]` → `remapValue` + `localizeCallArgument` 后使用；
   ②再取调用者自己的 ABI 形参（若该槽寄存器就是调用者的入参寄存器）；
   ③都没有才落到现在的位置（裸读 / unknown）。
3. **验收**：审计里 `call_arg_rewrite_missing_binding` 显著下降；新增形参时 unknown **不净增**；
   10 目标 verify OK、ctest 12/12。
4. **然后**才做 §14 的第 2 步（扩 callee 入口形参）；填充槽维持 §10 的决定。

风险：跨 range 拼装（`extractRangeValue`）与 `localizeCallArgument` 的语义要保持和
`entryRangeArgument` 一致；`CurrentDef` 在调用点可能只有部分 range 的值，缺的部分要放弃
（不要临时造值）。

- 阶段 1 收尾判据（按 §10–§15 修订）：`entry_range_rejected` = 0；`local_register_reads` 明显下降，且剩余项
  逐条能说出“为什么是边界 / 为什么是 junk”。

## 16. 表示准则固定后的收口顺序（2026-09-28）

用户定了硬性准则，已写进 `AGENTS.md`（"寄存器消除路线"）和 `Arch2.md` 第 13 节：

- 优先级 **载体（形参/返回槽/实参） > 裸读 > unknown**；寄存器相关的未知值宁可保留裸读（信息量大）；
- 不用 poison/undef/freeze 当占位，一律 `notdec.unknown.*` + `!notdec.unknown.source`；
- 边界两侧的位要闭合。

按这个准则，§15 的"两处小改动"要重新排序并补全：

1. **caller 侧（已落地并量测，提交前保留）**
   - 改动 1：`SignatureRewriteState::CallArgRangeValues` 逐槽记录调用点 range 值，改写期缺绑定时按
     index 查（用户要求保留，不按收益衡量）；
   - 改动 2：`callerEntryArgumentForSlot`——槽就是调用者自己的入参寄存器时直接传调用者形参；
   - 量测：`call_arg_rewrite_missing_binding` 57 → 50（wolfssl 43→38、tmux 14→12）；
     `local_register_reads`/`boundary_register_reads` 不变；5 目标 `llvm-as`+`opt -passes=verify` OK、
     `ctest` 12/12。唯一未归因项：wolfssl `clobber_placeholders` 2900 → 2684。
2. **caller 侧补第三档（按新准则）**：绑定、值表、调用者形参都答不上时**保留裸读**
   （`load @REG` / `notdec.partial_read.*`），不再直接造 unknown。落地后
   `call_arg_rewrite_missing_binding` 应归零，代价是 local 裸读上升——这是准则允许的中间状态。
3. **callee 侧闭合**：重放 §9.1 的 1c patch（入口 `paramMask` 并上
   `returnEntryMask = ExitDemandMask & MayNonEntryMask & MayEntryMask`，保留"跳过浮点返回寄存器与
   变参函数"的取舍）。验收：新增形参的位能在调用点答出来（missing_binding 不净增）、返回槽相关的
   裸读下降、`entry_range_rejected` 不涨。
4. **策略**：闭合之后才调"某个寄存器/某些位要不要进返回槽、要不要整条进形参"（§14 的三档变成逐位旋钮）。
5. **poison 清理**：x87 空槽入口读、`vararg_unknown` 占位降级、zero-demand 操作数三处逐个评估换 unknown。

例子（lighttpd `fdevent_fdnode_event_set` 的 RDX）见 `Arch2.md` 第 13 节：现状是"低半形参 + 整条返回 +
高半裸读"的混合态，闭合后是 `i64 %RDX.arg` + `i64` 返回、无裸读、无 unknown。

## 17. 按位闭合第一轮实现记录（2026-09-28）

按 §13/Arch2.md 第 13 节的判据落地并量测（命令：`notdec-native-llvm <target> --all-confirmed
--skip-runtime --register-ssa-summary`，5 目标 `llvm-as` + `opt -passes=verify` OK、`ctest` 12/12）。

**落地的东西**

1. **返回槽只覆盖"真写过 且 被需要"的位**：`shapeForInternalFunction` 返回槽循环改用
   `returnMask = ExitDemandMask & MayNonEntryMask`；交集为空就不建返回槽。
2. **逐 range 的"callee 从不写就保留调用者前值"**：新增 `calleeNeverWritesRange()`，
   调用分派按 range 判断（`MayNonEntryMask` 全 0 的位出口值就是入口值）。这是 (1) 的前提；
   整寄存器的 Mixed 布尔在这里被细化到每一位。
3. **caller 侧第三档**：`assembleRangeReadIfDominating / readSlotRangeBefore / readSlotValueBefore`
   增加 `entryFallback`，绑定收集在 range 状态答不上时退到入口读（裸读），不再直接落 unknown；
   入口闸门 `rangeMayComeFromEntry` 仍然生效（填充/junk 位不在此列）。
4. **`ReturnEntryMask` 显式标记**：summary 写 `return_entry_mask=0x…`
   （`ExitDemand ∩ MayEntry ∩ MayNonEntry`），SSA 侧解析进 `SummaryRegisterFact::ReturnEntryMask`。
5. **入口形参侧的消费（`paramMask |= ReturnEntryMask`）试过后回退**，patch 停在
   `/tmp/fix-return-entry-param.patch`（回退理由见下）。

**量测（基线 → 现在）**

| 目标 | local | boundary | clobber | unknowns | missing_binding |
|---|---|---|---|---|---|
| vsftpd | 7 → 7 | 6 → 6 | 23 → 23 | 0 → 0 | 0 → 0 |
| libuv | 4 → 4 | 18 → 18 | 8 → 8 | 8 → 8 | 0 → 0 |
| lighttpd | 120 → 120 | 6 → 6 | 519 → 519 | 26 → 26 | 0 → 0 |
| tmux | 308 → 310 | 473 → 473 | 6830 → 6867 | 25 → **22** | 14 → **11** |
| wolfssl | 648 → 648 | 903 → 906 | 2900 → 2849 | 556 → **535** | 43 → **22** |

missing_binding 合计 57 → 33；unknown 不净增（tmux −3、wolfssl −21），local 裸读基本不变
（tmux +2 来自第三档）。

**入口形参闭合单独打开的实测（回退理由）**：lighttpd 26 → **83**、libuv 8 → **11**、
tmux 25 → **27**、wolfssl 556 → **650**；同时裸读明显下降（lighttpd 120 → **78**、
tmux 308 → 293、libuv 4 → 1）。机制：新增形参/返回槽把"某个寄存器的值"变成调用点的需求，
而这个值在调用点被更早的一次 Mixed 调用丢掉了（那个 callee 的返回槽没覆盖它）→ 调用点冒
unknown。实例：lighttpd `h1_reqbody_read` 在 `chunkqueue_mark_written` 之后新增 4 条 RAX phi，
入边是 `call_effect_mixed` unknown；起因是 `chunkqueue_remove_empty_chunks` 等 4 个 callee 因
`ReturnEntryMask` 新增了 `%RAX.arg`，调用点必须提供 RAX。

**结论**：按位闭合方向正确（`fdevent_fdnode_event_set` 的 RDX 已从"半形参 + 整条返回 + 高半
裸读"变成 `i64 %RDX.arg` + i64 返回、无裸读），但**必须先闭合 demand 侧**
（§10 (i)：把"调用点之后 / 作为实参被读"的需求算进前一个 callee 的 ExitDemand），否则只是把
裸读换成调用点的 unknown。

**一处修正**：早先以为"从不写但被需要"的位（A 类）会建出多余返回槽——实际不会，返回槽还有
`MayNonEntry` 布尔闸门挡着（如 lighttpd `sys_setjmp_sigbus` 在基线和现在都是 `void`）。
返回槽收窄真正生效的是"部分写过"的寄存器（如 wolfssl 的 RAX 只有低字节写过）。

**下一步（按依赖顺序）**

1. demand 侧闭合（§10 (i)）：让"实参/调用后被读"的需求进前一个 callee 的 ExitDemand；
2. 再重放 `/tmp/fix-return-entry-param.patch`，验收"unknown 不净增 + 返回槽裸读下降"；
3. 栈槽跨块（tmux 7 + wolfssl 12）单独一条线。

## 18. demand 入口需求 + 入口形参闭合（2026-09-28 第二轮）

第一轮实测（§17）：单独打开入口形参闭合 → 裸读换成调用点 unknown。根因是 demand 没有把
"callee 入口需要的位"算成调用点的需求，调用点拿不到值。本轮先补 demand 输入，再打开闭合。

**改动**

1. `NativeRegisterSummary.cpp` 的 `applyBackwardCallDemand`（**已有机制**，不是新加的）在内部
   callee 分支补一段：callee 入口需要的位也要在调用点活着——
   `entryNeeds = callee.ReadEntry | (callee.MayEntry & live)`。
   - `ReadEntry`：callee 真会读入口值；
   - `MayEntry ∩ live`：调用点之后还需要这位，而 callee 可能原样交回，所以它必须从调用前就在。
   实参准备 store 会在更早的位置把这条需求擦掉（值走 value 链），所以真正受影响的是
   "没有 store、靠寄存器透传"的位：需求继续往上游传，直到真正的生产者（某次调用的返回槽）。
   已知外部 callee 的 `addInputDemand` 本来就在做同一件事，这里只是把内部 callee 补齐。
2. 重放 §17 停下的入口形参闭合 patch（`paramMask |= ReturnEntryMask`），
   `/tmp/fix-return-entry-param.patch`。

**量测（基线 → §17 的 after4 → 本轮 after5）**

| 目标 | local | boundary | clobber | unknowns | missing_binding |
|---|---|---|---|---|---|
| vsftpd | 7 | 6 | 23 | 0 → 0 → 0 | 0 |
| libuv | 4 | 18 | 8 | 8 → 8 → 8 | 0 |
| lighttpd | 120 → 120 → **117** | 6 | 519 → 519 → 523 | 26 → 26 → 26 | 0 |
| tmux | 308 → 310 → 312 | 473 | 6830 → 6867 → 6884 | 25 → 22 → 22 | 14 → 11 → 11 |
| wolfssl | 648 → 648 → **646** | 903 → 906 → 905 | 2900 → 2849 → **2632** | 556 → 535 → 535 | 43 → 22 → 22 |

- **unknown 直方图逐键相同**（wolfssl 94 个 key、lighttpd/tmux 各 6/9 个 key，值与 §17 完全一致），
  也就是说闭合没有再制造任何新的 unknown；裸读反而少了几条。
- 例子：`fdevent_fdnode_event_set` 现在是
  `define internal i64 @fdevent_fdnode_event_set(i64 %RDI.arg, i64 %RSI.arg, i64 %RDX.arg)`，
  函数体内的 `partial_read(@RDX, 32)` **消失**——就是 Arch2.md 第 13 节里的选项 3（前后一致）。
  调用点是真值：例如 `FUN_ea58` 传 `%0 = zext i32 %RSI.arg to i64`（机器上 EDX 写 32 位、
  高半按 x86-64 规则为 0），`connection_state_machine` 传 `%16` 并把返回的 RDX 高低半接着用。
- 验证：5 目标 `llvm-as` + `opt -passes=verify` OK、`ctest` 12/12、两个 summary 单测通过。

**demand 侧待办（下一轮，先量化后改）**

"非全位需求最后变成整寄存器需求"的候选点（都已定位，未量测）：

1. `valueDemand()`（`NativeRegisterSummary.cpp:2931`）：值不在 `valueDemands` 表里就返回
   **全位**需求——"没有信息"被当成"全都要"。
2. `computeValueDemands()` 的种子集合（`:678-725`）：只 seed ret 值、call 实参、**非寄存器**
   store 操作数、分支/switch/icmp 操作数、partial write 值；**load 结果不 seed，写寄存器 global
   的 store 操作数也不 seed**。于是 `load @R; store %v, @RDX; call f` 这条实参搬运链里，
   `%v` 不在表里 → `load @R` 的需求是全位，而 callee 真正需要的可能只有几位。
3. `liveBeforeBlock` 的 register store 分支（`:3029-3034`）：一条 store 擦掉**整个寄存器**的
   live 需求。窄写（非 x86-64 低 32 位那种零扩展写）本应只擦被写的位，否则上游返回槽会缺位
   （镜像问题：需求被低估）。
4. `seedAbiReturns`（`:2911`）：根函数按 ABI 输出整寄存器 seed——有意为之，不算 bug。
5. 不透明 callee 分支（`:3078-3087`）：`clobberMask = fullMask(unit)` 把整条 live 需求擦掉；
   已知外部靠 `addInputDemand` 按位补回，未知外部只能整条丢——边界取舍。

第 1、2 条正好解释 §17 里发现的"死 phi 网仍然产生整条 RDX 需求"：一个只用于寄存器间搬运、
最终没有真实消费者的 load 也会产生全位需求。修法方向：给"只喂给寄存器 store 的 load"按
callee 的入口宽度（`ReadEntryMask` / 输入槽）给需求，而不是全位；以及把"没有信息"从
"全位"改成"只保留调用点确实需要的位"。

## 19. demand 改成双向 worklist（2026-09-28 第三轮）

用户结论：这种结构问题是理论完整性问题，不做收益量化，直接实现——需求有两个方向，
不该硬套"自顶向下"，用单调 worklist 定点。

**改动（`lib/passes/summary/NativeRegisterSummary.cpp`）**

1. `runTopDownDemand`：版本号循环 → **worklist**。
   - caller → callee：调用点的 `additions[callee]` 进 callee 的 ExitDemand，变化则入队 callee；
   - callee → caller：callee 的 **ExitDemand** 变化 → 入队它的所有 caller（因为调用点规则要读
     它的 ExitDemand 取透传需求）——这就是那条反向需求边；
   - `inQueue` 去重；所有量只置位（`mergeDemand`/`addDemand` 只 OR），有限格 → 收敛；
     递归/互递归同样终止。
2. `applyBackwardCallDemand` 的入口需求改成**镜像签名判据**：
   `entryNeeds = ReadEntryMask | (MayEntryMask ∩ MayNonEntryMask ∩ callee.ExitDemandMask ∩ live)`，
   浮点返回寄存器和变参函数沿用 `shapeForInternalFunction` 的取舍。
   即"调用点必须提供的位" = callee 真读的入口位 + callee 出口透传、而调用点之后还要用的位。

**走过的一段弯路（记录备查）**：第一版直接用 callee 的 `EntryDemand` 当入口需求，实测是回归——

| 目标 | EntryDemand 版 | 镜像判据版（最终） |
|---|---|---|
| lighttpd unknowns / local | 35 / 163 | **26 / 117** |
| tmux unknowns / local | 76 / 815 | **22 / 312** |
| wolfssl unknowns / local / boundary | 552 / 1040 / 1859 | **535 / 646 / 905** |

原因：`EntryDemand` 是"入口 live-in"，含 partial write 的写保留位等垃圾位（
`shapeForInternalFunction` 的注释里解释过为什么不用它定形参宽度），而且它被假需求放大后
会沿整条调用链继续传播。镜像判据才是"调用点确实必须提供的位"。

**最终量测（基线 → 现在）**

| 目标 | local | boundary | clobber | unknowns | missing_binding |
|---|---|---|---|---|---|
| vsftpd | 7 | 6 | 23 | 0 | 0 |
| libuv | 4 | 18 | 8 | 8 | 0 |
| lighttpd | 120 → **117** | 6 | 519 → 523 | 26 | 0 |
| tmux | 308 → 312 | 473 | 6830 → 6854 | 25 → **22** | 14 → **11** |
| wolfssl | 648 → **646** | 903 → 905 | 2900 → **2690** | 556 → **535** | 43 → **22** |

数字与第二轮（§18）持平或略好，但结构上是完整的双向定点：callee 的 ExitDemand 变化会重算
caller，需求可以沿调用链一路往上传。`fdevent_fdnode_event_set` 仍是
`i64 %RDX.arg` + i64 返回、无 `partial_read`。5 目标 verify OK、`ctest` 12/12、两个 summary
单测通过。

**仍然存在的边界**：形参判据还是 `ReadEntry`（不含 ReturnEntryMask 之外传播上来的入口需求），
所以"只透传、自己从不读"的函数仍以入口裸读收尾（准则允许：裸读 > unknown）。要把它也变成
形参，需要让形参判据吃"传播上来的入口需求"，并且先把假需求（`valueDemand` 的 all-ones
fallback、种子集合漏寄存器 store 操作数，见 §18）收窄。

## 20. 试改记录："转交需求"进形参判据（2026-09-28 第四轮，已回退）

目标（§19 结尾留下的边界）：函数自己既不读也不返回某个寄存器、只是把入口值交给下一个
调用（转发/包装函数）时，也让这个寄存器成为它的入口形参，这样调用点不用裸读寄存器 global。

**实现**（已回退，工作区干净）

1. `FunctionDemand` 新增 `CallInputNeeds`：`liveBeforeBlock` 在每个调用点算出的入口需求
   （callee 的 ReadEntry + 出口透传位 + callee 自己的转交位）并集，最后和 `EntryDemand` 求交
   （函数自己定义过的位被滤掉）。
2. 调用点规则把 callee 的 `CallInputNeeds` 也算进调用点的 live（继续往上游传）。
3. worklist 增加一条失效源：callee 的 `CallInputNeeds` 变化 → 重算它的 caller。
4. 导出成 metadata `call_input_mask`，SSA 侧 `addParamForUnit` 并进 `paramMask`
   （`paramMask = ReadEntryMask | ReturnEntryMask | CallInputMask`，并放宽 `!ReadEntry` 早退）。

**量测（after8 → 本轮）**

| 目标 | local | boundary | clobber | unknowns | 其中 call_effect_mixed |
|---|---|---|---|---|---|
| vsftpd | 7 → 14 | 6 → 2 | 23 | 0 → 0 | 0 → 0 |
| libuv | 4 → 1 | 18 → 13 | 8 → 15 | 8 → 11 | — |
| lighttpd | 117 → **55** | 6 → 8 | 523 → 2239 | 26 → **1124** | 0 → **1036** |
| tmux | 312 → 398 | 473 → 437 | 6854 → 8296 | 22 → **74** | 4 → 36 |
| wolfssl | （未跑完，趋势同 lighttpd） | | | | |

裸读确实降了（lighttpd 117 → 55），但 unknown 爆炸，全部集中在 `call_effect_mixed`。

**原因（已定位）**：`CallInputNeeds` 的 "callee 真读的入口位" 用的是 `ReadEntryMask`，而它里面
含**只为了重建 partial write 才被读的位**。例：lighttpd `array_get_element_klen` 从
`(RDI, RSI, i32 RDX)` 变成 `(RDI, RSI, i64 RDX, RCX, R8)`，`buffer_extend` 多了 `RCX/R8`。
这些位并不是源码语义上的参数（多数来自 `mov cl, …` / `mov r8b, …` 这类半寄存器写之后的
读改写）。形参一加，所有调用点都得给这些位找值；需求再沿调用链继续往上传，于是整条链上
每个函数都多出 RCX/R8 形参、每个调用点都要值 → `call_effect_mixed` 泛滥。

**结论**：这一步本身是对的（它就是把 §13 的"载体 > 裸读"再往前推一格），但**必须先修需求
源头**——§18 列的第 1、2 条：

1. `valueDemand()` 对不在需求表里的值返回全位需求；
2. `computeValueDemands()` 不 seed 写寄存器 global 的 store 操作数，而 partial write 的
   `Value` 又被 `fullMaskForValue` 整条 seed。

把 partial write 造成的"读改写"需求从 `ReadEntryMask` / `EntryDemand` 里摘掉之后，
`CallInputMask` 才只剩真正的转发位，这一步再打开就不会级联。回退后状态仍是 §19 的
`bd3c54e`（unknown 不净增、fdevent 干净）。

**顺带记下这轮查到的边界分布**（after8 产物，按调用点统计"实参是入口读"的调用）：

| 目标 | 总数 | 变参调用 | 非变参 |
|---|---|---|---|
| tmux | 154 | 147（FUN_69630 / FUN_51790 / FUN_69fd0 这类内部变参函数） | 7 |
| lighttpd | 13 | 10（log_error / log_debug / log_pri） | 3（R13/R15/RBP 这类 callee-saved） |
| wolfssl | 2 | 0 | 2（callee-saved） |

也就是说当前剩余入口读主要是"变参寄存器保存区"和"callee-saved 寄存器"两类；真正属于
"普通参数寄存器、只转发"的很少，所以这一步的收益本来也不在数量上，而在结构闭合。

## 21. "非 ABI 返回槽（受控函数）"实现记录（2026-09-28 第五轮）

用户要求：统一一个"是否遵守 ABI"的判定，参数和返回保持一致；给一个开关（有些编译配置
所有函数都遵守 ABI）；直接完全实现，效果不好也不要急着回退，先讨论原因。

**实现（都在工作区）**

1. `NativeRegisterSummarySSAOptions::AbiForAllFunctions`（默认 false）+ CLI
   `--abi-for-all-functions`。
2. 判定 `abiConventionFor(function, abiForAllFunctions)`：
   - 声明 / 导出 / 地址被取 / 开关打开 → `External`（遵守 ABI）；
   - 本地链接 + 地址没被取 → `Internal`（接口可改，自定义约定）。
   参数侧和返回侧共用这一个判定。
3. 每个函数挂 `!notdec.abi !{!"conventions=external|internal"}` 标记。
4. 形状：
   - `External`：形参只允许 ABI 输入寄存器（`abi.Inputs`）；返回只允许
     `InternalReturnRegisters`；不做 `ReturnEntryMask` 闭包。
   - `Internal`：形参 = ABI 输入 ∪ 返回/unaffected ∪ 通用寄存器；返回 = ABI 输出 ∪
     unaffected ∪ **通用寄存器（caller-saved 也能当返回通道）**。参数和返回用同一个集合。
   - 只放"承载值"的通用寄存器（新增 `isValueCarryingRegisterName`）：标志位（OF 一度被
     当成返回寄存器）、段基址、x87 窗口、浮点向量都排除。
5. 审计 `auditNativeRegisterValueSources(module, abiForAllFunctions)` 用同一个判定分
   local/boundary。
6. 单测：`testInternalCalleeReturnsCallerSavedRegister`（内部函数 RCX Mixed + 调用后读
   → 签名带 RCX 返回槽、调用点拿到值、无 unknown；开关打开后不出现返回槽、
   标记变成 external）；两个原有单测改用外部/内部链接以匹配新语义。

**量测（基线 after8 → 默认模式 after11）**

| 目标 | local | boundary | clobber | unknowns |
|---|---|---|---|---|
| vsftpd | 7 → 7 | 6 → 6 | 23 → 23 | 0 → 0 |
| libuv | 4 → 4 | 18 → 18 | 8 → 8 | 8 → 8 |
| lighttpd | 117 → **215** | 6 → 19 | 523 → **876** | 26 → **54** |
| tmux | 312 → 343 | 473 → 473 | 6854 → 7543 | 22 → **27** |
| wolfssl | 646 → **1650** | 905 → 897 | 2690 → **11370** | 535 → **643** |

**对照：`--abi-for-all-functions`（所有函数都遵守 ABI）**

| 目标 | local | boundary | unknowns |
|---|---|---|---|
| lighttpd | 0 | 146 | **23** |
| tmux | 0 | 781 | 25 |

（打开开关时没有函数属于"接口可改"，所以 local 按定义是 0，寄存器读全部计 boundary。）

**结论与诊断（为什么变差）**

1. 调用点那一侧确实修好了：tmux 的 `call_effect_mixed` 4 → 0、
   `call_arg_rewrite_missing_binding` 11 → 7。
2. 但那些 unknown 只是**换了地方**：新的返回槽在 callee 的 `ret` 处组装出口值时，
   来源是"不透明调用的 clobber"，于是走 §11 的折叠 →
   `return_binding_uses_clobber_value` 从 10/1/14 涨到 38/14/182（lighttpd/tmux/wolfssl）。
   实例：lighttpd `fdevent_waitpid`（R8 的 `may_entry_mask=0`，出口 R8 只来自
   `call @waitpid`）→ `%R8.return_unknown69`。
3. 代价很大：callee 为了组装这些出口值，clobber 占位与局部读暴涨
   （wolfssl clobber 2690 → 11370、local 646 → 1650；返回结构体也变大）。
4. 底层动机仍然是假需求：partial write 造出来的位最早把 RCX/R8 卷进来，现在它们同时走
   形参和返回两条通道，等于给垃圾位修了两条路。

**待讨论的选项**

- **A（推荐先做）**：返回槽判据加一条"出口值必须有真实来源"：summary 侧区分"本函数自己
  写过"与"只被调用 clobber 过"（现在只有 MayNonEntry 一个 bool/mask，两者混在一起），
  只有 `需求 ∩ (本地写过 ∪ 可能还是入口值)` 非空才建返回槽。这样 §2 那类"出口只来自
  `waitpid`"的寄存器不会再被当成返回通道。
- **B**：按新准则重新设计 §11 的折叠——clobber 是"不透明 callee 的出口"（I3 里的已知来源），
  不该算建模缺口 unknown。上次删折叠挂了 3 个单测，需要连同"内部函数出口不暴露 clobber
  helper"这条一起设计（例如给返回槽的 clobber 占位挂 `!notdec.register.opaque_clobber`
  并按 I3 单独计数）。
- **C**：等假需求收窄（partial write 那两条）之后重新量同一组数字——现在 RCX/R8 被卷进来
  的动机大半是假的，修完可能默认模式自然变好。
- **D**：临时把 `AbiForAllFunctions` 默认打开（等价于"不做自定义约定"），把机制留着备用。
  对照数据里打开开关的 unknown 反而比基线低（lighttpd 23 vs 26）。







