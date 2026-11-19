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
- [ ] **P3 纠正错误方向**：在交接文档里把"用 `returns` 声明补高位"标为被否方案，
  说明 qfree pin 那 66 个 unknown 是造值换来的；给出正确做法（P1）与判据。
- [ ] **P4 回归**：ctest 19/19（fortune x86_64/i386 门槛）、hexx64 全量指标对比
  （native44 默认 vs native53 新默认 vs native52 pin 配置）。
- [ ] **P5 单测**：给 register 核心单测加一个"窄浮点返回 + 宽返回槽 → ABI 未定义位"的用例。

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
