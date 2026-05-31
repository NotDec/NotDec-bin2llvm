# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

最新 Bench2 smoke 里，`vsftpd` 和 `libuv` 还剩少量 `unsafe callsite input value`。抽查发现一类形状不是 callee 参数候选本身缺少 use，而是 caller 已经先被签名重写：原来的 `@RDI` / `@RSI` 入口值变成了 LLVM 函数参数，callsite 前不再有显式 register store。当前 `callsiteInputValueBeforeCall(...)` 只找 register store，所以会误判缺少参数值。

# Ghidra 对应实现

Ghidra 里 callsite 参数不是只看 call 前有没有一次寄存器写入，而是围绕 SSA varnode 和 active trials 判断当前调用点的输入 varnode：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`
  - `FuncCallSpecs::buildInputFromTrials(...)`
  - `FuncCallSpecs::hasEffect(...)`
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`

这些逻辑的关键点是：prototype trial 绑定的是当前 SSA 值。函数入口 input varnode 如果一路没有被覆盖，到 call 点仍然是同一个值；它不要求 call 前必须再写一次 ABI 寄存器。

# native 侧复刻路线

当前 native 侧已经把 callee-first rewrite 接上。callee rewrite 时，caller 可能还没重写，入口寄存器值仍是 `notdec.register.external_input` load；也可能 caller 已经在其它路径里重写，入口寄存器值已经变成 LLVM argument。下一步只补这两个同源形状：

1. `callsiteInputValueBeforeCall(...)` 仍优先使用 call 前显式 register store。
2. 沿唯一前驱链找不到 store 时，先尝试使用 caller 已匹配 recovered prototype 的同名 LLVM argument。
3. 如果 caller 还没重写，再尝试使用 caller 的唯一同名 `notdec.register.external_input` load。
4. 如果从 callsite 回看入口过程中遇到中间 call，先保守拒绝。因为 caller-saved register 可能已经被 clobber。
5. 多前驱复杂形状暂不做；仍使用已有的 equivalent predecessor store 逻辑。

这一步不把 `RBX` / `R12` 之类 callee-saved register 当作 ABI 参数来源，避免掩盖错误 prototype。

# 判断标准

- 增加 IR 单测：caller 已经是 `void(i64)`，callee 仍是 `void()`，call 前没有 `store @RDI`，rewrite callee 时能用 caller 的 `%RDI.external_input` 作为参数。
- 原有 missing input callsite 仍然失败。
- Bench2 smoke 中 `unsafe callsite input value` 数量应下降，至少不能新增 unsafe return/input 类错误。

# 风险

主要风险是把已经被中间 call clobber 的入口寄存器参数继续传给后续 callee。第一版用“路径上遇到 call 就不 fallback”控制风险，后续再按 `FuncCallSpecs::hasEffect(...)` 细化。

# 实现记录

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:259` 新增 `hasCallInReverseRange(...)`，用于判断从 callsite 回看到入口值的路径上是否遇到中间 call。
- `lib/passes/NativePrototypeRecovery.cpp:269` 新增 `functionArgumentForRecoveredInput(...)`，当 caller 已经匹配 recovered prototype 时，按 register name 找到对应 LLVM argument。
- `lib/passes/NativePrototypeRecovery.cpp:295` 新增 `functionEntryValueForRegister(...)`，统一返回 caller 的当前入口寄存器值：优先 recovered argument，其次唯一 external input load。
- `lib/passes/NativePrototypeRecovery.cpp:335` 修改 `callsiteInputValueBeforeCall(...)`，在显式 store 和唯一前驱链都没有找到值、且没有中间 call 时，使用 caller 入口寄存器值。
- `tests/native_prototype_recovery_test.cpp:2058` 增加 caller 尚未重写时使用 external input load 的 direct callsite 测试。
- `tests/native_prototype_recovery_test.cpp:2108` 增加 caller 已经重写时使用 LLVM argument 的 direct callsite 测试。

## 验证

```text
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery' -V
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
OUT_DIR=/tmp/notdec-bin2llvm-bench2-entry-input-smoke scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-entry-input-smoke
```

结果：

- `native_prototype_recovery_test` 通过。
- 全量 `ctest` 9/9 通过。
- Bench2 selected smoke 通过。
- metrics：
  - `vsftpd`: 85s，rewritten 133 -> 135，skipped 103 -> 101。
  - `libuv`: 220s，rewritten 283 -> 284，skipped 288 -> 287。
  - `memcached`: 117s，rewritten 181，skipped 134，不变。
- unsafe input：
  - `vsftpd` 的 `notdec_native_91c0`、`notdec_native_18470` 已改为 rewritten。
  - `libuv` 的 `uv_is_active` 已改为 rewritten。
  - `libuv` 仍剩 `notdec_native_9e70`、`notdec_native_17770` 两个 `unsafe callsite input value`。

性能：

- 本次只增加 callsite rewrite 前的局部反向扫描和入口值 fallback。
- Bench2 同口径时间：`vsftpd` 85s 不变，`libuv` 218s -> 220s，`memcached` 118s -> 117s。没有明显性能下降。

## 评分

- 实现效果：8/10。解决 3 个真实 unsafe input，剩余两个需要进一步分析更复杂 callsite。
- 复杂度：6/10。新增逻辑在现有 `callsiteInputValueBeforeCall(...)` 内部，没有引入新文件；但又增加了一种值来源，需要后续和 call effect 统一。
- 维护成本：6/10。当前“遇到中间 call 就拒绝”保守但简单，后续可以替换成基于 `FuncCallSpecs::hasEffect(...)` 的寄存器级判断。

更好的方案是把 callsite input lookup 改成统一的 SSA 当前寄存器值查询，显式处理 store、entry input、PHI 和 call effect。本次先做最小可验证部分。
