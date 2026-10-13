# native：GCC 字节 switch 跳转表下标的读宽度收窄（pre-summary peephole）

## 0. 用户原始 prompt（本轮）

> 对 按这个做出来吧，做成一个 low level ir pass，在类似于 summary 这种分析前面，和其他的
> low level pattern 的 pass 放在一起，或者考虑集成进现有的里面？

前置讨论（同一天的对话）在本文件 §1 归纳；P1 的需求来源定位记录见
`logs/20260928-01-native-register-handover.md` 与 `logs/20260927-02-native-register-value-source-plan.md`。

## 1. 背景与目标

### 1.1 现象（已定位，见 §2 证据）

lighttpd 的 `buffer_free` 在基线产物里就多出一个 `i24` 返回值（RDX bit[8,32)），
调用点没人用。需求链（用重放 + provenance 回溯钉死）：

    buffer_free <- FUN_26313 <- configparser <- FUN_263db <-(entryNeed)- FUN_16473
                                                                    ^ 全模块唯一 RDX read_entry=0xFFFFFF00

`FUN_16473`（= lighttpd `config_tokenizer`，configfile.c:2003）在 0x1655a 有一条
`%RDX227 = load i64, ptr @RDX`（提升器把它收窄成 `partial_read.i64.i32(@RDX,0)`），
记成"读了入口 RDX 的 bit[8,32)"，于是调用点被要求准备这 24 位，需求沿调用图反向传播，
把沿途每个可能 clobber RDX 的函数都变成"必须把 RDX 交回来"（lighttpd 108 个函数）。

### 1.2 机器码与编译器出处（ground truth）

    # gcc-13（Ubuntu lighttpd 1.4.74-1ubuntu3）
    1652c: movb   (%r14), %dl            ; c = s[0]，只写一个字节
    16534: cmpb   $0x8, %dl ; jg 1655a
    1655a: leal   -0x9(%rdx), %ecx       ; index = c - 9，32 位
    1655d: cmpb   $0x23, %cl             ; 范围检查只看低字节
    16560: ja     default
    16566: leaq   table(%rip), %rsi
    1656d: movzbl %cl, %ecx              ; 重新按字节扩展
    16570: movslq (%rsi,%rcx,4), %rcx ; jmpq *%rcx

GCC 出处：`gcc/tree-switch-conversion.cc` 聚跳转表 → `gcc/stmt.cc:expand_case` →
`emit_case_dispatch_table` → `try_tablejump`（`gcc/expr.cc:13279`，`index = (index_type)(index - minval)`）
→ `do_tablejump`（`gcc/expr.cc:13205`，`emit_cmp_and_jump_insns(index, range, GTU, …)` +
`convert_to_mode(Pmode, index, 1)` + 表查表跳转）。比较被收窄到字节是 RTL 层利用了
"该值来自字节装载"的已知高位信息。**clang 不生成这个形状**（同源 switch：`movzbl` +
`cmpl` + 表），所以这是 GCC 特有的产物。

### 1.3 目标

在 summary 之前（pre-summary peephole 阶段）识别这个固定形态，把那次"只被低字节观察的
32 位读"改写成 8 位读 + zext，让后续分析看到真实的读宽度：
读 8 位时命中的位已被前面的 `movb` 定义过，不再记成入口读，假需求从源头消失。

## 2. 路线（按仓库"模式匹配"纪律：整块命中或整体跳过）

集成进现有 `NativeRegisterPeephole`（pre-summary，和 `rewriteLoad` 同类：都是"按真正
用到的位收窄寄存器读"），新加一步 `narrowByteSwitchIndexRead`。

**匹配形态（提升器固定舞步）**

    %wide = partial_read.<full>.i32(@R, 0)      ; GCC 的 32 位下标读
    %idx  = add|sub i32 %wide, C                 ; index - lo
    partial_write.i64.i32(@R2, %idx, 0)          ; leal/subl 写回
    %rb   = partial_read.i64.i32(@R2, 0)         ; 提升器写回读
    %zx   = zext i32 %rb to i64
    store i64 %zx, ptr @R2                       ; x86-64 低 32 位写零扩展的物化
    ... 之后到 @R2 下一次 >=32 位写之间 ...

**安全条件（全部成立才改）**

1. `%wide` 只有这一个 user；`%idx` 只有这次写一个 user；
2. 写回读只在 zext 里用，zext 只在整存里用；
3. 从整存之后沿 CFG 到 @R2 下一次 >=32 位写（或整存）之间：@R2 的每次读都必须是 8 位读；
   出现更宽的读（标志位物化）、真实 call（callee 可能把该寄存器当参数读走）、或走到
   `ret`（值逃逸到调用者）都整体跳过；
4. 区间内至少见到一次"8 位读喂给常量比较"（GCC 的 `cmpb` 范围检查），确保命中的是
   switch 跳转表这条编译器形态。

**改写**：`%wide` → `partial_read.i64.i8(@R,0)` + `zext i8 to i32`，其余不动。
低 k 位等价：`(zext8(b) ± C)` 的低字节与 `(c ± C)` 的低字节相同（加法低位不依赖高位），
后续只读低字节。高 24 位变了，但按条件 3 无观察者。

## 3. 验证与判断标准

- 单测：正例（命中 + 收窄 + verify）+ 反例（区间里有宽读 / 有真实 call / `%wide` 多用户
  必须原样跳过）；
- 端到端：5 目标 `llvm-as` + `opt -passes=verify`；`ctest` 12/12；
- 效果：`buffer_free`/`FUN_26313`/`configparser` 的 RDX `exit_demand` 应消失；
  `return_binding_uses_clobber_value` / unknown / 裸读 与 after8 基线、after11 对照；
- 性能：lighttpd 全量耗时同档（共享机器，同负载下比）。

## 4. 风险与不做的事

- 风险：收窄判错会静默改值。故只做"整个区间只被字节观察"的强条件，拿不准就跳过；
- 不做：标志位物化那种真读 32 位（`icmp eq 0`）的收窄；不引入新 metadata；
- 不碰 summary 侧判据（本轮只做 pre-summary 收窄）。


## 5. 实现记录（2026-10-11）

### 5.1 落地位置

| 文件 | 内容 |
|---|---|
| `include/notdec-bin2llvm/passes/summary/NativeRegisterPeephole.h:24-32` | 计数 ByteIndexCandidates/Rewrites/Skipped + FlagStoresSeen/DeadFlagStoresRemoved/DeadFlagValuesRemoved |
| `lib/passes/summary/NativeRegisterPeephole.cpp:1186-1196` | `isRegisterGlobalPointer` |
| 同上 1198-1281 | `byteReadsOnlyUntilNextWrite`：区间检查（只准 8 位读；宽读 / 真实 call / ret 逃逸就跳过） |
| 同上 1283-1383 | `narrowByteObservedWideRead`：匹配 + 改写（`partial_read.i32` → `i8` + `zext`） |
| 同上 1387-1490 | `isArithmeticFlagGlobal` / `flagValueIsRead` / `isRemovableFlagChainNode` / `eraseDeadFlagValue`：死标志位物化消除 |
| 同上 1495-1545 | driver：先死标志位（反复扫到不动，最多 4 轮）→ `rewriteLoad` → 字节值宽读收窄 |
| `tests/native_register_summary_ssa_test.cpp:10790-10930` | 字节值收窄 1 正例 + 4 反例；死标志位 1 正例 + 1 反例；13676 起注册 |
| 本文件 | plan + 记录 |

两条规则的安全条件：

- 字节值宽读收窄：`%wide`/`%idx` 单用户；写回三件套吻合；从整存之后到 @R2 下一次 >=32 位写
  之间只准 8 位读，且低字节确实被读过；区间内不能有真实 call；不能走到 ret。
- 死标志位：flag store 到下一次同 flag 的写、真实调用或函数出口之间没有任何读 → 死；
  连同它现在没人用的计算链删掉（链上还有使用者的节点保留）。只处理 CF/OF/SF/ZF/PF。

顺序很重要：死标志位必须先做，否则 `decl` 那种"写之后还有 3 条 32 位标志位读"会让区间检查失败。

### 5.2 验证

- `native_register_summary_ssa_test` 通过（含新增 5 个用例）；
- `ctest --test-dir build` **12/12 通过**；
- 5 目标 `llvm-as` + `opt -passes=verify` 全部 OK；
- 量测（默认模式，与 after11 同一二进制除本 pass）：

| 目标 | byte_index rewrites/skipped | local/boundary/clobber/unknown | 对照 after11(HEAD) |
|---|---|---|---|
| vsftpd | 0 | 16030 / 55098 | 7/6/23/0 | 同 |
| libuv | 3 | 25120 / 90425 | 4/18/8/8 | 同 |
| lighttpd | 12 | 53546 / 196366 | **211**/19/**869**/54 | local 215→211，clobber 876→869 |
| tmux | 17 | 146489 / 512967 | 343/**474**/**7489**/27 | clobber 7543→7489 |
| wolfssl | 131 | 363781 / 1314477 | 1650/**896**/**11348**/643 | boundary 897→896，clobber 11370→11348 |

- **summary 事实零变化**：lighttpd 442 个函数里 RDX `read_entry` 掩码分布
  （`0x`/全宽/`0xFFFFFFFF`/`0xFFFFFF00` = 259/118/64/1）与 after11 **逐函数完全一致**
  （0 个函数变化）。也就是说：本 pass 命中的 5 处读都不是那些函数入口读的唯一来源。

### 5.3 效果：目标链已断

死标志位 + 字节值收窄之后：

- **FUN_16473 的 RDX `read_entry` 从 `0xFFFFFF00` 变成 `0x`（空）**；
- lighttpd 全模块 RDX `read_entry` 非零函数 183 → 182（只变了 FUN_16473 一个）；
- `buffer_free` 的 RDX `entry_demand`/`exit_demand` 消失，签名从 HEAD 的
  `{ i56, i24, i24 }` 变成 `{ i56, i24 }`（少了 RDX 那一路）。

也就是 §1 里那条"最开始的额外返回值"（RDX/i24）确实被摘掉了，来源正是死标志位物化：

    块 bb_16817（机器码 0x16817，configfile.c 的 --t->in_key）：
      movb 0x19(%rbx), %dl      ; 只写低字节
      decl %edx                 ; 32 位读；结果只写回 DL
      movb %dl, 0x19(%rbx)
    提升器给这条 decl 发了 5 条 32 位读（OF/SF/ZF/PF 物化 + 取值），MayEntry=[8,64) 时
    每条都贡献 bit[8,32)。机器码后续（movq/movb/movl/call）根本不读这些标志 → 4 条是死的，
    删掉之后剩下的取值读被字节值收窄规则吃掉，这个函数的 RDX 入口读就没了。

### 5.4 还没解决的部分（下一步的输入）

`buffer_free` 现在仍然不是 `void`，剩下的 `{ i56, i24 }` 来自 RCX / RSI：

- lighttpd 的 RSI `read_entry` 非零函数 **306 → 306**、RCX **78 → 78**，逐函数完全没变——
  说明这两条链的源头不是死标志位；
- 抽查 RSI 链的叶子（configparser）：`%ESI3241 = partial_read.i32(@RSI,0)` 紧跟
  `partial_write.i32(@RBX, %ESI3241, 0)`，是一条**32 位寄存器搬移**（`mov ebx, esi`），
  不是标志位物化，也不在"只被低字节观察"的形态里；
- 这类读之所以记成入口读，是因为寄存器 global 的 may 分析在路径合并后认为"这些位可能还是
  入口值"（真实路径上它们通常由更早的计算定义过）。形态匹配解决不了这一类，要动的是判据本身。

### 5.5 状态

- 单测（含新增 6 个用例）、`ctest` 12/12、5 目标 `llvm-as`+`verify` 全过；
- 副作用：pre-summary 阶段删掉的死标志位链规模很大（lighttpd 5.3 万 store / 19.6 万指令，
  wolfssl 36 万 store / 131 万指令），产物更小；审计数字小幅改善（lighttpd local 215→211、
  clobber 876→869；tmux clobber 7543→7489；wolfssl clobber 11370→11348），unknowns 不变
  （因为剩下的 RCX/RSI 返回槽还在）；
- 已提交：见 commit message。下一步候选：需求侧闸门（入口读不从 bit 0 开始就不作为调用点需求）
  或按 P2 收紧返回槽判据。

## 6. 一般化：按"观察宽度"收窄寄存器读（commit 2）

原来的匹配只认"32 位读 + 常量加减 + 三件套 + 区间里有常量字节比较"。一般化之后：

1. **锚点**不变：`partial_write.i{writeWidth}(@R2, %v, 0)` + 写回读 / zext / 整存；
   writeWidth 放宽到 8..32。
2. **观察宽度**（从使用出发）：从写走到 @R2 下一次写，收集每次读覆盖的最高位，取最大值 K；
   出现比本次写更宽的观察、整寄存器 load、真实 call、ret 逃逸就整体跳过。
3. **反向 use-def**：从 `%v` 出发，只走低位封闭的运算（加/减/乘/与/或/异或宽度不变、
   常量左移减 c、常量右移加 c、zext/sext/trunc），遇到 `partial_read.iW` 且 K < W 就是收窄
   目标；load 结果、调用结果、参数、除法、比较、变量移位都是边界。
4. **使用者闭包**：被改动过的值，使用者必须都还在这条链上（或就是那次写）。

只收窄读，运算和写保持原类型（收窄写会让寄存器高位不再被覆盖，按位 may 记账里反而
制造更多入口读）。

低位等价的依据：加/减/乘/与/或/异或的结果低 K 位只取决于操作数的低 K 位（进位只往上走）；
左移 c 位取决于低 K-c 位；右移 c 位取决于低 K+c 位。

### 6.1 落地与量测

| 位置 | 内容 |
|---|---|
| `NativeRegisterPeephole.cpp:1168-1250` | 段注释 + `isRegisterGlobalPointer` + `observedWidthUntilNextWrite` |
| 同上 1252-1400 | `isSupportedReadWidth` / `collectNarrowableReads`（反向 use-def，宽度传播） |
| 同上 1402-1530 | `narrowObservedReads`（锚点、观察宽度、闭包检查、改写） |
| `tests/native_register_summary_ssa_test.cpp` | 新增 4 个用例：寄存器搬移收窄 8 位 / 16 位观察 /
  常量右移（8+8=16）/ 除法边界跳过 |

lighttpd（`--all-confirmed --skip-runtime --register-ssa-summary`，1m17s）：

| 指标 | 第 5 节（只做字节 switch） | 本节（一般化） |
|---|---|---|
| 收窄次数 | 12 | **112** |
| local / boundary / clobber / unknown | 211/19/869/54 | **209/19/828/52** |
| `buffer_free` 签名 | `{ i56, i24 }` | **`i56`（RSI 那路消失）** |

`buffer_free` 只剩 RCX 那一路（掩码 `[8,64)`，整寄存器/指针宽度的读），它跟"只观察低位"
无关，属于"按位 may 认为高位可能还是入口值"的问题。

### 6.2 验证

- 单测：新增 4 例 + 原有 5 例字节 index 用例全过；`ctest` 12/12；
- lighttpd 产物 `llvm-as` + `opt -passes=verify` OK。
