# 20260529-24 Callsite Return Linear Clobber Stop

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的 heritage 会把寄存器写入变成新的 SSA varnode。call 后如果同一个返回寄存器又被写了一次，后面的读取不再是 call output。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritage(...)`：按定义点给 varnode 建 SSA，新的写入会截断旧定义。
  - `Heritage::buildInfoList(...)`：收集 varnode 的定义和使用位置。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：基于 call prototype 更新 callsite 数据流。
  - `ActionReturnRecovery::apply(...)`：恢复返回 storage。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/funcdata.hh`
  - `FuncCallSpecs`：保存 callsite output 信息。

native 侧现在沿唯一后继链找返回寄存器 load，但还没检查链上是否先出现同寄存器 store。这会把“后续重新写入的 RAX”误当成 call 返回值。

## native 复刻方式

这一步只补线性链上的 clobber 阻断：

- 在查找返回 load 的同一段指令里，同时检查同 register store。
- 如果先遇到 load，就返回可替换 load。
- 如果先遇到同 register store，就停止查找，不替换。
- 继续保留唯一后继链、多后继保守停止、visited 防循环的条件。

暂不做：

- 识别 store 的值是否等价于 call result。
- PHI 合流和多分支路径分析。
- 栈返回值。

## 判断标准

- 已有同 block、一层后继、线性后继链成功用例不退化。
- 线性链上先 store RAX 后 load RAX 时，不能把 load 替换成 call result。
- prototype recovery 单测通过。

## 实现记录

已实现。

源码改动：

- `lib/passes/NativePrototypeRecovery.cpp:294` 添加 `ReturnLoadSearchResult`，区分找到 load、被同 register store 阻断、没有结果。
- `lib/passes/NativePrototypeRecovery.cpp:299` 添加 `findReturnLoadBeforeStoreInRange(...)`，同一段指令里先遇到返回 register load 就返回，先遇到同 register store 就阻断。
- `lib/passes/NativePrototypeRecovery.cpp:321` 调整 `findCallsiteReturnLoad(...)`，沿线性唯一后继链传播三态结果。
- `lib/passes/NativePrototypeRecovery.cpp:394` 添加 `callsiteHasMismatchedReturnLoad(...)`，把 blocked 和类型不匹配都当成不安全 callsite。
- `lib/passes/NativePrototypeRecovery.cpp:403` 调整 `collectReturnOnlyDirectCallsites(...)`，拒绝被 store 阻断的返回值 callsite；没有返回 load 的旧调用点仍允许重写并丢弃 call result。
- `tests/native_prototype_recovery_test.cpp:216` 添加 `createReturnClobberLinearSuccessorCallerFunction(...)`，构造 call 后先 store RAX 再 load RAX 的形状。
- `tests/native_prototype_recovery_test.cpp:1401` 添加 `native-prototype-return-clobber-callsite-rewrite-test`，确认这种形状不会被签名重写。

实现时调整：

- 最初按“必须找到可替换 load 才能重写”做，发现会破坏已有“直接 caller 不读取返回值也能重写”的测试。
- 最终改成只拒绝 blocked 或 load 类型不匹配；完全没有返回 load 时仍保留旧行为。

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

- 仍只在线性唯一后继链里扫描。
- 每条路径遇到同 register store 会提前停止，避免继续扫无关 block。

风险和后续：

- 只识别带 `notdec.register.access` metadata 的 store。缺 metadata 的返回寄存器写入仍可能漏掉，需要后续从 register SSA 结果补更强的数据流判断。
- 多分支、PHI、栈返回值仍不处理。

评分：

- 实现效果：8/10，修掉一个明确错误重写形状。
- 复杂度：4/10，引入三态结果但局部可控。
- 维护成本：4/10，后续扩展 CFG 数据流时可以替换这段线性查找。
