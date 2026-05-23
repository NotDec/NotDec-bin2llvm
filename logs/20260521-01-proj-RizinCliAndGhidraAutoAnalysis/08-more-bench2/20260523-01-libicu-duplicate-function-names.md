# Libicu Duplicate Function Names

## 原始 prompt

```text
参考logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/GOAL.md，当前目标是进一步拓展最后一步，完善bench2上的效果。对于/sn640/NotDec-Exp/Bench2/plan.md里面的每个目标项目，都选择一个动态链接库或者二进制程序，修复转IR过程的问题，同时解决生成的IR里面明显的语义不一致。过程中的log文件记录到logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/08-more-bench2文件夹内。
```

## 当前目标和已有 native 状态

Bench2 计划里 libicu 优先使用动态库。现有动态库导出结果中：

- `libicudata.so.74.2` 已能完整 lower。
- `libicuuc.so.74.2` 和 `libicui18n.so.74.2` 只停在 JSON。

当前失败点不是 `llvm-as`，而是 module check 把 C++ 里大量同名成员函数、构造函数、析构函数当作错误。实际身份应由函数入口地址区分，名字只能做 LLVM 符号基名。

## Ghidra 相关实现

Ghidra 的函数管理和符号管理不会把短名字当作函数唯一身份：

- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/Function.java`
  - `getEntryPoint()` 是函数的地址身份。
- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/FunctionManager.java`
  - `getFunctionAt(...)` 按入口地址取函数。
- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/SymbolTable.java`
  - 名字可以重复，命名空间和地址共同参与区分。

## native 侧复刻策略

1. module check 保留 duplicate name 计数，但只把 duplicate entry 当错误。
2. LLVM module 继续用 `name + entry` 生成唯一符号。
3. direct call 解析优先用 `callTarget` 地址。只有原始名字在 module 内唯一时，才允许用名字兜底，避免同名函数被连到第一个同名符号。

## 判断标准

1. 当前 libicu common-library JSON 通过 module check。
2. common-library 能生成 `.ll`，并通过 LLVM 22 `llvm-as` 与 `opt -passes=verify`。
3. 生成 IR 中没有因为同名函数导致的重复 LLVM 符号。

## 风险

1. 如果 Ghidra JSON 对某些 call 只给同名 `callTargetName`，没有地址，且该名字不唯一，本次会让该函数 body lower 失败并保留声明。这比误连到错误函数更保守。
2. common-library JSON 很大，验证时间会明显长于 smoke 里的小用例。

## 实现记录

### 修改内容

- `lib/HeritageToLLVM.cpp:188`
  - 扩展 `HeritageModuleSymbolPlan`，增加 `UniqueNameByOriginalName` 和 `AmbiguousOriginalNames`。
  - 设计点：函数身份仍以入口地址为准；短名字只在全 module 唯一时用于 call 兜底。
- `lib/HeritageToLLVM.cpp:200`
  - `planModuleSymbols(...)` 先统计原始名字出现次数。
  - 对重复短名，只记录为 ambiguous，不再把第一个同名函数放进名字反查表。
- `lib/HeritageToLLVM.cpp:287`
  - `resolveCallTargetName(...)` 继续优先按 `callTarget` 地址解析。
  - 对 ambiguous 短名返回空目标，让当前函数 body lower 失败并回退成声明，避免生成错误内部 call。

### 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-heritage-module-check notdec-heritage-module-llvm -j2
```

libicu data-library 回归：

```bash
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/dynamic-libs/libicu/data-library/module-all.json \
  -o /tmp/notdec-libicu-data-regress.ll
```

结果：

```text
internal declarations: 5
external declarations: 2
lowered function bodies: 5
failed function bodies: 0
TIME 0.02
```

libicu common-library check：

```bash
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-check \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/dynamic-libs/libicu/common-library/module-all.json
```

结果：

```text
functions: 4517
externals: 85
failures: 0
duplicate function names: 804
direct calls: 16277
resolved internal calls: 12860
resolved external calls: 3417
unknown calls: 0
status: ok
TIME 133.94
```

libicu common-library lower：

```bash
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/dynamic-libs/libicu/common-library/module-all.json \
  -o /tmp/notdec-libicu-common.ll
```

结果：

```text
internal declarations: 4517
external declarations: 85
lowered function bodies: 4487
failed function bodies: 30
TIME 149.81
```

30 个失败 body 里，一部分是 `CALL needs resolvable target and target input`。这是本次对重复短名的保守处理结果：没有地址时不按同名函数误连。

LLVM 22 验证：

```bash
/usr/bin/time -f 'TIME %e' /sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/notdec-libicu-common.ll -o /tmp/notdec-libicu-common.bc
/usr/bin/time -f 'TIME %e' /sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-libicu-common.bc -o /tmp/notdec-libicu-common.verified.bc
git diff --check
```

结果：全部通过。`llvm-as` 28.13s，`opt -passes=verify` 16.44s。生成的 common-library IR 为 744685 行，39M。

### 性能和判断

这次没有改 JSON 解析和 lowering 主循环，只在 module 符号规划里增加一次名字计数，成本是 O(functions + externals)。对 libicu common-library 这种 4517 函数的大 JSON，check 133.94s、lower 149.81s，主要时间仍在大 JSON 读入和 IR 构造。

实现效果：4/5。libicu common-library 从 check 阶段失败推进到可生成并验证完整 module IR；同名短名不再误解析到第一个同名函数。

复杂度：2/5。只改 module 符号表规划和 call 解析，不碰 P-Code lowering 主逻辑。

维护成本：2/5。后续如果 JSON 导出命名空间或完整 mangled 名，可把 ambiguous 短名失败进一步恢复为精确解析。
