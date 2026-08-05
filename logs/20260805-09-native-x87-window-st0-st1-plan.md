# 20260805-09 native x87 改为 ST0/ST1 窗口显式建模（intrinsic 库只留衔接 API）

## 原始 prompt

```text
详细解释一下当前是怎么处理的？是增加了一个ST0全局变量吗？还是说专门匹配那种ret 前面的模式？

当前这种方式还是太启发式了，能不能这样：让所有 intrinsic 函数仅维护 ST0 和 ST1 同时显式表示出对 ST0 和 ST1 的操作，但是如果涉及ST2及之后的寄存器，就把它们看作是库内部的状态，不暴露出来。比如，如果是 pop 的话，就搞一个 intrinsic 函数获取 ST2 的值，然后给它 store 到 ST1 里。关键在于当前这种方式要去走 register summary静态分析的整个流程，把ST0寄存器的相关信息正确分析出来，再用静态分析的结果作为参数或返回值的依据，和其他的参数和返回值推理保持一致

对，按照这个做一下吧
```

## 背景

上一轮（20260804-08）解决 SysV long double 返回（ST0 留在 x87 栈）用的是启发式：检测"ret 前最后一条计算指令是 x87 intrinsic"，命中就把函数签名保持 void，值留在库内部状态，调用方用 `notdec.x87.fstp.f80()` 弹出。缺点：

- 是"ret 前模式匹配"的启发式，不走 register summary 的静态分析，和其他寄存器（RAX/XMM）的参数/返回值推理不一致。
- 函数只要体内有 x87 intrinsic 且某条返回路径最后是 x87 指令就会被判定，容易误报/漏报。

本轮改为：**ST0/ST1 建成真正的 LLVM 寄存器全局（i80），窗口内的操作直接展开成 LLVM 指令；ST2..ST7 留在库内部状态**，库只保留 4 个衔接 intrinsic。这样 ST0 走完整的 register summary 静态分析：函数尾 `store @ST0` → `ExitDemand` → 返回槽 `x86_fp80`，调用点拿返回值和传参走和 RAX/XMM0 相同的路径。

## 目标

- x87 机器指令按 mnemonic 直接生成"窗口展开"代码（继续跳过 p-code 部分），不再生成整条 intrinsic 库调用。
- `@ST0`/`@ST1` 是寄存器全局（i80，底层存储沿用 RegisterStorage 的整数寄存器模型，窗口边界用 bitcast 接 `x86_fp80` 浮点运算），ST2..ST7 不建全局。
- 库 intrinsic 收敛为：`notdec.x87.push(x86_fp80)`、`notdec.x87.pop() → x86_fp80`、`notdec.x87.peek(i8) → x86_fp80`、`notdec.x87.poke(i8, x86_fp80)`；fprem 改 `x86_fp80 notdec.x87.fprem(x86_fp80, x86_fp80)`；环境类（fnstsw/fnstcw/fldcw/fstenv/fldenv）维持库调用不变。
- SysV long double 返回：函数尾 ST0 有值 → summary 推断出 `x86_fp80` 返回槽，调用点直接拿返回的 `x86_fp80`；删除上一轮的 `functionReturnsX87StackValue` 启发式。
- wrk 全量提升 `llvm-as` + `opt -passes=verify` 通过，`stats_stdev` 等函数得到 `x86_fp80` 返回槽且无 unknown 返回绑定；fortune i386/x86_64 与 `ctest -R native` 不回归。

## 技术路线

1. **lifting（`lib/PcodeToLLVM.cpp`）**：
   - 寄存器过滤从"去掉全部 ST0..ST7"改为"只保留 ST0/ST1（i80 全局），过滤 ST2..ST7"；寄存器列表缺 ST0/ST1 时（heritage JSON / 单测）补默认项，保证窗口总有实体全局。
   - mnemonic 分类器保留（识别槽位/内存操作数），但产物从 `X87IntrinsicSpec` 换成窗口语义规格；折叠组仍整组抑制 p-code（只降内存操作数 LOAD/地址计算、跳过 STORE），由 lowering 手工展开：
     - push（fld/fild/fldz/fld1/fld st(i)）：`ST1←旧ST0`、`ST0←新值`、`push(旧ST1)`。
     - pop（fstp st(0)/fstpt/fistpt）：`ST0←旧ST1`、`ST1←pop()`；fstp 先 store 旧 ST0 到内存。
     - `faddp` 等：`s=fop(ST1,ST0)`、`ST0←s`、`ST1←pop()`；带显式槽位（`faddp st(2)`）用 peek/poke。
     - 寄存器算术（`fadd %st,%st(2)`）：`ST0←fop(ST0, peek(i))`，反向（fsubr/fdivr）调换操作数。
     - 内存算术（`fadds mem`）：读内存操作数转成 `x86_fp80` 再算。
     - `fsqrt/fabs/fchs`：`llvm.sqrt.f80` / `llvm.fabs.f80` / `fneg`。
     - fcomi/fucomi/fcomip/fucomip：直接 `fcmp`（`PF=uno`、`ZF=eq|uno`、`CF=lt|uno`，与 x86.sinc `fcomi` 宏一致），写 EFLAGS CF/PF/ZF、清 AF/SF/OF；`*P` 再 pop。
     - `fxch st(i)`：窗口内 i=0/1 直接交换，i>=2 peek/poke；`ffree st(i)` 写 undef；`fprem` 库调用。
   - `lib/NativeX87Intrinsic.cpp/.h`：新增 push/pop/peek/poke 声明，更新注释为窗口模型。
2. **summary（`lib/passes/summary/NativeRegisterSummarySSA.cpp`）**：
   - 删除 `functionReturnsX87StackValue` 及其辅助函数（`isX87StackPreservingIntrinsic`、`instructionIsX87StackReturn`、`blockLastComputationIsX87`、`blockIsStackCleanup` 等，先确认无其他引用）。
   - `collectAbiFacts`：手工把 ST0 加进 `FloatOutputsInOrder`/`Outputs`/`InternalReturnRegisters`（cspec 没有 x87 槽，类似 DF 的手工补充）；ST0/ST1 不进 `InternalParamRegisters`，且 `shapeForInternalFunction` 的加参路径显式跳过 ST0/ST1（SysV 不传 x87 参数，入口读 ST0 = unknown）。
   - `seedAbiReturns`：根函数除 RAX 外也 seed ST0 的 ExitDemand。
   - `floatTypeForSizeBits`：80 位 → `x86_fp80`，让 `floatSlotForDemand` 产出 f80 返回槽。
   - 调用点破坏：ST0/ST1 不在 cspec `Unaffected`，通用 clobber 路径自动在调用点写全量 + CallClobber，无需手工。
3. **测试**：`tests/pcode_to_llvm_test.cpp` 的 x87 用例按窗口模型重写（fild/fstp/fldz/fdivrp st(2)/fsubr st(3)/fcomi 等）；`tests/native_register_summary_ssa_test.cpp` 的 `testX87StackReturnKeepsVoidReturnType` 换成"fldt 形态函数恢复出 `x86_fp80` 返回槽"的正向用例。

## 风险与判断标准

- 窗口模型的 push/pop 不追踪逻辑栈深：栈浅时 old ST1 是垃圾值，也会 push 进库。这与 p-code 滚动 COPY 的模型等价（p-code 本身也拷垃圾），且编译器生成的 x87 代码 push/pop 平衡，实际值会被覆盖。不平衡的 `fld; ret` 正是 long double 返回场景，由调用方 pop 取走。
- 库 intrinsic 签名是外部声明，改动签名（fprem）不破坏本仓库；外部运行库需按新契约实现。
- fcomi 三态标志、fstp st(1) 的"pop 但 ST0 不变"、`faddp st(2)` 的 poke 后再 pop 取回，都是容易写错的地方，用 wrk 的 stats 模块和单测逐条核对。
- 判断标准：`pcode_to_llvm_test`、`native_register_summary_ssa_test` 通过；wrk 全量 `notdec-native-llvm wrk --all-confirmed --skip-runtime` rc=0，IR 里 `stats_stdev`/`stats_mean` 返回 `x86_fp80`、无 unknown 返回绑定；fortune i386/x86_64 回归脚本通过；`ctest --test-dir build -R native` 15/15。

## 不做什么

- 不做 `complex long double`（ST0+ST1 返回）和多 long double struct 返回。
- 不追踪 x87 逻辑栈深、不做 rounding mode 精确模拟（fistp 用 LLVM `FPToSI`，与 p-code FLOAT_TRUNC 一致）。
- 外部 `sqrtl`/`round` 的 long double 原型恢复押后（外部原型问题，不属本轮）。

## 实现记录（2026-08-05）

按上述方案完成，与计划的两处偏差见下。

### 实现内容

- `lib/PcodeToLLVM.cpp`：`readX87ST(uint32_t, bool windowShift=false)` 给窗口移位读打 `notdec.x87.window.shift` 标记；`x87WindowPush`（读 oldST0/oldST1）、`x87WindowPop`（读 oldST1）、`lowerX87Fxch`（st(1) 交换）传 `windowShift=true`。窗口移位读（push/pop/fxch 的槽位搬移）只转移窗口内部状态，不是对寄存器值的真实消费。
- `lib/passes/summary/NativeRegisterSummary.cpp`：`registerLoad` 跳过带 `notdec.x87.window.shift` 的 load，并新增 `isX87WindowShiftLoad` 兜底识别（InstCombine 会重写 load 丢 metadata）。识别规则：load 的 global 是 ST0/ST1 且**所有 use**（剥 bitcast 单链）都是 store 到寄存器全局或 `notdec.x87.*` 库调用；任一 use 是内存 store / 浮点运算则不是纯移位（fld %st(0) 与 fstpt 共享同一 load 的缓存复用场景）。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp`：只保留窗口模型轮次的改动（collectAbiFacts 补 ST0 输出、floatTypeForSizeBits 80 位、addParamForUnit 跳过 ST0/ST1、去掉 functionReturnsX87StackValue）；**没有**在 SSA 阶段跳过窗口移位 load（会导致 readRangeEntry/completeRangePhi 无限递归崩溃，SSA 阶段依赖这些 load 参与 range 流）。
- `lib/passes/summary/NativeRegisterSummary.cpp`：`seedAbiReturns` **不 seed 根函数 ST0**（计划里写的是 seed，实现时发现 seed 会让 ST0 需求沿调用图反向级联，给只在返回路径被调用点 clobber 的函数误加 x86_fp80 返回槽，fortune i386 大量误报；改为只靠真正读 @ST0 的调用点产生 ExitDemand）。
- `tests/native_register_summary_ssa_test.cpp`：`testX87StackReturnBecomesX86Fp80ReturnSlot` 改为加一个调用者（根函数）在调用后 `load @ST0` 存内存模拟 fstpt 接住返回值，由调用点提供 ST0 ExitDemand（无调用者的 internal 函数不恢复返回槽是正确的，seed 已去掉）。
- `tests/pcode_to_llvm_test.cpp`：fildl 窗口 push 用例补断言 `notdec.x87.window.shift` 标记存在。
- `scripts/native-fortune-i386-regression.sh`：断言从"无 @ST 全局 + fild/fstp intrinsic"更新为"有 @ST0/@ST1、无 @ST2..@ST7、有 notdec.x87.push/pop、warning 里无 ST[2-7]"。

### 验证

- `pcode_to_llvm_test`、`native_register_summary_ssa_test` 通过。
- `ctest --test-dir build -R native` 15/15 通过；`notdec.heritage_to_llvm.forward_defs` 失败是既有 baseline（stash 全部改动后仍失败，与 20260725-01 日志一致）。
- fortune i386 / x86_64 回归脚本通过；fortune i386 剩 1 条 ST0 clobber warning（外部调用后窗口移位读 clobber，语义正确）。
- wrk 全量 rc=0，`llvm-as` + `opt -passes=verify` 通过；`stats_mean`/`stats_within_stdev` 恢复 `x86_fp80` 返回，`FUN_88c0`/`stats_percentile`/`FUN_6b40` 的误报返回槽消除。ST0 warning 从 6 条降到 3 条：`main->gettimeofday`、`FUN_7300->luaL_checknumber` 是调用点后读 clobber（语义正确），`stats_stdev` 的 return_binding 是外部 `sqrtl` 原型恢复成 i64 导致 ST0 返回没接住（外部原型问题，按计划押后）。
