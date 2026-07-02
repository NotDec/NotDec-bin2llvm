# native eh_frame cold fragment cleanup 实现记录

## 背景

Bench2 fortune 里 `get_tbl` 主函数在 `0x4750`，错误路径 cold fragment 在
`0x27f0`。`.eh_frame` 给了两个 FDE range：`[0x4750,0x49d2)` 和
`[0x27f0,0x2803)`。`0x47d9` 是条件跳转到 `0x27f0`，不是 call；旧逻辑把
`0x27f0` 当成独立函数，后续 IR 里出现 `notdec_native_27f0` 和寄存器残留。

这次按保守策略处理：`.eh_frame` 仍作为 decode range / boundary hint，但如果一个
fallback 函数只来自 `.eh_frame`，并且被 conditional branch 跳入，就把它合回跳转来源
函数，不再当源码函数入口。无条件跳转先不合并，避免误伤真实 tail jump。

## 修改

- `include/notdec-bin2llvm/NativeAnalysis.h:37`：
  `NativeFunctionSeed` 增加 `IsEntry`，区分真正函数入口和 range hint。
- `include/notdec-bin2llvm/NativeAnalysis.h:306`：
  增加 `demoteFunctionSeedToRangeHint`、`removeFunction`、
  `restoreInstructionTailFlowTarget`。
- `lib/NativeAnalysis.cpp:4060`：
  `isKnownOtherFunctionEntry` 只认 `IsEntry=true` 的 seed，避免被降级的
  `.eh_frame` range 继续切函数。
- `lib/NativeAnalysis.cpp:4450`、`lib/NativeAnalysis.cpp:4551`：
  `FlowFactNormalizer` 新增 `foldEhFrameOnlyBranchTargets`，把
  `gtirb-seed-range-fallback` 且只来自 `.eh_frame` 的 branch target 合回 owner；
  conditional branch 里误放到 `TailFlowTargets` 的目标会恢复到 direct flow。
- `lib/NativeAnalysis.cpp:5118`、`lib/NativeAnalysis.cpp:5122`、
  `lib/NativeAnalysis.cpp:5384`：
  补 NativeProgramState 的函数删除、seed 降级、tail-flow 恢复操作。
- `lib/SleighLift.cpp:520`、`include/notdec-bin2llvm/SleighLift.h:83`：
  `collectSleighPcodeRanges` 增加 `preserveRangeOrder`，默认仍按地址排序。
- `tools/notdec-native-llvm.cpp:573`、`tools/notdec-native-llvm.cpp:822`、
  `tools/notdec-native-llvm.cpp:1095`：
  confirmed function lifting 时保持 entry block 排在前面，避免 cold fragment 地址更低时
  成为 LLVM 函数入口块。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1800`、
  `lib/passes/summary/NativeRegisterSummarySSA.cpp:1822`、
  `lib/passes/summary/NativeRegisterSummarySSA.cpp:1985`：
  partial demand 把普通内存 load/store 的 pointer 以及 `inttoptr` 的整数输入视为活跃，
  防止把 `%RDI + offset` 这类真实内存地址改成 0。
- `tests/native_analysis_facts_test.cpp:142`：
  新增 eh-frame-only branch target 合回 owner 的回归测试。
- `tests/native_register_summary_ssa_test.cpp:3493`：
  新增 partial demand 不应把普通内存地址改成 0 的回归测试。

## 验证

编译：

```bash
cmake --build external/NotDec-bin2llvm/build --target native_register_summary_ssa_test native_analysis_facts_test notdec-native-llvm -j4
```

回归测试：

```bash
./external/NotDec-bin2llvm/build/bin/native_register_summary_ssa_test
./external/NotDec-bin2llvm/build/bin/native_analysis_facts_test
ctest --test-dir external/NotDec-bin2llvm/build -R 'notdec.native_analysis.facts' --output-on-failure
ctest --test-dir external/NotDec-bin2llvm/build -R 'notdec.native_register_summary.ssa' --output-on-failure
```

fortune smoke：

```bash
OUT=/tmp/notdec-bin2llvm-fortune-final-20260702124002
/sn640/NotDec/external/NotDec-bin2llvm/build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  -o "$OUT/fortune.native.ll" --all-confirmed --skip-runtime
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as "$OUT/fortune.native.ll" -o "$OUT/fortune.native.bc"
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify "$OUT/fortune.native.bc" -o "$OUT/fortune.native.verified.bc"
```

结果：

- 输出路径：`/tmp/notdec-bin2llvm-fortune-final-20260702124002/fortune.native.ll`
- `notdec_native_27f0` 不再存在。
- `notdec_native_4750` 保留正文，`bb_27f0` 合入该函数，并由 `bb_47c8` 条件跳入。
- 没有 `@RBX`，没有直接 `load/store @RBX`，没有 `inttoptr i64 0`。
- `llvm-as` 和 `opt -passes=verify` 通过。
- 同口径 native run：本次 `elapsed=6.46s`；之前观察约 `5.85s-5.87s`。本次保留了
  `get_tbl` 正文并多做 fold/cleanup，短期看有小幅增加，后续要继续用 Bench2 批量看是否
  是稳定回退。

## 评估

- 实现效果：8/10。fortune 当前关注点修掉了，`.eh_frame` FDE 不再直接等同源码函数。
- 复杂度：6/10。主要复杂度在 flow normalizer 多了一步合并，但只处理明确的
  eh-frame-only fallback branch target。
- 维护成本：5/10。规则比较窄，后续如果遇到更多 split function 形状，需要继续扩展判定。

更好的长期方案是建立明确的 chunk/function 模型，把 FDE、symbol、call target、
branch target 分成不同等级的证据；当前先用小规则修 fortune，避免引入大改。
