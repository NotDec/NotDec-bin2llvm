# 20260529-01 CLI Prototype Recovery Pipeline

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 不是把 prototype recovery 当成孤立步骤跑，而是放在 decompiler action pipeline 里，依赖前面 heritage 和 call effect 信息。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionActiveParam::apply(...)`：在 input trial fully checked 后调用 `resolveModel(...)`、`deriveInputMap(...)`、`buildInputFromTrials(...)`。
  - `ActionActiveReturn::apply(...)`：对 call output trial 调用 `checkOutputTrialUse(...)`、`deriveOutputMap(...)`、`buildOutputFromTrials(...)`。
  - `ActionReturnRecovery::apply(...)`：对当前函数 return output trial 调用 `deriveOutputMap(...)`，再 `buildReturnOutput(...)`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto::deriveInputMap(...)` / `FuncProto::deriveOutputMap(...)`：把 trial 交给 prototype model 的 input/output `ParamList`。

所以 native 侧也应该把 prototype recovery 放在 register SSA 后面：先产生 `external_inputs`、preserve/clobber 和 call barrier 结果，再筛参数/返回候选。

## native 复刻方式

当前 `notdec-native-llvm` 已经在 ELF 路线里附加 `!notdec.abi`，然后运行 `NativeRegisterSSA`。这一步只接最小 pipeline：

- 引入 `NativePrototypeRecovery` 头文件。
- 新增 CLI 选项：
  - `--no-prototype-recovery-pass`
  - `--prototype-recovery-summary`
- 新增 `runPrototypeRecoveryPassIfEnabled(...)`。
- 在 `.ll/.bc` 输入和 ELF 输入里，都在 `runRegisterSSAPassIfEnabled(...)` 后调用 prototype recovery。
- 如果 module 没有 `!notdec.abi`，当前 pass 会返回空 summary，不报错。这样 `.ll/.bc` 输入可以复用同一路径。

暂不做：

- 默认跑 instcombine。
- Bench2 全量脚本。
- 改函数签名或 callsite。

## 判断标准

- `notdec-native-llvm` 能编译通过。
- 现有 native prototype recovery 单测不回退。
- 现有 ABI、prototype model、register effects 测试不回退。

## 实现记录

### 改动

- `tools/notdec-native-llvm.cpp:7` 引入 `NativePrototypeRecovery.h`。
- `tools/notdec-native-llvm.cpp:61`、`tools/notdec-native-llvm.cpp:62` 在 `CliOptions` 中加入 prototype recovery 开关和 summary 开关。
- `tools/notdec-native-llvm.cpp:71` 到 `tools/notdec-native-llvm.cpp:73` 更新 usage。
- `tools/notdec-native-llvm.cpp:138` 到 `tools/notdec-native-llvm.cpp:144` 解析 `--no-prototype-recovery-pass` 和 `--prototype-recovery-summary`。
- `tools/notdec-native-llvm.cpp:244` 到 `tools/notdec-native-llvm.cpp:245` 修正 `RootSlaDir` 的 `std::optional` 用法。这个问题阻塞本文件在 native build 下编译。
- `tools/notdec-native-llvm.cpp:775` 到 `tools/notdec-native-llvm.cpp:787` 新增 `runPrototypeRecoveryPassIfEnabled(...)`，默认运行 `runNativePrototypeRecovery(...)`，并在 pass 后验证 module。
- `tools/notdec-native-llvm.cpp:812`、`tools/notdec-native-llvm.cpp:920` 在 `.ll/.bc` 输入路径和 ELF 输入路径里，都在 register SSA 后运行 prototype recovery。

### 验证

- `cmake --build build-native --target notdec-native-llvm -j2`
  - 结果：通过。
- `ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|prototype_recovery.input_candidates|register_effects.preserved)' -V`
  - 结果：4/4 通过，总耗时约 0.10s。
- `./build-native/bin/notdec-native-llvm /bin/ls -a 0x6aa0 -l 1024 -o build-native/notdec-native-llvm-smoke.ll --prototype-recovery-summary`
  - 结果：通过，summary 显示 `functions=1`、`external_inputs=12`、`input_candidates=6`、`return_candidates=2`。
- `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as build-native/notdec-native-llvm-smoke.ll -o build-native/notdec-native-llvm-smoke.bc`
  - 结果：通过。
- `/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify build-native/notdec-native-llvm-smoke.bc -o build-native/notdec-native-llvm-smoke.opt.bc`
  - 结果：通过。
- `./build-native/bin/notdec-native-llvm build-native/notdec-native-llvm-smoke.ll -o build-native/notdec-native-llvm-smoke-roundtrip.ll --prototype-recovery-summary`
  - 结果：通过，覆盖 `.ll` 输入路径。
- `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as build-native/notdec-native-llvm-smoke-roundtrip.ll -o build-native/notdec-native-llvm-smoke-roundtrip.bc`
  - 结果：通过。
- `/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify build-native/notdec-native-llvm-smoke-roundtrip.bc -o build-native/notdec-native-llvm-smoke-roundtrip.opt.bc`
  - 结果：通过。

### 性能影响

本次只在 CLI pipeline 后追加一次 prototype recovery pass。当前 smoke 用例规模很小，未观察到明显耗时。Bench2 全量同口径时间还没跑，保留到后续 stage 6 小步。

### 评分

- 实现效果：8/10。默认 pipeline 已能输出 ABI、register SSA、prototype candidate metadata。
- 复杂度：2/10。只接入已有 pass，没有新增算法。
- 维护成本：2/10。开关和 summary 跟 register SSA 现有接口一致。

### 后续

- 跑 Bench2 selected native 全量验证。
- 后续如果开始改函数签名或 callsite，需要另开小步计划。
