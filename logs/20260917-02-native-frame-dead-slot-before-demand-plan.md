# native 帧死槽 partial read 不产生寄存器需求（plan + 记录）

## 用户原始 prompt

> 对，按这个推进试试

（上一轮结论第 1 条：memcached 里 7 个函数各有 8 个完全没人用的 ZMM 入口参数，调用方被迫
在调用点造 `ZMM*.arg_unknown`——序言把 ZMM0..ZMM7 存进栈帧后再没读过，但需求分析把它算成
入口读。）

## 已有状态 / 问题证据

- 基线：wrk unknown 57、memcached unknown 1181（其中 `call_arg_rewrite_missing_binding` ~400、
  ZMM `range_unknown` 496）。
- 14 个 memcached 函数带 ZMM 参数，102 个 `i128 %ZMMn.arg`；其中 57 个参数在函数体里完全
  没用（7 个函数整批 8 个）。
- 原始 IR（`--no-register-ssa-pass`）里这些函数序言是：

```llvm
  %unique_6b80_16 = call i128 @notdec.partial_read.i512.i128(ptr @ZMM0, i64 0)
  store i128 %unique_6b80_16, ptr %frame_slot      ; 之后 0 次 load（逐偏移统计）
```

- 帧重写（`runNativeStackFrameRewrite`）只做地址本地化；帧槽死值清理在
  `runNativeStackFrameCleanup` 里、排在需求分析和签名改写之后，所以需求分析仍看到这些读。

## 目标与判断标准

1. memcached 的 `unused ZMM args` 归零、ZMM 参数消失，调用点不再造 `ZMM*.arg_unknown`；
2. memcached unknown 明显下降，wrk 不回归；IR 通过 LLVM 22 `opt -passes=verify`；
3. ctest 全绿、fortune i386/x86_64 回归不变量不变；
4. 不因为这次改动丢掉任何函数或函数地址（这是方案 A 踩过的坑，见下）。

## 路线与取舍

### 方案 A（先试后否决）：把帧槽死值清理提前到需求分析之前

新增 `runNativeStackFrameSlotCleanup(module)`（只跑 `cleanupStackAllocaAccesses`），在
`runNativeRegisterSummarySSA` 里紧跟帧重写调用。

实测（memcached / wrk）：unknown 1181 → 1186、57 → 57（**没有达到目标**），IR 5.65 MB →
5.61 MB，warning 4974 → 4579，但 `defines` 230 → 229、wrk 87 → **84**：
`store i64 ptrtoint (ptr @FUN_6480 to i64), ptr %notdec_stack.native.ptr295` 和
`%0 = zext i64 ptrtoint (ptr @FUN_9990 to i64) to i128` 被清掉，GlobalDCE 随后删了
FUN_9990/FUN_99d0/FUN_6480。**根因**：帧的“逃逸”状态会在 SSA 阶段变化（栈地址本地化之后
才变逃逸），提前清理时帧看起来没逃逸，于是内容 store 被当死值删掉。另外它只删 store，
变成死值的 `partial_read` helper 调用还留着，需求照样产生。**结论：不安全且无效，回退。**

### 方案 B（采纳）：需求侧不把“只喂死帧槽的 partial read”算作读

在 `NativeRegisterSummary.cpp` 加三个 helper：

- `isNativeFrameSlotPointer`：指针是否落在 `notdec_stack.native` 的 alloca/GEP 上；
- `frameSlotPointerIsNeverLoaded`：该槽位指针上没有 load（判据与帧清理的
  `stackAllocaStoreIsLoaded` 一致：只认指针相等的 load；子偏移/位转换保守当成“会读”）；
- `partialReadFeedsOnlyDeadFrameSlots`：partial read 的结果只被这样的槽位 store 消费，
  或者根本没有使用者。

两处 gate：

1. 前向 transfer 的 partial read 分支不再 `readRegister`（不记入口读）；
2. 反向 demand 的 partial read 分支不再 `addDemand`（不再向上游要值）。

依据：帧清理本来就会把“值来自寄存器溢出、同槽位没有 load”的 store 当死值删掉，所以这个值
在管线里已经被判死，只是需求分析还没跟上。方案 B **不删任何 IR**，只影响需求，因此没有
“丢函数/丢 store”的风险。

## 验证

命令：`cmake --build build -j 8 --target notdec-native-llvm native_register_summary_ssa_test`、
`native_register_summary_ssa_test`、`notdec-native-llvm <bin> --all-confirmed --register-ssa-summary`、
`opt -passes=verify`、`ctest`、`scripts/native-fortune-{i386,x86_64}-regression.sh`。

| 指标 | 改前 | 改后 |
|---|---|---|
| memcached unknown | 1181 | **86**（−93%） |
| memcached IR bytes | 5,653,614 | 4,069,860（−28%） |
| memcached warning 行 | 4974 | 829（−83%） |
| memcached defines | 230 | 230 |
| memcached ZMM 参数 | 102（57 个没用） | 0 |
| wrk unknown | 57 | 57 |
| wrk IR bytes | 1,025,789 | 1,022,698 |
| wrk warning / defines | 311 / 87 | 311 / 87 |
| fortune i386 unk / bytes / warn | 2 / 248,050 / 9 | 2 / 248,050 / 9 |
| fortune x86_64 unk / bytes / warn | 0 / 293,816 / 34 | 0 / 293,746 / 34 |
| verify（LLVM 22） | OK | OK |
| ctest | 12/12 | 12/12 |

单测新增两个（`tests/native_register_summary_ssa_test.cpp`）：

- `testDeadFrameSpillPartialReadCreatesNoEntryParam`：partial read 只喂一个没人读的
  native frame 槽 → 不产生 ZMM 参数、也不产生 `notdec.unknown`；
- `testLiveFrameSpillPartialReadKeepsValue`：同一形态但槽位被 load → 值必须仍然存在
  （参数 / unknown / 入口读任一种形态都算通过）。

改后 memcached 剩的 86 条 unknown：DF 22、RBP 20、RBX 18、R9 6、RDI 4、RDX 4、ZMM0 2、
RSI 2，其余是 4 条调用点 clobber；`call_arg_rewrite_missing_binding` 全部消失。

## 效果、理解与维护成本

- 效果：memcached unknown 从 1181 砍到 86，IR 少 28%，warning 少 83%，而且没有新增任何
  “删 IR”的路径，函数/函数地址一个没丢；wrk 完全不动。
- 理解成本：多一条“寄存器溢出的帧槽值不算可观测值”的判据——它其实和帧清理已有假设同源，
  注释里写清楚了。
- 维护成本：槽位判据沿用指针相等（与帧清理一致）；如果将来帧重写不再保证“一个槽位一个
  GEP”，这里要跟 `stackAllocaStoreIsLoaded` 一起改成按偏移判。

## 遗留 / 不做什么

- 只处理“值只被死帧槽消费”的读；存到其它没人读的位置（全局、escaping 指针）的死读没管；
- 帧逃逸状态下“callee 会不会读这个溢出槽”按项目既有假设（寄存器溢出不是帧内容）处理，
  与帧清理的删除判据一致；
- 没动 unknown-external 返回模型（wrk 的 RDX 34 条）、x87 窗口（21 条）和 memcached 剩的
  86 条（DF/RBP/RBX/ZMM0 等），留作后续。
