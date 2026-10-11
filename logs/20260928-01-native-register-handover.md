# native 寄存器值来源 / 签名闭合 交接（2026-09-28）

接续 `logs/20260927-02-native-register-value-source-plan.md`（§15–§21 是本轮全部过程记录），
和 `logs/20260927-01-native-return-entry-param-plan.md`（返回槽入口判据的最初推导）。
规则见 `AGENTS.md` 的"寄存器值的表示准则"和 `Arch2.md` 第 13 节。

## 0. 一句话现状

- HEAD = `76b69e5`（chore: 忽略 paper/）；寄存器相关提交：`7b203b7`（每函数 ABI 约定 +
  非 ABI 返回槽）、`27b3671`（转交需求试改记录，已回退）、`bd3c54e`（demand 双向
  worklist）、`fbeb69e`（callee 入口需求）、`445cffb`（调用点值表 + 入口读兜底 +
  返回槽收窄）、`22e45df`（表示准则）。工作区干净，ctest 12/12。
- **当前 HEAD 的产物比上一版差**（见 §3 数字）：lighttpd unknown 26→54、
  tmux 22→27、wolfssl 535→643，local/clobber 都涨。用户明确要求先不回退，先把原因和
  修法讨论清楚（`logs/20260927-02` §21 有完整诊断）。
- 差的直接原因已定位：新加的"通用寄存器返回槽"给**没有真实来源**的值（出口只来自
  不透明外部调用的 clobber）建了返回槽，unknown 从调用点搬到 callee 的 `ret`
  （`return_binding_uses_clobber_value`：lighttpd 10→38、tmux 1→14、wolfssl 14→182），
  同时为了组装这些出口值 clobber 占位/locals 暴涨（wolfssl clobber 2690→11370、
  local 646→1650）。
- 用户已确认：**"撞上外部调用 → 按 ABI 该寄存器是 clobber"是预期行为**，不是 bug；
  要解决的是"这条需求该不该被当成要往外传的值"。

## 1. 当前要实现的目标（用户原话归纳）

寄存器消除后产物要满足（`AGENTS.md` 硬性准则）：

1. **载体（入口形参 / 返回槽 / 调用点实参） > 裸寄存器读 > `notdec.unknown`**；
   禁用 poison/undef/`freeze(poison)`；边界两侧的位要闭合（callee 返回槽里"可能还是
   入口值"的位必须有 callee 入口形参承载；形参要的位必须在每个调用点答得出来）。
2. 每个函数有统一的"是否遵守 ABI"判定（`!notdec.abi` 标记 + `--abi-for-all-functions`
   开关），参数和返回用同一套：遵守 ABI 的函数只用 ABI 寄存器，不遵守的（本地链接 +
   地址没被取）可以用自定义约定，包括把 caller-saved 通用寄存器当返回通道。
3. **需求必须是"真的需要"**：不能因为 partial write 读改写、需求播种的 all-ones
   fallback、或迭代残留，就凭空给一个值建形参/返回槽。

## 2. 已完成（都已提交并有单测覆盖）

- `445cffb` 调用点按实参记 range 值表 + 入口读兜底 + 返回槽按位收窄
  （`CallArgRangeValues`，`NativeRegisterSummarySSA.cpp`）。
- `fbeb69e` 调用点规则补上 callee 的入口需求（`ReadEntry | MayEntry∩live`），
  callee 侧 `ReturnEntryMask = ExitDemand ∩ MayEntry ∩ MayNonEntry` 并导出 metadata
  `return_entry_mask`；形参判据加 `ReturnEntryMask` 闭包。
- `bd3c54e` `runTopDownDemand` 改成双向单调 worklist（callee ExitDemand 变化 →
  重算 caller；CallInputNeeds 变化 → 重算 caller）。
- `7b203b7` ABI 约定机制：`abiConventionFor()`、`!notdec.abi` 标记、
  `--abi-for-all-functions` 开关、`isValueCarryingRegisterName()`（只放承载值的 GPR，
  排除标志位/段基址/x87 窗口/浮点）、审计按同一判定分 local/boundary、单测
  `testInternalCalleeReturnsCallerSavedRegister`。
- 记录：`27b3671`（转交需求 CallInputMask 试改 → 回归 → 已回退，§20）、
  `7b203b7`（ABI 约定机制的量测与诊断，§21）。

## 3. 量测基线与现状（命令见 §5）

`local/boundary/clobber/unknowns`：

| 目标 | 基线 after8（= `bd3c54e` 状态） | 现在 HEAD（after11） | 对照 `--abi-for-all-functions` |
|---|---|---|---|
| vsftpd | 7/6/23/0 | 7/6/23/0 | — |
| libuv | 4/18/8/8 | 4/18/8/8 | — |
| lighttpd | 117/6/523/26 | 215/19/876/54 | 0/146/-/23 |
| tmux | 312/473/6854/22 | 343/473/7543/27 | 0/781/-/25 |
| wolfssl | 646/905/2690/535 | 1650/897/11370/643 | — |

结论：**机制本身成立**（调用点侧 tmux `call_effect_mixed` 4→0、
`call_arg_rewrite_missing_binding` 11→7），但净效果负面，原因见 §0 与 §21。

## 4. 要解决的问题（按建议顺序）

### P1（下一步，先做）定位"最早那个额外返回值"的需求来源

- 案例：lighttpd `buffer_free`，**基线 after8 就已经**是 `define internal i24 @buffer_free(i64 %RDI.arg)`，
  返回的 i24 就是 RDX 的 bit[8,32)。它的全部内容：
  `%RDX.range_entry1 = partial_read(@RDX, 8)` → `phi(entry, clobber)` → `ret i24`，
  使用者只有 phi 和 ret。
- 调用点也**不用**这个返回值：4 个调用点（`FUN_f2c5`、`network_close`×2、
  `FUN_fc68`、`network_init`）里 `%9 = call i24 @buffer_free(...)` 之后无人使用；
  这 4 个函数体里**一次 `@RDX` 都没有**（facts 也是 `read_entry_mask=0x,
  exit_demand=false`）。
- 但 `buffer_free` 自己的 RDX facts 是 `exit_demand=true, exit_demand_mask=0xFFFFFF00`
  → **需求不是直接调用者传下来的**。怀疑来源（§18 记的假需求）：
  1. `computeValueDemands` 对 partial write 的 `Value`（旧寄存器值）
     `enqueue(partial->Value, fullMaskForValue(...))`（`NativeRegisterSummary.cpp:696`）；
  2. `valueDemand()` 对不在需求表里的值返回 all-ones（`:2951`）；
  3. `ExitDemand` 只并集、从不回收（`mergeDemand`），早期迭代的过近似会永久留下。
- **第一步做法**：给 demand 加"这个寄存器因为哪条来源进的需求"的调试记录
  （调用点规则 / partial write 播种 / ABI 输出种子 / 其它），不改行为，跑 lighttpd
  把 `buffer_free` 的 RDX 需求钉死；再决定改哪一条。若确认是 3，需要让 ExitDemand
  可回收（或改成按迭代快照比较）。

### P2 返回槽判据加"必须有真实来源"

- 现状：返回槽循环只看 `MayNonEntry && ExitDemand`，
  `returnMask = ExitDemandMask ∩ MayNonEntryMask`；而 `MayNonEntry` 同时表示
  "本函数自己写过"和"被调用 clobber 过"（两者都走 `writeRegister`，`:1312`）。
- 判据需要的掩码**已经存在但没导出**：`RegisterOriginBits{Entry, Local, CallClobber}`
  （`:250`）、`setRegisterOrigin`（`:1159`）、store 记 `Local`（`:2065-2069`）、
  clobber 记 `CallClobber`（`:2857`）、merge 按位并（`:1241-1248`）。
- 做法：导出 `origin_local_mask`（可选连 `origin_call_clobber_mask`），返回槽判据改成
  `returnMask = ExitDemandMask ∩ MayNonEntryMask ∩ (LocalMask ∪ EntryMask)`。
  这样 `fdevent_waitpid`（R8 的 `may_entry_mask=0`，出口只来自 `call @waitpid`）
  不会再建返回槽。**注意**：改动后要重量 §3 五个目标。

### P3 假需求收窄（与 P1 同源，P1 定位后一起做）

`logs/20260927-02` §18 的清单：`valueDemand` all-ones fallback、partial write 的
`Value` 整条 seed、寄存器 store 擦整条 live、`seedAbiReturns` 全宽、不透明 callee
`clobberMask` 全宽。修完再复量 §3。

### P4 重开"转交需求"（CallInputMask）

`27b3671`/`§20` 实现过并回退：把"函数自己既不读也不返回、只是把入口值交给下一个调用"
的位（`CallInputNeeds ∩ EntryDemand`）并进形参判据。当时裸读确实降了
（lighttpd local 117→55），但 unknown 爆炸（26→1124，`call_effect_mixed` 1036），
因为 `CallInputMask` 里的"callee 真读的入口位"含大量只为重建 partial write 才读的位
（`array_get_element_klen` 多出 RCX/R8 形参）。P3 修完后可重开复量。

### P5 §11 折叠是否需要重新设计（可选，需专门设计）

"内部函数出口不暴露 clobber helper 占位"这条折叠（`NativeRegisterSummarySSA.cpp:8021-8038`）
把所有路径都是 clobber/unknown 的返回绑定换成显式 unknown（reason
`return_binding_uses_clobber_value`）。按新准则，clobber 是"不透明 callee 的出口"
（I3 的已知来源），是否还算建模缺口 unknown 值得重新讨论；上次删折叠挂了 3 个单测，
要连"出口不暴露 helper"一起设计。

### P6 独立线：栈槽跨块 store 绑定（不属本轮）

tmux 7 处、wolfssl 12 处 `call_arg_rewrite_missing_binding:...:stack+N`；根因是 outgoing
store 在另一个块里、调用点还没绑上。与本轮的寄存器通道无关，单独开线。

### 保留不动的部分（符合准则，不要再当问题修）

- 变参寄存器保存区（tmux 147 个入口读实参）和 callee-saved 寄存器（lighttpd 3、wolfssl 2）
  的入口读：形参化无意义 / 不在参数寄存器集合，属于"裸读 > unknown"的允许项。
- `fdevent_waitpid` 这类"出口值只来自外部调用"的寄存器：本身该是 unknown，
  只是不该为它建返回槽（P2）。

## 5. 环境、命令、脚本

```bash
# 构建 + 单测
make -C build -j10 notdec-native-llvm native_register_summary_ssa_test native_register_summary_test
./build/bin/native_register_summary_ssa_test && ./build/bin/native_register_summary_test
ctest --test-dir build            # 12 个测试

# 量测（脚本在 /tmp/argsrc/run-audit2.sh，支持 NOTDEC_EXTRA_ARGS）
/tmp/argsrc/run-audit2.sh /tmp/argsrc/after12 \
  vsftpd=/sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd \
  libuv=/sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 \
  lighttpd=/sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/lighttpd \
  tmux=/sn640/NotDec-Exp/Bench2/rootfs/usr/bin/tmux
NOTDEC_EXTRA_ARGS=--abi-for-all-functions /tmp/argsrc/run-audit2.sh /tmp/argsrc/after12abi ...
# wolfssl（最慢，750~1000s，单独跑）
/tmp/argsrc/run-audit2.sh /tmp/argsrc/after12 \
  wolfssl=/sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libwolfssl.so.42.0.0

# 验证（唯一允许的 LLVM 工具）
L=/sn640/NotDec/llvm-22.1.0.obj/bin; $L/llvm-as x.ll -o x.bc && $L/opt -passes=verify x.bc -o /dev/null
```

- 现有产物目录：`/tmp/argsrc/after8`（基线）、`after10`/`after11`（ABI 机制两版）、
  `after10abi`（开关对照）。
- 现成分析脚本（`/tmp/argsrc/`）：`mask-partition.py`、`mid-masks.py`、
  `mid-partial.py`、`shape-diff.py`、`rdx-facts.py`、`use-chain.py`、`find-unk.py`、
  `classify-reads.py`（后两个的正则不太可靠，用前先复核输出）。
- 机器是共享的，负载高时耗时能差 8 倍（vsftpd 33s vs 269s），比较时间要在同一负载下。

## 6. 关键代码位置索引

`lib/passes/summary/NativeRegisterSummary.cpp`（summary：按位效果 + 需求）：

| 行 | 内容 |
|---|---|
| 328 | `struct FunctionDemand { ExitDemand, EntryDemand, CallInputNeeds }` |
| ~2900 | `runTopDownDemand`（双向单调 worklist，callee ExitDemand/CallInputNeeds 变化 → 重算 caller） |
| ~3100 | `applyBackwardCallDemand`（调用点规则：callee ReadEntry + 出口透传位 + 转交位） |
| 250/1159/1241 | `RegisterOriginBits{Entry,Local,CallClobber}` / `setRegisterOrigin` / merge |
| 1312 | `writeRegister`（store 与 clobber 共用，只改 MayEntry/MayNonEntry） |
| 696 | partial write 的 `Value` 被 `fullMaskForValue` 整条 seed |
| 2951 | `valueDemand` 的 all-ones fallback |
| 2846 | `applyAbiCallClobbers`（外部调用 → 整寄存器 CallClobber） |
| 3330 | `buildPublicSummary`（导出 `*MaskHex`、`return_entry_mask` 等 metadata） |

`lib/passes/summary/NativeRegisterSummarySSA.cpp`（签名形状 + 改写）：

| 行 | 内容 |
|---|---|
| ~730 | `registerValueSourceIsControlled` / `abiConventionFor` |
| ~1160 | `isValueCarryingRegisterName`（通用寄存器白名单） |
| ~1810 | `addParamForUnit`（形参判据：ReadEntryMask ∪ ReturnEntryMask，External 只允许 ABI 输入） |
| ~1948 | 返回槽循环（`InternalReturnRegisters` / 通用寄存器 + `MayNonEntry && ExitDemand`） |
| 6686 | `entryPartialRead`（`notdec.partial_read` 入口读） |
| 7954 | `callRangeValueHelper`（`notdec.register.summary_clobber.*` 占位） |
| 8021-8038 | §11 折叠（纯 clobber/unknown 的返回绑定 → 显式 unknown） |

## 7. 教训 / 陷阱（别重复踩）

1. `EntryDemand` 当调用点需求是**回归**（§19）：它是入口 live-in，含写保留等垃圾位，
   直接拿去建形参会放大（lighttpd unknown 26→35、tmux 22→76）。
2. `CallInputNeeds` 必须和 `EntryDemand` 求交后才用，但即使这样也仍含 partial write
   的垃圾位（§20）——**源头不干净时，任何形参/返回槽判据都会被污染**。
3. `ExitDemand` 只并集从不回收，早期迭代的过近似会永久留在签名里（P1 要查）。
4. `--abi-for-all-functions` 会改变审计口径（所有函数都算 boundary，local 按定义为 0），
   不要跨模式比较 local/boundary。
5. 表示准则：禁止 poison/undef/freeze；寄存器未知值优先裸读；`notdec.unknown.*` 必须带
   `!notdec.unknown.source` 归因。
6. 单测里"内部函数"要显式用 `InternalLinkage`，否则默认 `ExternalLinkage` 会按 ABI 处理
   （本轮改了两个原有单测，见 `tests/native_register_summary_ssa_test.cpp`）。
