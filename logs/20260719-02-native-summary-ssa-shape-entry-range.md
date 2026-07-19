# SummarySSA shape 参数入口 range 修复

## 背景

fortune 里 `FUN_3470` 已经被 rewrite 成 6 个参数，但函数体入口的 `RSI/RDX/RCX/R8/R9` range 仍被物化成 `notdec.unknown.*`。这不是调用点没传参数，而是 SummarySSA 在旧函数体里读入口 range 时只看 summary facts 的 `MayEntry`，没有看已经确定的函数 shape。

当函数先读取入口寄存器、后面又覆盖同一个寄存器时，summary facts 可以出现 `ReadEntry=true` 但 exit `MayEntry=false`。shape 会据此把寄存器加入参数；但 range SSA 入口读取又因为 `MayEntry=false` 拒绝生成 entry range，后续 `allowUnknownSegments` 会把缺段补成 unknown。

## 实现

- `lib/passes/summary/NativeRegisterSummarySSA.cpp:5064`
  - 增加 `shapeParamCoversRange()`，判断当前函数已确定的 shape 参数是否覆盖某个 register range。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:5081`
  - `rangeMayComeFromEntry()` 优先接受 shape 参数覆盖的 range，再回退到 summary facts 的 `MayEntry`。
- `tests/native_register_summary_ssa_test.cpp:4236`
  - 增加 `testInternalSignatureShapeParamKeepsOverwrittenEntryRange()`：函数读取 RDI 低 32 位后覆盖 RDI，仍应 rewrite 成 i32 参数，不能留下 partial read 或 unknown。
- `tests/native_register_summary_ssa_test.cpp:7586`
  - 将新测试接入 `native_register_summary_ssa_test`。

## 验证

```bash
cmake --build external/NotDec-bin2llvm/build --target native_register_summary_ssa_test -j4
external/NotDec-bin2llvm/build/bin/native_register_summary_ssa_test
cmake --build external/NotDec-bin2llvm/build --target notdec-native-llvm -j4
```

fortune smoke：

```bash
OUT=/tmp/notdec-bin2llvm-fortune-shape-entry-range-20260719-094236
external/NotDec-bin2llvm/build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  -o "$OUT/fortune.native.ll" \
  --all-confirmed \
  --skip-runtime \
  --external-prototypes /sn640/NotDec-Exp/Bench2/bin2llvm-external-prototypes/fortune-executable.external-prototypes.json \
  --register-ssa-warning-out "$OUT/register-ssa-warnings.tsv"
llvm-22.1.0.obj/bin/llvm-as "$OUT/fortune.native.ll" -o "$OUT/fortune.native.bc"
llvm-22.1.0.obj/bin/opt -passes=verify "$OUT/fortune.native.bc" -disable-output
```

结果：

- IR：`/tmp/notdec-bin2llvm-fortune-shape-entry-range-20260719-094236/fortune.native.ll`
- warning：`/tmp/notdec-bin2llvm-fortune-shape-entry-range-20260719-094236/register-ssa-warnings.tsv`
- `call_arg_uses_unknown_value` 从上一轮 `46` 降到 `11`。
- `FUN_3470` 入口现在直接从 `%RDI.arg/%RSI.arg/%RDX.arg/%RCX.arg/%R8.arg/%R9.arg` extract range。
- `llvm-as` / verifier 通过。

## 备注

剩余主要问题是 `range_return_helper_rewrite_missing_value=32`，需要单独判断 RAX 高位 range 是死 helper、EAX 零扩展语义，还是返回 shape 还不够准。
