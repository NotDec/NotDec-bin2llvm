# Native signature rewrite in SummarySSA plan

用户原始要求：

> 感觉既然之前都做register消除了，那是不是应该同步也做函数签名的修改。详细规划一下，是合并到前面的registerSSA，还是说怎么做？

> NativeExternalCallSignatureRewrite当前做的很奇怪，考虑完全重新写新的pass，同时处理internal函数和external函数的rewrite，logs/20260617-01-native-external-call-signature-rewrite/20260617-01-native-external-call-signature-rewrite-plan.md这个规划里面，提到了实现前要参考 LLVM 自己的函数复制和函数类型改写代码，避免漏掉属性和 metadata。

> 参数就严格按照read_entry那边使用到的值来，如果出现了跳过某个寄存器的情况，就按数量更多最多的那个值对应的数量处理，就当做前面传过参数，但是没有被用上。当前的理解是对的。返回值的处理是看top down的分析，分析所有call site中caller使用到的callee修改过的寄存器的值，然后是取并集。只要有任何一个caller用了就当做它用过了。对于external的函数，就按ABI假设返回值寄存器都有值即可，然后看用了多少寄存器。当前的理解看着问题不太大。indirect call / address-taken internal function要改问题也不大，没必要太小心，可以按照参数和返回值的数量偏多的角度考虑，反正调用前都要给它强制类型转换为对应的函数指针类型。关于SummarySSA，确实当前可能导出的不太多，思路上就是按需导出额外的信息即可，之前插入的不需要的导出信息可以直接去掉。目前看下来，我严重怀疑反正summary ssa也是要重写IR，而且目前和函数签名重写的耦合非常严重，我严重怀疑这两个步骤应该一起进行，即让summary ssa也负责函数签名的修改

> 名字可能可以不用改。先更新plan吧，然后写一个goal按这个实现吧，之前实现的没用就都清理掉，比如NativeExternalCallSignatureRewrite

## 背景

当前 summary 链路已经做了两件事：

- `NativeRegisterSummary` 计算每个函数对寄存器的 read/modify/preserve/return demand。
- `NativeRegisterSummarySSA` 根据 summary 构建寄存器 SSA，替换 register load，并清理一部分 register residue。

之前单独写过一个 `NativeExternalCallSignatureRewrite`，只处理 external call 参数：

```llvm
store i64 %x, ptr @RDI
call void @free()
```

改成：

```llvm
call void @free(i64 %x)
```

这个 pass 现在不适合作为后续基础。原因很简单：

- 它只处理 external declaration，不处理 internal `notdec_native_*`。
- 它只处理参数，不处理返回值。
- 它需要 SummarySSA 额外导出 operand bundle / metadata，实际是在跨 pass 传递 SummarySSA 的内部状态。
- 函数签名 rewrite 需要替换 entry register load、callsite 参数、return helper、callee return，这些都和 SummarySSA 构建 SSA value 的过程强耦合。

所以后续不再扩展 `NativeExternalCallSignatureRewrite`。
签名改写并入 `NativeRegisterSummarySSA` 的 rewrite 流程；名字可以先不改。

## 目标

SummarySSA 直接负责 native register calling convention 到 LLVM function signature 的改写。

目标 IR 形态：

```llvm
define i64 @notdec_native_1234(i64 %arg0, i64 %arg1) {
  ...
  ret i64 %ret
}

%r = call i64 @notdec_native_1234(i64 %a0, i64 %a1)
```

而不是继续保留：

```llvm
store i64 %a0, ptr @RDI
store i64 %a1, ptr @RSI
call void @notdec_native_1234()
%r = call i64 @notdec.register.summary_return.i64()
```

这一步仍然不是类型恢复。
参数和返回类型先使用寄存器宽度对应的整数类型；多返回值使用 LLVM struct。

## 总体路线

运行顺序保持 summary 链路：

```text
NativeRegisterSummary
  -> NativeRegisterSummarySSA
       - 构建 register SSA
       - 确定每个函数的 ABI 参数和返回值形状
       - 重写 internal/external/direct/indirect callsite
       - 重写 internal function type、entry input、return
       - 清理对应 register store/load/helper residue
  -> InstCombine / DCE
```

`NativePrototypeRecovery` 属于 heritage 链路，不能参与这里。
`NativeExternalCallSignatureRewrite` 作为旧 external-only 实现删除。

## 参数规则

### internal 函数

参数严格来自 callee 的 `read_entry`。

按 ABI 参数寄存器顺序，例如 x86-64 SysV：

```text
RDI, RSI, RDX, RCX, R8, R9
```

如果 callee 的 `read_entry` 使用了 `RDX`，即使 `RDI` / `RSI` 没用，也认为函数有 3 个参数。
前面未使用的 slot 只是“传了但 callee 没用”。

规则：

```text
param_count = max(read_entry ABI input register index) + 1
```

callee body 中：

```llvm
%RDI.entry = load i64, ptr @RDI
```

替换为对应 LLVM argument。

未被 callee 读取的中间参数 slot 可以存在于签名里，但 callee body 不使用它。

### external 函数

常见 libc / runtime symbol 优先使用已知签名表确定参数数量。

未知 external 的参数数量按 callsite 中能确定的 ABI 参数数量合并。
如果不同 callsite 不统一，按偏多处理，避免少传。

external 的真实 C 类型仍不恢复，参数类型用整数寄存器类型。

### indirect call / address-taken internal

不因为 indirect call 或 address-taken 自动跳过。

处理原则：

- 函数本体按偏多的统一 signature 改写。
- direct call 直接按新 FunctionType 创建新 call。
- indirect call 在创建 call 时使用目标 FunctionType；opaque pointer 下不依赖 callee pointer 类型。
- 需要时在 call 前插入 cast/bitcast 形态的适配，但 LLVM opaque pointer 下重点是 call 使用的 `FunctionType`。

## 返回值规则

返回值来自 top-down demand。

internal 函数：

```text
返回寄存器集合 =
  所有 caller 使用过的、callee 确实会修改的 ABI output 寄存器的并集
```

也就是只要任何 caller 使用了 callee 修改后的某个返回寄存器，就把这个寄存器放进 callee return shape。

常见规则：

```text
0 个返回寄存器 -> void
1 个返回寄存器 -> 对应整数类型
多个返回寄存器 -> LLVM struct
```

external 函数：

- 按 ABI 假设 output 寄存器都有值。
- 仍然用 caller 侧实际 demand 判断用了多少返回寄存器。
- 多个返回寄存器同样用 struct 表达。

callsite 重写后：

```llvm
%call = call i64 @foo(...)
```

替换原来的：

```llvm
%RAX.return = call i64 @notdec.register.summary_return.i64()
```

多返回时：

```llvm
%ret = call { i64, i64 } @foo(...)
%rax = extractvalue { i64, i64 } %ret, 0
%rdx = extractvalue { i64, i64 } %ret, 1
```

callee return 重写时，在每个 `ret void` 前读取对应返回寄存器的当前 SSA value，生成新的 `ret`。

## callsite 不统一的处理

核心原则：同一个 callee 只有一个统一签名。

参数不统一：

- internal 函数按 callee `read_entry` 得到统一参数数量。
- external known prototype 按已知数量。
- unknown external / indirect 情况按所有 callsite 可见参数数量取最大。
- 某个 callsite 缺少未使用 slot 的值，用 `undef`。
- 某个 callsite 缺少实际使用 slot 的值，也先用 `undef`，但计入 warning / summary，后续用真实用例判断是否要收紧。

返回不统一：

- 返回寄存器集合取所有 caller demand 的并集。
- 某个 callsite 不使用某个返回 slot，不生成 extract，或生成后交给 DCE。
- 如果 callsite 原先存在某个 return helper，但统一 return shape 没有这个寄存器，说明 summary/demand 不一致，记录 warning；实现上优先保证 IR 可验证。

这样不会出现同一个 `notdec_native_*` 在不同 callsite 变成多个 LLVM 函数类型。

## 和 SummarySSA 的关系

签名改写不再作为独立 pass 消费 SummarySSA metadata。
SummarySSA 内部已经能回答这些问题：

- call 前某个 ABI 参数寄存器的 SSA value 是什么。
- callee 是否读取 entry 寄存器。
- call 后某个 ABI output 寄存器是否是 return value。
- 某个参数 store / return helper 是否只是在服务 ABI register passing。

因此实现时应该减少临时导出：

- 可以删除旧 external-only operand bundle / call_arg_store metadata。
- 如需调试，可以保留统计或 debug metadata，但不能作为两个 pass 之间的硬接口。
- 之前为了 `NativeExternalCallSignatureRewrite` 加的无用导出信息应清理。

## LLVM 函数复制和类型改写参考

LLVM 不能直接修改 `FunctionType`。
改 internal function 或 external declaration 时，需要新建函数，再替换调用点和函数体。

实现前必须参考这些源码，避免漏掉属性和 metadata：

- `/sn640/NotDec/llvm-source/llvm/lib/Transforms/Utils/CloneModule.cpp`
  - `CloneModule` 创建新 `Function` 后调用 `copyAttributesFrom`。
  - 对 declaration 会 `getAllMetadata` 后逐个复制 metadata。
- `/sn640/NotDec/llvm-source/llvm/lib/Transforms/Utils/CloneFunction.cpp`
  - `CloneFunctionAttributesInto` 复制 global/function 级信息，再重建 `AttributeList`。
  - `CloneFunctionMetadataInto` 用 `getAllMetadata` / `MapMetadata` 复制 function metadata。
- `/sn640/NotDec/llvm-source/llvm/lib/Transforms/IPO/DeadArgumentElimination.cpp`
  - 改函数类型时新建 `Function`，复制 attributes、comdat，再重建 callsite。
  - 重建 callsite 时保留 calling convention、callsite attributes、operand bundles、tail call kind、`prof/dbg` metadata。
- `/sn640/NotDec/llvm-source/llvm/lib/Transforms/IPO/ArgumentPromotion.cpp`
  - 展示了新建函数、复制 metadata、重建 callsite、保留 tail call kind 的做法。

需要保留：

- linkage、visibility、DLL storage、unnamed addr、section、partition。
- comdat、GC、personality、prefix/prologue data。
- function attrs、return attrs、calling convention。
- function metadata。
- callsite tail kind、calling convention、attributes、operand bundles、`prof/dbg` metadata。

不能只写裸的：

```cpp
Function::Create(NewTy, ExternalLinkage, Name, M)
```

## 清理旧实现

后续实现时清理：

- `include/notdec-bin2llvm/passes/summary/NativeExternalCallSignatureRewrite.h`
- `lib/passes/summary/NativeExternalCallSignatureRewrite.cpp`
- `tests/native_external_call_signature_rewrite_test.cpp`
- `tools/notdec-native-llvm.cpp` 中独立调用 external rewrite pass 的入口。
- `lib/CMakeLists.txt` / 顶层测试目标里的旧 pass 和旧测试。

旧 external-only 的能力不能丢：

- external call 参数仍要改成 LLVM call operand。
- known libc prototype 仍可保留。
- 已确认只服务于 call 的 register store 仍要删除。

但这些能力都应迁移到 SummarySSA rewrite 流程里。

## 判断标准

最小 internal 用例：

```llvm
define void @notdec_native_callee() {
  %RDI.entry = load i64, ptr @RDI
  store i64 %RDI.entry, ptr @RAX
  ret void
}

define void @notdec_native_caller() {
  store i64 7, ptr @RDI
  call void @notdec_native_callee()
  %r = load i64, ptr @RAX
  ret void
}
```

改写后应接近：

```llvm
define i64 @notdec_native_callee(i64 %arg0) {
  ret i64 %arg0
}

define void @notdec_native_caller() {
  %r = call i64 @notdec_native_callee(i64 7)
  ret void
}
```

还需要覆盖：

- external direct call 参数和返回值。
- internal direct call 参数和返回值。
- skipped ABI input slot，例如只读 `RDX` 时生成 3 个参数。
- 多返回寄存器，生成 struct return。
- 同一 callee 多 callsite 参数数量不统一时按最大数量统一。
- 同一 callee 多 callsite 返回 demand 不统一时按并集统一。
- indirect call / address-taken internal function 按统一 FunctionType 创建 call。
- tail call 保留 tail kind；不使用 `musttail`。

验证：

- 新增 / 改写 SummarySSA 单元测试。
- 删除旧 external-only pass 测试，迁移有效 case 到 SummarySSA 测试。
- 用 LLVM 22：
  - `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as`
  - `/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify`
- 对 Bench2 当前 fortune 用例同口径记录运行时间和 register residue：
  - `register_access_metadata`
  - `loads_from_register_globals`
  - `stores_to_register_globals`

## 实现记录：SummarySSA 内建 signature rewrite

本次已经按计划把 signature rewrite 并入 SummarySSA，并删除旧 external-only pass。

主要改动：

- [NativeRegisterSummarySSA.h](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/passes/summary/NativeRegisterSummarySSA.h:22)
  - 增加 `CallsRewritten`、`FunctionsRewritten` 统计。
- [NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:69)
  - 新增 `SignatureShape` / `SignatureRewriteState`。
  - internal 参数来自 callee `read_entry`，按 ABI 参数寄存器最大 index 决定参数数量。
  - internal 返回值来自 `ExitDemand && MayNonEntry` 的 ABI output register。
  - external 参数使用 known prototype；unknown external 先按 ABI 输入寄存器偏多处理。
  - external 返回值按实际 `summary_return` helper demand 加入 shape。
- [NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:1380)
  - 新建 replacement function，复制 attributes、metadata、comdat、calling convention。
  - internal function body 移到新函数，entry register load 替换成 LLVM argument。
  - 函数 `ret void` 根据返回寄存器 SSA value 改成 `ret value` 或 struct return。
- [NativeRegisterSummarySSA.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/summary/NativeRegisterSummarySSA.cpp:1487)
  - callsite 直接改成带 LLVM operand 的 call。
  - call return helper 替换为 call result 或 `extractvalue`。
  - 被消费的 ABI 参数 store 删除。
- [tools/notdec-native-llvm.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:7)
  - 删除独立 `NativeExternalCallSignatureRewrite` 调用，默认 summary pipeline 只跑 SummarySSA。
- 删除旧 external-only 文件：
  - `include/notdec-bin2llvm/passes/summary/NativeExternalCallSignatureRewrite.h`
  - `lib/passes/summary/NativeExternalCallSignatureRewrite.cpp`
  - `tests/native_external_call_signature_rewrite_test.cpp`

测试覆盖：

- [native_register_summary_ssa_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_register_summary_ssa_test.cpp:386)
  - external call 参数 store 直接由 SummarySSA 改成 call operand。
- [native_register_summary_ssa_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_register_summary_ssa_test.cpp:417)
  - internal function 参数和返回值改写。

验证：

```text
cmake --build /tmp/notdec-bin2llvm-build --target native_register_summary_test native_register_summary_ssa_test notdec-native-llvm -j2
/tmp/notdec-bin2llvm-build/bin/native_register_summary_test
/tmp/notdec-bin2llvm-build/bin/native_register_summary_ssa_test

/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  --all-confirmed \
  -o /tmp/notdec-fortune-summary-signature/fortune.ll \
  --summary-json-out /tmp/notdec-fortune-summary-signature/summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-fortune-summary-signature/fortune.ll -o /tmp/notdec-fortune-summary-signature/fortune.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-fortune-summary-signature/fortune.bc -o /tmp/notdec-fortune-summary-signature/fortune.verified.bc
```

fortune 结果：

```text
fortune native pipeline: 10.13 s
register_access_metadata: 422
loads_from_register_globals: 82
stores_to_register_globals: 417
summary_ssa_call_args_metadata: 0
native_call_with_args: 29
summary_return_helpers: 55
```

和上一版同口径相比：

```text
register_access_metadata: 508 -> 422
loads_from_register_globals: 108 -> 82
stores_to_register_globals: 503 -> 417
```

剩余问题：

- 还有一部分 `notdec.register.summary_return.*` helper，主要是当前没有纳入 ABI return shape 的寄存器效果。
- 现在 direct call 已覆盖，indirect call / address-taken 的强制 FunctionType 适配还需要继续补。
- 当前 external unknown 参数按 ABI 输入偏多处理，会产生一些 `undef` 参数；这是按本阶段“偏多优先”的规则实现的。

复杂度评估：

- 实现效果：7/10。internal/external 参数和 ABI 返回值已经进入 SummarySSA 主链路，fortune residue 明显下降。
- 理解成本：6/10。SummarySSA 现在同时负责 SSA 和 signature rewrite，逻辑集中但文件更重。
- 维护成本：5/10。少了跨 pass metadata，但函数类型重写需要持续用真实 Bench2 用例校验。
