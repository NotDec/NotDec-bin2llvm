# 20260529-19 Return Callsite Result Load Rewrite

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的返回值恢复不只改 callee 类型，也会让 callsite 的返回 varnode 使用 call result。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionReturnRecovery::apply(...)`：恢复输出 storage。
  - `ActionOutputPrototype::apply(...)`：把输出 storage 写入 prototype。
  - `ActionPrototypeTypes::apply(...)`：把 prototype 类型应用到 call 和 return。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
  - `FuncProto::deriveOutputMap(...)`：形成返回 storage 列表。
  - `FuncProto::updateAllTypes(...)`：把返回类型传播到调用点。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/funcdata.hh`
  - `FuncCallSpecs`：保存 callsite 使用的返回 storage 和类型。

native 侧当前 return-only direct callsite 已能把旧 void call 改成返回 `i64` 的 call，但结果暂时 unused。下一步先把 call 后同一 basic block 内的 RAX register load 替换成 call result。

## native 复刻方式

这一步只做 return-only direct callsite 的最小结果接线：

- 只处理 recovered return 是单个 register，例如当前测试里的 `RAX`。
- 在旧 void call 后、同一 basic block 内向后找第一个同 register 的 `notdec.register.access` load。
- 如果 load 类型等于 recovered return type，就把 load uses 替换成新 call result，并删除空 load。
- 找不到 load 时仍允许重写 callsite，保持上一小步行为。
- 遇到类型不匹配时保守不替换 load。

暂不做：

- 跨 basic block 查找返回值使用。
- 替换 store 到 RAX 的模式。
- 多返回值。
- input-return callsite 的返回值接线。

## 判断标准

- 旧 return-only direct callsite rewrite 行为不变。
- call 后有同 block RAX load 时，该 load 的使用改用新 call result。
- 找不到 RAX load 时仍能生成返回 i64 的 call。
- prototype recovery 单测通过。

## 实现记录

已实现。

### 改动位置

- `lib/passes/NativePrototypeRecovery.cpp:296` 的 `rewriteReturnOnlyDirectCallsites(...)` 增加 `returnRegisterName` 参数。
- `lib/passes/NativePrototypeRecovery.cpp:304` 在旧 void call 后、同一 basic block 内向后查第一个同 register 的 `notdec.register.access` load。
- `lib/passes/NativePrototypeRecovery.cpp:319` 在 load 类型等于新 call result 类型时，用新 call 替换 load uses，并删除空 load。
- `lib/passes/NativePrototypeRecovery.cpp:807` 的 `rewriteNativeRecoveredPrototypeReturnOnly(...)` 把 recovered return register 传给 callsite rewrite。
- `tests/native_prototype_recovery_test.cpp:133` 新增 `createReturnLoadCallerFunction(...)`，构造 call 后读取 RAX 的 caller。
- `tests/native_prototype_recovery_test.cpp:1010` 更新 return-only callsite 测试，验证旧 RAX load 被删除，新 call result 有 use。

### 验证

命令：

```sh
cmake -S . -B build \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON &&
cmake --build build --target native_prototype_recovery_test -j2 &&
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
```

结果：1 个测试通过。

### 性能和风险

- 默认不开签名重写时不走这段逻辑。
- 开启 return-only direct callsite rewrite 时，只在每个 direct call 所在 basic block 内向后扫描到第一个匹配 load。
- 找不到 load 时保持上一小步行为：仍生成返回 i64 的 call。
- 当前不跨 basic block，不处理 PHI，不处理 call 后 store 到 RAX，不处理 input-return 的返回值接线。

### 评分

- 实现效果：7/10。return-only callsite 的直接返回值使用已经能接到 LLVM call result。
- 复杂度：7/10。逻辑集中在 direct callsite 替换处，但扫描和删除 load 需要小心迭代器。
- 维护成本：7/10。后续 input-return 和跨 block 场景应复用同一套返回值接线 helper。
