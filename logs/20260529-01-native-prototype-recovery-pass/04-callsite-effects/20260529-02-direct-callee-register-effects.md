# 20260529-02 Direct Callee Register Effects

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 在 callsite 处理时不是只看默认 ABI，也会使用具体 callee 的 prototype / call specs：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::hasEffect(...)`：如果函数 prototype 自己有 effect override，就优先查自身 `effectlist`；否则回到 `ProtoModel::hasEffect(...)`。
  - `ProtoModel::lookupEffect(...)` / `ProtoModel::hasEffect(...)`：默认 model effect 查询。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::guardCalls(...)`：每个 call 通过 `FuncCallSpecs` 查询 effect，再决定是否插 INDIRECT guard。

也就是说，callsite effect 的查询入口是 callee/callsite 的 summary，而不是永远用默认 ABI。默认 ABI 是外部函数、未知函数、还没有 callee summary 时的 fallback。

## native 复刻方式

上一小步已经让 `NativeRegisterSSA` 的 call barrier 按 module ABI effect 判断：

- ABI `unaffected` register 跨 call 传播。
- 其它 register 保守阻断。

这一步补 direct callee metadata：

- 如果 call 的 callee 是本 module 内定义函数，且 callee 带函数级 metadata：
  - `notdec.register.preserves` 包含当前 register：不阻断。
  - `notdec.register.clobbers` 包含当前 register：阻断。
- 如果 callee 没有这两个 metadata，回到上一小步的 ABI fallback。
- 先不做 fixpoint。当前测试手工给 callee 附 metadata，后续再让多轮 pass 自动稳定。

测试用例：

- 构造 direct call 到 module 内定义函数 `callee_preserves_rbx`，给它 `notdec.register.preserves`/`RBX`。
- call 前 store `RBX`，call 后 load `RBX`，应传播。
- 同时保留外部 call 的 `RAX` killedbycall 测试，确保 fallback 没退化。

## 判断标准

- direct call 读取 callee `notdec.register.preserves` 后允许 `RBX` 跨 call 传播。
- 未知 / 外部 call 仍按 ABI fallback。
- 现有 preserved、clobber、ABI call effect 测试不回退。

## 实现记录

修改文件：

- `lib/passes/NativeRegisterSSA.cpp:206`：新增 `functionMetadataHasRegister(...)`，读取函数级 `notdec.register.preserves` / `notdec.register.clobbers`。
- `lib/passes/NativeRegisterSSA.cpp:387`：`callClobbersRegister(...)` 接收具体 call 指令，direct callee 是本模块定义函数时优先查 callee metadata。
- `lib/passes/NativeRegisterSSA.cpp:392`：callee metadata 标 preserved 时不阻断当前 register。
- `lib/passes/NativeRegisterSSA.cpp:396`：callee metadata 标 clobber 时阻断当前 register。
- `tests/native_register_effects_test.cpp:127`：新增 `attachRegisterMetadataToFunction(...)`，给测试 callee 挂函数级 register effect metadata。
- `tests/native_register_effects_test.cpp:140`：新增 `createDirectCallEffectFunction(...)`，构造 direct call 到本模块 callee 的样例。
- `tests/native_register_effects_test.cpp:233`：把 direct call 样例加入测试 module。
- `tests/native_register_effects_test.cpp:263`：断言 direct call 后 `RBX` load 被传播。

验证命令：

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|register_effects.preserved)' -V
```

结果：通过。`notdec.native_abi.cspec`、`notdec.native_prototype_model.register`、`notdec.native_register_effects.preserved` 全部通过，总用时 0.08 秒。

## 风险

- 当前没有 fixpoint。测试是手工给 callee 加 metadata；真实 pipeline 中 callee metadata 的稳定顺序后续要解决。
- 如果 direct callee 没 metadata，仍 fallback 到 ABI。这个行为保守，但会错过本模块已知但暂未分析出的信息。
- 仍未拆出 `NativeCallEffectResolver`，后续阶段继续扩展时应拆分。

## 评分

- 实现效果：7/10。direct call 已能使用 callee metadata，符合阶段 4 的第一版目标。
- 复杂度：4/10。只加 metadata 查询和测试样例。
- 维护成本：5/10。逻辑仍在 `NativeRegisterSSA` 中，后续 fixpoint 和 external summary 应拆出去。
