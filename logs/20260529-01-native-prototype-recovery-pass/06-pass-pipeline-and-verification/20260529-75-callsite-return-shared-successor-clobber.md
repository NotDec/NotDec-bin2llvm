# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

第 74 步后，Bench2 当前关注用例只剩 `libuv.notdec_native_9e70` 的
`unsafe callsite return load`。真实形状有两个 callsite：

```llvm
call void @notdec_native_9e70()
br label %bb_b9ba

bb_b9ba: ; 多前驱
  ...
  br i1 ..., label %bb_b9c3, label %bb_b9d5
```

以及：

```llvm
call void @notdec_native_9e70()
br label %bb_b987

bb_b987: ; 多前驱
  store ..., ptr @RAX
  %RAX170 = load i64, ptr @RAX
```

这两处 successor 都有多个前驱。当前 `findCallsiteReturnLoad(...)` 遇到
multi-pred successor，如果该块还有后继，就直接判 unsafe。这里过严：
`bb_b987` 先覆盖 `RAX`，后面的 load 读的是覆盖后的值，不是 call 返回值；
`bb_b9ba` 本块也先写其它寄存器并继续分支，后继路径里 `RAX` 也是先被覆盖。

# Ghidra 对应实现

Ghidra prototype recovery 不会只因为 CFG join 就保留一个 call output。它会看
call output varnode 后续是否还能作为当前 call 的值被使用：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveOutputMap(...)`
  - `FuncCallSpecs::buildOutputFromTrials(...)`
  - `FuncCallSpecs::hasEffect(...)`
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`
- `Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritage(...)`

对应到 native 侧：如果 call 后的返回寄存器在任何读取前被 store 或 clobber 覆盖，
这个 call 的返回值就是未使用，不应该阻止签名 rewrite。

# native 侧复刻路线

本轮只处理当前真实 case 需要的窄形状：

1. 保留已有规则：multi-pred successor 里直接读返回寄存器仍然 unsafe。
2. 如果 multi-pred successor 里先覆盖返回寄存器，按返回值未使用处理。
3. 如果 successor 本块没有触碰返回寄存器，就继续看后继；所有路径都必须在读取前覆盖返回寄存器，或者不使用返回寄存器。
4. 遇到循环、缺 terminator 或任一路径先读返回寄存器，继续保守拒绝。

这不是完整支配关系分析，只是把“返回寄存器先被覆盖”这个 Ghidra 会自然识别的
SSA 事实补上。

# 判断标准

- 增加单测：target call 后跳到 shared successor，successor 先 store `RAX`，再 load
  `RAX`；rewrite 应成功，旧 load 不能被替换成 call 结果。
- `libuv.notdec_native_9e70` 不再因为 `unsafe callsite return load` 跳过。
- Bench2 selected smoke 通过，运行时间不能明显变差。

# 风险

风险是把 join 后来自其它前驱的返回寄存器使用误判为当前 call 的返回值未使用。
本轮只在 first touch 是 store/clobber 时放行；如果是 load 仍然 unsafe。如果本块
没有 first touch，会继续检查所有后继路径；遇到循环或任一路径先 load 就保守拒绝。

# 实现记录

已完成。

## 实现调整

计划里原本写“只允许继续走唯一后继”，但真实 `bb_b9ba` 后面是两路分支，两路都先覆盖
`RAX`。实现时改为“所有后继路径都不能先读返回寄存器”。这仍然不是完整支配分析；
遇到循环和任一路径先读返回寄存器都拒绝。

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:697` 新增 `findSharedSuccessorUnusedReturn(...)`，从 shared successor 开始沿 CFG 检查所有后继路径；如果先遇到返回寄存器 load，则判 unsafe；如果先遇到 store/call clobber，则按返回值未使用处理；遇到循环保守拒绝。
- `lib/passes/NativePrototypeRecovery.cpp:778` 修改 `findCallsiteReturnLoad(...)` 的 multi-pred successor 分支：shared successor 没有直接 load 时，调用新的路径检查，不再只因后面还有分支就阻断。
- `tests/native_prototype_recovery_test.cpp:603` 新增 `createClobberReturnSharedSuccessorCallerFunction(...)`，构造 call 后进入 shared successor，successor 先覆盖 `RAX` 再读取 `RAX` 的形状。
- `tests/native_prototype_recovery_test.cpp:3437` 增加单测：clobbered shared successor callsite 应允许 return-only rewrite，且旧 `RAX` load 不能被替换成 call 返回值。

## 验证

```text
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery' -V
ctest --test-dir build --output-on-failure
cmake --build build --target notdec-native-llvm -j2
OUT_DIR=/tmp/notdec-bin2llvm-bench2-shared-successor-clobber-smoke2 scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-shared-successor-clobber-smoke2
```

结果：

- `native_prototype_recovery_test` 通过。
- 全量 `ctest` 9/9 通过。
- Bench2 selected smoke 通过。
- metrics：
  - `vsftpd`: 84s，rewritten 135，skipped 101，不变。
  - `libuv`: 219s，rewritten 286，skipped 285；`notdec_native_9e70` 已 rewritten。
  - `memcached`: 118s，rewritten 181，skipped 134，不变。
- skip reason：
  - 三个目标的 `unsafe callsite return load` 已清零。
  - `libuv` 只剩 `declaration: 86` 和 `missing recovered prototype: 199`。

性能：

- 本次只在 shared successor return 判定里做 CFG 后继检查，遇到循环立即拒绝。
- Bench2 同口径时间：`vsftpd` 85s -> 84s，`libuv` 218s -> 219s，`memcached` 118s 不变。没有明显性能下降。

## 评分

- 实现效果：8/10。清掉 `libuv.notdec_native_9e70` 的 return blocker，当前关注三个目标不再有 unsafe callsite return/input reason。
- 复杂度：6/10。递归看 shared successor 后继，比原来多一点 CFG 逻辑，但范围只在 return callsite 检查里。
- 维护成本：6/10。后续最好用统一的寄存器 SSA 当前值/支配查询替代这里的局部 CFG 扫描。

更完整的方案是做基于 DominatorTree 的 call result 使用归属判断。本轮按真实 case 先补“所有路径先覆盖返回寄存器”的窄形状。
