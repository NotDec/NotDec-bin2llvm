# bin2llvm 工作规范

这套代码由你来负责，做什么、怎么搭都由你决定，以后也由你来继续维护。

- 已讨论一致且验证通过的修改默认直接提交。每次修改都检查性能；涉及类型恢复、结构体合并、pointer analysis 或 pipeline 时，按相同条件对比 fortune 用例。

### 技术细节

- 使用 `logs/` 记录修改日志和规划日志：plan 顶部保留用户原始 prompt，顶部列出计划，计划对应的后续的实现不要单开一个plan，在对应的计划plan后继续补充。
- 调试参数、Bench2 命令和产物位置见 `DEBUG.md`。
- IR 用 `/sn640/NotDec/llvm-22.1.0.obj` 下的 LLVM 22 工具验证，不能用系统 `llvm-as` 或 `opt`。
- 仓库目标是为真实二进制生成语义正确的 LLVM IR。能被 `llvm-as` 接受只是底线，真正正确与否需要手动检查IR语义是否与二进制对应的原始源码是否一致。
- Bench2是一个专门用来实验的文件夹。数据和产物位于 `/sn640/NotDec-Exp/Bench2`：`rootfs/`、`manifest/benchmark-targets.tsv`、`manifest/benchmark-needed.tsv`、`bin2llvm-ir/`。
- 需要对照 Ghidra的逻辑 时直接读 `/sn640/ghidra`。

## 模式匹配类 pass 的设计理念

native 侧凡是"识别编译器生成的模式"（变参序言/va_arg 展开、帧指针用法、栈 canary、
x87 窗口等）都遵循：

1. **先看编译器源码**，确定"编译器会生成什么"，再照着写匹配。例如变参序言和 va_arg
   展开直接对照 LLVM `lib/Target/X86/X86ISelLowering.cpp` 的 `LowerVASTART`/`LowerVAARG`；
   必要时用 LLVM 22 的 `clang`/`llc` 造 ground truth 对拍（C 例子 → 汇编、手写 `.ll` → 汇编），
   不凭印象猜形态。
2. **用最少的代码匹配这些固定模式**：机器码 ↔ IR 一一对应；不做通用数据流推断、不猜类型、
   不推广到编译器不会生成的形态。识别不到就是识别不到。
3. **要么完整命中、要么整体放弃**：匹配与改写整体成功或整体跳过（保守跳过），绝不"半改"
   ——半改会让寄存器/栈槽的推进和手工代码对不上。
4. **识别结果挂 metadata**（如 `!notdec.vararg`），后续签名/调用点推断只读 metadata，
   匹配逻辑与消费方解耦，改匹配不连带动签名推断。

native 计划必须说明已有 native 状态，列出 Ghidra 源码文件和关键函数，区分需要复刻的策略与保守暂缓的部分，再写阶段、判断标准、风险和不做什么。本格式只用于 native bin2llvm。

**动手做底层二进制操作的识别/还原（常量除法、取模、强度削减、变参、canary 等）之前，
先看 `docs/analysis/native-compiler-idiom-matching.md`**：它写了定位手法（覆盖率 fixture、
dump 目标 pass 那一刻的 IR、这一层常见的 helper/宽度往返噪声）、等价性证明的固定套路
（闭式 → 代入 x = d*q + r → 两次 APInt 检查）、落地与验证清单，以及现状/未做清单。
"最终 IR 看着该匹配却没匹配"几乎都是那一层的值图噪声，先 dump 再猜。

## 寄存器消除路线

当前默认且后续重点是 `summary`：

- 代码在 `include/notdec-bin2llvm/passes/summary/`、`lib/passes/summary/`。
- `NativeRegisterSummary` 与 `NativeRegisterSummarySSA` 负责寄存器 SSA、寄存器消除和 native 函数签名重写。
- `notdec-native-llvm` 默认且唯一使用该路；`--summary-register-ssa-pass` 仅保留为兼容别名。
- 新的寄存器消除、call/internal signature rewrite 都基于 summary 结果，不依赖 Ghidra `notdec.prototype.*` metadata。相关 plan/记录在 `logs/20260616-01-native-prototype-recovery-stage2/`。

**寄存器值的表示准则（硬性，2026-09-28 定）**

寄存器消除的产物里，"裸的寄存器读"（`load @REG` / `notdec.partial_read.*(@REG, off)`）和
`notdec.unknown.*` 都要尽量少；两者的取舍和表示方式固定为：

- **值的优先级：载体（入口形参 / 返回槽 / 调用点实参） > 裸寄存器读 > `notdec.unknown`。**
  寄存器相关的未知值宁可先落成裸读，也不要直接落成 unknown：裸读保留了"哪个寄存器、哪几位、
  在哪个位置"，后续阶段还能把它提成形参/实参；unknown 把这些信息全丢掉，只能靠人重新分析。
- **不要用 poison / undef / `freeze(poison)` 表示"寄存器值未知"**：LLVM 可以把 poison 细化成
  任意具体值（`freeze(poison)` 会被 InstCombine 折成 0），占位会静默变成具体值并丢掉
  `!notdec.unknown.source` 归因。占位统一用 `notdec.unknown.*` + `!notdec.unknown.source`。
- **边界两侧的位要闭合**：callee 返回槽里"可能还是入口值"的位，必须由该 callee 的入口形参承载；
  callee 形参要的位，必须在每个调用点答得出来（调用点 range SSA 值 / 调用者自己的形参 / 裸读）。
  只做单边（例如只扩入口形参）会把裸读换成调用点的 unknown。

背景、例子和演进见 `Arch2.md` 第 13 节；收口计划见
`logs/20260927-02-native-register-value-source-plan.md`。

旧 heritage pass、prototype recovery、Ghidra JSON schema、工具和 exporter 已删除；相关历史只保留在 logs 和 git 历史中。不要重新引入旧路，SummarySSA 新功能直接落在 `passes/summary/`。


