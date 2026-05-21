# 本次用户原始 prompt

继续按照规划完善 bin2llvm native 链路。上一小步已经把 unresolved indirect flow 记录下来；下一小步先补一个保守函数边界：direct branch 如果跳到已知的其他函数入口，不把目标 block 并进当前函数。

## 背景

Ghidra 侧相关实现：

- `Ghidra/Features/Base/src/main/java/ghidra/app/cmd/function/CreateFunctionCmd.java::createFunction(...)` 会先检查 entry 处是否已有函数，并在已有函数时走 `handleExistingFunction(...)`，不会盲目新建重叠函数。
- `CreateFunctionCmd.java::getFunctionBody(...)` 会调用 `FollowFlow` 计算函数体，并通过 `includeOtherFunctions` 控制是否跟进已有函数。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/block/FollowFlow.java::followInstruction(...)` 在处理 fallthrough 时，如果 `followIntoFunction == false` 且下一地址是函数 symbol，会停止继续跟随。
- `FollowFlow.java::getFlowsFromInstruction(...)` 处理显式 flow 时也会在 `followIntoFunction == false` 时跳过目标函数 symbol。

Rizin 侧相关实现：

- `/sn640/rizin/librz/include/rz_analysis.h` 中 `RzAnalysisFunction` 有独立的 `addr`、basic block 列表和 `_min/_max` 范围。
- 同文件提供 `rz_analysis_get_function_at(...)`、`rz_analysis_get_functions_in(...)`、`rz_analysis_function_contains(...)` 等接口，说明 Rizin 也把函数入口和函数包含关系作为分析边界使用。
- `RZ_ANALYSIS_OP_TYPE_TAIL` 明确标注 tail call 会结束当前 sub-routine，这也说明跳到其他函数入口时不能简单当作当前函数内部 block。

native 侧现在 direct branch successor 会按 `{currentFunctionEntry, target}` 入队。如果 target 正好是 `.eh_frame`、symbol 或 direct call 已知的另一个函数入口，当前逻辑会把它作为当前函数的 block 解码，容易把函数边界合并得过宽。

## 目标

本小步只做已知函数入口边界：

1. direct branch target 如果是已知 function seed，且不等于当前 function entry，不再作为当前函数 block 入队。
2. 这种 target 仍保留 direct flow xref。
3. 当前函数的 `NativeBasicBlock.Successors` 不记录这个跨函数 target，避免后续 lowering 把它当内部 CFG 边。
4. 不判断普通 label，不判断未发现的函数入口。
5. 不解析 tail call 语义，只做保守边界保护。

## 技术路线

- 新增小 helper 判断 `isKnownOtherFunctionEntry(state, currentEntry, target)`。
- `addDecodedFunctionBlocks(...)` 在收集 branch target 前，先过滤已知其他函数入口。
- 对同一个过滤逻辑同时作用于 block successor 和本地 decode 队列。
- `NativeXref` 仍在 `addDirectControlFlow(...)` 中保留，因为跨函数直接跳转依然是真实引用。

## 风险

- `.eh_frame` / symbol seed 可能有误，过早拦截会让少量真实 intra-procedural branch 不被跟随。
- 但 Bench2 当前目标是先语义保守，不把另一个明确函数入口并进当前函数，比扩大范围更安全。
- 这个保护不会解决“跳进未发现函数入口”的情况。

## 判断标准

- Bench2 三个 smoke 能跑通。
- confirmed functions 不异常减少。
- basic block / instruction 数可能小幅减少或不变。
- direct flow xref 数不应因为边界过滤消失。
- 时间不应明显增加。

## 实现记录

改动文件和函数：

- `lib/NativeAnalysis.cpp:1331`，`SleighSeedInstructionAnalyzer::addDecodedFunctionBlocks(...)`：在收集 branch target 和追加 block 前，先过滤已知其他函数入口 successor。
- `lib/NativeAnalysis.cpp:1494`，新增 `eraseKnownOtherFunctionSuccessors(...)`：从 block successor 列表里移除已知其他函数入口。
- `lib/NativeAnalysis.cpp:1506`，新增 `isKnownOtherFunctionEntry(...)`：如果 successor 是 `functionSeeds()` 中的地址，且不等于当前 entry，就认为它是其他函数入口。
- `ARCHITECTURE.md:59`：记录 direct branch 到其他 function seed 时只保留 xref，不并入当前函数。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:20`：记录本小步已完成。

实现效果：

- direct branch 到已知其他 function seed 时，不再作为当前函数 block 入队。
- 对应 direct flow xref 仍由 `addDirectControlFlow(...)` 保留。
- `NativeBasicBlock.Successors` 不再包含这种跨函数入口，减少后续 lowering 把它当内部 CFG 边的风险。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
```

验证结果：

- build：通过。
- `vsftpd`：function seeds 187，confirmed functions 9，basic blocks 31，instructions 80，xrefs 9，unresolved indirect flows 7，TIME 3.15。
- `libuv.so.1.0.0`：function seeds 486，`sleigh-direct-call: 2`，confirmed functions 10，basic blocks 32，instructions 93，xrefs 11，unresolved indirect flows 5，TIME 3.71。
- `memcached`：function seeds 259，confirmed functions 9，basic blocks 31，instructions 80，xrefs 9，unresolved indirect flows 7，TIME 3.32。

性能和效果判断：

- `vsftpd` 和 `memcached` 数量基本不变。
- `libuv.so.1.0.0` 的 confirmed functions / blocks / instructions / xrefs 下降，说明之前确实有 direct branch 进入已知其他函数入口，本小步把它拦住了。
- 时间没有明显增加。

评分：

- 实现效果：7/10。能挡住已知函数入口边界，但不能识别未知函数入口。
- 理解成本：8/10。只增加两个小 helper，逻辑直接。
- 维护成本：8/10。后续如果有更准确的 function boundary 模型，可以替换这个 seed-based 判断。

更好的方案：

- 后续应区分 tail call、thunk、PLT stub 和普通跨函数跳转。
- 还应让 `functionContaining(...)` 变成 block-aware 查询，避免 `RangeStart/RangeEnd` 覆盖空洞带来的误判。
