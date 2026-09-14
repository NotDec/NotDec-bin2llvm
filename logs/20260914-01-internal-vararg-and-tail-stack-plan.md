# Internal vararg prototype and tail-call stack forwarding

## 背景

`format_units` / `format_time_s` / `format_time_us` / `format_binary` 的
`long double` 栈参数在 summary 阶段被污染，根因不是 `fldt` lowering，
而是内部 vararg helper `aprintf` 被错误地恢复成“所有 ABI 输入寄存器都是固定
参数”。

`aprintf` 是 SysV AMD64 variadic 函数：

```c
char *aprintf(char **s, const char *fmt, ...);
```

编译器 prologue 会把 GP 参数寄存器 `RDI..R9` 和条件性地把 `XMM0..XMM7` 保存到
register save area。`NativeRegisterSummary` 看到这些入口值被读取，于是把
`ReadEntry` 全部置位；`shapeForInternalFunction()` 又只根据 `ReadEntry`
建固定参数，最终得到：

```llvm
aprintf(RDI, RSI, RDX, RCX, R8, R9, ZMM0..7, RAX)
```

这些假固定参数再沿调用图传播到 `FUN_81d0` 和所有 formatter wrapper。

## 修改

1. **给 `aprintf` 增加可信 vararg 原型**：
   ```cpp
   {"aprintf", {2, true, false, 1,
                {PointerSized, PointerSized}, PointerSized}}
   ```
   固定参数只有 `s` 和 `fmt`；其余寄存器作为 vararg tail。

2. **允许 known vararg prototype 覆盖 internal definition**：
   - `knownVarArgExternalShape()` 不再要求 callee 必须是 declaration。
   - `buildExternalCallShapes()` 对已定义的 known vararg 函数也生成 call shape。
   - `buildInitialSignatureShapes()` 对 known vararg 内部函数优先使用
     `shapeForKnownExternal()`。

3. **summary call transfer 优先使用可信 call shape**：
   - `transferCall()` 先查 `externalCallShape(call)`，再退回 internal
     `FunctionEffect`。
   - 避免 `aprintf` 的 variadic register save area 假 `ReadEntry` 往后传播。

4. **tail-call stack 参数转发**：
   - `propagateTailCallStackParams()` 让 tail caller 继承 callee 的 stack 参数；
   - `PreserveParam` 防止被 `refineInternalStackParamShapes()` 裁掉；
   - `rewriteSignatureShapes()` 对 tail call 的 stack slot，在没有显式 store
     binding 时直接使用 caller 新签名里的对应 LLVM 参数。

## 验证

- `native_register_summary_ssa_test` 通过。
- 全量 ctest：12/12 通过。
- wrk 重新生成并通过 `llvm-as` + `opt -passes=verify`。
- 关键 IR 结果：

```llvm
define internal i64 @aprintf(i64 %RDI.arg, i64 %RSI.arg, ...)

define internal i64 @FUN_81d0(i64 %RDI.arg, i32 %RSI.arg,
                              i80 %"stack+8.arg")

define internal i64 @format_time_s(i80 %"stack+8.arg")
define internal i64 @format_binary(i80 %"stack+8.arg")
define internal i64 @format_time_us(i80 %"stack+8.arg")
```

- wrk 总 warning：392 -> 342。
- `format_time_s` / `format_binary` / `FUN_81d0` 不再有
  `ZMM*.arg_unknown` 和假 R8/R9 参数。

## 剩余

- `main -> format_*` 的 `stack+8` 仍有 `call_arg_uses_unknown_value` /
  `uses_clobber_value`；说明 main 里计算并写到栈上的 long double 值还没有被
  调用点绑定识别。
- `format_time_us` / `format_binary` 调用 `FUN_81d0` 时 `RSI` 仍缺 binding，
  原因是 wrapper 内部把 `p` 写成常量 2/0，但该 store 在调用点绑定中没有被
  识别；下一步可单独修这个。
