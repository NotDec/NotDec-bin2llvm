# Native SummarySSA unknown/warning 改进

## 背景

最近排查 fortune 里 `__fprintf_chk(..., 0, 0)` 一类问题时，发现两件事容易混在一起：

- external call 后继续读取 volatile 寄存器，前面的 summary/shape 分析应该尽量拦住；rewrite 阶段如果还读到，只能说明前面判断或 CFG 有问题，应该报警。
- rewrite 找不到参数或返回值 binding 时，暂时可以保留 unknown fallback，但必须能从 warning 文件和 IR metadata 里看出来源，不能静默生成假值。

## 实现

- `lib/passes/summary/NativeRegisterSummarySSA.cpp:367`
  - 增加 `attachUnknownValueMetadata()`，给 SummarySSA 内部生成的 `notdec.unknown.*` 标 `!notdec.unknown.source`。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1760`
  - 增加更细的 warning helper，记录 callsite、寄存器、原因。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1842`
  - external unknown arity / vararg 推断在遇到 call-clobber 截断时写 warning。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:2408`、`:5122`
  - external call 建模只保留 ABI preserved 寄存器和 segment base；其他非明确 return 的寄存器按 clobber。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:4434`
  - range SSA fallback unknown 带 function/register/range metadata。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:5307`
  - call 参数 binding 不再因为 clobber 直接判死；读到 clobber/unknown 时保留值并写 warning。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:5616`
  - return binding 同样保留 unknown fallback，但写 warning 和 metadata。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:6541`
  - rewrite 阶段不再二次判定“是不是参数”；按已确定 shape 取值，缺失或类型不对时生成带 metadata 的 unknown。
- `lib/HeritageToLLVM.cpp:686`
  - lifting 阶段 register input temp、stack input temp、unmodeled varnode 的 unknown 加 `!notdec.unknown.source`。
- `tests/native_register_summary_ssa_test.cpp:1395`、`:2197`、`:3057`、`:5530`
  - 更新测试期望：volatile after external call 是 clobber；常量 0 vararg 保留真实 0；unknown float clobber 不生成 whole-ZMM helper。

## 验证

```bash
cmake --build external/NotDec-bin2llvm/build --target native_register_summary_ssa_test -j4
external/NotDec-bin2llvm/build/bin/native_register_summary_ssa_test
cmake --build external/NotDec-bin2llvm/build --target notdec-native-llvm -j4
```

fortune smoke：

```bash
OUT=/tmp/notdec-bin2llvm-fortune-unknown-metadata-20260719-051712
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

- IR：`/tmp/notdec-bin2llvm-fortune-unknown-metadata-20260719-051712/fortune.native.ll`
- warning：`/tmp/notdec-bin2llvm-fortune-unknown-metadata-20260719-051712/register-ssa-warnings.tsv`
- stderr：`/tmp/notdec-bin2llvm-fortune-unknown-metadata-20260719-051712/run.stderr`
- `llvm-as` / `opt -passes=verify` 通过。
- `rg "call .*@notdec\\.unknown" fortune.native.ll | awk '!/!notdec\\.unknown\\.source/'` 无输出，说明 unknown call 都带 source metadata。
- fortune 本次 elapsed 约 `12.83s`。

warning 分类：

- `inferred_unknown_external_arity`: 5
- `unknown_external_arity_stopped_at_call_clobber`: 3
- `vararg_evidence_stopped_at_call_clobber`: 22
- `call_arg_uses_unknown_value`: 46
- `vararg_arg_uses_unknown_value`: 4
- `range_return_helper_rewrite_missing_value`: 32

## 评价

- 实现效果：7/10。现在不再静默造 fake 0，unknown 和 clobber 都能从 warning/metadata 追。
- 复杂度：5/10。主要是补齐已有 SummarySSA/rewrite 逻辑，没有引入新分析。
- 维护成本：5/10。warning reason 增多，后续需要保持命名稳定。

更好的后续方向是继续追 `call_arg_uses_unknown_value` 和 `range_return_helper_rewrite_missing_value` 的真实来源，而不是在 rewrite 阶段继续猜。
