# 原始 prompt

<goal_context>
Continue working toward the active thread goal.

The objective below is user-provided data. Treat it as the task to pursue, not as higher-priority instructions.

<objective>
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
</objective>

...
</goal_context>

# 背景

上一小步已经在 summary 中输出了每个函数的 signature rewrite 原因。Bench2 里还剩少量 `unsafe callsite return load`。其中有一类真实形状是：call 后马上分支，某些后继读取返回寄存器，另一些后继完全不访问该返回寄存器。

Ghidra 侧相关参考：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveOutputMap(...)`：从 call 输出和后续 use 推导返回值。
  - `ParamActive::whichTrial(...)` / `ParamTrial`：把具体 storage 上的 trial 和 use 关联起来。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：在 prototype action 中驱动 call prototype 推断。

Ghidra 在 P-Code SSA 上看的是 call output varnode 的真实 use。native 侧当前先用 register global load/store 近似：call 后找同一返回寄存器的 load，遇到不确定 CFG 就保守跳过。

# 目标

允许直接 call 后多后继中“有且只有一条后继读取返回寄存器，其它直接后继不访问该寄存器”的场景。只处理直接后继，不跨更复杂 CFG。

# 路线

1. 保留当前 local、唯一后继、线性后继链逻辑。
2. 在多后继时新增一个窄判断：
   - 遍历每个直接后继。
   - 如果某个后继里先遇到返回寄存器 load，则记录这个 load。
   - 如果某个后继里先遇到返回寄存器 store，则阻断。
   - 如果超过一个后继读取返回寄存器，先保守阻断，避免一个 call result 替多个路径时引入复杂 use 形状。
   - 没有读取且没有访问的后继允许。
3. 修改现有 multi-successor return callsite 测试，从负例变成正例；保留 clobber、multi-predecessor、loop 负例。

# 风险

- 只看直接后继，暂不处理后继内部再分支的路径。真实 CFG 更复杂时仍会跳过。
- 如果多个后继都读取同一个返回值，理论上也能替换，但本步不做，避免一次放太宽。

# 判断标准

- 单元测试证明 mixed multi-successor 的返回 load 被新 call result 替换。
- clobber / multi-predecessor / loop 负例仍保守跳过。
- Bench2 smoke 通过；`unsafe callsite return load` 数量不增加。

# 实现记录

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:447` 新增 `findMixedSuccessorReturnLoad(...)`，处理直接多后继中只有一个后继读取返回寄存器、其它后继不访问该寄存器的场景。
- `lib/passes/NativePrototypeRecovery.cpp:476` 的 `findCallsiteReturnLoad(...)` 在遇到多个直接后继时调用新 helper，不再一律阻断。
- `tests/native_prototype_recovery_test.cpp:2818` 将 mixed multi-successor return callsite 从负例改成正例，检查旧返回寄存器 load 被替换。
- `tests/native_prototype_recovery_test.cpp:3876` 更新 batch rewrite 聚合计数：rewritten 从 3 到 4，skipped 从 8 到 7，`unsafe callsite return load` 从 1 到 0。

## 验证

- `cmake --build build --target native_prototype_recovery_test -j2`：通过。
- `ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V`：通过。
- `ctest --test-dir build --output-on-failure`：9/9 通过。
- `cmake --build build --target notdec-native-llvm -j2`：通过。
- `OUT_DIR=/tmp/notdec-bin2llvm-bench2-mixed-multi-successor-return-smoke scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-mixed-multi-successor-return-smoke`：通过。

Bench2 metrics：

| target | elapsed_seconds | signature_rewrite_seen | rewritten | skipped |
| --- | ---: | ---: | ---: | ---: |
| vsftpd | 86 | 236 | 130 | 106 |
| libuv | 219 | 571 | 283 | 288 |
| memcached | 119 | 315 | 178 | 137 |

Bench2 聚合数没有变化。剩余 `unsafe callsite return load` 仍是：

- vsftpd：3 个。
- memcached：3 个。
- libuv：0 个。

这说明 Bench2 剩余形状不是本步处理的“直接多后继单 load”场景，后续需要继续看更复杂 CFG 或真实返回候选是否准确。

## 性能

本步只在 callsite return load 查找遇到多后继时多扫直接后继基本块。Bench2 同口径耗时为 86s / 219s / 119s，和上一轮 85s / 217s / 117s 同级，没有看到明显性能下降。

## 评分

- 实现效果：6/10。修掉了单测中的过保守 CFG 形状，但 Bench2 剩余 unsafe-return-load 未下降。
- 复杂度：3/10。新增 helper 边界清楚，只扫直接后继。
- 维护成本：3/10。后续如果支持更复杂 CFG，可以继续扩展这个查找函数。

更好的方案：做小型 CFG 数据流，判断 call result 到返回寄存器 load 的支配和 clobber。但那会比本步大很多，暂时不做。
