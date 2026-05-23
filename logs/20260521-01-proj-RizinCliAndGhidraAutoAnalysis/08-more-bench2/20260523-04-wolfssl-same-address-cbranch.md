# Wolfssl Same Address Cbranch

## 原始 prompt

```text
参考logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/GOAL.md，当前目标是进一步拓展最后一步，完善bench2上的效果。对于/sn640/NotDec-Exp/Bench2/plan.md里面的每个目标项目，都选择一个动态链接库或者二进制程序，修复转IR过程的问题，同时解决生成的IR里面明显的语义不一致。过程中的log文件记录到logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/08-more-bench2文件夹内。
```

## 当前目标和已有 native 状态

Bench2 计划里 wolfssl 优先选动态库：

```text
/usr/lib/x86_64-linux-gnu/libwolfssl.so.42.0.0
```

现有动态库 module 已能生成 IR，但仍是 partial：

```text
internal declarations: 4057
external declarations: 146
lowered function bodies: 4037
failed function bodies: 20
```

失败都是：

```text
branch target address is ambiguous
```

以 `wc_ReadDirFirst` 为例，Ghidra 导出的 `CBRANCH` 有两个 successor 都从 `ram:00163ea4` 开始，但它们是不同 SSA 返回路径。只按地址解析会误判为无法区分。

## Ghidra 相关实现

Ghidra 的 block model 不只靠 start address 表示 CFG：

- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/block/CodeBlock.java`
  - block 有地址范围和 successor 引用。
- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/block/CodeBlockReference.java`
  - edge 本身携带 source / destination。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/block.cc`
  - decompiler CFG 允许同一机器地址在不同 SSA 路径中出现。

native 侧不能只把 start address 当唯一 block id。已有 JSON 的 `block.Out` 是更直接的 CFG edge 信息。

## native 侧复刻策略

1. 保持普通 direct branch 仍优先按地址解析。
2. 对 `CBRANCH`，如果两个 successor 的 start address 都等于目标地址，说明地址无法消歧。
3. 这种情况下保守复用 Ghidra 导出的 `block.Out` 顺序：`Out[0]` 做 true edge，`Out[1]` 做 false edge。

## 判断标准

1. `wolfssl` module lowered body 数量增加。
2. 生成 `.ll` 通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。
3. libicu data-library 小回归仍通过。

## 风险

1. 该修复只在两个 successor 同地址时使用 `block.Out` 顺序，避免扩大到普通地址不匹配情况。
2. 如果 Ghidra JSON 的 successor 顺序本身不稳定，后续需要在导出侧显式标记 true/false edge。

## 实现记录

### 修改内容

- `lib/HeritageToLLVM.cpp:1766`
  - 新增 `allSuccessorsStartAt(...)`。
  - 用来判断当前 block 的所有 successor 是否都指向同一个机器地址。
- `lib/HeritageToLLVM.cpp:1792`
  - `lowerBranch(...)` 处理 `CBRANCH` 时，如果按地址解析得到 ambiguous，但 `block.Out.size() == 2` 且两个 successor 的 start address 都等于 target address，就清掉 ambiguous 错误。
  - 后续沿用已有 fallback：`Out[0]` 作为 true edge，`Out[1]` 作为 false edge。

### 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-heritage-module-llvm notdec-heritage-module-check -j2
```

libicu data-library 回归：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/dynamic-libs/libicu/data-library/module-all.json \
  -o /tmp/libicu-data-after-wolfssl.ll
```

结果：

```text
internal declarations: 5
external declarations: 2
lowered function bodies: 5
failed function bodies: 0
```

wolfssl module check：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-check \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/dynamic-libs/wolfssl/shared-library/module-all.json
```

结果：

```text
functions: 4057
externals: 146
failures: 0
duplicate function names: 9
direct calls: 19391
resolved internal calls: 15831
resolved external calls: 3560
unknown calls: 0
status: ok
```

wolfssl full lower：

```bash
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/dynamic-libs/wolfssl/shared-library/module-all.json \
  -o /tmp/notdec-wolfssl-cbranch.ll
```

结果：

```text
internal declarations: 4057
external declarations: 146
lowered function bodies: 4057
failed function bodies: 0
TIME 201.72
```

对比旧结果：

```text
lowered function bodies: 4037
failed function bodies: 20
```

LLVM 22 验证：

```bash
/usr/bin/time -f 'TIME %e' /sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/notdec-wolfssl-cbranch.ll -o /tmp/notdec-wolfssl-cbranch.bc
/usr/bin/time -f 'TIME %e' /sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-wolfssl-cbranch.bc -o /tmp/notdec-wolfssl-cbranch.verified.bc
```

结果：全部通过。`llvm-as` 40.05s，`opt -passes=verify` 22.94s。

生成 IR：

```text
1050063 /tmp/notdec-wolfssl-cbranch.ll
55M /tmp/notdec-wolfssl-cbranch.ll
12M /tmp/notdec-wolfssl-cbranch.bc
```

### 性能和判断

这次只在 `CBRANCH` ambiguous 错误路径上增加一次 successor start 检查，不影响普通分支。wolfssl full lower 201.72s，和旧记录 203.12s 同量级，没有看到性能下降。

实现效果：5/5。wolfssl shared-library 从 partial-ir 变成 4057/4057 bodies lowered，并通过 LLVM 22 汇编和 verify。

复杂度：2/5。只处理同地址双 successor 的窄场景。

维护成本：2/5。后续如果导出 true/false edge，可以替换对 `block.Out` 顺序的依赖。
