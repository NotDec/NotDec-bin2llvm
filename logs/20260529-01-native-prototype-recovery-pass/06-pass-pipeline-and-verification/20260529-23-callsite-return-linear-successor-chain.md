# 20260529-23 Callsite Return Linear Successor Chain

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的返回值使用不是按“相邻 basic block”硬匹配。heritage 先把寄存器 varnode 做 SSA，prototype 阶段再在 SSA 图上看 call output 是否被使用。

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::heritage(...)`：跨 block 给 varnode 建 SSA。
  - `Heritage::buildInfoList(...)`：为 block 和 varnode 准备 heritage 信息。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionPrototypeTypes::apply(...)`：把 recovered prototype 应用到 callsite。
  - `ActionReturnRecovery::apply(...)`：处理返回 storage 恢复。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/funcdata.hh`
  - `FuncCallSpecs`：保存 callsite prototype 和返回 storage 信息。

native 侧还没有完整 SSA value use 图。上一小步只支持 call block 的一层唯一后继。这一步继续保守扩展到“唯一后继链”：每一段都只有一个后继，且后继只有当前 block 一个前驱。

## native 复刻方式

这一步只改 return load 查找：

- 仍优先查 call 后同 block。
- 同 block 找不到时，沿唯一后继链向前走。
- 每个后继必须只有当前 block 一个前驱，否则停止并不替换。
- 找到第一个匹配返回寄存器的 load 后，仍要求 load 类型等于新 call result 类型。
- 用 visited 集合防止异常 CFG 造成循环。

暂不做：

- 分支合流 PHI。
- 多后继路径共同使用返回值。
- 跨 block 检查返回寄存器被重新 store 后的覆盖关系。
- 栈返回值。

## 判断标准

- 旧的同 block 和一层唯一后继用例不退化。
- call 后经过一个中间 block，再到唯一后继读取返回寄存器时，load 能被新 call result 替换。
- prototype recovery 单测通过。

## 实现记录

已实现。

源码改动：

- `lib/passes/NativePrototypeRecovery.cpp:315` 扩展 `findCallsiteReturnLoad(...)`，从一层唯一后继改成沿唯一后继链查找。
- `lib/passes/NativePrototypeRecovery.cpp:324` 增加 visited 集合，避免异常 CFG 循环。
- `tests/native_prototype_recovery_test.cpp:184` 添加 `createReturnLoadLinearSuccessorCallerFunction(...)`，构造 call 后经过一个中间 block 再读取 RAX 的形状。
- `tests/native_prototype_recovery_test.cpp:1303` 添加 `native-prototype-return-linear-callsite-rewrite-test`，确认线性后继链上的返回寄存器 load 会被新 call result 替换。

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

- 只在显式签名重写 callsite 时执行。
- 每个 callsite 最多沿一条唯一后继链扫描，visited 防止循环；真实大函数里如果有很长线性链会多扫一些 block，但不做分支展开。

风险和后续：

- 还没有判断链上是否有返回寄存器重新 store。当前测试形状没有这种覆盖，后续需要补“链上 clobber 停止”。
- 多分支和 PHI 仍保守不处理。

评分：

- 实现效果：7/10，覆盖更常见的线性拆块形状。
- 复杂度：4/10，增加循环但仍只沿唯一路径。
- 维护成本：4/10，后续要在同一循环里补 clobber 检查。
