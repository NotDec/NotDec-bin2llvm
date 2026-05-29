# Native Register SSA Pass Plan

## 原始 prompt

接下来需要思考寄存器如何消除。对于每个native链路生成的函数，需要基于SSA构建的思想，如果使用点没有对应的定义点的话，那么就是使用到了外部传入的寄存器结果，要分析出这个函数使用了哪些外部传入的寄存器结果。其次，如果使用点有对应的明确的定义点的话，就直接用SSAvalue去替代。另外，如果有函数开头保存寄存器，函数结尾恢复寄存器的行为的话，这个也可以直接单独考虑进来，标注出来。SSA构建算法需要使用那个《Simple and Efficient SSA Construction》的那种算法。先讨论，目前是规划阶段，只需要形成一个logs/下的规划。

补充要求：拓展当前 native 链路，最后部分增加一个跑 pass 的过程。命令行直接支持 IR 输入，如果输入是 IR，就只跑 pass。Pass 的代码单独放到一个文件夹里。

## 当前目标和已有 native 状态

目标是把 native 链路生成的寄存器读写从“全局寄存器变量 load/store”推进到函数内 SSA 形式。

当前 native lowering 的状态：

- `lib/PcodeToLLVM.cpp` 里 `PcodeLowerer::lower(...)` 按 native CFG 建 LLVM basic block。
- `PcodeLowerer::read(...)` 对 register varnode 会走 `RegisterStorage::read(...)`。
- `PcodeLowerer::write(...)` 对已知 register varnode 会走 `RegisterStorage::write(...)`，然后直接返回。
- `Values` 是 block 内局部值表，每个 block 开始会 `Values.clear()`。它适合保存 raw Sleigh `unique` 这类临时值，不适合作为跨 block SSA 状态。
- `lib/RegisterStorage.cpp` 现在负责把 `RAX/EAX/AX/AL` 这类重叠寄存器合成同一个 backing global，并用 bit slice 处理子寄存器读写。

所以本阶段不直接改 `Values` 的语义，也不把 `unique` 纳入跨块 SSA。寄存器 SSA 单独做一层，只接管 register space 的读写结果。

## Ghidra 相关实现

Ghidra 的 SSA 建构在 heritage 阶段完成，相关源码在：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.hh`
  - `Heritage` 类说明 SSA 构建由两个核心步骤组成：`placeMultiequals()` 放置 `MULTIEQUAL`，`rename()` 做重命名。
  - 同文件还说明 Ghidra 会按 address space 分阶段 heritage，先处理寄存器，再处理栈等更复杂位置。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::placeMultiequals()` 负责合流点 PHI 形态的 `MULTIEQUAL` 放置。
  - `Heritage::rename()` / `Heritage::renameRecurse(...)` 负责沿 CFG 重命名 varnode。
  - `Heritage::calcMultiequals(...)` 计算需要插入 merge 的位置。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/funcdata_varnode.cc`
  - `Funcdata::setInputVarnode(...)` 把没有本函数定义的 varnode 变成函数输入。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionInputPrototype::apply(...)` 后续会从 input varnode 推导函数原型输入。

native 侧不用完整复刻 Ghidra heritage。这里先只复刻寄存器部分：本函数内有定义就替换成 SSA value；没有定义就记录为外部传入寄存器；多前驱合流时创建 PHI。

算法上不走 dominance frontier 预放置路线，采用《Simple and Efficient SSA Construction》里的按需思路：读某个 block 的某个寄存器时，如果本 block 没有定义，就递归查前驱，必要时先放一个不完整 PHI，等前驱值补齐后再简化。

## native 侧复刻策略

### 1. Pass 位置

当前 native 链路先由 raw Sleigh P-Code 生成 LLVM IR。寄存器 SSA 消除作为生成后的 LLVM pass 运行：

```text
ELF -> native discovery -> Sleigh P-Code -> initial LLVM IR -> native register SSA pass -> output IR
```

原因：

- 初始 lowering 已经能处理 native CFG、PLT、external call、sparse block 等问题。
- pass 可以直接看 LLVM IR 里的 `notdec.register` / `notdec.register.access` metadata，不需要重新理解所有 P-Code opcode。
- pass 可以单独跑在已有 `.ll` / `.bc` 上，方便对 Bench2 已生成 IR 反复调试。

### 2. 代码组织

Pass 代码单独放到一个目录，不塞进 `PcodeToLLVM.cpp`：

- `include/notdec-bin2llvm/passes/`
  - 放 pass 的公开声明。
- `lib/passes/`
  - 放寄存器 SSA pass 实现。
- `lib/CMakeLists.txt`
  - 把 pass 目录接进现有 native library 或单独 pass library。

先只做 in-tree pass，不先做独立 `opt` 插件。这样 `notdec-native-llvm` 可以直接调用，后续如果需要再导出给 `opt`。

### 3. 命令行输入

`notdec-native-llvm` 增加 IR 输入路径：

- 输入是 ELF 时，保持现有流程，最后默认跑 native register SSA pass。
- 输入是 `.ll` 或 `.bc` 时，不跑 discovery，不跑 Sleigh，不重新 lower，只读取 IR module，然后跑 pass，再输出。
- 需要保留一个关闭开关，例如 `--no-register-ssa-pass`，方便对比 pass 前后差异。

判断 IR 输入可以先按扩展名和 LLVM parser 结果做保守处理。解析失败时直接报错，不 fallback 成 ELF。

### 4. 寄存器 SSA 状态

Pass 需要先从现有 IR 里识别寄存器读写：

- register global：带 `notdec.register` metadata 的 global。
- register access：带 `notdec.register.access` metadata 的 load/store。
- 寄存器单位：优先沿用 `RegisterStorage` 的 backing global 粒度。

每个函数内建立：

- `LocalDef[block, reg-unit]`：本 block 内最后一次写出的 SSA value。
- `CurrentDef[block, reg-unit]`：按需 SSA 查询缓存。
- `ExternalInput[function, reg-unit]`：本函数读到但没有本函数定义的寄存器输入。
- `PendingPhi[block, reg-unit]`：按需创建的 PHI，等前驱值补齐。

读寄存器时：

- 如果本 block 内已有明确写入，直接替换成该 SSA value。
- 如果没有，按需递归查前驱。
- 无前驱或所有路径都没有定义时，生成外部输入占位值，并记录该函数依赖这个寄存器。

写寄存器时：

- 把 store 的 value 记成当前 block 的 `LocalDef`。
- 后续同一函数内能被 SSA 值替代的 load/store 可以删。

### 5. 部分寄存器和别名

不能按 `RAX/EAX/AX/AL` 分别做完全独立变量，否则会破坏别名关系。

第一阶段建议只对完整 backing register unit 做强替换：

- 全宽写后全宽读，可以直接替换。
- 子寄存器读写先保持原有 bit slice 逻辑，或者只在能精确组合时改写。
- `EAX` 写会清零 `RAX` 高 32 位这类 x86-64 语义，必须确认初始 IR 里已经明确表达后才能折叠。

如果某个 access 不是完整 unit，pass 先保守保留 load/store，并在统计里标出来。这样不会为了消除寄存器引入错误别名语义。

### 6. CALL 和 clobber

CALL 是寄存器 SSA 的主要风险点。第一阶段不能假装 call 不改寄存器。

保守策略：

- 对已知 call 后的 caller-saved register，认为定义被 clobber。
- 对 return register，如 x86-64 的 `RAX/XMM0`，如果 lowering 没有显式写返回值，先记录为 call result 缺口，不强行造语义。
- callee-saved register 不因 call 自动 clobber。

更准确的 ABI 建模可以后续单独做，不能和第一版寄存器 SSA 混在一起。

### 7. 保存和恢复标注

入口保存、出口恢复先做标注，不直接删 IR：

- 入口附近识别 `push rbx`、`mov [rsp+off], r12` 这类保存。
- 出口附近识别 `pop rbx`、`mov r12, [rsp+off]` 这类恢复。
- 如果恢复值能追到同一个入口保存槽，给函数或指令加 metadata，标记 callee-saved preserve。

这部分先不影响主 SSA 替换。原因是栈别名、异常出口、多 return block 都可能让“看起来保存恢复”的模式变复杂。

## 阶段计划

### 阶段 1：只做可观测分析

新增 pass skeleton 和 CLI 接入，但先不删 load/store。

输出每个函数：

- 使用了哪些外部传入寄存器。
- 哪些 register load 找到了本函数内定义。
- 哪些 load 因为子寄存器、call clobber、CFG 不完整而不能替换。
- 哪些函数疑似有 callee-saved 保存/恢复。

判断标准：

- ELF 输入原流程能正常生成 IR 并跑 pass。
- `.ll` / `.bc` 输入能只跑 pass。
- `libuv module-all.ll` 能跑完并产出统计。

### 阶段 2：完整寄存器 unit 替换

只替换最安全的场景：

- 同一完整 register unit。
- 明确被本函数内 store 定义。
- PHI incoming 都能找到有效 SSA value。
- 中间没有未建模 call clobber。

判断标准：

- 替换后 `llvm-as` 和 `opt -passes=verify` 通过。
- Bench2 selected native 的定义数量不下降。
- pass 后 register load/store 数量下降，且没有新增 poison/freeze fallback。

### 阶段 3：PHI 简化和外部输入表达

完善按需 SSA：

- 单 incoming 或所有 incoming 相同的 PHI 要删掉。
- 无本函数定义的寄存器读统一映射到函数级外部输入。
- 外部输入先用 metadata 和占位 intrinsic 表达，不急着改函数签名。

判断标准：

- 每个函数能列出稳定的 external register inputs。
- 同一个寄存器输入不会在函数内重复生成多个无关占位值。

### 阶段 4：保存/恢复标注

加入 callee-saved preserve 标注。

判断标准：

- 能在 libuv、memcached、vsftpd 里识别常见 `RBX/RBP/R12-R15` 保存恢复。
- 标注不改变 IR 语义。
- 多出口函数不会误标成完整恢复。

## 风险

- 寄存器别名是最大风险。第一版必须保守处理子寄存器。
- CALL clobber 如果处理太乐观，会直接造成错误语义。
- 当前 IR 里 metadata 是后补分析的入口，metadata 不完整会限制 pass 能力。
- `unique` varnode 仍由 block 内 `Values` 处理，不纳入本 pass。若发现跨 block `unique` 依赖，要先记录成 lower/block 切分问题。
- 保存/恢复模式依赖栈分析，第一版只做标注，不用它删除代码。

## 不做什么

- 不在第一版改函数签名传入寄存器参数。
- 不把 RAM/stack 也纳入 SSA。
- 不重写 `PcodeToLLVM.cpp` 的 block 内 `Values` 逻辑。
- 不做完整 ABI prototype recovery。
- 不把 pass 做成外部 `opt` 插件。

## 判断标准

本规划完成后的实现应满足：

- `notdec-native-llvm` 对 ELF 输入仍能生成 module，并在末尾跑 register SSA pass。
- `notdec-native-llvm` 对 `.ll` / `.bc` 输入只跑 register SSA pass。
- pass 代码位于单独 pass 目录。
- 每个 native 函数能报告外部传入寄存器集合。
- 安全场景下 register load 能被 SSA value 替代。
- `libuv` selected native module 通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
- 对 fortune 当前关注用例记录 pass 前后同口径运行时间，确认没有明显性能下降。

## 实现记录 2026-05-29

本次完成了阶段 1、阶段 2、阶段 3 的第一版。阶段 4 保存/恢复标注暂未做。

改动文件和函数：

- `include/notdec-bin2llvm/passes/NativeRegisterSSA.h:14` 新增 `NativeRegisterSSAOptions`、`NativeRegisterSSAFunctionSummary`、`NativeRegisterSSASummary`，公开 `runNativeRegisterSSA(...)` 和 `printNativeRegisterSSASummary(...)`。
- `lib/passes/NativeRegisterSSA.cpp:88` 新增 `collectRegisterUnits(...)`，从 `notdec.register` metadata 收集寄存器 backing global。
- `lib/passes/NativeRegisterSSA.cpp:105`、`lib/passes/NativeRegisterSSA.cpp:123` 新增 `registerLoad(...)`、`registerStore(...)`，只接受带 `notdec.register.access` 的完整 register unit load/store。
- `lib/passes/NativeRegisterSSA.cpp:142` 新增 `isRegisterClobberCall(...)`，LLVM intrinsic 不算 clobber，普通/间接 call 算 clobber。
- `lib/passes/NativeRegisterSSA.cpp:151` 新增 `FunctionPromoter`，实现函数内寄存器 SSA 查询、load 替换、external input metadata、PHI 创建和简化。
- `lib/passes/NativeRegisterSSA.cpp:288` 的 `readBlockEntry(...)` 按需查前驱，无法证明 call 后寄存器值时保守返回空，不替换原 load。
- `lib/passes/NativeRegisterSSA.cpp:403` 的 `externalInput(...)` 在函数入口生成一次带 `notdec.register.external_input` metadata 的 load，函数级 metadata 用 `notdec.register.external_inputs` 记录输入寄存器集合。
- `lib/passes/NativeRegisterSSA.cpp:486` 新增 `runNativeRegisterSSA(...)`，按 module 运行 pass。
- `lib/passes/NativeRegisterSSA.cpp:513` 新增 `printNativeRegisterSSASummary(...)`，输出全局和逐函数统计。
- `lib/CMakeLists.txt:7` 把 `passes/NativeRegisterSSA.cpp` 加入 `notdec-bin2llvm-core`。
- `tools/notdec-native-llvm.cpp:96` 新增 IR 后缀识别，`.ll` / `.bc` 输入只读 IR 并跑 pass。
- `tools/notdec-native-llvm.cpp:124` 新增 `--no-register-ssa-pass`，`tools/notdec-native-llvm.cpp:128` 新增 `--register-ssa-summary`。
- `tools/notdec-native-llvm.cpp:698` 新增 `readIRModule(...)`。
- `tools/notdec-native-llvm.cpp:716` 新增 `runRegisterSSAPassIfEnabled(...)`。
- `tools/notdec-native-llvm.cpp:742` 接入 IR 输入路径。
- `tools/notdec-native-llvm.cpp:853` 在 ELF/native lowering 输出后默认运行 register SSA pass。

关键取舍：

- 只替换完整 backing register unit。子寄存器和别名暂时不碰。
- store 暂时保留，保证函数边界上的全局寄存器状态仍可观察。
- 普通 call 之后的寄存器 load 如果没有后续明确 store，不做替换，避免把入口寄存器误当成 call 后状态。
- 外部输入没有改函数签名，只用入口 load 和 metadata 表达。

测试中发现并修复的问题：

- `/bin/ls` smoke 暴露 PHI incoming 引用了后续要删除的 register load。修复在 `lib/passes/NativeRegisterSSA.cpp:283` 的 `resolveValue(...)` 和 `lib/passes/NativeRegisterSSA.cpp:221` 的 `Replacement` 缓存，后续 store/PHI incoming 会改用最终 SSA value。
- `/bin/ls` smoke 暴露复杂 CFG 下可能创建 incoming 不完整的 PHI。修复在 `lib/passes/NativeRegisterSSA.cpp:336` 的 `readBlockEntry(...)`，先收齐所有前驱 incoming；任一路径未知就不创建 PHI，也不替换对应 load。

验证命令和结果：

```bash
cmake -S /sn640/NotDec/external/NotDec-bin2llvm \
  -B /sn640/NotDec/external/NotDec-bin2llvm/build \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON \
  -DNOTDEC_BIN2LLVM_SLEIGH_SOURCE_DIR=/sn640/sleigh \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -GNinja
cmake --build /sn640/NotDec/external/NotDec-bin2llvm/build --target notdec-native-llvm -j2
time build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/libuv/shared-library/module-all.ll \
  -o /tmp/notdec-regssa/libuv-regssa.ll \
  --register-ssa-summary
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-regssa/libuv-regssa.ll \
  -o /tmp/notdec-regssa/libuv-regssa.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-regssa/libuv-regssa.bc \
  -o /tmp/notdec-regssa/libuv-regssa.verify.bc
```

libuv 全模块结果：

- `notdec-native-llvm` pass 运行时间：real `0m1.753s`，user `0m1.725s`，sys `0m0.028s`。
- 函数数：485。
- register load：7791。
- register store：6208。
- 替换 load：7551。
- 创建 PHI：168。
- 简化 PHI：126。
- external inputs：2032。
- 普通/间接 call：165。
- `.bc` 输入路径对 pass 后 bitcode 再运行通过 `llvm-as` 和 `opt -passes=verify`；第二次运行没有继续替换 load。
- ELF 输入 smoke：`/bin/ls -a 0x6aa0 -l 1024` 通过 pass、`llvm-as` 和 `opt -passes=verify`；该用例 loads=537 stores=537 replaced=505 phis=61 simplified=9 external_inputs=12 calls=3。
- pass 后剩余带 `notdec.register.access` 的 register load：240，主要是普通 call 后保守保留。
- `llvm-as` 和 `opt -passes=verify` 通过。

单函数抽样 15 个，全部 `llvm-extract` 后 `llvm-as` 和 `opt -passes=verify` 通过：

- `notdec_native_8000`: loads=20 stores=21 replaced=17 phis=0 simplified=0 external_inputs=1 calls=1
- `notdec_native_8020`: loads=4 stores=2 replaced=4 phis=1 simplified=0 external_inputs=1 calls=1
- `notdec_native_8da0`: loads=4 stores=0 replaced=4 phis=0 simplified=0 external_inputs=1 calls=3
- `notdec_native_8dd0`: loads=4 stores=0 replaced=4 phis=0 simplified=0 external_inputs=1 calls=3
- `notdec_native_9b40`: loads=6 stores=3 replaced=6 phis=0 simplified=0 external_inputs=2 calls=1
- `notdec_native_9b4d`: loads=2 stores=1 replaced=2 phis=0 simplified=0 external_inputs=1 calls=1
- `notdec_native_9b52`: loads=2 stores=1 replaced=2 phis=0 simplified=0 external_inputs=1 calls=1
- `notdec_native_9b57`: loads=2 stores=1 replaced=2 phis=0 simplified=0 external_inputs=1 calls=1
- `notdec_native_9b5c`: loads=2 stores=1 replaced=2 phis=0 simplified=0 external_inputs=1 calls=1
- `notdec_native_9b61`: loads=2 stores=1 replaced=2 phis=0 simplified=0 external_inputs=1 calls=1
- `notdec_native_9b66`: loads=2 stores=1 replaced=2 phis=0 simplified=0 external_inputs=1 calls=1
- `notdec_native_9b6b`: loads=2 stores=1 replaced=2 phis=0 simplified=0 external_inputs=1 calls=1
- `notdec_native_9b70`: loads=15 stores=14 replaced=15 phis=0 simplified=0 external_inputs=3 calls=0
- `notdec_native_9b7a`: loads=2 stores=1 replaced=2 phis=0 simplified=0 external_inputs=1 calls=1
- `notdec_native_9b7f`: loads=2 stores=1 replaced=2 phis=0 simplified=0 external_inputs=1 calls=1

评分：

- 实现效果：8/10。完整寄存器 unit 能消除大部分 load，并保守保留 call 后未知状态。
- 复杂度：6/10。新增一个独立 pass，按需 SSA 查询逻辑有递归和缓存，但范围集中。
- 后期维护成本：5/10。后续主要要补 ABI clobber、子寄存器别名和保存/恢复标注。

更好的后续方案：

- 给 call clobber 接 ABI 表，caller-saved 才保守失效，callee-saved 不失效。
- 给 partial register access 加明确 alias 合成规则后，再替换 `EAX/AX/AL` 这类访问。
- 保存/恢复标注单独做，不和本 pass 的 load 替换混在一起。
