# 原始 prompt

```text
对，按照思路都修一下试试
```

（上下文：上一轮修掉 escape 栈对象的 dead-store 删除后，接着审计"5 个函数为什么
从模块里消失"，以及 `notdec.unknown` 为什么被生成。本轮先结掉其中一条已定位的
根因：`main` 里 `FUN_a800` 的函数体整个消失。）

# 背景（FUN_a800 消失的定位过程）

现象：

- `--no-register-ssa-pass --no-instcombine-pass` 的 IR 里 `FUN_a800` 有 4 个基本块、
  142 条指令，含 `call void @zcalloc()`、`call void @memcpy()`；
- 最终 IR 里只剩
  `%RAX.range_unknown = call i64 @notdec.unknown.i64(); ret i64 %RAX.range_unknown`；
- `--no-summary-register-residue-removal` 也是被掏空的状态。

在 `NativeRegisterSummarySSA` 里临时插桩（在 first/base/final truncate、builder、
rewriteSignatureShapes、cleanup iteration、post-rewrite instcombine 之后各打一次
`FUN_a800` 的块/指令/调用列表）后，链条清楚：

1. `rewriteSignatureShapes` 之后仍有 4 块 / 164 指令，`zcalloc`/`memcpy` 都在；
2. residue 清理第一轮开头的 `runPostRewriteInstCombine()`（InstCombine+SimplifyCFG）
   之后变成 1 块 / 6 指令，`zcalloc`/`memcpy` 和另外 3 个块一起消失；
3. 打开 rewrite 后的 IR 看到 entry 里的分支条件链是：

   ```llvm
   %RAX.partial_range.covered = call i32 @notdec.reg.extract.i64.i32(i64 %1, i64 0)
   %2 = lshr i32 %RAX.partial_range.covered, poison, !notdec.register.summary_ssa.zero_demand_operand !201
   ```

   即 `lshr` 的**移位量被 partial demand 换成了 poison**（原始形态是
   `%unique_32880_4 = and i32 %EDX, 31`）。`br i1 poison` 允许 InstCombine/SimplifyCFG
   任选分支，于是另一条分支上的 `zcalloc`/`memcpy` 整块被当不可达删除。

# 根因

`lib/passes/summary/NativeRegisterSummarySSA.cpp` 的 `computePartialDemands()`
worklist 里，`Shl`/`LShr` 在**移位量不是常量**时只给操作数 0 入队 demand，完全没给
移位量本身入队；`AShr` 更是任何情况都不动操作数 1。于是移位量 demand 为 0，
`rewriteUndemandedRegisterStoreOperands()` 把它整块替换成 poison
（该函数只对"整个操作数 demand 为 0"下毒），结果变 poison，分支被折叠。

对动态移位量本来就没有逐位映射：任何源位都可能被移到 demanded 位置，移位量本身也
完全参与结果。两者都必须保持全量 demand。

# 目标

1. 动态移位量的两个操作数保持全量 demand，不再被替换成 poison。
2. wrk：`FUN_a800` 恢复 `zcalloc`/`memcpy` 与 4 块结构；`notdec.unknown` 数量只减不增；
   warning TSV / residue / define 数 / promotion 计数不劣化。
3. fortune、ctest 不变；性能不退化。
4. 加一个能复现该 bug 的回归测试（不带修复时必须失败）。

# 不做什么

- 不改 partial demand 的整体模型，不动 `AShr`（常量移位）与 `Select` 条件的既有行为。
- 不动 `notdec.unknown` 的表示形式（freeze 方案已在 07 号 log 中否掉）。
- 不做通用 range/bit 值域分析。

# 设计（最小修改）

`computePartialDemands()` 的 worklist switch 中，把 `Shl`/`LShr`/`AShr` 合并处理：

- 移位量不是 `ConstantInt`：对操作数 0 和操作数 1 都入队 `fullMaskFor(...)`；
- 移位量是常量：`Shl` 走 `shlSourceDemand`、`LShr` 走 `lshrSourceDemand`（原逻辑），
  `AShr` 保持原来的 `enqueue(操作数 0, inputDemand)` 不收紧。

判断标准：修复后 `FUN_a800` 的分支保持活值；反向验证（临时回退修复）时新测试必须
失败。

# 实现记录

- `lib/passes/summary/NativeRegisterSummarySSA.cpp`（`FunctionBuilder::computePartialDemands`
  的 worklist switch）：`Shl`/`LShr`/`AShr` 三个 case 合并；动态移位量分支新增
  `enqueue(inst->getOperand(0), fullMaskFor(...))` 与
  `enqueue(inst->getOperand(1), fullMaskFor(...))`；常量分支按 opcode 分派到
  `shlSourceDemand`/`lshrSourceDemand`/`inputDemand`。
- `tests/native_register_summary_ssa_test.cpp`：新增
  `testPartialDemandKeepsDynamicShiftAmount()`，在 `main()` 里注册。测试构造
  `RDX = ABI 输入`、`%amount = and RDX, 31`、`%shifted = lshr RDX, %amount`、
  `%bit = and %shifted, 1`、`icmp`、`store %shifted, ptr @RDX`、条件分支
  （taken 分支调一个外部函数，避免 SimplifyCFG 把分支和条件一起折掉），断言
  rewrite 后仍存在 shift 指令且没有任何 poison 操作数。

# 验证

## 反向验证（证明测试确实钉住这个 bug）

```bash
git stash push -- lib/passes/summary/NativeRegisterSummarySSA.cpp
cmake --build build -j 8 && ./build/bin/native_register_summary_ssa_test   # exit=1
# "dynamic shift disappeared from the function"
git stash pop
```

## wrk

命令：

```bash
build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk \
  --all-confirmed --register-ssa-summary \
  --register-ssa-warning-out /tmp/nb-shift/wrk.warn.tsv \
  -o /tmp/nb-shift/wrk.native.ll
```

| 项 | 修复前 | 修复后 |
| --- | --- | --- |
| define 数 | 82 | 82 |
| `ptrtoint (ptr @...)` 使用数 | 19 | 19 |
| `notdec.unknown.iN` 调用数 | 70 | 69 |
| `range_unknown` | 30 | 29（`FUN_a800` 的那一条） |
| `range_return_helper_clobbered_register` | 34 | 34 |
| promotion 计数 | seen=54 promoted=20 slots=18 | 同 |
| warning TSV | 354 行基线 | 354 行，diff 0 |
| residue audit | 1 行 | 1 行 |
| IR 体积 | 1,789,455 B / 25,861 行 | 1,835,329 B / 26,555 行（+2.6%） |
| 性能 | 83.0s / 138MB | 81.3s / 138MB |
| LLVM 22 `llvm-as` + `opt -passes=verify` | 通过 | 通过 |
| ctest | 12/12 | 12/12（含 fortune x86_64/i386） |

`FUN_a800` 修复后的形态（faithful）：

```llvm
entry:
  %notdec_ram_ptr = inttoptr i64 %RSI.arg to ptr
  %unique_dd80_2 = load i16, ptr %notdec_ram_ptr, align 1
  %unique_32880_4 = and i32 %RDX.arg, 31
  %1 = shl nuw i32 1, %unique_32880_4
  %2 = and i32 %notdec.reg.extract.lower, %1
  %.not.not = icmp eq i32 %2, 0
  br i1 %.not.not, label %common.ret, label %bb_a808

bb_a808:
  ...
  %6 = call i64 @zcalloc(i64 %unique_4700_8)
  %7 = call i64 @memcpy(i64 %6, i64 %unique_4a00_8, i64 %4)
  br label %common.ret

common.ret:
  %RAX.range_summary_ssa = phi i64 [ %7, %bb_a808 ], [ 0, %entry ]
  ret i64 %RAX.range_summary_ssa
```

同一份修改还修掉了 `http_parser_execute` 一类大函数里 `%RDX.range_summary_ssa` phi
的 `poison` 入边（那些 poison 也是动态移位量被下毒的结果）：这些 phi 现在接真实
计算值，IR 因此长了 694 行。

# 效果与成本

- 效果：动态移位量（`bt`/`shl+and`、`shrx` 类）不再被误判为"无需求"，依赖它的
  分支不会被折叠，也不会连带删除分支上的真实调用/基本块。
- 理解成本：新增注释说明"动态移位量没有逐位映射"；`Shl`/`LShr`/`AShr` 合并到一个
  case 后常量分支用内层 switch 分派，逻辑更集中。
- 维护成本：低。`AShr` 常量移位、`Select` 条件等相邻问题保持原样，留给后续单独处理。

# 后续（本轮未做，已定位）

审计过程中确认了另一条更基础的 lifting 缺口，会单独出方案：

- x86 sleigh 把**静态地址的写**建模成"ram 空间 varnode 的赋值"
  （例如 `0x5a61: (ram,0x142e0,8) = COPY (register,0x0,8)`，
  `0x5a9b: (ram,0x14348,8) = INT_ADD (ram,0x14348,8) (register,0xa0,8)`），
  而不是 `CPUI_STORE`；
- `lib/PcodeToLLVM.cpp` 的 `write()` 只把 ram varnode 的值写进 SSA 缓存
  （`Values[key] = resized`），**不生成 store**，于是整模块所有静态地址写入都丢掉了；
- 直接证据：`main` 里 17 条 RIP 相对 store（`mov %rax,0x...(%rip)`、
  `movaps %xmm0,0x...(%rip)`）的 p-code 都在，但 raw IR 里一个对应的 store 都没有；
  `ssl_readable` 的唯一引用（`0x5a61` 写 `0x142e0`）就是这样丢的，函数随后被
  GlobalDCE 删掉。
- 这条同时是"5 个函数缺失"里 `ssl_readable`/`buffer_append` 的根因；另外三个
  （`FUN_9990`/`FUN_99d0`/`FUN_9a10`）走的是寄存器/ZMM partial write 重建 unknown
  那条线，见 06 号 log 末尾。
