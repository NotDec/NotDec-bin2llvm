# Native no-return summary and prototype overlay

用户原始问题：

> 默认的表里有的话，为什么fortune 的小 JSON里还要写？没必要重复啊。就算覆盖，仅覆盖对应的函数就可以吧？反正就是json支持标记noreturn属性，在默认里面还是在fotrune的里面都行啊

## 背景

fortune 的 `FUN_4db0` 里有一条失败路径调用本地 helper `FUN_27a0`。这个 helper 内部调用 `perror` 后调用 `exit`，语义上不返回。之前只截断已知外部 noreturn call，`FUN_4db0` 里 `call FUN_27a0` 后面的假 fallthrough 仍会进 return PHI，导致 RBP 被当成函数返回值，最后留下 `@RBP` 入口 load。

同时，`--external-prototypes` 加载用户 JSON 时直接替换默认表。fortune JSON 只写了 `recode_new_request`，因此默认表里的 `exit` noreturn 信息会丢失。这不是用户应该重复写 `exit`，而是加载策略应该改成 overlay。

## 实现

- `include/notdec-bin2llvm/passes/summary/NativeRegisterSummary.h:23`：`NativeExternalCallShape` 增加 `NoReturn`，让 RegisterSummary 能消费外部原型里的 noreturn。
- `include/notdec-bin2llvm/passes/summary/NativeRegisterSummary.h:120`、`:134`：公共 summary 增加函数级 `NoReturn` 和模块级计数，方便调试。
- `lib/passes/summary/NativeRegisterSummary.cpp:105`、`:142`：在 forward state 和 `FunctionEffect` 中记录 no-return 终止事实。
- `lib/passes/summary/NativeRegisterSummary.cpp:810`、`:1296`：no-return 路径只合并 `ReadEntry`，不把它当正常返回出口。
- `lib/passes/summary/NativeRegisterSummary.cpp:1446`、`:1466`：forward transfer 和 callsite evidence replay 遇到 no-return 后停止扫同一基本块后续旧指令。
- `lib/passes/summary/NativeRegisterSummary.cpp:1889`：call transfer 遇到外部或内部 noreturn callee 后把当前状态标成无 fallthrough。
- `lib/passes/summary/NativeRegisterSummary.cpp:2139`：backward demand 遇到 noreturn call 清掉 call 后面的假 live，避免制造虚假的返回 demand。
- `lib/passes/summary/NativeRegisterSummary.cpp:2304`、`:2346`、`:2412`：把 noreturn 写入 LLVM function attribute、metadata 和 summary 打印。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:410`：`truncateKnownNoReturnCalls()` 支持外部 prototype 和内部 summary fact 两种来源。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1458`：从 known external prototype 构造 call shape 时保留 `NoReturn`。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:6346`：用户 JSON 改为覆盖同名 prototype，不再替换默认原型表。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:6388`、`:6407`：base summary 后先截断内部 noreturn fallthrough，final summary 再在清理后的 CFG 上跑。
- `tests/native_register_summary_ssa_test.cpp:247`、`:4763`、`:4815`、`:7011`：补充 JSON overlay 保留默认 `exit` noreturn，以及内部 helper noreturn 截断测试。

## 验证

```bash
cmake --build external/NotDec-bin2llvm/build --target native_register_summary_ssa_test -j4
external/NotDec-bin2llvm/build/bin/native_register_summary_ssa_test
cmake --build external/NotDec-bin2llvm/build --target native_register_summary_test -j4
external/NotDec-bin2llvm/build/bin/native_register_summary_test
cmake --build external/NotDec-bin2llvm/build --target notdec-native-llvm -j4
```

fortune 验证：

```bash
OUT=/tmp/notdec-bin2llvm-fortune-noreturn-final-20260715
external/NotDec-bin2llvm/build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  -o "$OUT/fortune.native.ll" \
  --all-confirmed --skip-runtime \
  --external-prototypes /sn640/NotDec-Exp/Bench2/bin2llvm-external-prototypes/fortune-executable.external-prototypes.json \
  --summary-json-out "$OUT/summary.json" \
  --register-ssa-warning-out "$OUT/register-ssa-warnings.tsv"
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as "$OUT/fortune.native.ll" -o "$OUT/fortune.native.bc"
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify "$OUT/fortune.native.bc" -disable-output
python3 external/NotDec-bin2llvm/scripts/native-register-residue-audit.py --details "$OUT/fortune.native.ll"
```

结果：

- 输出 IR：`/tmp/notdec-bin2llvm-fortune-noreturn-final-20260715/fortune.native.ll`
- `FUN_27a0` 被标记为 `noreturn`。
- `FUN_4db0` 从 `i64 @FUN_4db0(...)` 变成 `void @FUN_4db0(i64 %RDI.arg)`。
- `@RBP`、`RBP.entry`、`summary_return`、`summary_clobber` 均不再出现。
- 剩余寄存器残留为 `FUN_32e0` 的 `RSI` high32 partial read 和 `ZMM1` lane partial read，和本次 RBP/no-return 问题无关。

## 评估

- 实现效果：4/5。RBP 假返回路径消除，fortune 上 RBP 残留清零；剩余两个 partial read 需要另行处理。
- 复杂度：2/5。只在现有 summary lattice 上加函数级 noreturn bit，没有新建分析框架。
- 维护成本：2/5。外部 JSON overlay 更符合使用预期；noreturn fact 后续也可被其它 native pass 复用。
