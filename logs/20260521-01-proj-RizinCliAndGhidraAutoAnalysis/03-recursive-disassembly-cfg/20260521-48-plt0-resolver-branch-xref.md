# PLT0 Resolver Branch Xref

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

当前 Bench2 discovery 只剩 `vsftpd` / `memcached` 的 `0x5026` unresolved indirect branch。
反汇编显示这是标准 x86-64 PLT0 resolver：

```text
5020: push qword ptr [GOT+8]
5026: jmp  qword ptr [GOT+16]
```

这不是 jump table，也不是普通函数指针；它是动态链接器 lazy binding 入口。当前 native 已能识别
普通 PLT stub、`.plt.got` thunk 和外部 GOT tail jump，只差 PLT0 这一个固定模式。

## Ghidra 相关实现

Ghidra 对 PLT / thunk / external flow 会用 thunk 和 reference 机制处理，不把它当普通间接跳转：

- `Ghidra/Processors/x86/data/languages/ia.sinc`
  - `JMP rm64` 把 `jmp *mem` 翻译成 `goto [rm64]`，即 P-Code `BRANCHIND`。
- `Ghidra/Features/Base/src/main/java/ghidra/app/cmd/function/CreateThunkFunctionCmd.java::getThunkedAddr(...)`
  - 判断 thunk 时要求最终是 jump 或 call-return，并限制副作用。
- `CreateThunkFunctionCmd.java::resolveComputableFlow(...)`
  - 对 computed flow 做单 block 常量传播，只有解析出单一目标才接受。
- `Ghidra/Features/Base/src/main/java/ghidra/app/cmd/function/CreateFunctionCmd.java::resolveThunk(...)`
  - 创建函数前先尝试 thunk 解析，命中时不把 computed jump 当普通函数体继续扩张。

native 侧这次不建模动态链接器，也不解析 lazy binding 的真实目标；只把 PLT0 resolver slot 记录成一个
已知 flow xref，避免继续算未知 indirect branch。

## native 侧复刻策略

1. 在 `SleighSeedInstructionAnalyzer::addDirectControlFlow(...)` 的 `BRANCHIND` 分支中，在普通
   PLT GOT 和外部 GOT 判断之后，增加 PLT0 resolver 判断。
2. 只接受这个窄模式：
   - 指令地址是 `.plt` 起始地址加 6，也就是 PLT0 的第二条指令。
   - `BRANCHIND` 输入是 direct `ram`。
   - 目标地址是 `.got` 起始地址加 16，也就是 GOT[2] resolver slot。
3. 命中时记录 flow xref：`sleigh-pcode-plt0-resolver-branch`。
4. 命中后不再写 unresolved。
5. 其他 `BRANCHIND` 保持原样，后续 jump table 仍单独处理。

暂时不做：

- 不解析动态链接器 resolver 的真实地址。
- 不把 PLT0 当外部函数 call lowering。
- 不处理非 x86-64 或非标准 PLT0 形状。
- 不解析 jump table。

## 判断标准

1. `vsftpd` / `memcached` 的 `0x5026` 不再 unresolved。
2. 三个 Bench2 目标 unresolved indirect call / branch 都为 0。
3. xref 查询能看到 `sleigh-pcode-plt0-resolver-branch`。
4. Bench2 smoke 继续通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
5. 性能不明显变慢。

## 风险

1. 这个判断依赖标准 x86-64 ELF PLT 布局，所以必须同时检查 `.plt` 和 `.got` 位置。
2. 只记录 resolver slot xref，不代表已经恢复 lazy binding 语义。
3. 如果某些二进制没有 `.got` 或使用不同 PLT 布局，本规则不会命中，仍保留 unresolved。

## 实现记录

改动文件：

- `lib/NativeAnalysis.cpp`
  - 第 1571-1586 行 `SleighSeedInstructionAnalyzer::addDirectControlFlow(...)` 的 `BRANCHIND` 分支：在普通 PLT GOT slot 判断后，增加 PLT0 resolver slot 判断，命中时记录 `sleigh-pcode-plt0-resolver-branch` flow xref，并跳过 unresolved。
  - 第 1709-1717 行 `isPlt0ResolverSlot(...)`：只接受 branch 地址等于 `.plt + 6` 且目标 GOT 地址等于 `.got + 16` 的标准 x86-64 PLT0 resolver。
- `scripts/bench2-native-smoke.sh`
  - 第 210-211 行：当前三目标 unresolved indirect branch 上限统一收紧到 0。
- `ARCHITECTURE.md`
  - 第 61-67 行：记录 PLT0 resolver branch xref 行为。
  - 第 144-145 行：记录 Bench2 smoke 不再允许 unresolved indirect call / branch。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
  - 第 27 行：阶段 3 记录 PLT0 resolver `BRANCHIND` xref。
  - 第 70 行：阶段 7 记录 branch 基线收紧到 0。

实现说明：

- 没有解析动态链接器 resolver 的真实地址。
- 没有改变 lowering；PLT0 IR 仍保守结束。
- 这个规则只匹配 `.plt` 首个 stub 的第二条指令和 `.got + 16`，不会泛化到普通间接跳转。

验证：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
```

通过。

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-48
```

结果：

- `vsftpd`：ok，8s，confirmed 9，blocks 30，instr 80，xrefs 304，flow 14，unresolved 0。
- `libuv`：ok，19s，confirmed 9，blocks 29，instr 85，xrefs 26，flow 10，unresolved 0。
- `memcached`：ok，7s，confirmed 9，blocks 30，instr 80，xrefs 186，flow 14，unresolved 0。

xref 查询：

```text
vsftpd 0x5026 -> 0x25a00 flow sleigh-pcode-plt0-resolver-branch
memcached 0x5026 -> 0x3ea18 flow sleigh-pcode-plt0-resolver-branch
```

`notdec-native-discover --unresolved-json`：

- `vsftpd`: `count = 0`
- `libuv`: `count = 0`
- `memcached`: `count = 0`

stdout/stderr 文件为空。LLVM 22 `llvm-as` 和 `opt -passes=verify` 均通过。

性能：

- 本次只在 `BRANCHIND` direct RAM GOT 判断里多查 `.plt` / `.got` 两个 section 地址。
- 三目标 smoke 总耗时约 34s，和上一块同口径接近，没有看到明显变慢。

评分：

- 实现效果：8/10。Bench2 当前唯一剩余 unresolved indirect branch 被解释成 PLT0 resolver xref。
- 复杂度：2/10。判断条件很窄，只依赖已有 section 信息。
- 维护成本：2/10。后续如要精确建模动态 resolver，可在这个 source 基础上扩展，不影响普通 jump table 处理。
