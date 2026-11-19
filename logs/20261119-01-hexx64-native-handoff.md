# hexx64 native 管线：当前进展与后续计划（交接文档）

> 目标：以 `/sn640/hexx64-dec/hexx64.so` 为固定输入调试 NotDec-bin2llvm native 管线
> （discovery → lowering → 寄存器消除 → 后续 pass），定位并修复问题，保持 fortune
> 用例不回退；并逐步把"用户 override 语义"打磨成可迭代的接口（类比编译：输入 =
> 二进制 + 用户语义，输出 = IR）。

## 1. 环境

- 开发 worktree：`/sn640/NotDec-bin2llvm2`（分支 `dev`），主 worktree：
  `/sn640/NotDec/external/NotDec-bin2llvm`（分支 `main`，已 ff 同步）。
- 构建：`/sn640/NotDec-bin2llvm2/build`（Ninja，RelWithDebInfo）；
  工具：`build/bin/notdec-native-llvm`、`notdec-native-discover`。
- LLVM 22 工具（必须用这套验证）：
  `/sn640/NotDec/llvm-22.1.0.obj/bin/{llvm-as,opt}`。
- 输入 GTIRB：`/tmp/hexx64/hexx64.gtirb`（用 ddisasm 1.9.5 生成，保留着可复用）。
- 全量跑一轮 ~10–11 分钟（discovery ~4 分钟 + 提升）；单函数（`-f <addr>`）~5 分钟。

全量命令模板：

```
cd /tmp/hexx64
/sn640/NotDec-bin2llvm2/build/bin/notdec-native-llvm /sn640/hexx64-dec/hexx64.so \
  --gtirb /tmp/hexx64/hexx64.gtirb --all-confirmed --register-ssa-summary \
  --config /tmp/hexx64/exp/pipeline-pin-qfree-abi.json -o nativeX.ll
```

单函数（带逐 stage dump，调试用）：

```
... --gtirb /tmp/hexx64/hexx64.gtirb -f 0x2e83c0 --dump-stages /tmp/hexx64/stages_x -o f.ll
```

注意：
- 单函数模式函数名是 `notdec_native_<addr>`，选择器用 `{"glob":"*2e83c0*"}` 之类。
- 不要 `pkill -f notdec-native-llvm`（会把自己这条 shell 也杀掉）；用
  `ps -o pid,etime -C notdec-native-llvm`。
- 验证：`llvm-as native.ll -o native.bc && opt -passes=verify native.bc -o /dev/null`。
- 回归：`ctest --test-dir build -j 8`（19 个测试，含 fortune x86_64/i386；
  fortune 门槛禁止产物里出现 `notdec.unknown`、`notdec.register.summary_clobber`）。

## 2. 当前指标（hexx64.so，6187 个函数）

| 配置 | poison | `notdec.unknown` 调用数 | IR 大小 |
|------|--------|--------------------------|---------|
| 默认（native44） | 6 | 728 | 88.1 MB |
| `abi_for_all_functions: true`（native47） | 2 | 712 | 83.2 MB |
| qfree 钉 `RAX:64 + XMM0:128`（native51） | 6 | 662 | 88.2 MB |
| 两者组合（native52） | 2 | **646** | 83.2 MB |

默认配置下 unknown 的归因分布：

| reason | 数量 |
|--------|------|
| `return_binding_uses_clobber_value` | 314 |
| `range_return_rewrite_missing_value` | 157 |
| `poison_call_argument` | 73 |
| `call_arg_rewrite_missing_binding` | 5 |
| `poison_aggregate_field` | 4 |
| `call_effect_mixed` | 2 |

产物都在 `/tmp/hexx64/native*.ll`；配置和 override 文件在 `/tmp/hexx64/exp/`。

## 3. 已修复的主要问题（近期提交，origin/main = 7af55cf）

- `7af55cf` 多寄存器返回声明 + `functions[].signature` 按函数钉死输入/输出寄存器。
- `5d9e4dd` `ValueType::Vector128`（`xmm128` 等）+ `functions[].abi`（按函数调用约定）；
  修"类型化 return 丢掉 fixed_args"和"i128 向量槽被判缺失"两个 bug。
- `441632d` ir-override 克隆带形参函数时段错误（VMap 缺形参映射）+ 回归测试。
- `9ebebef` 调用实参依赖闭包不再按零 demand 下毒（unknown 1223 → 728）。
- `18288a2` 没有返回值用途的间接调用补回实参（+42 个调用点）。
- `68e3fc7` 部分构造聚合的 poison 字段补成带归因 unknown。
- `ad05166` 位段胶水链操作数不再下毒；`7f16847` poison 位段源→unknown；
  `4fe9c93`/`1c5db0d` phi 入边/值下毒守卫。
- 更早：CMOV 孤儿目标、ud2→trap、内部 p-code fallthrough、重新跑 summary 等
  （详见 `logs/20261117-03-hexx64-native-input-plan.md` 第 5–31 节）。

## 4. 用户 override 接口现状

### 4.1 外部 callee 原型（`external_prototypes` JSON）

通过 pipeline 配置挂：

```json
{"stage":"register-elimination","options":{"external_prototypes":"protos.json"}}
```

条目支持：
- `params`: `["i64"]` 或类型化（`i32/i64/ptr/float/double/long_double/xmm128`）；
- `fixed_args`, `vararg`, `noreturn`, `max_args`, `return`（单值类型化）；
- **`returns`（多寄存器，本轮新增）**：

```json
{"qfree": {"params": ["i64"],
           "returns": [{"register":"RAX","width":64},
                       {"register":"XMM0","width":128}]}}
```

### 4.2 按函数钉死签名（pipeline 配置 `functions[]`）

```json
{"functions": [{"select": {"name": "FUN_x"},
                "signature": {"params":  [{"register":"RDI","width":64}],
                              "returns": [{"register":"RAX","width":64},
                                          {"register":"XMM0","width":128}]}}]}
```

寄存器名必须在模块里有对应 global，否则 pipeline 报错；`--print-config` 回显。

### 4.3 调用约定范围

- 全局：`register-elimination.options.abi_for_all_functions: true`；
- 按函数：`functions[]` 里 `{"select": {...}, "abi": "external"|"internal"}`。

### 4.4 整函数 IR override（`ir-override` stage）

```json
{"stage":"ir-override","options":{"file":"fix.ll",
  "select":[{"name":"FUN_2e83c0"}]}}
```

override 文件里定义同名同签名的函数体即可；产物挂 `!notdec.override`。
建议放在 pipeline 末尾（用最终 IR 的签名手写）。已验证可用（`FUN_2e83c0`
补回 4 个实参的用例）。

## 5. 剩余问题与结论（新会话不要重复踩）

### 5.1 `return_binding_uses_clobber_value`（314 → abi_for_all 后 91）

- 机制：函数签名被扩展成"任意 value-carrying GPR 都能当返回槽"，返回槽的值在图上
  是纯 clobber（最后一次内部调用之后的寄存器值）。典型 `FUN_29a670 = jmp qfree`，
  它返回 R8/R9/R10/R11/RCX/RDX/RSI/RDI = qfree 的 scratch。
- **已验证是忠实建模**（FUN_110760 的 R9/R10 返回槽确实被 FUN_d6110/FUN_2921b0
  读取并继续传出；调用点汇编在这条路径上没有先写 R9）。
- 想消掉：用调用约定范围（`abi_for_all_functions` 或 `functions[].abi`），
  314 → 91。**不要**把纯 clobber 返回槽改成 clobber 载体——单测
  `native_register_core_test`（"internal return exposed external clobber helper"）
  和 fortune 的 `summary_clobber` 门槛明确禁止。

### 5.2 `range_return_rewrite_missing_value`（157）

- 全是 ZMM0 128 位槽，值来源只给 64 位（double 返回）。callee 前几名：
  qfree 66、getinf 15、get_tinfo_property 11、getinf_flag 9、clear_tinfo_t 9、
  qvector_reserve 7、_ZdlPv 6、log2ceil 6、lexcompare_tinfo 6、cleanup_argloc 6。
- 二进制里 double 返回后 XMM0 高 64 位是 ABI 未定义，不能凭空补；正确做法是给
  这些 callee 声明真实的多寄存器返回（qfree 已示范：157 → 115）。

### 5.3 `poison_call_argument`（73 → abi_for_all 后 90）

- 都发生在重写时还是间接调用的调用点（callee 后来才被函数指针提升解析成内部
  变参函数），走"外部/间接"路径，poison 是 fortune 零-unknown 门槛的既定表示；
  cleanup 时再把内部 callee 的 poison 换成 unknown。要更好只能在重写时保留候选值。

### 5.4 小类

`call_arg_rewrite_missing_binding` 5（栈参数绑定缺失）、
`poison_aggregate_field` 4、`call_effect_mixed` 2/59（abi_for_all 后）。

## 6. 接下来该做什么（建议优先级）

1. **P0：给剩余 class B callee 补 `returns` 声明**（收益最直接、不动代码）。
   需要真实签名：从 IDA SDK 头文件/文档确认每个函数在哪些寄存器返回值；先用
   asm 观察调用点之后读了哪些寄存器，再对照 callee 的 `ret` 路径。每加一个 callee
   重跑全量，记录 unknown / poison / IR 大小（native52 = 646 是当前最好）。
2. **P1：class A 的分函数约定范围**。`functions[].abi: "external"` 已经可用；
   对 class A 高发函数（`FUN_29a670`、`FUN_29a660`、`FUN_2a9cf0`、`FUN_2e4e70`、
   `FUN_195a70`…）逐个钉，或调研 `call_effect_mixed` 的 59 处（可能是可修的
   effect 分类问题，而不是用户信息缺失）。
3. **P2：工程化"提升-观察-调 override"循环**：给工具加一个 `--metrics-json`
   输出（按 reason 的 unknown 计数、poison、IR 大小、verify 结果），方便批量对比
   不同 config；顺手给 `external_prototypes` 里解析不到的寄存器名加 warning
   （现在会静默退化成"未知外部"）。
4. **P3：小类**：`call_arg_rewrite_missing_binding`（栈参数绑定）、
   `poison_aggregate_field`（常量聚合基座）、`call_effect_mixed`。
5. **P4：文档**：把"用户 override 工作流"（如何写 prototypes / functions[].signature
   / ir-override，怎么迭代、怎么验证）整理进 `docs/`，现在只在 logs 里。
6. 每轮结束照旧：`ctest` 必须 19/19；产物用 LLVM 22 的 `llvm-as` + `opt -passes=verify`
   验证；改动默认直接提交，rebase 到 main 后 `git push origin dev:main`，主 worktree
   `git merge --ff-only origin/main` + 重建。

## 7. 失败/被否的方向（别重复试）

- 纯 clobber 返回槽 → clobber 载体：被单测 + fortune `summary_clobber` 门槛否掉。
- 全局把 vararg 的 poison 换成 `notdec.unknown`：fortune 零-unknown 门槛。
- 给外部 noreturn 函数做截断：指标回退（unknowns、clobber 都涨）。
- 任何"看起来能省 unknown"的改动，先跑 fortune ctest 再跑全量。

## 8. 关键文件

- 交接与实验记录：`logs/20261117-01-native-external-config-ir-override-plan.md`
  第 9–11 节（override/原型/128 位/多寄存器返回/签名钉死）。
- 本轮修复记录：`logs/20261117-03-hexx64-native-input-plan.md` 第 5–31 节。
- 相关代码：
  - `lib/passes/register/RegisterSignatureSlots.cpp`（`explicitRegisterSlot`、
    `shapeForKnownExternal`）；
  - `lib/passes/register/RegisterExternalShapes.cpp`（`buildInitialSignatureShapes`）；
  - `lib/core/NativePipeline.cpp`（`applyFunctionClaims`、stage runner）；
  - `lib/abi/NativeExternalPrototype.cpp`（prototypes JSON 解析）；
  - `lib/core/NativeIROverride.cpp`（整函数 override）；
  - `lib/passes/register/NativeRegisterFinalCleanup.cpp`（poison 收尾）。
