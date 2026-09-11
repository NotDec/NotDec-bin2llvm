# Partial-demand poison replacement

用户原始要求：

> 然后把相关输入替换成零感觉是危险的设计，之前不是考虑全部去掉这种东西吗，然后改成，让没有用到的值变成poison value。这样可以把问题至少暴露出来。搜一下之前的历史log，难道这个是漏网之鱼？

## 背景

`NativeRegisterSummarySSA::rewritePartialWrites()` 的 backward partial-demand
rewrite 在判断某个寄存器 store 数据流 operand 没有 demand 时，当前用整数常量
`0` 替换 operand。这个策略来自 2026-06-26 的第一版 partial register demand
实现，后续虽然 unknown fallback 统一加强了来源标记，但这条 zero-demand store
路径仍然保留了真实常量。

`script_parse_url` 已经证明该设计会隐藏分析错误：返回绑定在第一次
partial-demand rewrite 之后才登记，漏掉的返回观察使 RAX 链被静默清成 `0`。

## 目标

- 不再由 zero-demand rewrite 静默制造整数常量 `0`。
- 用 LLVM 裸 `poison` 保留“这个值不应被依赖”的诊断信号，使遗漏的 demand
  或过早的 binding 能在 IR 中暴露。
- 保留 `notdec.register.summary_ssa.zero_demand_operand` metadata；它描述的
  是 demand mask 为零，而不是替换值的类型。
- 补回归测试，确认替换 operand 确实是 `PoisonValue`，并验证模块合法。

## 最小修改方案

1. 将 `undemandedStoreOperandZero()` 改为返回整数类型的
   `llvm::PoisonValue::get(type)`，调用端用 `llvm::Value *` 接收。
2. 更新注释和 metadata 增加 `replacement=poison`，不改 metadata key，避免扩大
   兼容面和清理逻辑。
3. 更新 partial-demand 单测；不重写其他 unknown fallback，也不改变 demand
   transfer 规则、signature rewrite 顺序或 residue cleanup。
4. 更新当前架构说明和原 partial-demand plan 的后续修订记录；历史提交内容不改。

## 取舍与风险

- 选择裸 `poison` 而非 `freeze poison`：目标是让错误在最终 IR 中可见；代价是
  如果 demand 分析确实漏掉真实 observer，LLVM 优化可能沿 poison 传播并改变控制流
  或把结果判为 poison，这正是需要暴露的错误，不应被静默掩盖。
- 只替换现有逻辑已经允许的整数 operand；普通内存 pointer、非整数值和有真实
  demand 的 operand 仍按原规则处理。
- 需要检查 `wrk`、`fortune` 的 IR 是否出现预期的 poison/metadata，是否有新的
  verifier、崩溃或性能退化。LLVM IR 只用 `/sn640/NotDec/llvm-22.1.0.obj`
  验证。

## 判断标准

- `native_register_summary_ssa_test` 和对应 ctest 通过。
- `wrk`、`fortune` 生成成功；`llvm-as` 与 `opt -passes=verify` 通过。
- `script_parse_url` 不再静默生成由 partial-demand 引入的恒定 `0`；若返回
  绑定仍然遗漏，IR 能看到 `poison` 或对应 metadata/warning。
- 对比修改前后的生成时间、IR 体量和 partial-demand 统计，确认影响局限于诊断
  值语义。

## 实现记录

改动文件：

- `lib/passes/summary/NativeRegisterSummarySSA.cpp:457-492`：保留
  `zero_demand_operand` 标记，并记录 `replacement=poison`。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:3189-3198`：将
  `undemandedStoreOperandZero()` 改为 `undemandedStoreOperandPoison()`，返回
  `llvm::PoisonValue`。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:3369-3384`：用 `Value *`
  接收并写入 poison，保留原来的整数类型和寄存器 store 数据流边界。
- `tests/native_register_summary_ssa_test.cpp:615-640, 8615-8650`：增加
  `PoisonValue` 与 `replacement=poison` 的回归断言。
- `docs/analysis/simple-efficient-ai-register-elimination.md:233-237`、
  `docs/analysis/simple-efficient-ai-register-elimination.zh.md:178`：更新当前
  partial-demand 语义说明。
- `ARCHITECTURE.md:330-331`：更新 SummarySSA 当前 pipeline 说明。
- 原 partial-demand plan 追加本次修订；早期历史日志不改，保留其作为“当时为何
  选择 0”的证据。

验证命令和结果：

- `cmake --build build --target native_register_summary_ssa_test -j4`：通过。
- `build/bin/native_register_summary_ssa_test`：通过。
- `ctest --test-dir build -R '^notdec\.native_register_summary\.ssa$' --output-on-failure`：通过。
- `ctest --test-dir build --output-on-failure`：12/12 通过，包括 native smoke 和
  x86-64/i386 fortune；测试重命名后重新构建并再次运行 SummarySSA 单测。
- 使用 `build/bin/notdec-native-llvm` 生成：
  - `/tmp/notdec-bin2llvm-partial-poison-20260911/wrk.native.ll`
  - `/tmp/notdec-bin2llvm-partial-poison-20260911/fortune.native.ll`
- 使用 `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as` 及同目录
  `opt -passes=verify` 验证 wrk/fortune：均通过。

实际效果：

- 修改前 wrk `/tmp/notdec-wrk-current-20260911.ll` 的
  `script_parse_url` 返回为 `phi i32 [ 0, %bb_e83e ], [ 0, %entry ]`；修改后为
  `phi i32 [ poison, %bb_e83e ], [ 0, %entry ]`。
- 新 wrk IR 出现 36 处 `poison`（修改前同一基线已有 5 处非本次路径的
  `poison`），fortune 出现 24 处；这说明 zero-demand rewrite 的遗漏不再被
  常量 0 静默吞掉。
- wrk IR 从 22,686 行/1,742,984 字节变为 22,285 行/1,718,440 字节；生成耗时
  `95.86s`、峰值 RSS `202432 KiB`，与此前同类 wrk 记录 `96.18s`、`207884 KiB`
  同档。fortune 本次耗时 `14.10s`、峰值 RSS `169380 KiB`；现有历史 8.x 秒
  记录来自不同 revision/参数，不能作为严格基线，但未观察到构建或 verifier
  退化。

实现效果、理解成本和维护成本：

- 实现效果：达成诊断目标；`script_parse_url` 的错误路径直接显现 poison，且
  verifier、现有单测和 Bench2 smoke 均不回退。
- 理解成本：略有增加，需要区分“zero demand”这个 metadata 名称和实际
  `poison` replacement；保留原 key 是为了维持清理与历史追踪的一致性。
- 维护成本：低；只改一个替换点和一组测试。后续若 partial-demand 规则扩大到
  非整数类型，必须重新评估 poison 是否应直接传播，以及是否需要专门的诊断
  metadata/输出，而不能重新静默填 0。
