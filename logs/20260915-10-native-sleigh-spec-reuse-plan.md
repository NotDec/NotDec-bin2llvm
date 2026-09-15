# 原始 prompt

```text
当前的任务可以先继续完成吧。然后看看为什么bin2llvm还这么慢呢，没跑类型推理不应该啊。
```

# 背景

wrk（9791 条指令 / 247 函数 / 2584 基本块）单次 native lifting 一直是 80s 量级。
本轮先用 perf 定位：

```bash
perf record -F 199 -g -o /tmp/perf-wrk.data -- \
  build/bin/notdec-native-llvm wrk --all-confirmed --register-ssa-summary -o /tmp/wrk.native.ll
perf report --stdio -i /tmp/perf-wrk.data --percent-limit 0.5
```

inclusive 结果（优化前）：

| 阶段 | 占比 |
| --- | --- |
| `buildConfirmedModule` | 48.3% |
| └ `collectSleighPcodeRanges` | 35.9% |
| &nbsp;&nbsp;└ `initializeSleighEngine`（解析 .sla） | **32.7%** |
| └ `appendPcodeFunction` | 6.1% |
| `runNativeRegisterSummarySSA` | 39.4% |
| └ `runNativeRegisterSummary` | 7.0% |
| 其余（discovery / promotion / peephole / final cleanup） | ~12% |

flat 视图里 `ghidra::PackedDecode::*` 合计约 14%，`std::_Rb_tree<Value*, pair<Value*,Value*>>`
（SSA pass 的值映射）合计 13%+。

根因：`buildConfirmedModule()` 对**每个函数**调用一次 `collectSleighPcodeRanges()`，
而它每次都新建 SLEIGH engine 并重新解析一遍 x86-64.sla（XML/Packed）。247 个函数 =
247 次 .sla 解析。discovery 侧早就用 `SleighInstructionDecoder` 复用同一个 engine
（注释里写明了 "reparsing the .sla for every seed is much more expensive"），
confirmed lifting 这条路漏了同样的处理。

# 目标

1. confirmed module 构建阶段只解析一次 .sla。
2. 输出 IR 与优化前语义一致（raw IR 归一化后必须完全相同）。
3. ctest / fortune 不变。

# 实现

- `include/notdec-bin2llvm/SleighLift.h`：给 `SleighInstructionDecoder` 增加
  `collectPcode(ranges, preserveRangeOrder, errorStream)`，复用已初始化的 engine。
- `lib/SleighLift.cpp`：实现该方法（逻辑与 `collectSleighPcodeRanges` 相同，只是
  不再构造 engine / 解析 .sla；寄存器表直接复用 `Impl::Registers`）。
- `tools/notdec-native-llvm.cpp` `buildConfirmedModule()`：循环外构造一个
  `SleighInstructionDecoder`，循环内调用 `collectPcode(...)`。

# 验证

| 项 | 优化前 | 优化后 |
| --- | --- | --- |
| wrk 运行时间 | 82.3s | **48.9s**（-40%） |
| 内存 | 136MB | 137MB |
| raw IR（`--no-register-ssa-pass --no-instcombine-pass`） | — | 归一化后与优化前**完全相同**（0 行差异） |
| 最终 IR | — | 仅 metadata 节点顺序不同（96 行 `!#` 行，指令 0 差异）；体积 1,862,320 -> 1,862,315 B |
| warning TSV | 348 行 | 完全相同 |
| promotion 计数 | seen=50 promoted=2 slots=14 unknown_writes=3 escaped=0 multiple_targets=1 | 同 |
| ctest | 12/12 | 12/12 |

共享 engine 跨函数复用是安全的：raw IR（未跑 SSA/InstCombine 的逐指令 lowering）在
归一化后与"每函数新建 engine"版本完全一致，说明没有 context 泄漏导致的解码差异。

# 后续（下一项）

剩下的热点在 `runNativeRegisterSummarySSA`（优化后占比更高）。flat profile 显示
`std::map<llvm::Value*, llvm::Value*>` 系列查找（`_Rb_tree`/`less<Value*>`/
`_Rb_tree_increment`）占 13%+，候选是 SSA pass 的 `Replacement`/`ValueMap` 等
以 `Value*` 为键的红黑树；换成 `llvm::DenseMap`/`SmallPtrSet` 是下一步。
