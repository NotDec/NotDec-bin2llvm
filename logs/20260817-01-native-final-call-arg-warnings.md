# Native 最终调用参数警告

## 用户原始 prompt

> 那按这个改进一下警告

## 背景

Native SummarySSA 会先按初始签名收集调用参数，随后再根据所有调用点裁剪内部
函数的栈参数。当前参数收集阶段立即写入 `call_arg_binding_*` 警告，因此已经被
最终签名裁掉的参数仍会留在 warning TSV 中。wrk 的 `stats_stdev stack+8` 就是
这种情况：最终调用只有一个寄存器参数，但旧的栈参数预警仍在。

最终签名重写阶段已经会检查每个实际参数，并报告缺失 binding、unknown 值和
clobber 值。收集阶段的预警既早于签名收敛，也和最终检查重复。

## 目标

- 警告只描述最终生成的调用参数，不报告已被签名裁掉的候选参数。
- 最终调用中真实存在的缺失、unknown 和 clobber 参数继续报告。
- 不改变参数推断、签名裁剪和生成 IR 的语义。

## 路线

让 `callArgStoreBindings()` 只收集参数值和截断前缀，不直接写警告。继续由
`rewriteSignatureShapes()` 按最终 `SignatureShape` 统一输出参数警告。增加内部
栈参数因调用点无有效 binding 而被裁掉的回归测试，确认最终调用为零参数且没有
旧的 `call_arg_binding_*` 警告。

## 风险和判断标准

主要风险是删掉预警后遗漏真实问题。判断时要确认最终重写的缺失、unknown 和
clobber 分支仍被测试覆盖，并用 wrk 对照 warning reason 分布；`stats_stdev` 的
旧 `stack+8` 警告应消失，输出 IR 必须不变并通过 LLVM 22 verifier。

本次不调整参数推断规则，也不处理 Heritage 测试。

## 实现记录（已完成）

### 修改

- `lib/passes/summary/NativeRegisterSummarySSA.cpp:6067` 的
  `callArgStoreBindings()` 不再写入收敛前的 `call_arg_binding_*` 警告；缺失或
  clobber binding 仍用于参数前缀裁剪。
- 同文件 `rewriteSignatureShapes()`（`:7715`）保存新 callee 的最终名字，最终
  参数和 return helper 警告都使用该名字。实现时发现旧 call 指向的函数已经被
  `takeName()` 清空，wrk 原有 56 条最终参数警告因此没有 callee；这部分随最终
  诊断统一一起修正。
- `tests/native_register_summary_ssa_test.cpp:2937` 的
  `testTrimmedInternalStackInputDoesNotLeaveBindingWarning()` 覆盖内部栈参数被裁掉
  后不留预警；`:3008` 的 `testFinalCallArgClobberWarningIsKept()` 确认最终
  clobber 参数仍报告，并带正确的 `free` callee 名字。

### 验证

- `native_register_summary_ssa_test`：通过。
- bin2llvm CTest 排除已知失败的 Heritage 用例：16/16 通过。
- wrk `--all-confirmed --skip-runtime`：1 分 57 秒，峰值 RSS 199676 KiB，退出码
  为 0。warning 从 849 条降到 692 条，删除的正好是 128 条
  `call_arg_binding_uses_clobber_value` 和 29 条 `call_arg_binding_missing`。
- 最终参数警告保持 102 条不变：50 条 unknown、30 条 missing、21 条
  clobber、1 条 vararg；callee 从 56 条空名字变为 0 条空名字。
- `stats_stdev stack+8` 的两条失效警告消失；`stats_stdev` 仍返回
  `x86_fp80`，`sqrtl` 仍为 `x86_fp80 (x86_fp80)`。
- wrk 输出通过 LLVM 22 `llvm-as` 和 verifier。

### 评分

| 维度 | 评分 | 判断 |
| --- | ---: | --- |
| 实现效果 | 9/10 | 只删除收敛前噪声，最终问题数量不变且定位信息完整。 |
| 理解成本 | 3/10 | 诊断点统一到最终 rewrite；只需额外保存重写后的 callee 名字。 |
| 维护成本 | 2/10 | 后续参数裁剪不需要同步维护一套 warning 回收规则。 |

相比给 warning 增加阶段标识并在末尾过滤，直接让最终 shape 成为唯一参数诊断来源
更简单，也不会依赖 call 指针在多轮重写后的存活情况。
