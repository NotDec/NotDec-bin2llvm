# 100. callsite return 复杂后继中证明返回值未使用

原始 prompt：

> 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass。首先将相关的数据结构划分为几个工作量大约相等的部分（目前应该差不多完成了，主要是在数据集上测试并进一步提升），其次，规划一下如何做测试和验证。把进度记录到PROGRESS.md里面。依然要求，
>
> 根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：
>
> - 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
> - 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
> - 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
>
> 在数据集上测试并进一步提升时，因为不涉及实现具体的模块或数据结构，可以不需要遵守上面的功能复刻流程。
>
> 1. 每一轮先看 Bench2 当前 skip reason 和真实函数样本，再决定实现点。
> 2. 先按 Ghidra 数据结构和 Bench2 blocker 识别“大块能力”，再从大块能力里切小步。小步服务于大块任务，不允许小步自己变成主线。
> 3. 每次实现不应以“一个极小 CFG 变体”作为默认粒度。应把同一类语义问题合并成一个小阶段处理，同类问题多修复一些再合并commit。

## 背景

第 99 步后，`lighttpd` 还剩 5 个 `unsafe callsite return load`：

- `notdec_native_f558`
- `buffer_extend`
- `notdec_native_1c6d8`
- `http_method_buf`
- `http_request_parse_target`

这些属于同一块能力：callsite return 重写时，需要判断旧返回寄存器在 call 后是否真的被读取。当前 helper 在多后继、shared successor、环形 CFG 上已经支持一部分形状，但遇到复杂后继或循环时仍偏保守，不能证明“所有路径都没有读取返回寄存器”。

候选大块任务：

1. callsite return load 查找和未使用返回值证明。
2. 统一 register current-value 查询。
3. 扩展更多 Bench2 目标，继续归类 blocker。

本轮选择第 1 块。原因是当前真实 blocker 全部集中在 `unsafe callsite return load`，并且 Ghidra 对返回 trial 的判断本质上也是看 output storage 是否有真实 use。

## Ghidra 对应实现

Ghidra 不会因为 CFG 复杂就默认认为返回值被使用，而是基于 P-Code SSA 和 trial use 情况判断 output storage 是否 active：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveOutputMap(...)`
  - `FuncCallSpecs::buildOutputFromTrials(...)`
  - `FuncCallSpecs::hasEffect(...)`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/paramid.cc`
  - `ParamActive::registerTrial(...)`
  - `ParamActive::whichTrial(...)`
  - `ParamTrial::testShrink(...)`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritage(...)`
  - `HeritageInfo::buildInfoList(...)`

核心对应关系是：Ghidra 看 call 后 output varnode 是否流向真实 use；如果先被覆盖，或者所有可达路径都没有读取这个 storage，则这个 callsite 的返回值可以视为未使用。

## native 侧计划

当前 native 侧的 `findCallsiteReturnLoad(...)` 负责找旧 IR 里的返回寄存器 load。它已经能处理：

- call 后同 block 的 load；
- 唯一后继链；
- 同 register store 或 clobbering call 先出现时按未使用处理；
- shared successor 中的返回 load PHI 重写；
- 简单多后继中只有一个后继读返回值。

本轮补同一类能力，不再补单个函数特判：

1. 把 “shared successor 没有直接 load 后继续检查后续路径” 改成 worklist CFG 检查。
2. 如果某条路径先遇到返回寄存器 load，仍然判 unsafe。
3. 如果某条路径先遇到同寄存器 store、clobbering call、return，或只在无 load 的环里循环，则认为该路径未使用返回值。
4. 多后继中没有直接 load 的分支，也用同一套 unused-return CFG 检查。
5. 保留已有 shared successor load PHI 重写，不扩大到无法证明支配关系的 load。

## 判断标准

- 单测覆盖 call 后进入无返回寄存器 load 的 loop 时允许 rewrite。
- 原有 unsafe load / shared successor 负例继续通过。
- `native_prototype_recovery_test` 通过。
- `lighttpd` 的 `unsafe callsite return load` 数量下降。
- LLVM 22 `llvm-as` / `opt -passes=verify` 通过。

## 风险

风险是把真实使用的返回寄存器误判成未使用。因此本轮只在 CFG 检查能看到所有可达 block，且没有任何路径先读目标 register 时才放行。遇到 load 仍阻断；遇到 shared successor 里的 load 仍走原来的 PHI 重写路径。

## 实现记录

已实现。

### 改动

- `lib/passes/NativePrototypeRecovery.cpp`
  - 新增 `findDominatedSuccessorReturnLoad(...)`，在 call 后多后继的单支配子图里继续查找返回寄存器 load。
  - 新增 `hasUnvisitedPredecessor(...)`，避免把有外部前驱的 shared block 当成可直接替换的 dominated load。
  - 调整 `findMixedSuccessorReturnLoad(...)`：
    - 直接后继没有返回 load 时，继续查找后续 dominated successor。
    - 如果 dominated 查找不能证明安全，再用 unused-return CFG 检查兜底。
  - 调整 `findSharedSuccessorUnusedReturn(...)`：从递归 active-stack 改为 worklist visited 检查，允许无返回寄存器 load 的环。
  - 调整 `findCallsiteReturnLoad(...)`：唯一后继链遇到无返回寄存器 load 的环时，尝试证明返回值未使用，而不是直接 blocked。
- `tests/native_prototype_recovery_test.cpp`
  - 新增 nested successor 返回 load 测试。
  - 将无返回 load 的 loop callsite 从负例改为正例。
  - 删除已经不用的 `expectReturnOnlyRewriteRejected` helper。

### 验证

- `git diff --check`：通过。
- `cmake --build /tmp/notdec-bin2llvm-build --target native_prototype_recovery_test -j$(nproc)`：通过。
- `ctest --test-dir /tmp/notdec-bin2llvm-build -R 'notdec.native_prototype_recovery' --output-on-failure`：通过。
- `cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm -j$(nproc)`：通过。
- `ctest --test-dir /tmp/notdec-bin2llvm-build --output-on-failure`：9/9 通过。
- `lighttpd` signature rewrite：
  - 输出目录：`/tmp/notdec-bin2llvm-bench2-lighttpd-return-unused-cfg-v3`
  - rewritten：697 -> 702。
  - skipped：270 -> 265。
  - `unsafe callsite return load`：5 -> 0。
  - `unsafe callsite input value`：保持 0。
  - 剩余 skip reason 只有 `already matches` 191、`declaration` 74。
  - `notdec_native_f558`、`buffer_extend`、`notdec_native_1c6d8`、`http_method_buf`、`http_request_parse_target` 都已 rewrite。
  - `llvm-as` / `opt -passes=verify` stderr 都是 0 字节。
- Bench2 旧三目标 smoke：
  - 输出目录：`/tmp/notdec-bin2llvm-bench2-return-unused-cfg-smoke`
  - `vsftpd`：86s，rewritten 139，skipped 97。
  - `libuv`：225s，rewritten 338，skipped 233。
  - `memcached`：121s，rewritten 188，skipped 127。
  - 三个目标的 rewrite 指标和上一轮一致，skip reason 仍只有 `already matches` / `declaration`。
  - signature rewrite 的 `llvm-as` / `opt -passes=verify` stderr 都是 0 字节。

### 性能对比

上一轮旧三目标 smoke 是：

- `vsftpd`：87s。
- `libuv`：223s。
- `memcached`：120s。

本轮是 86s / 225s / 121s，变化很小。`lighttpd` signature rewrite 约 4 分钟级别，主要来自更大的目标和 return CFG 检查；当前没有看到旧三目标性能回退。

### 评价

- 实现效果：5/5。清零 lighttpd 当前所有非合理 signature rewrite skip reason。
- 复杂度：3/5。新增两个 CFG helper，但仍集中在 callsite return 查找逻辑里。
- 维护成本：3/5。逻辑比之前更强，但后续最好把 callsite rewrite 计划统一成显式数据结构，避免 helper 继续分散增长。
