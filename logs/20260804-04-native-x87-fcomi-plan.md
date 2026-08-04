# 20260804-04 native x87 fcomi/fucomip 折叠

## 原始 prompt

```text
对，当前先不考虑x87的实现，我只是说intrinsic的设计按照这种方式来。先解决fcomi/fucomip吧
```

## 背景

x87 提升已改为助记符驱动（20260804-03），fortune/wrk 里除 `FCOMI/FUCOMIP` 外全部折叠。wrk 剩 5 条不折叠（`FCOMI`×4、`FUCOMIP`×1），原因是这类指令不只碰 FPU 栈，还写 EFLAGS，现有 `X87IntrinsicSpec` 只支持"返回 ST0 值 + STORE 重建"，没有 flags 回写机制，分类器也没覆盖这四个助记符。

当前这些指令回退普通 p-code：ST 寄存器已过滤，`FLOAT_NAN/FLOAT_EQUAL/FLOAT_LESS` 读 ST0/ST1 得到 unknown，写出的 CF/PF/ZF 是垃圾，后续 `JNC/JZ/JA` 分支读到错误标志。

## 目标

- 新增 `notdec.x87.fcomi.sti(i8)` / `fcomip.sti(i8)` / `fucomi.sti(i8)` / `fucomip.sti(i8)`，返回 `i8` 打包三态（bit0=CF、bit1=PF、bit2=ZF，PF 在这里是 unordered 不是奇偶）。库内部读栈、比较、按变体 pop。
- `lowerX87Group` 对这类指令：整组 pcode 抑制，call 后把返回的 i8 按位写回 EFLAGS（CF=0x200/PF=0x202/ZF=0x206），并把 OF/AF/SF/C1 清 0、FPU 状态字 FSW（0x10a2）清 bit9。
- wrk 5 条告警消失，fcomi 后的分支读到真实比较结果。

## 技术路线

Ghidra 侧事实（已确认）：

- x86 flag 寄存器布局：CF=0x200、PF=0x202、AF=0x204、ZF=0x206、SF=0x207、OF=0x20b；C1=0x1091（1 字节）；FPUStatusWord=0x10a2（2 字节）。都是独立寄存器，RegisterStorage 按 Space/Offset/Size 匹配。
- fcomi pcode（wrk 0x84e8 FUCOMIP 实测）：`PF = FLOAT_NAN(ST0)|FLOAT_NAN(ST1)`，`ZF = PF | FLOAT_EQUAL(ST0,ST1)`，`CF = PF | FLOAT_LESS(ST0,ST1)`；const 清 OF/AF/SF/C1；`FSW &= ~0x200`；`FCOMIP/FUCOMIP` 再带 pop 滚动，`FCOMI/FUCOMI` 不 pop。

native 侧改动（`lib/PcodeToLLVM.cpp`）：

1. `X87IntrinsicSpec` 加 `bool WritesComparisonFlags`。
2. `classifyX87ByMnemonic()` 加 `FCOMI/FCOMIP/FUCOMI/FUCOMIP` 分支：从 `FLOAT_EQUAL/FLOAT_LESS` 的输入里找非 ST0 的 ST varnode 取 st(i) 下标（FCOMI 只有寄存器形式，无内存形式）；intrinsic 名 = 助记符小写 + `.sti`，参数 `i8` 下标，返回 `i8`；设 `WritesComparisonFlags`。
3. `lowerX87Group()`：`WritesComparisonFlags` 时整组 op 不 lower（fcomi 无内存操作数，组内 unique 都是私有）；call 后按位提取 i8 写 CF/PF/ZF，写 0 到 AF/SF/OF/C1，`FSW = read(0x10a2) & ~0x200` 写回。
4. 单测：`FCOMI ST0,ST1`（不 pop）和 `FUCOMIP ST0,ST1`（pop）各一条，断言折叠成 `notdec.x87.fcomi.sti(i8 1)` / `fucomip.sti(i8 1)`、CF/PF/ZF 有写入、无 ST0 全局、verifier 通过。

## 阶段计划

1. 本规划文档。
2. `X87IntrinsicSpec` + 分类分支 + `lowerX87Group` flags 回写。
3. 单测两条。
4. 验证：wrk 告警清零（5 条 fcomi/fucomip 都折叠）、IR 里 fcomi 后是 call→flag 写→分支读、`llvm-as`+verify；fortune i386/x86_64 回归；`ctest -R native`。
5. 更新本规划文档"实现"段 + 提交。

## 判断标准

- wrk 提升无 x87 告警；`notdec.x87.fcomi.sti/fucomip.sti` 出现且下标正确（ST1/ST2）。
- fcomi 后分支（`JNC/JA`）读的 CF/ZF 来自 intrinsic 返回值的写入，不是 unknown。
- 单测、fortune 回归、`ctest -R native` 全绿。

## 风险与不做什么

- 风险：FSW/C1 的读写依赖 RegisterStorage 有对应寄存器（已确认存在）；QNaN invalid 异常的差异（fcomi vs fucomi）由库实现负责，IR 只保证 flag 位正确。
- 不做什么：不实现 `notdec.x87` 库；heritage JSON 的 fcomi 保持回退（无助记符，形状兜底不含 fcomi）；不补 `FCOM/FCOMP/FCOMPP/FTST/FXAM` 等其它比较形态（fortune/wrk 未出现，同一模式后续按需补）。

## 实现（已完成）

改动文件与函数（`lib/PcodeToLLVM.cpp`）：

- `X87IntrinsicSpec` 加 `bool WritesComparisonFlags`（约 455 行）：fcomi/fucomip 专用，intrinsic 返回打包 i8（bit0=CF、bit1=PF、bit2=ZF），由 lowering 写回 EFLAGS/FPU 状态位。
- `classifyX87ByMnemonic()` 加 `FCOMI/FCOMIP/FUCOMI/FUCOMIP` 分支（约 1043 行）：从 `FLOAT_EQUAL/FLOAT_LESS` 的两个 ST 输入里取非 ST0 的下标（FCOMI 只有寄存器形式）；intrinsic 名 = 助记符小写 + `.sti(i8)`，返回 `i8`，设 `WritesComparisonFlags`。
- `lowerX87Group()`（约 1377 行起）：`WritesComparisonFlags` 时整组 op 不 lower（fcomi 无内存操作数、组内 unique 私有）；call 后按位写 CF=0x200/PF=0x202/ZF=0x206，清 AF=0x204/SF=0x207/OF=0x20b/C1=0x1091，FSW=0x10a2 清 bit9。
- `tests/pcode_to_llvm_test.cpp`：`addX87FcomiOps()`（按 FCOMI/FUCOMIP 的 pcode 手写 flag 计算 + 可选 pop 滚动）、`testX87FcomiFoldsToIntrinsicCall()`、`testX87FucomipFoldsToIntrinsicCall()`，断言折叠成 `fcomi.sti(i8 1)`/`fucomip.sti(i8 1)`、CF 全局有写入、无 ST0 全局、verifier 通过。

验证：

- `./build/bin/pcode_to_llvm_test` 通过。
- wrk：提升无任何 x87 告警；`fcomi.sti(i8 1)`×3、`fcomi.sti(i8 2)`×1、`fucomip.sti(i8 1)`×1，与 objdump 逐条对上；IR 中 fcomi 后是 `call`→flag 位→`JNC/JA` 分支（如 `%1 = and i8 %0, 1` 后 `icmp eq` 跳转），`llvm-as` + `opt -passes=verify` 通过。
- fortune i386/x86_64 回归通过；`ctest --test-dir build -R native` 15/15 通过。

说明：wrk 里 flag 寄存器经 SummarySSA 后以 SSA 值流动（IR 无 `@CF` 等全局），fcomi 后的分支直接从 intrinsic 返回值算 `CF|ZF`，与直接读写 flag 寄存器等价；单测里 RegisterStorage 路径（`@CF` 全局写入）单独覆盖。

### 实现补充（FSW/C1 移出 LLVM 层）

复查后调整：fcomi 原来还写 `C1=0x1091` 和 `FPUStatusWord=0x10a2`（清 bit9），这两个是"只写不读"的全局——FSW 在 pcode 层只被 `FNSTSW/FSTSW` 等 x87 指令读，整数代码读不到它（唯一桥是 `FNSTSW AX`，走普通 AX 寄存器）；Ghidra 也没把 fcomi 的 C0/C2/C3 写进 FSW（比较结果映射成 EFLAGS）。按 intrinsic 设计"FPU 状态全由库维护"，把这两处写入去掉，C1 清零由库的 fcomi/fucomip 实现负责。

改动（`lib/PcodeToLLVM.cpp`）：

- `X87IntrinsicSpec` 注释（约 455 行）：改为"lowering 只写整数代码能观察到的 EFLAGS 位，状态字与 FPU 栈一样归库内部"。
- `lowerX87Group()`（约 1411 行）：删掉 `write(flagVarnode(0x1091,1), zero)` 和 `FSW = read(0x10a2) & 0xfdff` 写回，保留 CF/PF/ZF 写入和 AF/SF/OF 清零；注释说明 C1/FSW 由库负责。
- `tests/pcode_to_llvm_test.cpp`：两条 fcomi 单测追加断言，IR 不得使用 `FPUStatusWord`/`C1` 全局。

验证：

- `./build/bin/pcode_to_llvm_test` 通过；`ctest --test-dir build -R native` 15/15 通过；fortune i386 回归通过。
- wrk 全量（默认 summary-SSA 路径）：IR 无 `@FPUStatusWord`/`@C1`，fcomi 后分支仍直接从返回值算 `CF|ZF`；`--no-register-ssa-pass` 路径：只写 `@CF/@PF/@ZF`（清 `@AF/@SF/@OF`），无 FSW/C1 全局，`llvm-as`+verify 通过。
