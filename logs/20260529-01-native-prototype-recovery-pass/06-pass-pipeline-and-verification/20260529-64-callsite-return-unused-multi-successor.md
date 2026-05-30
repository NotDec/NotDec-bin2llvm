# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

Bench2 还剩少量 `unsafe callsite return load`。当前 `findCallsiteReturnLoad(...)` 遇到 call 后有多个后继时直接 `Blocked=true`。这个保守策略能避免漏改旧返回寄存器 load，但也会挡住一种安全形态：call 后分支到多个直接结束的 block，且这些 block 都没有读取返回寄存器。此时新 typed call 的返回值确实没人用，可以安全丢弃。

# Ghidra 实现参考

- `Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritagePass(...)`：通过 SSA/use-def 判断 varnode 是否真的被使用。
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveOutputMap(...)`：根据 call output varnode 的真实使用和返回 storage 推导输出。
  - `FuncProto::updateAllTypes(...)`：把确定的 output trial 写入 prototype。

native 侧现在没有完整 use-def 图，这一步只复刻最保守的“多后继都没有返回寄存器使用”形态。

# native 侧复刻策略

- 扩展 `findCallsiteReturnLoad(...)` 的多后继处理。
- 如果 call block 有多个后继：
  - 每个后继必须没有继续后继，也就是直接结束。
  - 每个后继块内都不能出现目标返回寄存器的 `notdec.register.access` load/store。
  - 满足时返回 `{nullptr, false}`，表示没有旧返回 load，也没有阻断。
- 任何后继继续往下走、或出现目标返回寄存器 access，仍返回 blocked。
- 增加测试：
  - return-only callee 有 direct callsite。
  - call 后分支到两个直接 return block，两个 block 都不读 RAX。
  - rewrite 应成功，typed call 的返回值可以没有 use。
  - 原有多后继含返回 load 的 unsafe 负例继续保留。

# 判断标准

- 新多后继无返回 load 测试通过。
- 原有 unsafe callsite return load 负例仍通过。
- 全量 CTest 通过。
- Bench2 smoke 通过，观察 `unsafe callsite return load` 是否下降。

# 风险

这一步不沿多后继继续递归找 load，不处理合流、PHI、路径条件。只接受直接结束且无目标寄存器 access 的后继块，避免漏掉旧返回值使用。

# 实现记录

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:447` 新增 `blockHasNoReturnRegisterAccess(...)`。
  - 检查一个 basic block 内是否没有目标返回寄存器的 `notdec.register.access`。
- `lib/passes/NativePrototypeRecovery.cpp:463` 新增 `allSuccessorsEndWithoutReturnRegisterAccess(...)`。
  - 只接受多个后继。
  - 每个后继都必须直接结束，且没有目标返回寄存器 access。
- `lib/passes/NativePrototypeRecovery.cpp:479` 调整 `findCallsiteReturnLoad(...)`。
  - call 后多后继时，如果所有后继都满足“直接结束且不访问返回寄存器”，返回未阻断。
  - 其它多后继仍返回 blocked。
- `tests/native_prototype_recovery_test.cpp:432` 新增 `createUnusedReturnMultiSuccessorCallerFunction(...)`。
  - call 后分成两个直接 `ret void` 的后继，不读取 RAX。
- `tests/native_prototype_recovery_test.cpp:2696` 增加 return-only callsite rewrite 正例。
  - rewrite 后 typed call 的返回值允许没有 use。
  - 旧的多后继含 RAX load 负例仍保持 unsafe。

## 验证

- `git diff --check`
  - 通过。
- `cmake --build build --target native_prototype_recovery_test -j2`
  - 通过。
- `ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V`
  - 通过，1/1。
- `ctest --test-dir build --output-on-failure`
  - 通过，9/9。
- `cmake --build build --target notdec-native-llvm -j2`
  - 通过。
- `OUT_DIR=/tmp/notdec-bin2llvm-bench2-callsite-return-unused-multi-successor-smoke scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-callsite-return-unused-multi-successor-smoke`
  - 通过。

## Bench2 smoke 指标

| target | elapsed_seconds | prototype_functions | prototype_input_candidates | prototype_return_candidates | signature_rewrite_seen | signature_rewrite_rewritten | signature_rewrite_skipped |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| vsftpd | 86 | 187 | 163 | 56 | 236 | 130 | 106 |
| libuv | 220 | 485 | 321 | 157 | 571 | 283 | 288 |
| memcached | 118 | 259 | 224 | 94 | 315 | 178 | 137 |

和上一轮同口径相比，rewrite/skipped 数没有变化。说明当前真实样本剩余的 `unsafe callsite return load` 不是“多后继都不使用返回寄存器”这个简单形态。

skip reason：

- vsftpd：declaration 49，missing recovered prototype 52，unsafe callsite input value 2，unsafe callsite return load 3。
- libuv：declaration 86，missing recovered prototype 199，unsafe callsite input value 3。
- memcached：declaration 56，missing recovered prototype 78，unsafe callsite return load 3。

## 性能和复杂度

- 性能：Bench2 smoke 三个目标通过，耗时 86s / 220s / 118s，和上一轮接近；新增检查只在 call 后多后继时扫描直接后继块。
- 实现效果：6/10。补了一个安全的 unused-return 多后继形态，但真实样本未变化。
- 复杂度：3/10。没有递归追 CFG，也没有合成 PHI。
- 维护成本：3/10。后续要处理更复杂多后继返回 load，需要更完整的 use-def/PHI 判断。

## 后续不做

这一步不处理多后继后继续合流的情况，也不处理某些路径读返回值、某些路径不读返回值的路径相关形态。
