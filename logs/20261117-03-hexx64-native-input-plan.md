# hexx64.so 作为 native 输入的问题定位

> 用户原始 prompt：
>
> 当前bin2llvm除了当前位置开发，我还希望创建一个独立的worktree和分支用于开发。要不就在/sn640/NotDec-bin2llvm2 这里创建一个worktree吧。开发好了再合并到主worktree上面。
>
> 创建好了之后，尝试在/sn640/hexx64-dec/hexx64.so这个二进制上测试一下试试，基于它作为输入不断去修复问题。当前先看看对它会不会遇到问题吧。可能要先看看ddisasm的效果。

## 计划

- [x] P0 建立独立 worktree `/sn640/NotDec-bin2llvm2`（分支 `dev`，基于 main `78b8384`），构建写在 worktree 自己的 `build/`。
- [x] P1 跑 ddisasm 并评估效果：函数/块/指令覆盖、eh_frame 可信度、异常 FDE。
- [x] P2 全量 discovery + `--all-confirmed` lowering + LLVM 22 `llvm-as` / `opt -passes=verify` 冒烟。
- [ ] P3 定位并修复 3 个 `missing a native block` 被整函数跳过的样本（先拿到来源块）。
- [ ] P4 统计并收敛间接调用（discovery 2403 unresolved indirect call）与 `notdec.unknown`/裸寄存器读。
- [ ] P5 处理假 FDE / `ud2` 填充带来的假函数与 `_start` 垃圾 IR（确认这些是否应该被确认）。
- [ ] P6 每个修复按相同条件对比 fortune x86_64/i386 用例，保持不回退。

## 0. 输入与产物

- 输入：`/sn640/hexx64-dec/hexx64.so`，4.0MB，ELF64 DYN，x86-64，stripped
  （`.dynsym` 只有 556 个 UND 符号，无任何定义的 `FUNC`；`Clp_*` 是 import）。
- `e_entry = 0x25a40`，落在 `.text` 开头的 `ud2` 填充里。
- 依赖：`libida.so`、`libclpx.so`、libstdc++/libm/libc 等。
- `.text`：`0x25a40 .. 0x345c76`（3.28MB）；`.eh_frame`：8147 条 FDE。
- 工作目录：`/tmp/hexx64/`（gtirb、json、ll、bc 都在这里）。

命令（在 dev worktree 上）：

```bash
ddisasm --ir /tmp/hexx64/hexx64.gtirb --json /tmp/hexx64/hexx64.ddisasm.json \
  /sn640/hexx64-dec/hexx64.so

build/bin/notdec-native-discover --gtirb /tmp/hexx64/hexx64.gtirb \
  --summary-json /sn640/hexx64-dec/hexx64.so

build/bin/notdec-native-llvm /sn640/hexx64-dec/hexx64.so \
  --gtirb /tmp/hexx64/hexx64.gtirb --all-confirmed --register-ssa-summary \
  -o /tmp/hexx64/hexx64.native.ll

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/hexx64/hexx64.native.ll -o /tmp/hexx64/hexx64.native.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/hexx64/hexx64.native.bc -o /dev/null
```

## 1. ddisasm 效果

- 1.9.5（`6fd978cd 2026-11-17`），耗时约 5m28s，gtirb 96MB，只有一个告警：
  `Could not find GLOBAL/WEAK symbol for .L_5f3a00`。
- 汇编（gtirb-pprinter）33MB：8151 个 `@function`，809053 条指令；`.text` 内
  `.byte` 行约 8 万行（每行 1 字节），指令覆盖总体正常。
- `.text` 前 16 字节是 `ud2` 填充，`_start` 就指向这里；这就是 ddisasm 里
  `_start`、`FUN_25a42`…`FUN_25a4e` 一串“函数”的来源。
- **`.eh_frame` 明显被构造过**：8147 条 FDE 里 702 条长度 ≤4 字节（275 条 1 字节、
  353 条 2 字节），且顺序不按地址递增；大量 2 字节 FDE 正好覆盖 `ud2` 填充
  （如 `pc=0x25a40..0x25a42`、`0x25a42..0x25a44`…）。ddisasm 的 function inference
  把这类 FDE 当成函数入口，于是产生 709 个首指令是 `ud2` 的“函数”（341 个是纯
  单条 `ud2`）。我们的 discovery 用 confidence 过滤掉了其中大部分。
- 结论：ddisasm 的指令/块覆盖可用；函数集合里混有大量假入口，不能直接全信
  `functionEntries`，必须靠 discovery 的确认/置信度过滤。

## 2. discovery 结果

`--summary-json`：

- seeds 10483（gtirb-ddisasm 9224 / eh-frame 8147 / elf-relocation-code 4397 /
  dt-init-array 10 / dt-init、dt-fini、elf-entry 各 1），confirmed 7257。
- 未确认 3226：1967 个 eh-frame（high，全部是假/过短 FDE 入口）、1259 个
  elf-relocation-code（low）。
- 228405 个 basic block、745342 条指令；xrefs：call 139733、flow 13954、data 3817。
- unresolved indirect flows 2403，全部是 indirect call，无 indirect branch。
- 耗时约 3m50s/次（gexx64 规模）。

## 3. lowering 结果

`--all-confirmed --register-ssa-summary`：约 7m30s，exit 0，产物 87MB `.ll` /
20.5MB `.bc`；`llvm-as` 与 `opt -passes=verify` 均通过。

- 7251 个函数定义、648 个声明；IR 里 1.34M 条语句（asm 809k 条指令，约 1.66x）。
- 3 个函数被整函数跳过（见 4.1），所以 7257 confirmed → 7251 defined。
- 1545 个函数里共 3017 个间接调用点；429 个函数里共 694 处 `notdec.unknown`。
- `notdec.partial_read` 1511 处；裸 `load i64, ptr @RSP` 26 处。
- pass 自报：
  `local_register_reads=2678 boundary_register_reads=204 partial_write_helpers=0
  clobber_placeholders=11567 unknowns=694
  call_arg_rewrite_missing_binding=19 call_arg_rewrite_type_mismatch=1
  range_return_rewrite_missing_value=217 return_binding_uses_clobber_value=457`。

## 4. 问题清单（按影响排序）

### 4.1 3 个函数因 “branch target missing a native block” 被跳过

```
skip native function 0x206d20: native branch target 0x207484 is missing a native block
skip native function 0x321fe0: native branch target 0x321f64 is missing a native block
skip native function 0x33f310: native branch target 0x33f6bc is missing a native block
```

- 三个目标地址都落在 discovery 已有 block 的内部而不是 block 起点：
  - `0x207484` ∈ `[0x207465,0x207488)`（函数 0x206d20）
  - `0x321f64` ∈ `[0x321f47,0x321f68)`（函数 0x321fe0）
  - `0x33f6bc` ∈ `[0x33f6ac,0x33f6c0)`（函数 0x33f310）
- 这三个函数的 `range_start..range_end` 跨度异常大（0x206d20 覆盖
  `0x381e0..0x207c10`，0x33f310 覆盖 `0x43fca..0x340270`），说明 follow 到了
  远处的 cold block（跳转目标 0x381e0、0x43fca 一带）。
- 需要确认：Sleigh 从哪个 block 解出指向 block 内部的直接分支。已在 dev 上加了
  诊断（错误信息带 `from block 0x...`），待复现后补结论。

### 4.2 间接调用没收敛

2403 个 unresolved indirect call；lowering 后仍有 3017 个间接调用点、582 个
`call i64 @notdec.unknown`。这是 C++/虚调用 + 函数指针二进制的主战场，后续需要
按 Bench2 的同一指标体系做统计和收敛。

### 4.3 `ud2` 假函数（含 `_start`）产出的 IR 语义无意义

`_start`（entry 指向 `ud2`）被 lowered 成：

```llvm
%unique = call i64 @notdec.register.userop.invalidInstructionException.i64()
%0 = inttoptr i64 %unique to ptr
%1 = tail call double %0()
...
```

`ud2` 本身是 trap，这里却被当成“返回地址、再间接调用”的语义。需要决定：
对“首块只有 trap”的确认函数，直接按 trap 处理（unreachable/空函数）并记录，
而不是产出这种调用链。注意本二进制里 341 个单 `ud2` 的 ddisasm 函数里只有
`_start` 被确认，所以影响面目前是 1 个函数，但语义是错的。

### 4.4 局部标签名泄漏成函数名

存在 `.L_48a10` 这类函数名（来自 ddisasm 的 local label symbol），功能上能用，
但会干扰按名字选择/对比。需要确认名字解析优先级（优先 `FUN_<addr>`）。

### 4.5 迭代成本

一遍 discovery ≈4min、一遍 lowering ≈7.5min，对“不断修”的循环偏慢。后续要
评估：
- 缓存 discovery 结果 / 提供 `--discovery-in`（当前只有 `--discovery-json` 输出）；
- 或对单函数工作流提供“只解析一次 facts、按函数重跑 lowering”的调试入口。

## 5. P3 根因与修复：CMOV 内部目标落在 nop 填充上

### 5.1 根因

三个失败样本形态完全一致：

| 函数 | 分支目标 | 所在块 | 块后继 | 指令 |
|------|----------|--------|--------|------|
| 0x206d20 | 0x207484 | [0x207465,0x207488) | [0x207488] | `cmovs` @0x207480 后接 `nopl` |
| 0x321fe0 | 0x321f64 | [0x321f47,0x321f68) | [0x321f68] | `cmovl` @0x321f60 后接 `nopl` |
| 0x33f310 | 0x33f6bc | [0x33f6ac,0x33f6c0) | [0x33f6c0] | `cmovl` @0x33f6b9 后接 `nopl` |

SLEIGH 的 x86 `CMOVcc` 是这样实现的：

```
tmp = BOOL_NEGATE(cond)
CBRANCH <指令结束地址>, tmp     ; 条件不成立才跳过写回
dst = src
```

`<指令结束地址>` 通常是下一条指令地址。如果下一条指令是 `nopl`（x86 里 NOP 不发
任何 p-code），这个地址上就没有 p-code op：

- `buildBasicBlocks` 注册内部块时用 `firstOpForAddress.find(target)`，找不到；
- `BlockForAddress` 里没有目标地址对应块；
- lowering 的 `CBranch` 直接目标走到 `blockForNativeTarget()`，报
  `missing a native block`，整个函数被跳过。

（此前在 dev 上加了诊断，错误信息现在会带 `from block 0x...`，正是靠它定位到
0x207478/0x207465 这条 CMOV 链。）

### 5.2 修复

`lib/core/PcodeLowererCfg.cpp` 新增 `resolveOrphanBranchTarget()`：当

1. 目标落在当前 native block 内部（`blockStart < target < blockEnd`），
2. 目标地址没有已注册的 LLVM 块，
3. 目标到块尾之间没有任何 p-code（纯填充），
4. 当前块的成功后继恰好只有一个，且等于块尾，并且该后继有 LLVM 块，

时把目标归一为块尾后继——“继续执行下一条指令”。`lowerTerminator` 的 `Branch`
和 `CBranch` 两条直接目标路径都先过这个归一。不做归一的情形（跨块目标、目标后
还有 p-code、后继不唯一等）保持原来的报错，避免静默改变语义。

新增单测 `testNativeInternalConditionalTargetOnPaddingUsesBlockSuccessor`
（`tests/pcode_to_llvm_test.cpp`）：目标 0x1001 是 0x1000 块内的填充，块尾
0x1002 是唯一后继；断言条件分支有一条边落到 `bb_1002`、仍然保留指令内部 false
边、模块通过 verifier。

### 5.3 验证

- `pcode_to_llvm_test` 通过；`ctest` 19/19 通过（含 fortune x86_64/i386 两个
  realworld 用例）。
- hexx64 全量重跑结果见下一节。

## 6. worktree 与合并流程

- worktree：`/sn640/NotDec-bin2llvm2`，分支 `dev`（从 main `78b8384` 起）。
- 构建复用主 worktree 里 FetchContent 下好的源码，避免重复 clone：

```bash
cmake -S /sn640/NotDec-bin2llvm2 -B /sn640/NotDec-bin2llvm2/build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON \
  -DNOTDEC_BIN2LLVM_SLEIGH_SOURCE_DIR=/sn640/NotDec/external/NotDec-bin2llvm/build/_deps/sleigh-src \
  -DNOTDEC_BIN2LLVM_LIEF_SOURCE_DIR=/sn640/NotDec/external/NotDec-bin2llvm/build/_deps/lief-src
```

- 合并：`git rebase main` → 重建 + `ctest` → `git push origin dev:main` →
  主 worktree `git merge --ff-only dev`。P3 的修复就是按这个流程进 main 的。

## 7. rebase 后的最终验证

rebase 到 main 的新 commit `e858f43`（稳定 GTIRB 导入顺序并清理死 notdec 占位）
之后重跑：

- `ctest`：19/19 通过（含 fortune x86_64/i386）。
- hexx64.so 全量 lowering：`skip native function` 0 条，函数定义 7257 个
  （修复前 7251），`FUN_206d20` / `FUN_321fe0` / `FUN_33f310` 都在产物里；
  `llvm-as` + `opt -passes=verify` 通过。
- 修复后统计：`unknowns=691`、`clobber_placeholders=11019`、
  `local_register_reads=2680`、`boundary_register_reads=204`。
