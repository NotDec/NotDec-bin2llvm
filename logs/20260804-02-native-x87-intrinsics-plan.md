# 20260804-02 native x87 intrinsic 提升

## 原始 prompt

```text
我觉得应该按照这样的方式设计 intrinsic：
1. 不需要任何全局变量的状态，就仿佛在使用一个外部链接进来的 C 语言库，在 C 语言库内部，这个库自己会维护一个全局状态。
2. 因此，每个具体的 API 并不需要引用某个状态，lifting得到的IR也不需要额外的全局状态变量。
3. 每个 API 的输入输出尽量贴合底层汇编指令，应该不太需要额外的参数。
4. 不需要把ST相关寄存器也弄成全局变量，直接不创建对应的寄存器全局变量
再重新设计一下看看

对，形成一个规划文件，然后开始实现，x87指令都lifting成这些intrinsic的call吧
```

## 背景

fortune i386 是 gcc i386 默认浮点，确实有 x87 指令（0x1a5c/0x1a64/0x1a87/0x1a8b/0x1b10/0x1b2e/0x1b37/0x1b43/0x1b45/0x1b49），不是解码 bug。Ghidra 的 ia.sinc 把 x87 寄存器栈 ST0..ST7（register space offset 0x1100..0x1140，各 10 字节）建模成物理寄存器，每条 x87 指令展开成一段"滚动" p-code：push 类先 `ST7=ST6;...;ST0=旧ST7` 再写 ST0，pop 类先算再 `ST0=ST1;...;ST6=ST7`。

当前提升把这些 ST 寄存器建成外部 `i80` 全局，再经过 SummarySSA 的 ABI clobber 处理，ST6 在 19b0 的 `__fprintf_chk` 调用点被 clobber 值接管，`fdiv` 分子变成 clobber 污染（`%ST1569604.cast = bitcast i80 %ST6.clobber706 to x86_fp80`），`@ST1` 还有未定义读（滚动写被优化丢）。这是 i386 fortune 剩余的最后一条 register SSA 告警。

## 目标

x87 指令提升为外部库风格 intrinsic call：

- IR 里不再创建 ST0..ST7 全局，也没有任何 x87 状态全局变量。
- 每条 x87 指令对应一个 `notdec.x87.*` 声明函数，参数贴合汇编操作数（如 `fildl (%ecx)` → `notdec.x87.fild.i32(i32)`），FPU 栈状态藏在库内部。
- 19b0 的 `fdiv` 用真实的 `fmul` 结果，ST6 clobber 告警消失，llvm-as/opt verify 通过。

## 技术路线

在 `lib/PcodeToLLVM.cpp` 的指令归组层（按 `Address` 分组，已有 `prepareX86CallReturnStackSuppression` 同款模式）加 x87 折叠：

1. 过滤 RegisterStorage：构造前把 `program.Registers` 里 ST 区（register space offset 0x1100..0x1140、size 10）的项剔除，`notdec.x87.*` intrinsic 声明不带任何状态参数。
2. `prepareX87IntrinsicSuppression(program)`：逐指令扫描，组内有 ST varnode 就分类成一条 intrinsic 描述（名称、参数 varnode、返回类型、fstp 的 STORE 地址）。
3. 分类规则（按 Ghidra 展开形状）：
   - `ST0 = INT2FLOAT(mem)` → `fild.i16/i32/i64`，参数按 mem size。
   - `ST0 = FLOAT2FLOAT(mem)` → `fld.f32/f64/f80`。
   - `STORE(addr, FLOAT2FLOAT(ST0))` → `fstp.f32/f64/f80`，返回后由 IR 写内存。
   - `STORE(addr, FLOAT_TRUNC(ST0))` → `fistp.i32/i64`。
   - `ST0 = FLOAT_ADD/SUB/MULT/DIV(mem, ST0)` → `fadd/fsub/fmul/fdiv/fsubr/fdivr.<f32/f64>`（subr/divr 是 mem 在前）。
   - `ST1 = FLOAT_ADD/SUB/MULT/DIV(ST0, ST1)` → `faddp/fsubp/fmulp/fdivp/fsubrp/fdivrp()`（无参）。
   - 未覆盖形态（fxch/ffree/fld %st(i)/fcom 等）不折叠，按普通 p-code 降（ST 全局已过滤，不建全局不崩，语义不保证）。
4. 折叠时组内非 ST op（LOAD/INT_ADD 取参）正常 lower 进 SSA cache，intrinsic call 的参数直接 `read()`；ST 滚动 COPY、ST0/ST1 的浮点写、以及 fstp 的 STORE 都跳过，fstp 的 STORE 用 call 返回值重建。
5. intrinsic 声明挂 `memory(inaccessiblemem: readwrite)` + `nounwind`：隐藏栈状态不可见普通内存，防止被优化乱排/误删，同时不影响对显式内存操作数的分析。
6. SummarySSA：`isNotDecRegisterHelperCall`（`NativeRegisterSummarySSA.cpp`、`NativeRegisterSummary.cpp`）把 `notdec.x87.*` 也当 helper 跳过，调用点不再按 ABI clobber ECX/EDX/ST；ST 单元不建后 killedbycall 告警自然消失。

## 阶段计划与判断标准

1. 规划文档（本文件）。
2. PcodeToLLVM 折叠实现 + RegisterStorage 过滤。
3. SummarySSA 跳过 intrinsic。
4. 构建 `notdec-native-llvm`，跑 i386/x64 fortune 回归 + `ctest -R native`。

判断标准：

- IR 无 `@ST*` 全局、无 `summary_clobber` ST6 告警。
- 19b0 的 `fdiv` 两个操作数来自真实 `fild`/`fmul` 结果，不是 clobber/unknown。
- x64 fortune 与 native 单测无回归。

## 风险与不做什么

- 风险：intrinsic 调用顺序/副作用建模依赖 `inaccessiblemem` 属性；不同优化级别下库内状态一致性由库语义保证，本方案只保证指令间不重排。
- 不做什么：不实现 x87 栈的 IR 内仿真（不用全局、不用 phi）；fxch/ffree/fcom/fld %st(i) 等 fortune 未出现的形态先不折叠，fallback 原 p-code 路径，后续按需要补。

## 实现（已完成）

按规划实现，fortune i386 的 x87 全部折叠为 `notdec.x87.*` call，ST 全局和 ST6 clobber 告警清零。

改动文件与函数：

- 新增 `include/notdec-bin2llvm/NativeX87Intrinsic.h`、`lib/NativeX87Intrinsic.cpp`：`isNativeX87IntrinsicName()`、`getOrInsertNativeX87Intrinsic()`，声明挂 `nounwind memory(inaccessiblemem: readwrite)`。
- `lib/CMakeLists.txt`：加入 `NativeX87Intrinsic.cpp`。
- `lib/PcodeToLLVM.cpp`：
  - 文件作用域 `isX87StackVarnode()`（register space offset 0x1100..0x1140、size 10，MMX 同 offset 但 size 8 不进）、`x87StackIndex()`、`registersWithoutX87Stack()`（构造 RegisterStorage 前过滤 ST 项，`lower()` 约 233 行）、`fpSuffix()`、`popArithName()`、`memArithName()`。
  - `PcodeLowerer::prepareX87IntrinsicSuppression()`（约 733 行）：按机器指令 Address 分组，含 ST varnode 的组交给 `classifyX87Intrinsic()`（约 765 行）分类成 `X87IntrinsicSpec`（约 416 行）。
  - 分类覆盖：fild（INT2FLOAT 写 ST0，按 2/4/8 字节出 `fild.i16/i32/i64`）、fld（FLOAT2FLOAT 写 ST0，`fld.f32/f64/f80`）、fstp/fistp（STORE 的值来自 FLOAT2FLOAT(ST0)/FLOAT_TRUNC(ST0)，`fstp.f32/f64/f80`/`fistp.i32/i64`，注意 STORE 有 3 个输入，地址是 Inputs[1]）、mem 算术（FLOAT_ADD/SUB/MULT/DIV 写 ST0，`fadd/fsub/fmul/fdiv/fsubr/fdivr.<f32/f64>`，mem 操作数溯源到 FLOAT2FLOAT 的原始 LOAD）、pop 算术（写 ST1 且两输入都 ST，`faddp/fsubp/fmulp/fdivp/fsubrp/fdivrp()`）。
  - 主循环约 269 行：组首 op 调 `lowerX87Group()`（约 954 行）——组内非 ST op（LOAD/INT_ADD 取参）正常 lower 进 SSA cache，再生成 intrinsic call；fstp/fistp 用返回值重建 STORE（`memoryPointer` 转指针）。
  - 未覆盖形态（fxch/ffree/fld %st(i)/fcom 等）不折叠，回退普通 p-code（ST 已过滤，不建全局不崩）。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp`：`isNotDecRegisterHelperCall()`（约 874 行）加 `isNativeX87IntrinsicName`；`isUnknownExternalFunction()`（约 515 行）排除 x87 intrinsic。
- `lib/passes/summary/NativeRegisterSummary.cpp`：`isNotDecRegisterHelperCall()`（约 782 行）加 x87；`isUnknownExternalCall()`（约 1434 行）跳过 helper。
- `scripts/native-fortune-i386-regression.sh`：断言无 `@ST` 全局、有 `notdec.x87.fild.i32`/`fstp.f64`、warning TSV 无 `ST[0-7]` 行。
- `tests/pcode_to_llvm_test.cpp`：`testX87FildlFoldsToIntrinsicCall()`（约 1650 行）、`testX87FstpFoldsToIntrinsicCall()`（约 1690 行），手写滚动 p-code，断言折叠成 intrinsic call、无 ST0 全局、verifier 通过。
- `tools/SleighBytes.cpp/h`、`tools/notdec-native-pcode.cpp`：`printPcodeProgram` 加 `withAddress` 参数，pcode dump 每行打印机器指令地址（排查指令分组的调试增强，native-pcode 默认开）。

验证：

- `cmake --build build --target notdec-native-llvm pcode_to_llvm_test` 通过。
- `scripts/native-fortune-i386-regression.sh` 通过：19b0 正常提升，`fildl`→`notdec.x87.fild.i32/i64`、`fdivrs`→`fdivr.f32`、`fsubrs`→`fsubr.f32`、`fstpl`→`fstp.f64` 返回值直接作为 `__fprintf_chk` vararg 参数、`fmulp`/`fdivrp` 无参；无 `@ST*` 全局；register SSA 警告只剩外部签名信息行，ST6 clobber 告警清零；`llvm-as` + `opt -passes=verify` 通过。
- `scripts/native-fortune-x86_64-regression.sh` 通过，x64 无 x87 无回归。
- `ctest -R native` 15/15 通过（含 realworld_fortune_i386/x86_64）。
- `./build/bin/pcode_to_llvm_test` 通过。

## 下一步

fortune 未出现的 x87 形态（fxch/ffree/fld %st(i)/fst/fcom/fnstsw 等）仍未折叠，回退普通 p-code，语义不保证；后续有真实用例再按同样模式补分类。
