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
