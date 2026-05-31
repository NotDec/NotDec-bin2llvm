# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

第 76 步后，Bench2 关注用例只剩 `vsftpd.notdec_native_8410` 一个
`unsafe callsite input value`。这个 callee 的 recovered prototype 需要
`RDI` input。

抽查失败调用点发现，`notdec_native_8480` 调用 `notdec_native_8410` 前没有
显式 `store @RDI`，caller 自身也没有 `RDI.external_input`。旧 IR 里
callee 入口会自己 `load @RDI`；如果把 callee 改成 `void(i64)`，caller
侧需要在旧 call 前补一个当前 `@RDI` load 作为实参，才等价。

# Ghidra 对应实现

Ghidra 不要求 callsite 前一定有一次写寄存器。prototype 阶段使用当前
SSA varnode 作为 call input：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`
  - `FuncCallSpecs::buildInputFromTrials(...)`
  - `FuncCallSpecs::hasEffect(...)`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritage(...)`
  - `HeritageInfo::buildInfoList(...)`

这里的关键点是：call input 来自当前 storage 的当前值。这个值可能来自
显式定义、入口 input、PHI，也可能在 native LLVM 里还只表现为 register
global 的当前内存值。

# native 侧计划

本轮只补当前真实 case 需要的最小形状：

1. `callsiteInputValueBeforeCall(...)` 先保持现有顺序：本块 store、前驱
   store/PHI、caller 入口值优先。
2. 如果这些都没有找到，就查 module 中带 `notdec.register` metadata 的
   register global。
3. 如果 global 名字和目标 input register 相同，且类型匹配，就在旧 call
   前插入一个 load，把它作为新 typed call 的参数。
4. 这不是把缺失参数随便补成常量；它只是把旧 callee 入口的 `load @RDI`
   移到 caller 侧。

# 判断标准

- 单测覆盖“无 store、无 caller external input 时，用 callsite 前 register
  global load 传参”。
- Bench2 smoke 通过。
- `vsftpd` 的 `unsafe callsite input value` 应清零，或至少不新增其它
  unsafe return/input blocker。
- 同口径运行时间不能明显变差。

# 风险

风险是过早接受本该保守的缺失参数。这里插入的是旧 call 前的真实 register
global load，和旧 callee 入口 load 读同一个 storage，语义上比随便复用入口值
更保守。

# 实现记录

## 修改内容

- `lib/passes/NativePrototypeRecovery.cpp:336` 增加
  `registerGlobalForName(...)`，按 `notdec.register` metadata 的 `name`
  字段和参数类型查找唯一 register global。找不到或找到多个都返回空。
- `lib/passes/NativePrototypeRecovery.cpp:362` 增加
  `registerGlobalValueBeforeCall(...)`，在旧 call 前插入 register global load，
  并保留 register access metadata。
- `lib/passes/NativePrototypeRecovery.cpp:408` 更新
  `callsiteInputValueBeforeCall(...)`。原有本块 store、前驱 store/PHI、
  caller 入口值仍优先；都找不到时，fallback 到 call 前 register global
  load。
- `tests/native_prototype_recovery_test.cpp:1995`、`:2680`、`:4571`
  更新 input-only 和 batch rewrite 预期，覆盖无显式 store 时的 register
  global callsite load。
- `tests/native_prototype_recovery_test.cpp:2714` 增加重复 RDI register global
  负例，确认不能唯一定位 global 时仍返回 `unsafe callsite input value`。

## 验证

- `cmake --build build --target native_prototype_recovery_test -j2`
- `ctest --test-dir build -R 'notdec.native_prototype_recovery' -V`
  - 通过，1/1。
- `ctest --test-dir build --output-on-failure`
  - 通过，9/9。
- `cmake --build build --target notdec-native-llvm -j2`
- `scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-callsite-register-global-load-smoke-clean`
  - 通过。

## Bench2 结果

同口径上一轮基线：

- `vsftpd`: 84s，rewritten 138，skipped 98，仍有
  `unsafe callsite input value`: `notdec_native_8410`
- `libuv`: 217s，rewritten 338，skipped 233
- `memcached`: 116s，rewritten 188，skipped 127

本轮：

- `vsftpd`: 87s，rewritten 139，skipped 97。`notdec_native_8410`
  已 `rewritten=1`，skipped reason 只剩 `declaration` 和
  `missing recovered prototype`。
- `libuv`: 223s，rewritten 338，skipped 233。skipped reason 仍只剩
  `declaration` 和 `missing recovered prototype`。
- `memcached`: 118s，rewritten 188，skipped 127。skipped reason 仍只剩
  `declaration` 和 `missing recovered prototype`。

性能没有明显退化；三个目标耗时都在上一轮附近。

## 评分

- 实现效果：4/5。解决当前真实 blocker，并保留不能唯一定位 global 时的保守跳过。
- 复杂度：3/5。新增两个小 helper，理解成本有限，但把旧 entry load 移到 caller
  侧需要知道 native IR 的 register global 语义。
- 维护成本：3/5。后续如果 register global metadata 结构变化，需要同步这里的
  查找逻辑。

更好的长期方案是让 callsite input 查询统一走 RegisterSSA 的当前值接口。现在还没
有这样的统一接口，本轮先用最小 fallback 解决真实 case。
