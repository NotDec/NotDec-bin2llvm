# NotDec-bin2llvm 代码架构

本文只描述 `external/NotDec-bin2llvm` 当前代码。它现在最重要的链路是：

1. Ghidra 脚本把二进制里的函数导出成 heritage P-Code JSON。
2. native 工具读取 JSON。
3. `HeritageToLLVM.cpp` 把函数和模块 lowered 成 LLVM IR。

Sleigh 相关代码仍在，但默认不开启，当前 Bench2 主要不走那条路径。

## native discovery 状态骨架

native 路线的共享分析状态在 `include/notdec-bin2llvm/NativeAnalysis.h` 和
`lib/NativeAnalysis.cpp`。当前它先复刻 Ghidra Program 里最需要的几类事实，但不做
Ghidra 那种完整数据库：

1. `NativeFunctionSeed`：候选函数入口，来自 ELF entry、dynamic init/fini、
   static symbol、dynamic symbol、PLT、`.eh_frame`。
2. `NativeFunctionWorkItem`：待递归 decode 的函数入口。新 seed 首次插入时进入这个队列。
3. `NativeFunction`：已经确认的函数。它和 seed 分开，避免还没 decode 就把候选入口当
   成真实函数。
4. `NativeBasicBlock`：函数内的最小 block 范围和 successor 地址。
5. `NativeXref`：统一记录 flow/call/data/string 四类引用，支持 from/to 查询。
6. `NativeInstruction`：已接受的指令，保留地址、长度、字节和来源。

`NativeProgramState` 现在提供：

- `functionAt(...)`、`functionContaining(...)`
- `functionWorklist()`
- `xrefsFrom(...)`、`xrefsTo(...)`
- `instructionAt(...)`、`instructionsInRange(...)`
- `addFunction(...)`、`addBasicBlock(...)`、`addXref(...)`、`addInstruction(...)`

`notdec-native-discover` 默认输出文本 report，包含 function seed、worklist、confirmed
function、basic block、instruction、xref 和 unresolved indirect flow 数量。加
`--summary-json` 时会跳过文本 report，输出同口径的最小 JSON summary，供 Bench2 smoke
做机器检查。`--functions-json` 输出已确认函数列表，包含入口、保守范围、名字、来源和
block 数。`--blocks-json` 输出已确认函数里的 basic block 起止地址、大小和 successor。
`--xrefs-json` 输出当前 direct control-flow xref 列表，`--xrefs-from-json <addr>` 和
`--xrefs-to-json <addr>` 按地址查引用。`--instructions-json` 输出已接受指令的地址、
大小、字节、显示文本和来源。

当前 recursive decode 只接入了一个很小的 Sleigh 线性指令解码入口：

- `lib/SleighLift.cpp::collectSleighInstructionSummaries(...)` 用
  `Sleigh::printAssembly(...)` 解码指令地址、长度和显示文本。
- `lib/NativeAnalysis.cpp::SleighSeedInstructionAnalyzer` 先从 function worklist 取前 8 个
  seed 入本地队列。队列元素区分 function entry 和 block address，每个 block 最多解码
  8 条 / 64 字节，并写入 `NativeInstruction`。
- 如果某个 seed 成功解码出指令，它会被保守写成一个 `NativeFunction`，并带一个覆盖已解码
  指令前缀的 `NativeBasicBlock`。
- `lib/SleighLift.cpp::collectSleighInstructionDecode(...)` 在同一次 Sleigh 初始化里收集指令摘要
  和对应 P-Code，避免为了 xref 再解码一次。
- analyzer 会从 P-Code 里识别直接 `CALL`、`BRANCH`、`CBRANCH`。第一个输入是 `ram` 地址时，
  写入 `NativeXref`；直接 branch 目标也会写入当前 block 的 `Successors`。
- analyzer 也会从非控制流 P-Code 里识别 direct `ram` varnode。如果目标不是 executable
  address，会记录为 data 或 string xref。目标在可读、不可写、不可执行内存里，并且像
  NUL 结尾 ASCII C 字符串时，记为 `NativeXrefKind::String`，来源是
  `sleigh-pcode-direct-string`；否则仍记为 `NativeXrefKind::Data`，来源是
  `sleigh-pcode-direct-data`。
- `RelocationPltAnalyzer` 会把已应用的本地 relocated pointer 也写成 xref：relocation
  slot 是 from，computed pointer 是 to。目标像只读 C 字符串时记 string，来源
  `elf-relocation-string`；其他落在已加载内存里的目标记 data，来源
  `elf-relocation-pointer`。
- 当前 block 不是单纯整段线性范围：`SleighSeedInstructionAnalyzer` 会按控制流指令切分已解码
  前缀。`CBRANCH` block 同时记录直接目标和下一条指令 fallthrough，`BRANCH` block 只记录直接
  目标，`BRANCHIND` / `RETURN` block 暂不记录 successor。
- `CALLIND` / `BRANCHIND` 会写入 `NativeUnresolvedFlow`，report 按 indirect call / indirect
  branch 统计。这里不猜目标，只保留后续跳表和函数指针分析需要的样本。
- direct `CALL` 的可执行目标会作为 `sleigh-direct-call` function seed 写入，并进入同一个本地
  decode 队列。本轮总 decode 上限是 16 个 seed，已入队或已 decode 的地址不会重复处理。
- direct `BRANCH` / `CBRANCH` 的可执行 successor 会作为同一个 function entry 下的 block
  address 入队。追加 block 时，`NativeFunction` 的 decoded range 会随 block 扩展。
- 如果 direct branch successor 是已知的其他 function seed 入口，它会保留 direct flow xref，
  但不会作为当前函数 block successor 或本地 decode block 入队。
- 这一步只用于受控消费 direct call seed 和 direct branch successor。间接 branch/call 目前只记录，
  不跟随；函数边界也只处理已知其他函数入口。

因此 Bench2 运行时 instruction、confirmed function、basic block、xref 数量应该已经大于 0。

## 目录

```text
external/NotDec-bin2llvm/
  ghidra_scripts/   Ghidra headless 导出脚本
  include/          对外头文件和主要数据结构
  lib/              JSON 读取、P-Code 表达、LLVM IR lowering
  tools/            命令行工具
  cmake/            LLVM 查找逻辑
```

几个文件的职责：

- `ghidra_scripts/ExportHeritageModule.java`：模块级 JSON 导出入口。遍历 Ghidra 函数，逐个 decompile，成功的写入 `functions[]`，失败的写入 `failures[]`，外部函数写入 `externals[]`。
- `ghidra_scripts/ExportHeritagePcode.java`：单函数 JSON 导出入口。现在主要用于小样例和定位单个函数问题。
- `include/notdec-bin2llvm/HeritagePcode.h`：heritage JSON 在 C++ 侧的数据结构。
- `lib/HeritagePcode.cpp`：读取单函数和模块级 heritage JSON，并建立 id 到对象的索引。
- `include/notdec-bin2llvm/HeritageToLLVM.h`：heritage lowering 的对外入口。
- `lib/HeritageToLLVM.cpp`：当前最核心的 lowering 实现。
- `tools/notdec-heritage-module-llvm.cpp`：模块级 JSON 到 `.ll` 的命令行入口。
- `tools/notdec-heritage-module-check.cpp`：模块级 JSON 引用关系检查工具。
- `tools/notdec-heritage-llvm.cpp`：单函数 JSON 到 `.ll` 的命令行入口。
- `tools/notdec-heritage-check.cpp`：单函数 JSON 检查工具。
- `tools/notdec-native-discover.cpp`：native discovery smoke 入口。默认打印文本 report，
  `--summary-json` 打印 function、block、instruction、xref 和 unresolved indirect flow
  的汇总 JSON，`--functions-json` 打印 confirmed function 列表 JSON，`--blocks-json`
  打印 confirmed function 下的 basic block 列表 JSON，`--xrefs-json` 打印 xref 列表
  JSON，`--xrefs-from-json` / `--xrefs-to-json` 按地址查 xref，`--instructions-json`
  打印 instruction 列表 JSON。
- `tools/notdec-native-llvm.cpp`：native P-Code 到 LLVM IR 的入口。可以继续用
  `-a <address> -l <length>` 手工指定范围，也可以用 `-f <entry>`、`-n <name>` 或
  `--all-confirmed` 先跑 native discovery，从 confirmed function 取入口到保守 range end，
  再生成 `.ll`。`--all-confirmed` 会跳过当前 lowering 还不能通过 verifier 的函数。
- `include/notdec-bin2llvm/Pcode.h`、`lib/PcodeToLLVM.cpp`、`tools/SleighBytes.cpp`：Sleigh 字节到 P-Code、再到 LLVM IR 的旧实验路径。默认 `NOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF`。
- `include/notdec-bin2llvm/ModuleBuilder.h`、`lib/ModuleBuilder.cpp`、`tools/notdec-bin2llvm.cpp`：最早的 demo module 入口，只生成一个空函数。

## 当前主执行顺序：模块级 heritage JSON 到 LLVM IR

常用命令：

```bash
notdec-heritage-module-llvm /tmp/module.json -o /tmp/module.ll
```

大致顺序如下。

### 1. Ghidra 导出模块 JSON

入口：

- `ExportHeritageModule.run()`

关键步骤：

1. `parseOptions(...)` 读取输出路径、函数数量限制、decompile timeout 和 simplification style。
2. `createDecompiler(...)` 创建 Ghidra `DecompInterface`，关闭 C 输出，保留 syntax tree，并设置 simplification style。
3. `selectFunctions(...)` 遍历 `currentProgram.getFunctionManager().getFunctions(true)`，跳过 external 和 thunk，按 `--limit` 或 `--all` 选择内部函数。
4. 对每个函数调用 `decompile(...)`，拿到 `HighFunction`。
5. `writeFunctionObject(...)` 写一个函数对象，内容包括函数签名、basic block、op、varnode。
6. `writeExternals(...)` 写模块级外部函数表。
7. `writeFailures(...)` 写 decompile 失败的函数。

输出 schema 是 `notdec.heritage-module.v0`，核心字段是：

- `program`：程序名、语言、compiler spec、simplification style。
- `functions[]`：内部函数，每个元素复用单函数 heritage 字段。
- `externals[]`：外部函数声明。
- `failures[]`：Ghidra 导出阶段失败的函数。

### 2. native 工具解析参数

入口：

- `tools/notdec-heritage-module-llvm.cpp::main(...)`

关键函数：

- `parseArgs(...)`：接受 `<heritage-module.json> -o <output.ll>`，可选 `--declarations-only`。
- `printUsage(...)`：参数错误时打印用法。
- `writeModule(...)`：最后把 LLVM module 写到 `.ll`。

`main(...)` 的顺序：

1. 解析 CLI。
2. 调用 `loadHeritageModuleFromJson(...)` 读取 JSON。
3. 创建 `llvm::LLVMContext`。
4. 如果是 `--declarations-only`，调用 `buildHeritageDeclarationModule(...)`。
5. 否则调用 `buildHeritageModuleWithBodies(...)`。
6. 调用 `llvm::verifyModule(...)`。
7. 写出 `.ll`。

### 3. 读取 heritage module JSON

入口：

- `lib/HeritagePcode.cpp::loadHeritageModuleFromJson(...)`

关键函数：

- `requireString(...)`：读取必填字符串。
- `readProgramInfo(...)`：读取 `program`。
- `readModuleFunction(...)`：读取 `functions[]` 里的一个函数。
- `readFunctionObject(...)`：读取函数名、入口地址、返回类型和参数。
- `readBlocks(...)`：读取 basic block 和 CFG 边。
- `readOps(...)`：读取 P-Code op、输入输出、call target。
- `readVarnodes(...)`：读取 varnode 的 space、offset、size、寄存器信息和 high variable 信息。
- `readExternalFunction(...)`：读取 `externals[]`。
- `readFailure(...)`：读取 `failures[]`。
- `indexHeritageProgram(...)`：给每个函数建立 `BlockById`、`OpById`、`VarnodeById`、`BlockByStart`。

这里的数据结构尽量贴近 JSON。当前没有再造一套中间 IR，主要是为了让 Ghidra 导出结果和 native lowering 一一对上。

### 4. 规划模块符号名

入口：

- `lib/HeritageToLLVM.cpp::buildHeritageModuleWithBodies(...)`

第一步会调用：

- `planModuleSymbols(...)`

它做三件事：

1. 给内部函数生成 LLVM symbol。
2. 给外部函数生成 LLVM symbol。
3. 建立 `NameByEntry` 和 `NameByOriginalName`，后面 `CALL` lowering 用它解析目标函数。

相关辅助函数：

- `sanitizeSymbolName(...)`：把 Ghidra 名字转成 LLVM 里更安全的 symbol。
- `addressSuffix(...)`：从地址里提取后缀。
- `uniqueSymbolName(...)`：处理重名，必要时拼地址和序号。

### 5. 先声明所有函数

`buildHeritageModuleWithBodies(...)` 先创建一个空 module，然后分两类声明函数：

1. 内部函数：调用 `declareInternalFunction(...)`。
2. 外部函数：调用 `module->getOrInsertFunction(...)`，当前使用 vararg function type。

这样做是为了让函数 body lowering 时可以解析内部互调，也让单个函数失败时还能保留 declaration。

函数类型相关逻辑：

- `typeForSourceType(...)`：把 Ghidra 类型字符串粗略映射到 LLVM integer/void 类型。
- `functionTypeForHeritageFunction(...)`：根据 `HeritageFunction.Params` 和 `ReturnType` 生成内部函数类型。
- `varargFunctionType(...)`：给外部函数和不稳定 call prototype 用。

### 6. 逐个 lower 函数体

入口：

- `HeritageLowerer::lower(...)`

顺序：

1. `createFunction(...)`：确认或创建当前 LLVM function。
2. `createBlocks()`：按 heritage block 建 LLVM basic block。
3. `mapParameters(...)`：把函数参数绑定到对应 varnode。
4. 遍历 `Program.Blocks`，对每个 block 调 `lowerBlock(...)`。
5. `finalizePendingPhis(...)`：补完所有 `MULTIEQUAL` 的 incoming value。

`HeritageLowerer` 里几个核心状态：

- `BlockMap`：heritage block id 到 LLVM basic block。
- `Values`：varnode id 到当前 LLVM value。
- `PendingPhis`：先创建但还没填 incoming 的 PHI。
- `Symbols`：模块级函数符号表，用于解析 `CALL`。
- `Memory`：临时 `notdec_ram` 外部数组，用于表达 LOAD/STORE。

### 7. lower 一个 basic block

入口：

- `HeritageLowerer::lowerBlock(...)`

它的顺序和普通 SSA lowering 一样：

1. 第一遍只处理本 block 里的 `MULTIEQUAL`，调用 `lowerPhi(...)` 创建 LLVM PHI。
2. 第二遍跳过 `MULTIEQUAL`，按 op 顺序 lower 普通指令。
3. 如果遇到 `BRANCH`、`CBRANCH`、`BRANCHIND`、`RETURN`，调用 `lowerBranch(...)` 并结束这个 block。
4. 如果没有显式 terminator，就按 `block.Out` 生成 fallthrough branch；没有 successor 时生成 return。

这也是 `scripts/bin2llvm-dump-module-pcode.py` 当前按 lowering 顺序打印 P-Code 的依据。

### 8. lower 普通 P-Code op

入口：

- `HeritageLowerer::lowerOp(...)`

它按 mnemonic 分发到具体函数：

- 数据移动：`lowerCopy(...)`、`lowerCopyLike(...)`
- 整数运算：`lowerBinary(...)`、`lowerUnary(...)`
- 整数比较：`lowerCompare(...)`
- 溢出判断：`lowerOverflow(...)`
- 位计数：`lowerCountBits(...)`
- 扩展/截断：`lowerCast(...)`
- 布尔运算：`lowerBoolNegate(...)`、`lowerBoolBinary(...)`
- 浮点运算：`lowerFloatBinary(...)`、`lowerFloatCompare(...)`、`lowerFloatUnary(...)`、`lowerFloatNan(...)`、`lowerFloatCast(...)`
- 拼接/切片：`lowerPiece(...)`、`lowerSubpiece(...)`
- 指针加减：`lowerPtrAdd(...)`、`lowerPtrSub(...)`
- bit range：`lowerInsert(...)`、`lowerExtract(...)`
- 内存访问：`lowerLoad(...)`、`lowerStore(...)`
- PHI：`lowerPhi(...)`
- 调用：`lowerCall(...)`
- 暂不精确支持的 op：`lowerHelperCall(...)`

读写 varnode 的基础函数：

- `read(...)`：优先从 `Values` 取值；常量 varnode 生成 `ConstantInt`；未初始化 varnode 目前生成 `freeze poison` 并打印 warning。
- `write(...)`：按 varnode size resize 后写回 `Values`。
- `resize(...)`：用 zero extend 或 truncate 调整 integer bit width。

### 9. 内存、调用和 PHI 的当前处理

内存：

- `memoryGlobal()` 创建外部全局数组 `@notdec_ram`。
- `memoryPointer(...)` 用地址对 `@notdec_ram` 做 GEP。
- `lowerLoad(...)` 和 `lowerStore(...)` 只接受常量 address-space selector，按 1 字节对齐生成 load/store。

调用：

- `lowerCall(...)` 先用 `resolveCallTargetName(...)` 查模块符号表。
- 当前 call prototype 还不稳定，所以用 vararg declaration。
- 如果是 `CALLIND`、`CALLOTHER`、`SEGMENTOP` 等暂不精确支持的 op，走 `lowerHelperCall(...)`，生成 `notdec_heritage_<opcode>_*` helper call。

PHI：

- `lowerPhi(...)` 只创建 PHI，并把 predecessor block id 和 input varnode id 存到 `PendingPhis`。
- `finalizePendingPhis(...)` 在所有 block lower 完之后补 incoming。
- `resizeForPhiIncoming(...)` 会尽量在 predecessor terminator 前插入 zext/trunc。
- 找不到 incoming value 时当前使用 poison fallback，并打印 warning。

寄存器：

- register space 的全局变量由 `RegisterStorage` 统一管理。
- 模块级 heritage lowering 先收集整个模块里的 register varnode，再给所有函数共享同一份
  `RegisterStorage`。同一个寄存器不能因为出现在不同函数里就生成 `@R13`、`@R13.1`
  这种多份状态。
- 寄存器内存段拆分按“不会被重叠访问跨过的边界”来切。也就是不能存在一个访问范围把切分边界包在内部。
  例如 x86-64 的 `RAX/EAX/AX/AL` 都落在 `RAX` 这个最大承载寄存器里，LLVM IR
  只建一个 `@RAX` global；更小的访问用 shift/trunc/mask 访问它的一部分。
- `EAX` 写入会清空 `RAX` 高位这类语义由 P-Code 表达，这里不要用额外规则重复模拟。
- register varnode 的写入默认只进入函数内 SSA `Values`，不再同步写回 `@RAX/@RDX`
  这类 module-global register。只有读一个没有 SSA 值的 register input 时，才从
  `RegisterStorage` 兜底读全局寄存器。

栈：

- heritage 输出里普通 prologue/epilogue 的 `RSP` 加减通常已经被 Ghidra 抽成
  `Stack[-offset]` 这类 frame-relative varnode。
- `HeritageLowerer` 会扫描每个函数的负偏移 stack varnode，按覆盖范围在函数入口生成一个
  byte-addressed `alloca`。例如最低访问到 `Stack[-0x38]` 时，栈帧至少覆盖 0x38 字节。
- `Stack[-offset]` 的读写直接落到这个 alloca 的 GEP 上，不再通过 `@RSP + offset`
  访问临时内存模型。
- 正偏移 stack varnode 先不放进这个 alloca，避免把调用者栈参数或返回地址混成本函数本地栈。

### 10. 函数失败时恢复成 declaration

`buildHeritageModuleWithBodies(...)` 对每个 `status == "ok"` 的函数尝试填 body。

如果 `HeritageLowerer::lower(...)` 失败，或 `llvm::verifyFunction(...)` 失败，会调用局部 lambda `restoreDeclaration(...)`：

1. 删除当前半成品 LLVM function。
2. 用同一个 symbol 重新声明函数。
3. 把失败信息写入 `HeritageModuleLoweringStats::Failures`。
4. 继续处理后面的函数。

这保证一个坏函数不会导致整个 module 没有输出。

## 单函数 heritage 路径

常用命令：

```bash
notdec-heritage-llvm /tmp/function.json -o /tmp/function.ll
```

顺序：

1. `tools/notdec-heritage-llvm.cpp::main(...)` 解析参数。
2. `loadHeritageProgramFromJson(...)` 读取单函数 JSON。
3. `buildHeritageModule(...)` 创建 module。
4. `HeritageLowerer::lower(...)` lower 一个函数体。
5. `llvm::verifyModule(...)` 检查。
6. 写出 `.ll`。

这条路径和模块级路径共用 `HeritageProgram`、`HeritageLowerer` 和大部分 lowering 逻辑。

## 检查工具

### `notdec-heritage-module-check`

入口：

- `tools/notdec-heritage-module-check.cpp::main(...)`

主要检查：

- schema 是否是 `notdec.heritage-module.v0`。
- 内部函数 name 和 entry 是否重复。
- block 的 `in/out/ops` 引用是否存在。
- op 的 parent、input、output varnode 是否存在。
- direct call 能否解析到内部函数或外部函数。

关键函数：

- `checkModuleSymbols(...)`
- `checkFunctionRefs(...)`
- `countCalls(...)`
- `printSummary(...)`

### `notdec-heritage-check`

入口：

- `tools/notdec-heritage-check.cpp::main(...)`

主要检查单函数 JSON：

- schema 是否是 `notdec.heritage-pcode.v0`。
- 参数 varnode 是否存在。
- block/op/varnode 引用是否完整。
- `MULTIEQUAL` 输入数量是否等于 predecessor 数量。
- 统计 register varnode 和 direct call。

## Sleigh 实验路径

默认构建时 `NOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF`，所以这条路径通常不会进 Bench2 当前验证。

Sleigh 依赖会从 `lifting-bits/sleigh` 拉取，Ghidra 源码统一使用
`NOTDEC_BIN2LLVM_GHIDRA_SOURCE_DIR` 指向的 checkout。当前本机默认是
`/sn640/ghidra`，需要 checkout 到 sleigh pin 的 Ghidra tag。不要混用旧
Ghidra release 目录里的 `.sla/.pspec`；native pcode 工具应优先使用
`/sn640/ghidra/Ghidra/Processors/.../data/languages/` 下同版本的 spec。

如果开启 Sleigh，入口大致是：

```bash
notdec-sleigh-pcode ...
notdec-sleigh-llvm ...
notdec-native-pcode ...
notdec-native-llvm ...
```

`notdec-native-llvm` 对 x86-64 ELF 可以自动选择 spec，默认使用
`/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64.sla` 和同目录的
`x86-64.pspec`。手动传入 `[sla-file]` 和 `-s <pspec-file>` 时仍会覆盖默认选择。

执行顺序：

1. `tools/SleighBytes.cpp` 用 Sleigh 从字节生成 `PcodeProgram`。
2. `PcodeCollector::dump(...)` 收集每条 P-Code op。
3. `buildPcodeModule(...)` 创建一个 void LLVM function。
4. `PcodeLowerer::lower(...)` 按线性 P-Code 建 basic block 并 lower op。

这条路径使用的是 `Pcode.h` 里的简化结构：

- `VarnodeView`
- `PcodeOpView`
- `PcodeProgram`

它没有 Ghidra HighFunction 的 SSA、block、high variable 信息，所以能力弱于 heritage 路径。

## 主要限制

当前代码能生成结构上可验证的 LLVM IR，但还不是完整语义恢复：

1. 源类型到 LLVM 类型的映射很粗，很多类型都落到整数。
2. 外部函数和 call-site prototype 仍使用 vararg。
3. `notdec_ram` 只是临时内存模型，还没表达真实 ELF section、stack object、GOT/PLT 和 relocation。
4. 未初始化 varnode、部分 PHI incoming、间接跳转失败路径会使用 poison fallback。
5. 部分 P-Code op 通过 helper call 保留，不是精确 lowering。
6. 模块级 lowering 能隔离单函数失败，但失败函数只有 declaration，没有 body。
