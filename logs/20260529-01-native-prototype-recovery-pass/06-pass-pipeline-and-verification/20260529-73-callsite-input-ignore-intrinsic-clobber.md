# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

第 72 步后，`libuv` 还剩两个 `unsafe callsite input value`。其中 `uv_poll_stop -> notdec_native_17770` 的 callsite 前有 `llvm.ctpop.i8`，并且 call block 的前驱是一个条件分支块。当前 input fallback 用“遇到 call 就拒绝”和“前驱必须只有一个后继”控制风险，导致 caller 的 `RDI` 参数不能继续作为 callee input。

# Ghidra 对应实现

Ghidra 判断 call 对寄存器值的影响时，不是按“语法上有 CALL”一刀切，而是看 call spec 的 effect：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::hasEffect(...)`
  - `FuncCallSpecs::deriveInputMap(...)`
  - `FuncCallSpecs::buildInputFromTrials(...)`
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`

也就是说，只有会 clobber 目标 storage 的调用才应该阻断这个 storage 的当前值。LLVM intrinsic 是编译器内部纯计算/辅助操作，不是 native ABI 调用点，不能按 killedbycall 处理。

# native 侧复刻路线

本轮只补最明确的一小块：

1. callsite input 回看入口值时，遇到 LLVM intrinsic 不再认为它阻断寄存器值。
2. 如果唯一前驱有多个后继，但当前块确实是它的一个后继，且前驱里没有目标寄存器 store，就允许继续向入口回看。
3. 普通 direct/indirect call 仍保守阻断。
4. 后续再把这里改成完整的 `callClobbersRegister(...)`，按 callee metadata 和 ABI unaffected 判断。

# 判断标准

- 增加 IR 单测：caller 参数值和 callee call 之间插入 `llvm.ctpop` 和条件分支，callee rewrite 仍能使用 caller 参数。
- `uv_poll_stop -> notdec_native_17770` 应从 unsafe input 变成 rewritten。
- Bench2 selected smoke 通过，运行时间不能明显变差。

# 风险

风险较低。LLVM intrinsic 本身不是 native call，不应该改变 ABI register global。当前只放开 intrinsic，不放开普通 call。

# 实现记录

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:259` 修改 `hasCallInReverseRange(...)`，遇到 `callee->isIntrinsic()` 时跳过，不再把 LLVM intrinsic 当作会 clobber ABI register 的 call。
- `lib/passes/NativePrototypeRecovery.cpp:379` 修改 `callsiteInputValueBeforeCall(...)` 的唯一前驱链判断：前驱可以有多个后继，只要当前块确实是其中一个后继，就继续沿该边回看。
- `tests/native_prototype_recovery_test.cpp:2163` 增加 intrinsic + 条件分支前驱的 callsite input 测试，覆盖 `uv_poll_stop -> notdec_native_17770` 这类形状。

## 验证

```text
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery' -V
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
OUT_DIR=/tmp/notdec-bin2llvm-bench2-branch-input-smoke scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-branch-input-smoke
```

结果：

- `native_prototype_recovery_test` 通过。
- 全量 `ctest` 9/9 通过。
- Bench2 selected smoke 通过。
- metrics：
  - `vsftpd`: 85s，rewritten 135，skipped 101，不变。
  - `libuv`: 219s，rewritten 284 -> 285，skipped 287 -> 286。
  - `memcached`: 117s，rewritten 181，skipped 134，不变。
- unsafe input：
  - `notdec_native_17770` 已改为 rewritten。
  - `libuv` 还剩 `notdec_native_9e70` 一个 `unsafe callsite input value`。

性能：

- 本次只放宽 callsite input 的局部 CFG 回看，未新增全局分析。
- Bench2 同口径时间：`vsftpd` 85s 不变，`libuv` 220s -> 219s，`memcached` 117s 不变。没有明显性能下降。

## 评分

- 实现效果：8/10。解决一个真实 unsafe input，保留 `notdec_native_9e70` 的 PHI 形状待后续处理。
- 复杂度：5/10。改动仍在现有 callsite input 查找内，没有新数据结构。
- 维护成本：6/10。多后继前驱回看是合理的 CFG 局部语义，但后续最好和完整 register SSA 当前值查询合并。

更好的方案是用统一的“某条边上的寄存器当前 SSA 值”查询替代现在的反向扫描。本轮先覆盖真实 Bench2 case。
