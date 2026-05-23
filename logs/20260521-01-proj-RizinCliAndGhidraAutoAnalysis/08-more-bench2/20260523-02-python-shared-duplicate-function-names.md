# Python Shared Duplicate Function Names

## 原始 prompt

```text
参考logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/GOAL.md，当前目标是进一步拓展最后一步，完善bench2上的效果。对于/sn640/NotDec-Exp/Bench2/plan.md里面的每个目标项目，都选择一个动态链接库或者二进制程序，修复转IR过程的问题，同时解决生成的IR里面明显的语义不一致。过程中的log文件记录到logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/08-more-bench2文件夹内。
```

## 当前目标和已有 native 状态

Bench2 计划里 python 优先选动态库：

```text
/usr/lib/x86_64-linux-gnu/libpython3.12.so.1.0
```

现有产物已经有 Ghidra 导出的 `module-all.json`，但旧 check 停在重复函数名：

```text
error: duplicate function name: caseD_6
error: duplicate function name: default
```

这和 libicu 的问题一致：短名字不是函数身份。Python 的 Ghidra 导出里有 switch case 辅助函数和 `default` 这类重复短名，不能用名字唯一性判断函数唯一性。

## Ghidra 相关实现

Ghidra 的函数身份仍然按地址管理：

- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Function.java`
  - `getEntryPoint()` 返回函数入口地址。
- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/FunctionManager.java`
  - `getFunctionAt(...)` 用入口地址查询函数。
- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/SymbolTable.java`
  - 符号名字不是全程序唯一身份。

## native 侧复刻策略

本次复用上一小步已经提交的 module 符号修复：

1. `lib/HeritageToLLVM.cpp:188` 的 `HeritageModuleSymbolPlan` 保存按入口地址的 `NameByEntry`。
2. `lib/HeritageToLLVM.cpp:200` 的 `planModuleSymbols(...)` 统计原始短名次数。
3. `lib/HeritageToLLVM.cpp:287` 的 `resolveCallTargetName(...)` 只在短名唯一时用名字兜底；重复短名不误连。

## 判断标准

1. `libpython3.12.so.1.0` 的 module check 从 duplicate name 失败变成 ok。
2. 完整 module 能生成 `.ll`。
3. `.ll` 能用 LLVM 22 `llvm-as` 汇编，并通过 `opt -passes=verify`。

## 风险

1. Python module 很大，JSON 读入和 IR 验证时间都明显长。
2. 当前仍有部分函数因 ambiguous branch 回退为声明，这是 CFG 精确性问题，不是本次短名身份问题。

## 实现记录

### 修改内容

没有新增代码改动。本目标验证的是提交 `9a6f77c Fix ambiguous heritage module symbol lookup` 对 python shared-library 的效果。

相关代码位置：

- `lib/HeritageToLLVM.cpp:188`
  - `HeritageModuleSymbolPlan` 区分入口地址映射、唯一短名映射和重复短名集合。
- `lib/HeritageToLLVM.cpp:200`
  - `planModuleSymbols(...)` 先统计短名出现次数。
- `lib/HeritageToLLVM.cpp:287`
  - `resolveCallTargetName(...)` 优先按地址解析 call；重复短名不做名字兜底。

### 验证

module check：

```bash
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-check \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/dynamic-libs/python/shared-library/module-all.json
```

结果：

```text
functions: 6966
externals: 508
failures: 0
duplicate function names: 3
direct calls: 57006
resolved internal calls: 52328
resolved external calls: 4678
unknown calls: 0
status: ok
TIME 369.57
```

完整 lower：

```bash
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/dynamic-libs/python/shared-library/module-all.json \
  -o /tmp/notdec-python-shared.ll
```

结果：

```text
internal declarations: 6966
external declarations: 508
lowered function bodies: 6733
failed function bodies: 233
TIME 406.90
```

生成 IR：

```text
1779342 /tmp/notdec-python-shared.ll
89M /tmp/notdec-python-shared.ll
```

LLVM 22 验证：

```bash
/usr/bin/time -f 'TIME %e' /sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/notdec-python-shared.ll -o /tmp/notdec-python-shared.bc
/usr/bin/time -f 'TIME %e' /sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-python-shared.bc -o /tmp/notdec-python-shared.verified.bc
```

结果：全部通过。`llvm-as` 64.52s，`opt -passes=verify` 37.41s。

### 性能和判断

这次没有新增代码；性能数据反映当前大 module 的端到端成本。check 369.57s，lower 406.90s，主要成本来自 1.7GB JSON 读入和 89M LLVM IR 生成。

实现效果：4/5。python shared-library 从 check 阶段失败推进到完整 module IR 可汇编、可 verify。

复杂度：1/5。本目标只是复用已有入口地址优先的符号解析修复。

维护成本：1/5。后续需要继续解决 233 个 ambiguous branch body 回退，但这不影响当前 module 生成和验证。
