# 20260529-01 ABI Call Effect Barrier

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 在 heritage 阶段用 prototype effect 判断 call 是否阻断 storage 数据流：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ProtoModel::lookupEffect(...)`：在 effect list 中查 storage，返回 `unaffected`、`killedbycall` 或 `unknown_effect`。
  - `ProtoModel::hasEffect(...)`：对外提供 model effect 查询。
  - `FuncProto::hasEffect(...)`：函数 prototype 先查自身 override effect，没有则回到 model effect。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::guardCalls(...)`：对每个 call 查询 `fc->hasEffect(...)`。
  - 关键语义：注释写明 effect 是 `unaffected` / `reload` 时不 guard call；`unknown_effect` / `return_address` 会插 INDIRECT guard，从而阻断 SSA 传播。

这对应 native 侧要做的事：call 不能再无条件阻断所有 register。ABI preserved register 应能跨外部 call 传播，ABI killedbycall register 应被阻断。

## native 复刻方式

当前 `NativeRegisterSSA` 的 `isRegisterClobberCall(...)` 是全局 barrier：只要 block 中有 call，所有 register 的传播都会被切断。这比 Ghidra 保守太多，会让 `RBX` 这类 ABI preserved register 跨 call 信息丢失。

这一步做最小复刻：

- 从 `!notdec.abi` 读取 register effect：
  - `effect=unaffected` 记为 unaffected。
  - `effect=killedbycall` 记为 killedbycall。
- 在 SSA 查找时，call barrier 变成 per-register：
  - 当前 register 是 ABI `unaffected`：call 不阻断。
  - 当前 register 是 ABI `killedbycall`：call 阻断。
  - 未知 effect：保持阻断。
- 仍不区分 direct call / external call / indirect call。第一小步按 ABI effect 处理所有非 intrinsic call。
- 不引入新 `NativeCallEffects` 文件，先在 `NativeRegisterSSA` 内复刻最小逻辑；后续 direct callee summary 和 fixpoint 再拆。

测试用例：

- 构造 `@RBX` 和 `@RAX` 两个 register global。
- ABI metadata 标 `RBX` unaffected、`RAX` killedbycall。
- 函数中 call 外部函数后读取：
  - `RBX` load 应被替换成 call 前 store 的值。
  - `RAX` load 不应被 call 前 store 传播。

## 判断标准

- 外部 call 后 `RBX` 可按 ABI unaffected 传播。
- 外部 call 后 `RAX` 不从 call 前传播。
- 现有 preserved/clobber 测试不回退。

## 实现记录

修改文件：

- `lib/passes/NativeRegisterSSA.cpp:45`：新增 `AbiRegisterEffects`，保存 ABI unaffected / killedbycall register 名字。
- `lib/passes/NativeRegisterSSA.cpp:112`：把 `collectAbiUnaffectedRegisters(...)` 扩展为 `collectAbiRegisterEffects(...)`，同时读取 `unaffected` 和 `killedbycall`。
- `lib/passes/NativeRegisterSSA.cpp:212`：`FunctionPromoter` 接收完整 ABI register effect。
- `lib/passes/NativeRegisterSSA.cpp:298`：`readRegister(...)` 的 call barrier 变成 per-register。
- `lib/passes/NativeRegisterSSA.cpp:320`：`localValueBefore(...)` 只在当前 register 会被 call clobber 时阻断前序 store。
- `lib/passes/NativeRegisterSSA.cpp:364`：新增 `callClobbersRegister(...)`；当前 ABI unaffected 不阻断，其它保持阻断。
- `tests/native_register_effects_test.cpp:48`：测试 ABI 同时标 `RBX` unaffected、`RAX` killedbycall。
- `tests/native_register_effects_test.cpp:93`：新增 `createCallEffectFunction(...)`，构造外部 call 后读 `RBX` / `RAX` 的样例。
- `tests/native_register_effects_test.cpp:127`：新增 `countRegisterLoads(...)`，检查 pass 是否传播掉对应 load。
- `tests/native_register_effects_test.cpp:202`：断言 call 后 `RBX` load 被传播，`RAX` load 未传播。

验证命令：

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|register_effects.preserved)' -V
```

结果：通过。`notdec.native_abi.cspec`、`notdec.native_prototype_model.register`、`notdec.native_register_effects.preserved` 全部通过，总用时 0.08 秒。

## 风险

- 这一步仍按 ABI effect 处理所有非 intrinsic call，没有区分 direct callee summary。
- `killedbycall` 目前没有单独判断分支，因为未知默认也是阻断；后续做 direct call summary 时要显式区分 killed / unknown / preserved。
- 只支持按 register 名字匹配，不处理 alias。

## 评分

- 实现效果：7/10。解决了 ABI preserved register 跨 call 传播的关键问题，并保留 killed register 阻断。
- 复杂度：5/10。仍在 `NativeRegisterSSA` 内完成，未引入新 pass。
- 维护成本：5/10。下一步 direct call callee metadata 时，建议拆出 `NativeCallEffectResolver`，避免 SSA pass 继续变大。
