# 原始 prompt

```text
根据整个bin2llvm针对x64的链路来看，下一步是不是就是静态分析，然后再后一步就是register SSA消除寄存器了？是的话，就分析一下静态分析那一块怎么支持 x86。主要是要重构，使得参数分析支持栈上传递。现在是规划阶段，先规划，不要直接执行

得在external/NotDec-bin2llvm/AGENTS.md里面记录一下，heritage那个路线不再维护，接近弃用了

注意一定要从cspec文件里面获取传参的栈范围。这样严格按照ABI标准走。当前规划里面是不是已经是这样了？另外，要同时考虑到x64的栈传参，X86和X64的栈传参一起修
```

# 背景

i386 fortune 的 spec、PLT/GOT、CALL/RET 栈动作、基础 stack frame rewrite 和 GS canary 已经先后补上。当前剩下的 `ESP.entry` 主要是正偏移 caller stack argument / return slot 形状，不属于前一轮只处理 fixed negative offset local stack 的范围。x64 当前主链路已经能处理寄存器参数，但 ABI 规定的栈上传参、溢出参数和部分 vararg tail 也应该进入同一套模型。

按默认 native x64 链路看，寄存器消除不是一个后置独立阶段。当前 `notdec-native-llvm` 默认跑 `NativeRegisterSummarySSA`，里面先做 stack frame / canary 前置清理，再跑第一遍 summary、外部签名推断、第二遍 summary，最后做 signature rewrite 和寄存器残留清理。

所以下一步不是去维护旧 heritage 路线，也不是先写一个独立 register SSA pass，而是先把 SummarySSA 里的参数/签名静态分析从“只会寄存器参数”改成“能表达并使用 cspec 定义的寄存器/栈参数”。register SSA 消除要吃这个结果。

# Ghidra 对应实现

Ghidra 的 x86-family ABI 都来自 cspec，native 侧必须沿用这些 `pentry` 范围，不能在 SummarySSA 里另写一套固定栈偏移规则。

i386 ABI 来自 `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86gcc.cspec`：

- 默认 `__cdecl` 使用 `ESP` 作为 stackpointer，`stackshift=4`。
- 输入参数的主 `pentry` 是 `space="stack"`、`offset=4`、`align=4`。
- 返回值仍主要走 `EAX`，宽返回可能走 `EDX:EAX` join。

x64 ABI 来自 `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec`：

- 默认 x86-64 gcc prototype 使用 `RSP` 作为 stackpointer，`stackshift=8`。
- 前几个整数/浮点参数走 register `pentry`。
- register 参数之后还有 `space="stack"` 的栈参数 `pentry`，默认栈起点是 `offset=8`、`align=8`。
- MS ABI prototype 也在同一个 cspec 中，栈参数起点不同，例如 shadow space 后的 `offset=40`。后续如果启用该 prototype，也必须从 cspec 读，不硬编码。

Ghidra decompiler 的通用参数逻辑在：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamEntry` 描述 ABI storage 范围。
  - `ParamTrial` / `ParamActive` 保存候选参数。
  - `ParamListStandard` 负责判断某个 storage 能不能作为参数。
  - `FuncProto::deriveInputMap()` 把 active trials 变成最终参数。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamListStandard::possibleParamWithSlot()` 用 storage space、offset、size、align 匹配 ABI slot。
  - `FuncCallSpecs::checkInputTrialUse()` 判断 callsite 参数是否真的被使用。
  - `FuncCallSpecs::buildInputFromTrials()` 把 stack trial 翻译成调用参数。

关键点是：Ghidra 没有把栈参当特殊功能，而是把 register 和 stack 都当 ParamEntry storage。native 要复刻这个方向。第一版只覆盖简单、固定 offset 的栈参，但 offset/range/align 必须来自 cspec 解析出的 `NativeAbiSpec`。

# 当前 native 缺口

当前 SummarySSA 的核心数据结构还是寄存器专用：

- `AbiFacts` 只收集 register input/output，忽略 cspec 里的 stack input pentry，所以 i386 栈参和 x64 register overflow 栈参都没有统一入口。
- `NativeSignatureSlot` 只有 integer register 和 float register 两种。
- `NativeRegisterCallInputSlot` 只记录 register unit、bit offset 和 bit size。
- `typedParamSlot()`、`shapeForKnownExternal()`、`shapeForInternalFunction()` 都按 ABI input register 分配参数。
- 外部签名推断依赖 register local-definition prefix；在 i386 `__cdecl` 下会得到 arity 0 或 fixed vararg mapping incomplete，在 x64 超过寄存器参数数量后也不能表达 stack tail。

旧 `NativePrototypeRecovery` 已经有过 stack input candidate/rewrite 代码，但它只在 `--heritage-register-ssa-pass` 路线运行。按 AGENTS.md 新约束，heritage 路线只保留历史对照和必要维护，不作为本轮实现基础。

# 技术路线

## 1. 先重构参数 storage 表达

给 SummarySSA 的签名模型补一个统一 storage 层：

- register slot 继续保存 backing register unit、bit range、float/integer 类型。
- stack slot 保存 stack space、ABI offset、size、align、slot order 和 LLVM 参数类型；这些字段都来自 cspec `pentry`，不能从架构名手写。
- `slotType()`、function type 构造、warning 文本、call shape 都通过统一 slot 访问，不再默认 `slot.Unit` 必然存在。

这一步只改模型表达，不急着推断复杂栈参，目的是先让 i386 `__cdecl` 和 x64 stack overflow ABI shape 都可以被表示。

## 2. 扩展 ABI facts

`collectAbiFacts()` 需要保留 cspec 输入顺序里的 stack pentry，并记录成可匹配的 stack range。实现时以 `NativeAbiSpec::Inputs` 为唯一来源：

- i386 默认 ABI 的参数范围来自 `x86gcc.cspec` 的 `stack+4` / `align=4`。
- x64 默认 ABI 的栈参数范围来自 `x86-64-gcc.cspec` 的 `stack+8` / `align=8`，排在 register pentry 后面。
- 如果将来支持 x64 MS ABI，也从同一个 cspec 的 prototype 读 `stack+40`，不在 SummarySSA 里特殊判断。
- 参数 slot 展开、offset 匹配、size/alignment 检查复用 `NativePrototypeModel::findInputStack()` 这类 cspec pentry 匹配逻辑，避免两套规则。

x64 仍以现有 register 参数为主，但 register 参数溢出后的 stack pentry 要作为同一套 ABI 参数序列的一部分。目标是 x64 现有行为不退化，同时能处理超过寄存器参数数量的栈参。

## 3. 支持 callee 侧固定栈参读取

第一版只识别简单正偏移入口栈读取，栈指针寄存器来自 cspec metadata：

- i386：`ESP.entry + 常量正偏移` 再 load。
- x64：`RSP.entry + 常量正偏移` 再 load。
- offset 必须匹配 ABI stack pentry。
- load 必须有真实 use。
- 不处理动态 ESP、复杂 alias、byval、结构体、varargs 内部展开。

识别后把这些 stack input 加进 internal function signature shape，rewrite 时用 LLVM 参数替换对应 load。目标是先消掉 i386 内部函数里最直接的 `ESP.entry + 4/8/...` 参数读取，同时支持 x64 超过 register 参数后的 `RSP.entry + 8/16/...` 读取。

`EBP + 正偏移` 可以作为第二阶段。它要依赖 frame pointer 和 entry ESP 的关系稳定，不能和保存/恢复 EBP、局部变量 slot 混在一起。

## 4. 支持 callsite 侧栈上传参

callsite 需要从“寄存器 store before call”扩展到“栈 slot value before call”，栈 slot 序列仍由 cspec 决定：

- 对已知外部函数，按 typed prototype / fixed args 映射到 stack slots。
- 对未知外部函数，用连续的 cspec stack argument stores/loads 推断 arity，避免 i386 全部变成 arity 0。
- 对 known vararg，固定参数必须能完整映射；可变尾部按连续 stack slots 推断，并受 `MaxArgs` 限制。
- 对 x64，先填寄存器 pentry，再按 cspec stack pentry 继续填溢出参数。

第一版只接受 call 前局部、固定 offset、没有跨 call clobber 的值。不能为了消 warning 把不确定的 stack slot 当参数。

## 5. 改写和清理要保守

signature rewrite 可以把 stack 参数变成 LLVM call operands / function args，但删除旧 stack store、ESP 调整、临时 inttoptr 必须保守：

- 只删确定由本次参数 rewrite 消费的 stack argument store。
- 不碰普通 push/pop、保存寄存器、局部栈 slot、动态栈调整。
- cleanup 仍放在 signature rewrite 后，和现有 dead store / stack frame cleanup 协同。

# 阶段计划

1. 写 SummarySSA slot/storage 重构单测：i386 `__cdecl` 和 x64 register+stack overflow 都能由 cspec pentry 形成 signature shape。
2. 扩展 ABI facts：保留 cspec stack pentry，并验证 i386 `stack+4`、x64 `stack+8` 都来自 metadata，不来自硬编码架构分支。
3. 实现 callee 侧 `ESP/RSP.entry + positive constant` stack input，先让内部函数参数读取能被 LLVM arg 替换。
4. 实现 callsite 侧固定 stack arg 绑定，先覆盖已知外部函数和直接内部调用。
5. 再做 i386/x64 known vararg / unknown external arity 推断，目标是减少 `incomplete_fixed_vararg_mapping` 和 arity 0 warning，并覆盖 x64 stack vararg tail。
6. 最后做 cleanup 和 fortune 回归，确认 `ESP.entry`/栈参数相关 `RSP.entry` 只剩真正不确定的动态/返回地址/未建模场景。

# 判断标准

- i386 SummarySSA 单测能覆盖 stack input function rewrite、direct callsite rewrite、known external fixed args、known vararg 固定参数。
- x64 SummarySSA 单测能覆盖 register 参数之后的 stack overflow 参数，以及 vararg stack tail。
- 单测必须证明 stack 参数范围来自 cspec metadata：同一套逻辑在 i386 `stack+4` 和 x64 `stack+8` 下都能工作。
- i386 fortune 的 `ESP.entry` 明显下降，`incomplete_fixed_vararg_mapping` 明显下降。
- i386 fortune 继续通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
- x64 fortune / native smoke 不退化，尤其是现有 register 参数、float 参数、vararg 推断不变；新增栈参测试通过。
- 不依赖 `notdec.prototype.*` metadata，不要求 `--heritage-register-ssa-pass`。

# 风险和不做什么

最大风险是把 caller stack argument、local stack、saved register、return address slot 混在一起。第一版必须只做固定 offset、可证明来源的栈参，并且参数范围只能来自 cspec `pentry`。不能因为 i386 常见 `ESP+4` 或 x64 常见 `RSP+8` 就在实现里硬编码。

暂不处理：

- 动态栈调整。
- alias 分析。
- struct/byval/sret。
- callee pop / stdcall 清理差异。
- 复杂 `EBP` frame pointer 场景。
- 完整 i386 float/x87 参数恢复。
