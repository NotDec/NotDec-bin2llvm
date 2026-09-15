# 原始 prompt

```text
当前的任务可以先继续完成吧。然后看看为什么bin2llvm还这么慢呢，没跑类型推理不应该啊。
```

# 背景（perf 定位）

wrk（9791 指令 / 247 函数 / 2584 基本块）单次 native lifting：

```bash
perf record -F 199 -g -o /tmp/perf-wrk.data -- \
  build/bin/notdec-native-llvm wrk --all-confirmed --register-ssa-summary -o /tmp/wrk.native.ll
perf report --stdio -i /tmp/perf-wrk.data --percent-limit 0.5
```

优化前 inclusive：

| 阶段 | 占比 |
| --- | --- |
| `buildConfirmedModule` | 48.3% |
| └ `collectSleighPcodeRanges` | 35.9% |
| &nbsp;&nbsp;└ `initializeSleighEngine`（解析 .sla） | **32.7%** |
| └ `appendPcodeFunction` | 6.1% |
| `runNativeRegisterSummarySSA` | 39.4% |
| └ `runNativeRegisterSummary` | 7.0% |
| 其余（discovery / promotion / peephole / final cleanup） | ~12% |

flat 里 `ghidra::PackedDecode::*`（解析 .sla）≈14%，
`std::_Rb_tree<Value*, pair<Value*,Value*>>` 系列 ≈13%，
`PcodeLowerer::sortedNativeRanges()` 的排序 ≈3.5%。

# 修的两处

## 1) .sla 只解析一次（commit 6531b3d）

`buildConfirmedModule()` 之前对**每个函数**调用一次 `collectSleighPcodeRanges()`，
每次都新建 SLEIGH engine 并重新解析 x86-64.sla（247 函数 = 247 次）。discovery 侧早就用
`SleighInstructionDecoder` 复用 engine，confirmed lifting 这条路漏了。

- `SleighInstructionDecoder::collectPcode(ranges, preserveRangeOrder, errorStream)`：
  用已初始化的 engine 收集 p-code；
- `buildConfirmedModule()` 循环外建一个 decoder，循环内复用。

## 2) buildBasicBlocks 里只排序一次（本 commit）

`buildBasicBlocks()` 的"每个 op 是否被 block range 覆盖"检查里，对每个 op 都调用
`sortedNativeRanges()`——该函数每次重新构造并 `std::sort` 一个 vector，main 这类函数是
O(ops × blocks log blocks)。改成进循环前算一次。

# 结果

| 项 | 优化前 | .sla 复用后 | +排序提升后 |
| --- | --- | --- | --- |
| wrk 运行时间 | 82.3s | 48.9s | **41.3s**（-50%） |
| 内存 | 136MB | 137MB | 139MB |
| raw IR（`--no-register-ssa-pass --no-instcombine-pass`） | — | 归一化后完全相同 | 相同 |
| warning TSV | 348 行 | 相同 | 完全相同 |
| ctest | 12/12 | 12/12 | 12/12（fortune x86_64/i386 在内） |

共享 engine 跨函数复用安全：raw IR 归一化后与"每函数新建 engine"版本 0 行差异。

# 还没修的（下一步性能项）

优化后 SSA pass 仍是主要开销，flat/inclusive 显示：

1. `FunctionBuilder::rewritePartialWrites` 24.3%、`removeDeadPartialReads` 11.4%、
   `removeDeadReplacedLoads` 6.3%，其中绝大部分是
   `isRecordedCallArgValue()` -> `resolve()`：
   ```cpp
   for (const auto &[call, bindings] : SignatureState.CallArgs)   // 全模块
     for (const CallArgStoreBinding &binding : bindings)
       if (resolve(binding.Value) == value || resolve(binding.RegisterValue) == value)
   ```
   即每个 store/load 都线性扫一遍**全模块**的 call-arg 绑定（wrk 约 1256 条），
   整体是 O(指令数 × 绑定数)。修法：按阶段预计算"已解析绑定值集合"（注意
   `Replacement` 在阶段内只做删除、不做新增，可以用保守集合），或把绑定值索引起来。
2. `forgetReplacementValue()` 每次删指令都线性扫整个 `Replacement` map（≈6%），
   可加反向索引。
3. `std::map<llvm::Value*, ...>` 系列本身（`resolve`/`Demands`/`ValueMap`）可换
   `DenseMap`。

# 附带发现：输出不确定（同一二进制两次运行不同）

```bash
build/bin/notdec-native-llvm wrk --all-confirmed --register-ssa-summary -o runA.ll
build/bin/notdec-native-llvm wrk --all-confirmed --register-ssa-summary -o runB.ll
diff runA.ll runB.ll | grep -c '^[<>]'   # 936
```

把数字归一化后按**多重集合**比较：0 差异。也就是说两次运行的内容完全一样，只是
指令/metadata 的**顺序**不同——符合"以 `llvm::Value*` 为键的 `std::map` 按指针地址
遍历"的特征（地址随分配变化）。影响：

- 不能用 byte-level diff 比较两次/两个 build 的 IR，必须先按行排序或归一化；
- 想彻底稳定需要把这些 map 换成有序键（如按 `Value` 的创建序号）或 DenseMap + 确定性遍历；
  这与上面的性能项 3 是同一处。

# FUN_9990 / FUN_99d0（当前任务的收尾结论）

本轮把这条追到底了，结论比 09 号 log 的附录更精确：

1. `main` 里回调地址（`@FUN_9990`/`@FUN_99d0`）确实被写进 frame：
   `store i128 %ZMM2.partial_range.part_insert2322, ptr %notdec_stack.native.ptr2237`，
   且这条 store 的值链能追到 `zext ptrtoint(@FUN_9990)`（用脚本按 def-use 验证过）。
2. 但在 SSA pass 结束时，`main` 里已经**没有任何 `ptrtoint(gep(%notdec_stack.native))`**
   逃逸值（norr IR 与 pre-cleanup IR 都是 0）——即 frame 在 IR 里不再逃逸。
3. 更上游的原因：`main` 传给 `script_verify_request` 的 settings 指针来自
   `%notdec.reg.insert.lower1476`，而它的链是
   `zext/and/or (notdec.register.summary_clobber.i32())`——RCX 的值在寄存器模型里被
   重建成了 unknown clobber。也就是说：**frame 写了，但唯一会读它的指针丢了**。
4. 于是 InstCombine 的"只被 store、从不被 load 的 alloca"消除 + GlobalDCE 把 frame
   store、alloca 和这两个函数一起删掉是"合法"的（给定已经没有 escape 的 IR）。
5. 验证：把 pre-cleanup IR dump 出来，单独跑
   `opt -passes='instcombine<no-verify-fixpoint>'` 就能复现删除；删掉的是一整段
   含 alloca/GEP/prologue store/settings store/`call @zmalloc` 的区域。

下一步（下一轮）：在 `NativeStackFrame` 的 rewrite 阶段把"RSP 派生出来、又被当参数/
存值/返回值传出去"的地址**规范化成 `ptrtoint(gep(alloca))`**（让逃逸在 IR 里显式且
活得比寄存器模型久），而不是回到寄存器模型里被 clobber 掉。或者先修 RCX clobber
重建 unknown 那条线（06 号 log 末尾列的同一个问题）。
