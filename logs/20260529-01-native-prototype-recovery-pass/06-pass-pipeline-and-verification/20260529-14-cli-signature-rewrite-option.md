# 20260529-14 CLI Signature Rewrite Option

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的 decompiler action pipeline 会在同一轮分析里恢复 prototype，并让后续 action 使用结果。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionInputPrototype::apply(...)`：恢复输入参数。
  - `ActionReturnRecovery::apply(...)`：恢复返回候选。
  - `ActionOutputPrototype::apply(...)`：更新输出 prototype。
  - `ActionPrototypeTypes::apply(...)`：把 prototype 应用到 varnode、call、return 类型。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto::deriveInputMap(...)` / `FuncProto::deriveOutputMap(...)`：保存恢复出的输入和输出 storage。
  - `FuncProto::updateAllTypes(...)`：把 prototype 结果应用到当前函数。

native 侧已经有默认关闭的库层 `RewriteSignatures`。下一步给 CLI 加显式开关，让用户可以试用当前支持的无调用者签名重写，同时保持默认只写 metadata。

## native 复刻方式

这一步只改 CLI：

- `CliOptions` 新增 `RewritePrototypeSignatures`，默认 false。
- 新增 `--rewrite-prototype-signatures` 参数。
- `runPrototypeRecoveryPassIfEnabled(...)` 把该选项传给 `NativePrototypeRecoveryOptions::RewriteSignatures`。
- `printUsage(...)` 更新用法说明。

这一步不把签名重写设为默认、不新增 callsite rewrite、不改变 Bench2 默认验证路径。

## 判断标准

- 默认 CLI 行为不变。
- 带 `--rewrite-prototype-signatures` 时，prototype recovery pass 会开启库层 opt-in。
- 现有 prototype recovery 和 instcombine metadata 测试通过。

## 实现记录

已实现。

### 改动位置

- `tools/notdec-native-llvm.cpp:46` 的 `CliOptions` 新增 `RewritePrototypeSignatures`，默认 false。
- `tools/notdec-native-llvm.cpp:70` 的 `printUsage(...)` 增加 `--rewrite-prototype-signatures`。
- `tools/notdec-native-llvm.cpp:117` 的 `parseArgs(...)` 识别该开关。
- `tools/notdec-native-llvm.cpp:824` 的 `runPrototypeRecoveryPassIfEnabled(...)` 把 CLI 选项传给 `NativePrototypeRecoveryOptions::RewriteSignatures`。

### 验证

命令：

```sh
cmake -S . -B build \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON &&
cmake --build build --target notdec-native-llvm native_prototype_recovery_test native_instcombine_metadata_test -j2 &&
ctest --test-dir build -R 'notdec.native_(prototype_recovery.input_candidates|instcombine.metadata)' -V
```

结果：2 个测试通过。

### 性能和风险

- 默认关闭，不增加默认 pass 成本。
- 显式开启时只调用已有 module 级签名重写 helper。
- 当前仍不处理 callsite rewrite，只支持已有无调用者形状；带调用者函数会保守跳过。

### 评分

- 实现效果：8/10。CLI 已能显式打开库层签名重写。
- 复杂度：9/10。只加一个布尔开关和传参，理解成本低。
- 维护成本：9/10。默认行为不变，后续扩展 callsite rewrite 时可以复用同一开关。
