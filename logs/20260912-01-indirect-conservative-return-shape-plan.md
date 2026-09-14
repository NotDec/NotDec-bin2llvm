# Indirect callsite return shape: live filter + conservative single-class rule

## 背景

第 8 代之后，`callRangeValue()` 会为每个 ABI 输出寄存器生成
`RangeReturnHelper` 占位，它表示“调用后状态”，不再表示返回结论。直接外部调用
的 `addDemandedExternalReturns()` 已经用 live-use / 旧返回类型 / 保守规则来筛选
真正的返回槽；但 `addIndirectCallsiteShapes()` 仍然把每个 `RangeReturnHelper`
都当成 return slot。

wrk 的 `http_parser_execute` 暴露了这个问题：

- 34 个间接 callback 调用被重写成
  `call { i512, i64, i512, i80, i64 }`；
- 实际只有 RAX（element 1）和部分 RDX（element 4）被 `extractvalue` 使用；
  XMM0/XMM1/ST0 对应的 element 0/2/3 没有任何使用者；
- 这些未读输出和 live RDX 一起进入调用返回类型，导致 IR 体积、PHI 和 clobber
  诊断都被放大。

## 最小修改

1. `addIndirectCallsiteShapes()` 只把有真实 SSA use 的
   `RangeReturnHelper` 作为读取证据，未读的 after-call ABI 输出占位不再进入
   返回形状。
2. 抽出 `addConservativeUnknownReturnSlots()`，让直接未知外部和间接调用共用
   同一套保守返回类规则：
   - 旧 LLVM 返回类型非 void 时作为最强证据；
   - 浮点返回（XMM0 优先，ST0 仅在有 x87 证据时）和整型返回互斥，只取一类；
   - 整型返回从 RAX 开始，RDX 默认排除，ZMM1 等后续 SSE 输出一律当 clobber；
   - void call 才退回 live read evidence，且只看真实读取的返回槽。
3. 删除只服务于“把每个 range helper 都变成返回值”的
   `addReturnSlotForRange()`。

该修改不改变间接调用参数/clobber 推断，也不改变已确认的原型行为；直接未知外部
返回逻辑只是抽取复用，语义保持不变。

## 验证

- 新增 `testIndirectConservativeReturnShapeExcludesRdx`：
  多输出 ABI（RAX/RDX/XMM0/ST0），间接调用后同时读 RAX 和 RDX；断言 call 仍为
  `i64`，live RDX 不会被提升成第二返回值，未读 XMM0/ST0 也不进入返回形状。
- `native_register_summary_ssa_test` 通过。
- 全量 ctest：12/12 通过。
- 重建 `notdec-native-llvm` 后生成 wrk：
  - 5 元组间接返回从 34 个降到 0；
  - 9 个 `{ i32, i16 }`、2 个 `{ i64, i1 }` 是直接外部/内部 call 的合法结构，
    不再有间接调用聚合返回；
  - IR 行数从 22,900 降到 22,803，字节数从 1,771,590 降到 1,764,893；
  - `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as` + `opt -passes=verify` 通过；
  - warning 总数暂未变化（703），说明 clobber / 参数侧仍是下一步。

## 取舍与后续

- RDX 默认排除会牺牲“未知间接调用返回 int64 结构体”的情况，这与直接未知外部
  的保守策略一致；需要时仍可通过可信原型进入精确路径。
- `http_parser_execute` 的 239 条 `remaining_summary_clobber_value` 和 32 条
  `call_arg_uses_unknown_value` 仍未解决；下一步应继续收紧间接调用参数/clobber
  的数据流，而不是继续放宽返回形状。
- 本次不改变部分写、x87 和 partial-demand 的既有修复。

## 后续修订

间接调用参数 evidence 修复见
`logs/20260912-02-indirect-call-input-evidence-plan.md`。该修复完成后 wrk 总
warning 从 703 降到 392，`http_parser_execute` 的 271 条 warning 已清零。
