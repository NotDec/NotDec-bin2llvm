# 20260804-03 native x87 提升改为助记符驱动

## 原始 prompt

```text
能不能在pcode lifting的过程中，直接知道这些pcode对应的是哪个底层汇编指令，从而直接切换到生成intrinsic，而暂时不走pcode指令转换的思路？

当前的都是匹配pcode模式转换x87 intrinsic的吗？那是不是改成这种方式更好一些？
```

## 背景

当前 x87 折叠（`lib/PcodeToLLVM.cpp` 的 `classifyX87Intrinsic()`，约 765 行）完全靠反推 pcode 形状：找 STORE、找写 ST 的 FLOAT_* op，按 opcode 和操作数形状猜 fild/fld/fstp/算术。pcode 形状不唯一且脆：滚动顺序、unique 临时偏移每条指令都不同，fortune 与 wrk 同指令展开也不一致，新形态必须逐个补匹配分支。

实测 wrk（x86-64，158 条 x87）当前只折叠出 58 个 intrinsic call，静默回退的形态包括：`fstpt`(24)、`fstp %st(i)`(15)、`fldz`(10)、`fldt`(9)、`fxch`(7)、`fld %st(0)`(4)、`fcomi`(4)+`fucomip`(1)、`fsqrt`(2)、寄存器算术 `fadd/fsub/fmul/fdiv %st,%st(i)`(4)，以及少量 `fild/fadds/fstpl`。回退不是报错：ST 寄存器已从 RegisterStorage 过滤，未折叠指令的 ST 读写变成 unknown/SSA 临时值，IR 能过 verifier 但浮点语义不可信。

已验证的关键事实：`lib/SleighLift.cpp` 里已有 `AssemblyCollector`（`ghidra::AssemblyEmit` 实现），`SleighInstructionDecoder::decode()`（约 445-500 行）就是 `oneInstruction()` 拿 pcode 后调 `Engine.printAssembly()` 拿助记符/操作数文本（`SleighInstructionSummary.Mnemonic/Body`）。只是 lifting 主路径 `collectSleighPcode` / `collectSleighPcodeRanges` 只挂了 `PcodeCollector`，没跑 `printAssembly`。已用临时工具在 wrk/fortune 上 dump 验证：Ghidra 助记符与 pcode 一一对应（`FSTP tword ptr`、`FLD ST0`、`FSTP ST0`、`FXCH`、`FDIVRP`、`FUCOMIP ST0,ST1`、`FSQRT`、`FLDZ`、`FDIV ST0,ST1` 等）。

## 目标

- pcode lifting 时直接拿到每条机器指令的助记符，x87 分类先按助记符分派，pcode 只负责提供操作数值（内存地址、const、st 下标），不再靠 pcode 模式反推指令类别。
- wrk 除 `fcomi/fucomip` 外全部折叠成 `notdec.x87.*` call，消除静默回退。
- fortune i386/x86_64 已折叠路径不回归。

## 技术路线

Ghidra 侧已有 `AssemblyEmit`/`printAssembly` 机制，native 侧只做两件事：

1. **助记符挂到 pcode op 上**：`PcodeOpView`（`include/notdec-bin2llvm/Pcode.h`）加 `Mnemonic` 字段；`collectSleighPcode`/`collectSleighPcodeRanges` 的逐指令循环里，`oneInstruction()` 之后调 `Engine.printAssembly()`（顺序沿用 `SleighInstructionDecoder::decode()` 已有注释的原因：先 pcode 后 asm，避免 parser 缓存状态问题），用类似 `setInstructionSize` 的辅助函数把 mnemonic 挂到该指令产生的所有 op。
2. **分类改为助记符优先**：`classifyX87Intrinsic()` 按 `Ops[start].Mnemonic` 分派：
   - 现有形态照旧：`FILD`→`fild.i16/i32/i64`、`FISTP`→`fistp.i32/i64`、`FLD`+mem→`fld.f32/f64/f80`、`FSTP`+mem→`fstp.f32/f64/f80`、`FADDP/FSUBP/FMULP/FDIVP/FSUBRP/FDIVRP`→pop 算术。
   - 新形态：`FLDZ`→`fldz()`、`FLD1`→`fld1()`、`FLD`+ST→`fld.sti(i8)`、`FSTP`+ST→`fstp.sti(i8)`（含 `FSTP ST0` 纯 pop）、`FXCH`→`fxch.sti(i8)`、`FSQRT`→`fsqrt()`、`FADD/FSUB/FMUL/FDIV`+ST 操作数→`fadd.sti/fsub.sti/fmul.sti/fdiv.sti(i8)`（ST0 := ST0 op STi）。
   - `FCOMI/FCOMIP/FUCOMI/FUCOMIP` 本轮不折叠（写 EFLAGS，单独一轮设计返回三态）。
   - 操作数值仍从 pcode 取：内存地址来自 LOAD 的 ram 输入（现有逻辑），`st(i)` 下标来自 ST varnode 偏移（`x87StackIndex`），`fldz/fld1` 无参。
   - mnemonic 为空（JSON heritage 输入）时保留现有 pcode 形状分类兜底。
   - 新增 intrinsic 声明沿用 `memory(inaccessiblemem: readwrite)` + `nounwind`，`NativeX87Intrinsic.cpp` 的声明表加新名字。

## 阶段计划

1. 本规划文档。
2. `PcodeOpView.Mnemonic` + collect 路径挂助记符。
3. `classifyX87Intrinsic` 助记符优先分派 + 新 intrinsic spec（sti 下标参数）+ `lowerX87Group` 传参。
4. `NativeX87Intrinsic` 声明覆盖新名字。
5. 验证：wrk 全量 lifting 无回退（fcomi 除外）、fortune i386/x86_64 回归、`ctest -R native`。
6. 更新本规划文档的"实现"段 + 提交。

## 判断标准

- wrk 158 条 x87 中，除 5 条 fcomi/fucomip 外全部对应一个 `notdec.x87.*` call；新增形态（fldz/fld.sti/fstp.sti/fxch/fsqrt/寄存器算术）都能在 IR 里找到对应 call，不再有静默回退。
- fortune i386/x86_64 输出与现状一致（无 `@ST*` 全局、fild/fstp/fdivr/fmulp/fdivrp 折叠不变），`llvm-as` + `opt -passes=verify` 通过。
- `ctest -R native` 全绿。

## 风险与不做什么

- 风险：`printAssembly` 全量接入有额外开销，先直接接上，大 binary 变慢再按需开关；Ghidra 助记符是归一化命名（`FILD` 不分 fildl/fildll、`FADD` 不分内存/寄存器形式），类别判断够用，size/值仍从 pcode 取。
- 不做什么：不实现 `fcomi/fucomip`（涉及 EFLAGS 写入，单独一轮）；寄存器算术只支持 wrk 出现的目的 ST0 形态，`fadd %st(i),%st`（目的 STi）暂不补；不改 heritage JSON 路径（保留 pcode 形状兜底）。

## 实现（已完成）

改动文件与函数：

- `include/notdec-bin2llvm/Pcode.h`：`PcodeOpView` 加 `Mnemonic` 字段（约 108 行），注释说明空值表示 heritage JSON 输入。
- `lib/SleighLift.cpp`：
  - `setInstructionMnemonic()`（约 269 行）：把助记符挂到一条机器指令产生的所有 op。
  - `appendInstructionPcode()`（约 276 行）：`oneInstruction()` 后调 `Engine.printAssembly()`（顺序沿用 `SleighInstructionDecoder::decode()` 的注释原因），长度不一致时报错终止；失败仍按原逻辑清空 pcode。
  - `collectSleighPcode()`（约 526 行）、`collectSleighPcodeRanges()`（约 557 行）：各持一个 `AssemblyCollector` 传入 `appendInstructionPcode`。
- `lib/PcodeToLLVM.cpp`：
  - 文件作用域 `touchesX87Stack()`（约 154 行）。
  - `popArithName()`（约 201 行）：reverse 判据从 `input0 == ST1` 改为 `input0 != ST0`，支持 `fdivrp %st,%st(2)`。
  - 新增 `regArithName()`（约 235 行）。
  - `prepareX87IntrinsicSuppression()`（约 770 行）：分类失败且指令带助记符、含 ST varnode 时打 stderr 告警，消除静默回退。
  - `classifyX87Intrinsic()`（约 799 行）改为分派：有助记符走 `classifyX87ByMnemonic()`（约 876 行），空助记符（heritage JSON）走原形状分类 `classifyX87ByShape()`（原 `classifyX87Intrinsic` 改名）。
  - 新增辅助：`findStackWrite()`（约 811 行）、`stIndexFromSt0Copy()`（约 830 行）、`stIndexFromSt0WriteCopy()`（约 846 行）、`stIndexVarnode()`（约 862 行）。
  - 助记符分类覆盖：`FLDZ/FLD1`→无参、`FSQRT`→无参、`FLD`+ST→`fld.sti(i8)`（按 ST0 写 op 的 unique 生产者区分 `fld %st(i)` 与 f80 内存加载）、`FSTP`+mem→`fstp.f32/f64/f80`（FLOAT2FLOAT 或 COPY 生产者，f80 无转换 op）、`FSTP`+ST→`fstp.sti(i8)`（含 `fstp %st(0)` 纯 pop）、`FXCH`→`fxch.sti(i8)`、`FADD/FSUB/FMUL/FDIV`+ST→`fadd/fsub/fmul/fdiv.sti(i8)`、`FILD/FISTP`、mem 算术、pop 算术（st(1) 保留无参旧名，其他槽位用 `.sti(i8)`）。
- `tests/pcode_to_llvm_test.cpp`：`testX87FldzFoldsToIntrinsicCall()`（约 1760 行）、`testX87FdivrpSt2FoldsToIntrinsicCall()`（约 1825 行），分别验证 `FLDZ`→`notdec.x87.fldz` 和 `FDIVP ST2,ST0`→`notdec.x87.fdivrp.sti(i8 2)`，断言无 ST0 全局、verifier 通过。

实现中调整的点：

- `fld %st(i)` 不能只凭"有 `unique=COPY(STi)`"判断：`fldl/flds` 内存加载的滚动前缀也保存 `unique=COPY(ST7)`。最终按 ST0 写 op 溯源——`FLOAT2FLOAT(mem)`→f32/f64，`COPY(unique)` 的 unique 生产者是 `COPY(STi)`→`fld.sti`，是 LOAD→f80。
- `fldt`（f80）无转换 op，ST0 直接 `COPY(LOAD)`；`fstpt` 的 STORE 值 producer 是 `COPY(ST0)` 而非 `FLOAT2FLOAT(ST0)`，都靠 mnemonic 确认类别后放宽 producer 匹配。
- `fdivrp %st,%st(2)`（Ghidra 显示 `FDIVP ST2,ST0`）output 是 ST2 不是 ST1，pop 算术分支放开 output 槽位并用 `.sti(i8)` 传下标。
- 未折叠的 native x87 指令现在打 `warning: x87 instruction ... was not folded`，不再静默。

验证：

- `./build/bin/pcode_to_llvm_test` 通过。
- wrk（x86-64）：`notdec-native-llvm wrk --all-confirmed --skip-runtime` rc=0；158 条 x87 中提升范围内全部折叠，仅 5 条 `FCOMI/FUCOMIP` 告警（规划内单独一轮）；新增折叠 `fstp.f80`×23、`fstp.sti`×15、`fldz`×7、`fld.sti`×4、`fxch.sti`×7、`fsqrt`×2、`fld.f80`×9、寄存器算术 `.sti`×4；sti 下标与 objdump 逐条对上；IR 无 `@ST` 全局，`llvm-as` + `opt -passes=verify` 通过。
- `scripts/native-fortune-i386-regression.sh`、`scripts/native-fortune-x86_64-regression.sh` 通过。
- `ctest --test-dir build -R native` 15/15 通过。
