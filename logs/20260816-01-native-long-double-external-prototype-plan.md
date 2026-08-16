# Native 外部 long double 原型支持

## 用户原始 prompt

> 对于之前提到的第二个问题，先补充long double 支持试试
>
> 先按这个试试吧

## 背景

当前 native SummarySSA 已能把 x87 ST0 建成 `x86_fp80` 返回槽，但外部
`sqrtl` 仍按未知零参函数处理。wrk 的 `stats_stdev` 会先把 80 位参数写到
调用栈，再 tail call `sqrtl`；参数和 ST0 返回都没有可信原型约束，最终生成的
`sqrtl` 声明及 `stats_stdev` 返回类型错误。

本次只处理 bin2llvm native 支持的 x86 ELF。这里的 `long double` 指 SysV
x86-64/i386 使用的 80 位 x87 类型，对应 LLVM `x86_fp80`。

## Ghidra 现状

Ghidra 的 x86-64 SysV cspec 位于
`Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec`：浮点寄存器 pentry
最大为 8 字节，通用栈 pentry 从 `stack+8` 开始、按 8 字节对齐。

`Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc` 的
`ParamListStandard::assignAddressFallback()` 先按浮点资源匹配；10 字节
long double 放不进 XMM pentry 后，会回退到通用栈 pentry，并按类型大小和
对齐推进栈位。cspec 没有写 ST0 输出，bin2llvm 现有 `abiFacts()` 已针对 SysV
补入 ST0 浮点返回槽。

## 目标与路线

- 给可信外部原型增加 `LongDouble` 类型，并允许 JSON 使用 `long_double`。
- LLVM 类型固定为 `x86_fp80`；参数绕过 XMM，使用 ABI 栈 pentry；返回只匹配
  已存在的 ST0 浮点输出槽。
- 默认原型表先只加入 `long double sqrtl(long double)`，不批量猜测其他
  `*l` 函数。
- wrk 调用前保存的是 `i80` 位模式，栈参数绑定允许等宽整数与浮点类型之间做
  bitcast，再交给重写后的 `sqrtl(x86_fp80)`。

## 风险与判断标准

风险主要在栈槽大小和返回槽选择：80 位有效载荷在 x86-64 占 16 字节参数格，
不能压到 XMM0，也不能把返回误接到 XMM0。实现应继续从 cspec 的栈起点和对齐
计算位置，不硬编码 RSP 偏移。

判断标准：定向测试覆盖 `i80` 栈写入、`x86_fp80` 参数和 ST0 返回；wrk
`stats_stdev` 生成 `x86_fp80 @sqrtl(x86_fp80)`，相关 unresolved warning
消失，最终 IR 通过 LLVM 22 assemble 和 verifier，native 测试不回归。

## 不做的事

- 不推断一般未知外部函数的栈参数。
- 不支持 `complex long double`、IEEE 128 位 long double 或非 x86 ELF ABI。
- 不批量加入 `powl`、`frexpl` 等签名不同的 long double 函数。

## 实现记录（已完成）

实现按计划保持在 x86 ELF 和 `sqrtl` 范围内：

- `include/notdec-bin2llvm/NativeExternalPrototype.h:27` 的
  `NativeExternalPrototype::ValueType` 增加 `LongDouble`。
- `lib/NativeExternalPrototype.cpp:18` 的 `parseValueType()` 接受
  `long_double`；`:638` 的 `defaultNativeExternalPrototypes()` 增加
  `long double sqrtl(long double)`。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1135` 的
  `llvmTypeForKnownValue()` 映射到 `x86_fp80`；`:1649` 的
  `typedParamSlot()` 选择栈 pentry；`:1708` 的 `typedReturnSlot()` 只选择
  ST0；`:6183` 的 `readStackSlotValueBefore()` 支持等宽 `i80` 与
  `x86_fp80` bitcast。
- 同文件 `addDemandedExternalReturns()`（`:2593`）把 `TypedReturn` 视为完整
  可信返回形状，避免旧 i64 call evidence 再追加 RAX。
- `lib/passes/summary/NativeRegisterSummary.cpp:2129` 的
  `callsiteStackSlot()` 和
  `lib/passes/summary/NativeRegisterSummarySSA.cpp:6258` 的
  `callsiteStackAddress()` 对 tail call 使用入口 SP 的原始 cspec 偏移；普通
  call 仍减 `StackShift`。
- 实现时发现 wrk 的 RBP 栈地址依赖循环 phi。SummarySSA 现由
  `collectSignatureCallArgs()`（`:5910`）分两次收集：寄存器参数保持原时机，
  栈参数在 `finalizePendingPhis()` 后收集，并由 `callArgStoreBindings()`
  （`:6068`）按参数 index 合并。
- `lib/passes/summary/NativeStackAddress.cpp:337` 的 `addressForInteger()`
  仅接受“self edge 加唯一相同实际 incoming”的 loop-invariant phi；一般多值
  phi 仍返回未知。
- `tests/native_register_summary_test.cpp:769` 增加 tail call 栈 evidence
  回归；`tests/native_register_summary_ssa_test.cpp:5183` 覆盖 JSON 解析，
  `:7391` 覆盖 loop phi、`RSP+8` 栈参数、i80 位模式、ST0 返回及禁止追加 RAX。

相比计划，多出的两阶段绑定和保守 phi 解析不是扩大 long double 类型范围，而是
真实 `stats_stdev` 中参数 store 在绑定阶段仍写成 `RBP phi + 16` 所必需。后续
cleanup 才会把它化成 `RSP.entry + 8`，只改 tail 偏移无法找到该 store。

## 验证

- `cmake --build build --target all -j4`：通过。
- `native_register_summary_test` 和
  `notdec.native_register_summary.ssa`：通过。
- `stats_stdev` 单函数：生成
  `tail call x86_fp80 @sqrtl(x86_fp80 %storemerge348)`，`sqrtl` warning 清零，
  LLVM 22 assemble 和 verifier 通过。
- wrk `--all-confirmed --skip-runtime`：89.51 秒，峰值 RSS 209624 KiB；
  `stats_stdev` 为 `define internal x86_fp80`，`sqrtl` 声明和调用均为
  `x86_fp80 (x86_fp80)`，LLVM 22 assemble 和 verifier 通过。
- 完整 17 项 CTest：16 项通过。既有的
  `notdec.heritage_to_llvm.forward_defs` 仍报
  `stack input fallback was not reused`；该测试不经过本次 native SummarySSA
  long double 路径，本次未改 heritage 模块。

## 评分

| 维度 | 评分 | 判断 |
| --- | ---: | --- |
| 实现效果 | 9/10 | wrk 的参数、返回、声明和跨函数 `stats_stdev` 返回均恢复正确。 |
| 理解成本 | 4/10 | 新增两阶段参数绑定，但仍复用同一 binding 结构和严格地址分析。 |
| 维护成本 | 3/10 | 类型只加一个枚举和一个默认原型；phi 规则刻意限制为单一实际 incoming。 |

更完整的方案是让通用 ABI 类型分配器直接理解所有 C long double 变体，并让
SummarySSA 的循环 phi 在任何消费者读取前达到化简 fixpoint。当前项目只处理 x86
ELF，且这会明显扩大改动范围；本次的受限实现更合适。
