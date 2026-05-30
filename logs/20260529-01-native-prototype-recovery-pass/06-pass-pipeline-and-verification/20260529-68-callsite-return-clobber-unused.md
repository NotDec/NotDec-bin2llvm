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

Bench2 剩余 `unsafe callsite return load` 中，有一些调用点在 call 之后先写返回寄存器，再没有读取原返回值。例如 memcached 中 `notdec_native_f3f0` 的一个调用点：

```llvm
call void @notdec_native_f3f0()
store i64 3, ptr @RAX, ...
```

当前 `findCallsiteReturnLoad(...)` 把这种“先遇到返回寄存器 store”记为 blocked，导致整函数签名重写跳过。但从 Ghidra 的角度看，prototype 输出 trial 需要有真实 use；call 后返回 storage 被覆盖，说明这个调用点不使用 callee 返回值。

Ghidra 侧相关参考：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveOutputMap(...)`：从 call output varnode 的 use 推导返回值。
  - `FuncCallSpecs::buildOutputFromTrials(...)`：根据确认 trial 写回输出 prototype。
  - `ParamActive::whichTrial(...)`：根据 storage 找 trial。
- `Ghidra/Features/Decompiler/src/decompile/cpp/varnode.cc`
  - `Varnode::hasNoDescend(...)` / descend 遍历：判断值是否有真实使用。

# 目标

callsite return load 查找时，把“call 后先遇到同一返回寄存器 store，且之前没有 load”视为返回值未使用，而不是 unsafe。复杂 CFG、多个可能 load、路径不确定仍继续保守 blocked。

# 路线

1. 扩展 `ReturnLoadSearchResult`，区分：
   - 找到 load。
   - 先遇到 store，表示返回值在该路径被覆盖、未使用。
   - CFG 不确定或多个 load，仍 blocked。
2. `findReturnLoadBeforeStoreInRange(...)` 遇到 store 返回 `Clobbered`，不再直接当 blocked。
3. `findCallsiteReturnLoad(...)` 在 local / 唯一后继 / 线性链里遇到 `Clobbered` 时返回“无 load、非 blocked”。
4. 保留现有 clobber 负例需要调整：原来“先 store 再 load”的测试不应再阻止 rewrite，因为 load 读的是 store 后的新值，不是 call result。新增或保留复杂 CFG blocked 负例。

# 风险

- 如果后续还想追踪 store 后 load 的新值来源，这不属于 call return rewrite 的职责，本步不做。
- 多路径里有的路径 clobber、有的路径 load 的复杂情况暂不放宽，避免错误替换。

# 判断标准

- 单测覆盖 call 后先 clobber 返回寄存器再 load 的场景：rewrite 应通过，旧 load 不应被替换。
- 现有 multi-predecessor / loop 负例仍通过。
- Bench2 smoke 通过，并观察剩余 `unsafe callsite return load` 是否减少。

# 实现记录

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:392` 在 `ReturnLoadSearchResult` 增加 `Clobbered`，区分返回寄存器先被覆盖和 CFG 不确定。
- `lib/passes/NativePrototypeRecovery.cpp:427` 的 `findReturnLoadBeforeStoreInRange(...)` 遇到同一返回寄存器 store 时返回 `Clobbered`，不再直接 `Blocked`。
- `lib/passes/NativePrototypeRecovery.cpp:494` 的 `findCallsiteReturnLoad(...)` 把 `Clobbered` 当作“没有可替换的返回 load”，允许 return-only callsite 保留 unused call result。
- `lib/passes/NativePrototypeRecovery.cpp:463` 的 `findMixedSuccessorReturnLoad(...)` 仍把多后继内的 clobber 当 blocked，保持本步范围窄。
- `tests/native_prototype_recovery_test.cpp:2814` 将 call 后先 clobber 再 load 的测试改为正例：rewrite 应通过，但旧 load 不能被替换。

## 验证

- `cmake --build build --target native_prototype_recovery_test -j2`：通过。
- `ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V`：通过。
- `ctest --test-dir build --output-on-failure`：9/9 通过。
- `cmake --build build --target notdec-native-llvm -j2`：通过。
- `OUT_DIR=/tmp/notdec-bin2llvm-bench2-clobbered-return-unused-smoke scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-clobbered-return-unused-smoke`：通过。

Bench2 metrics：

| target | elapsed_seconds | signature_rewrite_seen | rewritten | skipped |
| --- | ---: | ---: | ---: | ---: |
| vsftpd | 85 | 236 | 132 | 104 |
| libuv | 218 | 571 | 283 | 288 |
| memcached | 118 | 315 | 179 | 136 |

变化：

- memcached rewritten 从 178 增加到 179，skipped 从 137 降到 136。
- memcached `unsafe callsite return load` 从 3 降到 2。
- vsftpd、libuv 聚合数不变。

剩余 unsafe：

- vsftpd：`notdec_native_132f0` 仍是 `unsafe callsite return load`，`notdec_native_91c0` / `notdec_native_18470` 仍是 `unsafe callsite input value`。
- memcached：`notdec_native_bc60` / `notdec_native_1d760` 仍是 `unsafe callsite return load`。
- libuv：3 个 `unsafe callsite input value`。

## 性能

本步只多维护一个搜索状态，不增加额外 CFG 扫描。Bench2 同口径耗时为 85s / 218s / 118s，和上一轮 84s / 219s / 119s 同级，没有看到性能下降。

## 评分

- 实现效果：7/10。修掉 memcached 一个真实 skip，并让“返回寄存器被覆盖表示返回值未使用”的语义更接近 Ghidra。
- 复杂度：3/10。只拆分了搜索结果状态。
- 维护成本：3/10。后续多返回路径如果要支持 unused return，需要更细的 per-return policy。

更好的方案：对每个 callsite 做小型 use/def 数据流，明确区分每条路径上返回 storage 的 use 和 def。本步先只处理线性先 clobber 的常见形状。
