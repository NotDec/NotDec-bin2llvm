# 20260529-02 Clobbered Unaffected Registers

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的函数寄存器效果仍然是 ABI effect 和 heritage SSA 结合判断：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
  - `FuncProto::hasEffect(...)`：查询 storage 是否 `unaffected` / `killedbycall` / `unknown_effect`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/funcdata_varnode.cc`
  - 创建 input varnode 时，如果 ABI effect 是 `EffectRecord::unaffected`，调用 `Varnode::setUnaffected()`。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/varnode.hh`
  - `Varnode::isUnaffected()` 表示这个 input storage 应被函数保持。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::guardCalls(...)` 会根据 call effect 决定是否插入 INDIRECT guard；`unaffected` / `reload` 不 guard，其它 effect 会打断数据流。

换句话说，对 ABI 声明应保持的寄存器，如果 SSA 结果显示 return 前不是 entry input 值，就说明这个函数破坏了 ABI preserved 约定。Ghidra 后续会把这些信息用于参数筛选和异常输入过滤。

## native 复刻方式

上一小步已经在 `NativeRegisterSSA` 中标注：

- `notdec.register.preserves`：ABI unaffected register 在所有 return 前都等于 entry external input。

这一步补相反情况的最小标注：

- 对 ABI `unaffected` register，如果函数存在这个 register 的 entry external input，但不能证明所有 return 前等于这个 input，则标注 `notdec.register.clobbers`。
- 仍只覆盖 ABI unaffected register。caller-saved / killedbycall 寄存器的 clobber 规则留到 callsite effects 阶段和更完整 ABI effect 模型里做。
- 不靠 push/pop 字节模式。
- 不处理没有被读到的 unaffected register。没有 external input 时，当前 SSA 还没有足够证据判断它是否被恢复。

测试沿用 `native_register_effects_test`：

- `preserved_rbx`：读 RBX、写临时值、return 前恢复 entry input，应有 `preserves`，不应有 `clobbers`。
- `clobbered_rbx`：读 RBX、写临时值、不恢复，应有 `clobbers`，不应有 `preserves`。

## 判断标准

- 新增 `notdec.register.clobbers` metadata。
- `native_register_effects_test` 覆盖 preserved / clobbered 两条路径。
- 现有 ABI 和 prototype model 测试不回退。

## 实现记录

修改文件：

- `lib/passes/NativeRegisterSSA.cpp:214`：把原来的 preserved-only 入口改为 `attachRegisterEffectMetadata(...)`。
- `lib/passes/NativeRegisterSSA.cpp:520`：新增 `registerEffectMetadata(...)`，复用 register effect metadata 构造。
- `lib/passes/NativeRegisterSSA.cpp:537`：新增 `attachRegisterEffectMetadata(...)`，同一次 return SSA 检查中分别填充 preserved / clobbered。
- `lib/passes/NativeRegisterSSA.cpp:567`：写函数级 `notdec.register.clobbers`。
- `tests/native_register_effects_test.cpp:140`：断言恢复 RBX 的函数不标 clobber。
- `tests/native_register_effects_test.cpp:146`：断言未恢复 RBX 的函数标 clobber。

验证命令：

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_register_effects_test native_prototype_model_test native_abi_cspec_test -j2
ctest --test-dir build -R 'notdec.native_(abi.cspec|prototype_model.register|register_effects.preserved)' -V
```

结果：通过。`notdec.native_abi.cspec`、`notdec.native_prototype_model.register`、`notdec.native_register_effects.preserved` 全部通过，总用时 0.08 秒。

## 风险

- 当前 clobber 只覆盖 ABI unaffected register 被读入 external input 后未恢复的情况。
- 未被读取的 ABI unaffected register 不会标 clobber，因为当前 SSA 没有 entry input 证据。
- caller-saved / killedbycall 的 clobber 语义还没接入，留到 callsite effects。

## 评分

- 实现效果：7/10。能发现 saved register 写后未恢复的情况，满足阶段 3 的第一批用例。
- 复杂度：4/10。和 preserved 共用判断，代码变化不大。
- 维护成本：4/10。后续 caller-saved 和 call effect 扩展时需要把这个 metadata 规则拆得更清楚。
