# Native register peephole cleanup

## 背景

fortune 的 `lea r15d,[rsi*4]` 这类指令在 raw lifting 后会出现 full register
load 再 `trunc` 的形状。这个形状会让 range SSA 误以为完整寄存器入口值被读取。

另一个问题是 SummarySSA rewrite 后会临时生成 `notdec.reg.insert.*`，如果马上被
`trunc` 取回刚插入的 range，应该在 helper 还没 lowering 前直接化简。这个化简不该放在
final cleanup 里；final cleanup 只负责 lowering、DCE、死寄存器和 metadata 清理。

## 实现

- `include/notdec-bin2llvm/passes/summary/NativeRegisterPeephole.h:12`：
  新增 `NativeRegisterPreSummaryPeepholeSummary`，声明 pre-summary peephole。
- `include/notdec-bin2llvm/passes/summary/NativeRegisterPeephole.h:23`：
  新增 `NativeRegisterPostRewritePeepholeSummary`，声明 post-rewrite peephole。
- `lib/passes/summary/NativeRegisterPeephole.cpp:29`：
  `fullRegisterLoadGlobal()` 识别带 `notdec.register.access` 的 full-width register load。
- `lib/passes/summary/NativeRegisterPeephole.cpp:52`：
  `narrowUseFromFullLoad()` 匹配 `load -> trunc` 和 `load -> lshr const -> trunc`。要求
  full load 只有一个 use；`lshr` 也必须只有一个 use。
- `lib/passes/summary/NativeRegisterPeephole.cpp:111`：
  `rewriteLoad()` 将匹配到的 narrow use 改成
  `notdec.partial_read.iFULL.iREAD(@REG, offset)`。
- `lib/passes/summary/NativeRegisterPeephole.cpp:159`：
  `findInsertedRangePiece()` 只在 request range 完全等于某个 inserted value 时替换，遇到
  overlap 或子 range 不完全一致时保守跳过。
- `lib/passes/summary/NativeRegisterPeephole.cpp:200`：
  `simplifyInsertedValueExtract()` 匹配 `trunc(full)` 和 `trunc(lshr full, C)`。
- `lib/passes/summary/NativeRegisterPeephole.cpp:243`：
  `runNativeRegisterPreSummaryPeephole()` 执行 raw full-load narrow-use 化简。
- `lib/passes/summary/NativeRegisterPeephole.cpp:266`：
  `runNativeRegisterPostRewritePeephole()` 执行 inserted-range extract 化简。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:6370`：
  在 `canonicalizeRegisterPointerPhiLoads()` 后、ABI/register summary 分析前默认运行
  pre-summary peephole。
- `tools/notdec-native-llvm.cpp:1082`：
  新增 `runPostRewritePeepholePass()`。
- `tools/notdec-native-llvm.cpp:1130` 和 `tools/notdec-native-llvm.cpp:1265`：
  IR 输入和 ELF 输入的默认链路都在 prototype/signature rewrite 后、final cleanup 前运行
  post-rewrite peephole。
- `include/notdec-bin2llvm/passes/summary/NativeRegisterFinalCleanup.h` 和
  `lib/passes/summary/NativeRegisterFinalCleanup.cpp`：
  删除 final cleanup 里的 inserted-range extract 化简计数和实现。
- `lib/CMakeLists.txt:17`：
  将 `NativeRegisterPeephole.cpp` 加入 `notdec-bin2llvm-core`。
- `tests/native_register_summary_ssa_test.cpp:6691`：
  覆盖 direct full load trunc、shift trunc、multi-use full load skip。
- `tests/native_register_summary_ssa_test.cpp:6989`：
  覆盖 post-rewrite peephole 从 inserted range 里直接取低位，并确认后续 cleanup 能清掉死
  partial read。

## 验证

```bash
cmake --build external/NotDec-bin2llvm/build --target native_register_summary_ssa_test notdec-native-llvm -j4
ctest --test-dir external/NotDec-bin2llvm/build -R notdec.native_register_summary.ssa --output-on-failure
```

结果：通过。

fortune：

```bash
OUT=/tmp/notdec-bin2llvm-fortune-peephole-default-rerun-20260716
external/NotDec-bin2llvm/build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  -o "$OUT/fortune.native.ll" \
  --all-confirmed --skip-runtime \
  --external-prototypes /sn640/NotDec-Exp/Bench2/bin2llvm-external-prototypes/fortune-executable.external-prototypes.json \
  --register-ssa-warning-out "$OUT/register-ssa-warnings.tsv"
llvm-22.1.0.obj/bin/llvm-as "$OUT/fortune.native.ll" -o "$OUT/fortune.native.bc"
llvm-22.1.0.obj/bin/opt -passes=verify "$OUT/fortune.native.bc" -disable-output
python3 external/NotDec-bin2llvm/scripts/native-register-residue-audit.py --details "$OUT/fortune.native.ll"
```

结果：

- `stderr.log` 为空。
- `llvm-as` 和 `opt -passes=verify` 通过。
- `FUN_32e0` 保持 `%0 = shl i32 %RSI.arg, 2`。
- `FUN_32e0` 第一处 `__fprintf_chk` 参数保持为
  `(..., i64 2, i64 24585, i64 %2, i64 24841)`。
- residue audit 只剩 `FUN_32e0` 的 `ZMM1` range entry：
  `notdec.partial_read.i512.i64(@ZMM1, 64)`。
- warning 文件仍是 6 条签名相关 warning：
  `open` vararg tail、`_ITM_registerTMCloneTable`、`recode_delete_request`、
  `recode_new_outer`、`recode_scan_request`、`recode_string`。

## 评价

- 实现效果：4/5。两个 peephole 都进入默认 native 链路，fortune 关键 callsite 参数没有回退。
- 复杂度：2/5。把两个局部模式放在一个 peephole 组件里，没有新增 CLI 或旁路链路。
- 维护成本：2/5。final cleanup 职责更清楚，但后续如果 range helper 增多，需要继续把
  helper-level 化简放在 peephole 阶段，而不是塞回 final cleanup。
