# x87 ST0/ST1 fp80 window access canonicalization

## 背景

`main -> format_*` 的 `stack+8` 参数仍经常是 `notdec.unknown.i80`。跟踪后确认：

- callsite stack binding 已找到；
- 但绑定的值链里含 `notdec.unknown`；
- unknown 的 metadata 是：

```text
reason=range_return_helper_clobbered_register
function=main
callee=gettimeofday
register=ST0
kind=return
```

也就是说，后面写进 ST0 的真实 `x86_fp80` 没有被识别，导致 ST0 的
current-def 仍是前一次 `gettimeofday` 调用产生的 clobber 占位。

## 根因

`@ST0` / `@ST1` 是 `i80` 全局，但前面 pass/InstCombine 会把 x87 window 访问
规范成：

```llvm
store x86_fp80 %v, ptr @ST0
%v = load x86_fp80, ptr @ST0
```

而 `registerLoad()` / `registerStore()` 的 storage 判断是：

```cpp
access.IsStorageValue = valueType == global->getValueType();
```

`x86_fp80` != `i80`，所以这些访问被 summary/SSA 忽略：

- ST0 写不更新 current-def；
- ST0 读继续拿到旧的 clobber helper；
- 之后 outgoing stack store 的值就是 unknown。

## 修改

在 x86-64 SysV 路径上增加一个前置 canonicalization：

```cpp
canonicalizeX87WindowAccesses(module, units);
```

对 `@ST0` / `@ST1`：

- `load x86_fp80` -> `load i80` + `bitcast to x86_fp80`；
- `store x86_fp80` -> `bitcast to i80` + `store i80`。

这样继续复用已有的 `i80` x87 window summary/SSA 逻辑。

i386 暂时不启用这条 canonicalization，避免影响 i386 的 x87 回归预期。

## 验证

- `native_register_summary_ssa_test` 通过。
- 全量 ctest：12/12 通过。
- wrk 重新生成并通过 `llvm-as` + `opt -passes=verify`。

修复后格式函数调用点：

```llvm
%58 = call i64 @format_time_s(i80 %ST0.range_summary_ssa)
%108 = call i64 @format_time_us(i80 %ST0.range_summary_ssa5694)
%109 = call i64 @format_binary(i80 %stack.arg.cast)
%125 = call i64 @format_binary(i80 %stack.arg.cast6683)
```

`main -> format_*` 的 `stack+8` warning 全部消失。

wrk 总 warning：

```text
322 -> 330
```

warning 数略增，是因为以前被忽略的 x87 `ST0/ST1` clobber 现在可见并进入诊断；
但 formatter 的参数值链已经不再接错。
