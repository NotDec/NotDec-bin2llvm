# Native low-bit extract cleanup

## 背景

fortune 的 `lea r15d,[rsi*4]` 这类指令在 raw lifting 中会出现 full register
load 再 `trunc` 的形状。这个形状会让后面的 range SSA 认为高位入口值也被读取。
直接在 SummarySSA 前把它改成 `partial_read` 容易影响 callsite 参数绑定，所以这次只把
raw peephole 做成独立 pass，并在 final cleanup 增加一个后置化简。

## 实现

- `include/notdec-bin2llvm/passes/summary/NativeRegisterLowBitDemandPeephole.h`：
  新增 standalone summary 和入口声明。
- `lib/passes/summary/NativeRegisterLowBitDemandPeephole.cpp:27`：
  识别带 `notdec.register.access` 的 full-width register load。
- `lib/passes/summary/NativeRegisterLowBitDemandPeephole.cpp:50`：
  匹配 `load -> trunc` 和 `load -> lshr const -> trunc`。要求 full load 只有一个 use；
  `lshr` 也必须只有一个 use。
- `lib/passes/summary/NativeRegisterLowBitDemandPeephole.cpp:109`：
  将匹配到的 narrow use 改成 `notdec.partial_read.iFULL.iREAD(@REG, offset)`。
- `lib/CMakeLists.txt`：
  把新的 peephole cpp 加入 `notdec-bin2llvm-core`。
- `include/notdec-bin2llvm/passes/summary/NativeRegisterFinalCleanup.h`：
  summary 增加 `ValueRangeExtractsSimplified`。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:151`：
  增加 inserted range 查找逻辑，只在 request range 完全等于某个 inserted value 时替换。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:207`：
  后置匹配 `trunc(full)` 和 `trunc(lshr full, C)`。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:485`：
  在 lower `notdec.reg.insert/extract` 之前先跑后置化简。
- `tests/native_register_summary_ssa_test.cpp:6671`：
  覆盖 direct full load trunc、shift trunc、multi-use full load skip。
- `tests/native_register_summary_ssa_test.cpp:6989`：
  覆盖 final cleanup 从 inserted range 里直接取低位，确认死 high partial read 会被清掉。

## 验证

```bash
cmake --build external/NotDec-bin2llvm/build --target native_register_summary_ssa_test notdec-native-llvm -j4
ctest --test-dir external/NotDec-bin2llvm/build -R notdec.native_register_summary.ssa --output-on-failure
```

结果：通过。

fortune：

```bash
OUT=/tmp/notdec-bin2llvm-fortune-lowbit-cleanup-final-20260716
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

- native run `elapsed=12.17s`。
- `llvm-as` 和 `opt -passes=verify` 通过。
- `FUN_32e0` 现在使用 `%RSI.arg` 做 `shl i32 %RSI.arg, 2`，不再有 RSI 的
  `range_entry` / `partial_read` 残留。
- 第一处 `__fprintf_chk` 参数保持为
  `(..., i64 2, i64 24585, i64 %2, i64 24841)`，没有回退成全 0。
- residue audit 只剩 `FUN_32e0` 里的一个 `ZMM1` range entry：
  `notdec.partial_read.i512.i64(@ZMM1, 64)`。这个不是本次 low-bit integer extract pass
  的覆盖范围。

## 评价

- 实现效果：4/5。解决了 fortune 当前 RSI 低位入口残留，不破坏 callsite 参数重写。
- 复杂度：2/5。新增一个 standalone peephole 和一个 final cleanup 小匹配器，逻辑局部。
- 维护成本：2/5。默认链路只启用后置 cleanup；早期 peephole 保留给后续实验，避免影响
  SummarySSA 输入语义。
