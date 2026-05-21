# BRANCHIND GOT External Lowering

## 原始 prompt

```text
在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis里面根据划分的几个关键宏观步骤，创建对应的文件夹，然后在prompt中要求，对应步骤的规划和修改必须放到对应的文件夹里。

本次目标是按照规划，完善bin2llvm项目反汇编转IR的native链路（即不走GhidraScript的链路，而是走libsla，libdecomp的这个链路），从Ghidra和rizin那边复刻足够多的功能。每当实现一部分功能后，就看看能否利用bench2里面这些项目去做初步的测试： （vsftpd可执行文件，libuv动态链接库，memcached可执行文件），测试实现是否正确。规划中每一部分都完成后即可停止。在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis 下创建一个PROGRESS.md里追踪计划的实现。完成一个部分后，同步更新 项目顶层的ARCHITECTURE.md 反映实现的功能模块的架构。

约束：根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## 当前目标和已有 native 状态

上一小块已经让 discovery 把 guarded external tail jump 识别成
`sleigh-pcode-got-indirect-branch` flow xref，不再计入 unresolved。Bench2 里对应位置是：

- `vsftpd`: `0x82af -> _ITM_deregisterTMCloneTable`
- `libuv`: `0x9d9f -> _ITM_deregisterTMCloneTable`
- `memcached`: `0xb96f -> _ITM_deregisterTMCloneTable`

但 lowering 现在遇到 `BRANCHIND` 只跳到 `notdec_exit`，没有表达外部 tail jump 语义。
本小块只补这个已由 relocation 证明的外部 `GLOB_DAT` tail jump。

## Ghidra 相关实现

Ghidra 对这类问题不是在 `BRANCHIND` 上乱猜，而是结合 thunk、computed flow 和 external symbol：

- `Ghidra/Processors/x86/data/languages/ia.sinc`
  - `JMP rm64` 规则把 `jmp *reg/mem` 翻译成 `goto [rm64]`，即 P-Code `BRANCHIND`。
- `Ghidra/Features/Base/src/main/java/ghidra/app/cmd/function/CreateThunkFunctionCmd.java::getThunkedAddr(...)`
  - 判断函数是否是 thunk，要求不能有复杂副作用，最终是跳转或 call-return。
- `CreateThunkFunctionCmd.java::resolveComputableFlow(...)`
  - 在单个 basic block 内用 `SymbolicPropogator::flowConstants(...)` 尝试解析 computed flow。
- `CreateThunkFunctionCmd.java::getExternalFunction(...)`
  - 如果目标是 external location，转成 external function。
- `Ghidra/Features/Base/src/main/java/ghidra/app/cmd/function/CreateFunctionCmd.java::resolveThunk(...)`
  - 创建函数时先尝试把 thunk 解析到外部或已知目标。

native 侧不做完整 thunk 判定，只复刻 Bench2 已出现的安全子集：`BRANCHIND` 输入来源能追到外部
`X86_64_GLOB_DAT` GOT slot。

## native 侧复刻策略

1. 复用 `PcodeLoweringConfig::IndirectExternalCallTargets` 这张 GOT slot 到外部符号名的表。
2. `PcodeLowerer` 已经记录 `COPY` 后的来源 RAM 地址，`BRANCHIND` 也可以查同一份来源。
3. `BRANCHIND` 如果输入来源命中外部 GOT 表，生成 `call void @symbol()` 后 `ret void`。
4. 其他 `BRANCHIND` 继续跳到 `notdec_exit`，不解析 PLT0、jump table 或普通函数指针。
5. Bench2 smoke 增加 `_ITM_deregisterTMCloneTable` pattern，避免以后退回无语义的 exit。

暂时不做：

- 不恢复 tail call 的真实 ABI、参数和返回值。
- 不把它建成 LLVM `musttail` 或 `tail` call。
- 不解析 jump table。
- 不改 discovery unresolved 规则。

## 判断标准

1. 三个 Bench2 目标的 all-confirmed IR 中出现 `call void @_ITM_deregisterTMCloneTable()`。
2. Bench2 smoke 继续通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
3. 未知 `BRANCHIND` 行为不扩大，仍走 `notdec_exit`。
4. 性能不明显变慢。

## 风险

1. `BRANCHIND` 是 tail jump，不是普通 call。当前 native lowering 只有 `void ()` 骨架，先用
   `call void` 后 `ret void` 表达“把控制交给外部函数后本函数结束”。
2. `_ITM_deregisterTMCloneTable` 可能是弱 `NOTYPE UND`，不能要求 FUNC；必须要求来源 GOT slot
   是外部 `GLOB_DAT`。
3. 如果后续接入 ABI/原型恢复，这里需要改成更准确的 tail-call 表达。

## 实现记录

改动文件：

- `include/notdec-bin2llvm/PcodeToLLVM.h`
  - 第 31-35 行 `PcodeLoweringConfig::IndirectExternalCallTargets` 注释：说明这张 GOT slot 到符号名的表同时服务 `CALLIND` 和 `BRANCHIND`。
- `lib/PcodeToLLVM.cpp`
  - 第 242-253 行 `PcodeLowerer::lowerTerminator(...)` 的 `BRANCHIND` 分支：如果输入来源能追到 `IndirectExternalCallTargets` 中的外部 GOT slot，走 external tail jump lowering；否则仍跳到 `notdec_exit`。
  - 第 679-685 行 `lowerKnownVoidTailJump(...)`：生成 `call void @symbol()` 后 `ret void`。
- `scripts/bench2-native-smoke.sh`
  - 第 126-149 行 `check_ir_features(...)`：三个 Bench2 目标都检查 `call void @_ITM_deregisterTMCloneTable()`。
- `ARCHITECTURE.md`
  - 第 135-140 行：记录 `BRANCHIND` 外部 GOT tail jump lowering。
  - 第 146-148 行：记录 smoke 覆盖 GOT external tail jump pattern。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
  - 第 55 行：阶段 6 记录 `BRANCHIND` external tail jump lowering。
  - 第 66 行：阶段 7 记录 `_ITM_deregisterTMCloneTable` smoke pattern。

实现说明：

- 没有新增目标发现逻辑，只复用已有 `NativeProgramState::relocations()` 规划出的外部 `GLOB_DAT` 表。
- 未命中的 `BRANCHIND` 仍保留原行为，跳到 `notdec_exit`。
- 这不是完整 tail-call ABI 建模，只是当前 `void ()` native lowering 下的保守表达。

验证：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
```

通过。

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-46
```

结果：

- `vsftpd`：ok，8s，confirmed 9，blocks 30，instr 80，xrefs 303，unresolved branch 1。
- `libuv`：ok，18s，confirmed 9，blocks 29，instr 85，xrefs 26，unresolved branch 0。
- `memcached`：ok，8s，confirmed 9，blocks 30，instr 80，xrefs 185，unresolved branch 1。

IR pattern：

```text
vsftpd.all-confirmed.ll: call void @_ITM_deregisterTMCloneTable()
libuv.all-confirmed.ll: call void @_ITM_deregisterTMCloneTable()
memcached.all-confirmed.ll: call void @_ITM_deregisterTMCloneTable()
```

stdout/stderr 文件为空。LLVM 22 `llvm-as` 和 `opt -passes=verify` 均通过。

性能：

- 本次只在 `BRANCHIND` terminator lowering 时多一次来源 map 和外部 GOT 表查询。
- 三目标 smoke 总耗时约 34s，和上一块同口径结果接近，没有看到明显变慢。

评分：

- 实现效果：8/10。Bench2 三个外部 GOT tail jump 都落到明确 external symbol。
- 复杂度：2/10。复用已有 GOT 外部表和来源追踪，只新增一条 terminator lowering 分支。
- 维护成本：3/10。后续 ABI/原型恢复时需要把 `void ()` tail jump 表达替换成更准确的调用约定。
