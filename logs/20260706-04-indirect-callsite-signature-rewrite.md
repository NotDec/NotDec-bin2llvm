# Indirect callsite signature rewrite

## 背景

`php calendar.so` 里寄存器全局已经消掉了，但还有 indirect call 后的返回 helper：

```llvm
call void %3()
%RAX.return = call i32 @notdec.register.summary_return.i32()
```

这些 helper 表示“这个 call 后面读取了 RAX 的返回值”。原先签名重写只按 direct callee 的 `Function*` 建 shape，indirect call 没有 `getCalledFunction()`，所以不会进入参数推断和调用重写，helper 会残留。

## 修改

- `lib/passes/summary/NativeRegisterSummarySSA.cpp:263` 在 `SignatureRewriteState` 增加 `IndirectCallShapes`，把 indirect call 的推断签名挂在具体 callsite 上。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1328` 新增 `signatureHasReturnForRegister()`，避免同一个返回寄存器重复加入返回列表。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1338` 新增 `addReturnSlotForRange()`，对 indirect call 的返回 range 使用整寄存器返回，再由 `extractReturnRange()` 拆出低/高位。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1355` 新增 `addAbiInputParamSlots()`，先给 indirect callsite 放入 ABI 输入寄存器槽，后续复用现有参数推断来截断。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1370` 新增 `addIndirectCallsiteShapes()`，从 `ReturnHelpers` / `RangeReturnHelpers` 给 indirect callsite 建返回 shape。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1622` 新增 `refineIndirectCallsiteParamShapes()`，用 `callsiteBoundArgPrefix()` 复用已有参数数量推断。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1684` 新增 `shapeForCall()`，统一 direct function shape 和 indirect callsite shape 的查询。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1745` 在 `rewriteLoads()` 后、`collectSignatureCallArgs()` 前构造 indirect callsite shape。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:4188` 让 `collectSignatureCallArgs()` 支持 indirect callsite shape。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:5322` 让 `rewriteSignatureShapes()` 对 indirect call 使用 `functionTypeForShape()` 重建 typed indirect call，并复用已有 return helper 替换逻辑。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:5616` 在 external arity refine 后增加 indirect callsite arity refine。
- `tests/native_register_summary_ssa_test.cpp:1584` 新增 `testIndirectCallReturnHelperIsRewritten()`，覆盖 `call void %fp()` 后读取 RAX 的场景。
- `tests/native_register_summary_ssa_test.cpp:6300` 把新测试接入 `main()`。

## 验证

构建和单测：

```bash
cmake --build external/NotDec-bin2llvm/build --target notdec-native-llvm native_register_summary_ssa_test -j4
external/NotDec-bin2llvm/build/bin/native_register_summary_ssa_test
```

结果：通过。

`php calendar.so`：

```bash
external/NotDec-bin2llvm/build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/php/20230831/calendar.so \
  -o /tmp/notdec-bin2llvm-php-calendar-indirect-call-20260706162155/native.ll \
  --all-confirmed --skip-runtime \
  --register-ssa-warning-out /tmp/notdec-bin2llvm-php-calendar-indirect-call-20260706162155/register-ssa-warnings.tsv
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-bin2llvm-php-calendar-indirect-call-20260706162155/native.ll -o /tmp/notdec-bin2llvm-php-calendar-indirect-call-20260706162155/native.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-bin2llvm-php-calendar-indirect-call-20260706162155/native.bc -disable-output
```

结果：

- 输出路径：`/tmp/notdec-bin2llvm-php-calendar-indirect-call-20260706162155/native.ll`
- 运行时间：`elapsed=17.44`
- `integer_register_refs=0`
- `all_register_metadata_globals=0`
- `summary_helpers=0`
- `summary_ssa_metadata=0`
- `raw_notdec_register=0`
- indirect call 已重写成带真实返回值和参数的 call，例如 `call i64 %3(...)`。

回归：

- `lighttpd-angel` 输出路径：`/tmp/notdec-bin2llvm-lighttpd-angel-indirect-call-20260706162254/native.ll`，`elapsed=1.38`，寄存器和 summary 残留全为 0。
- `fortune` 输出路径：`/tmp/notdec-bin2llvm-fortune-indirect-call-20260706162255/native.ll`，`elapsed=12.05`，寄存器和 summary 残留全为 0。

## 评价

- 实现效果：5/5。`calendar.so` 的 indirect call return helper 被消除，三个当前关注样例都达到寄存器残留清零。
- 复杂度：3/5。没有改 SummarySSA 主数据流，只是在签名重写层增加 callsite 级 shape；但 indirect call 参数和返回都进入了同一条重写路径，需要维护 direct/indirect 两种 shape 来源。
- 维护成本：3/5。后续如果要做更准的函数指针类型合并，可以在 `IndirectCallShapes` 上继续汇总；当前实现按 callsite 保守落地，不强行猜全局函数指针类型。

## Direct external return-only 补充修复

`wrk` 暴露了另一类相近问题：有些 direct external call 没有参数证据，只有后续对返回寄存器 range 的 demand。旧逻辑只重写 `CallArgs` 里出现过的 callsite，所以这类调用会先生成 `summary_return` helper，后续 signature rewrite 又不会把 call 改成真正返回值。

本次只补这个整数返回路径，不处理 `wrk` 里剩余的 x87 `ST*` 和 `ZMM0` partial read/write。

### 修改

- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1271` 新增 `integerReturnSlotForUnit()`，优先按 ABI output slot 保留返回寄存器的 offset/size。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1421` 修改 `addDemandedExternalReturns()`，即使 external callee 初始没有 shape，也会为有返回 demand 的 call 建 shape。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:1706` 补 `extractReturnRange()` 前置声明，供 cleanup 阶段复用已重写 call 的返回值。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:4134` 修改 `callEffect()`，post-signature cleanup 遇到已重写 call 时，如果签名说明该寄存器是返回值，就继续按 `ReturnValue` 处理。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:4457` 修改 `callRangeValue()`，已重写 call 的 return range 直接从 call 结果拆，不再重新生成 `summary_return` helper。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:4490` 修改 `signatureReturnUsesUnit()`，让 indirect callsite shape 也能参与返回寄存器判断。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:5348` 修改 `rewriteSignatureShapes()`，除了参数证据，也用 `ReturnHelpers` / `RangeReturnHelpers` 决定 call 是否要重写。
- `tests/native_register_summary_ssa_test.cpp:1570` 更新 `testExternalReturnUsesRangeCallValue()`，要求 direct external call 被重写成真实 integer return，且 `summary_return.i64` 不再残留。

### 验证

构建和单测：

```bash
cmake --build build --target native_register_summary_ssa_test notdec-native-llvm -j4
./build/bin/native_register_summary_ssa_test
```

结果：通过。

`wrk`：

```bash
external/NotDec-bin2llvm/build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk \
  -o /tmp/notdec-bin2llvm-wrk-return-rewrite3-20260706165326/native.ll \
  --all-confirmed --skip-runtime \
  --register-ssa-warning-out /tmp/notdec-bin2llvm-wrk-return-rewrite3-20260706165326/register-ssa-warnings.tsv
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-bin2llvm-wrk-return-rewrite3-20260706165326/native.ll -o /tmp/notdec-bin2llvm-wrk-return-rewrite3-20260706165326/native.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-bin2llvm-wrk-return-rewrite3-20260706165326/native.bc -disable-output
```

结果：

- 输出路径：`/tmp/notdec-bin2llvm-wrk-return-rewrite3-20260706165326/native.ll`
- 运行时间：`elapsed=81.73`
- `integer_register_refs=0`
- `summary_helpers=0`
- `register_metadata_globals=8`
- `summary_ssa_metadata=65`
- `raw_notdec_register=95`
- 剩余寄存器集中在 `@ZMM0` partial read/write 和 x87 `@ST0` 到 `@ST7`，不是这次 direct external integer return 修复范围。

### 评价

- 实现效果：4/5。整数寄存器和 summary helper 已清零，direct external return-only 场景补上了；浮点和 SIMD 残留仍需单独分析。
- 复杂度：2/5。没有改主数据流，只把返回 demand 纳入已有签名重写和 cleanup 路径。
- 维护成本：2/5。逻辑仍集中在 signature rewrite 层，后续如果扩展浮点返回，可以沿用同一入口。
