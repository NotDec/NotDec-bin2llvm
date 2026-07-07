# Native register summary RSP save/restore tracking

## 原始需求

能不能准确识别出这种开头保存的指令和结尾恢复的指令，然后直接把这些都从寄存器的读写分析中排除掉？

## 背景

fortune 的 native register summary 里，`main` 一开始的 callee-saved 保存序列会先 `load @R15/@R14/...`，再通过更新后的 `RSP` 写到栈上。之前 `RSP` 在 ignored register 里，summary 只知道第一下入口 `RSP`，连续 push 后的 `entry RSP - N` 没有被继续跟踪，导致保存槽识别不准。结果是 RBX/R12-R15 这类保存寄存器被误判成 `ReadEntry=true`，后面 `shapeForInternalFunction()` 把它们做成函数参数。

## 改动

- `lib/passes/summary/NativeRegisterSummary.cpp:65` 增加 `ValueOrigin`，把 SSA value 来源从“入口寄存器”扩成“入口寄存器 + 常量偏移”。
- `lib/passes/summary/NativeRegisterSummary.cpp:89` 在 `State` 里记录 `StackPointerOffset`，专门用于识别连续 push/pop 的 entry-RSP-relative 栈槽。
- `lib/passes/summary/NativeRegisterSummary.cpp:811` 在 CFG join 时，如果不同前驱的 `StackPointerOffset` 不一致，就保守清空。
- `lib/passes/summary/NativeRegisterSummary.cpp:1132` 在 transfer load/store 时，即使 `RSP` 是 ignored register，也保留它的入口偏移来源，并在 `store @RSP` 时更新当前偏移。
- `lib/passes/summary/NativeRegisterSummary.cpp:1203` 对固定 entry stack slot 的保存值只在 offset 为 0 时记录为 saved register，避免普通偏移值被误当保存寄存器。
- `lib/passes/summary/NativeRegisterSummary.cpp:1267` 扩展 `entryValueOrigin()`，支持 `entry register + constant` / `entry register - constant` 的窄范围传播。
- `lib/passes/summary/NativeRegisterSummary.cpp:834` 把 x86-64 低 32 位 GPR 写识别为整寄存器非入口定义，避免 `xor r15d,r15d` 这类清零后仍保留高 32 位入口值。

## 验证

命令：

```bash
cmake --build external/NotDec-bin2llvm/build --target notdec-native-llvm -j4

OUT=/tmp/fortune-rsp-load-offset-20260707095714
external/NotDec-bin2llvm/build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  --all-confirmed --skip-runtime \
  -o "$OUT/fortune.ll" \
  --summary-json-out "$OUT/summary.json" \
  --register-ssa-warning-out "$OUT/register-warnings.json"

llvm-22.1.0.obj/bin/llvm-as "$OUT/fortune.ll" -o "$OUT/fortune.bc"
```

结果：

- `main` 不再带 RBX/R12/R13/R14/R15/RBP 参数，只剩 RCX/RSI/R8/R9/ZMM 参数。
- `FUN_3eb0` 不再因为保存/恢复 RBX/R12/R13/R14/R15 生成多返回值，返回类型降为 `i32`。
- 寄存器相关残留从本轮修复前的 23 个降到 4 个，剩余主要是 `FUN_3470` 内部真实使用 R14 作局部临时值，不是保存/恢复误判。
- fortune 同口径耗时约 `11.48s`，对比之前约 `12.27s` / `12.13s` 没有性能回退。

## 判断

这次只修 summary 事实本身：让入口 RSP 派生出来的保存槽能被准确识别，避免 callee-saved 保存动作污染 `ReadEntry` 和函数签名。没有把 ABI preserved register 直接从参数/返回里硬过滤，所以如果函数体真的把 R14/RBX 当局部寄存器使用，后续仍会正常保留对应数据流。
