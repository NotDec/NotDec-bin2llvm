# 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

当前显式签名重写会把 recovered return value 写进 LLVM `ret`，caller 侧也会改成直接使用 call result。但 callee 内原来的返回寄存器 `store` 还留着。对于已经改成 LLVM 返回值的函数，这个 register store 是旧表示，会让后续 pass 继续看到一次返回寄存器写。

# Ghidra 实现参考

Ghidra 在 prototype recovery 里会把 output storage 纳入函数原型，而不是继续把返回值当普通寄存器副作用：

- `Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncCallSpecs::deriveOutputMap(...)`：从 output trial 生成函数返回 storage。
  - `FuncProto::updateAllTypes(...)`：把 recovered storage/type 更新到函数原型。
  - `FuncCallSpecs::buildOutputFromTrials(...)`：让 callsite 的返回表达按 recovered output 走。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionFuncLink::funcLinkOutput(...)`：建立 call output trial。

native 侧已经有 `NativePrototypeReturnBinding`，能找到 recovered return 对应的旧 register store。这一步只复刻最小效果：签名重写已经创建 LLVM `ret` 后，把这些旧 return store 删除。

# native 侧复刻策略

先覆盖已有显式签名重写形状：

- return-only。
- input + single return。
- multi-return。
- input + multi-return。

做法：

- 新增一个小 helper，遍历 `NativePrototypeReturnBinding`。
- 对每个 binding，在 `ret` 已经创建后删除 `ReturnStore`。
- 只删除当前 recovered return binding 指向的 store，不扫描其它寄存器写。
- 不改变 recovered metadata，metadata 仍记录恢复出的原型。

暂时不做：

- 删除 caller 侧参数 store。参数 store 目前还可能被 register SSA / 其它寄存器语义观察到，需要单独评估。
- 删除非 return candidate 的普通 register store。

# 判断标准

- return-only 重写后，callee 内旧 RAX return store 被删除。
- multi-return 重写后，callee 内旧 RAX/RDX return store 被删除。
- 现有 callsite 重写测试继续通过。

# 风险

- 如果后续还依赖 rewritten callee 内的 register global store 表示副作用，这会减少这种副作用。但显式签名重写的目标就是把返回值转成 LLVM 返回值，保留旧 store 反而会造成重复语义。

# 实现记录

改动：

- `lib/passes/NativePrototypeRecovery.cpp:1021` 新增 `eraseReturnBindingStores(...)`，只删除 recovered return binding 指向的旧 register store。
- `lib/passes/NativePrototypeRecovery.cpp:1109` 在 return-only 重写创建 LLVM `ret` 后删除旧 return store。
- `lib/passes/NativePrototypeRecovery.cpp:1339` 在 input + single return 重写后删除旧 return store。
- `lib/passes/NativePrototypeRecovery.cpp:1442` 在 multi-return 重写后删除旧 return stores。
- `lib/passes/NativePrototypeRecovery.cpp:1579` 在 input + multi-return 重写后删除旧 return stores。
- `tests/native_prototype_recovery_test.cpp:1065` 新增 `hasRegisterStore(...)`，用于检查 rewritten callee 内是否还保留指定寄存器 store。
- `tests/native_prototype_recovery_test.cpp:1838` 验证 return-only direct callsite 重写后 callee 不再保留旧 RAX store。
- `tests/native_prototype_recovery_test.cpp:2127` 在删除 store 前保存 return value，避免测试继续从已删除 store 读取 operand。
- `tests/native_prototype_recovery_test.cpp:2670` 验证多 input + 多 return 重写后 callee 不再保留旧 RAX/RDX store。
- `logs/20260529-01-native-prototype-recovery-pass/PROGRESS.md` 记录本步骤完成。

验证：

```sh
cmake --build build --target native_prototype_recovery_test -j2
git diff --check
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
ctest --test-dir build --output-on-failure
```

结果：通过，目标测试 `1/1 Test #4: notdec.native_prototype_recovery.input_candidates ... Passed`；全量测试 `8/8` 通过。

性能：只在显式签名重写成功路径按 return binding 数量删除 store，不影响默认 metadata recovery。目标测试耗时约 `0.04 sec`，全量测试约 `0.81 sec`。

评分：

- 实现效果：8/10。rewritten 函数不再同时用 LLVM return 和旧 register store 表示返回值。
- 复杂度：5/10。只新增一个小 helper，调用点清楚。
- 维护成本：5/10。后续如果增加栈返回或其它 output storage，可以继续让 binding 层负责定位旧写入点。
