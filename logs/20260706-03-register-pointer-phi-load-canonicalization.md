# Register pointer PHI load canonicalization

## 背景

`lighttpd-angel` 里出现了这类残留：

```llvm
%storemerge.in = phi ptr [ @R12, ... ], [ @R13, ... ]
%storemerge = load i64, ptr %storemerge.in
```

原始汇编是按路径选择 `r12/r13` 的值，不是普通内存间接读。这个形状来自 LLVM InstCombine 把多个寄存器全局 load 折成了 `load(phi ptr @REG...)`。SummarySSA 只识别直接读写寄存器全局，所以这个 load 没被寄存器消除处理。

## 修改

- `lib/passes/summary/NativeRegisterSummarySSA.cpp:533` 新增 `registerAccessNode()`，按实际 `RegisterUnit` 重建 `notdec.register.access` metadata，避免新插入的 load 继承错误寄存器名。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:586` 新增 `registerGlobalFromValue()`，只接受带 `notdec.register` 的寄存器全局。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:597` 新增 `valueTypeMatchesRegisterStorage()`，只处理 load 类型等于寄存器全局存储类型的安全场景。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:602` 新增 `canonicalizeRegisterPointerPhiLoads()`，把非 atomic/volatile 的 `load(phi ptr @REG...)` 改写为各前驱上的 direct register load 加 join block 的 value PHI。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:5458` 在 `runNativeRegisterSummarySSA()` 里提前收集寄存器全局，并在 `runNativeRegisterSummary()` 前运行该 canonicalization。
- `tests/native_register_summary_ssa_test.cpp:306` 新增 `hasLoadFromPhiPointer()`。
- `tests/native_register_summary_ssa_test.cpp:1247` 新增 `testRegisterPointerPhiLoadIsCanonicalized()`，覆盖 `phi ptr [@R12], [@R13]` 后再 load 的场景。
- `tests/native_register_summary_ssa_test.cpp:6248` 把新测试接入 `main()`。

本次只处理 load-through-PHI，不处理 store-through-PHI，避免扩大语义风险。

## 验证

构建和单测：

```bash
cmake --build external/NotDec-bin2llvm/build --target notdec-native-llvm native_register_summary_ssa_test -j4
external/NotDec-bin2llvm/build/bin/native_register_summary_ssa_test
```

结果：通过。

`lighttpd-angel`：

```bash
external/NotDec-bin2llvm/build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/lighttpd-angel \
  -o /tmp/notdec-bin2llvm-lighttpd-angel-phi-canon-20260706141751/native.ll \
  --all-confirmed --skip-runtime \
  --register-ssa-warning-out /tmp/notdec-bin2llvm-lighttpd-angel-phi-canon-20260706141751/register-ssa-warnings.tsv
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-bin2llvm-lighttpd-angel-phi-canon-20260706141751/native.ll -o /tmp/notdec-bin2llvm-lighttpd-angel-phi-canon-20260706141751/native.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-bin2llvm-lighttpd-angel-phi-canon-20260706141751/native.bc -disable-output
```

结果：

- 输出路径：`/tmp/notdec-bin2llvm-lighttpd-angel-phi-canon-20260706141751/native.ll`
- 运行时间：`elapsed=1.38`
- 搜索 `R12/R13/RDX/storemerge.in/notdec.register.summary_`：无残留。
- warning 只剩 `_ITM_registerTMCloneTable` 的未知外部签名推断。

`fortune`：

```bash
external/NotDec-bin2llvm/build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  -o /tmp/notdec-bin2llvm-fortune-phi-canon-20260706141752/native.ll \
  --all-confirmed --skip-runtime \
  --register-ssa-warning-out /tmp/notdec-bin2llvm-fortune-phi-canon-20260706141752/register-ssa-warnings.tsv
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-bin2llvm-fortune-phi-canon-20260706141752/native.ll -o /tmp/notdec-bin2llvm-fortune-phi-canon-20260706141752/native.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-bin2llvm-fortune-phi-canon-20260706141752/native.bc -disable-output
```

结果：

- 输出路径：`/tmp/notdec-bin2llvm-fortune-phi-canon-20260706141752/native.ll`
- 运行时间：`elapsed=11.97`
- 搜索整数寄存器全局、`notdec.register.summary_`、`notdec.register.summary_ssa`：无残留。
- warning 仍是未知外部签名推断，没有新增寄存器残留。

## 评价

- 实现效果：4/5。解决了当前 `lighttpd-angel` 的寄存器指针 PHI load 残留，也没有影响 fortune。
- 复杂度：2/5。是 SummarySSA 前的局部 canonicalization，没有改 SSA 主算法。
- 维护成本：2/5。规则保守，只接受全部 incoming 都是同类型寄存器全局的 load PHI；后续如果要支持 select 或 store-through-PHI，可以单独扩展。
