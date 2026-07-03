# native external register SSA warning 实现记录

## 背景

fortune 里 `__ctype_b_loc`、`__ctype_toupper_loc` 这类 libc 函数原型不够时，
SummarySSA 会把调用后的 `RAX` 恢复成 `summary_return` helper；同时未知外部调用后的
`RDX` 容易被误当成第二返回值。这样既影响寄存器消除，也不方便批量定位剩余问题。

这次先补常见 zero-arg libc 返回值，并把没有强原型时的 `RDX` 当成 clobber 证据处理。
对仍然残留的 `notdec.register.summary_return/clobber` helper，增加可落盘 warning。

## 修改

- `include/notdec-bin2llvm/passes/summary/NativeRegisterSummarySSA.h:56`：
  增加 `NativeRegisterSummarySSAWarning`，记录函数、callee、寄存器、helper 类型、原因和使用数；
  `NativeRegisterSummarySSASummary:98` 增加 `Warnings`。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:183`：
  `KnownExternalPrototype` 支持 `TypedReturn`。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:301`、`467`、`642`：
  给 `__ctype_b_loc`、`__ctype_tolower_loc`、`__ctype_toupper_loc`、`getpid`、`random`
  补 `i64` 返回。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1367`：
  增加 `isIntegerAbiOutput`，避免把 XMM/ZMM float ABI output 当成 integer helper 返回。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1375`、`1429`、`2609`：
  没有强外部原型时，`RDX` 不再作为第二 integer 返回寄存器推断，改走 clobber。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1395`：
  `shapeForKnownExternal` 支持“无参数但有 typed return”的原型。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:3462`、`3496`、`3652`、`3695`：
  收集并打印残留 helper warning。
- `tools/notdec-native-llvm.cpp:63`、`87`、`224`、`537`、`960`、`980`：
  增加 `--register-ssa-warning-out <path>`，输出 TSV warning 文件；heritage SSA 模式下写空表头。
- `tests/native_register_summary_ssa_test.cpp:1579`、`2464`、`4024`、`4030`：
  新增 `__ctype_b_loc` typed return 和未知外部 `RDX` clobber 回归测试。

## 验证

编译和单测：

```bash
cmake --build build --target native_register_summary_ssa_test notdec-native-llvm -j4
./build/bin/native_register_summary_ssa_test
```

fortune smoke：

```bash
OUT=/tmp/notdec-bin2llvm-fortune-external-warning-20260703051949
./build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  --all-confirmed --skip-runtime --register-ssa-summary \
  --summary-json-out "$OUT/summary.json" \
  --register-ssa-warning-out "$OUT/register-ssa-warnings.tsv" \
  -o "$OUT/fortune.native.ll"
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as "$OUT/fortune.native.ll" -o "$OUT/fortune.native.bc"
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify "$OUT/fortune.native.bc" -o /dev/null
```

结果：

- IR：`/tmp/notdec-bin2llvm-fortune-external-warning-20260703051949/fortune.native.ll`
- warning TSV：`/tmp/notdec-bin2llvm-fortune-external-warning-20260703051949/register-ssa-warnings.tsv`
- stderr：`/tmp/notdec-bin2llvm-fortune-external-warning-20260703051949/run.stderr`
- `summary_return refs=0`
- `summary_clobber refs=23`
- `RDX.return refs=0`
- `__ctype void declares=0`
- `__ctype i64 declares=3`
- `getpid i64 declares=1`
- `random i64 declares=1`
- warning rows：`21`
- `llvm-as` 和 `opt -passes=verify` 通过。

性能：本次 fortune native run `elapsed=5.94s`；上一轮同口径记录约 `6.07s`，未见明显回退。

## 评估

- 实现效果：8/10。fortune 的 `summary_return` 和 `RDX.return` 已清零，剩余 clobber 有 warning 文件可查。
- 复杂度：4/10。主要是补原型表、窄化 ABI 判断和 TSV 输出，没有改 pass 主流程。
- 维护成本：4/10。warning reason 目前较粗，后续可以继续按 callee 原型库和调用点证据细分。

更好的长期方案是接入更完整的 libc 原型库，并用调用点前后的实际寄存器使用来推断未知外部函数的输入/输出；这次先把当前 fortune 的误判和可观测性补上。
