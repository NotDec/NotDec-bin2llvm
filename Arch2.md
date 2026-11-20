# Summary 静态分析的思路

## 两遍自底向上分析

首先在 CallGraph SCC 上自底向上分析，收集每个寄存器按位的三类事实（与调用者无关）：

1. **ReadEntry**：入口值在被覆盖前被读过（参数候选）。入口值被读过说明很可能存在参数；反之即使原本有参数，函数内完全不用，也等价于无此参数。
   - 特殊情况：参数在原位置直接透传给其他函数，中间没有任何显式读写。
2. **MayNonEntry**：出口处可能已被本函数（或内部调用）改写过。对调用者而言，被改写过的位不能再假定保留原值（callee-saved 寄存器通过保存槽识别单独处理）。
3. **MayEntry**：出口处可能仍是入口值。若为 false，说明该位在调用后一定不再是原值。

外部导入函数严格按**调用约定（ABI）**建模被改动与被保留的寄存器：

- **内部函数（有函数体）**：参数和返回值都按"使用线索"判定。参数来自自底向上 `ReadEntry`；返回值需汇总所有 caller，看调用点之后用到了哪些被该函数改写过的位。
- **外部导入函数**：返回值同样可按调用点后续使用推断；参数则没有函数体，需按调用点的"定义线索"推断（见下节）。

第一遍自底向上分析时，未知外部函数按 clobber 处理（不假设其输入），这会漏掉"入口值直接透传给外部函数"的场景（例如 `int f(int x) { g(x); }` 中 `g` 的参数尚未确定时，`f` 无法推出对 `x` 的读取）。因此流程拆成两遍：第一遍收集调用点证据并推断外部函数参数与 vararg 尾参，第二遍带入最终外部签名重跑完整分析。内部调用在第一遍就能直接代入 callee 的 `ReadEntry`，因此不受影响。

## 未知外部参数与 vararg 处理

对每个"未知外部 / 已知 vararg 外部"调用点，基于第一遍稳定后的正向数据流状态，对每个候选寄存器/栈槽按来源分类：

- **Entry**：入口值透传（不算参数证据）
- **Local**：本函数显式写过（参数证据）
- **FloatLocal**：`Local` 中最后一次写为浮点指令的子集（区分 `movsd` 真浮点参数与 `movq` 等整数打包，避免误判假浮点参数）
- **CallClobber**：前一次调用弄脏的（不算）
- **Mixed**：多条路径合并后来源不唯一

## 需求传播（双向不动点）

汇总所有 caller 对 callee 的使用需求，反推每个函数的返回值和调用点活跃位：

- **根函数（顶层导出 / 无内部 caller）**：按 ABI 主输出寄存器（如 `RAX`）注入初始 `ExitDemand`。
- **ExitDemand（返回值判定的关键）**：函数退出时调用者还会使用的寄存器位。多个 caller 的需求取并集（部分调用点可能忽略返回值）。`shapeForInternalFunction` 要求 `MayNonEntry && ExitDemand`，返回槽按 `ExitDemandMask & MayNonEntryMask` 收窄。
- **函数内计算**：采用"寄存器级按位反向 liveness + SSA 值级位需求"两层不动点，把寄存器写的实际观察位反馈给值层，收窄宽读带来的虚假位需求。
- **跨函数传播**：需求在调用边界上是双向的（caller → callee 传 `ExitDemand`；callee → caller 传入口与返回透传需求），因此以单调 worklist 不动点迭代至收敛。
- **EntryDemand**：函数入口处活跃（live-in）的寄存器位。因可能包含部分写（读改写）的保留位，内部函数形参实际以 `ReadEntryMask`（以及返回槽透传位 `ReturnEntryMask`）为准，不直接使用 `EntryDemand`。

---

# 寄存器消除的架构演进（bin2llvm）

## 0. 背景与术语

反编译流程：机器码 → Ghidra Sleigh p-code → LLVM IR → `.ll` / `.bc`。

最早期，寄存器被直接表示为 **LLVM 全局变量**（`@RAX`、`@RSP` 等），指令读写寄存器即对全局变量的 `load`/`store`。这种表示有两个明显缺陷：

- 函数间通过全局内存隐式通信，看不出参数和返回值；
- IR 充满全局 `load`/`store`，阻碍后续优化与分析。

**寄存器消除**的目标是把寄存器读写还原为普通 SSA 值、函数参数和返回值，让 IR 恢复正常程序形态。

术语：

- **寄存器**：CPU 内部高速存储（`RAX`、`RSP`、`XMM0`、`ST0` 等）。
- **SSA / PHI**：每个变量只赋值一次的表示形式；多分支汇合处用 `PHI` 合并不同路径的值。
- **调用约定（ABI）**：规定参数/返回值寄存器、caller-saved（调用可能改写）与 callee-saved（被调用方必须保存/恢复）的规则。
- **clobber**：一次调用改写（弄脏）了某个寄存器。
- **部分读写**：只读写寄存器的一部分位段（如写 `EAX`、读 `AL` 或写 `XMM` 低 lane）。
- **栈指针 / 帧指针**：`RSP` 指向当前栈顶，`RBP` 常用于定位栈帧局部变量。
- **intrinsic**：编译器/分析器特殊识别的内建函数调用。
- **标志寄存器 / x87**：标志位记录运算状态（`ZF`/`CF`/`DF` 等）；x87 是由 `ST0`..`ST7` 组成的旧式浮点寄存器栈。

## 1. 第 0 代：起点——寄存器全是全局变量

```text
机器码 -> Sleigh p-code -> PcodeToLLVM
    寄存器 = 全局变量（@RAX、@RSP、@XMM0 ...）
    读写寄存器 = load / store 全局变量
```

提升层（`RegisterStorage`）将重叠寄存器（`RAX`/`EAX`/`AX`/`AL`）合并到同一个全局变量，用位切片处理子寄存器读写，解决了"如何表示到 LLVM IR"。

**问题**：函数边界隐藏在全局内存操作里，没有参数和返回值，后续分析无从下手。

## 2. 第 1 代：`NativeRegisterSSA`（整寄存器按需 SSA）

引入按需 SSA 构建（参考 *Simple and Efficient SSA Construction*），初步将全局寄存器的 `load`/`store` 转为显式 SSA 值与 `PHI`：

```text
机器码 -> p-code -> PcodeToLLVM
  -> NativeRegisterSSA
      读寄存器：沿 CFG 向前查找最近定义（找不到则视为入口候选值）
      多前驱汇合：插入 PHI
```

**暴露的问题**：

1. 仅支持整寄存器粒度，部分读写（`EAX`、`XMM` lane 等）无法进入 SSA；
2. 函数调用对寄存器的影响缺乏数据流建模；
3. `RSP`/`RBP` 与 callee-saved 保存恢复不能当作普通数据寄存器消除；
4. SSA 本身无法推断哪些寄存器是真实参数或返回值。

## 3. 第 2 代：`NativeRegisterSummary`（静态分析确定函数边界）

将顺序反过来：**先做模块级静态分析算出函数边界，再建 SSA**（静态分析细节见文首）：

```text
机器码 -> p-code -> PcodeToLLVM
  -> 前置栈帧/变参/canary 处理
  -> NativeRegisterSummary
      自底向上 SCC 不动点（ReadEntry / MayEntry / MayNonEntry）
      跨函数需求传播（ExitDemand）
  -> NativeRegisterElimination
```

**配套前置处理（栈帧与 callee-saved）**：

- `RSP` 承载栈分配、返回地址与栈参数，`RBP` 既可能是帧指针也可能是普通 callee-saved 寄存器，不能一刀切按普通寄存器消除。前置 pass `NativeStackFrameRewrite` 将可证明为本地栈帧的 `RSP` 访问转为 LLVM `alloca`（`RBP` 仅在证明来自 `RSP` 时替换）；`NativeStackCanaryCleanup` 清理 `__stack_chk_fail` 栈保护检查；`NativeVarArgRewrite` 将变参序言的寄存器保存区改写为 `llvm.va_start`。
- `NativeRegisterSummary` 在数据流中跟踪"入口 `RSP` + 常量偏移"的固定栈槽，识别 callee-saved 的 `push`/`pop` 保存恢复及栈对齐槽，防止机械保存动作被误判为读取入口参数。

**暴露的问题**：SSA 构建与函数签名重写强耦合，拆成独立 pass 会导致状态失配并重新暴露大量死 `store`。

## 4. 第 3 代：签名重写并入 `NativeRegisterElimination`

去掉独立的签名重写 pass，将 SSA 构建、签名重写、返回绑定与死代码清理统一编排在 `NativeRegisterElimination` 内，并确立**返回信息唯一由签名决定**的原则：

```text
NativeRegisterSummary
  -> NativeRegisterElimination
      1. 逐函数构建 SSA：调用点统一生成"调用后状态"占位，并收集
         "调用后 ABI 输出寄存器被实际读取"的证据（不在此阶段硬区分 return/clobber）
      2. 确定函数与调用点签名（shape.Returns 唯一决定返回槽）
      3. 重写函数类型、call 实参/返回值（ret 此时先建占位）
      4. post-rewrite cleanup 循环：
           首轮按最终值链收集返回绑定并填入 ret
           多轮 InstCombine + 删死 store，末尾再清残余栈帧/canary
```

关键设计：

- **解耦占位与返回判定**：构建 SSA 时不再提前猜测某个调用后寄存器是返回值还是 clobber，消除鸡生蛋问题；是否为返回槽唯一由签名 `shape.Returns` 决定（内部函数来自静态分析，已知外部函数来自原型，未知外部函数按保守规则推断）。
- **混合 clobber 路径不静默截断**：收集返回绑定时，若返回值的 PHI/计算链同时包含真实来源与 clobber/unknown 来源，保留整条值链与分支并记录 warning；仅当整条链全为 clobber/unknown 时才折叠为单个显式 `notdec.unknown`。

**暴露的问题**：整寄存器粒度仍消不掉部分读写；外部函数缺少原型，签名无法准确推断。

## 5. 第 4 代：按位区间的 range SSA

x86 指令大量只读写寄存器的一部分（写 `EAX` 清零 `RAX` 高 32 位、写 `AL`/`AH` 保留高位、写 `XMM` 低 lane 保留高位）。整寄存器 SSA 会拖入过宽的 PHI（甚至 512 位 `ZMM`），也无法跟踪部分覆盖。

方案将粒度从"整寄存器"细化为"寄存器的 bit 区间（range）"：

- **提升层**：部分读写统一展开为 `notdec.partial_read` / `notdec.partial_write` intrinsic；
- **分析与 SSA**：`NativeRegisterSummary` 的 effect/demand 和 `NativeRegisterElimination` 的 SSA 变量均按 bit 区间维护，`RAX[0:32]`、`RAX[32:32]`、`ZMM0[0:64]` 各自拥有独立的 SSA 值与 PHI，整寄存器访问化为区间的拆分与拼装；
- **前置规整与终态清理**：前置 `canonicalizeRegisterPointerPhiLoads` 将 InstCombine 生成的"PHI 指针的 load"规整回"load 的 PHI"；末尾由 `NativeRegisterFinalCleanup` 统一清理无用寄存器全局与 helper。

## 6. 第 5 代：外部函数签名推断、两遍 summary 与特殊寄存器

外部库函数在 IR 中只有 `declare`，既无函数体也无原型。为此将 summary 拆为两遍，中间插入外部签名推断：

```text
第一遍 NativeRegisterSummary（仅自底向上 effect + 收集未知外部/vararg 调用点证据）
  -> inferExternalCallShapes（外部参数与 vararg 尾参推断）
  -> 第二遍 NativeRegisterSummary（带入外部签名，跑完整 effect + 双向需求 + metadata）
```

- **外部参数推断**：依据调用点前的 `Local`/`FloatLocal`/`Entry`/`CallClobber`/`Mixed` 证据，只统计调用前被本函数显式写入（`Local`）的连续 ABI 参数前缀，排除 `CallClobber` 与已消费的临时值；浮点槽要求 `FloatLocal`（`float_write` 标记），避免把 `movq` 等整数打包误判为浮点参数；vararg 外部函数按各调用点独立推断尾参。
- **未知外部返回槽（保守规则）**：整型返回（`RAX`，排除 `RDX`）与浮点返回（`XMM0` 优先，有 x87 证据时选 `ST0`）互斥；旧 call 若已有非 void 返回类型则作为最高优先级证据，否则退回"调用后状态占位被实际读取（非死值）"的证据。
- **特殊寄存器处理**：
  - **DF 标志**：ABI 规定函数入口与调用返回后 `DF` 恒为 0。值侧在边界直接定为常量 0，需求侧跳过 `DF` 不记录需求（`isAbiBoundaryZeroRegister`），避免 `repz cmpsb`/`movs*` 等循环在调用图上扩散无意义的需求链。
  - **x87（`ST0`/`ST1` 窗口）**：p-code 提升层将 `ST0`/`ST1` 建模为 80 位寄存器全局并直接展开窗口内运算，深层栈 `ST2`..`ST7` 留在库内（仅保留 push/pop/peek/poke 四个衔接 intrinsic）。由此 `ST0`（SysV `long double` 返回）直接复用与 `RAX`/`XMM0` 相同的静态分析、需求传播与签名路径。

## 7. 当前完整流程

```text
ELF 机器码
  -> Sleigh p-code -> PcodeToLLVM
      部分读写展开为 partial_read / partial_write
      x87 ST0/ST1 窗口展开
      同块内重复寄存器读 CSE（写/调用/换块失效）
  -> 加载外部原型（内置表 + JSON 覆盖）& 截断已知 noreturn 调用
  -> 前置规整：
      NativeStackFrameRewrite（RSP/RBP -> alloca）
      NativeStackCanaryCleanup（删栈保护检查）
      canonicalizeRegisterPointerPhiLoads / PreSummaryPeephole / x87 窗口规整
      NativeVarArgRewrite（变参序言 -> llvm.va_start）
  -> 第一遍 NativeRegisterSummary
      自底向上 effect + 收集未知外部/vararg 调用点证据（不跑 demand）
  -> inferExternalCallShapes（外部参数 / vararg 尾参推断）
  -> 第二遍 NativeRegisterSummary
      自底向上 effect -> 双向需求不动点（含函数内寄存器/值两层定点）
      -> 可选寄存器感知 DCE -> 写入 metadata
  -> 构建初始签名形状（含间接调用输入推断与尾调用栈参数传播）
  -> 逐函数构建 range SSA（调用点统一占位 + 收集被读证据）
  -> 签名细化与重写
      间接调用/内部栈参数细化、补外部返回值、重写函数与 call 签名、折叠手写 va_arg
  -> post-rewrite cleanup 循环
      首轮按最终值链收集返回绑定填 ret；多轮 InstCombine + 删死 store；再清残余栈帧/canary
  -> NativeRegisterFinalCleanup（删无用全局/helper，统计残留）
  -> .ll / .bc
```

配套验证手段：

- `llvm-as` + `opt -passes=verify`：校验 IR 合法性；
- `native-register-residue-audit.py`：统计残留的寄存器 `load`/`store`；
- `--register-ssa-warning-out`：量化输出残留 helper、未知外部签名与未消除访问；
- fortune（i386/x86_64）、wrk、vsftpd、memcached、libuv 等真实二进制回归验证。

## 8. 寄存器值的表示准则

寄存器消除中，裸寄存器读（`load @REG` / `notdec.partial_read`）与 `notdec.unknown.*` 都应尽量减少；在尚未完全消除时，遵循三条硬性准则：

1. **值来源优先级：载体 > 裸读 > `notdec.unknown`**
   - **载体**（入口形参 / 返回槽 / 调用点实参）是正常数据流；
   - **裸读**保留了"哪个寄存器、哪几位、在何处读取"，后续阶段仍可将其提升为载体；
   - **`notdec.unknown.*`** 彻底丢失来源身份，只能依赖 `!notdec.unknown.source` 人工排查。
   - 因此未知的寄存器值宁可先退化为裸读，也不要过早落成 unknown；终态目标是两者都不出现（裸读仅允许留在接口不可改边界和不透明调用出口）。

2. **禁用 `poison` / `undef` / `freeze(poison)` 表示未知**
   - LLVM 允许将 `poison` 细化为任意具体值（如 `freeze(poison)` 会被 InstCombine 折叠为 0），会静默篡改语义并丢失归因。凡表示"寄存器值未知"一律使用 `notdec.unknown.*` + `!notdec.unknown.source`。

3. **边界两侧的位必须闭合（双向需求）**
   - **返回侧收窄**：返回槽只覆盖 `ExitDemand ∩ MayNonEntry`；callee 从未改写过的位直接由调用者调用前值保留，不塞入返回槽。
   - **入口侧闭合**：返回槽中"某些路径上仍为入口值"的位（`ReturnEntryMask = ExitDemand ∩ MayEntry ∩ MayNonEntry`）必须并入入口形参（`paramMask = ReadEntryMask | ReturnEntryMask`），否则函数体在未写路径上只能裸读寄存器（适用于接口可改的内部通用寄存器；浮点返回与变参函数暂不扩展）。
   - **调用点侧闭合**：若只扩入口形参而不要求调用点提供对应位，会把函数内裸读转嫁成调用点 unknown。因此在 `applyBackwardCallDemand` 中同步将 callee 入口需要的位（`ReadEntry | (ReturnEntryMask ∩ live)`）加回调用点的 live 需求；跨函数需求传播也随之成为**双向单调 worklist 不动点**（caller → callee 传 `ExitDemand`，callee → caller 传入口/透传需求），保证载体在调用边界两侧闭合。

## 9. 已知边界与押后项

- 未知外部返回推断仍依赖保守启发式：调用后同时读 `XMM0` 和 `ST0` 的 `long double` 外部函数可能误选 `XMM0`（更精确的返回侧后向数据流分析押后）。
- 外部 libm `long double` 原型（`sqrtl`/`round`）、`complex long double` 返回及多 `long double` 结构体返回押后。
- 栈参数绑定（将调用前压栈参数还原为 call 实参）仍在持续收口。
