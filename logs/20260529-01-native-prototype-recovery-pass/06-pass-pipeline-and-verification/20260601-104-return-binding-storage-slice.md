# 20260601-104 return binding storage slice

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

用 manifest 审计脚本跑 `redis:cli` 后，signature rewrite 只剩一类非合理 skip：

- `return value type mismatch=3`
- 函数：`genrand64_real1`、`genrand64_real2`、`genrand64_real3`

三个函数的 recovered prototype 都是两个返回 storage：`XMM0_Qa` 和 `RAX`。其中 `XMM0_Qa` 的 access metadata 是 `size=8`，但实际 IR 是 `store i512 ... @ZMM0`。当前 return binding 直接使用 store 的 value operand，导致 recovered return type 期望 `i64`，实际 value 是 `i512`。

候选大块任务：

- 返回 storage 和 store value 的 size 对齐：本轮处理。
- prototype type 从固定 `i64` 扩展到按 ABI storage/type 生成：暂不做，当前 recovered metadata 还没有完整类型信息。
- SSE/AVX 浮点返回类型恢复：暂不做，本轮只保证 storage slice 和 IR rewrite 闭环。
- wolfssl 规模/性能边界：暂只记录，`wolfssl:shared-library` 在 all-confirmed 阶段超过 12 分钟被杀，不是本轮功能 blocker。

本轮选择第一项，因为它直接对应当前真实 skip reason，且是 Ghidra `ParamTrial` storage size 思路的一小步。

## Ghidra 对应实现

Ghidra 的输出恢复不是直接拿“写入寄存器的原始表达式类型”当返回类型，而是围绕 storage trial 做判断。

相关源码：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamTrial` 保存 `addr`、`size`、`slot`、`entry`、`offset`。
  - `ParamActive::registerTrial(...)` 创建 trial，并给 trial 分配 slot。
  - `ProtoModel::deriveOutputMap(...)` 调用 output `ParamList` 的 `fillinMap(...)`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamTrial::testShrink(...)` 判断一个 trial 能否缩小到更小 storage。
  - `ParamActive::registerTrial(...)` 记录 trial 的地址和大小。
  - `FuncCallSpecs::buildOutputFromTrials(...)` 根据 active output trials 生成最终输出。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/modelrules.hh`
  - `ModelRule::fillinOutputMap(...)` 判断 trial 是否能作为返回值 storage。

这里最重要的是：返回值恢复看的是 ABI storage 的地址、大小和 slot，而不是宽寄存器整体值的 LLVM 类型。宽寄存器里只有一个 8 字节 lane 被 ABI 当作返回 storage 时，需要把这个 storage 范围对应的值切出来。

## native 侧复刻策略

当前 native 侧已经把 return candidate 记录成 recovered prototype metadata，但 `getNativePrototypeReturnBindings(...)` 还太粗：

- 根据 `notdec.register.access` 的 `name` 找到返回 store。
- 直接把 `store->getValueOperand()` 作为返回值。

本轮做一个小步：

- return binding 读取返回 store 的 `notdec.register.access` metadata。
- 如果 metadata 里 `size=8`，而 store value 是更宽的 integer，就在返回 store 前插入 `trunc` 到 `i64`。
- 多 return path 的等价判断仍用原始 store value 做，避免先扩大改动面。
- 只处理 integer 宽值到 `i64` 的安全切片；不在本轮引入浮点返回类型、大小端通用 subpiece、非 8 字节返回类型。

判断标准：

- 新增单元测试覆盖 `store i512 @ZMM0` + `size=8 name=XMM0_Qa` 的 return binding 和 multi-return rewrite。
- `redis:cli` 的 `return value type mismatch` 消失，只剩 `already matches` / `declaration` 这类合理 skip。
- LLVM 22 assemble / verify 通过。

风险：

- 这只是低 64 位 slice。当前 Redis 样本低 64 位确实是返回 double 的 bit pattern。后续如果遇到大端或非低位 lane，需要把 offset/endianness 和 ABI storage type 接进来。

## 实现记录

改动文件：

- `lib/passes/NativePrototypeRecovery.cpp`
  - 第 217 行附近：`hasUnsafeReturnValueLoad(...)` 继续把 `trunc(register load)` 视为不安全返回值，避免新 slice 隐藏旧的 register-load 风险。
  - 第 1406 行附近：新增 `returnValueForStore(...)`，读取 `notdec.register.access` 的 `size` 字段；当 storage 是 8 字节、store value 是更宽 integer 时，在 store 前插入 `trunc i64`。
  - 第 1760 行附近：`getNativePrototypeReturnBindings(...)` 使用 `returnValueForStore(...)` 生成 return binding 的返回值；多 return store 的等价判断仍按原始 store value 比较。
- `tests/native_prototype_recovery_test.cpp`
  - 第 25 行附近：测试 helper 支持创建非 `i64` register global。
  - 第 70 行附近：测试 helper 支持自定义 `base` / `offset` / `size` / `name` 的 register access metadata。
  - 第 112 行附近：新增 `attachVectorReturnTestAbi(...)`，让 ABI output 包含 `XMM0_Qa` 和 `RAX`。
  - 第 1382 行附近：新增 `createWideVectorAndScalarReturnFunction(...)`，构造 `store i512 @ZMM0` 但 access metadata 是 `size=8 name=XMM0_Qa` 的样例。
  - 第 3395 行附近：新增回归测试，确认 `XMM0_Qa` return binding 被切成 `i64`，multi-return rewrite 生成 `{ i64, i64 }`，并删除旧 register store。

验证：

- `cmake --build /tmp/notdec-bin2llvm-build --target native_prototype_recovery_test -j$(nproc)`：通过。
- `ctest --test-dir /tmp/notdec-bin2llvm-build -R 'notdec.native_prototype_recovery' --output-on-failure`：通过。
- `cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm -j$(nproc)`：通过。
- `scripts/bench2-native-prototype-audit.sh --build-dir /tmp/notdec-bin2llvm-build --out-dir /tmp/notdec-bin2llvm-bench2-redis-cli-return-slice --target redis:cli`：通过。
  - `signature rewrite rewritten functions`: 424。
  - `signature rewrite skipped functions`: 183。
  - skip reason 只剩 `already matches=118`、`declaration=65`。
  - `genrand64_real1`、`genrand64_real2`、`genrand64_real3` 均为 `rewritten=1`。
  - LLVM 22 `llvm-as` / `opt -passes=verify` stderr 为空。
- `ctest --test-dir /tmp/notdec-bin2llvm-build --output-on-failure`：9/9 通过。

性能记录：

- `redis:cli` manifest 审计：discovery 1s，all-confirmed 122s，signature-rewrite 123s。
- 该结果和本轮修复前同口径 Redis 审计相比没有明显新增性能问题；本轮主要增加一个 return store 前的 `trunc`，影响面只在宽 integer store 到 8 字节返回 storage 的函数。

当前结论：

- `redis:cli` 暴露的 `return value type mismatch=3` 已清零。
- 这轮只处理 storage size 到固定 `i64` recovered return type 的切片，不处理完整浮点返回类型和非 8 字节 storage。
