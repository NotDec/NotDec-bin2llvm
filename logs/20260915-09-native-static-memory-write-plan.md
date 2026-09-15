# 原始 prompt

```text
对，按照思路都修一下试试
```

（上下文：动态移位量的 partial demand 修复提交后，继续追"5 个函数从模块里消失"。
本轮定位到比寄存器模型更基础的一条：**静态地址的内存写根本没有进 IR**。）

# 背景（怎么发现的）

追 `FUN_a800` 时顺手对 `main` 做了"汇编 store 数 vs IR store 数"的对比：

```bash
objdump -d --no-show-raw-insn wrk | # main 范围内 mov/movaps ...,0x..(%rip)
```

结果：`main` 里有 **17 条 RIP 相对 store**，而 `--no-register-ssa-pass
--no-instcombine-pass` 的 raw IR 里对应地址的 store 数是 **0**（整个模块都是 0）。

以 `main` 的 `0x5a4b` 块为例，汇编是：

```asm
5a4b: movq   0x13fe0(%rip),%xmm0     ; 读 ssl_connect 槽
5a53: lea    0xf16(%rip),%rax        ; &ssl_readable
5a5a: movhps 0x13fa8(%rip),%xmm0     ; 读 ssl_close 槽
5a61: mov    %rax,0xe878(%rip)       ; 写 0x142e0 = &ssl_readable
5a68: movaps %xmm0,0xe851(%rip)      ; 写 0x142c0..0x142d0
5a6f: movq   0x13f98(%rip),%xmm0
5a77: movhps 0xe542(%rip),%xmm0
5a7e: movaps %xmm0,0xe84b(%rip)      ; 写 0x142d0..0x142e0
```

p-code（`notdec-native-pcode -a 0x5a4b -l 0x3a`）里三条写都在：

```text
0x5a61: (ram,0x142e0,8) = COPY (register,0x0,8)
0x5a68: (ram,0x142c0,16) = COPY (register,0x1200,16)
0x5a7e: (ram,0x142d0,16) = COPY (register,0x1200,16)
```

但 GTIRB 事实里指令是在的（`--instructions-range-json` 能看到 0x5a61/0x5a68/0x5a7e），
lifter 却把它们丢了。

# 根因

Ghidra 的 sleigh 对**静态地址**的内存访问（`*[ram]:8 0x142e0 = ...`）不生成
`CPUI_STORE`/`CPUI_LOAD`，而是把该地址建模成 **ram 空间的 varnode**：写是
"ram varnode = 值"的 COPY/算术赋值，读是"值 = ram varnode"。

`lib/PcodeToLLVM.cpp` 里读侧已经处理了（`read()` 对 `Space == "ram"` 生成真实
`load`），但写侧 `write()` 只做 `Values[varnodeKey(varnode)] = resized`——写进
SSA 缓存，**不生成 store**。于是：

- 整个模块所有静态地址写入从 IR 里消失（wrk：raw IR 里 0 条，实际 50 条）；
- 只有 SSA 缓存能在同一个块里"看见"这次写，跨块/跨函数/走动态指针都看不见；
- `ssl_readable` 的唯一引用就是 `0x5a61` 这次写，写没了 → 函数没人引用 →
  最终 GlobalDCE 把 `ssl_readable` 和只被它调用的 `buffer_append` 删掉。

# 目标

1. 静态地址的写落成真实 `store`，不再停留在 SSA 缓存里。
2. wrk：raw IR 静态 store 数 0 -> 实际数量；缺失函数减少；warning 不增；
   residue / verify / ctest 不劣化。
3. 记录对 function pointer promotion 的影响（见下）。

# 不做什么

- 不做 data image / 数据内容建模，不改 memory model（仍是 IntToPtr）。
- 不动 promotion pass 的判定规则（`state->Targets.size() != 1` 就跳过），
  "guarded multi-target promotion" 仍留作后续。
- 不给 ram varnode 写做缓存回填。

# 设计（最小修改）

`PcodeToLLVM.cpp` 的 `write()`，在寄存器分支之后加一个 ram 分支：

- `varnode.Space == "ram"`：用 `llvm::ConstantInt::get(intType(pointerByteSize()),
  varnode.Offset)` 造地址，`Builder.CreateStore(resized, memoryPointer(address))`，
  `align 1`，然后 return（**不**写回 `Values`）；
- 其余（unique 临时变量等）保持原缓存行为。

不回填缓存是刻意的：缓存会在"中间夹了动态 store / 调用"时读到过期值，而读侧本来
就会生成真实 `load`，交给 LLVM 自己转发即可。

# 实现记录

- `lib/PcodeToLLVM.cpp` `PcodeLowerer::write()`：新增 `varnode.Space == "ram"`
  分支，生成真实 store 并直接返回；注释说明 sleigh 的静态内存建模方式。
- `tests/pcode_to_llvm_test.cpp`：新增 `testStaticMemoryWriteIsLiftedAsStore()`
  （在 `main()` 注册），手工构造一个 `ram:0x143a0/8 = COPY const:0x1234/8` 的
  p-code op，用 `IntToPtr` 内存模型 lowering，断言出现指向 `inttoptr (i64 82848
  to ptr)` 的 store。

反向验证：

```bash
git stash push -- lib/PcodeToLLVM.cpp
cmake --build build -j 8 && ./build/bin/pcode_to_llvm_test   # exit=1
# "static address write was not lifted as a store"
git stash pop
```

# 验证

## 静态 store 数（raw lowering）

```bash
build/bin/notdec-native-llvm wrk --all-confirmed --register-ssa-summary \
  --no-register-ssa-pass --no-instcombine-pass -o /tmp/wrk-raw.ll
```

| | 修复前 | 修复后 |
| --- | --- | --- |
| raw IR 里指向常量地址的 store | 0 条 / 0 个地址 | 50 条 / 19 个地址（全部落在 .data 段） |
| 最终 IR | 0 条 | 48 条 |

实际恢复的写包括 `used_memory += size`（`zmalloc`）、`zmalloc_set_oom_handler`
对 0x14100 的写、`main` 对 ssl 函数指针表（0x142c0..0x142e0）的初始化。

## wrk（最终 IR）

命令同 08 号 log，输出 `/tmp/nb-static3/wrk.native.ll`。

| 项 | 修复前 | 修复后 |
| --- | --- | --- |
| define 数 | 82 | 85 |
| 回来的函数 | — | `ssl_readable`、`buffer_append`、`FUN_9a10`(response_body) |
| `ptrtoint (ptr @...)` 使用数 | 19 | 21 |
| `notdec.unknown.iN` 调用数 | 69 | 69 |
| warning TSV | 354 行 | 348 行（删 6 行，0 新增） |
| residue audit | 1 行 | 1 行 |
| IR 体积 | 1,835,329 B | 1,862,320 B（+1.5%） |
| 性能 | 81.3s / 138MB | 82.3s / 136MB |
| LLVM 22 verify | 通过 | 通过 |
| ctest | 12/12 | 12/12（含 fortune x86_64/i386） |

删掉的 6 条 warning 是 `aeCreateEventLoop`/`aeCreateTimeEvent` 的
`remaining_summary_clobber_value` 与 `return_binding_preserved_mixed_clobber`，
来自之前被错误提升的间接调用。

## promotion 计数变化（取舍）

```text
修复前: seen=54 promoted=20 slots=18 unknown_writes=0 escaped=0 multiple_targets=0
修复后: seen=50 promoted=2  slots=14 unknown_writes=3 escaped=0 multiple_targets=1
```

原因不是回归，而是这些 slot 本来就会被运行时改写，现在能看见了：

- `zmalloc_set_oom_handler` 里新 lift 出的 `store i64 %RDI.arg, ptr inttoptr
  (i64 0x14100)` 让 OOM handler 槽变成 unknown write：15 个间接调用不再被提升为
  `@FUN_7b30`（修复前它们被当成"永远是默认 handler"，语义是错的）；
- `main` 对 0x142c0/0x142d0/0x142e0 的写让 ssl 函数指针表槽变成 unknown /
  双 target：不再被提升为静态重定位值 `sock_*`（运行时实际会换成 `ssl_*`）。

也就是说：**promotion 数下降是拿到了正确性**。要在这个前提下恢复提升，
需要做之前 deferred 的 "guarded multi-target promotion"（按 store 的常量函数地址
更新 target 集合、只有集合唯一时才提升）。

# 效果与成本

- 效果：静态地址写不再丢失；函数指针表 / 全局计数器 / OOM handler 这类语义回到 IR；
  3 个函数恢复；6 条错误 warning 消失；误提升的间接调用回到诚实的 indirect call。
- 理解成本：`write()` 多了一条 ram 分支和一段注释（sleigh 静态内存建模）；
  promotion 计数需要按"正确性"而不是"提升数量"来读。
- 维护成本：低。读侧本来就是这么做的，写侧只是补齐；代价是 IR 体积 +1.5%。

# 后续

1. `FUN_9990`(header_field)、`FUN_99d0`(header_value) 仍缺失，走的是寄存器 /
   ZMM partial write 重建 unknown 那条线（见 06 号 log 末尾），下一项处理。
2. guarded multi-target promotion：用新可见的 slot store 常量值恢复（正确的）
   直接调用提升。
3. `http_parser_execute` 里还有 20+ 个 `inttoptr` 间接调用无法回溯到 slot
   （`%RCX.full_range.part_insert...`），属于寄存器值域问题，不在本轮范围。

# 附：FUN_9990 / FUN_99d0 的定位（本轮未改完，代码已回退）

这两个函数（http_parser 的 header_field / header_value 回调）仍然缺失。本轮定位：

- `--no-summary-register-residue-removal` 的 IR 里，`main` 把回调地址组成
  128 位值写进自己的 frame：

  ```llvm
  store i64 ptrtoint (ptr @FUN_99d0 to i64), ptr @RAX
  %10 = zext i64 ptrtoint (ptr @FUN_99d0 to i64) to i128
  call void @notdec.partial_write.i512.i128(ptr @ZMM3, i128 %10, i64 0)
  ... 同样方式处理 @FUN_9990 ...
  store i128 %notdec.reg.insert.lower907, ptr %notdec_stack.native.ptr2237
  ```

  也就是说地址**已经到达** frame store（06 号 log 里 "地址没到达 settings store"
  的判断在这个版本已经不成立）。

- 用临时插桩跟踪 `FUN_9990` 的 use 数：SSA pass 全程（rewrite、post-rewrite
  InstCombine、dead-store 清理、stack frame cleanup）都保持 `uses=1`，
  说明**不是** SSA/stack cleanup 删的。

- 引用消失发生在 SSA pass 之后的 `runInstCombinePassIfEnabled()` 或其后的 pass。
  最可能的是 InstCombine 自己的 "store 到从不被读取的 alloca" 死存储消除：
  它用 LLVM 自己的逃逸判断，看不到我们 pass 内部的 "escaped frame" 结论。

- 试过一版补丁：在 `NativeStackFrame.cpp` 的 `collectEscapedStackAllocas()` 里
  增加"整数形式的栈地址传出去（实参/存值/返回值）也算逃逸"（`valueDerivesFromStackPointer`）。
  结果：wrk 运行时间 82s -> 106s（+29%），define 数仍是 85，目标函数没有回来，
  所以整段回退，没有提交。

结论/下一步：要么让 "frame escaped" 这个事实以 InstCombine 能看懂的形式表达
（例如把 `ptrtoint(gep(alloca))` 换成 `@llvm.used` 风格的保持，或在 final cleanup
之前禁止对 `notdec_stack.native` 做 alloca 死存储消除），要么在 final cleanup 阶段
再做一次 "escaped frame store 保护"。需要单独一轮设计和测量。
