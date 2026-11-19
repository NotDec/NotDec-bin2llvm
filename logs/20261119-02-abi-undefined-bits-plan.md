# ABI 未定义位表示 + 指标工程化（hexx64 收尾）

> 用户原始 prompt：
>
> 用特殊的带标记的unknown应该能用来表示ABI-undefined 位吧？比如带个metadata之类的？
>
> 其他的按照推荐的方式来吧。那个错的方向还是尽快改成正确的方向吧
>
> （上一条讨论的结论：交接文档 §6 的 P0「给剩余 class B callee 补 `returns` 声明」
> 属于用声明造值，先把方向纠正过来；S1 指标工程化 → S2 ABI 未定义位 → S3/S4。）

## 计划

- [x] **P0 判定 ZMM0 那批 unknown 的性质**（交接文档 §5.2 的 157/115 个）。
  结论：不是 callee 原型缺失，而是"外部 callee 只声明了 64 位浮点返回，而调用链
  上游要 128 位"。78 个函数里 72 个的调用点确实 `extractvalue` 取了高位，所以
  不能简单收窄返回槽（原先的猜测被数据否掉）。
- [x] **P1 引入 ABI 未定义位的表示**：值仍然是 `notdec.unknown.<ty>`（保持确定性，
  不用 poison/undef），但挂 `!notdec.abi_undefined` 标记 + `reason=abi_undefined_bits`，
  审计/指标把它从 unknown 里分出来。
- [x] **P2 指标工程化**：`--metrics-json`（unknown 按 reason/function/callee、
  ABI 未定义位、裸寄存器读、poison、模块规模、verify）+ `scripts/native-metrics-matrix.sh`。
- [x] **P3 纠正错误方向**：交接文档 §5.2 已把"用 `returns` 声明补高位"标为被否
  方案，说明 qfree pin 那 66 个 unknown 是造值换来的；给出正确做法（P1）与判据。
  §7"被否方向"也补了这一条。
- [x] **P4 回归**：ctest 19/19（fortune x86_64/i386 门槛通过）、hexx64 全量指标
  对比（默认 728 → 517+211、abi 501+211，见上面表格）、`native53.ll` 过
  LLVM 22 `llvm-as` + `opt -passes=verify`。
- [x] **P5 单测**：`testAbiUndefinedValuesAreAuditedSeparately` 覆盖**记账侧**
  （审计/剩余 unknown 统计把 ABI 未定义位分开）。**发射侧**（窄浮点返回槽 +
  宽请求位段）没写单测：复现它要完整走一遍签名重写的返回槽装配，成本高，
  当前由 hexx64 全量跑 + 指标 JSON 覆盖（qfree/getinf 等 211 个值）。

## 参数口径统一（本轮顺手做的）

产物里的洞目前有三种名字混着用，后面统一按这个口径：

| 名字 | IR 形态 | 含义 |
|---|---|---|
| unknown | `call @notdec.unknown.<ty>()` + `!notdec.unknown.source` | 我们不知道值（建模缺口/信息缺失） |
| abi-undefined | 同上 + `!notdec.abi_undefined` | 二进制里本来就没有约定值（ABI 未定义位） |
| poison | `poison` 常量操作数 | 只用于 ABI 保证的"死值"（如 x87 空槽），不用于未知 |

指标分 calls / sources 两栏：一个 `!notdec.unknown.source` 源节点常被多个
`notdec.unknown` 调用引用（native52 里 363 个源节点对应 646 个调用点）。

## 实现记录

（见下方"实现"小节，随做随补。）
## 实现

### 1. ABI 未定义位的表示（`!notdec.abi_undefined`）

- 值本身仍然是 `notdec.unknown.<ty>()`：ABI 未定义位"可以是任意值"，用不透明
  调用承载既保持确定性（不用 poison/undef，见 `NotDecIntrinsics.cpp` 的注释和
  `logs/20261117-02-native-ir-determinism-plan.md`），又不会被 InstCombine 折成
  具体值。
- 额外挂一个标记元数据（不是另造 helper 家族，避免动 6 处
  `notdec.unknown.` 的特判）：

```llvm
%ZMM0.abi_undefined = call i64 @notdec.unknown.i64(),
    !notdec.unknown.source !3282,   ; reason=abi_undefined_bits, kind=abi_undefined,
                                    ; detail=slot_bits=64,unit_bits=128,abi=float_return
    !notdec.abi_undefined !3283     ; register=ZMM0, callee=qfree, detail=...
```

- 判定（`FunctionBuilderSignature.cpp: abiUndefinedReturnRange`）：
  1. callee 是**声明**（没有函数体）——内部 callee 的返回槽是我们按需求建的，
     缺位是建模缺口，继续算 unknown；
  2. shape 里该寄存器的返回槽是 `FloatRegister`、`LlvmType` 是 `float`/`double`
     （SysV 只定义 XMM 低 32/64 位）；
  3. 请求位段**整个落在槽之上**（`range.BitOffset >= slot.SizeBits`）。和槽部分
     重叠的位段保守地继续报 unknown。
- 记账：`NativeRegisterValueSourceAudit::AbiUndefinedValues` /
  `AbiUndefinedByCallee` 单独统计，`UnknownValuesByReason` 不再包含它们；
  `countRemainingUnknownValues` 跳过它们；summary 里新增
  `abi_undefined_returns`（在重写后的清理遍之后按最终 IR 重新数，因为清理遍用的是
  临时 summary）。

### 2. 指标工程化

- `--metrics-json <path>`：模块规模、unknown（calls/sources、by_reason、
  sources_by_reason、by_function、by_callee）、abi_undefined（calls、by_callee）、
  poison 操作数指令数、裸寄存器读（local/boundary/partial_write/clobber）、
  verify 结果、pipeline stage 列表。
- `scripts/native-metrics-matrix.sh`：`<label>\t<config|->` 矩阵 → 每个配置一份
  `.ll` + `.json`，最后打印对比表。DEBUG.md 第 2.2 节写了用法。

## 结果（hexx64.so，6187 函数）

口径：`unknown.calls` / `unknown.sources` / `abi_undefined.calls` / IR 大小。

| 配置 | unknown calls | unknown sources | abi-undefined | IR |
|---|---|---|---|---|
| native44 旧默认 | 728 | 556 | （混在 unknown 里，157 个源） | 88.1 MB |
| **default（新）** | **517** | **400** | **211** | 88.1 MB |
| **abi_for_all（新）** | **501** | **249** | **211** | 83.2 MB |
| native52（abi + qfree pin，旧最好） | 646 | — | 0（pin 把高 64 位造出来了） | 83.2 MB |

- 517 + 211 = 728，和旧默认完全对齐：**没有修掉 211 个洞，只是不再把它们算成
  "我们不知道"**。这批值的来源分布和交接文档 §5.2 一致（qfree 66、getinf 15、
  get_tinfo_property 11、clear_tinfo_t 9、getinf_flag 9、qvector_reserve 7…）。
- qfree pin 那 66 个"收益"现在看是造值：声明 `XMM0:128` 之后这 66 个值变成
  `extractvalue` 出来的"已定义高半"，而二进制里它们就是未定义的。

## 未做 / 后续

- 整数返回槽的同类情况（`int` 返回后 RAX 高 32 位）：同一套机制可以扩展，但需要
  按 callee 返回类型宽度判断，等 S3 之后一起做。
- 交接文档 §5.2 里"给 callee 补 `returns` 声明"的方向已在原文标注为被否方案。
- 单测覆盖的是**记账侧**（`testAbiUndefinedValuesAreAuditedSeparately`）：手工
  构造带标记的值，断言审计/剩余 unknown 统计把它们分开。**发射侧**（窄浮点返回槽 +
  宽请求位段）在单测里复现成本高（要完整走一遍签名重写的返回槽装配），当前由
  hexx64 全量跑 + 指标 JSON 覆盖（qfree/getinf 等 211 个值）。

## 结论：`abi_for_all_functions` 暂不进默认（S4 的答案）

- 收益：unknown 517 → 501（少 16，来自原来"内部函数把 caller-saved 当返回通道"
  那批 clobber 返回槽），IR 88.1 → 83.2 MB。
- 代价：所有函数都被当成"边界函数"，审计里 `local_register_reads` 直接变成 0、
  `boundary_register_reads` 从 213 涨到 1385——I1/I3 这两个不变量失去区分度
  （"接口可改的内部函数里不该残留寄存器读"变得无法检查）。
- 所以默认保持 `abi_for_all_functions: false`，需要压 unknown 时再按配置开。
  真正该动的是 `call_effect_mixed`（abi 下 2 → 59）：那是"外部 callee 的
  caller-saved 寄存器既可能是入口值又可能被改写"的建模缺口，下一轮 S3 处理。

## S3：`call_effect_mixed` / `poison_call_argument` triage

两个桶都查了机制和证据，**结论都不是独立 bug**：一个是调用约定范围的必然代价，
一个是"缺载体"而不是"值错"，后者需要新特性（不是修 bug）。因此本轮不改代码。

### S3.1 `call_effect_mixed`（默认 2 / abi_for_all 59）

- 默认配置的 2 个都在 `FUN_326470 → FUN_2d6a40`：RDI 的上下两个 32 位各一个。
  机制（`callEffect`）：callee 的 RDI 事实是 `MayEntry && MayNonEntry`——某些路径
  保持入口值、某些路径改写；调用点在 RDI 之后要值，但 callee 遵守 ABI 时只有
  RAX/XMM0 能给返回槽，RDI 拿不到载体 → `call_effect_mixed`。
- 这 2 个值确实被用着（喂给 `%RDI.range_summary_ssa` 的 phi），不是死值。
- abi_for_all 下 2 → 59 的原因：更多函数被强制成"遵守 ABI"，同样的模式被放大，
  不是新 bug。**想消掉只有一条路**：让"非导出"的函数即使标成 external 也允许
  透传返回槽（`ReturnEntryMask`）。没有做——它和 S4 的结论冲突（外部约定的意义
  就是"签名不可改"），而且会重新引入 §5.1 那批造值风险。

### S3.2 `poison_call_argument`（73 个源 / 176 个调用点）

- 位置直方图（0 基 args 下标，统计 176 个未知实参）：
  `{1:1, 2:2, 3:7, 4:55, 5:69, 6:42}`——集中在第 4/5/6 个实参，也就是
  **变参尾部**（RCX/R8/R9/栈），不是固定参数。
- asm 证据（`FUN_16afc0` → `FUN_cc790`，调用点在 `0x16b491`）：

  ```
  16b485: lea    0x1e2b61(%rip),%rsi   # 0x34dfed  ← IR 里的 i64 3465197
  16b48c: mov    %rbx,%rdi
  16b48f: xor    %eax,%eax             ← 变参的 AL 计数标记
  16b491: call   cc790
  ```

  调用点只写了 RDI/RSI/EAX，RDX/RCX/R8/R9 全是残留值。而 IR 这边给
  `@FUN_cc790(i64, i64, i8, ...)` 传了 7 个实参，第 5/6 个是未知。
- 机制：这些寄存器的值 = **本函数的入口值**（函数内从没写过），但函数签名里
  没有它们的载体（`FUN_16afc0(i64 RDI, i64 RSI, i32 RDX)`），于是调用点答不出
  来 → poison → unknown。所以是"缺载体"，不是"值算错"。
- **正确的修法（下一步的特性，不是 bug fix）**：把调用实参槽也当成入口需求
  （现在只有显式 `partial_read`/`load` 才算读），给这些位加入口形参，让透传
  沿调用链上升，最终在导出边界落成 ABI 未定义位（就是本轮新加的类别）。
  代价：1–2 天，要改需求分析（`liveBeforeBlock`/`applyBackwardCallDemand`）
  和签名构建；签名会变宽（每个透传寄存器一个形参）。
- 顺带确认的两件事：
  - 73 个源节点对 176 个调用点，是因为 poison 占位按"函数 × 类型"复用
    （`replacePoisonCallArguments` 缓存），一个源节点会被多个调用点共用。
  - 36/37 个变参函数带一个**没用的** `i8 %RAX.arg` 形参（SysV 的 AL 变参计数
    被当成固定参数），每个变参调用点因此多传一个实参。但它只导致 2 个未知
    （位置 2），当前不值得为它改签名；记在这里当清理项。

## 更正（2026-11-19 晚）：这批值的"ABI 未定义"定性只对了一半

用户质疑"二进制怎么可能真读 ABI 未定义位"，查了汇编和值流，**质疑成立**：

- 这个 .so **没有任何导出函数**：`readelf --dyn-syms` 里定义的 `FUNC` 是 0 个
  （只有 556 个 UND 导入）。所以"导出函数边界"这类函数根本不存在，"外部约定"
  只落在被取地址的函数上。
- `FUN_29a670` 就是 `jmp qfree@plt`（0x29a670: `e9 bb b2 d8 ff`）。调用者
  `FUN_dfd90` 在 `0xdff66` 调完之后的下一条是
  `movabs $0xffffffff00000000,%rax` / `movq $0,0x8(%rbx)` / `mov %rax,(%rbx)`，
  **没有任何 XMM0 读**。而 `FUN_dfd90` 的 pre-RE 函数体里 ZMM0 出现 **0 次**。
- 最终 IR 里 i128 返回值被 `extractvalue` 取 1038 次：**1037 次是纯透传**
  （取出它的函数自己也返回 i128），唯一一次"本地使用"（`FUN_2b12e0`）是把值当
  实参传给 `@FUN_29a940(i64, i128)`，而那个 i128 形参在函数体里**没有任何使用**。
  → 这 128 位值没有任何可观察的消费者，是一个自我循环。
- 374 个带 `exit_demand[ZMM0]` 的函数里，**352 个在 pre-RE 函数体里连 ZMM0 都
  没出现过**。需求不是从这些函数的读来的。

**结论**：这不是"二进制真的读 ABI 未定义位"，而是**两个建模选择叠加出来的人造值流**：

1. ZMM0 单元是 i128，一旦"这个寄存器在出口需要有值"，返回槽就是整个 128 位；
   而值来源（`double` 返回）只有 64 位 → 缺 64 位。
2. 需求沿"我的返回槽要 ZMM0 ⇒ 我的 callee 必须给"一路传，不需要任何真实读；
   链上有环，所以永远不会消失。

所以 `!notdec.abi_undefined` 这个标记**语义上仍然正确**（这些位若真被读，确实没有
约定值），但把它当成"二进制读了 ABI 未定义位"的结论是错的；**真正的修法在需求
分析/槽宽**：槽宽按需求位宽而不是单元位宽，并加一次"可观察使用"反向可达清扫，
把纯透传环上的槽剪掉。第一颗种子在哪还没查清（候选：某处 128 位读经提升放大，
或某个间接调用点的默认 shape），下一步用单函数 `-f` + `--summary-json-out` 定位。

## 需求源头定位（2026-11-19 晚）

用户要求"把'返回槽 → 实参 → 返回槽'追到源头，如果消掉源头这些是不是都能消掉"。做了：

**方法**：临时诊断 `NOTDEC_TRACE_ZMM0_DEMAND=1`，在
`applyBackwardCallDemand` 里按寄存器记录"caller 在调用点把需求传给 callee"的边，
另在 `liveBeforeBlock` 的 partial_read 处记录"种子读"，跑一遍全量 hexx64 后离线构图
（`/tmp/hexx64/trace.err`）。

**结果（ZMM0）**：

| 指标 | 值 |
|---|---|
| 需求图节点 | 385 个函数 |
| 去重后的边 | 1183 |
| **源头（无入边）** | **1 个：`FUN_2b12e0`** |
| 汇点（不再传下去） | 49 |
| 纯透传节点（对 ZMM0 没有任何本地访问） | 369 |
| 本地读点（去重） | 402 处 / 312 个函数（其中 365 处是 128 位读，多数被同块本地写抵消） |

**源头细节**：

1. `FUN_2b12e0` 里唯一的"消费"是：`%ZMM0.ret_slot45 = extractvalue {i64,i128} %16, 1`
   然后 `call void @FUN_29a940(i64 %x, i128 %ZMM0.ret_slot45)` —— 只是把值当前实参传出去。
2. `FUN_29a940` 的这个 `i128 %ZMM0.arg` 形参在最终 IR 的**整个函数体里没有任何使用**。
3. `FUN_29a940` 的 pre-RE 体里有 16 处 ZMM0 访问（`partial_read.i512.i32` ×8、
   `partial_write.i512.i32` ×4、`partial_read.i512.i128` ×4），重写后全部消失——
   这些读本来就是**死读**（结果只喂给没人用的计算）。
4. 链条：死读 → `ReadEntry[ZMM0]=true` → 签名加 `i128` 形参 → 调用点必须提供值 →
   调用者在"上一个调用"处的 live 需求带上 ZMM0 → 那个 callee 的 ExitDemand 带上 128 位
   → 它的返回槽又要求自己的 callee 提供……**385 个函数、1183 条边，全部挂在 1 个死读上**。

**结论**：这批"ABI 未定义位"不是 ABI 事实，也不是"二进制真的读了未定义值"，而是
**一个死读经过"入口需求"和"返回槽透传"两轮放大**。消掉源头，384 个函数的 ZMM0 槽
会一起消失（和用户的猜测一致）。

**已加的工具**：`Analyzer::reportDemandOrigins()`（定点结束后打印，见下），
以及 env 诊断 `NOTDEC_TRACE_ZMM0_DEMAND=1`。

**待做的真正修法**（按性价比排序）：

1. **死读 DCE（推荐）**：建 summary 前，把 `partial_read`/`load` 的结果沿 SSA
   反向淘汰——只有能到达"可观察使用"（store 到内存/分支/调用实参/ret）的读才算读。
   `FUN_29a940` 的死读会被整棵删掉，源头消失。
2. **死形参裁剪**：签名重写后，形参在函数体里没有使用就从签名删掉并同步调用点。
   更窄，但直接命中本次源头。
3. 槽宽按需求位宽：**其实已经是这样** —— `integerSlotForSingleDemandRange` 用
   `returnMask` 建槽；只有 mask 不连续时才退化成整个单元宽。本例 mask 是满 128 位，
   所以换宽度不解决问题；真正要换的是 mask 的来源（即 1/2）。

## 可观察性分析（阶段 1）与源头的真正性质（2026-11-19 晚）

按讨论实现阶段 1：`Analyzer::computeUsedInputs()` + `reportUnobservableInputs()`，
在第二遍 summary 的 effect 与 demand 之间跑一个"入口值有没有被可观察地使用"的定点
（保守规则：ret/br/store 到内存/外部调用 ABI 输入/内部 callee 递归/地址被取）。
Arch2.md 新增 11.1 节记录设计，§10 全貌和 §12 边界同步更新。

**结果推翻了"死读"假设**：

- 报告里 **ZMM0 根本没出现**（`read_entry_functions=0 cuttable_callsites=0`）——
  因为 `FUN_29a940` 的 16 字节 XMM0 读**确实被 store 到内存**（`movups` 到
  `RDI+0x28/0x38/0x9c/0xac`），是可观察使用。所以阶段 2 按"可观察性"过滤
  `ReadEntry` **不会**消掉 ZMM0 这条链。
- 继续看 asm 找到真正的原因：

  ```asm
  29a94a: 66 0f 76 c0    pcmpeqd %xmm0,%xmm0      ; XMM0 = 全 1（常见 idiom）
  ...
  29a9f1: 0f 11 47 28     movups %xmm0,0x28(%rdi)
  29a9f5: 0f 11 47 38     movups %xmm0,0x38(%rdi)
  29a9f9: 0f 11 87 9c ..  movups %xmm0,0x9c(%rdi)
  29aa00: 0f 11 87 ac ..  movups %xmm0,0xac(%rdi)
  ```

  p-code 把它降成这样（pre-RE IR）：

  ```llvm
  %XMM0_Da  = call i32 @notdec.partial_read.i512.i32(ptr @ZMM0, i64 0)   ; 同一个读
  %XMM0_Da1 = call i32 @notdec.partial_read.i512.i32(ptr @ZMM0, i64 0)   ; 又读一次
  %0 = icmp eq i32 %XMM0_Da, %XMM0_Da1
  %neg = sext i1 %0 to i32
  call void @notdec.partial_write.i512.i32(ptr @ZMM0, i32 %neg, i64 0)
  ```

  两条 `partial_read` 是同一个寄存器同一个位置，但 summary 的 SSA 不认为它们相等
  （是两个独立的调用），于是 `icmp eq` 的操作数被当成"入口值读"→ `ReadEntry[ZMM0]`
  → 形参 → 385 个函数的返回槽。而**重写后 `icmp eq %a, %a` 被 InstCombine 折成
  true**，值变成 `-1`（最终 IR 里就是 `store i128 -1`），形参因此没人用。

  换句话说：值本身最后是对的，**错的是 summary 把"自比较 idiom"当成了入口依赖**。

**结论 / 下一步**：真正的修法是**在 p-code 降级阶段把同一个 p-code 指令内的重复
寄存器读 CSE 掉**（同 `(global, offset, width)` 的 `partial_read` 复用同一个调用，
遇到写失效）。这样 `icmp eq %v, %v` 直接折成 true，`partial_write` 变成常量，
`ReadEntry` 不再有 ZMM0，整条 385 函数的链自然消失。这比"死读 DCE"更小更准，
而且是通用收益（IR 也会变小）。

阶段 1 的价值就在这：**在动需求种子之前就否掉了错的假设**。报告里其余寄存器
（RSI 38、RDI 144、RAX 181 个 cuttable callsites；DF 33）是另一类真实的可剪需求，
等 idiom 修完再单独评估。

## 修复：p-code 降级阶段 CSE 同一段 p-code 内的重复寄存器读（2026-11-19 晚）

**根因**（承接上一节）：`PcodeLowerer` 对非寄存器 varnode 有 `Values` 缓存，但寄存器
读完全绕过它（`Registers->read()` 每次都发射新的 `load`/`partial_read`），而
`write()` 在寄存器分支直接 `return`、也不失效。于是 `pcmpeqd %xmm0,%xmm0` 的
`icmp eq <同一个读>, <同一个读>` 变成两个独立调用的比较，summary 把操作数当入口读。

**改动**（`lib/core/PcodeToLLVMInternal.h` + `lib/core/PcodeLowererOps.cpp`）：

- 新增 `RegisterReadCache`（key 同 `varnodeKey`，只缓存寄存器读，不缓存 ram load）；
- `read()`：换块清空 → 查缓存 → 命中直接复用 → 未命中读一次并写入缓存；
- `write()`：寄存器写之后清空（写 EAX 也让 RAX/AX/AL 失效，一条机器指令的 p-code
  只有几十个 op，整清空最简单）；
- `Call`/`CallInd`/`CallOther`：调用会隐式 clobber，先清空。

**验证**：

- 单函数 `-f 0x29a940`：签名从 `(i64 %RDI.arg, i128 %ZMM0.arg)` 变回
  `(i64 %RDI.arg)`，函数体里 4 个 `store i128 -1`（值是对的，之前也是 -1，
  但那时靠 InstCombine 折 `icmp eq %a,%a`），**全文 ZMM0 出现 0 次**。
- 全量（6187 函数，`--metrics-json`）：

| 指标 | 修复前 native55 | 修复后 native56 |
|---|---|---|
| `unknown.calls` | 517 | **515** |
| `abi_undefined.calls` | **211** | **0** |
| `return_binding_uses_clobber_value` | 314 | 312 |
| 指令数 | 1,145,990 | 1,131,049（−1.3%） |
| IR 大小 | 88.1 MB | 86.3 MB |
| verify | ok | ok |

- 需求图诊断里 `ZMM0 nodes=385 sources=1` 那一行**消失**：源头修掉之后 384 个
  下游返回槽一起没了（和预期一致）。
- `ctest` 19/19（含 fortune x86_64/i386）。

**还没动的同类链**（各自有独立源头，属于阶段 2 / 后续 idiom 的范畴）：
DF 1450 节点 / 1402 纯透传（106 源头）、R9 1305/881（121）、R8 551/317（17）、
R10 487/436（4）、R11 478/452（4）。可观察性报告里 RDI 144、RAX 181、RSI 38 个
cuttable callsites 也还在。

## 第二条链：DF（1450 节点 / 4430 边 → 0）
DF 是方向标志：值侧早就按 ABI 处理成"边界处恒为 0"（`isAbiBoundaryZeroRegister`
注释里写了：函数入口按 ABI 是 0，调用后也是 0，`cmpsb` 展开循环的方向不能被
clobber 占位污染），但**需求侧没跟上**，于是每个读 DF 的 `cmp*/sca*/movs*` 循环都
给"上一个调用"记一笔退出需求，再沿返回槽透传放大。

两处补齐（`RegisterSummarySignature.cpp`）：
1. `liveBeforeBlock` 的 `partial_read` / `load` 分支：DF 不加需求（值由边界规则给 0）；
2. `applyBackwardCallDemand` 的 `entryNeeds` 循环：DF 跳过（callee 读入口 DF 不需要
   调用点准备值）。

效果：需求图里 `DF nodes=1450 edges=4430 sources=106` **整行消失**。指标不变
（515 / abi_undef 0）——DF 不是 value-carrying 寄存器，从来没有返回槽或形参，
所以这条链此前只是纯记账噪音和定点开销；修它主要是让需求图恢复可信。

## 诊断工具升级
`NOTDEC_TRACE_ZMM0_DEMAND` 改成 `NOTDEC_TRACE_DEMAND=<寄存器名>`，可以给任意寄存器
打种子读（`DEMANDREAD`）和需求边（`DEMANDEXIT`）。DF 的定位就是靠它：先看到
`DEMANDEXIT` 有 19 万条、`DEMANDREAD` 一条都没有，才意识到种子在 `load` 分支和
callee 的 `ReadEntry` 上，而不是 `partial_read`。

## 下一条链：R10 / R11（487 / 478 节点，各 4 个源头）
- 源头函数：`FUN_1d52d0`、`FUN_334770`、`FUN_340d90`（R10 另有 `FUN_2244e0`，
  R11 另有 `FUN_12bb90`）；
- 这两个寄存器是 value-carrying，链条对应 `return_binding_uses_clobber_value` 里
  R10=33、R11=30 个 unknown，**修掉是有指标收益的**；
- 已确认它们确实是普通寄存器写/读（`FUN_334770` 的 asm 里就有对 R10/R11 的写），
  不是 idiom；源头是"某条路径上读了一个本函数没写过的 R10/R11"，下一步用
  `NOTDEC_TRACE_DEMAND=R11` 的 `DEMANDREAD` 定位到具体 block 再对照 asm。
