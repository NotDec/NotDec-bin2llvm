# 99. callsite input 模糊前驱时读取当前 register global

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

第 98 步把 Bench2 样本扩到 `lighttpd` 后，发现 7 个 `unsafe callsite input value`：

- `log_perror`
- `log_debug`
- `log_error`
- `buffer_extend`
- `notdec_native_3acd0`
- `http_chunk_append_file_ref_range`
- `http_chunk_append_file_fd_range`

旧三目标已经没有这类 blocker，所以本轮不再补单个 CFG 变体，而是处理 lighttpd 暴露的一类真实问题：callsite input 当前寄存器值在多前驱或不完整前驱下不能唯一静态还原。

## Ghidra 对应实现

Ghidra 的 call input 不是只看 call 前最近一条写寄存器指令，而是看 call 点处 storage 的当前 SSA 值：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`
  - `FuncCallSpecs::buildInputFromTrials(...)`
  - `FuncCallSpecs::hasEffect(...)`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritage(...)`
  - `HeritageInfo::buildInfoList(...)`

在 Ghidra 里，如果多个前驱给同一个 register varnode 赋值，heritage 会用 MULTIEQUAL 表达当前值。native LLVM 目前没有统一的“call 点 register current value”接口，只能从 store、PHI、entry value 和 register global 里恢复。

## native 侧计划

当前 `callsiteInputValueBeforeCall(...)` 已经支持：

- 当前块 call 前 register store；
- 唯一前驱链中的 register store；
- register SSA PHI；
- caller 入口 external input；
- 完全缺失时在 call 前 load register global。

但如果遇到多前驱，当前逻辑要求每个前驱都能找到同一个等价值。lighttpd 的真实样本里，经常是多前驱或某些前驱没有显式 store。旧 IR 的 callee 入口本来会 `load @R8/@R9/@RSI/...`，所以在 rewrite 后把这个 load 移到 callsite 前，是更接近旧语义的保守做法。

本轮做最小同类修复：

1. 多前驱无法得到等价 store 时，不直接判 unsafe。
2. 改为在旧 call 前插入目标 register global load，作为 typed call 参数。
3. loop/复杂前驱链最终无法唯一推出值时，也 fallback 到 call 前 register global load。
4. 原有明确 store、PHI、entry value 仍优先使用，避免不必要的 global load。
5. 仍要求 module 中能唯一找到目标 register global，找不到或重复时继续 unsafe。

## 判断标准

- 单测覆盖“冲突前驱 store 不再失败，而是在 call 前 load register global”。
- `native_prototype_recovery_test` 通过。
- `lighttpd` signature rewrite 的 `unsafe callsite input value` 数量下降。
- 不引入 LLVM verify 失败。

## 风险

风险是把本该精确表达的 SSA 值退回 register global load。这里的判断是：当我们无法精确表达当前值时，读取 call 点当前 register global 比跳过 rewrite 更接近旧 IR，因为旧 callee 入口也是从同一个 global 读取 ABI input。后续更好的方案是给 `NativeRegisterSSA` 暴露统一的 current-value 查询接口，本轮先只解决 lighttpd 暴露的真实 blocker。

## 实现记录

已实现。

### 改动

- `lib/passes/NativePrototypeRecovery.cpp`
  - `callsiteInputValueBeforeCall(...)`：
    - 多前驱先继续尝试 `equivalentInputValueFromPredecessors(...)`。
    - 如果多前驱值不等价，改为在 call 前通过 `registerGlobalValueBeforeCall(...)` load 当前 register global。
    - 前驱回看循环达到深度上限后，也改为 fallback 到 call 前 register global load。
- `tests/native_prototype_recovery_test.cpp`
  - 调整 conflicting predecessor callsite 测试：现在要求 rewrite 成功，并检查 call 参数来自对应 register global load。

### 验证

- `git diff --check`：通过。
- `cmake --build /tmp/notdec-bin2llvm-build --target native_prototype_recovery_test -j$(nproc)`：通过。
- `ctest --test-dir /tmp/notdec-bin2llvm-build -R 'notdec.native_prototype_recovery' --output-on-failure`：通过。
- `cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm -j$(nproc)`：通过。
- `ctest --test-dir /tmp/notdec-bin2llvm-build --output-on-failure`：9/9 通过。
- `lighttpd` signature rewrite：
  - 输出目录：`/tmp/notdec-bin2llvm-bench2-lighttpd-input-global-fallback`
  - `unsafe callsite input value`：7 -> 0。
  - rewritten：691 -> 697。
  - skipped：276 -> 270。
  - 剩余 skip reason：`already matches` 191、`declaration` 74、`unsafe callsite return load` 5。
  - `llvm-as` / `opt -passes=verify` stderr 都是 0 字节。
- Bench2 旧三目标 smoke：
  - 输出目录：`/tmp/notdec-bin2llvm-bench2-input-ambiguous-global-smoke`
  - `vsftpd`：87s，rewritten 139，skipped 97。
  - `libuv`：223s，rewritten 338，skipped 233。
  - `memcached`：120s，rewritten 188，skipped 127。
  - 三个目标的 signature rewrite 指标和上一轮一致，未新增 skip reason。
  - signature rewrite 的 `llvm-as` / `opt -passes=verify` stderr 都是 0 字节。

### 评价

- 实现效果：4/5。解决了 lighttpd 暴露的同类 callsite input blocker，旧三目标不回退。
- 复杂度：2/5。只调整一个 fallback 分支，没有新增数据结构。
- 维护成本：2/5。当前做法清楚，但长期仍应把 register current-value 查询下沉到 `NativeRegisterSSA`。
