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

Bench2 `vsftpd` 还剩 1 个 `unsafe callsite return load`：`notdec_native_132f0`。

真实形状在 `vsftpd.signature-rewrite.ll` 里大致是：

```llvm
bb_16ee5:
  call void @notdec_native_132f0()
  br label %bb_16efb

bb_16efb: ; preds = %bb_16ee5, %bb_16ed4
  ; 不读取 RAX
  ret void
```

当前 `findCallsiteReturnLoad(...)` 遇到后继块有多个前驱时直接返回 `Blocked`。这对“后继里读取返回寄存器”的情况是对的，因为 load 不被当前 call 唯一支配；但如果 shared successor 根本不读返回寄存器，那么目标 call 的返回值就是未使用，不应该阻止签名 rewrite。

Ghidra 侧相关参考：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveOutputMap(...)`：只把能归到当前 call 且真实被使用的 output 放进 output map。
  - `FuncCallSpecs::hasEffect(...)`：判断 call 对 storage 的影响。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：驱动 prototype 推断。

# 目标

callsite return 查找遇到 shared successor 时，如果这个 successor 块内没有目标返回寄存器 load，且该块直接结束，不再把它当 unsafe；按“调用点未使用返回值”处理。

仍然保守处理：

- shared successor 里读取返回寄存器：unsafe。
- shared successor 还有后继：unsafe，避免跨更复杂 CFG 误判。
- shared successor 中出现目标返回寄存器 store 或中间 call clobber：按未使用返回值处理。

# 路线

1. 在 `findCallsiteReturnLoad(...)` 的多前驱后继分支里，先扫描当前 successor。
2. 如果扫描结果有 load，返回 `Blocked`。
3. 如果扫描结果是 store/clobber 或空，并且 successor 终结指令没有后继，返回未使用。
4. 增加单测：目标 call 后跳到 shared successor，shared successor 另有一个前驱，但不读返回寄存器；rewrite 应成功，typed call result 不被使用。

# 风险

- 这一步只处理单个 shared successor 块直接结束的形状，不追更深 CFG。更深 CFG 需要单独分析支配关系，先不做。
- 如果后续需要处理 shared successor 里的 PHI / 更复杂控制流，应先确认真实 CFG 语义，不在这里扩大范围。

# 判断标准

- 单测覆盖 shared successor 未使用返回值。
- 现有 return callsite 负例仍通过。
- Bench2 smoke 通过，并观察 `vsftpd` 的 `unsafe callsite return load` 是否降到 0。

# 实现记录

已完成。

## 源码改动

- `lib/passes/NativePrototypeRecovery.cpp:613` 修改 `findCallsiteReturnLoad(...)`：后继块有多个前驱时，不再直接判 `Blocked`，而是先扫描这个 shared successor。
- `lib/passes/NativePrototypeRecovery.cpp:631` 新增 shared successor 的窄判定：如果块内没有目标返回寄存器 `load`，且终结指令没有后继，就按“目标 call 返回值未使用”返回空结果；如果块里读了返回寄存器，仍然按 unsafe 处理。
- `tests/native_prototype_recovery_test.cpp:571` 新增 `createUnusedReturnSharedSuccessorCallerFunction(...)`，构造一个 target call 后跳到 shared successor，但 shared successor 不读返回寄存器的形状。
- `tests/native_prototype_recovery_test.cpp:3058` 新增单测：shared successor 未使用返回值时，return-only rewrite 应成功，调用点结果值不再被使用。

## 验证

已运行：

```bash
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
ctest --test-dir build --output-on-failure
OUT_DIR=/tmp/notdec-bin2llvm-bench2-shared-successor-unused-smoke \
  scripts/bench2-native-smoke.sh --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-bench2-shared-successor-unused-smoke
```

结果：

- `native_prototype_recovery_test` 构建通过。
- `notdec.native_prototype_recovery.input_candidates` 通过。
- 全量 `ctest` 通过，`9/9`。
- Bench2 smoke 通过：
  - `vsftpd`：`elapsed=85s`，`rewritten=133`，`skipped=103`。
  - `libuv`：`elapsed=218s`，`rewritten=283`，`skipped=288`。
  - `memcached`：`elapsed=118s`，`rewritten=181`，`skipped=134`。

## Bench2 结果

相对上一步：

- `vsftpd` 的 `unsafe callsite return load` 从 `1` 降到 `0`。
- `notdec_native_132f0` 完成 rewrite。
- `libuv` 仍有 `unsafe callsite input value: 3`。
- `memcached` 保持不变。

`vsftpd.signature-rewrite.ll` 里现在能看到：

```llvm
%24 = call i64 @notdec_native_132f0(i64 2)
%27 = call i64 @notdec_native_132f0(i64 2)
%3 = call i64 @notdec_native_132f0(i64 1)
%12 = call i64 @notdec_native_132f0(i64 2)
%26 = call i64 @notdec_native_132f0(i64 2)
```

## 性能

Bench2 同口径耗时对比上一步：

- `vsftpd`：`85s -> 85s`。
- `libuv`：`220s -> 218s`。
- `memcached`：`117s -> 118s`。

没有看到明显下降。

## 评分

- 实现效果：8/10。清掉了 `vsftpd` 最后一个 return-load skip。
- 复杂度：4/10。只是在已有 CFG 查找里放宽了一个窄形状。
- 维护成本：4/10。规则仍然很局部，后续如果 shared successor 形状变复杂，再单独拆分。
