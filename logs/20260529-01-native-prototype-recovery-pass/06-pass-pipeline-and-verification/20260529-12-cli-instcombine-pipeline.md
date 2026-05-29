# 20260529-12 CLI Instcombine Pipeline

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 在 prototype recovery 前会先通过 action pipeline 做一批表达式和数据流规整，让参数、返回值 trial 看到更稳定的 P-Code 形态。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionDatabase::universalAction(...)`：组织 decompiler action pipeline。
  - `ActionPool::apply(...)`：按阶段反复执行 action。
  - `ActionPrototypeTypes::apply(...)`：在规整后的数据流上推动 prototype 类型恢复。
  - `ActionActiveParam::apply(...)`、`ActionReturnRecovery::apply(...)`：依赖前面规整后的 varnode/use 信息筛选参数和返回值 trial。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/funcdata.hh`
  - `Funcdata`：保存函数级 P-Code、Varnode、prototype 状态，是 action pipeline 的共享载体。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamActive` / `ParamTrial`：保存经过 action pipeline 后筛出的 storage trial。

native 侧没有完整 Ghidra action pipeline。当前阶段先用 LLVM `InstCombinePass` 做最保守的函数内 canonicalization，目标是减少后续 register/prototype recovery 对局部表达式形态的依赖。前面已经用单元测试覆盖了 register metadata、call barrier、direct/indirect call、参数/返回候选、多 return 和冲突 return 的安全性。

## native 复刻方式

这一步把 `instcombine` 接入 `notdec-native-llvm` 默认链路：

- 新增 CLI 开关 `--no-instcombine-pass`，用于调试或对比。
- 默认在 `NativeRegisterSSA` 前对 module 内每个非 declaration 函数跑 `InstCombinePass`。
- 每个阶段后继续用 `verifyModule(...)` 检查 IR。
- IR 输入和 ELF 输入走同一条 pass 顺序：`instcombine -> NativeRegisterSSA -> NativePrototypeRecovery`。

这一步不改 `NativeRegisterSSA` / `NativePrototypeRecovery` 的语义，也不把 instcombine 抽成新的库 pass。先只接 CLI，后续如果其他工具也需要，再拆公共 helper。

## 判断标准

- `notdec-native-llvm` 能构建。
- 默认链路会跑 `instcombine`，`--no-instcombine-pass` 能跳过。
- 现有 native 单元测试继续通过。
- `notdec-native-llvm` 对一个 `.ll` 输入能输出可被 LLVM 22 `llvm-as` 和 `opt -passes=verify` 接受的 IR。

## 实现记录

### 源码改动

- `tools/notdec-native-llvm.cpp:13`、`tools/notdec-native-llvm.cpp:16`、`tools/notdec-native-llvm.cpp:19`
  - 引入 LLVM new pass manager、`PassBuilder` 和 `InstCombinePass`。
- `tools/notdec-native-llvm.cpp:64`
  - `CliOptions` 新增 `DisableInstCombinePass`，默认不关闭。
- `tools/notdec-native-llvm.cpp:75`、`tools/notdec-native-llvm.cpp:139`
  - usage 和 `parseArgs(...)` 支持 `--no-instcombine-pass`。
- `tools/notdec-native-llvm.cpp:784`
  - 新增 `runInstCombinePassIfEnabled(...)`。
  - 函数内建立 LLVM analysis manager，给每个非 declaration 函数跑一次 `llvm::InstCombinePass`。
  - pass 后调用 `verifyModule(...)`，失败时返回错误。
- `tools/notdec-native-llvm.cpp:852`、`tools/notdec-native-llvm.cpp:963`
  - IR 输入和 ELF 输入都按 `instcombine -> NativeRegisterSSA -> NativePrototypeRecovery` 顺序执行。

这一步没有新增数据结构，也没有改变 `NativeRegisterSSA` / `NativePrototypeRecovery` 的候选判断逻辑。

### 验证

构建和单元测试：

```sh
cmake --build build --target notdec-native-llvm -j2
cmake --build build --target native_instcombine_metadata_test native_prototype_recovery_test native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|prototype_recovery.input_candidates|register_effects.preserved|instcombine.metadata)' -V
```

结果：5 个测试全部通过。

CLI `.ll` 输入验证：

```sh
build/bin/notdec-native-llvm build/instcombine-cli-input.ll -o build/instcombine-cli-output.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as build/instcombine-cli-output.ll -o build/instcombine-cli-output.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify build/instcombine-cli-output.bc -o build/instcombine-cli-output.opt.bc
build/bin/notdec-native-llvm build/instcombine-cli-input.ll --no-instcombine-pass -o build/instcombine-cli-noic.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as build/instcombine-cli-noic.ll -o build/instcombine-cli-noic.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify build/instcombine-cli-noic.bc -o build/instcombine-cli-noic.opt.bc
```

结果：默认链路和 `--no-instcombine-pass` 链路都通过 LLVM 22 汇编和 verify。

### 性能和风险

- 性能：现在 CLI 默认对每个函数多跑一次 `InstCombinePass`，会增加少量函数内优化时间。当前只做了小样例和单元测试验证，Bench2 selected native 全量耗时还没重新对比。
- 风险：`InstCombinePass` 可能改变局部表达式形态。前面已覆盖 register metadata、call barrier、direct/indirect call、input/return candidate、多 return 和冲突 return 的安全测试，但还没覆盖真实 Bench2 全量输入。
- 实现效果：8/10。CLI 默认链路已经接上，关闭开关可用。
- 复杂度：2/10。只在 CLI 层增加一个 helper，没有把 pass pipeline 抽到公共层。
- 维护成本：2/10。后续如果多个工具需要同一套 pipeline，再抽公共 helper。
