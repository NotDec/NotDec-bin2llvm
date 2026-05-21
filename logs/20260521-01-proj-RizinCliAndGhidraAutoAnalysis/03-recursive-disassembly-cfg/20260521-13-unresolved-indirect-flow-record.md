# 本次用户原始 prompt

继续按照规划完善 bin2llvm native 链路。上一小步已经能受控跟随 direct branch successor；下一小步先把未解析的间接 call / branch 记录下来，不急着解析跳表或函数指针。

## 背景

Ghidra 侧相关实现：

- `Ghidra/Features/Base/src/main/java/ghidra/app/cmd/function/CreateFunctionCmd.java::getFunctionBody(...)` 构造 `dontFollow`，明确不跟随 `COMPUTED_CALL` 和 `INDIRECTION`，所以 computed call / pointer indirection 不会直接扩进函数 body。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/block/FollowFlow.java::shouldFollowFlow(...)` 按 `RefType.COMPUTED_CALL`、`RefType.COMPUTED_JUMP`、`RefType.INDIRECTION` 等类型决定是否跟随。
- `FollowFlow.java::getFlowsFromInstruction(...)` 只把 `shouldFollowFlow(...)` 允许的 flow 地址加入待处理列表。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/RefType.java` 里 `COMPUTED_JUMP` 标记 `.setIsJump().setIsComputed()`，`COMPUTED_CALL` 标记 `.setHasFall().setIsCall().setIsComputed()`。

Rizin 侧相关实现：

- `/sn640/rizin/librz/include/rz_analysis.h` 的 `_RzAnalysisOpType` 把 direct 和 unknown / indirect flow 分开：`RZ_ANALYSIS_OP_TYPE_UJMP`、`RZ_ANALYSIS_OP_TYPE_IJMP`、`RZ_ANALYSIS_OP_TYPE_UCALL`、`RZ_ANALYSIS_OP_TYPE_ICALL` 等。
- `/sn640/rizin/librz/core/disasm.c` 多处按 `RZ_ANALYSIS_OP_TYPE_UJMP`、`RZ_ANALYSIS_OP_TYPE_UCALL` 系列类型做展示和分析分支，说明 unknown indirect flow 是先分类记录，再由后续分析决定是否能解析。

native 侧现在只把 `BRANCHIND` 当成 block 结束，没有可查询的记录；`CALLIND` 还完全忽略。这样后续没法知道哪些函数卡在间接 call / jump 上，也没法为跳表和函数指针恢复列样本。

## 目标

本小步只做记录，不做解析：

1. 新增 native unresolved indirect flow 状态。
2. 记录 `CALLIND` 和 `BRANCHIND` 的地址、类型和来源。
3. `CALLIND` 不切 block，不入队，保持 fallthrough。
4. `BRANCHIND` 继续结束当前 block，不加 successor。
5. report 输出 unresolved indirect flow 总数和 call / branch 分类数。
6. 不解析跳表，不猜函数指针目标。

## 技术路线

- 在 `NativeAnalysis.h` 加 `NativeUnresolvedFlowKind` 和 `NativeUnresolvedFlow`。
- `NativeProgramState` 增加 `unresolvedFlows()` 和 `addUnresolvedFlow(...)`。
- `SleighSeedInstructionAnalyzer::addDirectControlFlow(...)` 遇到 `CallInd` / `BranchInd` 时写入 unresolved flow。
- 为避免同一条指令多个 P-Code op 重复记录，按 `{address, kind}` 去重。
- `ReportAnalyzer` 输出 unresolved indirect flows 的总数和分类数。

## 风险

- Sleigh P-Code 里一条指令可能产生多个相关 op，本小步只按指令地址和 kind 去重，后续如果要记录 operand，需要更细。
- Bench2 当前 bounded decode 上限小，未必每个样例都会遇到 indirect flow。
- 这一步只增强可观测性，不会让 CFG 覆盖率明显变大。

## 判断标准

- Bench2 三个 smoke 能跑通。
- report 中出现 `unresolved indirect flows`。
- 如果当前样例 bounded decode 遇到 `CALLIND` / `BRANCHIND`，分类计数应大于 0。
- confirmed functions / blocks / instructions 不应回退。
- 时间仍在小范围内。

## 实现记录

改动文件和函数：

- `include/notdec-bin2llvm/NativeAnalysis.h:137`：新增 `NativeUnresolvedFlowKind`，先区分 indirect call 和 indirect branch。
- `include/notdec-bin2llvm/NativeAnalysis.h:176`：新增 `NativeUnresolvedFlow`，记录地址、类型和来源。
- `include/notdec-bin2llvm/NativeAnalysis.h:237`、`include/notdec-bin2llvm/NativeAnalysis.h:262`、`include/notdec-bin2llvm/NativeAnalysis.h:285`：给 `NativeProgramState` 增加 `unresolvedFlows()`、`addUnresolvedFlow(...)` 和内部存储。
- `lib/NativeAnalysis.cpp:1375`，`SleighSeedInstructionAnalyzer::addDirectControlFlow(...)`：遇到 `CALLIND` 写入 unresolved indirect call。
- `lib/NativeAnalysis.cpp:1394`，`SleighSeedInstructionAnalyzer::addDirectControlFlow(...)`：遇到 `BRANCHIND` 写入 unresolved indirect branch，并继续把 block 标记为 indirect branch 结束。
- `lib/NativeAnalysis.cpp:1616`，`ReportAnalyzer::run(...)`：report 输出 unresolved indirect flows 总数和分类数。
- `lib/NativeAnalysis.cpp:1748`：新增 `toString(NativeUnresolvedFlowKind)`。
- `lib/NativeAnalysis.cpp:2037`，`NativeProgramState::addUnresolvedFlow(...)`：按地址和类型去重，避免同一条指令重复记录。
- `ARCHITECTURE.md:52`：补充 unresolved indirect flow 的状态和 report 行为。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:19`：记录本小步已完成。

实现效果：

- `CALLIND` / `BRANCHIND` 不再只是被忽略或只影响 block 切分，而是能在 native state 里查询。
- `CALLIND` 不切 block，不入队；`BRANCHIND` 仍结束 block，不加 successor。
- 这一步没有解析跳表，也没有猜函数指针目标。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
```

验证结果：

- build：通过。
- `vsftpd`：function seeds 187，confirmed functions 9，basic blocks 31，instructions 80，xrefs 9，unresolved indirect flows 7，其中 indirect call 1、indirect branch 6，TIME 3.31。
- `libuv.so.1.0.0`：function seeds 488，`sleigh-direct-call: 4`，confirmed functions 11，basic blocks 35，instructions 107，xrefs 17，unresolved indirect flows 8，其中 indirect call 1、indirect branch 7，TIME 3.93。
- `memcached`：function seeds 259，confirmed functions 9，basic blocks 31，instructions 80，xrefs 9，unresolved indirect flows 7，其中 indirect call 1、indirect branch 6，TIME 3.04。

性能和效果判断：

- confirmed function / block / instruction / xref 数量没有回退。
- 三个样例都出现 unresolved indirect flow，说明当前 bounded decode 已经能采到后续要处理的间接控制流样本。
- 时间相比上一轮略有波动，主要仍是 Sleigh decode 成本；本小步只追加少量状态写入，不应成为主要性能来源。

评分：

- 实现效果：7/10。已把间接 flow 变成可观测状态，但还没有解析目标。
- 理解成本：8/10。新增状态很小，和 xref 分开，避免用无效 to 地址污染 xref。
- 维护成本：8/10。后续跳表和函数指针分析可以直接消费这个列表。

更好的方案：

- 后续给 unresolved flow 增加 operand 摘要或 P-Code 输入摘要，便于区分 jump table、vtable call、PLT 间接跳转。
- 当前先不加，是因为还没有明确消费方，避免过早固定字段。
