# Php Phi Incoming Verification

## 原始 prompt

```text
参考logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/GOAL.md，当前目标是进一步拓展最后一步，完善bench2上的效果。对于/sn640/NotDec-Exp/Bench2/plan.md里面的每个目标项目，都选择一个动态链接库或者二进制程序，修复转IR过程的问题，同时解决生成的IR里面明显的语义不一致。过程中的log文件记录到logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/08-more-bench2文件夹内。
```

## 当前目标和已有 native 状态

Bench2 计划里 php 优先选扩展 `.so`。当前已有单函数回归来自 `/usr/bin/php8.3` 的 `zm_info_date` 相关函数：

```text
/sn640/NotDec-Exp/Bench2/bin2llvm-ir/php/one-zm_info_date.json
```

这个 JSON 的 P-Code check 已通过。旧 lower log 记录过 PHI verifier 错误：

```text
Instruction does not dominate all uses!
PHI nodes not grouped at top of basic block!
module verification failed
```

当前构建重新 lower 后已经通过。本记录确认旧 PHI 错误已经消失，生成 IR 中 PHI 在 block 顶部，incoming 读值在对应 predecessor 内。

## Ghidra 相关实现

Ghidra heritage 输出里的 `MULTIEQUAL` 表示 SSA PHI：

- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/pcode/PcodeOp.java`
  - `MULTIEQUAL` 是 P-Code PHI。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - heritage/SSA 阶段创建和整理 MULTIEQUAL。

LLVM IR 的约束更严格：PHI 必须位于 basic block 顶部，incoming value 必须支配对应 predecessor edge。

## native 侧复刻策略

本次不新增代码，先用当前构建验证 php 旧失败是否已经消失：

1. 单函数 `one-zm_info_date.json` 重新 lower。
2. 检查生成 IR 里 `bb:2` / `bb:4` 的 PHI 是否在 block 顶部。
3. 用 LLVM 22 `llvm-as` 和 `opt -passes=verify` 做最终判断。

## 判断标准

1. `php/one-zm_info_date.json` 能生成 `.ll`。
2. 生成 `.ll` 能用 LLVM 22 `llvm-as` 和 `opt -passes=verify` 验证。
3. 已有 libicu data-library 小回归仍能 lower。

## 风险

1. 这次只证明当前 php 单函数已恢复，不代表所有复杂 PHI/CFG 已完整正确。
2. 如果后续扩到 php extension module，仍可能遇到新的 CFG 或 indirect branch 问题。

## 实现记录

### 修改内容

没有新增代码改动。本目标记录当前构建对 php 旧 PHI lowering 失败的验证结果。

相关代码位置：

- `lib/HeritageToLLVM.cpp:1507`
  - `lowerPhi(...)` 创建 LLVM PHI，并把 incoming 延后到 `finalizePendingPhis(...)` 填充。
- `lib/HeritageToLLVM.cpp:1529`
  - `resizeForPhiIncoming(...)` 在 incoming predecessor terminator 前插入 resize。
- `lib/HeritageToLLVM.cpp:1560`
  - `readPhiIncoming(...)` 在 predecessor terminator 前读取 register / address-tied input。
- `lib/HeritageToLLVM.cpp:1600`
  - `finalizePendingPhis(...)` 按 `block.In` 给 PHI 填 incoming。

### 验证

当前重新 lower：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-heritage-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/php/one-zm_info_date.json \
  -o /tmp/php-one-current.ll
```

结果：通过，生成 `/tmp/php-one-current.ll`。

LLVM 22 验证：

```bash
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/php-one-current.ll -o /tmp/php-one-current.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/php-one-current.bc -o /tmp/php-one-current.verified.bc
```

结果：全部通过。

IR 大小：

```text
86 /tmp/php-one-current.ll
4.3K /tmp/php-one-current.ll
3.0K /tmp/php-one-current.bc
```

关键 IR 片段：

```llvm
"bb:2":
  %"vn:1134" = phi i64 [ %"vn:1296.mem3", %entry ], [ %"vn:1296.mem2", %"bb:1" ]
  %"vn:1099" = phi i32 [ %"vn:1298.mem4", %entry ], [ %"vn:1298.mem", %"bb:1" ]
  %"vn:651" = phi i64 [ %"vn:1296.mem1", %entry ], [ %3, %"bb:1" ]
  call void (...) @php_info_print_table_start()

"bb:4":
  %"vn:645" = phi i64 [ 5358447, %"bb:2" ], [ 5329068, %"bb:3" ]
  call void (...) @php_info_print_table_row(...)
```

libicu data-library 回归：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/dynamic-libs/libicu/data-library/module-all.json \
  -o /tmp/libicu-data-after-php.ll
```

结果：

```text
internal declarations: 5
external declarations: 2
lowered function bodies: 5
failed function bodies: 0
```

### 性能和判断

本次没有新增代码，不引入性能变化。php 单函数 IR 很小，验证成本可以忽略。

实现效果：3/5。旧 PHI verifier 错误已经消失，单函数能生成可验证 IR。

复杂度：1/5。仅记录当前构建验证。

维护成本：1/5。后续应继续选 php extension `.so` 做 module 级验证。
