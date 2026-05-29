# 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

# 背景

当前 native 侧已经能把 call 后同块、一层唯一后继、线性唯一后继链中的返回寄存器 load 改成新 call result，并且遇到同寄存器 store 会阻断。

还缺少几个 CFG 负例的回归测试：call 后多后继、目标块多前驱、线性链成环。这些形状现在应该保守跳过，不能误把某条路径上的寄存器 load 当成确定的 call 返回值。

# Ghidra 实现参考

Ghidra 不靠“沿唯一后继链找 load”这种局部模式完成返回值恢复，而是在 P-Code SSA 和 CFG 上做：

- `Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc`
  - `Heritage::placeMultiequals(...)`：在需要合流的位置放 `MULTIEQUAL`。
  - `Heritage::renameRecurse(...)`：按支配树重命名 varnode，读到未定义值时产生 input varnode，遇到分支合流由 `MULTIEQUAL` 表达。
  - `Heritage::heritage(...)`：组织整轮 heritage，让 register/stack varnode 有 SSA 定义链。
- `Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - `ActionReturnRecovery::apply(...)`：遍历 `CPUI_RETURN`，用 `ParamActive` / `ParamTrial` 和 ancestor 检查确认返回 trial 是否 active。
  - `ActionReturnRecovery::buildReturnOutput(...)`：把确认后的返回 varnode 按 prototype 输出顺序接到 `RETURN` op。
- 同文件 `ActionStackPtrFlow::repair(...)` 也有一个很接近的保守 CFG 策略：只在 `curblock->sizeIn() == 1` 时向前跨块追踪，遇到 call 或冲突 store 就停。

所以 Ghidra 的关键点是：复杂 CFG 靠 SSA/MULTIEQUAL 表达，不在有多路控制流时猜测单一路径的值。

# native 侧复刻策略

这一步不扩大实现范围，只把当前线性搜索的保守边界固定下来：

- call 所在块有多个后继：跳过。
- 后继块有多个前驱：跳过。
- 唯一后继链成环：不能卡住，也不能重写。

后续如果要支持这些形状，应该先补更完整的 SSA/PHI 语义，再让返回值使用从确定的数据流定义来，不在这里直接猜。

# 判断标准

- 新增三个负例测试。
- 现有正例和 clobber 负例不回退。
- `native_prototype_recovery_test` 通过。
- 复杂度不明显增加；这一步优先用测试覆盖现有策略。

# 风险

- 只补测试，不提高真实 CFG 支持能力。
- 多分支上真实安全的返回 load 仍会跳过，这是当前阶段接受的保守策略。

# 实现记录

实现时发现当前 `findCallsiteReturnLoad(...)` 对“找不到 load”和“CFG 不确定”都返回空结果。这样多后继或多前驱场景里，如果某条路径上有返回寄存器 load，签名重写可能继续发生，但旧 load 不会被新 call result 替换。这个行为不够安全，所以本步不只是补测试，也把 CFG 不确定改成阻断。

改动：

- `lib/passes/NativePrototypeRecovery.cpp:321` 的 `findCallsiteReturnLoad(...)`：遇到多个后继、后继多个前驱、前驱不是当前块、唯一后继链成环时返回 `Blocked=true`。
- `lib/passes/NativePrototypeRecovery.cpp:394` 已有 `callsiteHasMismatchedReturnLoad(...)` 会把 `Blocked=true` 当成 unsafe，所以 return-only / input-return direct callsite rewrite 会拒绝这些形状。
- `tests/native_prototype_recovery_test.cpp:252` 添加 `createReturnLoadMultiSuccessorCallerFunction(...)`，构造 call 后分支，其中一条路径读取 RAX。
- `tests/native_prototype_recovery_test.cpp:284` 添加 `createReturnLoadMultiPredecessorCallerFunction(...)`，构造返回 load 所在块有两个前驱。
- `tests/native_prototype_recovery_test.cpp:322` 添加 `createReturnLoadLoopCallerFunction(...)`，构造 call 后进入自环，确认搜索不会卡住也不会重写。
- `tests/native_prototype_recovery_test.cpp:1519` 添加共用断言 `expectReturnOnlyRewriteRejected(...)`。
- `tests/native_prototype_recovery_test.cpp:1537` 到 `tests/native_prototype_recovery_test.cpp:1598` 覆盖多后继、多前驱、环形链三个负例。

验证：

```sh
cmake -S . -B build \
  -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj \
  -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=ON \
  -DNOTDEC_BIN2LLVM_ENABLE_LIEF=ON
cmake --build build --target native_prototype_recovery_test -j2
ctest --test-dir build -R 'notdec.native_prototype_recovery.input_candidates' -V
```

结果：通过，`1/1 Test #4: notdec.native_prototype_recovery.input_candidates ... Passed`。

性能：这一步只在已有线性 successor 搜索里多返回 `Blocked=true`，没有扩大搜索范围。相关测试总耗时约 `0.04 sec`，无可见性能下降。

评分：

- 实现效果：7/10。修掉了 unsafe CFG 被改签名的风险，并补了回归测试。
- 复杂度：3/10。只复用已有三态结果，没有新增数据结构。
- 维护成本：3/10。后续如果实现 PHI/多分支返回值数据流，需要调整这些负例的期望。
