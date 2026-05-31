# 原始 prompt

```text
阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：
- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

第 73 步后，Bench2 只剩 `libuv.notdec_native_9e70` 一个 `unsafe callsite input value`。这个 callee 需要 `RSI` 和 `RDX` 两个输入。失败 callsite 前，`RSI` 有显式 store；`RDX` 的当前值在唯一前驱块里是 `RDX.regssa` PHI，没有再写回 `@RDX`。当前 `callsiteInputValueBeforeCall(...)` 只认 register store 和入口 external input，所以拿不到这个 PHI。

# Ghidra 对应实现

Ghidra heritage 后，多个路径汇合的 varnode 会形成 MULTIEQUAL。prototype 阶段拿到的是当前 SSA varnode，而不是必须等待它写回寄存器：

- `Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritage(...)`
  - `HeritageInfo::buildInfoList(...)`
- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveInputMap(...)`
  - `FuncCallSpecs::buildInputFromTrials(...)`
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`

所以 native 侧也应该能把 RegisterSSA 生成的 `RDX.regssa` PHI 当作 callsite input 当前值。

# native 侧复刻路线

本轮只处理当前真实 case 的最小形状：

1. callsite input 查找仍优先使用显式 register store。
2. 唯一前驱没有目标寄存器 store 时，如果该前驱块有目标寄存器名开头的 `*.regssa` PHI，并且类型匹配，就把这个 PHI 当作当前寄存器值。
3. 只识别 RegisterSSA 生成的命名 PHI，不识别任意普通 PHI。
4. 多前驱、PHI incoming 按边选择等更完整形态后续再做。

# 判断标准

- 增加 IR 单测：callee 需要 `RSI` 和 `RDX`，callsite 前 `RSI` 有 store，`RDX` 来自唯一前驱的 `RDX.regssa` PHI。
- `libuv.notdec_native_9e70` 应从 unsafe input 变成 rewritten。
- Bench2 selected smoke 通过，运行时间不能明显变差。

# 风险

风险主要是误把无关 PHI 当寄存器当前值。本轮只接受 RegisterSSA 命名的 `register.regssa*` PHI，且要求类型匹配；如果同块出现多个同名候选，则保守拒绝。

# 实现记录

## 改动

- `lib/passes/NativePrototypeRecovery.cpp:317` 新增 `registerPhiValueAtBlockEntry(...)`，只识别 RegisterSSA 生成的 `register.regssa*` PHI，类型不匹配或同块多个候选时保守拒绝。
- `lib/passes/NativePrototypeRecovery.cpp:409` 修改 `callsiteInputValueBeforeCall(...)`，唯一前驱没有目标寄存器 store 时，尝试使用该前驱块入口处的 register SSA PHI。
- `tests/native_prototype_recovery_test.cpp:2331` 增加多 input callsite 测试：`RSI` 来自显式 store，`RDX` 来自唯一前驱的 `RDX.regssa` PHI。

## 验证

```text
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery' -V
cmake --build build --target notdec-native-llvm -j2
ctest --test-dir build --output-on-failure
OUT_DIR=/tmp/notdec-bin2llvm-bench2-predecessor-phi-input-smoke scripts/bench2-native-smoke.sh --build-dir build --out-dir /tmp/notdec-bin2llvm-bench2-predecessor-phi-input-smoke
```

结果：

- `native_prototype_recovery_test` 通过。
- 全量 `ctest` 9/9 通过。
- Bench2 selected smoke 通过。
- metrics：
  - `vsftpd`: 85s，rewritten 135，skipped 101，不变。
  - `libuv`: 218s，rewritten 285，skipped 286，不变。
  - `memcached`: 118s，rewritten 181，skipped 134，不变。
- skip reason：
  - `libuv.notdec_native_9e70` 不再是 `unsafe callsite input value`。
  - 该函数现在暴露为 `unsafe callsite return load`，说明 input 侧已推进到下一类 blocker。

性能：

- 本次只在现有唯一前驱回看里扫描前驱块开头 PHI。
- Bench2 同口径时间：`vsftpd` 85s 不变，`libuv` 219s -> 218s，`memcached` 117s -> 118s。没有明显性能下降。

## 评分

- 实现效果：7/10。清掉最后一个 unsafe input，但函数仍被 return load blocker 跳过。
- 复杂度：6/10。依赖 RegisterSSA 的 `*.regssa` 命名，范围窄但可控。
- 维护成本：6/10。后续最好改为显式 metadata 或统一 SSA 当前值查询，减少对名字的依赖。

更好的方案是让 `NativeRegisterSSA` 暴露 block/edge 上的寄存器当前值查询 API。本次先按真实 Bench2 case 做最小复刻。
