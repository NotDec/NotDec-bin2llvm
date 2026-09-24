# 栈对齐 push 被当成入口读（lighttpd RCX 伪形参级联）

## 0. 用户原始 prompt

> 对，查一下下一个其他问题吧

承接上一轮交接（`logs/20260923-01-native-handover.md`）：P1（多处 va_list 初始化）、P2（`fixed_gp=6`）、
调用点栈上变参、Mixed 分派都已修完，lighttpd 的 `notdec.unknown` 从 97 降到 48。
本轮我提了两个候选：(A) lighttpd 剩余 19 个 `call_effect_mixed` 是否有假 Mixed；
(B) 变参识别的 `fp_offset` 硬编码 48 是否会漏识别"固定参数里有浮点的变参函数"。

## 1. 本轮结论

1. **候选 B 在 benchmark 上没有实例**，不需要改（证据见 §2.1）。
   顺带用 DWARF 把 7 个目标的变参函数定义做了完整核对：工具一个都没漏。
2. **候选 A 方向查到一个具体根因**：GCC 为栈对齐生成的 `push %rcx` / `pop %rdx`（值无关）
   被 base summary 记成 RCX 的入口读，导致 lighttpd 里 3 个函数多出 RCX 伪形参，
   并级联出约 12 个 unknown（48 里的 1/4）。细节见 §2.2。

本轮只做诊断，没有改产品代码；临时加的 getenv 门控打印已回退，工作区干净（HEAD `6e33eed`）。
调试产物在 `/tmp/padding/`（lighttpd 一份完整 IR+log）、`/tmp/dump_*.log`、`/tmp/kind_*.log`、
`/tmp/raw_array_insert_unique.ll`。

## 2. 证据

### 2.1 变参识别完整性（候选 B）

用 DWARF 当 ground truth（只有诊断用，算法不读 DWARF）。方法：从 `benchmark-targets.tsv` 的
debug 文件读 DWARF，列"有 `DW_TAG_unspecified_parameters` 且带 `low_pc`（即本二进制里有定义）"
的函数；再和工具日志的 `Native vararg rewrite: detected=... [名字]` 对比。

| 目标 | DWARF 定义的变参函数 | 工具 detected | 对应关系 |
|---|---|---|---|
| lighttpd | log_debug / log_error / log_perror / log_pri / log_pri_multiline | 5 | 名字完全一致 |
| ffmpeg | update_benchmark（0x3e3b0） | 1（FUN_3e3b0） | 地址一致 |
| wrk | aprintf | 1 | 一致 |
| vsftpd | 无 | 0 | 一致 |
| libuv | uv_loop_configure | 1 | 一致 |
| memcached | logger_log(0x1b320) / append_stat(0x293a0) / restart_set_kv(0x208d0) / vperror(0x6a2d) | 4 | 地址逐一对应 |
| redis-cli | 10 个（含静态函数） | 9 | 唯一差的是 `_serverPanic`（0x2d7e0） |

- redis-cli `_serverPanic`：DWARF 里是变参，但机器码 0x2d7e0-0x2d84f 里**没有 va_start**
  （只有 `endbr64/push %rbp/xor %eax/...` + 3 次 `vfprintf` + `exit`），而且它在 IR 里
  不可达（没有调用者、地址没被取），被最后的 GlobalDCE 删掉。所以不是漏识别。
- 7 个目标的变参函数固定参数**全是整型/指针**，没有一个带 `float/double`，
  因此 `fp_offset = 48 + 16*固定浮点参数个数` 恒等于 48，`NativeVarArgRewrite.cpp:364`
  的 `fp.Value != FpOffsetBase` 硬判据在这些目标上永不触发。旧扫描器报的
  vsftpd `FUN_1c630`、ffmpeg `FUN_3c030` 也确认是普通函数里的 0/48 常量误报。
- 结论：候选 B 不改；等真有"固定参数含浮点的变参函数"的目标时再说。

（核对命令示例：`gdb -batch -ex "set debug-file-directory <rootfs>/usr/lib/debug" -ex "file <bin>" -ex "info functions"`
里带 `...` 的就是变参原型；`info address <name>` 确认是本二进制定义。）

### 2.2 根因：栈对齐 push 被算成入口读

机器码（lighttpd，两处同形）：

```
array_insert_unique 0x2037f:
  20383: push %rbp
  ...
  20388: mov  %rsi,%rbx
  2038b: push %rcx          <-- 只为 16 字节对齐，值无人用
  2038c: call 202c2
  ...
  203d6: pop  %rdx          <-- 配对的 pop，落到另一个寄存器，之后 RDX 死
configparserFree 0x15126:
  15142: push %rcx
  1514b: call 14f78
  15152: pop  %rdx
```

lift 之后（summary 时刻的 IR，`/tmp/dump_array_insert_unique.log`）：

```llvm
%unique_23c00_87 = load i64, ptr @RCX          ; 入口值
%RSP8 = load i64, ptr @RSP
%2 = add i64 %RSP8, -8
store i64 %2, ptr @RSP                          ; SP 减 8（push 的栈调整）
store i64 %unique_23c00_87, ptr %notdec_stack.native.ptr89   ; push 的值
...
store i64 %unique_51280_8, ptr @RDX             ; pop：写到 RDX
call void %33()                                 ; 间接尾跳（jmp *%rax）
```

工具其实已经有一处专门识别这个形态的代码：`lib/passes/summary/NativeRegisterSummary.cpp`
的 `collectStackAdjustmentSlots`（注释里写明了 GCC `push %rcx`+`pop %rax` 与 LLVM
`X86FrameLowering::BuildStackAdjustment`），三条判据：①store 的值是 caller-saved 寄存器的 load；
②同一个槽的 load 不写回同一个寄存器；③每个 load 只被搬进"搬完就死"的寄存器
（`registerValueDiesAfter`）。命中后 `transferInstruction` 就不把这次 store 算成入口读
（`NativeRegisterSummary.cpp:2061`），load 侧也不挂保存值来源（同文件 1992）。

本处卡在判据③：pop 把值写进 RDX 之后，函数在同一个块里还有一个**间接尾调用**
（`jmp *%rax`，IR 里是 `call void %33()`），`callMayReadRegister` 对
间接/未知调用一律保守返回"会读"，于是判定 RDX 还活 → 整个槽不算 padding → 入口读成立。

临时调试打印（getenv 门控，已回退）证实：

```
padding-slot fn=array_insert_unique off=0 reg=RCX sawLoad=1 accepted=0 relaxed=1 reason=dest-alive dest=RDX
padding-slot fn=configparserFree    off=0 reg=RCX sawLoad=1 accepted=0 relaxed=1 reason=dest-alive dest=RDX
```

级联（DWARF 对照，`gdb ptype`）：

| 函数 | 地址 | DWARF 签名 | 工具 IR 形参 | 判断 |
|---|---|---|---|---|
| array_insert_unique | 0x2037f | `void (array * const, data_unset * const)` | RDI,RSI,**RCX** | RCX 伪 |
| configparserFree | 0x15126 | `void (void *, void (*)(void *))` | RDI,RSI,**RCX** | RCX 伪 |
| configparser_push | 0x19645 | `void (config_t *, data_config *, int)` | RDI,RSI,RDX,**RCX** | RCX 伪（经 array_insert_unique 传播） |
| configparser | — | `void (void *, int, buffer *, config_t *)` | RDI,RSI,RDX,RCX | RCX 是真形参，不动 |
| config_parse.isra.0 | 0x263db | （.isra 原型） | RDI,RSI,RCX | RCX 是真形参（0x26400 `mov %rcx,-0x58(%rbp)`），不动 |

受影响的 unknown（当前 48 个里）：

- `call_arg_rewrite_missing_binding:array_insert_unique:RCX=2`
- `call_arg_rewrite_missing_binding:FUN_19645:RCX=1`（= configparser_push）
- `call_effect_mixed:http_header_hkey_get:RCX=3`（configparser 里给 FUN_19645 补第 4 个实参）
- `call_effect_mixed:configparser:RCX=6`（config_parse.isra 调 configparserFree 时要用 RCX，
  值取 configparser 调用之后的 RCX，configparser 的出口 RCX 是 Mixed）

合计约 **12 个 unknown + 3 个伪形参**。

全目标普查（在最终 IR 里找"形参只被 store 进一个从不 load、地址不外逃的帧槽"的函数）：
lighttpd 2 个（就是上面两个），redis-cli/libuv/ffmpeg/vsftpd/memcached/wrk 各 0 个。
影响面小，可以逐个人工核对机器码。

### 2.3 试过但不安全的方案（记下来免得重走）

- **直接放宽判据③的"调用算读"**（调试版实测 `relaxed=1`）：lighttpd 里有上百个候选会被误判成
  padding。反例：`buffer_append_str3` 机器码 `mov %r9,-0x38(%rbp)` … `mov -0x38(%rbp),%rsi`
  之后把 RSI 当第二个实参传给 `memcpy`；`http_header_str_contains_token` 是
  `mov %rax,-0x40(%rbp)` → `mov -0x40(%rbp),%rdx`。这些都是真值搬运，放宽会丢真形参。
  调试打印是行为中立的（同一版跑 lighttpd 仍是 48），只是把"会新接受哪些槽"打出来。
- **不清洗判据，改成 SSA 之后裁剪"只进死槽的形参"**：判断"值有没有被 unknown 调用消费"
  这个不确定性一样存在，而且要在签名/调用点重写之后再动一次，改动更大。

## 3. 最小修改方案（待确认后实施）

在 `collectStackAdjustmentSlots` 里补一个"这条访问是不是正好在当前栈顶"的判据，
**只在确认是 push/pop 对时**才放宽判据③里"调用算读"的部分：

1. **push 判据**：候选 store 的地址（`stackAddressAnalysis(function).addressForPointer(...)`）
   等于该指令处的栈指针地址（`stackPointerBefore(*store)`，两边用
   `entrySlotToNativeFrameKey` 换算到同一坐标），即"正好写在当前栈顶"；
   且紧邻的前一条是 `@RSP = @RSP - 访问宽度` 的 SP 更新。
2. **pop 判据**：该槽的每个 load 也正好在当前栈顶，且紧邻后面是 `@RSP = @RSP + 访问宽度`。
3. 再加上现有判据①（值来自 caller-saved 寄存器的 load）和②（写回不同寄存器）→ 认定为对齐填充；
   此时目的寄存器只被调用消费不算"活"。
4. 其余情况一律走现在的严格 `registerValueDiesAfter`，不放宽。

理由：`push <scratch>` 是编译器为对齐插的固定形态（LLVM `BuildStackAdjustment` 用
`getUndefRegState` 的 undef 值、pop 用 dead-caller-saved 搜索挑寄存器），而普通溢出槽是
`sub $N,%rsp; mov %reg,(%rsp)` 或 RBP 相对槽，SP 变化量与访问宽度、以及"紧邻性"都和 push 不同。
现有三条判据缺的正是"这是不是一条 push 指令"。

- 风险：无帧指针的函数里，如果编译器用 `sub $8,%rsp; mov %rax,(%rsp)` 做临时溢出、之后
  `pop` 到另一个寄存器，形态与 push/pop 相同；这种槽的入口读会被吞掉。
  判断标准：改完后把每个"新接受"的槽逐个人工核对机器码（预期个位数），确认都是
  `push <caller-saved>` / `pop <另一个寄存器>`。
- 更保守的替代：push 判据成立时，只忽略"目的寄存器被**间接/未知调用**消费"这一种读，
  已知 callee 的形参证据仍然算读。

## 4. 判断标准

- `padding-slot` 打印里 lighttpd 新增接受的槽 = 2 个（且逐个人工确认机器码是 push/pop）；
  其他 6 个目标不新增或新增的也都能确认为 push/pop。
- lighttpd `notdec.unknown` 48 → 约 36；redis-cli/libuv/ffmpeg/vsftpd/memcached/wrk 不上升。
- 3 个函数的 IR 签名与 DWARF 一致（list 见 §2.2 表格），`configparser` 的 RCX 保留。
- 7 目标 `llvm-as` + `opt -passes=verify` 全 OK；`ctest` 12/12。
- 量测表（IR bytes/unknown/warnings/verify）记到本文件"实施记录"一节；提交信息说明改动理由。

## 5. 不做什么

- 不动 `registerValueDiesAfter` 的默认行为（实测放宽会丢真形参）。
- 不做 SSA 后的形参裁剪。
- 不动 `configparser` / `config_parse.isra` 的真 RCX 形参。
- 不为 `fp_offset > 48` 加分支（benchmark 无实例）。
- 不顺手重构 `collectStackAdjustmentSlots` 或相邻的栈槽/入口读逻辑。

## 6. 实施记录（2026-09-24）

### 6.1 改动

- `lib/passes/summary/NativeRegisterSummary.cpp`
  - `registerValueDiesAfter` 增加 `ignoreCallReads` 参数（默认 `false`，只有判据 4 传
    `true`）。
  - 新增 `comparableSlotKey`（入口 SP 坐标 ↔ 帧 alloca 坐标换算后比较）、
    `isStackAdjustmentAccess`（访问落在当前栈顶 + 另一侧 SP 调整量 == 访问宽度）、
    `valueSizeInBytes`。
  - `collectStackAdjustmentSlots`：候选里记 `StackTop`（push 侧）；load 侧先走原有严格
    `registerValueDiesAfter`，不成立时若 `StackTop && stackTopLoad` 再用
    `ignoreCallReads=true` 复核；两条都不成立才拒绝。原有接受路径逐字未动。
- `tests/native_register_summary_ssa_test.cpp`：新增
  `testStackAdjustmentPushDoesNotCreateEntryParam`（去掉改动会失败：
  `stack adjustment push created an entry parameter`）。

### 6.2 收紧判据的过程（重要，第一版判错过）

第一版只判"访问是否落在当前栈顶"，redis-cli `FUN_1ba10`（`completionCallback.lto_priv.0`，
DWARF `void (const char *, linenoiseCompletions *)`）被误判：它的
`mov %rsi,-0x50(%rbp)` 正好写在帧底（`sub $0x28,%rsp` 之后的当前栈顶），而该槽在
0x1bb1d 被 `mov -0x50(%rbp),%rdi` 真的读走当实参。后果是 RSI 形参被换成
`%RSIentry_range_rejected` unknown（redis-cli unknown 7 → 8）。加上"SP 调整量 ==
访问宽度"后排除（0x28 ≠ 8），redis-cli 回到 7、该函数签名回到 `RDI,RSI`（与 DWARF 一致）。

### 6.3 量测（`/tmp/fix19`；与 `/tmp/fix17` 同命令同机器；LLVM 22 验证）

| 目标 | unknown fix17 → fix19 | warnings | bytes fix17 → fix19 | verify |
|---|---|---|---|---|
| lighttpd | **48 → 26** | 929 → 880 | 4586461 → 4564150 | OK |
| redis-cli | 7 → 7 | 1332 → 1332 | 4024778 → 4024816 | OK |
| libuv | 8 → 8 | 64 → 64 | 2765033 → 2765025 | OK |
| ffmpeg | 5 → 5 | 2082 → 2082 | 4693493 → 4694057 | OK |
| vsftpd | 0 → 0 | 158 → 158 | 1708019 → 1708023 | OK |
| memcached | 0 → 0 | 755 → 755 | 3802086 → 3802086 | OK |
| wrk | 0 → 0 | 276 → 276 | 929919 → 929927 | OK |

lighttpd 的 19 个 `call_effect_mixed` 和 3 个 `call_arg_rewrite_missing_binding` 全部消失，
只剩 16 个 `entry_range_rejected` + 10 个 `return_binding_uses_clobber_value`（交接里判过
属于保守正确）。warning 分类的增减都落在同一类 `remaining_summary_clobber_value` 里，
没有新类别的告警。

### 6.4 签名对照（`/tmp/sig_diff.py`；只有 lighttpd 6 个函数变化，其余 6 目标零变化）

| 函数 | fix17 → fix19 | DWARF | 机器码证据 |
|---|---|---|---|
| array_insert_unique | RDI,RSI,**RCX** → RDI,RSI | `void (array*, data_unset*)` | 0x2038b `push %rcx` / 0x203d6 `pop %rdx` |
| configparserFree | RDI,RSI,**RCX** → RDI,RSI | `void (void*, void(*)(void*))` | 0x15142 `push %rcx` / 0x15152 `pop %rdx` |
| configparser_push (FUN_19645) | RDI,RSI,RDX,**RCX** → 少 RCX | `void (config_t*, data_config*, int)` | 经 array_insert_unique 传播 |
| gw_proc_init_portpath (FUN_1f08a) | RDI,RSI,**R8** → RDI,RSI | `void (gw_host*, gw_proc*)` | 0x1f099 `push %r8` / 0x1f0ae `pop %rsi` |
| gw_proc_spawn (FUN_20915) | 少 **R8** | `void (gw_host* const, log_error_st* const, const int)` | 经 gw_proc_init_portpath（0x2098b call）传播 |
| gw_host_get (FUN_39c90) | 少 **R8** | `gw_host *(request_st* const, gw_extension*, int, int)` | 同一条 R8 传播链 |

6 个都经 DWARF 签名 + 机器码核对为真修正（`push %r8`/`push %rcx` 的 pop 都落到另一个
寄存器）。

### 6.5 验证清单

- 7 目标 `llvm-as` + `opt -passes=verify` 全 OK（上表 verify 列）。
- `ctest` 12/12（含 fortune x86_64/i386 回归）。
- 新单测有牙：`git stash` 掉 `NativeRegisterSummary.cpp` 后该测试失败，恢复后通过。
- redis-cli 的 `FUN_1ba10` 误判由宽度判据排除（unknown 回到 7）。

### 6.6 残留风险

`push %reg` 与 `sub $8,%rsp; mov %reg,(%rsp)` 的 p-code 完全相同：无帧指针函数里
"预留区正好只有一个 8 字节槽、值被 pop 到别的寄存器、目的寄存器只被未知调用消费"
这种形态仍会被误判。本 benchmark 上已用签名对照 + unknown/warning 计数逐个核对，
没有这种实例；真出现时表现为该函数少一个真形参（签名与 DWARF 不符），
可用 `/tmp/sig_diff.py` 的对照方法发现。
