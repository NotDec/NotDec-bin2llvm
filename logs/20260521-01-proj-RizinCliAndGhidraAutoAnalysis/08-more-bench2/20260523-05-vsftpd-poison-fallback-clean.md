# Vsftpd Poison Fallback Clean

## 原始 prompt

```text
参考logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/GOAL.md，当前目标是进一步拓展最后一步，完善bench2上的效果。对于/sn640/NotDec-Exp/Bench2/plan.md里面的每个目标项目，都选择一个动态链接库或者二进制程序，修复转IR过程的问题，同时解决生成的IR里面明显的语义不一致。过程中的log文件记录到logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/08-more-bench2文件夹内。
```

## 当前目标和已有 native 状态

Bench2 计划里 `vsftpd` 没有动态库，按计划使用二进制 fallback：

```text
/usr/sbin/vsftpd
```

已有 `module-limit5.json` 能生成 IR，但旧 lower log 里有大量 poison fallback：

```text
Warning: heritage lowering uses poison fallback in FUN_00106740: read uninitialized varnode ...
Warning: heritage lowering uses poison fallback in FUN_00106740: PHI incoming varnode is unavailable ...
```

这类 fallback 会让 IR 虽然能汇编，但语义明显不可靠。当前构建重新 lower 后，这些 warning 已消失。

## Ghidra 相关实现

Ghidra heritage 的 SSA varnode 应由 def-use 和 PHI incoming 维护：

- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/pcode/Varnode.java`
  - varnode 表示 P-Code SSA 值。
- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/pcode/PcodeOp.java`
  - `MULTIEQUAL` 表示 SSA PHI。
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`
  - heritage/SSA 阶段整理 varnode 和 MULTIEQUAL。

native lowering 侧要避免用 poison 掩盖普通 PHI incoming 或 address-tied 读值。

## native 侧复刻策略

本次不新增代码，验证当前构建已经消除旧 fallback：

1. 重新 lower `vsftpd/module-limit5.json`。
2. 检查 stderr 是否还有 `poison fallback`。
3. 检查生成 IR 是否包含 `poison`、`undef`、`freeze`。
4. 用 LLVM 22 `llvm-as` 和 `opt -passes=verify` 做最终验证。

## 判断标准

1. `module-limit5.json` 仍是 5/5 bodies lowered。
2. stderr 没有 poison fallback warning。
3. 生成 IR 里没有 `poison`、`undef`、`freeze`。
4. LLVM 22 汇编和 verify 通过。

## 风险

1. 这个验证只覆盖当前 5 函数 module，不代表整个 vsftpd 都没有 fallback。
2. 后续如果扩到更多函数，仍可能出现新的 missing varnode 或 PHI incoming 问题。

## 实现记录

### 修改内容

没有新增代码改动。本目标记录当前构建对 vsftpd 旧 poison fallback 的验证结果。

### 验证

重新 lower：

```bash
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/vsftpd/module-limit5.json \
  -o /tmp/vsftpd-module-current.ll
```

结果：

```text
heritage module lowering
internal declarations: 5
external declarations: 95
lowered function bodies: 5
failed function bodies: 0
TIME 11.62
```

stderr 没有 `poison fallback` warning。

IR 检查：

```bash
rg -n "poison|undef|freeze" /tmp/vsftpd-module-current.ll
```

结果：无匹配。

LLVM 22 验证：

```bash
/usr/bin/time -f 'TIME %e' /sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/vsftpd-module-current.ll -o /tmp/vsftpd-module-current.bc
/usr/bin/time -f 'TIME %e' /sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/vsftpd-module-current.bc -o /tmp/vsftpd-module-current.verified.bc
```

结果：全部通过。`llvm-as` 1.44s，`opt -passes=verify` 0.80s。

IR 大小：

```text
32867 /tmp/vsftpd-module-current.ll
2.3M /tmp/vsftpd-module-current.ll
```

### 性能和判断

本次没有新增代码，不引入性能变化。当前 full lower 11.62s，和旧记录 11.01s 同量级。

实现效果：4/5。vsftpd module-limit5 从旧 poison fallback 状态恢复为无 fallback、可汇编、可 verify。

复杂度：1/5。仅记录当前构建验证。

维护成本：1/5。后续扩大函数范围时继续按同口径检查 fallback。
