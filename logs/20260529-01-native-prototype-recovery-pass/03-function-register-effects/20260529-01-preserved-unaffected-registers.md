# 20260529-01 Preserved Unaffected Registers

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 对 preserved register 的核心判断不是单独靠 push/pop 模式，而是 ABI effect + heritage SSA：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::hasEffect(...)`：查询某个 storage 对 call/function prototype 的 effect，返回 `unaffected`、`killedbycall`、`unknown_effect` 等。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/funcdata_varnode.cc`
  - `Funcdata::newVarnode(...)` 附近：创建 input varnode 后调用 `funcp.hasEffect(...)`，如果是 `EffectRecord::unaffected`，则 `vn->setUnaffected()`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/varnode.hh`
  - `Varnode::setUnaffected()` / `Varnode::isUnaffected()`：标记“这个 input 值应在函数过程中保持不变”。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::guardCalls(...)`：对 call side effect 做 SSA guard；注释明确说 effect 是 `unaffected` / `reload` 时不 guard call。

也就是说，Ghidra 的 preserved 信息来自 ABI 的 unaffected storage 和 SSA 之后的真实数据流。保存恢复寄存器最后应该回到 entry input 值，而不是只看指令字节模式。

## native 复刻方式

当前 `NativeRegisterSSA` 已经有 native 侧 heritage 底座：

- entry 无定义但被读的寄存器会生成 `notdec.register.external_input`。
- 函数级会写 `notdec.register.external_inputs`。
- pass 内部能通过 `readBlockExit(...)` 计算每个 basic block exit 的寄存器 SSA 值。

这一步先复刻最小 preserved 判断：

- 从 module 级 `!notdec.abi` 读取 `effect=unaffected` 的 register 名字。
- 对每个函数、每个 ABI unaffected register：
  - 必须存在这个 register 的 entry external input。
  - 所有 `return` block 的 exit value 都必须等于这个 external input。
  - 满足时在函数上写 `notdec.register.preserves` metadata。
- 只标注 preserved，不做 clobber。clobber 下一小步再做。
- 不处理 call effect 细分；现有 SSA 遇到 call 仍保守 barrier。这个判断只在没有被 call barrier 打断或能从本函数恢复到 entry 值时成立。

测试用例先用纯 LLVM IR 构造：

- `@RBX` 带 `notdec.register` metadata。
- module 带 `!notdec.abi`，包含 `RBX` unaffected。
- 函数读 `RBX` 形成 external input，中间写一个临时值，return 前写回 entry input。
- 跑 `runNativeRegisterSSA` 后，函数应有 `notdec.register.preserves`，包含 `RBX`。

## 判断标准

- 新增 CTest 通过。
- 现有 `notdec.native_abi.cspec` 和 `notdec.native_prototype_model.register` 不回退。
- `notdec.register.preserves` 只在 return exit 等于 external input 时出现。

## 实现记录

修改文件：

- `lib/passes/NativeRegisterSSA.cpp:107`：新增 `collectAbiUnaffectedRegisters(...)`，从 module 级 `!notdec.abi` 读取 `effect=unaffected` 的 register 名字。
- `lib/passes/NativeRegisterSSA.cpp:199`：`FunctionPromoter` 构造时接收 ABI unaffected register 集合。
- `lib/passes/NativeRegisterSSA.cpp:214`：SSA rewrite 后调用 `attachPreservedMetadata(...)`。
- `lib/passes/NativeRegisterSSA.cpp:521`：新增 `attachPreservedMetadata(...)`，写函数级 `notdec.register.preserves`。
- `lib/passes/NativeRegisterSSA.cpp:553`：新增 `isPreservedOnAllReturns(...)`，要求所有 return block exit value 都等于 entry external input。
- `lib/passes/NativeRegisterSSA.cpp:610`：`runNativeRegisterSSA(...)` 开始时收集 ABI unaffected register。
- `CMakeLists.txt:164`：新增 `native_register_effects_test`。
- `CMakeLists.txt:173`：新增 CTest `notdec.native_register_effects.preserved`。

新增文件：

- `tests/native_register_effects_test.cpp:31`：构造带 `notdec.register` metadata 的 `@RBX` global。
- `tests/native_register_effects_test.cpp:46`：构造只含 `RBX` unaffected 的 ABI metadata。
- `tests/native_register_effects_test.cpp:57`：构造测试函数，一个恢复 `RBX`，一个不恢复。
- `tests/native_register_effects_test.cpp:85`：检查函数 metadata 是否包含指定 register。
- `tests/native_register_effects_test.cpp:117`：运行 `runNativeRegisterSSA(...)` 并断言 restored 函数有 `notdec.register.preserves`，clobbered 函数没有。

验证命令：

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|register_effects.preserved)' -V
```

结果：通过。`notdec.native_abi.cspec`、`notdec.native_prototype_model.register`、`notdec.native_register_effects.preserved` 全部通过，总用时 0.07 秒。

## 风险

- 这一步只标注 preserved，不标注 clobber。
- call effect 仍沿用当前 `NativeRegisterSSA` 的保守 barrier。遇到 call 后能否证明 preserved，取决于现有 SSA 是否能追到 return 前恢复值。
- ABI unaffected register 的匹配现在仍按 register 名字，不处理 alias。

## 评分

- 实现效果：7/10。能基于 SSA exit value 判断 restored saved register，不靠 push/pop 字节模式。
- 复杂度：5/10。复用了现有 SSA cache，但在 SSA pass 中加入了 ABI metadata 读取。
- 维护成本：5/10。后续 clobber 和 call effect 细分会继续扩展同一区域，需要注意不要让 `NativeRegisterSSA` 变得过大。
