# PLT.GOT Byte Match

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

`RelocationPltAnalyzer` 已经能输出 `.plt.sec` 和部分 `.plt.got` 的 `NativePltEntry`。但现在
`.plt.got` 仍有一个问题：它要求 `.plt.got` slot 数量和所有函数型 `GLOB_DAT` relocation
数量一致。Bench2 的 `vsftpd` / `memcached` 不是这种形状：

- `.plt.got` section 只有 16 字节，一个 stub。
- 函数型 `GLOB_DAT` relocation 不止一个。
- 真正的 `.plt.got` stub 是 `jmpq *rip+disp32`，目标分别是：
  - `vsftpd`: `0x5ba4 -> 0x25fc8 -> __cxa_finalize`
  - `memcached`: `0x5b84 -> 0x3efc8 -> __cxa_finalize`

上一小步已经能把命中 `NativePltEntry` 的 `BRANCHIND` 从 unresolved 中移除。现在要让这些
`.plt.got` stub 先正确进入 `NativePltEntry`。

## Ghidra 相关实现

Ghidra 对这类外部 thunk 不靠 relocation 数量对齐硬猜，而是结合指令和外部符号引用：

- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/ExternalSymbolResolverAnalyzer.java`
  - 负责建立外部符号和外部引用。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/FunctionStartAnalyzer.java`
  - 使用引用和符号信息识别 thunk / 外部跳板。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java`
  - 保存指令到目标或外部符号的引用。
- `Ghidra/Processors/x86/data/languages/ia.sinc`
  - x86 `jmp *mem` 仍会生成 indirect branch；是否是外部 thunk 要结合指令目标和 relocation。

native 侧不做完整 thunk 分析。这次只复刻最小策略：直接读 `.plt.got` 机器码，算出 GOT slot，
再反查 `GLOB_DAT` relocation。

## native 侧复刻策略

1. `addPltGotEntries(...)` 不再要求 `.plt.got` stub 数量等于函数型 `GLOB_DAT` relocation 数量。
2. 对 `.plt.got` 每个 16 字节 slot 读取前 10 字节。
3. 只接受 `endbr64; ff 25 <disp32>` 这种 x86-64 RIP-relative indirect jump。
4. 用 `stub + 10 + disp32` 算 GOT 地址。
5. GOT 地址必须命中一个外部 `X86_64_GLOB_DAT` 且有符号名的 relocation，才加入 `NativePltEntry`。
6. 其他 slot 跳过，不猜。

暂时不做：

- 不解析 PLT0 resolver。
- 不处理非 `ff 25 disp32` 的 thunk。
- 不把 `_ITM_*` guarded tail jump 算成 PLT。
- 不改 xref JSON 格式。

## 判断标准

1. `vsftpd --plt-json` 出现 `0x5ba0 -> __cxa_finalize via GOT 0x25fc8`。
2. `memcached --plt-json` 出现 `0x5b80 -> __cxa_finalize via GOT 0x3efc8`。
3. 两个目标的 `.plt.got` `BRANCHIND` 不再 unresolved。
4. Bench2 smoke 继续通过 LLVM 22 verify。
5. 运行时间不明显变慢。

## 风险

1. `NativePltEntry::StubAddress` 仍记录 16 字节 slot 起点，也就是 `endbr64` 地址；`BRANCHIND`
   xref 目标是 GOT slot。这和当前 `.plt.sec` 建模保持一致。
2. 如果 `.plt.got` 有其他指令形状，本小步会跳过，避免误判。
3. 外部符号名只来自 relocation，不尝试恢复版本后缀以外的 ABI 信息。

## 实现记录

### 修改文件和函数

- `lib/NativeAnalysis.cpp`
  - 第 387 行 `RelocationPltAnalyzer::addPltGotEntries(...)`：不再要求 `.plt.got` slot 数量和函数型 `GLOB_DAT` relocation 数量一致。
  - 第 408 行起：逐个读取 `.plt.got` 16 字节 slot 的前 10 字节。
  - 第 415 行起：只接受 `endbr64; jmp *rip+disp32` 形状。
  - 第 421 行起：从 disp32 反算 GOT 地址，并反查外部 `X86_64_GLOB_DAT` relocation。
  - 第 439 行起：命中后写入 `NativePltEntry`。
- `scripts/bench2-native-smoke.sh`
  - 第 120 行和第 138 行：`vsftpd` / `memcached` 的 `.plt.got` direct call 预期改为 `__cxa_finalize()`。
  - 第 196 行：`vsftpd` / `memcached` unresolved indirect branch 上限从 3 收紧到 2。
- `ARCHITECTURE.md`
  - 第 70 行：记录 `.plt.got` 现在按机器码反查 GOT slot。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
  - 阶段 4 记录 `.plt.got` byte-match。
  - 阶段 3 记录 byte-match 带来的 `BRANCHIND` unresolved 减少。

### 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
```

`--plt-json`：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --plt-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --plt-json /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
```

结果：

```text
vsftpd: stub=0x5ba0 got=0x25fc8 symbol=__cxa_finalize
memcached: stub=0x5b80 got=0x3efc8 symbol=__cxa_finalize
```

Bench2 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-44c
```

结果：

```text
vsftpd ok elapsed=7s
libuv ok elapsed=19s
memcached ok elapsed=7s
```

summary：

```text
vsftpd: confirmed=9 blocks=30 instr=80 xrefs=302 flow=12 call=2 unresolved=2 indirect_call=0 indirect_branch=2
libuv: confirmed=9 blocks=29 instr=85 xrefs=25 flow=9 call=3 unresolved=1 indirect_call=0 indirect_branch=1
memcached: confirmed=9 blocks=30 instr=80 xrefs=184 flow=12 call=2 unresolved=2 indirect_call=0 indirect_branch=2
```

IR 检查：

```bash
rg -n 'notdec_native_5ba0\(|notdec_native_5b80\(|__cxa_finalize|notdec_native_8290|notdec_native_b950|notdec_pcode_CALLIND_void' /tmp/notdec-bin2llvm-bench2-smoke-20260521-44c/*.ll
```

结果：

- `vsftpd` / `memcached` 仍有对应 PLT stub 函数定义，但 direct call 已落到 `__cxa_finalize()`。
- `vsftpd` 仍有内部 direct call `notdec_native_8290()`。
- `memcached` 仍有内部 direct call `notdec_native_b950()`。
- 没有 `notdec_pcode_CALLIND_void`。
- 所有 stdout / stderr 文件为空。

### 评分

- 实现效果：8/10。`vsftpd` / `memcached` 的 `.plt.got` `__cxa_finalize` 都进入 PLT 映射，并减少 unresolved branch。
- 理解成本：4/10。增加了 10 字节指令形状检查，但逻辑直接。
- 维护成本：3/10。后续如果遇到非 `endbr64; jmp *rip+disp32` 形状，需要单独扩展，不影响当前规则。
