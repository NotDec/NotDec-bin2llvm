# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

第 77 步后，Bench2 的显式签名重写只剩 `declaration` 和
`missing recovered prototype`。抽查 `vsftpd` 的 skipped 函数发现，很多函数没有
ABI input/output 候选，只是 init/PLT/prologue 或 callee-saved 保存恢复形状。

这些函数和“存在 recovered metadata 但格式损坏/读不回”的情况不一样。当前统一报
`missing recovered prototype`，会把真实缺失和无候选混在一起。

# Ghidra 对应实现

Ghidra prototype recovery 会先把试探参数放进 `ParamActive`，再由 prototype model
判断哪些 trial 被使用：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamTrial`
  - `ParamActive`
  - `ParamActive::getNumUsed()`
  - `FuncProto::deriveInputMap(...)`
  - `FuncProto::deriveOutputMap(...)`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamActive::deleteUnusedTrials()`
  - `ProtoModel::deriveInputMap(...)`
  - `ProtoModel::deriveOutputMap(...)`
  - `FuncCallSpecs::buildInputFromTrials(...)`
  - `FuncCallSpecs::buildOutputFromTrials(...)`

这里的关键点是：没有 used trial 是一个可见状态，不等同于 recovered prototype
metadata 损坏。native 侧还没有完整 `ParamActive` 状态机，但已经有
`notdec.prototype.input_candidates` / `return_candidates` / `recovered`
metadata，可以先把 rewrite 诊断拆清楚。

# native 侧计划

本轮只改报告层，不新增参数/返回恢复规则，也不改 IR：

1. `getNativePrototypeRewriteEligibility(...)` 读不到 recovered prototype 时，
   如果函数也没有 input/return candidate metadata 和 recovered metadata，返回
   `no recovered prototype candidates`。
2. 如果存在 prototype 相关 metadata 但读回失败，仍返回
   `missing recovered prototype`，表示 metadata 损坏或不匹配。
3. 更新单测里的 batch/opt-in summary 预期。

# 判断标准

- 单测覆盖无候选函数的新 skip reason。
- 旧的 malformed recovered metadata 仍报 `missing recovered prototype`。
- Bench2 smoke 通过。
- Bench2 skipped reason 中应能看到 `no recovered prototype candidates`，且
  rewritten 数量不下降。

# 风险

这是诊断拆分，不改变 IR，也不让更多函数 rewrite。主要风险是旧统计预期需要同步。

# 实现记录

## 修改内容

- `lib/passes/NativePrototypeRecovery.cpp:234` 增加
  `hasAnyPrototypeCandidateMetadata(...)`，检查函数是否带有
  `notdec.prototype.recovered`、`notdec.prototype.input_candidates` 或
  `notdec.prototype.return_candidates`。
- `lib/passes/NativePrototypeRecovery.cpp:1488` 更新
  `getNativePrototypeRewriteEligibility(...)`。读不到 recovered prototype 时，
  有 prototype 相关 metadata 仍返回 `missing recovered prototype`；完全没有候选
  metadata 时返回 `no recovered prototype candidates`。
- `tests/native_prototype_recovery_test.cpp:3918`、`:4218`、`:4258` 更新
  direct dispatch、eligibility、清理 stale metadata 后的 reason 预期。
- `tests/native_prototype_recovery_test.cpp:4572`、`:4706` 更新 batch/opt-in
  rewrite summary，覆盖 `no recovered prototype candidates` 计数和 per-function
  reason。

## 验证

- `cmake --build build --target native_prototype_recovery_test -j2`
  - 通过。
- `ctest --test-dir build -R 'notdec.native_prototype_recovery' -V`
  - 通过，1/1。
- `ctest --test-dir build --output-on-failure`
  - 通过，9/9。
- `cmake --build build --target notdec-native-llvm -j2`
  - 通过。
- `scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-no-prototype-candidate-reason-smoke`
  - 通过。

## Bench2 结果

上一轮同口径基线：

- `vsftpd`: 87s，rewritten 139，skipped 97，skip reason 为
  `declaration=49`、`missing recovered prototype=48`
- `libuv`: 223s，rewritten 338，skipped 233，skip reason 为
  `declaration=86`、`missing recovered prototype=147`
- `memcached`: 118s，rewritten 188，skipped 127，skip reason 为
  `declaration=56`、`missing recovered prototype=71`

本轮：

- `vsftpd`: 86s，rewritten 139，skipped 97，skip reason 为
  `declaration=49`、`no recovered prototype candidates=48`
- `libuv`: 224s，rewritten 338，skipped 233，skip reason 为
  `declaration=86`、`no recovered prototype candidates=147`
- `memcached`: 120s，rewritten 188，skipped 127，skip reason 为
  `declaration=56`、`no recovered prototype candidates=71`

rewritten 数量没有下降；耗时和上一轮接近。

## 评分

- 实现效果：3/5。没有恢复更多 prototype，但把真实无候选和 metadata 损坏拆开，
  后续定位下一块恢复逻辑更清楚。
- 复杂度：1/5。新增一个小 helper 和 reason 分支。
- 维护成本：1/5。只依赖已有 metadata 名称，后续如果 `ParamActive` 状态更完整，
  可以把这个 reason 接到更精确的 trial 统计上。

更好的长期方案是显式保存 native `ParamActive` 的 trial/used/no-use 状态。本轮先用
已有 metadata 做最小诊断拆分，不新增中间结构。
