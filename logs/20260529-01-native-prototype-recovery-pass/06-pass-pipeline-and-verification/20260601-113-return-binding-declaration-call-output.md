# 20260601-113 return binding declaration call output

## 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass。首先将相关的数据结构划分为几个工作量大约相等的部分（目前应该差不多完成了，主要是在数据集上测试并进一步提升），其次，规划一下如何做测试和验证。把进度记录到PROGRESS.md里面。依然要求，

根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。

在数据集上测试并进一步提升时，因为不涉及实现具体的模块或数据结构，可以不需要遵守上面的功能复刻流程。

1. 每一轮先看 Bench2 当前 skip reason 和真实函数样本，再决定实现点。
2. 先按 Ghidra 数据结构和 Bench2 blocker 识别“大块能力”，再从大块能力里切小步。小步服务于大块任务，不允许小步自己变成主线。
3. 每次实现不应以“一个极小 CFG 变体”作为默认粒度。应把同一类语义问题合并成一个小阶段处理，同类问题多修复一些再合并commit。
```

## 当前 Bench2 现象

`ffmpeg:codec-library --decode-seed-limit 200` 暴露新的非合理 skip reason：

- `unsafe return value load=1`
- 函数：`av_packet_alloc`

真实 IR 形状：

```llvm
call void @av_malloc()
%RAX = load i64, ptr @RAX, !notdec.register.access !RAX
store i64 %RAX, ptr @RDX, !notdec.register.access !RDX
ret void
```

recovered prototype 有两个返回 storage：`RAX` 和 `RDX`。当前 return binding 只能看显式 store，`RDX` 绑定到 `%RAX` load 后被 `unsafe return value load` 挡住；`RAX` 则可能误用 call 前的旧 store。

候选大块任务：

- call output 作为 return binding 来源：本轮处理 declaration callee 的最小形状。
- 更完整的外部函数 prototype database：暂不做，当前 declaration 仍不 rewrite。
- 统一 register current-value / call output 查询：后续再整理，本轮只补 return binding。

## Ghidra 对应实现

Ghidra 在 P-code 层把 call 的输出当成 call spec 的 output trial，而不是要求机器返回寄存器必须有显式 COPY。

相关源码：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncCallSpecs` 保存 call 点的输入输出 trial。
  - `ProtoModel::deriveOutputMap(...)` 根据 output trial 推导返回 storage。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::buildOutputFromTrials(...)` 把 active output trials 转成 call output。
  - `FuncCallSpecs::checkOutputTrialUse(...)` 判断 call output 是否真的被使用。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionReturnRecovery::buildReturnOutput(...)` 从 active output 构造函数返回。

对应到 native 侧，declaration call 后的 `load @RAX` 是外部 call 的 ABI 返回值。它和普通函数内 `load @RAX` 不同，不应该一律按 unsafe register load 跳过。

## native 侧复刻策略

本轮做最小安全复刻：

- 只识别当前函数内、同一 basic block 内的 declaration call output：
  - register access load 向前最近的非 intrinsic call 是 declaration；
  - load 和 call 之间没有同 register store；
  - 该 load 可以作为安全的 declaration call output。
- `hasUnsafeReturnValueLoad(...)` 对这种 load 放行。
- 如果某个返回 storage 的显式 store 在 declaration call 之前，而同一组 return bindings 里已经有该 storage 的 declaration call output load，则把这个返回 storage 也绑定到该 call output load，并且不删除旧 store。
- 不给 declaration 自身改签名，不推断外部函数完整 prototype，不穿过复杂 CFG。

判断标准：

- 新增单元测试覆盖 `call @malloc_decl -> load RAX -> store RDX -> ret`，期望 no-input multi-return rewrite 成功，两个返回分量都来自 declaration call output load。
- 原有普通 `load RAX -> store RAX -> ret` 负例仍保持 `unsafe return value load`。
- `ffmpeg:codec-library --decode-seed-limit 200` 的 `av_packet_alloc` 从 `unsafe return value load` 变为 rewritten。

## 实现记录

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:87`：`NativePrototypeReturnBinding` 新增 `EraseReturnStores`，用于表示 return value 来自 declaration call output 时，不删除 call 前的旧 register store。
- `lib/passes/NativePrototypeRecovery.cpp:211`：新增 `registerAccessName(...)`，统一读取 `notdec.register.access` 的 register name。
- `lib/passes/NativePrototypeRecovery.cpp:219`：新增 `isDeclarationCallOutputLoad(...)`，只接受同 block 内 declaration call 后、同 register 无中间 store 的 register load。
- `lib/passes/NativePrototypeRecovery.cpp:245`：调整 `hasUnsafeReturnValueLoad(...)`，对 declaration call output load 放行，普通 register load 仍保持 unsafe。
- `lib/passes/NativePrototypeRecovery.cpp:1580`：新增 `returnPointDeclarationCallOutputLoads(...)`，在 return 前查找某个返回 register 对应的 declaration call output load。
- `lib/passes/NativePrototypeRecovery.cpp:1619`：新增 `applyDeclarationCallOutputAliases(...)`，把同组 return binding 里 stale store 绑定到已有 declaration call output load，并设置不删除旧 store。
- `lib/passes/NativePrototypeRecovery.cpp:1973`：`getNativePrototypeReturnBindings(...)` 优先为 declaration call output 建立 return binding，然后再回退到原有 return store 逻辑。
- `lib/passes/NativePrototypeRecovery.cpp:2038`：`eraseReturnBindingStores(...)` 跳过 `EraseReturnStores=false` 的 binding。
- `tests/native_prototype_recovery_test.cpp:1565`：新增 `createDeclarationCallOutputReturnFunction(...)`，构造 `call declaration -> load RAX -> store RDX -> ret`。
- `tests/native_prototype_recovery_test.cpp:4440`：新增 declaration call output multi-return 测试，验证两个返回分量都来自 call output load，旧 call setup store 保留，复制用的 return store 被删除。

验证：

- `cmake --build /tmp/notdec-bin2llvm-build --target native_prototype_recovery_test -j$(nproc)`：通过。
- `ctest --test-dir /tmp/notdec-bin2llvm-build -R 'notdec.native_prototype_recovery' --output-on-failure`：通过。
- `cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm -j$(nproc)`：通过。
- `scripts/bench2-native-prototype-audit.sh --build-dir /tmp/notdec-bin2llvm-build --out-dir /tmp/notdec-bin2llvm-bench2-ffmpeg-codec-seed200-call-output-final --target ffmpeg:codec-library --decode-seed-limit 200`：通过。
- `scripts/bench2-native-prototype-audit.sh --build-dir /tmp/notdec-bin2llvm-build --out-dir /tmp/notdec-bin2llvm-bench2-large-seed200-prototype-audit-final --target php:executable --target python:interpreter --target libicu:common-library --target ffmpeg:codec-library --decode-seed-limit 200`：通过。

Bench2 结果：

| target | all-confirmed | signature rewrite | needed | rewritten | skipped | skip reason |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `php:executable` | 46s | 47s | 159 | 159 | 66 | `already matches=41`, `declaration=25` |
| `python:interpreter` | 44s | 45s | 156 | 156 | 69 | `already matches=44`, `declaration=25` |
| `libicu:common-library` | 45s | 45s | 166 | 166 | 64 | `already matches=34`, `declaration=30` |
| `ffmpeg:codec-library` | 48s | 46s | 183 | 183 | 36 | `already matches=17`, `declaration=19` |

`av_packet_alloc` 从 `unsafe return value load` 变为 `rewritten=1`。输出中：

```llvm
call void @av_malloc()
%RAX = load i64, ptr @RAX, !notdec.register.access !RAX
%13 = insertvalue { i64, i64 } undef, i64 %RAX, 0
%14 = insertvalue { i64, i64 } %13, i64 %RAX, 1
ret { i64, i64 } %14
```

风险：

- 当前只处理同一 basic block 内的 declaration call output，不处理跨 CFG 的 call output。
- declaration 自身仍不改签名；这一步只是让 caller 的 return binding 能安全使用外部 call 的 ABI 返回寄存器 load。
- `RDX` 仍来自当前 return candidate 规则，后续如果要减少这类重复 multi-return，需要单独处理 return candidate 去重/alias。
