# 原始 prompt

```text
对，按照思路都修一下试试
```

（承接上一条：把 `notdec.unknown.*` 换成更合适的表示，并修掉产生 unknown 的几类来源）

# 背景（审计结论）

wrk 最终 IR 有 70 处 `call iN @notdec.unknown.iN()`、5 个 declare，按
`!notdec.unknown.source` 分类：

| 类别 | 数量 | 位置 | 原因 |
| --- | --- | --- | --- |
| `range_return_helper_clobbered_register` | 34 | http_parser_execute 33、FUN_71a0 1 | caller 重建 64 位 RAX:RDX，callee 返回形状只定义 `{i32,i16}` |
| `range_unknown` | 29 | http_parser_parse_url 21、stats_* 6、aprintf 2、FUN_a800 1 | 寄存器范围无定义：jump table 后继块读 ESI 分片、x87 ST0、void 函数被重写成返回值 |
| `call_arg_rewrite_missing_binding` | 1 | script_create | lua_getfield 多出来的第 4 个实参 |
| x87 `i80` | 7 | stats_* | ST0 窗口值不可知 |

# 目标

1. 表示形式：`call iN @notdec.unknown.iN()` -> `freeze iN poison`，
   `!notdec.unknown.source` 挂到 freeze 指令上。
2. 返回形状：caller 对 callee 未定义位的读取用 freeze 表达，不再当成“需要
   callee 提供返回值”的 demand。
3. jump table / switch 后继块的寄存器范围传播：解析得出的值应沿间接分支边流入
   后继块，而不是每个后继重新当 entry 读 unknown。
4. `void` 函数不因为 caller 读 RAX 就被重写成返回 i64；caller 那次读取用 freeze。
5. arity 回退产生的多余实参不再补 unknown（能删就删，删不了也只用 freeze）。

# 设计

## 1. freeze 表示（试过，**否决**）

试验：把 `unknownValueAt()` 改成 `freeze iN poison`（SSA 和 PcodeToLLVM 两处），
wrk 跑通、warning/residue/define 数都不变，但 IR 里只剩 **2 条 freeze**，
70 个 unknown 标记几乎全没了。

原因（用 `opt -passes=instcombine` 复现）：

```llvm
; 输入
define i32 @f(i1 %c) {
  %x = freeze i32 poison
  %y = call i32 @nd_unknown.i32()
  br i1 %c, label %a, label %b
a: %s = add i32 %x, %y
   ret i32 %s
b: ret i32 %x
}
; opt -passes=instcombine 输出
a: ret i32 %y        ; %x 被细化成 0
b: ret i32 0
```

`freeze` 的语义是“任意但固定”，LLVM 可以把未受限的 freeze 细化成任意具体值
（这里选 0）。这会把“未知寄存器值”变成“确定值 0”，并让 `!notdec.unknown.source`
随指令一起消失——比现状更糟。**不透明调用是唯一不会被优化器折叠的表示**，所以保留
call，只在两处 `unknownValueAt()` 加了注释说明为什么不能用 freeze。

## 2. 返回形状

`range_return_helper_clobbered_register` 现在表达的是“寄存器里 caller 读的位
callee 没定义”。这本身就是真实的未定义读，用 freeze 表示即可；另外检查
`rewriteCallReturns` 一侧的重建，确保只在 callee 返回形状定义的位上做
`extractvalue`，未定义的位直接用 freeze，而不是先 demand 一个宽返回槽。

## 3. jump table 传播

`http_parser_parse_url` 的 21 处是 `BRANCHIND` 降低成 `switch` 后，各 case
块把 ESI 分片当 entry 重新读 unknown。要看 `NativeRegisterSummarySSA` 里
`native_indirect_*` 后继块的 range entry 绑定：如果块有唯一/可枚举前驱，范围值
应从 switch 之前的 SSA 值 phi 进来，而不是新读 unknown。

## 4. void 返回

`FUN_a800` 原函数是 `void`，return 点没有 RAX 定义；现在因为 caller 读 RAX 被
重写成 `i64` 返回 unknown。目标：callee 保持 void（或返回类型不被无依据地加宽），
caller 对 RAX 的读用 freeze。

## 5. arity 回退实参

`call_arg_rewrite_missing_binding` 只出现 1 次（lua_getfield 第 4 个实参）。
如果 rewrite 时没有绑定值，优先删掉该实参；删不掉（声明已经是 4 参）时用 freeze，
不再造 unknown 调用。

# 判断标准

| 项 | 期望 |
| --- | --- |
| wrk `@notdec.unknown.*` 调用 | 0（declare 也应为 0） |
| wrk `freeze i\* poison` | 出现对应数量，带 `!notdec.unknown.source` |
| wrk parse_url 的 `range_unknown` | 显著减少（目标 0） |
| wrk FUN_a800 | 不再是 `ret unknown`（void 或 freeze） |
| wrk warning TSV | 0 diff |
| wrk residue / RSP.entry | 不变 |
| define 数 / ptrtoint(@function) | 不低于当前 82 / 19 |
| fortune x86_64 / i386 | 不变 |
| ctest | 12/12 |
| 性能 | 与当前 ~83s 同量级 |

# 风险

- freeze 会让 LLVM 更积极地把未知值当普通值传播（不会引入 UB，但可能改变 IR
  形态）；用 warning/residue/define 数回归判断。
- jump table 传播要动 range SSA 的入口绑定，属于寄存器消除核心逻辑，改不好会
  退回更多 unknown 或产生错误值；先只处理“后继有可枚举前驱”的保守情形。
- void 返回改动会影响签名重写结果，需要看 `functions_rewritten` 与调用点是否
  仍然一致。
