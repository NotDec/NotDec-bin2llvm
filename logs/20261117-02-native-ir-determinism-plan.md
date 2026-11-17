# native IR 确定性 / 逐字节可复现排查（2026-11-17）

## 0. 用户原始 prompt

> 盘点一下当前开发到什么地步了，之前是使用哪一批二进制在找IR里面的一些问题，比如unknown值的问题的？
>
> 要不先处理一下这个：
> tmux/wolfssl 的 clobber_placeholders 和 IR 字节在同一二进制重复运行时就会浮动（tmux 实测 7640 / 7641 / 7659，IR md5 不同），local/boundary/unknowns 稳定。原因是 ddisasm 每次生成的 GTIRB UUID/节点顺序随机，影响了占位生成的顺序/数量，我们是否需要做到产物逐字节可复现？需要的话值得看一下

## 1. 结论先行

**需要区分两种"可复现"：**

1. **语义/审计可复现**（必须）：同一二进制、同一代码版本，重复跑出来的
   `local/boundary/clobber/unknowns` 计数和最终 IR 的语义内容必须一致。
   这一条本轮已经做到：tmux 同一 GTIRB 连跑三次、两个不同 GTIRB 各跑一次，
   `clobber_placeholders` 都是 **7558**，其余审计项也完全一致。
2. **产物逐字节可复现**（可选）：同一输入连跑两次 `md5` 相同。**目前还做不到**，
   但剩余差异已经从"占位数量/内容漂移"缩小到 5/1280 个函数里**独立纯指令的
   先后顺序**和 LLVM 自动追加的数字后缀。去掉死占位后，两边内容集合完全相同。

因此本轮的处理是：**先消除会改变审计/语义的漂移，不追求纯字节相等**。
字节级相等需要再做一层 canonicalize（见 §6），收益主要是 CI 可以直接 `diff`，
风险是会给所有函数重排纯指令/重命名，暂不默认开启。

## 2. 复现与定位

### 2.1 现象

tmux（`/sn640/NotDec-Exp/Bench2/rootfs/usr/bin/tmux`）用同一份命令重复跑：

| 运行 | clobber_placeholders | local | boundary | unknowns | IR md5 |
|---|---|---|---|---|---|
| 旧 audit run 1 | 7640 | 515 | 612 | 21 | 不同 |
| 旧 audit run 2 | 7641 | 515 | 612 | 21 | 不同 |
| 旧 audit run 3 | 7659 | 515 | 612 | 21 | 不同 |

审计脚本是 `/sn640/bench2-audit/run-audit.sh`，产物在
`/sn640/bench2-audit/rerun/tmux{1,2}.{ll,log}`。

### 2.2 先排除/确认 ddisasm

- `ddisasm` 每次生成的 `.gtirb` 字节不同（UUID 随机），这是事实：
  `/tmp/tmux-a.gtirb` 与 `/tmp/tmux-b.gtirb` 的 md5 不同。
- 但 **用同一个 `.gtirb` 连跑两次，clobber 仍然不同**（实测 7619 / 7650），
  所以 ddisasm UUID 不是唯一原因，甚至不是主要原因。
- 两个不同 GTIRB 跑出来的前置 IR（`000-instcombine-before/after`）是**逐字节相同**的，
  说明 discovery 的大部分结果已经被现有代码规整到地址序；漂移发生在寄存器消除内部。
- 用 `setarch x86_64 -R` 关闭 ASLR 后仍然漂移，说明不是单纯"堆地址变了"，
  而是代码里仍有按指针/创建顺序遍历的容器，或死值清理顺序不同。

### 2.3 漂移的本质

对两次最终 IR 做"给所有 `notdec.*` 声明补 `memory(none) nounwind willreturn`，
再跑 `opt -passes=adce`"的对照实验：

- ADCE 前：tmux 两次差异约 3.5 万行，clobber 数 7619 vs 7650；
- ADCE 后：clobber 数都是 7566，行数都是 344145；
- 再把数字串去掉：内容只差 5 个函数里的纯指令顺序（`lshr`/`trunc` 的先后）。

也就是说，**数量漂移来自没人用的 `notdec.register.summary_clobber.*` /
`notdec.unknown.*` / `notdec.reg.*` 占位链**。它们是有副作用的普通 call，
LLVM 的 DCE/ADCE 不会删；但它们在"生成时"已经消耗了 LLVM 的同名后缀计数，
所以即使后续被 ADCE 掉，最终名字编号也已经漂移。

## 3. 根因

1. **GTIRB 顺序**：`FunctionEntries` 是 `std::map<UUID, ...>`，UUID 由 ddisasm 随机生成；
   `importFunctions` 按 UUID 顺序往 worklist 里塞函数。地址序函数表最终是稳定的，
   但 first-seed-wins 的 range、worklist 顺序、CFG successor 顺序会受影响。
2. **寄存器消除内部仍有按指针/创建顺序的遍历**：
   - `addDemandedExternalReturns` 用 `std::set<CallBase*>` 收集调用点，按指针顺序给
     同一个外部 callee 追加返回槽；
   - `removeDeadPartialReads` 用 `SmallPtrSet` 遍历删除候选；
   - 这些顺序一变，后续 `extractvalue`/`trunc`/`lshr` 的创建顺序和后缀就变。
3. **死占位没有被清**：`summary_clobber` 等 helper 调用没有函数属性，
   普通 call 不会被 DCE 删除，最后留在 IR 里，审计把它们当成了"剩余占位"。

## 4. 本轮改动

### 4.1 discovery：GTIRB 顺序规整

`lib/discovery/NativeGtirbFacts.cpp`

- `importFunctions` 先收集所有函数，再按
  `(Entry, RangeStart, RangeEnd, Name, Blocks.size())` 排序后统一
  `addFunctionSeed` / `addFunction`；不再按 UUID 顺序。
- `codeBlocksByAddress` 遇到同地址重复 CodeBlock 时，明确选择 size 更大的一个，
  不再依赖 `module.code_blocks()` 的 GTIRB 顺序。
- `importCfgEdges` 的 successor 列表排序并去重后再 `addBasicBlockSuccessors`。

验证：`notdec-native-discover --gtirb a/b --functions-json` 两份输出
**逐字节相同**（md5 `61f55576...`）。

### 4.2 寄存器消除：稳定遍历顺序

- `lib/passes/register/RegisterExternalShapes.cpp`
  `addDemandedExternalReturns` 改为按 module/指令顺序收集调用点（先按集合判 membership，
  再沿 module 扫一遍），不再遍历 `std::set<CallBase*>`。
- `lib/passes/register/FunctionBuilderRanges.cpp`
  `removeDeadPartialReads` 的候选改为"记录顺序 + 去重"，不再按 `SmallPtrSet` 遍历。
- `lib/passes/register/RegisterSignatureRewrite.cpp`
  `RangeReturnHelpers` 在重写前按 `(register name, bit offset, bit width)` 排序，
  让同一个调用点的 `ret_range` 提取顺序固定。

### 4.3 死占位清理

`lib/passes/register/NativeRegisterFinalCleanup.cpp` / 头文件 / `NativePipeline.cpp`

- 新增 `eraseDeadNotDecValueHelperCalls`：删除没人用的
  `notdec.register.summary_*` / `notdec.unknown.*` / `notdec.reg.extract.*` /
  `notdec.reg.insert.*` / `notdec.partial_read.*` / `notdec.register.vararg_unknown.*`
  调用，并递归删除因此变死的操作数链。
  - 不包含 `notdec.register.userop.*`：未建模指令可能对应 trap/异常，不能当纯值删。
- `runNativeRegisterFinalCleanup` 在 local DCE 之后调用一次；
  pipeline 的 `erase-dead-helpers` stage 改调这个更完整的清理。
- 实现使用 worklist，避免"每删一个就全模块重扫"的 O(k·n)。

## 5. 验证

### 5.1 确定性

同一份 `/tmp/tmux-a.gtirb` 连跑三次（新二进制）：

| 运行 | clobber | local | boundary | unknowns | missing_binding | return_clobber |
|---|---|---|---|---|---|---|
| 1 | 7558 | 515 | 611 | 21 | 7 | 14 |
| 2 | 7558 | 515 | 611 | 21 | 7 | 14 |
| 3 | 7558 | 515 | 611 | 21 | 7 | 14 |

不同 GTIRB（a/b）各跑一次：clobber 都是 **7558**，其余审计项一致。

wolfssl 用同一份 `/tmp/wolf-a.gtirb` 连跑两次：clobber 都是 **11229**，
local/boundary/unknowns 也完全一致；两次都通过 LLVM 22 verify。

### 5.2 其他目标（与 2026-11-17 system-stack 基线对照）

| 目标 | local | boundary | clobber | unknowns | 说明 |
|---|---|---|---|---|---|
| vsftpd | 7 | 6 | 31 → **0** | 0 | 31 个都是死占位，删后为 0 |
| lighttpd | 224 → **222** | 20 | 821 → **799** | 35 | 2 个死 register read + 22 个死占位 |
| tmux | 515 | 612 → **611** | 7640/7641/7659 → **7558** | 21 | 稳定 |

`vsftpd` / `lighttpd` / `tmux` 产物都通过 LLVM 22
`llvm-as` + `opt -passes=verify`。

### 5.3 测试与性能

- `ctest --test-dir /sn640/NotDec/build-bin2llvm`：**19/19 通过**。
- tmux：旧 33–35s → 新 30.7–32.3s（死代码少了，InstCombine 略快）。
- vsftpd 5.86s、lighttpd 16.03s，与基线同量级。
- wolfssl：同一 GTIRB 两次 77.1s / 80.7s（并行跑，内存约 2.1GB/进程），
  比旧 audit 的 139s 明显快；输出 verify OK。

## 6. 仍未做到：逐字节相等

同一 GTIRB 两次运行，raw `md5` 仍不同。ADCE + 去数字后只剩 **5 个函数**的差异，
全部是独立纯指令的先后顺序，例如：

```llvm
; run A
%31 = lshr i32 %30, 8
%32 = trunc nuw i32 %31 to i24
%33 = trunc i32 %30 to i8

; run B
%31 = trunc i32 %30 to i8
%32 = lshr i32 %30, 8
%33 = trunc nuw i32 %32 to i24
```

两者语义相同，只是 `RangeReturnHelpers` 里 i8/i24 两个 range 的提取先后不同。
wolfssl 同一 GTIRB 两次的 raw diff 约 1.98 万行，ADCE + 去数字后剩 334 行，
也全是 `extractvalue` / 返回槽字段的先后顺序（同一个返回结构体的字段集合相同）。

要彻底字节相等，需要二选一：

1. **继续找剩余的顺序源**：把 `extractReturnRange` / `extractReturnRegister`
   的每次创建都按 `(slot, range)` 缓存并统一按 shape 顺序生成；或者在
   `rewriteSignatureShapes` 末尾把 return-extraction 序列重排。
   风险较小，但需要再逐个 case 验证。
2. **加一个 canonical print**：最终 IR 按函数/块顺序重命名 SSA 值（保留语义前缀，
   去掉历史后缀），并对块内独立纯指令做稳定拓扑排序。能保证字节相等，
   但会改变所有产物的可读顺序，且需要额外验证，不建议默认开启。

**建议**：把"审计计数 + 规范化内容"作为回归门禁；字节级相等等有明确的
`diff` 需求时再做 canonicalizer。当前漂移已经不影响 unknown/clobber 的判断。

## 7. 当前开发盘点（回答 prompt 前半段）

### 7.1 最近完成

- native pipeline 架构重构（`logs/20261113-01-native-architecture-refactor-plan.md`）：
  按职责拆分 `NativeAnalysis` / `PcodeToLLVM` / register passes，测试拆成 5 个可执行文件，
  ctest 16→19。
- `--config` 驱动 pipeline + 函数级 IR override
  （`logs/20261117-01-native-external-config-ir-override-plan.md`）：
  `--print-config` / `--list-stages` / `--dump-stages` / `--manifest-out`，
  旧逐 pass flag 全部删除。
- 寄存器值来源收口（`logs/20260927-02-native-register-value-source-plan.md`）：
  ABI 约定、返回槽/入口形参闭合、双向 demand worklist、非 ABI 返回槽。
- 本轮：IR 确定性/死占位清理。

### 7.2 当前 IR 质量（2026-11-17 system-stack，5 个目标）

| 目标 | local | boundary | clobber（清理后） | unknowns |
|---|---|---|---|---|
| vsftpd | 7 | 6 | 0 | 0 |
| libuv | 5 | 18 | 8 | 8 |
| lighttpd | 222 | 20 | 799 | 35 |
| tmux | 515 | 611 | 7558 | 21 |
| wolfssl | 1681 → **1672** | 913 → **905** | 11528 → **11229** | 583 |

主要剩余问题仍是 wolfssl 的 583 个 unknown（`call_effect_mixed` 375、
`return_binding_uses_clobber_value` 181 等），以及 tmux/lighttpd 的
`call_arg_rewrite_missing_binding` 栈槽绑定。

### 7.3 之前用来找 IR 问题的二进制批次

1. **早期 7 目标集**（`logs/20260923-01-native-handover.md`）：
   lighttpd、redis-cli、libuv、ffmpeg、vsftpd、memcached、wrk。
   用来追 unknown / va_list / call_effect_mixed。
2. **Bench2 debug oracle selected6**（`DEBUG.md`）：
   libuv、vsftpd、memcached、python、vim、wolfssl。用于 coverage gate。
3. **Bench2 limit20**：memcached、lighttpd、openssh。
4. **2026-11-17 system-stack 5 目标**（本轮审计基线）：
   vsftpd、libuv、lighttpd、tmux、wolfssl。数据在
   `/sn640/bench2-audit/20261117-system-stack/` 和
   `20261117-system-metrics.txt`，专门用来看 local/boundary/clobber/unknown。
5. **fortune x86_64/i386**：每次改动后的逐字节回归夹具
   （`scripts/native-fortune-*-regression.sh`）。

所有真实二进制都在 `/sn640/NotDec-Exp/Bench2/rootfs/` 下，清单是
`/sn640/NotDec-Exp/Bench2/manifest/benchmark-targets.tsv`。
