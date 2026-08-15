# 20260815-01-native-summary-bitrange-effect-plan.md

## 用户原始 prompt

> 阅读external/NotDec-bin2llvm/Arch2.md。对于bottom up那些分析，比如ReadEntry那些，应该也搞成按照bit range来，而且可以消除partialWriteRegister :1173 那边整寄存器粒度下保守方向的选择：宁可 MayEntry 保留，不误杀，这种保守处理。由ReadEntry作为主要的判断参数的依据，而不是EntryDemand

> 完整实现后再测试吧

## 背景

bottom-up effect（`Cell` 的 `MayEntry`/`MayNonEntry`/`ReadEntry`）是整寄存器粒度的三个 bool。
`partialWriteRegister`（`NativeRegisterSummary.cpp` 原 :1173）只能保守地置 `MayNonEntry=true`、
不杀 `MayEntry`——因为没写到的位可能还是入口值。整寄存器粒度带来的问题：

1. 写 EAX 后再读 EAX：`MayEntry` 整寄存器仍 true → 假 `ReadEntry` → 假参数。
2. callee 只写低 32 位，调用者 `applyFunctionEffect` 整寄存器保留 `MayEntry` → 调用后状态判断错。
3. 参数判断依据分裂：存在性用 `ReadEntry`（bool），宽度用 `EntryDemandMask`。而 `EntryDemandMask`
   是反向 liveness"入口处有有效值"，包含写保留位（partial write 读改写），比真读的位宽。

## 目标

- bottom-up effect 三个事实全部按 bit 区间（APInt mask）计算。
- 删除 `partialWriteRegister` 的保守分支，partial write 只杀写到的位。
- 内部函数参数判断以 `ReadEntryMask` 为主（存在性 + 宽度），不再用 `EntryDemandMask`。

## 实现

### `lib/passes/summary/NativeRegisterSummary.cpp`

- `Cell`（:143）：三 bool → 三 `llvm::APInt` mask；缺省 = 整寄存器 MayEntry 全 1。
  `defaultCell(global)`、`cellFor`/`cellIn` 按 `registerBitWidth` 初始化宽度。
- `readRegister(state, global, readMask)`：`ReadEntry |= MayEntry & readMask`，被覆盖的位不再算入口读。
- `writeRegister(state, global, writeMask)`：`MayEntry &= ~mask; MayNonEntry |= mask`。
  **删除 `partialWriteRegister`**，partial write 直接按位区间写；`isX64Low32GprWrite`
  （写 EAX 清高 32）传全宽 mask。
- `restoreRegister(state, global, restoreMask)`：保存值写回只恢复对应位。
- `joinCell`/`joinState` 按位 or；缺失路径用 `defaultCell`（原来是 `Cell{}`，or 空 APInt 会宽度不匹配）。
- `applyFunctionEffect`（:2217）：三个事实按位组合；origin 的 CallClobber/Mixed 判断用 mask 非零投影。
- `applyCallInputs`：只把 input slot 覆盖的位算入口读（`registerRangeMask(OffsetBits, SizeBits)`）。
- `applyBackwardCallDemand`（:2472）：只把 callee `MayNonEntry` 的位加进 callee 退出需求，
  只擦 `~MayEntry` 的位，不再整寄存器 erase。
- `ValueOrigin` 加 `Mask` 字段（:130）：整 load 全宽、partial read 只带读到的位；
  `markEntryValueRead` 按 `origin.Mask` 记录入口读，避免窄值沿整寄存器扩散成全宽参数。
- metadata / 公共 summary：新增 `read_entry_mask`/`may_entry_mask`/`may_non_entry_mask` 字段，
  bool 字段保留为 mask 非零投影；`maskToHex` 全 0 返回空串。

### `include/notdec-bin2llvm/passes/summary/NativeRegisterSummary.h`

- `NativeRegisterSummaryRegister` 增加 `ReadEntryMaskHex`/`MayEntryMaskHex`/`MayNonEntryMaskHex`。

### `lib/passes/summary/NativeRegisterSummarySSA.cpp`

- `SummaryRegisterFact` 增加三个 mask 字段并解析。
- `addParamForUnit`（:1545）：参数宽度以 `ReadEntryMask` 为准（存在性判断仍是 `ReadEntry` bool
  投影 = mask 非零）；mask 空时退回 `EntryDemandMask`（旧产物兼容）再退整寄存器。
- `rangeMayComeFromEntry`（:5710）：按 range 位检查 `MayEntryMask` 是否有交集，不再整寄存器。
- `addExitLiveRegisters`（:4866）：返回需求取 `ExitDemandMask & MayNonEntryMask` 交集。
- `addSummaryDemandRangeBoundaries`：range 分界改用 `ReadEntryMask`。
- `rewritePartialReads`：`readAccessRangeIfDominating` 传 `allowUnknownSegments=true`，
  partial read 读 unknown 段时替换成 unknown 值而不是残留 helper。

### `tests/`

- `native_register_summary_test.cpp`：`testPartialWriteDoesNotReadEntryByItself` 增加 mask 断言；
  新增 `testPartialWriteBitwiseReadEntry`（写高 32 后读高 32 不算入口读、读低 32 算）。
- `native_register_summary_ssa_test.cpp`：修 7 处 run 后继续用旧函数指针的 UAF 测试
  （`runNativeRegisterSummarySSA` 会替换函数对象，按名字重新取）：
  `testUnknownExternalClobberArgBecomesUnknown`、`testNarrowEntryRangeDoesNotCreateWholeEntryLoad`
  （顺带去掉 `EnableResidueRemoval=false`，死 helper 不再残留）、`testPartialWriteHelperIsConsumedBySummarySSA`、
  `testPartialReadHelperIsConsumedBySummarySSA`、`testFullStoreFeedsPartialReadThroughRangeSSA`、
  `testBranchPartialReadUsesNarrowRangePhi`、`testDeadPartialWriteUsesRangeLiveness`。

## 验证

- `native_register_summary_test` / `native_register_summary_ssa_test` 全绿（直接跑 + gdb 跑）。
- ctest 全量 16/17 通过；唯一失败 `notdec.heritage_to_llvm.forward_defs` 是旧 heritage 链路
  既有失败（改动前同样失败），与本次无关。
- wrk 全量：rc=0，`llvm-as` + `opt -passes=verify` 通过，warning 938 条与基线持平不反弹。
- fortune x86_64 / i386 回归（ctest `notdec.native_llvm.realworld_fortune_*`）通过。

## 遗留

- 测试里仍有部分"run 后使用旧函数指针"的用例（canary/stack 系列等），目前函数不被重写时
  指针恰好有效，属于潜在 UAF，未逐一清理（超出本次范围）。
- 参数宽度回退链里 `EntryDemandMask` 保留作旧产物兼容。
