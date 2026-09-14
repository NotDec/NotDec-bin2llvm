# Indirect call input evidence

## 背景

`http_parser_execute` 里的间接 callback 调用只应有 1 个或 3 个参数：

- `int (*http_cb)(http_parser*)`：RDI。
- `int (*http_data_cb)(http_parser*, const char*, size_t)`：RDI/RSI/RDX。

但 lifted IR 在 `addIndirectCallsiteShapes()` 里先用
`addAbiInputParamSlots()` 把 RDI/RSI/RDX/RCX/R8/R9/XMM* 全部加成参数，随后
`refineIndirectCallsiteParamShapes()` 依赖 `collectSignatureCallArgs()` 的当前
SSA 值做裁剪。入口残留值和 call clobber 值也有 SSA 定义，于是被误认为是参数，
callback 调用最终可能带 5/6 个参数，并在 `http_parser_execute` 留下大量
`remaining_summary_clobber_value` 和 `call_arg_uses_unknown_value`。

## 根因

`NativeRegisterSummary` 已经为每个 callsite 收集了
`NativeRegisterCallsiteSlotEvidence`，包含 `Indirect` 标记和每个 ABI 输入槽的
来源：

- `LocalDefinition`
- `ForwardedEntry`
- `CallClobber`
- `Mixed`

但 `inferExternalCallShapes()` 对 `Indirect == true` 的 callsite 直接 `continue`，
没有使用这份 evidence；SummarySSA 侧的间接调用参数边界因此退回“所有 ABI 输入 +
当前 SSA 值裁剪”的过估方案。

## 实现

1. 抽出 `makeSignatureSlotForCallInput()`，让 per-callsite evidence 可以独立于
   `FunctionBuilder` 转成 `NativeSignatureSlot`。
2. 新增 `inferIndirectCallInputs()`：
   - 读取 `baseRegisterSummary.ExternalCallsites` 中 `Indirect == true` 的项；
   - 对整数和浮点分别用 `localDefinitionPrefix()` 数连续 `LocalDefinition`
     （允许 wrapper 式 `ForwardedEntry` 前缀）；
   - 按原始 callsite 顺序取出对应前缀 slot，存入
     `SignatureRewriteState::IndirectCallInputs`。
3. `addIndirectCallsiteShapes()` 优先使用 `IndirectCallInputs` 构造参数；
   没有 evidence 时才回退到旧的 `addAbiInputParamSlots()`。

该修改只改间接调用**参数形状**，不改变返回形状、direct external 原型和
clobber 语义。

## 验证

- 新增 `testIndirectCallInputsUseLocalDefinitionEvidence`：
  - 有 RDI/RSI/RDX 的局部写，RCX/R8/R9 无本地证据；
  - 断言间接调用重写后参数数量为 3，而不是全部 ABI 输入。
- `native_register_summary_ssa_test` 通过。
- 全量 ctest：12/12 通过。
- wrk `http_parser_execute`：
  - 修复前：271 条 warning（239 clobber + 32 unknown call arg）；
  - 修复后：0 条。
  - callback 调用参数从 5/6 个降到 1/2/4/5 等由 evidence 决定的数量。
- wrk 全量 warning：703 -> 392。
- wrk IR：22,803 行 / 1,764,893 字节 -> 20,793 行 / 1,491,308 字节。
- `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as` + `opt -passes=verify` 通过。

## 剩余

- 392 条 warning 中仍有：
  - 166 条 `unknown_external_arity_stopped_at_call_clobber`（Lua API 等未知外部）；
  - 98 条 `remaining_summary_clobber_value`；
  - 39 条 `inferred_unknown_external_arity`；
  - 30 条 `call_arg_rewrite_missing_binding`。
- 下一步可以优先补 Lua C API 可信原型，或继续收紧其余不确定外部调用的 clobber
  参数证据。
