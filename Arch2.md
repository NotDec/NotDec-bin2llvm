# Arch2.md — 寄存器消除的架构变迁史（bin2llvm）

> 本文只讲一条线：**寄存器消除**——把机器寄存器（`RAX`、`RSP`、`ST0` 这些）从反编译生成的
> LLVM IR 里消灭掉，变成普通变量、函数参数和返回值。
>
> 写作目标：没接触过这个项目的人也能看懂。尽量少用专有名词；第一次出现的术语都附一句人话解释。
>
> 主线：这个架构不是一次性设计出来的，而是被真实二进制文件一个接一个地"逼"出来的。每一代都配一张
> 当时的流程图，文字讲"和前一代相比流程多了/改了/删了什么"，以及为什么不得不这么改、改完又暴露了
> 什么新问题。

## 0. 背景：寄存器在反编译结果里长什么样

反编译流程：机器码 → Ghidra 的 p-code（指令语义的中间语言）→ LLVM IR → 输出 `.ll` 文件。

在最早期，寄存器被直接表示成 **LLVM 全局变量**：`@RAX`、`@RSP` 这样的程序级变量。指令读写寄存器，
翻译成 IR 就是 load/store（读/写内存）这个全局变量。

这样做的缺点很明显：

- 每个函数看起来都像在通过全局内存和其他函数"偷偷通信"，看不出参数和返回值；
- 代码里挤满 load/store，后续任何优化都做不动；
- 函数边界（谁给我传值、我把什么传出去）被藏在一堆内存操作里。

所以要做"寄存器消除"：把寄存器读写变成普通变量、函数参数、返回值，让 IR 重新像一个正常程序。

先放几个术语，后面不再解释：

- 寄存器：CPU 内部的高速存储位置（`RAX`、`RSP`、`XMM0`、`ST0` 等）。
- 全局变量：整个程序共享的变量。
- SSA：一种"每个变量只赋值一次"的表示，让每个值都有唯一来源，方便分析。
- PHI：SSA 里表示"几个分支的值在合流处汇合"的特殊节点。
- pass：LLVM 里的一个处理步骤。
- 静态分析：不运行程序，只看代码结构推断信息。
- 调用约定（ABI）：规定参数放哪些寄存器、返回值放哪些寄存器、哪些寄存器被调用后必须保持不变的规则。
- 调用点：程序中调用某个函数的那条指令。
- clobber：寄存器被一次调用"弄脏"（值被改写）。
- callee-saved：ABI 规定被调用方必须保持原值的寄存器；函数开头保存、结尾恢复。
- 部分读写：只读写寄存器的一部分（如只写 `EAX`，`RAX` 的高位不变）。
- 帧指针/栈指针：`RSP` 指向当前栈顶，`RBP` 常用来定位局部变量；它们管的是内存栈，不是普通值。
- intrinsic：一种内建函数，编译器/分析器特殊处理它。
- 标志寄存器：记录运算结果的零散位（`ZF`、`CF`、`DF` 等）。
- x87：老式 x86 浮点单元，8 个浮点寄存器（`ST0`..`ST7`）组成一个"栈"。

## 1. 第 0 代（2026-05 前）：起点——寄存器全是全局变量

当时的流程：

```text
机器码
  -> Sleigh p-code          （Ghidra 的指令语义中间语言）
  -> PcodeToLLVM            （转成 LLVM IR）
      寄存器 = 全局变量：@RAX、@RSP、@XMM0 ...
      读写寄存器 = load / store 这个全局变量
  -> LLVM IR：到处都是寄存器 load/store，没有参数、没有返回值
```

提升层（`RegisterStorage`）做了一件事：把重叠的寄存器（`RAX`/`EAX`/`AX`/`AL`）合并到同一个全局
变量，用位切片处理子寄存器读写。但它只解决"怎么表示"，不解决"值从哪里来"。

同时期还有一条模仿 Ghidra heritage 的对照链路（把 Ghidra Java 反编译器的 heritage 结果导入
LLVM），它用 Ghidra 的"trial/use"机制判断参数，参数判断依赖"调用点有没有准备参数的指令"这类局部
模式匹配，规则重、难扩展。寄存器消除的正戏后来都在新的 native 链路上展开，heritage 链路逐渐退化成
对照。

**卡住的问题**：每个函数看起来都像在通过全局内存和其他函数"偷偷通信"，函数边界被藏在一堆内存操作
里，后续任何分析都用不了。

## 2. 第 1 代（2026-05-29）：第一个寄存器 SSA pass

和前一代相比，流程上**多了一个 pass**：

```text
机器码 -> p-code -> PcodeToLLVM
  -> NativeRegisterSSA                    <- 新增
      读寄存器：向前找最近的写
        找到了  -> 用 SSA 值替换 load
        找不到  -> 说明是外部传进来的值（候选参数）
      多个前驱汇合 -> 插 PHI
  -> LLVM IR：整寄存器读写变成 SSA 值
```

**为什么改（关键问题）**：寄存器全是 load/store，看不出"这个值从哪里来"，没法给后面的原型恢复用。
SSA 恰好能表达"每个值都有唯一来源"，于是引入按需构建 SSA 的 pass（参考《Simple and Efficient SSA
Construction》）：用到寄存器时再往前查，不用预先算复杂的前置分析。目标是对标 Ghidra 的 heritage
输出——大部分寄存器不再反复 load/store，而是变成 SSA 值、参数、返回值和必要的 PHI。

**效果**：整寄存器粒度的读写被替换成 SSA 值。

**改完又暴露的新问题**：

1. 只处理"整个寄存器"的读写。部分读写（`EAX`、`XMM` 的 lane）进不了 SSA，消不掉。
2. call 对寄存器的影响没有数据流表达，实现里经常用 `undef`/空值糊结构。"调用后 `RAX` 是什么"
   没人说得清，后续分析也没法用。
3. `RSP`/`RBP` 是栈指针，不能当普通寄存器消。
4. "哪些寄存器是参数、哪些是返回值"这个信息，SSA 本身给不出来，需要另一套分析。

## 3. 第 2 代（2026-06-16~18）：引入寄存器静态分析（register summary）

和前一代相比，流程上**把"先建 SSA 再猜边界"反过来，变成"先静态分析出边界，再建 SSA"**，并且
**换了一条主链**：

```text
机器码 -> p-code -> PcodeToLLVM
  -> NativeRegisterSummary                 <- 新增：静态分析
      自底向上：每个寄存器记三类事实
        "可能仍是入口值" / "可能被函数改过" / "入口值被读过"
      递归函数：按调用图 SCC（互相调用的循环组）反复迭代到稳定
      自顶向下：从顶层 ABI 开始，传"调用者真会用的返回寄存器"（demand）
  -> NativeRegisterSummarySSA              <- 新增：基于 summary 的新 SSA
  -> LLVM IR
```

旧链路（按需 SSA + Ghidra trial/use 风格）改名 heritage 对照链，退居二线；summary 链成为默认主链
（2026-06-18）。

**为什么改（关键问题）**：函数签名（参数/返回）推断需要三个信息，SSA 给不出：

- 入口哪些寄存器被真实读过 → 参数候选；
- 出口哪些寄存器被改过且调用者真的用了 → 返回候选；
- call 会弄脏哪些寄存器 → clobber。

于是引入**寄存器静态分析**：不运行程序，沿函数控制流算每个寄存器"可能是什么值、被谁读过"。

- **自底向上**：从被调用者向调用者汇总。每个寄存器记"可能仍是入口值 / 可能被函数内部改过 / 入口值
  在被覆盖前被读过"三类事实；递归、互相调用的函数按调用图 SCC 反复迭代到不再变化。
- **自顶向下（demand）**：从遵守 ABI 的顶层开始，把"调用者真正会用的返回寄存器"传下去，过滤掉
  "改了但没人用"的寄存器——函数尾部往往改了一堆寄存器，但真正的返回值只有调用者用到的那些。
- 内部调用直接用 callee 的 summary；外部调用、间接调用先按 ABI 兜底。

**为什么换主链**：heritage 链路靠"调用点有没有准备参数的指令"这类局部模式猜参数；summary 链用全局
静态分析，更系统、更好扩展。

**改完又暴露的新问题**：SSA 构建和函数签名重写强耦合。分开两个 pass 时，签名重写完，旧寄存器 store
又"活"过来了，还要额外清理，两套逻辑很难保持一致。

## 4. 第 3 代（2026-06-19）：函数签名重写并入 SummarySSA

和前一代相比，流程上**删掉了独立的签名重写 pass，把它折进 SummarySSA**：

```text
机器码 -> p-code -> PcodeToLLVM
  -> NativeRegisterSummary
  -> NativeRegisterSummarySSA              <- 扩展：SSA + 签名重写 + 清理
      SSA 构建
      addDemandedExternalReturns
        （"调用后被读的 ABI 输出寄存器" -> 补返回槽）
      重写函数类型（内部函数 + 外部声明）
      调用前寄存器 store -> 变成 LLVM call 参数
      返回寄存器 -> 变成 LLVM 返回值
      清理暴露出来的死 store
  -> LLVM IR
```

**为什么改（关键问题）**：参数/返回信息算出来后，要改写函数类型和调用点——把"先 store 寄存器，再
call"变成"直接传参数"，把返回寄存器变成 LLVM 返回值。这件事和 SSA 构建强耦合，分开做两边都对不齐：
重写完旧寄存器 store 又活过来，还得额外清理。

**方案**：删掉独立的 `NativeExternalCallSignatureRewrite`，把签名重写按顺序折进
`NativeRegisterSummarySSA`：先建 SSA，再按 SSA 结果重写函数类型、call 参数、返回值，最后清理死
store。同时引入 `addDemandedExternalReturns`：给"调用后被读的 ABI 输出寄存器"补返回槽。

注意：从这一天起，返回推理就是放在 SSA 构建**之后**跑的——因为"调用后哪个寄存器被读了"这个证据是
在 SSA 构建过程中产生的。这个决定后来埋了雷，见第 8 代。

**改完又暴露的新问题**：`RSP`/`RBP` 和 callee-saved 保存恢复。函数开头 `push rbx`、结尾 `pop rbx`
这类机械动作被当成"读了入口寄存器"，导致函数被推断出大量假参数（`main` 甚至带上了 RBX/R12-R15
参数）。

## 5. 第 4 代（2026-06-19 起）：栈帧恢复——RSP/RBP 不能当普通寄存器消

和前一代相比，流程上**在整条链最前面加了两个 pass，并扩展了静态分析**：

```text
机器码 -> p-code -> PcodeToLLVM
  -> NativeStackFrameRewrite               <- 新增
      RSP 栈流量 -> alloca / 栈对象
      RBP 只在证明来自 RSP 时才替换
  -> NativeStackCanaryCleanup              <- 新增：删栈保护检查
  -> NativeRegisterSummary                 <- 扩展
      跟踪"入口 RSP + 常量偏移"，识别连续 push/pop 的保存槽
      callee-saved 保存/恢复不再算"读了入口寄存器"
  -> NativeRegisterSummarySSA
  -> LLVM IR
```

**为什么改（关键问题）**：

- `RSP` 是栈指针：函数开头分配栈、结尾释放、call 压返回地址、栈参数传递都走它。把它当普通寄存器消，
  栈语义全乱。
- `RBP` 有时是帧指针（用来定位局部变量），有时只是普通 callee-saved 寄存器，不能一刀切。
- callee-saved 的保存/恢复序列（`push rbx` 开头、`pop rbx` 结尾）是"机械动作"，不该算作函数真实
  读写了这些寄存器。

**方案**：`NativeStackFrameRewrite` 把能证明为本地栈帧的 `RSP` 流量转成 LLVM 的 alloca（栈上分配）/
栈对象；`RBP` 只在"当前值已证明来自 `RSP`"时才替换。静态分析里跟踪"入口 `RSP` + 常量偏移"，识别
连续 push/pop 形成的保存槽，callee-saved 保存/恢复不再污染"读了入口寄存器"的事实，函数不再多出假
参数。之后又陆续加了 canary 清理（删 `__stack_chk_fail` 检查）和栈参数绑定（把调用前压栈的参数也绑
成 call 参数）。

**改完又暴露的新问题**：部分读写仍然消不掉（第 5 代解决）；外部函数没有原型，签名靠猜（第 6 代）。

## 6. 第 5 代（2026-07 上中旬）：按"位区间"的 range SSA

和前一代相比，流程上**改动发生在三个位置**：

```text
机器码 -> p-code -> PcodeToLLVM            <- 扩展
      部分读写先展开成 partial_read / partial_write intrinsic
  -> NativeStackFrameRewrite / CanaryCleanup
  -> NativeRegisterSummary                 <- 扩展：effect / demand 按 bit 区间算
  -> NativeRegisterSummarySSA              <- 重写：SSA 变量 = 寄存器的某段 bit 区间
      RAX[0:32]、RAX[32:32]、ZMM0[0:64] 各自有独立 SSA 值和 PHI
  -> NativeRegisterFinalCleanup            <- 新增：删无用全局 / helper
  -> LLVM IR
```

**为什么改（关键问题）**：x86 指令大量只写寄存器的一部分——写 `EAX` 保留 `RAX` 高位、写 `XMM` 低
lane 保留高位、读写 8/16 位。整寄存器粒度的 SSA 面对部分读写时：

- 只读 `EAX` 却把整个 `RAX` 拖进来，生成 64 位 PHI；
- SIMD 低 lane 拖进整个 `ZMM0`，PHI 类型可能变成 512 位；
- 部分写没法进同一套"当前定义"表，之后的部分读要反向扫描补洞；
- 参数、返回、clobber、入口输入也容易按整寄存器扩散，什么都消不干净。

**方案**：

- 提升层先把部分读写展开成 `notdec.partial_read` / `notdec.partial_write` intrinsic（先读完整值，
  再切片/拼装）；
- SSA 的"变量"从"整个寄存器"改成"寄存器的某一段 bit 区间"（range）：`RAX[0:32]`、`RAX[32:32]`、
  `ZMM0[0:64]` 各自有独立的 SSA 值和 PHI，完整寄存器读写变成若干区间的拼接/拆分；
- 活跃性（哪些值还用得上）、demand（哪些 bit 被需要）也都按 bit 区间做；
- 末尾加 `NativeRegisterFinalCleanup`，统一删无用全局和 helper。

**配套修正**：

- InstCombine（LLVM 通用化简 pass）可能把寄存器 load 变成"PHI 指向的 load"，静态分析认不出这是寄存器
  访问，需要先规整回"load 的 PHI"（`canonicalizeRegisterPointerPhiLoads`）；
- 未知值用可清理的 opaque helper 表示，而不是 `freeze`/`undef`，方便后面统一清理。

**改完又暴露的新问题**：外部函数参数/返回还是靠猜，而且猜测证据和 clobber 混在一起会漏参（第 6 代
后半）。

## 7. 第 6 代（2026-07-03~21）：外部函数签名推断 + 两遍 summary

和前一代相比，流程上**把一遍 summary 拆成两遍，中间夹一个参数推断**：

```text
机器码 -> p-code -> PcodeToLLVM
  -> NativeStackFrameRewrite / CanaryCleanup
  -> 第一遍 NativeRegisterSummary          <- 变化：只做 effect + 收集调用点证据，不跑 demand
  -> 外部参数推断 inferExternalCallShapes  <- 新增
      按"调用前被显式写入的连续 ABI 参数前缀"数 arity
      区分整型 / 浮点（XMM）参数序列
      vararg 按调用点推断尾参
  -> 第二遍 NativeRegisterSummary          <- 变化：带最终签名重跑完整分析
      （自底向上 + 自顶向下 demand + 最终 metadata）
  -> NativeRegisterSummarySSA
  -> LLVM IR
```

**为什么改（关键问题）**：外部函数（libc 等）在 IR 里只有一句 `declare`，参数、返回都不知道。没有
签名就没法重写调用点，调用后哪些寄存器是返回值也无法分析。

**方案演进**：

1. **参数证据收紧**。第一版（07-03）把"被调用弄脏的寄存器"也算进参数/返回证据，严重过数：`RDX`
   经常只是被弄脏，却被当成第二返回值，参数前缀也会被 clobber 值污染。改成只数"调用前被本函数
   显式写入的连续 ABI 参数前缀"（LocalDefinition），clobber 不算。
2. **量化手段**。加 `--register-ssa-warning-out`，把残留的 helper、未知外部签名、未消除寄存器访问
   落盘成 warning 表，批量定位剩余问题。
3. **两遍 summary（07-10）**。参数是"调用前"信息，第一遍就能收集齐，不依赖返回需求。于是改成：
   第一遍只做自底向上 effect + 收集每个调用点的证据（不跑 top-down demand）；中间做外部参数推断
   （arity、整型/浮点、vararg 按调用点推断尾参）；第二遍带最终签名重跑完整分析。
4. **调用点证据分类（07-21）**。区分三类：`Entry`（函数入口带来的值）、`Local`（本函数显式写的）、
   `CallClobber`（call 隐式弄脏的）。之前把"来自 call 的显式写入"和"call 隐式弄脏"混成一类，导致
   `malloc` 的返回值存进 `RSI` 时 `RSI` 被当成 clobber、参数推断漏参。改完后 `Local` 就是 `Local`，
   路径合并分不清时保持 `Mixed`。

**改完又暴露的新问题**：返回信息仍来自第一遍在调用点的 return/clobber 区分——这是个鸡生蛋问题，
外部浮点返回（`XMM0`/`ST0`）也接不住（见第 8 代）。

## 8. 第 7 代（2026-08-04~05）：特殊寄存器——DF 标志与 x87

和前一代相比，流程上**改动集中在两端**：

```text
机器码 -> p-code -> PcodeToLLVM            <- 扩展
      x87 指令按助记符折叠 / 窗口展开（ST0、ST1 建全局，ST2..ST7 留在库内）
  -> NativeStackFrameRewrite / CanaryCleanup
  -> 第一遍 summary                         <- 扩展：ST0/ST1 也参与 effect 分析
  -> 外部参数推断                           <- 扩展：XMM 参数证据区分"真浮点写"和"整数打包"
  -> 第二遍 summary
  -> NativeRegisterSummarySSA               <- 扩展
      调用点 DF 定值为常量 0
      ST0 -> 返回槽 x86_fp80，跟 RAX/XMM0 同一套路径
  -> LLVM IR
```

### 8.1 DF 标志：ABI 规定"调用后必须是 0"

i386 的 `repz cmpsb`（字符串比较）循环方向由 `DF` 标志决定。ABI 规定 `DF` 在函数入口、返回、外部调用
返回后都必须为 0，但本地用的 cspec（ABI 描述文件）的"不受影响寄存器"列表里没有 `DF`，通用 clobber
逻辑把它当未知值，循环方向就变未知了。

修复：在代码层建模"调用后 `DF` 定为常量 0"（`isPostCallZeroRegister`）。其他标志（`CF`/`PF`/`AF`/
`ZF`/`SF`/`OF`）是 caller-saved、没有"必须为 0"的约定，不能一样处理。

### 8.2 x87：从整条折叠到 ST0/ST1 窗口建模

x87 指令的语义是 8 个浮点寄存器组成一个"栈"（`ST0`..`ST7`），push 会把寄存器整体往下挪，和普通
寄存器完全不同，直接进 SSA/静态分析很别扭。这里经历了两步：

1. **先整条折叠成 intrinsic 库调用**：按汇编助记符把每条 x87 指令变成 `notdec.x87.*` 库函数调用，
   `ST0`..`ST7` 不建全局，状态藏在库内部。
2. **再收敛成 ST0/ST1 窗口**：发现整条折叠有两个问题——SysV long double 返回（结果留在 `ST0`）只能
   靠"ret 前最后一条指令是 x87"这种启发式猜，不走静态分析，误报/漏报多；`fcomi` 这类指令把比较结果写
   进标志寄存器，不得不引入 `FPUStatusWord` 全局做读改写，和非 x87 的标志语义对不上。收敛方案：
   - `ST0`/`ST1` 建成真正的 LLVM 寄存器全局（80 位），窗口内的操作直接展开成 LLVM 指令；
   - `ST2`..`ST7` 留在库内部，只留 4 个衔接 intrinsic：push / pop / peek / poke；
   - 这样 `ST0` 也走完整的寄存器静态分析：函数尾 `store @ST0` → 出口需求 → 返回槽 `x86_fp80`，
     调用点拿返回值、传参，跟 `RAX`/`XMM0` 走同一套路径。

**配套修正**：

- XMM 参数证据里区分"真浮点写"（`float_write` 标记）和"整数打包"（`movq` 等），避免 vararg 浮点参数
  计数误判（`lua_createtable` 传 7 个 double 的场景曾因为 `XMM0` 里装的是整数而误判）；
- 部分写只擦掉实际写到的 bit 的 demand，不再整寄存器一起擦。

**改完又暴露的新问题**：返回信息归属问题到了必须解决的时候（第 8 代）。

## 9. 第 8 代（2026-08-05，当前）：返回信息唯一来源是签名

和前一代相比，流程上**第一遍不再区分 return/clobber，返回槽由签名决定，返回绑定收集移到重写之后**：

```text
机器码 -> p-code -> PcodeToLLVM
  -> NativeStackFrameRewrite / CanaryCleanup
  -> 第一遍 summary                          <- 变化
      统一生成"调用后状态"占位（不再区分 return / clobber）
      + "调用后 ABI 输出寄存器被读"的证据
  -> 外部参数推断
  -> 第二遍 summary
  -> NativeRegisterSummarySSA                <- 变化
      构建 SSA；重写函数和 call 签名
      重写时 ret 先建 unknown（返回槽由签名决定，不再由第一遍占位决定）
  -> post-rewrite cleanup                    <- 新增/移动
      按最终链值收集返回绑定，填 ret
      删死 store；再清一轮栈帧 / canary
  -> final cleanup
  -> LLVM IR
```

**为什么改（关键问题）**：三个具体 bug，背后是同一个架构问题——第一遍 SSA 构建在调用点区分
return/clobber，而"是不是返回寄存器"本该由签名决定。

1. 外部 `sqrtl`（`long double sqrtl(long double)`）原型推断失败：SysV 下 long double 参数走栈，
   arity 推断只数寄存器前缀 → 得出 0 参数；而且外部浮点返回没有 `RAX` 那种"整型 ABI 输出默认返回"
   的待遇，`XMM0`/`ST0` 的浮点返回全部接不住。
2. `stats_stdev` 返回链断掉：返回绑定遇到"依赖 clobber 占位的分支"时，把整个绑定判死成 unknown，
   连 PHI 里其他确定分支的值一起丢掉。
3. 鸡生蛋：第一遍不知道"是不是返回寄存器"，就没法产生返回占位；没有返回占位，就无法驱动签名补返回槽。

**方案**：

- 第一遍 SSA 构建不再区分 return/clobber：统一生成"调用后状态"占位 + "调用后 ABI 输出寄存器被读"
  的证据；
- "是不是返回寄存器"唯一由签名（`shape.Returns`）决定：内部函数/已知外部来自可信原型或静态分析事实，
  未知外部来自保守推断；
- 未知外部返回槽保守规则：浮点返回（`XMM0` 优先，有 x87 证据才 `ST0`）和整型返回（`RAX`，`RDX`
  排除）互斥，只取一类；旧 call 的非 void 返回类型是最高优先级证据，void call 才退回"占位被实际读取"
  的证据（死值不算）；
- 返回绑定收集移到重写后的清理遍：重写时 `ret` 先建 unknown，清理遍按最终链值填；确定分支保留真实值，
  未知分支保持显式 unknown，不再整体判死。

**效果**：`sqrtl`/`round` 的浮点返回恢复，`stats_stdev` 恢复 `x86_fp80` 返回，wrk 的 warning 净减
104 条，`i64 @sqrtl()` 这类错误返回类型消失。

## 10. 当前全貌（2026-08-06）

把上面各代拼起来，就是当前主 pass 的完整流程。每步标注它来自第几代：

```text
ELF 机器码
  -> Sleigh p-code -> PcodeToLLVM                    （第 0 代）
      部分读写展开成 partial_read / partial_write    （第 5 代）
      x87 按助记符折叠 / 窗口展开                    （第 7 代）
  -> 加载外部原型（内置表 + JSON 覆盖）               （第 6 代）
  -> noreturn 截断（exit、__stack_chk_fail）          （第 4 代）
  -> NativeStackFrameRewrite（RSP/RBP -> alloca）     （第 4 代）
  -> NativeStackCanaryCleanup（删栈保护检查）          （第 4 代）
  -> canonicalizeRegisterPointerPhiLoads              （第 5 代）
  -> 第一遍 NativeRegisterSummary                     （第 2/6/8 代）
      自底向上 effect + 调用点证据，不跑 demand
      调用点统一"调用后状态"占位 + 被读证据
  -> 外部参数推断 inferExternalCallShapes             （第 6 代）
      arity / 整型浮点 / vararg 按调用点
  -> 第二遍 NativeRegisterSummary                     （第 2/6 代）
      带最终签名重跑完整分析（effect + demand + metadata）
  -> 构建初始签名形状                                  （第 2 代）
  -> 逐函数 SummarySSA                                （第 1/3/5/8 代）
      按位区间建 SSA + PHI
      调用点占位 + 证据收集
      重写函数和 call 签名（ret 先建 unknown）
  -> 补外部返回值（按签名，保守规则）                  （第 3/8 代）
  -> post-rewrite cleanup loop                        （第 3/4/8 代）
      收集返回绑定填 ret；删死 store；再清栈帧/canary
  -> NativeRegisterFinalCleanup                       （第 5 代）
      删无用全局 / helper；统计残留
  -> .ll / .bc
```

配套的验证手段：

- `llvm-as` + `opt -passes=verify`：IR 至少合法；
- `native-register-residue-audit.py`：统计还剩多少寄存器 load/store；
- `--register-ssa-warning-out`：残留 helper、未知外部签名、未消除访问的量化清单；
- fortune（i386/x86_64）、wrk、vsftpd、memcached、libuv 等真实目标回归。

## 11. 主线总结

| 代 | 时间 | 和前一代比，流程上多了/改了/删了什么 | 为什么改 | 改完又暴露的问题 |
| --- | --- | --- | --- | --- |
| 0 | 2026-05 前 | 起点：寄存器 = 全局变量 | — | 看不出参数/返回 |
| 1 | 05-29 | 多一个按需 SSA pass（整寄存器粒度） | 寄存器读写没有数据流 | 部分读写消不掉；call 影响没表达；RSP/RBP 特殊 |
| 2 | 06-16~18 | 先静态分析出边界，再建 SSA；换 summary 链为主链 | 参数/返回只能静态分析得到 | 签名重写和 SSA 强耦合 |
| 3 | 06-19 | 删独立签名重写 pass，折进 SummarySSA | 签名重写与 SSA 分不开 | callee-saved 保存恢复被误当参数 |
| 4 | 06-19 起 | 链首加栈帧重写 + canary 清理；summary 识别保存槽 | RSP/RBP/保存恢复不能当普通寄存器 | 部分读写仍消不掉；外部签名靠猜 |
| 5 | 07-03~18 | 部分读写先展开；SSA 变量改成 bit 区间；末尾加 final cleanup | 部分读写进不了整寄存器 SSA | 外部签名猜测证据和 clobber 混在一起 |
| 6 | 07-03~21 | 一遍 summary 拆两遍，中间夹参数推断；证据分三类 | 外部函数没有签名 | 返回信息是鸡生蛋；浮点返回接不住 |
| 7 | 08-04~05 | p-code 层处理 x87；调用点 DF 定 0；ST0/ST1 走完整静态分析 | DF/x87 等特殊寄存器语义 | 返回信息归属问题 |
| 8 | 08-05 | 第一遍不区分 return/clobber；返回槽由签名决定；返回绑定移到重写后 | 返回/clobber 不该在第一遍区分 | （当前形态） |

一句话主线：**每次架构变更，都是旧架构的一个简化假设被真实二进制打破。** "寄存器是全局变量"被 SSA
打破，"整寄存器粒度够用"被部分读写打破，"SSA 自己能猜边界"被参数/返回需求打破，"分析单个函数就够"被
调用图/外部函数打破，"第一遍就能区分返回和 clobber"被浮点返回和鸡生蛋打破。

## 12. 已知边界与押后项

- 未知外部返回推断仍是保守启发式：调用后同时读 `XMM0` 和 `ST0` 的 long double 函数会误选 `XMM0`。
  更准的做法是"后向数据流：调用后被读的寄存器值是否到达本函数 ret"，改动面大，押后。
- 外部 libm 的 long double 原型（`sqrtl`/`round`）、`complex long double` 返回、多 long double
  结构体返回押后。
- 栈参数绑定（调用前压栈的参数恢复成 call 参数）从 2026-07-25 起持续推进，尚未完全收口。
- 旧 heritage 对照链路保留在代码里（`lib/passes/heritage/`），只用于对照，不再演进。
