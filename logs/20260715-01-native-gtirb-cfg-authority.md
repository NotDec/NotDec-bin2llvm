# GTIRB CFG 权威边界重构记录

## 用户原始要求

按这个推进吧，非 GTIRB / seed-linear mode 感觉可以暂时弃用，后续需要再修这一块

## 背景

fortune 里曾出现 `0x2a0e` 这类块：GTIRB/ddisasm 没把它当 native basic block，
但 SLEIGH p-code 内部 `CBRANCH` 被 native discovery 当成机器级控制流，后面又按这个假块拆函数。
这会让 ddisasm 给出的 CFG 和我们自己从 p-code 补出来的 CFG 混在一起，最后出现缺 fallthrough、
函数边界错位、jump table lowering 校验不一致等问题。

这次改动的边界是：GTIRB 模式下，ddisasm/GTIRB 是函数边界和机器 CFG 的权威；
SLEIGH 仍负责指令语义、call/data xref 辅助信息，但不再负责建 native CFG。

## 修改

- `include/notdec-bin2llvm/NativeAnalysis.h:219`：
  新增 `NativeControlFlowAuthority`，并在 `NativeProgramState` 上提供
  `controlFlowAuthority()`、`hasGtirbControlFlowAuthority()` 和
  `setControlFlowAuthority(...)`。
- `lib/NativeAnalysis.cpp:2648`、`lib/NativeAnalysis.cpp:5863`：
  GTIRB import 成功后设置 CFG authority 为 `Gtirb`；重复设置只记 note，不覆盖已有权威。
- `lib/NativeAnalysis.cpp:2846`：
  `importCfgEdges(...)` 只把同函数内 GTIRB branch/fallthrough edge 写入 block successors；
  跨函数 flow 只保留为 `gtirb-ddisasm-flow` xref，避免把别的函数块塞进当前函数 CFG。
- `lib/NativeAnalysis.cpp:3141`、`lib/NativeAnalysis.cpp:3679`：
  `decodeExistingBlock(...)` 在 GTIRB authority 下调用
  `collectDirectControlFlow(..., collectMachineFlow=false)`。这样会保留 call/data xref，
  但忽略 SLEIGH `BRANCH`、`CBRANCH`、`BRANCHIND`、`RETURN` 作为机器 CFG 事实。
- `lib/NativeAnalysis.cpp:4480`：
  `FlowFactNormalizer` 在 GTIRB authority 下跳过 SLEIGH 衍生的补块、拆块和 successor
  归一化，只保留 eh_frame 折叠、非法 successor 删除、外部函数指针 flow 恢复。
- `lib/NativeAnalysis.cpp:4642`、`lib/NativeAnalysis.cpp:4775`、
  `lib/NativeAnalysis.cpp:4851`：
  eh_frame/cold fragment 折叠会同时参考 `gtirb-ddisasm-flow` xref，并在折叠结束后恢复同函数内
  flow xref 对应的 successor，避免折叠后丢边。
- `lib/PcodeToLLVM.cpp:687`：
  新增 `nativeSuccessorsForBlockIndex(...)`。p-code 内部块 lowering 时，如果自身没有 native
  successor fact，会回退到父机器块的 successor fact。
- `lib/PcodeToLLVM.cpp:865`、`lib/PcodeToLLVM.cpp:941`：
  direct/indirect branch lowering 使用上面的父块 successor 查询，允许 SLEIGH 内部语义块存在，
  但不要求它们在 native CFG 里有独立 successor。
- `tools/notdec-native-llvm.cpp:452`、`tools/notdec-native-discover.cpp:1422`：
  internal seed-linear mode 不再跑 SLEIGH seed CFG discovery 和 x86 jump-table analyzer，
  直接输出 note：`native internal seed-linear CFG discovery is disabled; use --native-decode-mode gtirb`。
- `lib/passes/summary/NativeRegisterSummary.cpp:132`、`lib/passes/summary/NativeRegisterSummary.cpp:1109`、
  `lib/passes/summary/NativeRegisterSummary.cpp:1436`：
  修复本次验证时暴露的 summary 固定点收敛问题。`ValueOrigins` 和
  `CallProducedValues` 是块内证据辅助，不属于最终 summary lattice；块内用完的值不再跨 loop
  携带，收敛判断只看 `Reachable` 和 `Cells`。

## 验证

构建：

```bash
cmake --build external/NotDec-bin2llvm/build --target notdec-native-llvm notdec-native-discover -j
```

结果：通过。

CFG 针对性检查：

```bash
external/NotDec-bin2llvm/build/bin/notdec-native-discover \
  --block-json 0x2a0e /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune

external/NotDec-bin2llvm/build/bin/notdec-native-discover \
  --block-json 0x27bb /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune

external/NotDec-bin2llvm/build/bin/notdec-native-discover \
  --native-decode-mode internal --notes-json \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune
```

结果：

- `0x2a0e`：`found=false`，不再是 native discovery block。
- `0x27bb`：仍属于 `0x3470`，successor 为 `0x38cd`。
- internal mode：输出禁用提示 note。

完整 fortune lifting：

```bash
OUT=/tmp/notdec-bin2llvm-fortune-gtirb-authority-20260715-final
external/NotDec-bin2llvm/build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  -o "$OUT/fortune.native.ll" \
  --all-confirmed --skip-runtime --no-instcombine-pass \
  --external-prototypes /sn640/NotDec-Exp/Bench2/bin2llvm-external-prototypes/fortune-executable.external-prototypes.json \
  --summary-json-out "$OUT/summary.json" \
  --register-ssa-warning-out "$OUT/register-ssa-warnings.tsv"
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  "$OUT/fortune.native.ll" -o "$OUT/fortune.native.bc"
```

结果：

- IR：`/tmp/notdec-bin2llvm-fortune-gtirb-authority-20260715-final/fortune.native.ll`
- BC：`/tmp/notdec-bin2llvm-fortune-gtirb-authority-20260715-final/fortune.native.bc`
- warning：`/tmp/notdec-bin2llvm-fortune-gtirb-authority-20260715-final/register-ssa-warnings.tsv`
- `llvm-as` 通过。
- 完整 native run `elapsed=11.16s`。

## 当前边界

- `bb_2a0e` 仍可能作为 LLVM 里的 SLEIGH 语义内部块出现。这不是 native discovery block，
  也不再参与 GTIRB 模式的机器 CFG 建立。
- 本次没有解决后续寄存器残留、函数签名推断、SLEIGH 内部 `CBRANCH` 是否可改成 `select`
  之类的问题；这些属于后续 IR 语义清理。

## 评估

- 实现效果：8/10。GTIRB 模式下 native CFG 的权威边界清楚了，`0x2a0e` 不再污染 discovery CFG，
  fortune 完整 lifting 和 `llvm-as` 通过。
- 复杂度：4/10。主要是把 CFG 来源分流，并给 p-code 内部块 lowering 加父块 successor 回退。
- 维护成本：4/10。后续如果恢复 internal/seed-linear，需要重新补一条独立路径；GTIRB 主路径更简单。
