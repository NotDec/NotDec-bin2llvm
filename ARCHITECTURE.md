# NotDec-bin2llvm Architecture

本文只描述 `external/NotDec-bin2llvm` 当前主要使用的 native 链路：

```text
ELF
  -> native discovery: DDISASM / GTIRB + ELF facts + .eh_frame hints
  -> SLEIGH p-code collection
  -> PcodeToLLVM lowering
  -> LLVM cleanup
  -> native register SummarySSA
  -> LLVM cleanup
  -> final register cleanup
  -> .ll / .bc
```

旧的 Ghidra heritage JSON 链路还在代码里，但 Bench2 当前重点不是它。本文只在必要时提到
legacy 入口，避免和 native 主链路混在一起。

## 1. 入口工具

### `notdec-native-discover`

位置：

- `tools/notdec-native-discover.cpp`
- `include/notdec-bin2llvm/NativeAnalysis.h`
- `lib/NativeAnalysis.cpp`

作用是只做 native 发现和审计，不生成 LLVM IR。常用输出包括：

- `--summary-json`：函数、basic block、instruction、xref、unresolved flow 汇总。
- `--functions-json` / `--function-json`：确认函数及单函数详情。
- `--blocks-json` / `--cfg-json` / `--cfg-dot`：CFG 审计。
- `--instructions-json` / `--block-json`：解码出来的指令。
- `--xrefs-json` / `--callgraph-json` / `--callgraph-dot`：引用和调用关系。
- `--eh-frame-json`：`.eh_frame` / `.eh_frame_hdr` 解码结果。
- `--plt-json`：PLT、GOT slot 和外部符号映射。
- `--unresolved-json`：未解析的 indirect call / indirect branch。

### `notdec-native-llvm`

位置：

- `tools/notdec-native-llvm.cpp`
- `lib/PcodeToLLVM.cpp`

作用是把 native discovery 得到的函数和 CFG 变成 LLVM IR，然后跑 LLVM/native pass。

常用模式：

- `--all-confirmed`：对 discovery 确认的函数整体建 module。
- `-f <entry>`：按函数入口生成一个函数。
- `-n <name>`：按函数名生成一个函数。
- `-a <addr> -l <len>`：手动指定线性地址范围。
- `--skip-runtime`：跳过 `_start`、init/fini、PLT resolver 等 runtime 辅助函数。
- `--no-register-ssa-pass`：只看 p-code lowering 后的原始寄存器 IR。
- `--summary-register-ssa-pass`：默认的新 SummarySSA 链路。
- `--heritage-register-ssa-pass`：旧的 heritage SSA 链路，只用于对照。
- `--external-prototypes <json>`：加载额外外部函数原型。

## 2. 整体流程

当前 native 主链路按下面顺序执行：

1. **读取 ELF**
   - 用 LIEF 解析 ELF。
   - 建立 `LiefElfLoadImage`，提供按地址读取 section / segment 字节的能力。
   - 解析 relocation、dynamic symbol、PLT/GOT、memory permission。

2. **native discovery**
   - 入口：`runNativeDiscovery(...)`。
   - 默认优先用 DDISASM / GTIRB 导入函数、basic block、CFG edge 和指令。
   - 同时补充 ELF entry、dynamic init/fini、init/fini array、relocation code target。
   - 解析 `.eh_frame`，只作为 function boundary / range hint，不把 FDE 单独当源码函数。
   - 折叠 DDISASM 拆出来的 `.eh_frame` cold fragment，避免把 cold tail 当成独立源码函数。
   - 记录 xref、callgraph、unresolved indirect flow。

3. **选择要 lower 的函数**
   - `--all-confirmed`：用 discovery 确认的函数列表。
   - `-f` / `-n`：用 discovery 解析函数 range、block range、successor。
   - 手动 `-a/-l`：不依赖 confirmed function，只按线性范围收 p-code。

4. **规划 call target**
   - 内部 direct call 映射到本 module 里的 LLVM function。
   - PLT call 映射到外部 LLVM declaration。
   - GOT 间接外部 call 尽量映射到外部 LLVM declaration。
   - 未知 indirect call / branch 保守 lower，不猜普通函数指针和 jump table。

5. **收集 SLEIGH p-code**
   - 按 confirmed basic block range 收集 p-code，保留 DDISASM / GTIRB 给出的 CFG 顺序。
   - `CALL` / `RET` 指令自身的隐式栈动作在 lifting 中消除；普通显式栈操作仍保留。
   - x86-64 partial register read/write lower 成 `notdec.partial_read.*` /
     `notdec.partial_write.*` intrinsic，避免先 load full register 再拼位。

6. **p-code lowering 到 LLVM**
   - 入口：`buildPcodeModule(...)` / `PcodeToLLVM.cpp`。
   - 生成 register global、RAM load/store、basic block、branch、call。
   - direct call 尽量变成 LLVM direct call。
   - indirect branch 如果 DDISASM / GTIRB 已给出 CFG successor，按已有 CFG lower 成 switch/branch；
     否则保留 unresolved/tail-call 风格的保守 IR。
   - 给 module 附加 memory map metadata 和 ABI metadata。

7. **第一次 LLVM cleanup**
   - 入口：`runInstCombinePassIfEnabled(...)`。
   - 每个非 declaration 函数跑：
     - `InstCombine`
     - `SimplifyCFG`
   - 目的是先折叠 p-code lowering 产生的局部冗余，给 SummarySSA 更干净的输入。

8. **register SummarySSA**
   - 默认入口：`runNativeRegisterSummarySSA(...)`。
   - 这是当前寄存器消除和函数签名重写的主 pass，详细顺序见下一节。

9. **第二次 LLVM cleanup**
   - 再跑一次 `InstCombine + SimplifyCFG`。
   - 目的是折叠 SummarySSA/signature rewrite 后暴露出来的局部 IR。

10. **prototype recovery**
    - 当前只在旧 `--heritage-register-ssa-pass` 路径启用。
    - 默认 SummarySSA 路径不跑 `NativePrototypeRecovery`。

11. **final register cleanup**
    - 入口：`runNativeRegisterFinalCleanup(...)`。
    - 跑 GlobalDCE。
    - 删除死 register read、未使用 register global、未使用 helper declaration。
    - 对已经没有 register residue 的函数，清理 `notdec.register.summary*` 和
      `notdec.register.summary_ssa*` metadata。
    - 再跑一次 GlobalDCE，并统计剩余寄存器访问。

12. **验证和输出**
    - 每个关键阶段后用 `llvm::verifyModule(...)`。
    - 最后写出目标路径，通常是 `.ll`，也可以按工具支持写 `.bc`。

## 3. Native Discovery

核心状态是 `NativeProgramState`。它保存：

- `NativeFunctionSeed`：候选函数入口，带 source 和 confidence。
- `NativeFunction`：已确认函数，包含入口、名字、range、block。
- `NativeBasicBlock`：basic block 起止地址和 successor。
- `NativeInstruction`：地址、长度、字节、反汇编文本、来源。
- `NativeXref`：flow / call / data / string 引用。
- `NativeUnresolvedFlow`：未解析的 indirect call / branch。
- `NativePltEntry`：PLT stub、GOT slot、外部符号名。

当前 discovery 的主要来源：

1. **DDISASM / GTIRB**
   - 导入 DDISASM 识别的函数、块和 CFG edge。
   - 这是 Bench2 native 链路的主要 CFG 来源。
   - 对 jump table，当前原则是使用 DDISASM / GTIRB 已经确认的 successor，不在 p-code
     lowering 里重新猜跳表。

2. **ELF facts**
   - ELF entry。
   - dynamic init/fini。
   - init/fini array。
   - relocation 指向 executable memory 的 code target。
   - PLT/GOT 外部符号映射。

3. **`.eh_frame`**
   - 解码 FDE range。
   - 只作为函数 range / boundary hint。
   - 不把 `.eh_frame` FDE range 单独当成源码函数。
   - 如果 DDISASM 把 cold fragment 拆成独立函数，native 侧会尝试 fold 回 owner。

4. **SLEIGH fallback decode**
   - 作为受控补充。
   - 用于按 seed 或 range 解码 instruction 和 p-code。
   - 不追普通未知 indirect flow。

## 4. P-code Lifting

### 输入

`PcodeToLLVM` 的输入是：

- load image：按地址读 ELF bytes。
- SLEIGH spec / pspec / cspec。
- function block range。
- block successor。
- direct call target map。
- external call target map。
- indirect external call target map。
- ABI metadata。

### 输出 IR 形状

初始 IR 仍然显式暴露机器状态：

- register 是 LLVM global，例如 `@RAX`、`@RDI`、`@ZMM0`。
- RAM 访问用 `inttoptr` 或 global-array memory model。
- p-code unique varnode 变成局部 SSA value。
- partial register access 用 intrinsic 表示：
  - `notdec.partial_read.<full>.<part>(ptr @REG, offset)`
  - `notdec.partial_write.<full>.<part>(ptr @REG, value, offset)`
- p-code basic block 映射为 LLVM basic block，名字通常是 `bb_<address>`。

### Call / Return

当前 x86-64 lowering 的约定：

- LLVM `call` 自己已经表达“调用后会返回到下一条 LLVM 指令”。
- 因此 x86 `CALL` 指令自身 push return address 的隐式栈动作不保留。
- x86 `RET` 指令自身 pop return address 的隐式栈动作不保留。
- 函数体里普通 `push/pop/sub/add rsp` 等显式栈操作仍保留，后续由 stack frame pass 恢复。
- 如果某个 CALL/RET 的隐式栈模式没有被识别，应该发 warning，而不是悄悄生成假栈语义。

## 5. LLVM Pass Pipeline

`notdec-native-llvm` 当前主流程：

```text
PcodeToLLVM
  -> attach memory map metadata
  -> attach ABI metadata
  -> verify
  -> InstCombine + SimplifyCFG
  -> NativeRegisterSummarySSA
  -> InstCombine + SimplifyCFG
  -> optional legacy NativePrototypeRecovery
  -> NativeRegisterFinalCleanup
  -> verify
  -> write .ll / .bc
```

### 5.1 InstCombine + SimplifyCFG

位置：

- `tools/notdec-native-llvm.cpp::runInstCombinePassIfEnabled`

每次运行都只对非 declaration 函数跑：

1. `InstCombine`
2. `SimplifyCFG`

这一步只做通用 LLVM 局部化简，不负责寄存器语义。

### 5.2 NativeRegisterSummarySSA

位置：

- `include/notdec-bin2llvm/passes/summary/NativeRegisterSummarySSA.h`
- `lib/passes/summary/NativeRegisterSummarySSA.cpp`

这是当前 native register elimination 的主 pass。顶层顺序是：

1. **加载外部原型**
   - 从内置 `defaultNativeExternalPrototypes()` 开始。
   - 如果传了 `--external-prototypes <json>`，再加载 JSON 覆盖/扩展。
   - 原型用于外部函数 signature rewrite、noreturn 截断和 warning。

2. **截断已知 noreturn 外部 call**
   - 对 `exit`、`__stack_chk_fail` 等已知 noreturn 外部函数，把后继不可达部分截断。

3. **NativeStackFrameRewrite**
   - 入口：`runNativeStackFrameRewrite(...)`。
   - 从 ABI metadata 读取 stack pointer register。
   - 把 RSP/RBP 相关栈访问恢复为 `notdec_stack.native` alloca / GEP 形式。
   - 把 stack pointer 和已确认 frame pointer 加入 ignored register 集合。

4. **NativeStackCanaryCleanup**
   - 入口：`runNativeStackCanaryCleanup(...)`。
   - 匹配 FS canary load、保存、结尾比较、`__stack_chk_fail` 分支。
   - 删除 canary check 和 fail-only block。

5. **canonicalizeRegisterPointerPhiLoads**
   - 修 InstCombine 可能生成的 register pointer PHI load。
   - 让后续 register summary 能继续按 register global 识别访问。

6. **NativeRegisterSummary**
   - 入口：`runNativeRegisterSummary(...)`。
   - 做 bottom-up 函数 effect 分析：
     - `ReadEntry`：函数入口寄存器值是否被读。
     - `MayEntry`：正常返回时寄存器是否可能仍是入口值。
     - `MayNonEntry`：正常返回时寄存器是否可能是函数内新值。
   - 做 top-down demand 分析：
     - `EntryDemandMask`：入口寄存器哪些 bit 被真实观察。
     - `ExitDemandMask`：返回寄存器哪些 bit 被 caller 需要。
   - 处理 call：
     - 内部 call 用 callee summary effect。
     - 外部 call 当前 fallback 使用 ABI effect；这里后续需要按外部原型收窄输入寄存器。
   - 处理 partial write：
     - x86-64 低 32 位 GPR 写视为整寄存器定义，因为会清高 32 位。
     - 其他 partial write 保留未写 lane 的 entry 可能性。
   - 处理保存/恢复：
     - 跟踪固定 entry-SP offset / native stack alloca slot。
     - 识别 callee-saved register 保存到栈再恢复的模式。

7. **构建初始 signature shape**
   - 外部 declaration：
     - 优先用已知外部原型。
     - 未知外部函数先按 ABI 输入寄存器给保守形状，后面再用 callsite 收窄。
   - 内部函数：
     - 根据 `ReadEntry` 和 ABI register class 决定参数。
     - 根据 `ExitDemand` 和 ABI return register 决定返回值。
     - 用 demand mask 收窄整数宽度和 float/double lane。

8. **FunctionBuilder per-function SummarySSA**
   - 对每个非 declaration 函数运行 `FunctionBuilder::run()`。
   - 主要细分步骤：
     - 收集 register load/store/partial read/partial write 事件。
     - 规划 register range。现在是 range-aware SummarySSA，不再只有整寄存器粒度。
     - 为入口值创建 canonical entry read 或 range entry read。
     - 按 CFG 构造 SSA value，必要时插入 PHI。
     - 用 call effect 生成 return/clobber value。
     - 用 zero-demand metadata 标记被 demand 剪掉的 lane。
     - 收集函数返回值。
     - 收集 callsite argument store binding。

9. **未知外部函数参数数量推断**
   - 入口：`refineUnknownExternalParamShapes(...)`。
   - 综合多个 callsite 的连续参数 binding。
   - 遇到 clobber-derived value 不当作强证据。
   - 不一致时输出 warning，同时仍选一个可用 arity。

10. **间接 call 参数形状收窄**
    - 入口：`refineIndirectCallsiteParamShapes(...)`。
    - 对 indirect call 使用 callsite binding 收窄参数数量。

11. **补外部返回值**
    - 入口：`addDemandedExternalReturns(...)`。
    - 如果 call 后有 demanded `summary_return` / range return helper，给外部 declaration
      增加对应 ABI return slot。
    - 对 RDX 这类非主返回寄存器保持保守，避免误判为第二返回值。

12. **标记 call 参数 store 可删除**
    - 入口：`markSignatureCallArgStores(...)`。
    - 被成功重写进 LLVM call 参数的 register store 可以删除。

13. **重写函数和 call 签名**
    - 入口：`rewriteSignatureShapes(...)`。
    - 替换函数类型。
    - 内部函数 body 中 entry register read 替换为 LLVM argument。
    - callsite 从“先 store ABI register，再 call void”重写为普通 LLVM call 参数。
    - call return helper 替换为普通 LLVM call return value。

14. **删除未使用 helper declaration**
    - 清理 `notdec.register.summary_*`、partial read/write helper 的无用声明。

15. **post-rewrite cleanup loop**
    - 只在 SummarySSA residue removal 开启时运行。
    - 最多 10 轮。
    - 每轮可选跑 `InstCombine`。
    - 然后 `removeDeadStoresAfterSignatureRewrite()` 删除签名重写后暴露的死 register store。
    - 如果本轮没有新删 store，停止。

16. **NativeStackFrameCleanup**
    - 只在 SummarySSA residue removal 开启时运行。
    - 入口：`runNativeStackFrameCleanup(...)`。
    - 继续删除 stack/frame pointer bookkeeping。
    - 删除不用的 native stack alloca load/store/alloca。

17. **late canary cleanup**
    - 只在 SummarySSA residue removal 开启时运行。
    - 再跑一次 `runNativeStackCanaryCleanup(...)`。
    - 处理前面重写后新暴露出来的 canary 形状。

18. **SummarySSA residue cleanup**
    - 只在 SummarySSA residue removal 开启时运行。
    - 删除死 `summary_return` / `summary_clobber` helper。
    - 删除死 canonical entry read / range entry read。
    - 删除未使用 helper declaration。
    - 收集剩余 helper warning，写到 `--register-ssa-warning-out`。

### 5.3 NativeRegisterFinalCleanup

位置：

- `include/notdec-bin2llvm/passes/summary/NativeRegisterFinalCleanup.h`
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp`

顺序：

1. 跑 `GlobalDCE`。
2. 删除无 use 的 register load 和 partial read。
3. 删除无 use 的 register global。
4. 删除无 use 的 register helper declaration。
5. 扫描每个函数：
   - 如果还有 register load/store/helper，保留 debug metadata。
   - 如果没有 register residue，删除函数和指令上的 Summary/SummarySSA metadata。
6. 再跑一次 `GlobalDCE`。
7. 再删一次无用 register global/helper。
8. 统计 `remaining_register_accesses`。

## 6. 关键文件

```text
external/NotDec-bin2llvm/
  tools/
    notdec-native-discover.cpp      native discovery 审计入口
    notdec-native-llvm.cpp          native -> p-code -> LLVM -> pass 主入口

  include/notdec-bin2llvm/
    NativeAnalysis.h                NativeProgramState 和 discovery 数据结构
    Pcode.h                         p-code 数据结构
    PcodeToLLVM.h                   p-code lowering 对外入口
    NativeAbi.h                     ABI metadata 读写
    NativeExternalPrototype.h       外部函数原型表
    NativeRegisterPartialRead.h     partial read intrinsic 解析/生成
    NativeRegisterPartialWrite.h    partial write intrinsic 解析/生成

  lib/
    NativeAnalysis.cpp              ELF/DDISASM/GTIRB/.eh_frame/PLT/GOT discovery
    GtirbNativeAnalysis.cpp         GTIRB 导入
    NativeEhFrame.cpp               .eh_frame 解码
    NativeRuntime.cpp               runtime 函数识别和 skip-runtime 策略
    NativeExternalPrototype.cpp     默认外部原型和 JSON 加载
    SleighLift.cpp                  SLEIGH instruction / p-code 收集
    PcodeToLLVM.cpp                 p-code 到 LLVM IR lowering

  lib/passes/summary/
    NativeStackFrame.cpp            栈帧恢复和 cleanup
    NativeStackCanaryCleanup.cpp    stack canary 删除
    NativeRegisterSummary.cpp       bottom-up / top-down register summary
    NativeRegisterSummarySSA.cpp    range-aware SummarySSA 和签名重写
    NativeRegisterFinalCleanup.cpp  final GlobalDCE / metadata cleanup
```

## 7. 现在已知的设计边界

- DDISASM / GTIRB 是当前 CFG 主来源。p-code lowering 不主动猜 jump table。
- `.eh_frame` 是 range / boundary hint，不是源码函数来源。
- 外部函数原型会影响 signature rewrite；但 bottom-up register summary 目前还没有完整按原型收窄
  external call input，这是接下来要修的点。
- `main` 在 executable 里会保留 external linkage，其他 lifted 函数默认 internal linkage。
- register metadata 是 debug 辅助，不是 cleanup 后的新事实。final cleanup 只在函数没有剩余
  register residue 时清掉它。
- `--no-register-ssa-pass` 是排查 lifting 原始语义的关键开关。
- `--register-ssa-warning-out` 用来审计残留 summary helper、未知外部签名和未消除寄存器访问。
