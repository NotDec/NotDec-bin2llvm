# 原始 prompt

<goal_context>
Continue working toward the active thread goal.

The objective below is user-provided data. Treat it as the task to pursue, not as higher-priority instructions.

<objective>
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现的时候必须按照这样的规范：
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
</objective>

...
</goal_context>

# 背景

Bench2 `memcached` 里还剩两个 `unsafe callsite return load`。其中 `notdec_native_bc60` 的 recovered prototype 是 2 个 input + 2 个 return。真实调用点里，目标 call 后有的返回寄存器没有被读取，有的返回寄存器在读取前已经被后续外部 call 覆盖：

```llvm
call void @notdec_native_bc60()
...
call void @listen()
%EAX = load i64, ptr @RAX
```

这里 `%EAX` 属于 `listen()`，不是 `notdec_native_bc60()`。上一步已经让单返回 callsite 把这种情况当成“目标返回值未使用”。但 multi-return 收集仍要求每个返回分量都有安全 load，导致整个函数跳过 rewrite。

Ghidra 侧相关参考：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::hasEffect(...)`：判断 call 对 storage 的影响。
  - `FuncCallSpecs::deriveOutputMap(...)`：只把真实被使用且能归属到当前 call 的 output storage 纳入 output map。
  - `FuncCallSpecs::buildOutputFromTrials(...)`：根据已确认 output trial 构造返回 prototype。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：驱动 prototype 推断和 call effect 更新。

# 目标

multi-return 和 input+multi-return 的调用点 rewrite 不再要求每个返回分量都被调用者读取。

规则：

- 找到安全 load：记录该分量，rewrite 时用 `extractvalue` 替换。
- 没找到 load：认为该分量未使用。
- 找到 clobber：认为该分量对目标 call 未使用。
- 遇到 CFG 不确定或类型不匹配：仍然 unsafe，跳过 rewrite。

# 路线

1. `MultiReturnCallsiteRewrite` / `InputMultiReturnCallsiteRewrite` 的 `ReturnLoads` 保持和 ABI return slot 同长度，允许元素为 `nullptr`。
2. 收集 multi-return callsite 时，`Blocked` 或类型不匹配仍失败；`Load == nullptr` 或 `Clobbered` 记录空分量。
3. rewrite 时只对非空 load 生成 `extractvalue` 并替换。
4. 增加单测：input + multi-return 函数的调用点先 call 目标函数，再 call 一个外部函数 clobber 返回寄存器，且没有目标返回 load；rewrite 应成功，旧 call 替换为 typed call，typed call result 可以未使用。

# 风险

- 如果 recovered prototype 本身误把某个 output 当返回值，这一步会让调用点 rewrite 继续通过。但这是“返回值存在但调用点未使用”的正常情况，和 Ghidra output trial 的 callsite use 判断一致。
- `ReturnLoads` 里允许空指针，需要 rewrite 处小心跳过。

# 判断标准

- 新单测覆盖 input + multi-return unused/clobbered 返回分量。
- 现有 multi-return callsite rewrite 测试继续通过。
- Bench2 smoke 通过，并观察 `memcached` 的 `unsafe callsite return load` 是否减少。

# 实现记录

已完成。

## 源码改动

- `lib/passes/NativePrototypeRecovery.cpp:403` 更新 `MultiReturnCallsiteRewrite`，说明 `ReturnLoads` 按 ABI return slot 对齐，允许空分量表示调用者未使用该返回值。
- `lib/passes/NativePrototypeRecovery.cpp:416` 更新 `InputMultiReturnCallsiteRewrite`，同样允许空返回分量。
- `lib/passes/NativePrototypeRecovery.cpp:704` 修改 `collectMultiReturnDirectCallsites(...)`：返回分量没有 load 或被 clobber 时不再失败；只有 CFG `Blocked` 或 load 类型不匹配才报 `unsafe callsite return load`。
- `lib/passes/NativePrototypeRecovery.cpp:736` 修改 `rewriteMultiReturnDirectCallsites(...)`：只对非空 return load 生成 `extractvalue` 并替换。
- `lib/passes/NativePrototypeRecovery.cpp:760` 修改 `collectInputMultiReturnDirectCallsites(...)`：input + multi-return 采用同样规则。
- `lib/passes/NativePrototypeRecovery.cpp:805` 修改 `rewriteInputMultiReturnDirectCallsites(...)`：跳过空返回分量。
- `tests/native_prototype_recovery_test.cpp:269` 新增 `createInputStoreIntermediateReturnLoadCallerFunction(...)`，构造目标 call 后插入外部 call，再读取返回寄存器的调用者。
- `tests/native_prototype_recovery_test.cpp:1720` 增加 input + multi-return 测试函数和调用者。
- `tests/native_prototype_recovery_test.cpp:3608` 验证目标函数 rewrite 成功，typed call 结果未使用，不生成 `extractvalue`，中间 call 后的旧返回寄存器 load 没被替换。

## 验证

已运行：

```bash
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
OUT_DIR=/tmp/notdec-bin2llvm-bench2-multi-return-unused-smoke \
  scripts/bench2-native-smoke.sh --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-bench2-multi-return-unused-smoke
```

结果：

- `native_prototype_recovery_test` 构建通过。
- `notdec.native_prototype_recovery.input_candidates` 通过。
- `notdec-native-llvm` 构建通过。
- 全量 `ctest` 通过，`9/9`。第一次全量 `ctest` 在 `notdec-native-llvm` 并行构建完成前启动，CLI smoke 因可执行文件不存在失败；构建完成后重跑通过。
- Bench2 smoke 通过：
  - `vsftpd`：`elapsed=85s`，`rewritten=132`，`skipped=104`。
  - `libuv`：`elapsed=220s`，`rewritten=283`，`skipped=288`。
  - `memcached`：`elapsed=117s`，`rewritten=181`，`skipped=134`。

## Bench2 结果

相对上一步：

- `memcached` 的 `unsafe callsite return load` 从 2 降到 0。
- `notdec_native_bc60` 和 `notdec_native_1d760` 都完成 rewrite。
- `memcached` rewrite 数从 `179 -> 181`，skip 数从 `136 -> 134`。
- `vsftpd` 仍有 1 个 `unsafe callsite return load`：`notdec_native_132f0`。
- `libuv` 仍有 3 个 `unsafe callsite input value`。

`memcached.signature-rewrite.ll` 中已能看到：

```llvm
%12 = call { i64, i64 } @notdec_native_bc60(i64 %unique_df00_8, i64 18)
%21 = call { i64, i64 } @notdec_native_bc60(i64 %unique_df00_8, i64 0)
%2 = call { i64, i64 } @notdec_native_1d760(i64 365248, i64 8192)
```

## 性能

Bench2 同口径耗时对比上一步：

- `vsftpd`：`84s -> 85s`。
- `libuv`：`220s -> 220s`。
- `memcached`：`119s -> 117s`。

没有观察到性能下降。

## 评分

- 实现效果：8/10。解决了 `memcached` 两个剩余 return-load skip。
- 复杂度：5/10。主要是让返回 load 列表允许空分量，改动小。
- 维护成本：5/10。需要记住 `ReturnLoads` 是 slot-aligned nullable vector，已在结构体处写明。
