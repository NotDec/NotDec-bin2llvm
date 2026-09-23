# native 路线交接（2026-09-23）

## 0. 一句话现状

- HEAD = `addfa8b`（vararg: accept a prologue that saves a single GP slot (fixed_gp=5)），
  工作区干净，ctest 12/12，7 个目标 IR 全部 `llvm-as` + `opt -passes=verify` OK。
- 当前 `notdec.unknown` 计数（`/tmp/fix13/*.log`）：
  lighttpd **48**、redis-cli 7、libuv 8、ffmpeg 8、vsftpd/memcached/wrk **0**。
  本轮起点（`/tmp/fix12`）是 97 / 7 / 8 / 8 / 0 / 0 / 0；再往前是
  783 / 129 / 16 / 4920 / 5 / 0 / 0。
- 主要未决问题（按优先级）：① 一个函数里多处 va_list 初始化只改写一处（半改写，
  语义 bug，wrk/redis-cli）；② `fixed_gp=6`（GP 全固定、变参只在 XMM/栈）未识别
  （lighttpd `log_pri_multiline`）；③ tmux/ssh summary 阶段疑似超线性（1h 未出 IR）；
  ④ lighttpd 余下 48 个 unknown 的收尾。

## 1. 任务目标与工作方式（用户偏好，务必遵守）

- 目标：为 Bench2 真实二进制生成**语义正确**的 LLVM IR。native 路线 =
  Ghidra Sleigh p-code → LLVM IR，寄存器消除/签名恢复走 `summary` 路
  （`include/notdec-bin2llvm/passes/summary/`、`lib/passes/summary/`）。
- 循环：找下一个值得修的问题 → **先用证据定位**（dump 目标 pass 那一刻的 IR、
  对照机器码/DWARF/LLVM 源码）→ 说明假设、取舍、最小方案 → 改 → 量测 7 个目标
  + ctest + fortune → 写 `logs/` → 提交。
- 硬约束：
  - 最小改动，**不要顺手重构相邻代码**；
  - 识别编译器模式类 pass 遵循"要么完整命中、要么整体放弃"，绝不半改写；
  - IR 只能用 `/sn640/NotDec/llvm-22.1.0.obj/bin` 下的 LLVM 22 工具验证；
  - 行为变化必须量测 7 个目标（IR bytes / unknown / warnings / verify）；
  - 临时调试打印（`getenv` 门控）用完删掉，提交前 `git grep getenv` 复核。

## 2. 环境、命令、产物

```bash
# 构建（增量 ~30s）
make -C build -j16 notdec-native-llvm native_register_summary_ssa_test

# 全目标（7 个）：见 /tmp/fix13b_run.sh，输出 /tmp/fix13/<name>.{ll,log}
build/bin/notdec-native-llvm <bin> --all-confirmed --skip-runtime \
    --register-ssa-summary -o out.ll
# 单函数（注意：单函数跑时其它函数是 declaration，签名/facts 与全量不同，
#         不能用它判断最终签名）
build/bin/notdec-native-llvm <bin> -n <name> --register-ssa-summary -o one.ll
# 原始 lift（看机器码对应形态）
build/bin/notdec-native-llvm <bin> -n <name> --no-register-ssa-pass \
    --no-instcombine-pass -o raw.ll

# 验证（唯一允许的 LLVM 工具）
L=/sn640/NotDec/llvm-22.1.0.obj/bin
$L/llvm-as x.ll -o x.bc && $L/opt -passes=verify x.bc -o /dev/null
ctest                        # 12 个测试，含 fortune x86_64/i386 回归
# unknown 汇总
grep -o 'notdec.unknown remaining: total=[0-9]*.*' x.log
```

- 二进制在 `/sn640/NotDec-Exp/Bench2/rootfs/` 下；清单
  `/sn640/NotDec-Exp/Bench2/manifest/benchmark-targets.tsv`。
- **DWARF 是签名 ground truth**（只用于诊断，算法不能读 DWARF）：
  ```bash
  gdb -batch -ex 'set debug-file-directory /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/debug' \
      -ex 'file /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/lighttpd' \
      -ex 'ptype log_pri' -ex 'info symbol 0x20df0'
  ```
- 本轮产物：`/tmp/fix12` = 修复前基线；`/tmp/fix13` = 当前 HEAD；批量脚本
  `/tmp/fix13b_run.sh`；va_list 半改写扫描 `/tmp/va_scan4.py`；
  tmux 原始日志 `/tmp/tmux_ssa.log`。
- Ghidra 源码在 `/sn640/ghidra`；libc/LLVM 行为对照见
  `docs/analysis/native-compiler-idiom-matching.md`（含"dump 目标 pass 的 IR"套路）。

## 3. 代码地图（改哪里）

| 文件 | 职责 |
|---|---|
| `lib/passes/summary/NativeRegisterSummary.cpp` | base summary：寄存器 value/provenance 数据流、函数 effect（ReadEntry/MayEntry/MayNonEntry）、external callsite evidence、栈槽/保存区tracking |
| `lib/passes/summary/NativeRegisterSummarySSA.cpp` | 范围 SSA、签名形状推断与重写（`buildInitialSignatureShapes`、`refine*`、`rewriteSignatureShapes`）、unknown 归因、变参调用点、userop/intrinsic 命名空间 |
| `lib/passes/summary/NativeVarArgRewrite.cpp` | 变参序言识别与改写；产出 `!notdec.vararg`（`fixed_gp=`/`fixed_fp=`/`save_area=`/`va_list=`/`saved=`） |
| `lib/PcodeToLLVM.cpp` | p-code → IR；`lowerCallOther` / userop 占位（`notdec.register.userop.*`） |
| `lib/NativeRegisterPartial{Read,Write}.cpp`、`RegisterStorage.cpp` | 寄存器读写 helper 与降级 |
| `tests/native_register_summary_ssa_test.cpp` | 主单测（含变参识别用例）；`tests/native_register_summary_test.cpp` |

关键机制（理解这轮修复必需）：`applyFunctionEffect`（base summary）把 callee 的
`ReadEntry` 按 caller 当时的 `MayEntry` 传播回调用者——callee 的 ReadEntry 错一位，
调用者就会凭空"读过入口寄存器"，签名多一个形参，进而在它自己的调用点级联出
`call_effect_mixed` 占位。所以"callee 的入口读事实"必须先对。

## 4. 本轮做了什么（addfa8b）

- 症状：lighttpd 97 个 unknown 里约 50 个是 `call_effect_mixed:*:R9`（30 个 callee）。
- 根因：`log_pri`（DWARF：`void log_pri(log_error_st*, const char*, unsigned, int,
  const char*, ...)`，5 个固定 GP 参数）的变参全在 R9，编译器只保存 R9 一个 GP 槽；
  `NativeVarArgRewrite` 要求保存区 ≥2 槽 → 漏识别 → 保存区读被当固定形参 R9 → 级联。
- 修法：允许单个 GP 保存槽，但要求 `fixed_gp == 5` 且 FP 区 ≥2 个 XMM 保存槽；
  其余印证（gp_offset=8*fixed_gp、fp_offset=48、FP 区 16 字节对齐、AL 测试守 XMM 块）不变。
- 效果：lighttpd 97 → 48（`call_effect_mixed` 70 → 19，R9 全消失），其余目标持平；
  `log_pri` 签名与 DWARF 一致；新单测 `testVarArgPrologueWithSingleGpSaveIsDetected`
  （去掉改动会失败）。
- 细节和量测表见 `logs/20260917-03-native-callee-saved-and-df-plan.md` 最后一节。

## 5. 接下来做什么

### P1（推荐先做）多处 va_list 初始化只改写一处 —— 半改写，语义 bug

- 现象：函数里同一条 va_list 在多条路径上各初始化一次（编译器复制了 va_start 序言），
  plan 只记**最后**一个 `(gp,fp)` 对（`VaListOffset`/`VaListInitBlock` 被覆盖），
  step 5 只删那个块里的字段 store，`applyPlan` 只插一个 `llvm.va_start`。另一条路径的
  手工 va_list 还在，但它的 `reg_save_area` 指向的寄存器保存区已经被删掉。
- 实例：
  - `wrk aprintf`（entry 块手工 va_list 后立刻
    `__vsnprintf_chk(0,0,2,-1,fmt,ap)` 做长度探测；`llvm.va_start` 插在 bb_812f）——
    探测会读未初始化的保存区，长度错 → 后续格式化越界/错误。
  - `redis-cli FUN_20df0` = `clusterManagerLog.lto_priv.0`（同形，`__vfprintf_chk`）。
- 扫描方法：`python3 /tmp/va_scan4.py`（找最终 IR 里仍有成对 `store i32 8k` /
  `store i32 48` 的函数，并区分 half-rewritten（同函数有 `llvm.va_start`）与
  undetected）。注意 vsftpd `FUN_1c630`、ffmpeg `FUN_3c030` 是误报（普通函数里刚好有
  0/48 常量，objdump 已确认）。
- 推荐最小方案：把所有 `(gp,fp)` 初始化点收集成 vector（仍然要求所有点
  `fixed_gp` 一致，不一致沿用现有"保守放弃"）；step 5 按"每个点所在块 +
  `[site.gpOffset, site.gpOffset+24)`"收集字段 store；`applyPlan` 对每个点插
  `llvm.va_start` 并删该点的字段 store；metadata `va_list=` 用第一个点。
- 保守替代（如果不想动 applyPlan）：检测到 >1 个初始化点时整体放弃（保住手工
  va_list 的语义，代价是这些函数退回"变参寄存器当固定参数"，可能带回 unknown）。
- 判据：`va_scan4.py` 的 half-rewritten 为空；wrk/redis-cli unknown 不回升；
  7 目标 verify OK；ctest 12/12；单测覆盖两处初始化的形态。
- **2026-09-23 已修**：识别改成窗口（块+槽）+ 站点（每个 gp_offset store 一次 va_start）
  两级，每个初始化点各还原一个 `llvm.va_start`；第 2 个及以后的窗口要求 gp+16 有
  reg_save_area 的指针 store。`va_scan6.py` 指向 `/tmp/fix14` 时 half-rewritten 全空，
  unknown 48/7/8/8/0/0/0 持平，ctest 12/12。设计依据和量测见
  `logs/20260919-01-native-vararg-recovery-plan.md` 末节。

### P2 `fixed_gp=6`（GP 全固定、变参只在 XMM/栈）

- 实例：lighttpd `log_pri_multiline`（`gp_offset=48`，序言只有 `test %al` + 8 个
  `movaps`，没有 GP 保存槽）。
- 方案：允许零 GP 保存槽，但 base 必须从 va_list 第 4 个字段（`reg_save_area`
  指针 store，通常是 `ptrtoint(GEP(frame, const))`）反推，再用现有 FP 窗口对齐 +
  AL 测试校验；不能只靠"有 XMM 保存"就接受。
- 判据：lighttpd detected +1、该函数签名与 DWARF 一致（`...` 的位置正确）、无回归。

### P3 tmux/ssh summary 阶段超线性

- 证据：tmux 跑 1 小时仍未产出 IR（它的 IR 是 lighttpd 的 3–4 倍，lighttpd 全程
  9 分钟）；perf 显示 90% 时间在 base summary 的 `Analyzer::run`，其中
  `transferBlock` 47.8%、`fixedStackSlot`/`stackAddressAnalysis` 8%、
  `isAnalyzableCall`（每次重新 parse helper 名）3.4%，profile 很分散。
- 可疑点：`solveFunction` 是 round-robin 定点迭代（`while (changed) for each block`），
  每轮把**所有** block 的**所有**指令重跑一遍，大函数可能 O(CFG 深度 × 指令数)。
- 下一步：先量化（临时给 `solveFunction` 加计数：轮数、`transferBlock` 次数/函数，
  或按函数打印耗时），确认是否真超线性；确认后再决定是否换 worklist。
  不要直接上手重构数据结构。
- 相关：`/tmp/sweep/summary.txt` 记录 ssh/tmux/wolfssl 三个目标
  `exit=124 time=1500`（没有 IR）。

### P4 lighttpd 余下 48

- 19 `call_effect_mixed`（RCX/R8 为主：configparser:RCX 6、array_get_element_klen:RCX 3、
  config_remoteip_normalize:RCX 3…）——机制是 callee 出口 provenance 真 Mixed 且调用点
  随后读该寄存器，之前判过大多是保守正确，需要按 callee 逐个看是否有假 Mixed。
- 16 `entry_range_rejected`（RAX 10，其中 6 个是 FUN_f108/FUN_f147 这两个 `log_pri`
  wrapper 的转发值占位，属于可接受交换）。
- 10 `return_binding_uses_clobber_value`（RDX 8 / RAX 2）、3
  `call_arg_rewrite_missing_binding`。redis-cli/libuv 的 clobber-return 之前判过是保守
  正确，不是 bug。

## 6. 已知"不要再试"的清单（避免重复踩坑）

- 调用参数边界不要收窄成"只算本块写过的槽"（省 0.46% IR，但带回 4 个
  `call_arg_rewrite_type_mismatch`）。
- 不要给无证据的间接调用补 ABI 输入兜底（`d377436` 已删掉该逻辑）。
- stack-adjust 判断不要用"所有调用都读寄存器"（会破坏 FUN_17f60），现在是
  per-register `callMayReadRegister`。
- vararg 保存区分组不要再加"同一 alloca"限制（会让 redisAppendCommand 等 3 个退化）。
- 变参单测里 spill 槽不要放进 FP 保存窗口（会触发 step 4 放弃）。
- 单函数 `-n` 跑法的签名/facts 不代表全量结果，别用它下结论。
- 不要用系统 `llvm-as`/`opt`；不要 `pkill -f` 匹配含
  `tmux --all-confirmed` 这种会命中自身命令行的字符串（会杀掉自己的 shell，
  杀后台任务用 PID）。
- 长跑批处理用受管后台任务（`run_in_background`），不要用 `nohup ... &`：
  实测 nohup 批次会在中途被杀（本轮 lighttpd 跑完后批次进程消失，summary 没写出来）。

## 7. 环境残留

- 当前没有 notdec 后台进程（tmux/ssh 的两个长跑已停）。
- `/tmp/fix13/` 有 7 个目标的 `.ll`/`.log`；`/tmp/fix12/` 是修复前基线。
- 单测入口：`build/bin/native_register_summary_ssa_test`、`native_register_summary_test`。
