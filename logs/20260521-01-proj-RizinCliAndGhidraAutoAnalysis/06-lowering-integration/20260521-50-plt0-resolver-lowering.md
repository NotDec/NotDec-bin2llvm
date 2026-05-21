# PLT0 Resolver Lowering

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

上一小块后，Bench2 三目标已经没有 pcode call helper、没有 unresolved indirect flow，也没有
direct `ram` poison read。剩下一个明显的 lowering 缺口：`vsftpd` 和 `memcached` 的 PLT0
resolver block 仍然生成匿名 `notdec_exit`：

```text
define void @notdec_native_5020() {
bb_5020:
  ...
  br label %notdec_exit
notdec_exit:
  ret void
}
```

discovery 侧已经把这条 `BRANCHIND` 识别为 `sleigh-pcode-plt0-resolver-branch`，并且不再计入
unresolved。lowering 侧还没有消费这类 resolver 事实。

## Ghidra 相关实现

Ghidra 不会把 PLT0 当普通内部函数精确展开，而是用 ELF relocation / thunk / external
symbol 信息解释动态链接入口：

- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/ELFAnalyzer.java`
  - ELF 分析阶段会处理 dynamic table、relocation、PLT/GOT 相关信息。
- `Ghidra/Features/Base/src/main/java/ghidra/app/cmd/function/CreateThunkFunctionCmd.java`
  - thunk 创建逻辑把“跳到别处”的小函数标成 thunk，而不是强行当普通函数体解释。
- `Ghidra/Processors/x86/data/languages/ia.sinc`
  - x86 `JMP rm64` 会翻译成间接跳转 P-Code，也就是 native 侧看到的 `BRANCHIND`。

native 侧不做完整 thunk 数据库；这次只复刻 Bench2 里已经验证过的 PLT0 resolver 子集。

## native 侧复刻策略

1. 在 `notdec-native-llvm` 规划 call target 时，根据 section 判断 `.plt + 6` 对 `.got + 16`
   的 resolver slot。
2. 把 `.got + 16` 放进现有 `IndirectExternalCallTargets`，符号名用固定的
   `notdec_plt0_resolver`。
3. `PcodeLowerer::BRANCHIND` 已经会用 input 来源查 `IndirectExternalCallTargets`，因此不用新增
   P-Code lowering 分支。
4. Bench2 smoke 增加检查：PLT0 resolver 不应再生成 `notdec_exit`，并且应出现
   `call void @notdec_plt0_resolver()`。

暂时不做：

- 不模拟动态链接器真实 resolver ABI。
- 不把普通未知 `BRANCHIND` 改成 resolver。
- 不处理 jump table。
- 不改变外部函数 prototype，仍按当前 `void ()` 骨架表达 tail jump。

## 判断标准

1. `vsftpd` / `memcached` 的 `notdec_native_5020` 不再包含 `notdec_exit`。
2. `vsftpd` / `memcached` IR 出现 `call void @notdec_plt0_resolver()`。
3. `libuv` 不受影响。
4. Bench2 smoke 和 LLVM 22 verify 继续通过。
5. 性能不明显变慢。

## 风险

1. `notdec_plt0_resolver` 只是 native lowering 的占位外部符号，不是精确 ABI。
2. 不同 ELF 布局可能没有 `.plt + 6` / `.got + 16` 模式；没有命中时仍保持原行为。
3. 这只减少匿名退出点，不代表完整 PLT lazy binding 语义已经恢复。

## 实现记录

### 改动

- `tools/notdec-native-llvm.cpp:305` 新增 `sectionByName(...)`，用于在 LLVM target 规划阶段查
  `.plt` / `.got`。
- `tools/notdec-native-llvm.cpp:350` 的 `planNativeCallTargets(...)`
  - 如果同时存在 `.plt` 和 `.got`，把 `.got + 16` 加入 `IndirectExternalCallTargets`。
  - 符号名使用 `notdec_plt0_resolver`，后续仍由已有 `BRANCHIND` external tail jump lowering 消费。
- `scripts/bench2-native-smoke.sh:139` 和 `scripts/bench2-native-smoke.sh:167`
  - 要求 `vsftpd` / `memcached` 生成 `call void @notdec_plt0_resolver()`。
- `scripts/bench2-native-smoke.sh:180`
  - 禁止当前三目标 all-confirmed IR 重新出现 `notdec_exit`。
- `ARCHITECTURE.md:139` 和 `ARCHITECTURE.md:145` 记录 PLT0 resolver lowering 和 smoke 回归检查。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:59` 和
  `PROGRESS.md:74` 更新阶段 6 / 阶段 7 进度。

### 验证

格式检查：

```bash
git diff --check
```

结果：通过。

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
```

结果：通过。

Bench2 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-50
```

结果：

```text
vsftpd ok elapsed=8s
libuv ok elapsed=19s
memcached ok elapsed=7s
```

summary unresolved indirect flow：

```text
libuv.summary.json unresolved total 0
memcached.summary.json unresolved total 0
vsftpd.summary.json unresolved total 0
```

PLT0 resolver lowering 结果：

```text
vsftpd.all-confirmed.ll: call void @notdec_plt0_resolver()
memcached.all-confirmed.ll: call void @notdec_plt0_resolver()
```

`rg "notdec_exit"` 在三个 all-confirmed IR 中无命中，smoke 的禁止规则也已覆盖。

性能：本次 smoke 合计约 34s，三个目标分别约 8s / 19s / 7s。新增逻辑只是 target 规划时查
两个 section 和插入一个 map 项，没有看到 smoke 时间明显变慢。

### 评分

- 实现效果：7/10。当前 Bench2 里的 PLT0 resolver 不再落到匿名 `notdec_exit`。
- 复杂度：2/10。复用已有 `IndirectExternalCallTargets` 和 `BRANCHIND` lowering。
- 维护成本：2/10。规则很窄，后续做完整 PLT/ABI 时可以替换这个 synthetic symbol。

### 未做

- 没有模拟动态链接器 resolver ABI。
- 没有处理其他未知 `BRANCHIND`。
- 没有做 jump table。
