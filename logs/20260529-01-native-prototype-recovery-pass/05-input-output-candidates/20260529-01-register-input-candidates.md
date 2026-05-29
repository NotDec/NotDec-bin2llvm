# 20260529-01 Register Input Candidates

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的输入参数恢复分两步：先收集可能的输入 varnode，再用 prototype model 筛选成正式参数。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamTrial`：保存一个候选 storage 的地址、大小、slot、匹配到的 `ParamEntry`，以及 `active` / `used` / `killedbycall` 等状态。
  - `ParamActive`：保存当前函数或 callsite 的 `ParamTrial` 列表。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `ParamActive::registerTrial(...)`：把一个 storage 加入候选列表。
  - `ParamListStandard::fillinMap(...)`：调用 `buildTrialMap(...)` 把 trial 匹配到 ABI 参数位置，然后把 active trial 标成 used。
  - `ParamListRegister::fillinMap(...)`：寄存器参数列表的筛选入口。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionActiveParam::apply(...)` 附近：遍历 `Varnode::input`，如果 `possibleInputParam(...)` 通过，就 `registerTrial(...)`；有使用者的 input varnode 会 `markActive()`；最后调用 `deriveInputMap(...)`。

关键点是：heritage 产生的 input varnode 只是候选来源，不直接等于参数。候选还要过 calling convention 的 storage 规则，并过滤保存恢复寄存器这类噪声。

## native 复刻方式

当前 `NativeRegisterSSA` 已经把入口没有本函数定义但被使用的寄存器写成：

- 指令级 `notdec.register.external_input`
- 函数级 `notdec.register.external_inputs`

这一步只做寄存器输入候选：

- 新增 `NativeParamTrial`：保存寄存器名、ABI slot、对应 register global、是否 active。
- 新增 `NativeParamActive`：保存 trial 列表，先只支持 register input。
- 新增 `NativePrototypeRecovery` pass 入口：
  - 从 module `!notdec.abi` 读回 `NativeAbiSpec`。
  - 对每个函数读取 `notdec.register.external_inputs`。
  - 用 `NativePrototypeModel::findInputRegister(...)` 确认它是 ABI input register。
  - 如果该 register 是 ABI `unaffected`，先认为是 saved-register 噪声，不标成参数。
  - 写函数级 `notdec.prototype.input_candidates` metadata。

为了敏捷开发，测试只覆盖当前需要的两类情况：

- `RDI` 是 SysV input register，出现在 external input 后应标成候选。
- `RBX` 是 SysV preserved register，即使出现在 external input，也不标成输入参数。

暂不做：

- 返回值候选。
- 栈参数。
- active/use 反向切片。
- 改 LLVM 函数签名。

## 判断标准

- 新 pass 能独立读取 module `!notdec.abi`。
- `RDI` external input 会写入 `notdec.prototype.input_candidates`。
- `RBX` external input 不会写入候选。
- 现有 ABI、prototype model、register effects 测试不回退。

## 实现记录

修改文件：

- `include/notdec-bin2llvm/NativeAbi.h:72`：声明 `readNativeAbiMetadata(...)`。
- `lib/NativeAbi.cpp:395`：新增 `metadataField(...)`，从 metadata 字段里读取 `key=value`。
- `lib/NativeAbi.cpp:408`：新增 `storageFromMetadata(...)`，读回 register / stack storage。
- `lib/NativeAbi.cpp:429`：新增 `paramEntryFromMetadata(...)`，读回 ABI input/output pentry。
- `lib/NativeAbi.cpp:454`：新增 `effectFromMetadata(...)`，读回 `unaffected` / `killedbycall`。
- `lib/NativeAbi.cpp:574`：实现 `readNativeAbiMetadata(...)`，从 module `!notdec.abi` 重建 `NativeAbiSpec`。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:14`：新增 `NativePrototypeRecoveryOptions`。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:22`：新增 `NativeParamTrial`。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:30`：新增 `NativeParamActive`。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:34`：新增函数级 summary。
- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:40`：新增 pass 级 summary。
- `lib/passes/NativePrototypeRecovery.cpp:57`：实现 `runNativePrototypeRecovery(...)`。
- `lib/passes/NativePrototypeRecovery.cpp:89`：用 `NativePrototypeModel::findInputRegister(...)` 过滤 ABI input register。
- `lib/passes/NativePrototypeRecovery.cpp:94`：过滤 ABI `unaffected` register，避免把 `RBX` 这类 saved register 标成参数。
- `lib/passes/NativePrototypeRecovery.cpp:108`：写 `notdec.prototype.input_candidates`。
- `lib/passes/NativePrototypeRecovery.cpp:123`：实现 summary 输出。
- `lib/CMakeLists.txt:9`：把 `passes/NativePrototypeRecovery.cpp` 编进 core。
- `tests/native_prototype_recovery_test.cpp:48`：构造测试 ABI，包含 `RDI` input、`RAX` output、`RBX` unaffected。
- `tests/native_prototype_recovery_test.cpp:83`：构造函数级 `notdec.register.external_inputs`。
- `tests/native_prototype_recovery_test.cpp:145`：运行 `runNativePrototypeRecovery(...)`。
- `tests/native_prototype_recovery_test.cpp:154`：断言 `RDI` 被标成输入候选，`RBX` 没有被标成输入候选。
- `CMakeLists.txt:164`：新增 `native_prototype_recovery_test`。
- `CMakeLists.txt:173`：新增 CTest `notdec.native_prototype_recovery.input_candidates`。

验证命令：

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_prototype_recovery_test native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|prototype_recovery.input_candidates|register_effects.preserved)' -V
```

结果：通过。`notdec.native_abi.cspec`、`notdec.native_prototype_model.register`、`notdec.native_prototype_recovery.input_candidates`、`notdec.native_register_effects.preserved` 全部通过，总用时 0.10 秒。

## 风险

- 现在只按 ABI input register 和 `unaffected` 做粗筛，还没有 Ghidra 那种 active/no-use 多轮判断。
- 先把 ABI `unaffected` 全部当 saved-register 噪声过滤。SysV 下对 `RBX/RBP/R12-R15` 合理，但以后遇到特殊 ABI 或手写调用约定要再细分。
- 现在 metadata 里只写 register name 和 slot，没有写类型、size、global。后续返回值和签名改写时可能要扩展。

## 评分

- 实现效果：6/10。已经能从 external input 推出第一版寄存器输入候选，但还不是完整 Ghidra 参数分析。
- 复杂度：5/10。新增 pass 框架和 ABI metadata 读回，代码量可控。
- 维护成本：5/10。metadata 读写格式开始被多个 pass 依赖，后续最好抽成共用 helper。
