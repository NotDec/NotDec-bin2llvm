# 20260529-19 Input-only Signature Rewrite

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 会把恢复出的输入参数和函数体里的 input varnode 对上，后续类型和参数列表都围绕这些 input varnode 继续传播。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `ParamActive`：保存输入参数 trial。
  - `ParamActive::registerTrial(...)`：把 input varnode 的 storage 加入 trial。
  - `ProtoModel::deriveInputMap(...)` / `FuncProto::deriveInputMap(...)`：从 input trials 派生最终输入参数。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionInputPrototype::apply(...)`：遍历 `Varnode::input`，对可能是参数的 varnode 注册 trial，再更新 input prototype。
  - `ActionPrototypeTypes::apply(...)`：当输入 prototype 已锁定时，用 `setInputVarnode(...)` 创建 input varnode，并标记 locked input。

native 侧已经能把 recovered input 绑定到唯一 `external_input` load。现在可以做最小闭环：把 `void()` 改成 `void(i64)`，并把该 load 的 uses 替换成新 LLVM argument。

## native 复刻方式

这一步只处理最小安全场景：

- recovered prototype 必须能形成 `void(i64)`。
- 原函数必须是 `void()`。
- recovered input 必须只有一个。
- recovered return 必须为空。
- 必须能唯一绑定 input load。
- 函数必须没有 uses，先不做 callsite 重写。

实现方式：

- 新增 `rewriteNativeRecoveredPrototypeInputOnly(...)`。
- 创建一个新函数，类型用 recovered `FunctionType`。
- 把原函数 basic blocks 移到新函数。
- 用新函数第 0 个 argument 替换绑定的 `external_input` load uses。
- 如果 load 没有剩余 uses，删除该 load。
- 复制函数级 metadata。
- 新函数接管原函数名字，删除旧函数。

这一步不处理 callsite、不处理多个参数、不处理返回值、不处理 stack 参数。

## 判断标准

- `void @input_rdi_bindable()` 改成 `void @input_rdi_bindable(i64)`。
- 函数体里原 `RDI.external_input` load 的 uses 被新 argument 替换。
- 原 load 被删除。
- 原函数没有 uses 时可以改写。
- 有调用者、没有唯一 input binding、带返回值的函数都不改写。

## 实现记录

### 源码改动

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h:132`
  - 新增 `rewriteNativeRecoveredPrototypeInputOnly(...)`。
- `lib/passes/NativePrototypeRecovery.cpp:693`
  - 实现 `rewriteNativeRecoveredPrototypeInputOnly(...)`。
  - 只允许无 uses、原型为 `void()`、recovered prototype 为 `void(i64)`、单 recovered input、无 recovered return、且唯一 input binding 的函数改写。
  - 创建新函数并接管原函数名，用 `Function::splice(...)` 搬 basic blocks。
  - 用新函数第 0 个 argument 替换绑定的 `external_input` load uses。
  - load 没有剩余 uses 时删除旧 load。
  - 复制函数属性和函数级 metadata，最后删除旧函数。
- `tests/native_prototype_recovery_test.cpp:118`
  - 复用 `createCallerFunction(...)`，构造有调用者的 input-only 负例。
- `tests/native_prototype_recovery_test.cpp:543`
  - 新增 `input_rdi_bindable_used` 和调用者，验证有 uses 时不改写。
- `tests/native_prototype_recovery_test.cpp:611`
  - 新增测试函数后，summary 期望调整为 19 个函数、11 个 external input、8 个 input candidate、11 个 rewrite eligible、10 个 needs rewrite。
- `tests/native_prototype_recovery_test.cpp:733`
  - 验证有调用者的 input-only 函数不改写，原因是 `function has uses`。
- `tests/native_prototype_recovery_test.cpp:740`
  - 验证无调用者的 `input_rdi_bindable` 能改成 `void(i64)`。
  - 验证原 `external_input` load 的 use 被新 argument 替换。
  - 验证改写后的函数体里不再有 `notdec.register.external_input` load。

### 验证

```sh
cmake -S . -B build \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON
cmake --build build --target native_prototype_recovery_test native_instcombine_metadata_test -j2
ctest --test-dir build -R 'notdec.native_(prototype_recovery.input_candidates|instcombine.metadata)' -V
```

结果：`notdec.native_prototype_recovery.input_candidates` 和 `notdec.native_instcombine.metadata` 通过。

### 性能和风险

- 性能：helper 只有显式调用时才做检查、创建新函数、搬 basic blocks 和替换 load uses，当前默认 pass 不调用，主链路没有额外开销。
- 风险：这一步故意拒绝有 uses 的函数，callsite 重写还没接上；也不处理多个参数、返回值、stack 参数。
- 实现效果：7/10。已经打通 input-only 的真实 LLVM 参数签名和 entry load 替换，但还不是完整 prototype rewrite。
- 复杂度：4/10。继续改 IR 结构，但范围只限无调用者 input-only 函数。
- 维护成本：4/10。后续需要合并 input 和 return 的 rewrite 流程，并补 callsite 重写。
