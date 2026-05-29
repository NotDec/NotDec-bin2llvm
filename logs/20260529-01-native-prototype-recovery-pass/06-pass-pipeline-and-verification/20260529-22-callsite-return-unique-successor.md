# 20260529-22 Callsite Return Unique Successor

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 在 SSA heritage 后，call 返回值可以被后继 basic block 使用，不要求 CALL 和读取返回 register 的 p-code 在同一个 block。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritage(...)`：为 varnode 建 SSA，跨 basic block 追踪定义。
  - `Heritage::buildInfoList(...)`：收集 block 内和 block 间的 varnode 信息。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionReturnRecovery::apply(...)`：恢复返回 storage。
  - `ActionPrototypeTypes::apply(...)`：把返回类型应用到 callsite。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/funcdata.hh`
  - `FuncCallSpecs`：保存 callsite 返回值使用的 prototype 信息。

native 侧当前只在 call 后同一 basic block 内查返回 register load。下一步先支持最小跨块形状：call block 有唯一后继，且后继只有这个前驱时，从后继开头查返回 register load。

## native 复刻方式

这一步只扩展 callsite return load 查找：

- 仍优先查 call 所在 basic block 内 call 后的同 register load。
- 如果同 block 找不到，且 call block 只有一个后继、后继只有这个前驱，就在后继开头向后查同 register load。
- load 类型等于新 call result 类型时，用 call result 替换 load uses，并删除空 load。
- 多后继、多前驱、找不到 load、类型不匹配时仍不替换。
- 复用 return-only 和 input-return callsite result load rewrite。

暂不做：

- 多后继 PHI。
- 跨多层后继递归查找。
- call 后 store 到 RAX 的替换。
- 栈返回值。

## 判断标准

- 同 block 返回 load 的旧行为不变。
- call block 有唯一后继且后继唯一前驱时，return-only callsite 的后继 RAX load 能替换成 call result。
- 多后继或多前驱时不替换。
- prototype recovery 单测通过。

## 实现记录

已实现。

源码改动：

- `lib/passes/NativePrototypeRecovery.cpp:293` 添加 `findReturnLoadInRange(...)`，把同一段指令范围内查 `notdec.register.access` load 的逻辑独立出来。
- `lib/passes/NativePrototypeRecovery.cpp:314` 添加 `findCallsiteReturnLoad(...)`，先查 call 后同 block，再查唯一后继且后继唯一前驱的 block。
- `lib/passes/NativePrototypeRecovery.cpp:348` 调整 `rewriteCallsiteReturnLoad(...)`，复用新的查找函数，找到类型一致的 load 后用新 call result 替换。
- `tests/native_prototype_recovery_test.cpp:157` 添加 `createReturnLoadUniqueSuccessorCallerFunction(...)`，构造 call 后跳到唯一后继再读取返回寄存器的测试形状。
- `tests/native_prototype_recovery_test.cpp:1209` 添加 `native-prototype-return-successor-callsite-rewrite-test`，确认后继 block 的 RAX load 被新 call result 替换。

验证：

```sh
cmake -S . -B build \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
```

结果：`notdec.native_prototype_recovery.input_candidates` 通过。

性能影响：

- 新逻辑只在签名重写 callsite 时执行。
- 每个 callsite 最多多扫一个唯一后继 block，不进入递归 CFG 查找，当前测试没有发现可见性能风险。

风险和后续：

- 多后继和多前驱仍保守不替换，避免错误跨路径使用 call result。
- 还没有处理多层后继、PHI 合流、栈返回值。

评分：

- 实现效果：7/10，覆盖当前一层唯一后继用例。
- 复杂度：3/10，只拆了两个小查找函数。
- 维护成本：3/10，CFG 条件直接，后续可以在同一入口扩展。
