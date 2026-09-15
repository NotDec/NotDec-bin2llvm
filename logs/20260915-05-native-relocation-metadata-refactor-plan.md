# 原始 prompt

```text
当前的native分析只需要注意两块。首先lifting阶段一定要和底层的汇编能够对应上即可。
其次是针对arch-specific的特性做处理。之前的NativeImage这种东西就比较重量级，
首先确保它是后续的底层特性的处理pass，而不是一开始就生成成了这种形式。规划一下
NativeImage的重构吧，不能做特别重量级的分析，这个不是这个阶段该做的事情。而仅仅
针对常见的情况做处理。思考是否可以不用NativeImage这种重量级的机制，比如直接通过
标记metadata的方式去匹配并转换部分的relocation 函数地址 entry的读取和调用，
转换为直接call即可
```

# 结论先说

可以不用 data image。**metadata 标记 + 一个小 matcher** 就能覆盖当前真正有价值的
那一小块（relocation 函数指针 slot 的读/调用 → direct call），而且更符合“lifting
忠实于汇编 + 只做 arch-specific 处理”这个阶段目标。

# 背景（已有 native 状态）

当前 `notdec-native-llvm` 主流程（cbfb 之后）：

```text
PcodeToLLVM (IntToPtr memory model)
  -> memory map / ABI metadata
  -> verify
  -> InstCombine
  -> NativeRegisterSummarySSA
  -> InstCombine
  -> NativeDataImage          <- 本轮要处理掉的重机制
  -> NativeFunctionPointerPromotion
  -> peephole -> final cleanup
```

`NativeDataImage`（commit 9c6de13、bb14b4d）做的事情：

- 给每个被引用到的非可执行 LOAD segment 建一个 packed struct global，初值是
  整个 segment 的字节（wrk 21KB、tmux 约 220KB、python 可到 MB 级）；
- 函数指针 slot 变成 `i64 ptrtoint(@func)` 字段；
- 把模块里所有能解析出 image 地址的 `inttoptr` 改写成 GEP（wrk static=107、
  base_offset=4；memcached base_offset=46；tmux base_offset=6474）；
- 附带两套防误判规则：确认基址白名单、动态 addend 不能是别的地址空间基址。

它换来的实际收益（实测）：

- wrk 间接调用提升 20 个（sock_connect/close/read/write/readable 等）；
- warning / RSP.entry / residue 与做 image 之前**完全一致**；
- memcached/lighttpd/tmux 的 promotion 依然是 0（动态 store 把 slot 判成 Unknown）。

也就是说：**image 的绝大部分工作量没有体现在当前阶段的指标上**，真正有用的只有
“函数指针 slot 的读取 → 直接调用”这一条，加上一个“地址被 escape 就不提升”的保护。
数据内容建模、base+offset、结构体指针传递都属于后面的阶段，现在做就是提前背成本。

# 目标

1. 删掉 data image 建模，IR 恢复成“绝对地址 + inttoptr”的忠实 lifting 形态。
2. relocation 函数指针 slot 用模块 metadata 标记：`地址 + 宽度 + 目标函数名`。
3. 一个小 matcher：`load slot -> inttoptr -> call` 且满足保守条件时，替换成
   direct call；保留 call 的原 FunctionType / calling convention / attributes /
   tail kind。
4. 现有指标不退化：wrk promoted=20、warning 0 diff、residue 不变；fortune
   x86_64/i386 不变；ctest 12/12。

# 不做什么

- 不建模数据段内容（字符串、全局变量初值、指针表字节）。
- 不做 `base + offset` / 动态表访问，不做 guarded multi-target promotion。
- 不做跨函数参数地址传播（IPA 值集）。
- 不改 lowering、不改 memory model、不新增 early pipeline 的全局改写 pass。
- 不为“以后可能要建模”预留半成品；需要时再单独起 pass。

# 设计

## 1. metadata schema

用 named metadata，紧跟现有 `notdec.memory_map` / `notdec.abi` 的风格：

```llvm
!notdec.relocation.function_pointer = !{!0, !1}
!0 = !{i64 82624, i64 8, !"sock_connect"}   ; 0x142c0
!1 = !{i64 82632, i64 8, !"sock_close"}     ; 0x142c8
```

- key 是 slot 虚拟地址，value 是 discovery 已经算出来的 computed value 映射到
  `plannedTargets.Direct` 的名字；
- 只登记“目标是非 runtime 的本地函数”的 slot（和现在 `functionPointerSlots`
  的过滤条件一样），runtime glue 不登记；
- 需要扩展时（数据指针、relocation kind）再加字段，不改已有语义。

## 2. 发射位置

`tools/notdec-native-llvm.cpp` 在 `buildConfirmedModule` 之后（现在构造
`functionPointerSlots` 的地方）调用一个新的小 helper：

```cpp
attachNativeFunctionPointerMetadata(*module, functionPointerSlots);
```

- 不解析、不改写任何指令，只挂 metadata；
- 新增 `include/notdec-bin2llvm/NativeRelocationMetadata.h` +
  `lib/NativeRelocationMetadata.cpp`：emit + parse 两个函数；
- IR 输入路径（`.ll`/`.bc`）没有 discovery state，不挂 metadata；matcher 读不到
  就什么都不做，天然兼容。

## 3. matcher

仍然放在 `InstCombine -> SummarySSA -> InstCombine` 之后（i386 的地址常量要等
寄存器消除后才出现），替换现在的 `NativeFunctionPointerPromotion` 实现：

```text
1. parse metadata -> map<address, {width, Function*}>
2. 每个 slot 做写/escape 分析：
   - IntToPtrInst 的操作数是该地址 -> 正常的地址使用，忽略
   - 指令里出现等于该地址的裸 ConstantInt（不是 inttoptr 操作数）-> escaped
   - store 的 pointer 是 inttoptr(该地址)：
       value 是 ptrtoint(@同一个函数) -> 可见写，候选不变
       其它值 -> unknown write
   - store 的 pointer 是 inttoptr(add/sub/or 常量链，常量基址在 map 里)
       -> unknown write（动态偏移可能命中该 slot）
   - 全局 initializer 里出现该地址 -> escaped
3. 扫间接 CallInst，沿 cast/inttoptr 回溯到
   load <- inttoptr(C)（C 在 map 里）：
   - 目标唯一、没有 unknown write、没有 escaped -> 替换成 direct call
4. 计数：seen / promoted / unknown_writes / escaped / multiple_targets
```

保留现有 `promoteCall()` 的语义（原 call 的 FunctionType、calling conv、
attributes、tail kind、metadata、debug loc 全部保留）。

## 4. 删除清单

- 删 `include/notdec-bin2llvm/NativeDataImage.h`、`lib/NativeDataImage.cpp`
  （约 450 行）；
- 删 `include/notdec-bin2llvm/NativeRelocationData.h`、
  `lib/NativeRelocationData.cpp`（slot global 机制，metadata 之后不需要了）；
- `NativeFunctionPointerPromotion.{h,cpp}` 改成 metadata matcher（净减不少行）；
- `lib/CMakeLists.txt`、`ARCHITECTURE.md` 同步；
- 测试：现有的 image/slot-global 单测改成 metadata + matcher 单测
  （metadata round-trip、单目标提升、未知写不提升、裸地址 escape 不提升）。

## 5. 阶段划分

建议一个 commit 完成（删除 + 替换是一次逻辑变更，中间态没必要保留两套机制）：

```text
native: replace data image with relocation metadata promotion
```

如果想更稳，可以拆两步：先加 metadata + matcher（image 先留着但不再被依赖），验证
指标一致；再删 image 和 slot global。这个取舍留给讨论。

# 判断标准

| 项 | 期望 |
| --- | --- |
| wrk 全量 promotion | seen=54 promoted=20（与当前一致） |
| wrk warning TSV | 与 `/tmp/notdec-reloc3/wrk.warn.tsv` 逐行 0 diff |
| wrk RSP.entry / residue | 0 / 只剩 FUN_a260 FS_OFFSET |
| wrk IR 体积 | 回到 ~1.76MB（现在是 1.80MB） |
| fortune x86_64 / i386 | define 13/40、warning 35/9、residue 1/16 |
| ctest | 12/12 |
| LLVM 22 | `llvm-as` + `opt -passes=verify` 通过 |
| 性能 | wrk 与当前 ~87s / 136MB 持平（image 去掉后应该略降） |

# 风险

- **promotion 覆盖面回退**：如果 matcher 只看 `inttoptr(C)` 精确地址，image 时代
  通过 “base+offset 动态表” 拿到的提升会丢。实测 wrk 两边都是 20，tmux/memcached
  两边都是 0，所以当前无损失；但这是个已知的能力边界，要记录。
- **函数指针 slot 的写不可见**：和现在一样，靠“裸地址 escape”“动态基址 store”
  两类保守规则挡住；metadata 只带地址信息，不比 image 差。
- **函数 body 保留**：image 的 symbolic 字段曾经是 body 的 use；删除后靠现有
  `preserveAddressTakenFunctions()` + `@llvm.used`（wrk 18 个，覆盖同一批
  relocation 目标），验证时确认 define 数不变。
- **metadata 命名/格式**：一旦有外部工具消费就要稳定；本轮只在本仓使用，先按上表
  固定，后续扩展只加字段。

# 后续阶段（明确不做，但留位置）

- 数据内容 / 结构体指针传递：等做到“底层特性处理”阶段再单独起 pass，可以复用
  这份 metadata（扩成所有 relocation slot）按需建模，而不是一开始全量建 image。
- `base + offset` / guarded promotion / IPA：同一阶段的后续步骤。
- `@llvm.used` 兜底移除：等数据引用建模完整后再谈。


# 实现记录（已完成）

## 改动

删除：

- `include/notdec-bin2llvm/NativeDataImage.h` / `lib/NativeDataImage.cpp`
- `include/notdec-bin2llvm/NativeRelocationData.h` / `lib/NativeRelocationData.cpp`
  （`@notdec.reloc.*` slot global 机制）

新增：

- `include/notdec-bin2llvm/NativeRelocationMetadata.h` / `lib/NativeRelocationMetadata.cpp`
  （metadata emit + parse）

改写：

- `lib/NativeFunctionPointerPromotion.cpp`：metadata matcher；
  - `analyzePointerChain()` 把 `inttoptr` 指针归约到常量锚点（支持常量 GEP offset、
    常量/动态 addend），供 store 分析和 callee 回溯共用；
  - `scanEscapes()`：裸 `ConstantInt == slot 地址`（排除 inttoptr 操作数）和全局
    initializer 里的 slot 地址都记 escape；
  - `scanStore()`：精确 store 看 value 是不是同一个函数，动态 addend 的 store 直接
    记 unknown；
  - 先收集所有 indirect call 再改写（`promoteCall` 会 erase，原地迭代会失效）。
- `tools/notdec-native-llvm.cpp`：pipeline 恢复成
  `... -> InstCombine -> SSA -> InstCombine -> Promotion -> ...`，只多一步
  `attachNativeFunctionPointerMetadata()`；promotion summary 加 `escaped=`。
- `tests/pcode_to_llvm_test.cpp`：删 5 个 image 测试 + 1 个 slot global 测试，
  换成 5 个 metadata/matcher 测试（round-trip、单目标提升、未知写不提升、
  裸地址 escape 不提升、动态 store 记 unknown）。
- `ARCHITECTURE.md` 5.1/5.2 改为 NativeRelocationMetadata / matcher。

## 验证结果

wrk 全量（`--all-confirmed --register-ssa-summary`）：

```text
Native function pointer promotion: seen=54 promoted=20 slots=18 unknown_writes=0 escaped=0 multiple_targets=0
```

| 项 | 结果 |
| --- | --- |
| wrk promoted | 20（与 image 版本一致） |
| warning TSV | 353 行 0 diff |
| RSP.entry / residue | 0 / 2 行（只剩 FUN_a260 FS_OFFSET） |
| define / `@llvm.used` | 78 / 18（与 image 版本一致，body 没丢） |
| `inttoptr` 数 | 1141（恢复成 lifting 形态，image 版本是 107 处 GEP + 其余） |
| `notdec.image.*` / `notdec.reloc.0x*` | 0 处 |
| IR 体积 | 1,763,215 B（image 版本 1,802,704 B；加 image 前 1,762,030 B） |
| 性能 | 81.45s / RSS 138MB（image 版本 87~88s；加 image 前 87s） |
| LLVM 22 verify | 通过 |

fortune 回归：x86_64 define 13 / warning 35 行 / residue 1 行；i386 define 40 /
warning 9 行 / residue 16 行，与重构前一致。ctest 12/12。

## 说明

- image 的两个 commit（9c6de13、bb14b4d）保留在历史里，本次改动把它们撤回；
  连带 `inttoptr` 白名单、addend provenance、packed struct image 这些重量级
  机制一起删掉。
- 数据段内容、`base + offset`、IPA 参数传播全部不做，等后续“底层特性处理”
  阶段再按需起 pass。

