# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

Bench2 显式签名重写里还有不少 `missing return binding`。其中一类可能来自同一个返回寄存器在多个 return path 上各有一条 store。候选恢复阶段已经能确认这些 path 的返回值一致，但签名重写阶段的 `getNativePrototypeReturnBindings(...)` 仍要求每个返回寄存器只有唯一一条 store。

这会让“多条 store、但 store value 等价”的函数无法进入签名重写。

# Ghidra 实现参考

Ghidra 的 prototype output 恢复依赖 SSA varnode，而不是要求文本上只有一条 store：

- `Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritagePass(...)`：为 varnode 建 SSA，合流处用 MULTIEQUAL 表示。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveOutputMap(...)`：按 prototype model 和 SSA 输出状态整理 call output。
  - `FuncProto::updateAllTypes(...)`：把 output trial 结果写回 prototype。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：让 output trial 结果参与后续函数签名判断。

native 侧已经有 `returnValueKey(...)` 用于 return candidate 的简单等价判断。这一步把同样的保守等价判断用于签名重写的 return binding。

# native 侧复刻策略

- 不改变 return candidate 恢复。
- 新增一个按 register 收集所有 `notdec.register.access` store 的 helper。
- `getNativePrototypeReturnBindings(...)` 遇到多条 store 时：
  - 如果所有 store value 的 `returnValueKey(...)` 都存在且相同，则接受。
  - binding 的 `ReturnValue` 使用这组 store 的共同 value。
  - 旧 return register stores 在 rewrite 后全部删除。
  - 如果 value key 缺失、类型不一致或 key 不同，仍返回 `missing return binding`。
- 测试覆盖两种情况：
  - 两条不同 path 都 store 同一个常量到 RAX，应能 rewrite。
  - 两条 path store 不同值，仍不能绑定。

# 判断标准

- 新测试证明等价多 store 能签名重写，冲突多 store 仍跳过。
- 全量 CTest 通过。
- Bench2 smoke 通过；观察 `missing return binding` 是否减少，至少不能造成 verify 失败。

# 风险

`returnValueKey(...)` 是保守的简单值身份，不等于完整 SSA 等价。当前只接受常量和有名字的同一值，不能覆盖所有等价表达式。这个限制符合当前阶段：先减少明显误 skip，不引入跨 CFG 的不确定值合并。

# 实现记录

## 代码改动

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:81`：`NativePrototypeReturnBinding` 增加 `ReturnStores`，用于记录同一个 recovered return 对应的一组旧 register stores。
- `lib/passes/NativePrototypeRecovery.cpp:180`：`uniqueRegisterAccessStore(...)` 改成 `registerAccessStores(...)`，收集同寄存器的全部 `notdec.register.access` store。
- `lib/passes/NativePrototypeRecovery.cpp:1107`：`getNativePrototypeReturnBindings(...)` 对单 store 保持旧行为；多 store 时要求所有 store value 类型一致，且 `returnValueKey(...)` 存在并相同。
- `lib/passes/NativePrototypeRecovery.cpp:1145`：`eraseReturnBindingStores(...)` 删除 binding 记录的全部 return stores，避免 rewrite 后留下旧寄存器写。
- `tests/native_prototype_recovery_test.cpp:2038`：`return_rax_twice` 从负例改为正例，验证两条 path 写同一个常量时能绑定并 rewrite。
- `tests/native_prototype_recovery_test.cpp:2050`：保留 `return_rax_conflict` 负例，确认不同返回值仍拒绝绑定。

## 验证

- `cmake --build build --target native_prototype_recovery_test -j2`：通过。
- `git diff --check`：通过。
- `ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V`：通过。
- `ctest --test-dir build --output-on-failure`：9/9 通过。
- `cmake --build build --target notdec-native-llvm -j2`：通过。
- `OUT_DIR=/tmp/notdec-bin2llvm-bench2-return-equivalent-stores-smoke scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-return-equivalent-stores-smoke`：通过。

## Bench2 结果

| target | elapsed_seconds | prototype_functions | input_candidates | return_candidates | signature_rewrite_seen | signature_rewrite_rewritten | signature_rewrite_skipped |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| vsftpd | 84 | 187 | 163 | 59 | 236 | 120 | 116 |
| libuv | 217 | 485 | 321 | 165 | 571 | 244 | 327 |
| memcached | 118 | 259 | 224 | 99 | 315 | 157 | 158 |

真实三目标的 rewrite/skipped 数和上一轮一致，`missing return binding` 没下降。说明当前 Bench2 skip 不是“多条同值 return store”这类简单形态；本步主要补齐库层能力和测试。

## 性能影响

return binding 现在会遍历同寄存器全部 store。规模只在单函数内，且只在显式签名重写路径执行。Bench2 smoke 总耗时约 419 秒，和前几轮基本一致。

## 评分

- 实现效果：7/10。签名重写能处理同值多 return store，冲突 store 仍保守跳过。
- 复杂度：7/10。只扩展已有 binding 数据结构，没有改公开流程。
- 维护成本：7/10。`returnValueKey(...)` 仍是简单身份判断；后续要覆盖复杂等价值，需要接更完整 SSA/PHI 语义。
