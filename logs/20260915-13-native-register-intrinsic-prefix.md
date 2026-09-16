# 追加：intrinsic 前缀定为 notdec.register.（call 方案的两个 blocker）

按用户要求，把"内置 intrinsic"的命名空间从裸 `notdec.` 收紧为 **`notdec.register.`**：

- `NativeRegisterSummary.cpp` `isNotDecRegisterHelperCall()`：
  `notdec.register.*` + 历史前缀（`notdec.unknown.*`、`notdec.x87.*`、value-range）；
- `NativeRegisterSummarySSA.cpp` `isUnknownExternalFunction()`：同样处理；
- `NativeRegisterSummarySSA.cpp` `buildInitialSignatureShapes()` 过滤：同样处理。

约定：**新增寄存器模型 intrinsic 统一用 `notdec.register.` 前缀**，不再需要往三个名单里补；
历史前缀继续显式列出来保持兼容。keep-alive 调用因此改名为
`notdec.register.native_frame.keep`。

## call 方案实测：还是不行，而且是两个独立原因

复现命令：入口插 `call void @notdec.register.native_frame.keep(ptr %frame)`
（每帧一次）+ "非 spill 帧 store 一律保留"。

1. **range/clobber 那条路已经通了**：加前缀后 warning TSV 回到 348 行
   （之前 813 行、217 条来自 keep-alive 的 clobber 都没了），说明
   `isAnalyzableCall`/`applyCallEffect`（`NativeRegisterSummarySSA.cpp` 5761 起）
   确实会把 `notdec.register.*` 调用当作"不改寄存器"。
2. **真正挡住的是 `NativeStackFrame` 自己的一个支配 bug**：
   ```
   Instruction does not dominate all uses!
     %notdec_stack.native.ptr222 = getelementptr ..., i64 32
     %notdec_stack.native.int47 = ptrtoint ptr %notdec_stack.native.ptr222 to i64
   module verification failed after summary register SSA pass
   ```
   帧一旦能活到清理遍，`rewriteFunctionStackAccesses()` 的"复用已有 GEP"
   逻辑（`stackPointers.emplace(nativeOffset, gep)`，注释说为了让 clean pass
   按指针 identity 比较）会复用一个**位于使用点之后**的 GEP，于是新生成的
   `ptrtoint` 用了不支配它的指针。

## 下一步（很具体）

1. 修 `NativeStackFrame.cpp` 的 GEP 复用：只复用**支配使用点**的 GEP，否则在
   `storage` 后（entry builder 位置）新建一个；顺带把 store/load 匹配改成
   "alloca + 字节区间"而不是指针相等。
2. 然后重跑 call keep-alive + 非 spill 帧 store 全保留，预期：
   `FUN_9990`/`FUN_99d0` 回来（define 85 -> 87）、warning 仍 348 行、
   residue 1 行、IR +<1%、时间 ~41s。

## 追加：按上面第 1 条试过一轮，仍未通过

改动（未提交，已回退）：

1. `attachNativeFrameKeepAlive()`：入口插 `call void @notdec.register.native_frame.keep(ptr %frame)`；
2. `isFrameClusterInstruction()` + GEP 复用只认 entry block 里紧跟 alloca 的"帧指针簇"
   （GEP + keep-alive 调用），避免复用定义在使用点之后的旧 GEP；
3. 非 spill 帧 store 一律保留。

结果：仍然 `module verification failed after summary register SSA pass`，
两处 `Instruction does not dominate all uses`，形态是

```text
%notdec_stack.native.ptr2238 = getelementptr inbounds nuw i8, ptr %notdec_stack.native, i64 192
%notdec_stack.native.int921 = ptrtoint ptr %notdec_stack.native.ptr2238 to i64
```

即：帧能活得更久之后，仍然存在**在 alloca 簇之外被复用/创建的 `notdec_stack.native.ptrNNNN`**
（编号形态说明不是 `createStackFramePointer()` 那个带名字的路径，或是在簇被清理后
`storage->getNextNode()` 已经指向别处时新建的），它们的 `ptrtoint` 用户出现在更早的位置。
运行时间也从 41s 涨到 67s（保留的 IR 更多）。

下一步（比上一轮更具体）：

1. 找出所有创建 `notdec_stack.native.ptr*` 的路径（`grep -n "notdec_stack.native.ptr"
   lib/`），统一走"紧跟 alloca 插入"的单一 helper；`NativeRegisterSummarySSA` 里
   寄存器 spill 的栈槽也走同一套。
2. 或者更省事：不做 keep-alive，改成在 `rewriteFunctionStackAccesses` **结束时**
   统一把簇里的 GEP/ptrtoint 提到 entry block 最前面（alloca 之后），保证支配。
3. 修完再重跑本轮的三条改动，预期不变：`FUN_9990`/`FUN_99d0` 回来、warning 348 行、
   residue 1 行。

本轮已提交的只有前缀收紧（wrk 中性：348 行 diff 0、IR +26 B、ctest 12/12）。

## 追加：支配 bug 定位 + 帧保活落地（本轮完成）

本轮 prompt：

```text
按照这个思路继续推进试试
```

### 1. 根因：复用的 GEP 落在新建 ptrtoint 之后（最小复现）

上一轮"存在 alloca 簇之外的创建路径"的判断不成立：`notdec_stack.native.ptrNNNN`
只有 `NativeStackFrame.cpp createStackFramePointer()` 一个创建点，数字后缀只是
LLVM 给同名值自动加的编号。真正的原因在**复用**路径：

`rewriteFunctionStackAccesses()` 在一次 pipeline 里跑两次（早期 rewrite + 后期
cleanup）。第二次跑时：

- `stackValueBuilder` 锚在 `storage->getNextNode()`，也就是第一次跑留下的
  第一个簇值之前；
- `stackPointers` 复用第一次留下的 GEP，它们全部排在锚点**之后**；
- 于是第二次给某个 offset 新建的 `ptrtoint` 排在复用的 GEP 前面 →
  `Instruction does not dominate all uses`。

基线不复现，是因为基线里帧活不到 cleanup：第二次跑走 `storage == nullptr`
分支，全部新建、没有复用。

新增单测 `testLateStackIntegerAccessReusesDominatedFrameSlot()`（216 字节 frame +
offset 8 的 slot GEP + 一个把 `rsp-208` 当整数传给 call 的晚访问）复现了同一个形态：

```text
module failed verifier after late stack integer reuse
Instruction does not dominate all uses!
  %0 = getelementptr inbounds i8, ptr %notdec_stack.native, i64 8
  %notdec_stack.native.int = ptrtoint ptr %0 to i64
```

### 2. 修复一：帧指针簇统一锚在 alloca 之后

`NativeStackFrame.cpp`：

- 复用 GEP 时用它 `moveAfter(clusterTail)` 搬回 alloca 之后的簇里；
- `stackValueBuilder` 改成 `IRBuilder(storage->getParent(),
  std::next(clusterTail->getIterator()))`，锚在簇尾之后（顺带避开
  `storage->getNextNode() == nullptr` 的情况）。

每个 offset 仍然只有一个 GEP（cleanup 的指针 identity 匹配不变），而且本轮新建的
值一定排在所有复用值之后，支配关系成立。

### 3. 修复二：帧保活 marker

`attachNativeFrameKeepAlive()`：在帧 alloca 之后插
`call void @notdec.register.native_frame.keep(ptr %frame)`。

- 条件：`rewritten != 0` 且函数里存在 call。没有 call 的函数，帧不可能被 callee
  读到，死 store 继续按原来的 liveness 规则删
  （`testSummarySSARemovesDeadStackFrameStore` 因此保持通过）；
- 幂等：已有 keep 调用就不再插（早期 rewrite 插过，cleanup 不会插第二个）；
- `notdec.register.*` 调用对寄存器模型中性（上一轮的前缀收紧），warning 不增。

上一轮计划的第 3 条（"非 spill 帧 store 一律保留"）**不再需要**：marker 让帧在
`collectEscapedStackAllocas()` 里直接就是 escaped，现有 "escaped + 非 spill 保留"
规则已经覆盖，少一条规则、少一份 IR。

### 4. 验证（wrk，本机 Debug 构建）

命令：

```bash
build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk \
  --all-confirmed --register-ssa-summary \
  --register-ssa-warning-out /tmp/nb-frame/cond.warn.tsv \
  -o /tmp/nb-frame/cond.native.ll
```

| 项 | 基线 | 本轮 |
| --- | --- | --- |
| define 数 | 85 | **87**（`FUN_9990`/`FUN_99d0` 回来） |
| `ptrtoint (ptr @...)` 使用数 | 21 | 23 |
| warning TSV | 348 行 | 348 行，diff 0 |
| residue audit | 1 行 | 1 行 |
| IR 体积 | 1,862,313 B / 26,993 行 | 1,952,740 B / 27,539 行（+4.9%） |
| 帧 keep 调用 | 0 | 75（wrk 共 75 个带 call 的帧） |
| LLVM 22 verify | 通过 | 通过 |
| ctest | 12/12 | 12/12（含 fortune x86_64/i386） |
| 时间 / 内存 | 77.5s / 51.0s / ~137MB | 64.5s / 49.2s / ~137MB |

时间用同一台机器上的交错 A/B 测（分别 build 出基线/本轮两个二进制，按
base-fixed-base-fixed 各跑一遍）：本轮两次都不慢于基线。这台机器抖动很大
（同一份基线代码两次 77.5s vs 51.0s；11 号 log 记的 41s 也是同一份代码），所以
按"无回归"读，不按绝对值。

单测反向验证（两个新测试各钉一条）：

- 簇锚定：先加 `testLateStackIntegerAccessReusesDominatedFrameSlot()`、还没改
  `NativeStackFrame.cpp` 时，测试复现了 §1 的同一形态 dominator 错误；改完通过；
- 帧保活：临时去掉 `attachNativeFrameKeepAlive()` 调用重跑，
  `testNativeFrameContentStoreSurvivesSummarySSA()` 报
  `native frame was not marked address-taken for the pipeline`；恢复后整个
  `native_register_summary_ssa_test` 通过。

### 5. 代价：IR +4.9% 的构成

final IR 的函数 body 行数 26,216 -> 26,624，增量集中在：

| 函数 | 行数 | 内容 |
| --- | --- | --- |
| `main` | +61 | 回调表 store + frame 保活 |
| `FUN_8530` | +81 | 帧保活 + 帧转发恢复出的跨块数据流 |
| `FUN_9990`/`FUN_99d0` | +32/+36 | 恢复的两个函数本体 |
| `stats_within_stdev` | +35 | 帧 store/load 保留后分支不再被折叠 |
| `zmalloc`/`zcalloc`/`zrealloc` | +6/+6/+7 | 帧保活 + `%RAX.range_summary_ssa` phi |
| `FUN_81d0` | +11 | frame 28 -> 56 字节（不再被换成一个更小的临时帧） |
| 其它 | 其余 | 主要是 75 条 keep 调用 |

也就是说：marker 本身只有 75 行，绝大部分增长是"原来被 InstCombine 当死 alloca
删掉的帧 + 依赖这些帧才成立的跨块值流"。属于拿 IR 体积换语义。

### 6. 顺带：memcached 全量在基线上本来就验证失败

用同一对二进制（基线 / 本轮）跑了 memcached 全量：

```bash
<bin> /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached \
  --all-confirmed --register-ssa-summary \
  --register-ssa-warning-out /tmp/nb-frame/memcached.<bin>.warn.tsv \
  -o /tmp/nb-frame/memcached.<bin>.ll
```

| | 基线 | 本轮 |
| --- | --- | --- |
| 结果 | exit=1，`module verification failed after summary register SSA pass`（408s 时中断） | exit=0（654s） |
| 失败形态 | `ptr9140 = gep(..., 928)` / `int6856 = ptrtoint %ptr9140`，与 §1 同一形态 | — |
| define 数 | —（无输出） | 230 |
| warning TSV | — | 4,971 行 |
| IR | — | 10,759,379 B |
| residue | — | 5 行 |
| keep 调用 | — | 200 |

即这个支配 bug 不是 keep-alive 才有的：memcached 全量在**基线**上就撞。本轮的簇
锚定把它一并修掉，memcached 从"验证失败"变成"跑完 + 验证通过"。

### 7. 没做 / 后续

- 没做"非 spill 帧 store 一律保留"：被 marker 覆盖（见 §3）。
- 没做 store/load 匹配改 "alloca + 字节区间"：簇里每个 offset 仍然只有一个 GEP，
  指针 identity 匹配保持成立。
- 若以后 IR 体积成为问题，可把 marker 收窄为"帧里存了函数地址才插"，只覆盖
  callback 表这一类；代价是留下一般性的"内容 store 被当死 store 删"缺口。
- lighttpd 全量比 memcached 更大（34k loads），A/B 没跑完就停了；需要时再补。
- 按 func 保活的下一步仍是 06/11 号 log 记的：settings 指针在寄存器模型里被
  重建成 `summary_clobber` 那条线（本轮的 marker 只是让这类丢失不再导致内容被删，
  没有把指针找回来）。
