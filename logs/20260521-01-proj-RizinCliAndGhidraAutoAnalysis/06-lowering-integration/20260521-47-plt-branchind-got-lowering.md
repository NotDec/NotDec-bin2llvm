# PLT BRANCHIND GOT Lowering

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

discovery 已经能把 `.plt.sec` / `.plt.got` 的 indirect branch 识别成
`sleigh-pcode-plt-indirect-branch`，不再计入 unresolved。当前 lowering 还没有消费
`NativePltEntry::GotAddress`，所以一些 PLT stub 的 `BRANCHIND (ram,GOT)` 仍只跳到
`notdec_exit`。

Bench2 里能直接看到：

- `vsftpd` 的 `.plt.sec` stub，例如 `getegid` / `SSL_get_error`。
- `memcached` 的 `.plt.sec` stub，例如 `SSL_CTX_use_PrivateKey_file` / `pthread_cond_signal`。
- `.plt.got` 的 `__cxa_finalize` 已经能通过外部 `GLOB_DAT` lowering 命中，但 stub 和 GOT
  两侧会生成不同 LLVM 名字，出现 `__cxa_finalize` / `__cxa_finalize_1`。

本小块目标是让 PLT GOT slot 和 PLT stub 共享同一个外部 LLVM symbol，并让 direct `ram`
GOT `BRANCHIND` lower 成外部 tail jump。

## Ghidra 相关实现

Ghidra 处理 PLT / external thunk 依赖符号和 thunk 信息，而不是把 PLT 间接跳转当普通跳表：

- `Ghidra/Processors/x86/data/languages/ia.sinc`
  - `JMP rm64` 规则把 `jmp *mem/reg` 翻译成 `goto [rm64]`，对应 P-Code `BRANCHIND`。
- `Ghidra/Features/Base/src/main/java/ghidra/app/cmd/function/CreateFunctionCmd.java::resolveThunk(...)`
  - 创建函数时先尝试解析 thunk，命中后创建 thunk function。
- `Ghidra/Features/Base/src/main/java/ghidra/app/cmd/function/CreateThunkFunctionCmd.java::getFirstBlockJumpCall(...)`
  - 从首个 block 找 unconditional jump/call 目标。
- `CreateThunkFunctionCmd.java::getExternalFunction(...)`
  - 目标是 external location 时创建或转换成 external function。
- `Ghidra/Features/Base/src/main/java/ghidra/app/cmd/function/CreateThunkFunctionCmd.java::resolveComputableFlow(...)`
  - 对 computed flow 做单 block 常量传播，只在能解析出单一目标时接受。

native 侧已有 `RelocationPltAnalyzer` 产出 `NativePltEntry`，里面有 stub 地址、GOT slot 和外部符号名。
这次只复刻这个确定映射。

## native 侧复刻策略

1. `planNativeCallTargets(...)` 给每个外部符号只分配一个 LLVM 名字。
2. 对每个 `NativePltEntry` 同时写入：
   - `External[StubAddress] = symbolName`，服务 direct `CALL` 到 PLT stub。
   - `IndirectExternal[GotAddress] = symbolName`，服务 `BRANCHIND (ram,GOT)`。
3. 外部 `GLOB_DAT` relocation 也复用同一套符号名，避免同一符号出现 `_1` 名字。
4. P-Code lowering 不新增分支；上一小块的 `BRANCHIND` external tail jump 已经会消费
   `IndirectExternalCallTargets`。
5. Bench2 smoke 增加当前自然出现的 PLT `BRANCHIND` 外部 symbol pattern。

暂时不做：

- 不处理 PLT0 resolver。
- 不解析 jump table。
- 不恢复外部函数参数和返回值。
- 不把所有 dynamic symbol 都声明进 IR。

## 判断标准

1. `vsftpd` / `memcached` 当前 `.plt.sec` stub 的 `BRANCHIND` lower 成外部 call 后 return。
2. `.plt.got` 的 `__cxa_finalize` 不再生成 `__cxa_finalize_1` 这种重复 symbol。
3. Bench2 smoke 继续通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
4. 未命中的 `BRANCHIND` 仍走 `notdec_exit`，PLT0 unresolved 不被掩盖。
5. 性能不明显变慢。

## 风险

1. 这里仍按 `void ()` 外部声明处理，签名不准；后续 ABI/原型恢复再修。
2. 同名外部符号如果来自不同库，目前仍会共用一个 LLVM 名字；这和当前 smoke 的符号粒度一致。
3. 只处理已有 `NativePltEntry`，如果 PLT 识别漏了，不在本小块补。

## 实现记录

改动文件：

- `tools/notdec-native-llvm.cpp`
  - 第 309-318 行 `planNativeCallTargets(...)`：新增 `externalNamesBySymbol` 和 `externalNameFor(...)`，同一外部符号只分配一个 LLVM 名字。
  - 第 329-335 行：每个 `NativePltEntry` 同时写入 `External[StubAddress]` 和 `IndirectExternal[GotAddress]`。
  - 第 338-347 行：外部 `GLOB_DAT` relocation 复用同一套 `externalNameFor(...)`。
- `scripts/bench2-native-smoke.sh`
  - 第 128-131 行：`vsftpd` 检查 `getegid` 和 `SSL_get_error` 的 PLT GOT indirect branch lowering。
  - 第 154-157 行：`memcached` 检查 `SSL_CTX_use_PrivateKey_file` 和 `pthread_cond_signal` 的 PLT GOT indirect branch lowering。
  - 第 161-162 行：禁止 `__cxa_finalize_1` 这类重复外部 symbol 回退。
- `ARCHITECTURE.md`
  - 第 135-141 行：记录 `BRANCHIND` 命中 `NativePltEntry::GotAddress` 时的 external tail jump lowering，以及 PLT stub / GOT slot 共享 LLVM symbol。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
  - 第 56 行：阶段 6 记录 PLT GOT `BRANCHIND` lowering。
  - 第 68 行：阶段 7 记录 `.plt.sec` smoke pattern。

实现说明：

- 没有改 `PcodeLowerer`。上一小块已经让 `BRANCHIND` 消费 `IndirectExternalCallTargets`。
- 本次只是让 `NativePltEntry::GotAddress` 也进入这张表。
- `__cxa_finalize` 的 PLT stub 和 GOT slot 现在共享同一个 LLVM callee 名字，不再生成 `_1`。
- 未命中 `NativePltEntry` 或外部 `GLOB_DAT` 的 `BRANCHIND` 仍走 `notdec_exit`。

验证：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
```

通过。

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-47
```

结果：

- `vsftpd`：ok，8s，confirmed 9，blocks 30，instr 80，xrefs 303，unresolved branch 1。
- `libuv`：ok，19s，confirmed 9，blocks 29，instr 85，xrefs 26，unresolved branch 0。
- `memcached`：ok，7s，confirmed 9，blocks 30，instr 80，xrefs 185，unresolved branch 1。

IR pattern：

```text
vsftpd.all-confirmed.ll: call void @getegid()
vsftpd.all-confirmed.ll: call void @SSL_get_error()
memcached.all-confirmed.ll: call void @SSL_CTX_use_PrivateKey_file()
memcached.all-confirmed.ll: call void @pthread_cond_signal()
```

`rg __cxa_finalize_1 /tmp/notdec-bin2llvm-bench2-smoke-20260521-47/*.all-confirmed.ll`
没有命中。stdout/stderr 文件为空。LLVM 22 `llvm-as` 和 `opt -passes=verify` 均通过。

性能：

- 本次只在规划 call target 时多维护一张符号名表，lowering 阶段查表次数不变。
- 三目标 smoke 总耗时约 34s，和上一块同口径结果接近，没有看到明显变慢。

评分：

- 实现效果：8/10。当前 Bench2 里自然出现的 `.plt.sec` `BRANCHIND` 已落到外部符号，重复 external 名字也被消除。
- 复杂度：2/10。只改 call target 规划，不新增 lowering 分支。
- 维护成本：2/10。以后如果 external symbol 需要带库名区分，只需要调整 `externalNameFor(...)` 的 key。
