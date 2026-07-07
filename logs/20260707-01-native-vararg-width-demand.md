# Native vararg tail width demand cleanup

## 背景

fortune 里 `FUN_4750` 调 `open` 时，`open` 已知是 vararg，固定参数只有两个。旧逻辑在 `NativeRegisterSummary` 里把已知 vararg 外部调用的所有 ABI 输入都按完整寄存器拉活，导致 `RCX/R8/R9` 高 32 位也被认为是入口需求。后面 SummarySSA 为了满足这个假需求，会生成 `partial_read(@RCX, 32)`，让寄存器 global 残留到最终 IR。

本次只修宽度，不处理 `open` 是否应该删除多余 vararg 参数的问题。

## 实现

- `lib/passes/summary/NativeRegisterSummary.cpp:4` 引入 `NativeExternalPrototype`，让 summary 阶段复用默认外部原型表，不再维护一份手写 vararg 名单。
- `lib/passes/summary/NativeRegisterSummary.cpp:120` 在 `AbiFacts` 里保留 ABI 输入顺序，用于判断哪些寄存器属于 vararg tail。
- `lib/passes/summary/NativeRegisterSummary.cpp:657` 和 `:664` 改成从默认外部原型表判断 vararg 以及 fixed arg 数量。
- `lib/passes/summary/NativeRegisterSummary.cpp:681` 记录输入寄存器顺序。
- `lib/passes/summary/NativeRegisterSummary.cpp:1504` 对已知 vararg 外部调用，fixed 参数保留 ABI mask，超过 fixed 的 tail 参数只按低 32 位加 demand，避免误拉高 32 位入口值。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:147` 在 call-arg binding 里记录实际 value type。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:4397` 对 vararg tail 参数读低 32 位，不再默认按完整 i64 读取。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:5554` 重写 vararg 调用时只追加类型仍匹配的 tail 参数，避免 localize 后重新混入不匹配的整寄存器值。

## 验证

构建：

```bash
cmake --build build --target notdec-native-llvm -j4
```

fortune：

```bash
OUT=/tmp/notdec-bin2llvm-fortune-vararg-width3-20260707054325
./build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  -o "$OUT/fortune.native.ll" --all-confirmed --skip-runtime \
  --register-ssa-warning-out "$OUT/register-ssa-warnings.tsv"
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as "$OUT/fortune.native.ll" -o "$OUT/fortune.native.bc"
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify "$OUT/fortune.native.bc" -o /dev/null
```

结果：

- `elapsed=11.41s`
- `register_global_defs=0`
- `partial_read=0`
- `summary_return=0`
- `summary_clobber=0`
- `summary_ssa_metadata=0`
- `REG_REF_R8_R9=0`
- `REG_REF_RCX=8` 只剩局部 SSA 名字里的 `RCX`，没有 `@RCX` / `partial_read` / `RCX.range_entry`。

## 评估

- 实现效果：8/10。fortune 的真实寄存器残留清零，且 `open` 仍保留当前签名行为，符合“先修宽度”的范围。
- 复杂度：6/10。新增逻辑只接入已有外部原型表和 ABI 输入顺序，没有新 pass，但 vararg tail 统一按 32 位仍是 x64 当前链路里的保守规则。
- 维护成本：5/10。后续修 `open` 多余参数时，需要把参数数量推断和这里的宽度规则统一起来。
