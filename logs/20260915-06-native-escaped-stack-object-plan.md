# 原始 prompt

```text
直接按照B这个做吧，escape的栈对象就不参与dead-store/partial-demand 删除。后面就看看
notdec.unknown的值为什么被生成吧，思考是否应该转换成别的更好的表示形式，而不是用
unknown调用
```

# 背景（审计结论）

审计 wrk 最终 IR 发现：lifting 阶段本来正确地把函数地址写进栈对象，但
`NativeRegisterSummarySSA` 之后的 IR 把这些 store 当死代码删掉了，连带 9 个函数
（`FUN_6cc0`=script_wrk_lookup、`FUN_7100`=script_wrk_connect、`FUN_6450`=handler、
`FUN_6480`=verify_request、`FUN_9990`=header_field、`FUN_99d0`=header_value、
`FUN_9a10`=response_body、`ssl_readable`、`buffer_append`）从模块里消失。

证据：

- wrk 4.1.0 `src/script.c:68-73` 在栈上构造 `table_field fields[]`（含
  `script_wrk_lookup`/`script_wrk_connect`），然后 `set_fields(L, 4, fields)`；
  `main` 构造 `http_parser_settings` 后传给 `http_parser_execute`。
- `--no-register-ssa-pass` 的 IR 里有
  `store i64 ptrtoint (ptr @FUN_6cc0 to i64), ptr %notdec_ram_ptr111`；
  最终 IR 里没有。
- `ptrtoint (ptr @...)` 使用数 28 -> 15，被引用函数目标 20 -> 12，define 87 -> 78。
- pass 计数器 `stack_frame_alloca_stores_removed=246`。

根因在 `lib/passes/summary/NativeStackFrame.cpp` 的
`cleanupStackAllocaAccesses()`：删 store 的条件只有
“函数里没有从**同一个指针值**读的 load”，完全没考虑这个栈对象的地址已经 escape
（例如 `ptrtoint(gep %notdec_stack.native, k)` 当参数传给 callee）。于是
callee 会读到的表内容被删光，最终 IR 语义错误。

# 目标

1. escape 的 `notdec_stack.native` 对象不参与 alloca dead-store 删除。
2. 顺带把同一份 escape 判断用于 partial-demand 侧（按用户要求），确保 escape 对象
   上的值不会因为 “没有本地 demand” 被丢掉。
3. wrk：`ptrtoint(@function)` 使用数、define 数与 no-SSA IR 对齐；warning /
   residue / RSP.entry 不变；fortune、ctest 不变。

# 不做什么

- 不做通用指针 escape 分析，不做 IPA。
- 不改寄存器 liveness / partial-demand 的判定规则本身，只加 “escape 对象豁免”。
- 不恢复 data image、不做 base+offset。

# 设计

## escape 判定（对象级）

对函数里每个 `notdec_stack.native` alloca，扫描所有指令的操作数：

- 操作数是该 alloca 或它的 GEP（`stackAllocaBase()`）时：
  - load/store 的 **pointer operand** -> 正常访问，不算 escape；
  - `getelementptr` 的 pointer base、`bitcast`/`addrspacecast` 的 pointer operand
    -> 对象内部指针运算，不算 escape；
  - 其它一律算 escape：`ptrtoint`、call 参数、store 的 value、return、icmp、
    select/phi……
- 只要对象逃逸，**该对象的所有 store 都不删**（对象级、保守）。alloca 在 native
  链路里通常一个函数一个 frame，所以粒度就是“这个 frame 被通过指针传出去过”。

注意 `PtrToIntInst` 也是 `CastInst`，所以 cast 白名单只能放
`BitCastInst`/`AddrSpaceCastInst`，不能放 `CastInst` 整体，否则
`ptrtoint(gep alloca)` 会被误判成内部运算。

## 落点

`liB/passes/summary/NativeStackFrame.cpp`：

- 新增 `stackAllocaBase(llvm::Value *)`（返回基础 alloca）和
  `collectEscapedStackAllocas(llvm::Function &)`；
- `cleanupStackAllocaAccesses()` 里收集 escaped 集合，store 删除循环跳过
  escaped 对象的 store；并把 `EscapedStackObjects` 计数加进 summary 打印。

partial-demand 侧的豁免：`partial_demand_rejected` 的候选如果落在 escape 栈对象上，
不参与删除；实现时先看这一轮修复后行为，再决定是否需要单独改（wrk 的 9 个函数是
否回来就是判据）。

# 判断标准

| 项 | 期望 |
| --- | --- |
| wrk `ptrtoint (ptr @...)` 使用数 | 回到 ~28（no-SSA 水平） |
| wrk define 数 | 87（no-SSA 水平），9 个函数回来 |
| settings/fields 表的 store | 保留，callee 不再读到未初始化对象 |
| wrk warning TSV | 0 diff |
| wrk residue / RSP.entry | 不变 |
| fortune x86_64 / i386 | 不变 |
| ctest | 12/12 |
| 性能 | wrk 与当前 ~81s 同量级 |

# 风险

- 保守化后保留更多栈 store，IR 可能变大、后续 DSE 负担变重；用 wrk 的 IR 体积和
  耗时对比判断，必要时再细化到 “只有真正被传出去的偏移区间保留”。
- `ptrtoint(gep alloca)` 内部记账（RSP/RBP 重算）也会被判 escape，可能让一些函数
  的 frame 整体不再删 store；这是有意的保守方向，先看指标。


# 实现记录（已完成）

## 改动

`lib/passes/summary/NativeStackFrame.cpp`：

- `stackAllocaBase()`：把一个值归约到它所属的 `notdec_stack.native` alloca。
- `collectEscapedStackAllocas()`：扫描指令操作数，判定对象是否 escape。
  load/store 的 pointer operand、GEP base、bitcast/addrspacecast 属于对象内部访问；
  `ptrtoint`、call 参数、store 的 value、return、icmp、phi/select 等都算 escape。
  注意 `PtrToIntInst` 也是 `CastInst`，所以 cast 白名单只放 bitcast/addrspacecast。
- `cleanupStackAllocaAccesses()`：escape 对象上的 store 不再走 “没有同指针 load 就删”
  的规则，改为保留。

实现中发现并修掉的偏差：

1. 第一版对象级豁免把所有 callee-saved 寄存器 spill 也保住了，
   `RBP/RBX/R12-R15` 的 entry load 变活，residue 从 1 行涨到 109 行。
   于是加 `valueIsRegisterSpill()`：值只由寄存器读、`partial_read`、
   `notdec.register.*`/`notdec.reg.*` 位拼装、`notdec.unknown.*` 和常量构成时算
   spill，仍走旧规则；带函数地址常量、内存 load、参数、真实调用结果的算数据，保留。
2. 第一版 spill 判定漏了 `notdec.reg.insert.*`（前缀是 `notdec.reg.` 不是
   `notdec.register.`），靠临时打印 value 链定位后补上。

## wrk 结果

```text
Native function pointer promotion: seen=54 promoted=20 slots=18 unknown_writes=0 escaped=0 multiple_targets=0
```

| 项 | 修复前 | 修复后 |
| --- | --- | --- |
| define 数 | 78 | 82 |
| `ptrtoint (ptr @...)` 使用数 | 15 | 19 |
| 恢复的函数 | — | FUN_6cc0(script_wrk_lookup)、FUN_7100(script_wrk_connect)、FUN_6450(handler)、FUN_6480(verify_request) |
| residue 行数 | 1（空） | 1（空，保持基线） |
| warning TSV | 0 diff | 0 diff |
| RSP.entry | 0 | 0 |
| IR 体积 | 1,763,215 B | 1,789,446 B（+1.5%） |
| 性能 | 81.4s / 138MB | 83.0s / 138MB |
| LLVM 22 verify | 通过 | 通过 |

fortune x86_64/i386 不变（13/40 define、35/9 warning 行、1/16 residue 行），
ctest 12/12。

## 还没回来的 5 个函数（下一项：notdec.unknown）

`FUN_9990`(header_field)、`FUN_99d0`(header_value)、`FUN_9a10`(response_body)、
`ssl_readable`、`buffer_append` 仍在 IR 里缺失，但根因不在栈 store：

- `--no-summary-register-residue-removal` 下 87 个 define、29 处 ptrtoint 都在，
  说明丢失发生在 residue 阶段；
- 但栈 store 已经保留（`store i128 %notdec.reg.insert.lower1503, ptr %settings`），
  问题是 `main` 里这些地址只写进了 `@ZMM2`
  （`partial_write.i512.i128(ptr @ZMM2, zext(ptrtoint(@FUN_9990)), 0)`），
  而对应的寄存器读被重建成 `notdec.unknown`，地址没有到达 settings store，
  这条 leftover 写随后被合法 DCE 掉。

所以下一步是审计 `notdec.unknown` 的生成原因（ZMM2 的部分写重建、函数返回值缺失、
http_parser 回调返回值），并考虑用更合适的表示替代 unknown 调用。

