# 原始 prompt

```text
这个是之前的对话：在修复bin2llvm链路中的关于重定位表项，全局变量里存某些函数地址的情况。
（前一轮总结见 logs/20260915-03-native-function-pointer-slot-model-and-promotion-plan.md）

按照这个规划继续推进吧
```

上一轮留下的下一步：

1. 把 data image / `.data` relocation 槽位提升成更大范围的 LLVM global，覆盖
   `base + offset` 和 `parser_settings` 跨函数指针传递。
2. 在多目标场景加入 guarded promotion（icmp/switch + fallback indirect call）。
3. 等数据引用完整后移除 `@llvm.used` 兜底。
4. 清理 `FUN_a260` 的 `FS_OFFSET` residue。

本轮先做第 1 项里最小、可验证的一块：**data image 建模 + `inttoptr`
常量/常量加动态偏移访问改写**。guarded promotion 留下一个日志单独做。

# 背景

当前 native 链路的 memory model 是 `IntToPtr`：所有 RAM 访问都是
`load/store ..., ptr inttoptr (i64 <addr> to ptr)`。上一轮只给
“relocation 目标已知是本地函数”的 pointer-size slot 建了独立的
`@notdec.reloc.0xADDR` global，并且只改写“访问地址正好是槽位地址”的
load/store。

wrk 最终 IR 的实测：

- 常量 `inttoptr (i64 C)` 共 78 处，覆盖 27 个不同地址：
  `.bss` 36、`.rodata` 27、`.got` 14、`.data` 1；
- 动态到达 data image 的 `base + offset` 访问（例如
  `inttoptr(add i64 62240, %idx)`）只有个位数；
- 其余 1000+ 动态 `inttoptr` 是栈/寄存器地址，不属于 image。

也就是说：**常量地址访问已经能覆盖绝大多数 data 访问**，但它现在读写的是一块
模块里不存在的绝对地址内存。data image 建模就是把这块内存变成真实的 LLVM
object，顺便把 relocation slot 的初值（函数指针、字符串指针）固定下来。

# 目标

1. 把非可执行 LOAD segment 建模成 LLVM global（`@notdec.image.0x<start>`），
   initializer 是 segment 的实际字节（`.bss` 尾部补零）。
2. segment 内已知是本地函数入口的 pointer-size relocation slot，在
   initializer 里建成带符号的 `ptrtoint(@function)` 字段，保持上一轮的
   “函数指针有 use、body 不被 GlobalDCE 删掉”能力。
3. 把 `inttoptr(C)`（C 在 image 内）改写为指向 image 的常量 GEP；
   把 `inttoptr(add/sub C, %dyn)` 改写为 image GEP + 动态 index，覆盖
   `base + offset` 表访问。
4. 不在 image 覆盖范围内的函数指针 slot 继续走上一轮的
   `@notdec.reloc.0xADDR` 独立 global，保证旧能力不退化。
5. promotion pass 能识别 “load 自 image 的 pointer-size 字段”，继续把
   `load slot -> inttoptr -> call` 提升成 direct call。

# 不做什么

- **不做跨函数参数传递**：`call @f(i64 0x14370)` 之后 callee 内部
  `inttoptr(i64 %param)` 仍然是裸 inttoptr。这需要 IPA 值集传播，单独一轮。
- **不 fold 从 image 读出来的 data pointer**：把 `load @image+off` 常量折叠成
  `ptrtoint(gep @image, v)` 会让“和裸整数常量比较”的代码语义分叉，先不动。
- **不做全局常量改写**：image 范围内的整数常量并不都是地址。wrk 的
  `and i64 %x, 65535`（0xffff 落在 .rodata 的 0xf000..0x10c82 内）证明
  无条件改写会改坏语义；本轮只在 `inttoptr` 的地址链上改写。
- 不移除 `@llvm.used`；不碰 `FUN_a260` 的 `FS_OFFSET`。
- 不改 memory model 本身，栈/heap 访问仍然是裸 `inttoptr`。

# 设计取舍

## 为什么 byte image + struct 字段，而不是只建 slot global

- 只建 slot global 无法表达 `base + offset`：表里的相邻字段、结构体成员还是
  裸地址。
- 一个 segment 一个 global 保证 segment 内任意偏移访问都落在同一个 LLVM
  object 里，alias 关系正确；`inttoptr(C)` 和 `inttoptr(C+8)` 变成同一个
  global 的两个 GEP。
- 需要 `i64 ptrtoint(@func)` 这种符号初值，所以初始值类型用 **packed struct**
  `<{ [k x i8], i64, [k x i8], ... }>`：packed 保证字段偏移和文件字节完全一致
  （普通 struct 会插 padding，把后续字节整体挪位）。
- 访问统一用 `getelementptr i8, ptr @image, i64 off`（需要动态 index 时用两个
  index），不需要 GEP 到具体字段；字段只负责“初值带符号”。

## 常量基址白名单（实现中发现，必须做）

第一版 `resolve()` 只要 `add/sub` 链里出现落在 segment 内的常量就当基址，结果 wrk 报出
`base_offset=988`。原因是 wrk 第一个 LOAD segment 覆盖 `0x0..0x4510`，栈访问里的
`add %stack, 8`、`add %stack, 16` 全被当成 “image 0x0 + 8”，把栈访问改写进了 image。

修法：只有**前端已确认是数据地址**的常量才能当动态访问的基址：

- relocation slot 地址（`relocatedPointers()` 的 key）；
- relocation computed value（key 对应的 value，指向字符串/数据对象）；
- PLT GOT 地址；
- 模块里直接 `inttoptr(C)` 的 C（这种用法本身就是地址语义）。

完全常量折叠出来的 `inttoptr(C)` 不受白名单限制（`inttoptr` 本身就是地址用法）。
修完后 wrk `base_offset=0`：它仅有的动态表访问是 `.rodata` 里的 switch jump table
（`add 62240, %idx`），62240 既不是 relocation 槽位也不是 relocation 值，保守跳过。
这是有意的：漏改写只是保持现状，错改写会改坏语义。

同类风险还有 `and i64 %x, 65535`：`0xffff` 落在 wrk `.rodata` 的
`0xf000..0x10c82` 内。这也是本轮不做全局常量改写的原因。

## 动态 addend 不能是别的地址空间（memcached 抽查发现）

memcached 抽查时 `base_offset` 一度是 1627，检查 IR 发现里面有：

```llvm
; x86-64 stack guard：mov rax, fs:0x28
%FS_OFFSET = load i64, ptr @FS_OFFSET
%notdec.image.ptr = getelementptr i8, ptr getelementptr (i8, ptr @notdec.image.0x0, i64 40), i64 %FS_OFFSET
```

以及 `add 8, %RSP` → `@notdec.image.0x0 + 8 + %RSP`。原因有两层：

1. 这些是 `%fs:0x28` / `[rsp+8]` 访问，lowering 里有一部分被表示成裸
   `inttoptr (i64 40)` / `inttoptr (i64 8)`（TLS 偏移），这些常量按
   “直接 inttoptr 就是地址”规则进了确认基址集合；
2. 常量 8/16/40 又恰好落在 `@notdec.image.0x0`（ELF header segment）里，于是
   `add 40, %FS_OFFSET` 被当成 “image + 40 + FS 基址”。

修法：动态访问除了要求常量基址已确认，还要求 **addend 不是别的地址空间基址**。
`dynamicAddendIsAddressBase()` 沿 `add/sub/or/xor/and/zext/sext/trunc/ptrtoint/
phi/select` 走一遍，遇到以下情况就拒绝：

- load 自带 `!notdec.register` metadata 的 global（`@FS_OFFSET`、`@GS_OFFSET`、
  `@RSP` 等寄存器模型 global）；
- 参数名以 `RSP/ESP/RBP/EBP` 开头（SSA 后的 `%RSP.entry`）；
- `alloca` 或 `getelementptr`（addend 本身就是地址，不是 index）。

注意只查 addend，不查常量基址：`&settings + index`（index 是寄存器/栈上读出的
整数）仍然会改写。

## 已知语义边界（明确记录，不在本轮修）

改写后，**常量地址访问**落在 image object 上，**动态地址访问**（参数、
从 image 读出来的指针）仍然落在裸地址空间。读写同一地址时这两条路不共享
object。这是上一轮 `@notdec.reloc.*` 就已经存在的建模方式，本轮把范围从
“单个函数指针 slot”扩大到“整个 data segment”。要彻底消除分叉需要指针
provenance / IPA 值集传播，作为下一轮。

promotion 的 soundness 保护：除了 image 写分析，还检查 slot 地址是否在模块里
以裸 `ConstantInt` 形式出现过（例如 `call @scan_metric(..., i64 82800)`）；
出现过就不提升，避免“callee 通过参数写 slot、caller 直接读 slot”这种
看不见的写。

# 实现

## 1. 新 pass NativeDataImage

文件：

- `include/notdec-bin2llvm/NativeDataImage.h`
- `lib/NativeDataImage.cpp`

API：

```cpp
struct NativeDataImageSegment {
  uint64_t Address = 0;
  uint64_t Size = 0;              // 虚拟大小，可以大于 Bytes.size()（.bss）
  bool Executable = false;
  llvm::ArrayRef<uint8_t> Bytes;  // 文件里的内容
};

struct NativeDataImageFunctionSlot {
  uint64_t Address = 0;
  uint64_t Width = 0;
  std::string FunctionName;
};

struct NativeDataImageOptions {
  uint64_t MaxSegmentSize = 0;    // 0 = 不限制
};

struct NativeDataImageSummary {
  uint64_t SegmentsCreated = 0;
  uint64_t SegmentsSkipped = 0;
  uint64_t FunctionSlots = 0;
  uint64_t StaticAccesses = 0;      // inttoptr(const)
  uint64_t BaseOffsetAccesses = 0;  // inttoptr(const + dyn)
  uint64_t UnresolvedIntToPtrs = 0;
  uint64_t FallbackSlots = 0;       // 走 @notdec.reloc.* 的 slot
};

NativeDataImageSummary materializeNativeDataImage(
    llvm::Module &module,
    llvm::ArrayRef<NativeDataImageSegment> segments,
    llvm::ArrayRef<NativeDataImageFunctionSlot> slots,
    llvm::ArrayRef<uint64_t> knownAddressBases,
    const NativeDataImageOptions &options = {});
```

流程：

1. 先收集 module 里所有 `inttoptr` 的“常量基址”（含 `add/sub` 链上的常量），
   只有被引用到的非可执行 segment 才建 global，避免给 python 这种
   `.rodata` 3MB 的二进制建无意义 object。
2. 建 packed struct global：字节块用 `ConstantDataArray`，函数 slot 字段用
   `ConstantExpr::getPtrToInt(@function, iW)`。
3. image 覆盖不到的 slot 调上一轮的
   `materializeNativeFunctionPointerSlots()` 兜底。
4. 改写地址：
   - `inttoptr(C)`：C 在 image 内 -> 常量 GEP；
   - `inttoptr(add/sub(C, %dyn))`：-> `gep i8, ptr @image, i64 C-off, i64 %dyn`；
   - 常量 `inttoptr` ConstantExpr 也要改写（现在 IR 里 load/store 的 pointer
     operand 就是这种常量表达式）。
5. 统计 StaticAccesses / BaseOffsetAccesses / UnresolvedIntToPtrs。

## 2. promotion 识别 image 字段

`lib/NativeFunctionPointerPromotion.cpp`：

- 把 `slotLoadedByCall()` 从 “返回 GlobalVariable*” 改成
  “返回 (global, 常量 offset)”，用 GEP 链解析。
- global 名是 `notdec.image.*` 时，按 packed struct 的类型/初值找 offset 落在哪个
  字段：字段是 pointer-size integer 且初值是 `ptrtoint(@func)` 才是候选。
- 写分析按 global 缓存：常量 offset 的 store 记录 (offset, func) 或
  (offset, unknown)；动态 offset 的 store、非 load/store user 直接把整个
  segment 标记 Unknown。
- 新增“裸常量地址保护”：slot 地址在模块指令 operand 里以 `ConstantInt` 出现
  （非 image GEP）时，不提升。

## 3. 工具接入

`tools/notdec-native-llvm.cpp`：

- 从 `state.memoryRanges()` 构造 segments；
- 从 `state.relocatedPointers() × plannedTargets.Direct` 构造 function slots；
- 只在 `MemoryModel == IntToPtr` 时运行；
- 输出 summary：

```text
Native data image: segments=2 skipped=0 slots=20 static=78 base_offset=3 fallback=0 unresolved=1063
```

## 4. 测试

`tests/pcode_to_llvm_test.cpp` 新增：

- `testDataImageSegmentBecomesGlobal`：常量 `inttoptr` 访问变成
  `@notdec.image.0x1000` 的 GEP，initializer 字节一致。
- `testDataImageBasePlusOffsetAccess`：`add(C, %idx)` 变成带动态 index 的 GEP。
- `testDataImageFunctionSlotKeepsInitializer`：函数 slot 字段初值是
  `ptrtoint(@function)`，并且 `load image 字段 -> inttoptr -> call` 能提升。
- 已有 `testRelocatedFunctionPointerSlotBecomesGlobal` 等三个测试保持不变。

# 验证

- `pcode_to_llvm_test`、`native_register_summary_ssa_test`、`ctest` 12 项。
- LLVM 22 `llvm-as` + `opt -passes=verify`。
- wrk 全量：warning TSV 与上一版逐行一致；RSP.entry 仍为 0；residue 不变；
  性能与上一版（1:27.2 / 138MB）对比。
- fortune x86_64 / i386 回归脚本。
- lighttpd / memcached / tmux / redis / python `--decode-seed-limit 20` 抽查
  segment 数与 IR 规模。

# 评分与下一步

- 实现效果：7/10。`.rodata`/`.data`/`.got`/`.bss` 现在是真实 LLVM object，
  常量地址和 `base + offset` 访问都落在同一个 object 里；函数指针槽位保持
  `ptrtoint(@function)`；wrk 的 promotion 没有退化，warning/residue 完全不变。
  扣分点：动态地址（参数、从 image 读出的指针）仍然分叉在裸 `inttoptr` 上，
  大目标上 `promoted` 都是 0（被动态 store 的保守 Unknown 挡住）。
- 复杂度：7/10。一个 pass 450 行左右，规则集中；但白名单和 addend provenance
  两条规则都是实现中踩坑加上的，维护者需要理解"什么才算地址"。
- 维护成本：6/10。guarded promotion 需要复用这里 image 字段的枚举和写分析；
  IPA 值集传播要接在 `resolve()` 上。

下一步（按优先级）：

1. **guarded multi-target promotion**：把 `load (gep @image, base+%idx)` 的候选
   目标集合做成 `icmp/switch + fallback indirect call`，这样 `unknown_writes`
   和 `multiple_targets` 不再一刀切地挡住提升；需要先能枚举 "table 区间内的
   槽位集合"。
2. **跨函数参数地址传播**：`call @f(i64 &g)` 之后 callee 的
   `inttoptr(%param)` 还接不上；做一个保守的 IPA（所有直接调用点一致才传播，
   地址 escape 就放弃）。
3. `@llvm.used` 兜底移除：等 data image 里的 `ptrtoint(@function)` 覆盖了所有
   真实表引用之后再删。
4. FUN_a260 的 `FS_OFFSET` residue：和本轮发现的 TLS lowering 问题同源
   （有些 `%fs:` 访问被表示成裸偏移），可以一起看。

# 实现记录（2026-09-15）

## 文件

- 新增 `include/notdec-bin2llvm/NativeDataImage.h`
- 新增 `lib/NativeDataImage.cpp`
- 改写 `lib/NativeFunctionPointerPromotion.cpp`（image 字段识别 + 写分析）
- `tools/notdec-native-llvm.cpp`：segments/slots/knownAddressBases 构造与 summary 输出
- `tests/pcode_to_llvm_test.cpp`：4 个新测试
- `ARCHITECTURE.md` 5.1/5.2 更新

## 关键实现点

- `segmentOf/findRegion`：segment 内地址查找；
- `collectModuleAddresses`：收集模块里 `inttoptr(C)` 的常量 C 作为确认基址；
- `DataImageRewriter::resolve`：`add/sub/or/zext/sext/inttoptr` 链解析，返回
  `(region, offset, dynamic, BaseKnown)`；
- `pointerFor`：常量走 `ConstantExpr` GEP；动态走常量 GEP + `CreateGEP` 单 index
  （两 index 形式要求 source element type 是聚合类型，i8 会 verifier 失败）；
- `rewriteConstant`：改写 load/store 的 `inttoptr` 常量表达式（wrk 里常量访问的
  pointer operand 就是这种形式）；
- promotion：`decomposeGlobalPointer` 用 `GEPOperator::accumulateConstantOffset`
  解析 (global, 常量 offset)；`imageFieldAt` 按 packed struct 类型/初值找字段；
  `scanGlobalUses` 递归做写分析（动态 offset store / 地址 escape => Unknown）；
- 流水线位置：pass 放在 `InstCombine -> SummarySSA -> InstCombine` 之后。
  第一版按上一轮 `NativeRelocationData` 的位置放在 lowering 之后，结果
  fortune i386 是 `static=0 base_offset=0 unresolved=1581`：i386 的绝对地址先落在
  register/unique varnode 里，只有 SummarySSA 消除寄存器、InstCombine 折叠之后才
  变成 `inttoptr (i32 C)` 常量，pass 跑在它们之前什么都看不到。移动是安全的：
  grep 确认没有别的 pass 依赖 `notdec.image.*` / `notdec.reloc.*`，promotion
  本来就在 SSA 之后。移动后 i386 变成 `static=69 base_offset=1 unresolved=334`。
- 运行期过滤：runtime glue（`__do_global_dtors_aux` 等）不做符号化。否则
  `.init_array/.fini_array` 字段的 `ptrtoint(@function)` 会把 body 留住，
  多出 `__do_global_dtors_aux` 的 `RAX.entry` residue。

## wrk 全量结果

命令与上一版一致（`--all-confirmed --register-ssa-summary`）：

```text
Native data image: segments=2 skipped=0 slots=18 static=107 base_offset=4 fallback=0 unresolved=1223
Native function pointer promotion: seen=54 promoted=20 slots=18 unknown_writes=0 multiple_targets=0
```

- 2 个 segment：`.rodata`（`@notdec.image.0xf000`，纯 `[13156 x i8]`）和 RW
  （`@notdec.image.0x135f0`，packed struct，含
  `.data.rel.ro/.got/.data/.bss`）；
- 18 个函数指针字段（20 减掉 2 个 runtime glue）；symbolic 字段例如
  `i64 ptrtoint (ptr @sock_connect to i64)`；
- 访问改写示例（最终 IR）：

  ```llvm
  ; 0x142e8 字符串表槽位
  %unique_df00_8115 = load i64, ptr getelementptr (i8, ptr @notdec.image.0x135f0, i64 3320)
  ; base + offset：0x13820 的指针表 + 运行期 index
  %notdec.image.ptr = getelementptr i8, ptr getelementptr (i8, ptr @notdec.image.0x135f0, i64 560), i64 %idx
  ```

- `static=107 base_offset=4` 是改写后的数量；`unresolved` 从 3036 降到 1223，
  因为 pass 移到 SSA/InstCombine 之后，更多 `inttoptr` 已经能被解析；
- promotion 仍是 20，和上一版一致（sock_connect/close/read/write/readable 等）；
- warning TSV 353 行集合 0 diff；RSP.entry 0；residue 2 行（header +
  `FUN_a260` 的 `FS_OFFSET`，与上一版一致）；
- LLVM 22 `llvm-as` + `opt -passes=verify` 通过；
- 性能（无其它负载，跑两次）：`1:24.40` / `1:27.70`，RSS 135964KB / 136348KB；
  上一版 `1:27.20` / 137960KB。基本不变。

## fortune 回归

- x86_64：define 13、warning 35 行、residue 1 行（空），d
  ata image `segments=2 slots=1 static=82`；
- i386：define 40、warning 9 行、residue 16 行，data image
  `segments=1 slots=1 static=69 base_offset=1`；
- 两个 `native_llvm.realworld_fortune_*` ctest 与
  `pcode_to_llvm_test` / `native_register_summary_ssa_test` 全部通过，
  ctest 12/12。

## 大目标抽查

`--decode-seed-limit 20 --no-register-ssa-pass`（大目标在 SummarySSA 上本来就
verify 失败，这是改动前就存在的情况，用一个能跑完的配置看 data image 规模）：

| 目标 | segments | slots | static | base_offset | unresolved | promotion | 备注 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| memcached | 3 | 48 | 1724 | 46 | 11935 | seen=55 promoted=0 | 修 addend 前 base_offset=1627 |
| tmux | 3 | 505 | 1259 | 6474 | 55312 | seen=100 promoted=0 | IR 68.7MB，403s / RSS 1.05GB |
| lighttpd | 2 | 12 | 403 | 0 | 19948 | seen=117 promoted=0 | 178s / RSS 358MB，IR 17.7MB |

memcached 的 `base_offset=46` 说明白名单基址 + 动态 index 的真实表访问已经接上
（修 addend 规则前是 1627，其中大部分是 FS/RSP 误判）；`promoted=0` 是因为该
配置下动态 GEP store 把整个 segment 标成 Unknown（保守），guarded promotion
阶段再处理。tmux 有 505 个函数指针槽位，说明 pointer table 规模可以很大。

# 风险

- **语义分叉**（见上）：常量访问与动态访问不共享 object。评估标准是
  “比现状更接近可执行语义、warning/regression 不变”，不是完全语义等价。
- **IR 体积**：python 这类大 segment 会产生 MB 级 initializer。用
  “只建被引用 segment” + `MaxSegmentSize` 兜底，先在小目标上验证。
- **改写遗漏**：`inttoptr` 可能以 ConstantExpr 形式嵌在其他常量里；
  第一版只处理 instruction operand 和 global initializer，其余计入
  UnresolvedIntToPtrs。
