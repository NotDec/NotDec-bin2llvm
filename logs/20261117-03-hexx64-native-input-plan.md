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

## 8. P5 修复：`ud2` 按 trap 语义 lower

### 8.1 现象与根因

Sleigh 的 x86 `ud2`（以及 `ud1`）不是一条普通指令，而是：

```
unique = CALLOTHER("invalidInstructionException")   ; userop 77
BRANCHIND (unique)                                  ; 把 userop 返回值当跳转目标
```

旧 lowering 先把 CALLOTHER 落成返回 i64 的占位函数，再走 BRANCHIND 的间接
路径，于是每个 `ud2` 都变成：

```llvm
%u = call i64 @notdec.register.userop.invalidInstructionException.i64()
%p = inttoptr i64 %u to ptr
tail call void %p()
; 继续返回
```

更关键的是：hexx64.so 的 `.text` 开头 16 字节是一组**共享 trap stub**
（0x25a40..0x25a4e），反调试代码会 `if (under_debugger) jne <trap>` 跳过去。
discovery 把这些 stub 块挂到引用它们的函数上，导致 971 个函数的 IR 里都出现
这条“假间接调用 + 继续执行”的路径（旧的 979 个 userop 调用点）。

### 8.2 修复

- `lib/core/PcodeLowererOps.cpp`：`lowerCallOther` 一开头识别
  `invalidInstructionException`（名字匹配，index 77 兜底），直接发
  `call void @llvm.trap()` + `unreachable`，不再走占位 userop。
- `lib/core/PcodeLowererCore.cpp`：op lowering 之后若当前块已经有 terminator，
  立即结束当前块，跳过同一机器块里不可达的后续 P-Code（例如 BRANCHIND）。
- 新增单测 `testInvalidInstructionUserOpLowersToTrap`：构造
  CALLOTHER + BRANCHIND，断言产物里是 `llvm.trap` + `unreachable`、没有间接
  调用、模块通过 verifier。

### 8.3 验证（hexx64.so 全量）

| 指标 | 修复前（native3） | 修复后（native4） |
|------|------------------|------------------|
| `invalidInstructionException` 调用点 | 979 | 0 |
| `llvm.trap` / `unreachable` | 0 / 0 | 1185 / 1207 |
| `tail call` | 3146 | 2174 |
| `clobber_placeholders` | 11019 | 8880 |
| `unknowns` | 691 | 688 |
| `local_register_reads` | 2680 | 2645 |
| `boundary_register_reads` | 204 | 198 |

- `_start` 从“userop + 间接尾调用 + 返回未知寄存器”变成干净的
  `call void @llvm.trap(); unreachable`。
- 函数数保持 7257，0 条 skip；`llvm-as` + `opt -passes=verify` 通过。

## 9. 下一步问题：IR 里的 poison 泄漏（P4）

### 9.1 现象

`hexx64.native4.ll` 里实际值为 `poison` 的操作数共 4462 处（排除
`nocreateundeforpoison` 属性文本）：

| 位置 | 数量 |
|------|------|
| 调用实参 | 4026（2921 个调用点） |
| phi 入边 | 346 |
| 分支条件 | 28 |
| 返回值 | 7 |
| 其它（extract/insert 等） | 55 |

这违反 `AGENTS.md` 的硬性规则（“不要用 poison/undef/freeze(poison) 表示
寄存器值未知”），而且 `br i1 poison`、`ret i32 poison` 会直接把 lifted IR 变成
UB。仓库里既有的设计是**故意**用 poison 暴露“partial demand 漏掉的 observer”
（见 `logs/20260917-03-callee-saved-and-df-plan.md`、
`logs/20260915-08-native-dynamic-shift-demand-plan.md`），所以这些站点是
demand 分析的 bug 报告，应该逐个反推根因，而不是把 poison 一把换成 unknown。

### 9.2 按 stage 归因

用 `--dump-stages` 全模块跑一遍（`/tmp/hexx64/stages_full/`，16 个 stage）：

| stage | poison 数 |
|-------|-----------|
| 000-instcombine-before（lowering 产物） | 0 |
| 000-instcombine-after | 0 |
| 001-register-elimination-before | 0 |
| 001-register-elimination-after | 1816 |
| 005-final-cleanup-before | 1757 |
| 005-final-cleanup-after | 4924 |
| 006-instcombine-after（最终） | 4462 |

结论：

1. lowering 本身不产生 poison（此前的怀疑排除）。
2. register elimination 引入 1816：来自
   `FunctionBuilderCore::rewriteUndemandedRegisterStoreOperands`
   （`!notdec.register.summary_ssa.zero_demand_operand` 机制）和
   summary 覆盖范围读（`readAccessRangeIfDominating`）。
3. final cleanup 又加 3167：`lowerVarArgUnknownHelpers` 把
   `notdec.register.vararg_unknown.*`（3226 个 helper 调用，自带
   `!notdec.unknown.source`）整批换成 poison。这是既有设计（单测
   `testKnownVarArgUnknownPhiTailUsesPoison` 明确断言 poison），改它需要连同
   设计意图一起定（见下）。

### 9.3 已收集的具体案例

- `FUN_1456d0` bb_145ed3：asm 是
  `call get_tinfo_property; mov %rax,%rsi; ...; cltd; idiv %esi; cltq;
  cmp %rax,%rcx; jb ...`，但 register elimination 后只剩
  `%RAX.partial_range.covered = extract.i64.i32(i64 poison, 0)`，后续被
  InstCombine 折成 `br i1 poison`。pre-pass IR 的完整链见
  `/tmp/hexx64/f1456d0_pre.ll:2886-2935`（sdiv/srem、or disjoint、partial_read
  RAX 都在）。
- `FUN_48ff0`：`memmove(dst, poison, n)`——实参来自被下毒的寄存器值。
- `FUN_248840`：`ret i32 poison`，且函数里 `sbbq` 之后的 flag 链与
  `under_debugger` 检查都被折掉。

### 9.4 下一步

1. 先把 `FUN_1456d0` 的 sdiv 链追到底：确认是 `or disjoint` 的 lane demand、
   `zext/trunc` 还是 partial_read covered-range 的哪一步把 RAX 的 demand 归零。
   修法参照 20260915-08（shift 操作数保持全量 demand）的模式，只动那一条传播
   规则。
2. 评估 `vararg_unknown → poison`：它与 AGENTS 的“unknown 用 notdec.unknown”
   规则冲突，且占了 3167/4462。若确认改成 `notdec.unknown.<type>` 不改变现有
   benchmarks 的已知行为，就改表示 + 更新
   `testKnownVarArgUnknownPhiTailUsesPoison`，并在 log 里记录取舍。
3. 每修一类都重跑 fortune x86_64/i386 和 hexx64 全量，对比 poison 计数与
   `unknowns` 不能互相掩盖。

## 10. P4 修复：未解析的 partial_read 不再让写侧提前下毒

### 10.1 根因

`FunctionBuilder::run()` 的第一遍里，`rewritePartialReads()` 先跑、`rewritePartialWrites()`
后跑，但有些读在第一遍解析不掉：它们读的位是由 `notdec.partial_write` 写的，而这个
call 要到 `rewritePartialWrites()` 里才落成 `insert`/`store`。于是 demand 链断在
这个还没替换的读上：

```llvm
%unique_de00_41509 = load i32, ptr ...          ; 栈上值
call void @notdec.partial_write.i64.i32(ptr @RAX, i32 %unique_de00_41509, i64 0)
%EAX1510 = call i32 @notdec.partial_read.i64.i32(ptr @RAX, i64 0)  ; 第一遍解析不掉
%709 = zext i32 %EAX1510 to i64
store i64 %709, ptr @RAX                        ; demand=0 → 被换成 poison
...
%unique_45880_8 = sdiv ...                      ; 真正的观察者在后面
icmp ult → br                                   ; 第二遍解析读以后整条链被折掉
```

`rewritePartialWrites()` 里从 `store` 的 value 往回走，发现 `%709` 零 demand，就按
"undemanded store operand" 的设计换成 poison；第二遍（`PostSignatureCleanup`）
把读替换成 `extract(insert(poison, ...))`，InstCombine 再把比较折成
`br i1 poison`，sdiv 链整个被 DCE 掉。hexx64 的 `FUN_1456d0` 就是这样。

### 10.2 修复

`FunctionBuilder::rewritePartialWrites()` 开头先扫一遍函数里还剩的
`notdec.partial_read`，收集它们对应的寄存器 global；写侧要下毒时，如果目标
寄存器还在这个集合里就跳过（把决定推迟到读解析完的下一遍；如果一直解析不掉，
就保留原值/裸读，而不是 poison）。

定位手段：在 `rewriteUndemandedRegisterStoreOperands` 的替换点临时加
`NOTDEC_TRACE_POISON=<函数名>` 的 stderr 打印（打出被下毒的指令和 operand），
全量跑一遍就能看到调用链；验证完已删除。

### 10.3 验证

hexx64.so 全量（7257 函数、0 skip、`llvm-as` + `opt -passes=verify` 通过）：

| 指标 | 修复前 | 修复后 |
|------|--------|--------|
| poison 总数 | 4462 | 3461 |
| 分支条件 poison | 28 | 10 |
| phi 入边 poison | 346 | 89 |
| return poison | 7 | 5 |
| 调用实参 poison | 4026 | 3339 |
| `unknowns` | 688 | 688 |
| `notdec.partial_read` | 1471 | 1470 |
| `local_register_reads` | 2645 | 2641 |
| `clobber_placeholders` | 8880 | 8902 |

- `FUN_1456d0` 的 `bb_145ed3` 恢复成完整的
  `call get_tinfo_property → shl/ashr（除数）→ sext（被除数）→ sdiv → 比较`，
  `br i1 poison` 消失。
- `ctest` 19/19（含 fortune x86_64/i386）。

### 10.4 剩余 poison

- 3167 个来自 final cleanup 的 `vararg_unknown → poison`（见 9.4，待定表示）。
- 剩下的 RSP 地址链 poison（`%RSP.entry → add -8 → …`）是"栈地址只写不读"的死值，
  属于 zero-demand 的正常结果；要确认它们不会经 phi/调用再回流到观察者。

## 11. P4 修复二：div/rem 的 demand 传播

### 11.1 根因（FUN_1bad80）

128 位 `idiv` 在 p-code 里是 `sdiv/srem i128`，被除数由 RDX:RAX 拼成：

```llvm
%high = shl i128 (zext RDX), 64
%low  = zext RAX
%dividend = or disjoint %high, %low
%quot = sdiv i128 %dividend, %RBP
%rem  = srem i128 %dividend, %RBP
%q64 = trunc %quot to i64   → store @RAX   ; 死值
%r64 = trunc %rem  to i64   → store @RDX
icmp eq %r64, 0 → @ZF → br
```

`trunc` 把 demand 裁到低 64 位，`computePartialDemands` 的 default 传播把这份
low-64 直接塞给 `sdiv/srem` 的操作数；`or disjoint` 再按 lane 拆分，高半段
（`shl`）拿到 0 demand，被 `rewriteUndemandedRegisterStoreOperands` 换成
`shl i128 poison, 64`。这份被除数是商和余数共享的，余数链上的
`icmp → @ZF → br` 随即折成 `br i1 poison`（hexx64 里 7 个函数同类）。

除法/取余没有逐位映射：商/余数的低位同样依赖被除数全部位，demand 必须全量。
这和 20260915-08 修动态移位量是同一类问题。

### 11.2 修复

- `computePartialDemands` 增加 `SDiv/UDiv/SRem/URem`：两个操作数 enqueue 全量
  mask。
- 第 10 节的“跳过未解析读”守卫从**扫描 IR 里剩下的 `partial_read`** 改成在
  `rewritePartialReads()/rewriteLoads()` 里**精确记录没解析掉的读**
  （`UnresolvedReadGlobals`）。扫描版会把 `rewriteLoads()` 新建的 entry read
  也算进去，保护面过大（native6 的 poison 3461 主要是它压下去的），会掩盖
  poison 这个 bug 信号；精确版只在真的解析失败时暂缓下毒。
- 新单测 `testPartialDemandKeepsDivRemOperand`：128 位 `sdiv/srem` 共享被除数、
  商写死值、余数走分支；旧规则下出现 `shl i128 poison, 64`，修复后高半段保留
  （已确认该单测在去掉修复时会失败）。

### 11.3 验证（hexx64.so 全量，7257 函数、0 skip）

| 指标 | 第 10 节后（扫描守卫） | 本节（精确守卫 + div 修复） |
|------|----------------------|---------------------------|
| poison 总数 | 3461（守卫掩盖） | 4347（如实暴露） |
| `br i1 poison` | 10 | **3** |
| phi 入边 poison | 89 | 279 |
| return poison | 5 | 7 |
| `unknowns` | 688 | 688 |

- `FUN_1456d0` 的 `sdiv` 链、`FUN_1bad80` 的 128 位 `srem` 链都保留下来，
  `br i1 poison` 消失。
- `llvm-as` + `opt -passes=verify` 通过；`ctest` 19/19（含 fortune x86_64/i386）。
- 剩下 3 个 `br i1 poison` 在其它函数，留作下一批 demand 缺口。

## 12. 第 4 轮：剩余 3 个 poison 分支的刻画 + vararg 表示试验（回退）

### 12.1 剩余 3 个 `br i1 poison`

| 函数 | 位置 | 特征 |
|------|------|------|
| FUN_19e440 | bb_19e490 | 条件是间接调用返回值上的 `test al,al`；该块 R12/R15 的 range-summary phi 入边是 poison，trace 里全是 RSP 地址链 |
| FUN_20c910 | bb_20ca38 | 带间接调用的循环头；该块所有 range-summary phi 入边都是 poison；`%78 = sub %72,%77`（数组索引地址链）被下毒 |
| FUN_2ca720 | bb_2cadbc | 循环头 phi `%storemerge6347` 的入边被下毒，条件随后折成 poison |

共同点：poison 从“只写不读的地址/循环携带值”经 phi / covered-range 回流到分支条件。
要修需要按 phi 的 demand 传播逐例分析，可能还有 1–2 条独立的 demand 规则缺口，
本轮没有收口。

### 12.2 `vararg_unknown` 表示试验（已回退）

按 AGENTS 的“unknown 统一用 `notdec.unknown.*`”准则，试过把
`lowerVarArgUnknownHelpers` 的 `poison` 换成 `notdec.unknown.<type>` 并复制
`!notdec.unknown.source`：

- 改完后 `native_register_vararg_test` 通过；
- 但 `notdec.native_llvm.realworld_fortune_x86_64` 失败：
  `unexpected text ...: notdec.unknown`。门槛在
  `scripts/native-fortune-x86_64-regression.sh:106`
  （`require_not_contains "notdec.unknown"`）。
- 结论：现状是**故意**让 vararg 恢复不出来的槽走 poison 而不是 unknown——fortune
  这类小二进制要求“零 unknown”。要改表示必须先把这个质量门槛一起改掉（例如先把
  fortune 的变参槽全部恢复出来），不能只替换表示。
- 已回退，IR 与门槛保持原状。

### 12.3 下一步候选

1. 按 phi 的 demand 传播逐例修这 3 个 poison 分支；
2. 或者转向收益面更大的 688 个 unknown：其中 451 个是
   `return_binding_uses_clobber_value`（间接调用返回槽），217 个是
   `range_return_rewrite_missing_value`（外部 callee 的返回槽），与间接调用收敛
   是同一件事。

## 13. 第 5 轮：控制流条件不再允许 poison

### 13.1 动机

上一轮刻画的 3 个 `br i1 poison` 会让**整个函数**成为 UB（LLVM 可以把 poison
细化成任意值），比“值未知”严重得多。它们的根因（phi/covered-range 把死值回流到
条件）还需要逐例分析，但“条件未知”本身可以先用不透明调用表示，符合 AGENTS 的
“unknown 用 `notdec.unknown.*`”准则。

### 13.2 改动

`NativeRegisterFinalCleanup` 新增 `replacePoisonControlFlowConditions(module)`：

- 扫 `br`（条件分支）和 `switch`；条件是 `PoisonValue` 时，在指令前插入
  `call i1 @notdec.unknown.i1()`，挂
  `!notdec.unknown.source`（`reason=poison_control_flow_condition`,
  `register=condition`, `kind=branch`），替换条件。
- 统计进 `PoisonControlFlowConditionsReplaced`；pipeline 的 final-cleanup stage
  在计数非 0 时打印
  `Native final cleanup: poison control-flow conditions replaced=N`。
- **值层面的 poison 不动**，仍作为 demand 分析缺口的信号；只保证控制流不再
  直接依赖 poison。
- 新增单测 `testFinalCleanupReplacesPoisonBranchCondition`。

### 13.3 验证（hexx64.so 全量，7257 函数、0 skip）

- 最终 IR：`br i1 poison` 3 → **0**；出现 3 条
  `!notdec.unknown.source`（reason=poison_control_flow_condition，分别对应
  FUN_19e440 / FUN_20c910 / FUN_2ca720）。
- `llvm-as` + `opt -passes=verify` 通过；`ctest` 19/19（含 fortune x86_64/i386）。
- 已知边界：如果 poison 条件在 final cleanup **之前**就被 InstCombine/SimplifyCFG
  折掉（小函数里很常见），这个兜底看不到它；hexx64 这 3 条都是活到最后的。
  更根本的修法仍是逐例补 phi/covered-range 的 demand 规则。

## 14. 第 6 轮：终结指令兜底扩展到 `ret`，并在最后一次 InstCombine 之后再兜一次

### 14.1 `ret` 返回值同样是 UB 源

hexx64 里除了 3 个 poison 条件，还有 7 个 `ret ... poison`：调用点拿到 poison
返回值同样是 UB。把第 13 节的兜底扩展到 `ret`（`reason=poison_terminator_value`,
`kind=return`，计数 `PoisonReturnValuesReplaced`）。

顺带修了一个 `ret void` 时 `getReturnValue()` 为空、`isa<PoisonValue>(nullptr)`
的崩溃。

### 14.2 时机：跑两次

- **final cleanup（第一次）**：尽早换掉终结指令上的 poison，避免后续
  InstCombine 继续基于 poison 折叠、删代码；
- **erase-dead-helpers（最后一次 InstCombine 之后）**：stage-006 的 InstCombine
  还会把 phi/select 折成 poison——hexx64 里正好有 2 个 `ret poison` 是那时才
  出现的。兜底函数已导出（`replacePoisonTerminatorValues`），这个 stage 里再调
  一次，两个 stage 都会打印
  `poison terminators replaced=N (conditions=…, returns=…)`。
- 新增单测 `testFinalCleanupReplacesPoisonReturnValue`。

### 14.3 验证（hexx64.so 全量）

```
Native final cleanup: poison terminators replaced=9 (conditions=4, returns=5)
Native erase dead helpers: poison terminators replaced=2 (conditions=0, returns=2)
```

- 最终 IR：`br i1 poison` = 0，literal `ret ... poison` = 0；7 条 return 替换都带
  `reason=poison_terminator_value` 归因。
- `llvm-as` + `opt -passes=verify` 通过；`ctest` 19/19（含 fortune x86_64/i386）。
- conditions=4 而 metadata 只有 3 个节点：LLVM 会 unique 字段完全相同的 MDNode
  （同一函数、同一 kind 的重复替换共用一个节点）。

## 15. 第 7 轮：select 的 condition 被下毒导致整个函数体被删

### 15.1 现象

按 asm 指令数 / IR 语句数排序，`FUN_153800`（482 条 asm）在 final IR 里只剩 38 行、
3 个块；discovery 里它有 132 个块。stage 000 有 132 块，register-elimination 之后
只剩 4 块，并多出一条 `llvm.assume(i1 (ret == 0))`——整个函数体（128 个块）被
InstCombine/SimplifyCFG 当死代码删掉了。

### 15.2 根因（两个叠加）

1. `computePartialDemands` 的 `Select` 分支只给两个候选值（operand 1/2）传 demand，
   **没有给 condition（operand 0）传**。select 一旦被 demand（例如它的结果被当作
   load/store 的地址），condition 在 demand 表里仍是零，走到它时被当成死值。
2. `rewriteUndemandedRegisterStoreOperands` 按“零 demand → poison”处理 operand，
   于是 select 的 condition 被换成 `poison`；condition 又是内存地址的一部分，
   `load ptr (inttoptr (select i1 poison, ...))` 变成 UB，InstCombine/SimplifyCFG
   把该块折成 `unreachable`，再把指向它的分支折成 `llvm.assume` + 单边跳转，
   级联删掉整个函数体。

用 `NOTDEC_DUMP_PREINST` 在 register elimination 内部的每次 InstCombine 前 dump
可复现：修 demand 前 `preinst_0` 已有 `select i1 poison`；补上 demand 后
`preinst_0` 干净，但第二遍清理（iteration 1）的 walk 又把新 select 的 condition
毒化（`preinst_1` 出现 `select i1 poison`）。所以两处都需要修。

### 15.3 修复

- `computePartialDemands` 的 `Select` 分支补上
  `enqueue(operand(0), fullMaskFor(...))`：condition 决定选哪个值，结果被使用就
  必须完整。
- `rewriteUndemandedRegisterStoreOperands` 里，`select` 的 condition 子树**保留
  原样、不按零 demand 下毒**：最坏退化成裸寄存器读，符合“载体 > 裸读 > unknown”。
- 新增单测 `testPartialDemandKeepsSelectCondition`（去掉任一修复都会失败：
  “partial demand replaced the select condition with poison”）。

### 15.4 验证

- `FUN_153800` 单函数：register-elimination 后 132 → **98 块**（修前 4 块）、
  `llvm.assume` 0 条（修前 2 条）；最终 IR 96 块 / 1386 行（修前 38 行），
  `llvm-as` + `opt -passes=verify` 通过。
- `ctest` 19/19（含 fortune x86_64/i386）。

### 15.5 全量 hexx64 结果（native17 vs native16）

| 指标 | 修前 | 修后 |
|------|------|------|
| poison 总数 | 4347 | **4215** |
| phi 入边 poison | 279 | **158** |
| `br i1 poison` / literal `ret poison` | 0 / 0 | 0 / 0 |
| final-cleanup 终结指令替换 | 9（条件 4、返回 5） | **0**（不再需要兜底） |
| 产物大小 | 87.3 MB | 87.7 MB（多保住 0.4 MB 代码） |
| 极端坍缩函数（final < 20% × stage000 块数） | 145 | **139** |

- `FUN_153800`、`FUN_15d620` 已从坍缩名单里消失；`llvm-as` + `opt -passes=verify`
  通过，`ctest` 19/19。
- 还剩 139 个极端坍缩函数，其中 21 个在 lowering（stage 000）就已经坍缩——例如
  `FUN_210d80`：discovery 68 块，lowering 只剩 8 块，函数体在 0x3898b 附近被一条
  反调试 `jne .L_389b5` 带偏，主体块全部不可达后被删。这类属于 lowering/CFG
  的问题（不是寄存器消除），留作下一批。
- 调试钩子：`NOTDEC_DUMP_PREINST=<dir>` 会在 register elimination 内部每次
  InstCombine 前 dump 当前 module，定位"哪一遍 InstCombine 删的代码"很方便。

## 16. 第 8 轮：内部 p-code 块的 fallthrough 被接到别的 native 块

### 16.1 现象

`FUN_210d80`：discovery 68 个块，lowering（stage 000）只有 8 个块，最终 IR 只剩
一条 `_Unwind_Resume` 路径。反调试判断 `cmpb $0,(under_debugger); jne .L_389b5`
之后的整段主体全部不可达。

### 16.2 根因

`nativeFallthroughBlock` 对内部 p-code 块（CMOV 等指令内控制流拆出来的块）无条件
取 `nextBlock(blockIndex)`，即 `BlockStarts` 顺序里的下一个块。当这个内部块正好是
所属 native 块的最后一个 p-code 块时，下一个块可能属于完全不同的 native 块——
`FUN_210d80` 的 CMOV 内部块被接到了 `0x3898b` 的冷块上，主体块全部不可达，被
`EliminateUnreachableBlocks` 删掉。

### 16.3 修复

- 内部块先用 `internalPcodeContinuation(blockIndex, BlockEnds[blockIndex]-1)`：
  下一个 op 还在同一个 native 块里就继续走它；
- 否则用 `parentBlockAddressForIndex()` 查所属 native 块的 CFG 后继事实（和普通
  native 块一致），不再盲取 `nextBlock`。

### 16.4 验证

- `FUN_210d80` 单函数：stage 000 **8 → 51 块**，stage 001 **1 → 20 块**；
  最终 IR 155 行，包含 `FUN_29c650` / `FUN_1deb80` / `FUN_2e4e70`（50592/50593/
  50594 三个反调试分支）和 `clear_tinfo_t` 的真实主体，0 条 `llvm.assume`，
  `llvm-as` + `opt verify` 通过。
- hexx64.so 全量：7257 函数、0 skip；按 asm/IR 语句比筛出的“疑似丢代码”函数
  （asm≥40 且 IR<50%）**37 → 33**；verify 通过；`ctest` 19/19（含 fortune）。
- 顺带纠正一个度量口径：**块数坍缩不等于丢代码**。例如 `FUN_8a140` 39 块 → 1 块，
  但它是直线代码 + 36 条无条跳（调用边界拆分），语句全在（156 行），合并是
  正确行为。判断丢代码要看 asm 指令数 / IR 语句数，而不是块数。

## 17. 第 9 轮：变参槽不要毒化"数据流里真实的 0"

### 17.1 现象与证据

全量 IR 里约 4000 个调用实参是 `poison`，其中 3227 个来自
`notdec.register.vararg_unknown.*`（final cleanup 降级成 poison）。在
`005-final-cleanup-before` 的 dump 里按归因统计：**1878 / 3227 是
`vararg_arg_unproven_zero`**，即"IR 能证明这个值就是 0"却被换成了占位。

`shouldPoisonUnprovenVarArg` 的旧规则：`computeKnownBits(value).isZero()` 就换
`vararg_unknown`。但：

- `valueMayDependOnUnknownPlaceholder` 已经把 poison / undef / freeze / 已知占位
  调用排除在外；
- 剩下的 known-zero 是真实数据流（`xorl %edi,%edi`、`movl $0,...`、真实算术）
  证明出来的 0，传 0 是忠实翻译；
- 换成 `vararg_unknown → poison` 既让调用点成为 UB，又丢失
  `!notdec.unknown.source` 归因（poison 带不了 metadata）。

### 17.2 修复

`shouldPoisonUnprovenVarArg` 只保留"依赖 unknown 占位"这一条：真实值（包括真实
的 0）原样传进变参槽。`testKnownVarArgUnknownPhiTailUsesPoison` 仍通过（它的值
依赖 `freeze(poison)`，属于占位）。

### 17.3 验证

- `ctest` 19/19（含 fortune x86_64/i386；fortune 的 `notdec.unknown` 门槛不受
  影响）。
- hexx64 全量（native19 vs native18）：

| 指标 | 修前 | 修后 |
|------|------|------|
| poison 总数 | 4215 | **1418** |
| 调用实参 poison | 4023 | **1230** |
| phi 入边 poison | 158 | 158 |
| `br i1 poison` / literal `ret poison` | 0 / 0 | 0 / 0 |
| `unknowns` | 691 | 702（+11） |
| 产物大小 | 87.73 MB | 87.71 MB |

  调用实参 poison 减少 2793（1878 个是 known-zero 假零，其余是级联：值不再经过
  poison，链上后续槽也不再被毒化）。`llvm-as` + `opt verify` 通过，7257 函数、
  0 skip。

## 18. 第 10 轮：PLT 桩不再当普通函数 lifting

### 18.1 观察

hexx64 的 7257 个函数里有 **1070 个落在 `.plt` / `.plt.got` / `.plt.sec`**（16 字节
一条的链接器跳板；PLT0 被其余 554 条桩 tail call）。这些桩被当成普通函数 lift：

```llvm
%ptr = inttoptr (load GOT-slot)
tail call void %ptr()
ret i64 <unknown>          ; return_binding_uses_clobber_value
```

带出 145 条 `return_binding_uses_clobber_value` unknown。但真实调用点在降低时已经
通过 `ExternalCallTargets` 解析成对导入符号的直接调用，全量 IR 里**没有任何非 .plt
函数调用 .plt 里的函数**（脚本核对过），这些桩在产物里是死代码。

### 18.2 改动

`tools/notdec-native-llvm.cpp` 新增 `isPltStubAddress()`：降低时跳过
`.plt`/`.plt.got`/`.plt.sec` 里的函数，并打印
`skipped PLT stub native functions: N`。`--skip-runtime` 行为不变（这些函数本来就被
它跳过；fortune 两个用例都用 `--skip-runtime`，产物不变）。

### 18.3 验证（hexx64.so 全量）

| 指标 | 修前 | 修后 |
|------|------|------|
| 函数数 | 7257 | **6187**（−1070 桩） |
| `unknowns` | 702 | **559** |
| `return_binding_uses_clobber_value` | 459 | **314** |
| poison 总数 | 1418 | 1399 |
| 调用实参 poison | 1230 | 1204 |
| 产物大小 | 87.71 MB | 87.38 MB |

`llvm-as` + `opt verify` 通过；`ctest` 19/19（含 fortune x86_64/i386）。

## 19. 第 11 轮：调用实参不能是 poison / poison 依赖链

### 19.1 现象

全量 IR 里 **588 个对内部函数（有函数体）的调用带 poison 实参**，例如
`call @FUN_dfd90(i64 poison)`：实参应该是循环计数器，却变成 poison。poison 实参
让调用点成为 UB，而且 poison 带不了 `!notdec.unknown.source` 归因。

### 19.2 修复

`rewriteSignatureShapes` 的固定参数槽在拿到绑定值之后加两道检查：

- **poison/undef 依赖链**：新增 `valueDependsOnPoison()`，递归检查
  poison / undef / `freeze(poison)` / phi 的 poison 入边 / select 的 poison 分支 /
  算术 cast 链。命中的话先回落调用者自己的入口形参（thunk 转发、递归调用转发
  自己形参的场景），回落不到就换成带归因的 `notdec.unknown`（reason=
  `call_arg_uses_poison_value`），绝不让 poison 进实参。
- **类型不符**时也先试调用者入口形参：range SSA 给出的常是窄类型值，而形参
  本身就是要传的值（hexx64 FUN_5d030 的 `call_arg_rewrite_type_mismatch`）。

### 19.3 验证（hexx64.so 全量）

| 指标 | 修前 | 修后 |
|------|------|------|
| poison 总数 | 1399 | **643** |
| 调用实参 poison | 1204 | **470** |
| 内部调用带 poison 实参 | 588 | **170** |
| phi 入边 poison | 164 | 146 |
| `unknowns` | 558 | 740（+182，poison→带归因 unknown） |
| 产物大小 | 87.38 MB | 87.41 MB |

`llvm-as` + `opt verify` 通过；`ctest` 19/19（含 fortune x86_64/i386，fortune 的
`notdec.unknown` 门槛不受影响）。

剩下的 170 个内部调用 poison 实参不是签名重写时带进去的：它们在重写**之后**由
同一遍的 partial-demand walk 毒化了实参的 def 链（下一轮再追）。

## 20. 第 12 轮：变参槽同样不允许 poison

### 20.1 现象

第 19 节修掉了固定参数槽的 poison 实参，剩下的 170 个内部调用 poison 实参大多
落在**变参槽**上（`call i128 (i64, ...) @FUN_4f650(..., i64 poison)`）：这些槽走
`shouldPoisonUnprovenVarArg` → `vararg_unknown` → final cleanup 降级成 poison。

### 20.2 修复

变参槽在进入 `shouldPoisonUnprovenVarArg` 之前先做 poison 依赖检查：
`valueDependsOnPoison(value)` 命中就直接换成带归因的 `notdec.unknown`
（reason=`vararg_arg_uses_poison_value`），并且**优先于** `vararg_unknown` 路径
（否则又会被降级回 poison）。依赖 `notdec.unknown` 占位（非 poison）的槽仍走原
来的 `vararg_unknown` 路径——那条路径是 fortune x86_64 零-unknown 门槛的一部分，
不能一起改。

### 20.3 验证（hexx64.so 全量）

| 指标 | 修前 | 修后 |
|------|------|------|
| poison 总数 | 643 | **462** |
| 调用实参 poison | 470 | **289** |
| 内部调用带 poison 实参 | 170 | 168 |
| `unknowns` | 740 | 921（+181，poison→带归因 unknown） |

`llvm-as` + `opt verify` 通过；`ctest` 19/19。

剩余 168 个内部调用 poison 实参在签名重写时**还不是** poison：它们是重写之后由
同一遍的 partial-demand walk 毒化实参的 def 链产生的（下一轮在 walk 侧封住）。

## 21. 第 13 轮：内部 callee 的 poison 实参在 final cleanup 兜底

### 21.1 现象

第 20 节之后仍有 **168 个对内部函数的调用带 poison 实参**，而且它们不是签名重写
时带进去的：poison 是重写之后由寄存器 SSA 的胶水链在后续 InstCombine 里折出来的
（`call @FUN_48b90(i64 %RDI.arg, i64 %12, i64 poison)`）。在 walk 侧加"调用实参
观察者"守卫实测 **0 效果**（native26 数字完全不变，用后即回退）；从胶水链的源头
逐个堵不现实。

### 21.2 修复

`NativeRegisterFinalCleanup` 新增 `replacePoisonCallArguments()`：对**所有 callee
有函数体**的调用，把 literal poison 实参换成带归因的 `notdec.unknown`
（reason=`poison_call_argument`）。final cleanup 与 erase-dead-helpers 两个阶段各
跑一次（后者兜住最后一次 InstCombine 折出来的）。外部/间接 callee 不动：它们的
变参槽 poison 是 fortune x86_64 零-unknown 门槛的既定表示。

### 21.3 验证（hexx64.so 全量，native28）

| 指标 | 修前（native26） | 修后（native28） |
|------|------------------|------------------|
| 内部调用 poison 实参 | 168 | **0** |
| poison 总数（按 token 计） | 458 | **208** |
| 调用实参 poison | 287 | **37**（外部/间接调用） |
| phi 入边 poison | 146 | 146 |
| `unknowns` | 921 | 1171（新增的是带归因占位） |
| 函数数 / verify / ctest | 6187 / 通过 / 19/19 | 同左 |

### 21.4 度量口径教训

新建的占位 SSA 值名字里不能带 `poison` 这个词：`%poison.arg` 会让
`grep -c poison` 的统计反向暴涨（一开始差点误判成回归）。现在统一叫
`%unknown.arg`，统计时按 token `\bpoison\b` 匹配。

## 22. 第 14 轮：phi 的 poison 入边换成带归因 unknown

### 22.1 现象

第 21 节之后还剩 **146 个 phi 入边是 poison**（分布在 94 个 phi 上）。phi 不会被
InstCombine 折掉，poison 会顺着值流到观察点：统计里 2 个 phi 直接喂 `br`、3 个
直接喂 `ret`（`ret i64 %R9.range_summary_ssa`，其中一条入边 poison），其余喂
icmp/store。观察点上的 poison 就是 UB。

### 22.2 修复

`NativeRegisterFinalCleanup` 新增 `replacePoisonPhiIncomings()`：把所有 literal
poison 的 phi 入边换成带归因的 `notdec.unknown`（reason=`poison_phi_incoming`），
占位指令插在**对应前驱块的终止符前**（保证支配该入边；不能插在 phi 所在块里）。
final cleanup 与 erase-dead-helpers 两个阶段各跑一次。

### 22.3 验证（hexx64.so 全量，native29）

| 指标 | 修前（native28） | 修后（native29） |
|------|------------------|------------------|
| poison 总数 | 208 | **46** |
| phi 入边 poison | 146 | **0** |
| 调用实参 poison | 37 | 35（外部/间接） |
| 其它 poison | 25 | 11 |
| `unknowns` | 1171 | 1723（全部带 `!notdec.unknown.source`） |
| 产物大小 | 87.5 MB | 84.0 MB |

unknown 归因分布：`poison_phi_incoming` 462、`return_binding_uses_clobber_value`
314、`range_return_rewrite_missing_value` 162、`call_arg_uses_poison_value` 151、
`vararg_arg_uses_poison_value` 117、`poison_call_argument` 116 等。
`llvm-as` + `opt verify` 通过；`ctest` 19/19（含 fortune x86_64/i386）。

## 23. 第 15 轮：phi 入边值的下毒守卫与 poison 源的排查

### 23.1 排查

第 22 节之后 final IR 里 `poison` 只剩 46 个（33 个外部/间接调用实参 + 10 个
零散），但 final cleanup 仍需替换 450 处 `poison_phi_incoming`，说明寄存器消除阶段
还在产生 phi 入边 poison。逐阶段看 FUN_141fa0 的旧 dump：

- stage 000/001-after：`bb_142058` 里出现
  `%range_segment817 = call i8 @notdec.reg.extract.i64.i8(i64 poison, i64 0)`；
- stage 004 post-rewrite-peephole 之后，peephole 把 `extract(poison)` 降成
  `trunc(poison)`，phi 入边变成 literal `poison`；
- 也就是说 poison 的源头是寄存器消除阶段某次迭代里被下毒的 def，之后被
  `currentSegment` 当成了位段源。

单函数模式（`-f 0x141fa0`）复现不出这个 phi poison（模块上下文里的 callee shape
不同），所以只能靠全量 dump 定位；在 `materializeCoveredAccess`/`currentSegment`
加的"poison 不作为位段源"守卫实测对全量指标 0 效果（450 处不变），已回退。

### 23.2 保留的改动

`rewriteUndemandedRegisterStoreOperands` 增加守卫：要下毒的操作数如果**还被某个
PHINode 当作入边使用**，就保留原值（`isPhiIncomingValue`）。理由：phi 会把值带到
ret/br/store 等观察点，而入边的 demand 往往要在后续遍才确定。

### 23.3 验证

hexx64.so 全量：poison 46 → **43**，`poison_phi_incoming` 的使用数 462 → **450**；
unknown 总数 1668 不变；`llvm-as` + `opt verify` 通过；`ctest` 19/19。

### 23.4 结论

phi 入边 poison 的主源还没钉死（不是 walk 直接毒化 phi 操作数，也不是
materialize/currentSegment 的 poison 源）。下一步要么给全量 dump 加"poison 来源"
追踪（在寄存器消除内部的最终产物上打标），要么把 `notdec.reg.extract(poison)`
在 peephole 之前统一换成带归因 unknown（表示层兜底，和 final cleanup 的三处
兜底同一思路）。

## 24. 第 16 轮：poison 位段源的兜底替换（phi poison 主源找到）

### 24.1 定位

新增调试钩子 `NOTDEC_DUMP_AFTER_REG=<path>`（在 register-elimination stage 结束后
dump 整个 module），全量跑一次得到寄存器消除后的真实状态（139 MB）：

- **literal-poison phi 入边：0 个**；
- `notdec.reg.extract.i64.*(i64 poison, off)`：**749 个**，分布在 151 个函数里；
- 典型形态：`%range_segment336 = call i32 @notdec.reg.extract.i64.i32(i64 poison, i64 0)`
  再被 select 使用。

结论：extract 创建时源还是真实值，是后续迭代的 partial-demand walk 把源下毒；
post-rewrite peephole 把 `extract(poison)` 折成 poison，poison 再顺着 select/phi
流到 ret/br/store。所以第 23 节在 `currentSegment`/`materializeCoveredAccess` 加的
"poison 不作源"守卫没用（创建时源还不是 poison）。

### 24.2 修复

`runNativeRegisterElimination` 末尾（peephole 之前）新增
`replacePoisonRangeExtracts()`：把所有源是 literal poison 的
`notdec.reg.extract.*` 调用替换成带归因的 `notdec.unknown`
（reason=`poison_range_extract_source`），表示层兜底。

### 24.3 验证（hexx64.so 全量，native34）

| 指标 | 修前（native31） | 修后（native34） |
|------|------------------|------------------|
| poison 总数 | 43 | **27** |
| `poison_phi_incoming` 使用数 | 450 | **0** |
| 调用实参 poison | 33 | 21 |
| 其它 poison | 10 | 6 |
| `unknowns` | 1668 | 1810（+142，带归因） |
| 产物大小 | 84 MB | 84 MB |

`llvm-as` + `opt verify` 通过；`ctest` 19/19（含 fortune x86_64/i386）。

### 24.4 剩余 poison

27 个，全部是**外部/间接调用的变参槽**（`pack_dq` / `copy_tinfo_t` /
`clear_tinfo_t` / `qsnprintf` 等导入）加 1 个 `insertvalue`。前者是
`vararg_unknown` 降级路径的既定设计（fortune 零-unknown 门槛依赖它）。

## 25. 第 17 轮：poison 位段源的溯源（未果）与回归测试

### 25.1 溯源尝试

目标：找出 749 个 `notdec.reg.extract(poison)` 的创建者，从源头消除而不只是表示层
兜底。在三个入口加了 env-gated 探针 `NOTDEC_TRACE_POISON_EXTRACT=1`：

- `extractBitsFromInteger`（位段提取调用创建点）：0 次命中；
- `recordReplacement`（值替换点，检查 target 是 poison）：0 次命中；
- 配合 `NOTDEC_DUMP_AFTER_REG` 的 dump：0 个 `store ... poison`。

结论：poison 不是从这两个"值传入/替换"入口进来的。剩下的候选是
`extractBitsFromIntegerValue`（`currentSegment`/`materializeCoveredAccess` 用的另一
个创建点）或签名重写的 `createValueRangeExtract`，也可能是 partial-range 重建时
"没有旧值"用的 poison 基座；本轮没有钉死。探针已全部移除。

### 25.2 交付：回归测试

`testPoisonRangeExtractBecomesAttributedUnknown`：构造
`%seg = call i32 @notdec.reg.extract.i64.i32(i64 poison, i64 0)` 的函数，跑完
`runNativeRegisterElimination` 后断言：

- 模块里不再有"源是 poison"的 `notdec.reg.extract.*` 调用；
- 存在 `reason=poison_range_extract_source` 的带归因 `notdec.unknown`；
- 模块通过 verifier。

已确认：把 `replacePoisonRangeExtracts(module)` 注释掉时该测试失败（poison 源的
extract 残留），恢复后通过。`ctest` 19/19。

## 26. 第 18 轮：位段胶水链的操作数不再按零 demand 下毒

### 26.1 溯源结论

给四个候选创建入口都加了 env-gated 探针（`NOTDEC_TRACE_POISON_EXTRACT=1`）并全量跑：
`extractBitsFromInteger`、`extractBitsFromIntegerValue`、`createValueRangeExtract`、
`recordReplacement`（target=poison）——**全部 0 命中**。结合 dump 里 0 个 poison
store，机制是：

1. partial-demand walk 把某个"位段重建计算"（binop 链）的**操作数**换成 poison；
2. InstCombine 随后把整个 binop 折成 literal poison 常量并 RAUW，于是
   `notdec.reg.extract.i64.i32(i64 poison, 0)` 出现（创建时源还是真实值）。

所以不能只在创建点拦，要在 walk 侧拦住"会喂给位段 helper 的计算"。

### 26.2 修复

`rewriteUndemandedRegisterStoreOperands` 新增守卫：当前指令的值还被
`notdec.reg.extract.*` / `notdec.reg.insert.*` 这类位段胶水 helper 当源使用时，
不下毒它的操作数（`isRangeHelperSource`）。

### 26.3 验证（hexx64.so 全量，native38）

| 指标 | 修前（native34） | 修后（native38） |
|------|------------------|------------------|
| poison 总数 | 27 | **8** |
| 调用实参 poison | 21 | 2 |
| `unknowns`（notdec.unknown 调用数） | 1810 | **1219** |
| `poison_range_extract_source` 使用数 | 149 | 44 |
| 产物大小 | 84.0 MB | 87.8 MB（保留的未删计算） |

`llvm-as` + `opt verify` 通过；`ctest` 19/19（含 fortune x86_64/i386）。

剩下的 8 个 poison：2 个外部/间接调用变参槽 + 6 个 select/insert 类死值。
unknown 降了 591（位段 poison 链断掉后，大量级联 unknown 消失）。
