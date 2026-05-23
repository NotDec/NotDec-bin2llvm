# Lighttpd Poison Cleanup Verification

## 原始 prompt

```text
参考logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/GOAL.md，当前目标是进一步拓展最后一步，完善bench2上的效果。对于/sn640/NotDec-Exp/Bench2/plan.md里面的每个目标项目，都选择一个动态链接库或者二进制程序，修复转IR过程的问题，同时解决生成的IR里面明显的语义不一致。过程中的log文件记录到logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/08-more-bench2文件夹内。
```

## 当前目标和已有 native 状态

Bench2 计划里的 `lighttpd` 没有单独的动态库可选，这里继续用二进制入口：

```text
/usr/sbin/lighttpd
```

旧的 `1-main_init_once.ll` 里有多处 `freeze i64 poison`，说明当时还是在用不可靠的兜底值。当前构建重新 lower 后，这些 `freeze poison` 已经消失，只剩一个 `notdec_heritage_CALLIND_void` 调用。

## Ghidra 相关实现

这次不改 Ghidra 源码，只是复验当前导出的 heritage JSON 和 native lowering 结果。

相关的 SSA / call 语义仍然对应：

- `ghidra/program/model/pcode/Varnode.java`
- `ghidra/program/model/pcode/PcodeOp.java`
- `ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`

## native 侧复刻策略

这次只做验证，不新增处理逻辑：

1. 重新 lower `lighttpd/1-main_init_once.json`。
2. 检查生成 IR 里是否还残留 `poison`、`undef`、`freeze`。
3. 用 LLVM 22 做 `llvm-as` 和 `opt -passes=verify`。

## 判断标准

1. 当前 IR 里不再出现 `freeze i64 poison`。
2. `llvm-as` 能接受生成的 `.ll`。
3. `opt -passes=verify` 通过。

## 风险

1. 这只覆盖 `main_init_once` 这一个入口，不代表 lighttpd 其他块都没有同类问题。
2. `notdec_heritage_CALLIND_void` 仍然存在，后面如果要继续收紧语义，还得单独看调用目标恢复。

## 实现记录

### 修改内容

没有代码改动。本目标只记录当前构建对 lighttpd 旧 `freeze poison` 的复验结果。

### 验证

重新 lower：

```bash
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-heritage-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/lighttpd/1-main_init_once.json \
  -o /tmp/lighttpd-main-current.ll
```

结果：

```text
TIME 0.04
```

IR 检查：

```bash
rg -n "poison|undef|freeze|notdec_heritage" /tmp/lighttpd-main-current.ll
```

结果：

```text
87:  call void (...) @notdec_heritage_CALLIND_void(i64 %44, i64 4294967288, i64 2), !notdec.effect !13
158:declare void @notdec_heritage_CALLIND_void(...)
```

旧 IR 对照里原来有多处 `freeze i64 poison`，当前结果已清掉。

LLVM 22 验证：

```bash
/usr/bin/time -f 'AS_TIME %e' /sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/lighttpd-main-current.ll -o /tmp/lighttpd-main-current.bc
/usr/bin/time -f 'VERIFY_TIME %e' /sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/lighttpd-main-current.bc -o /tmp/lighttpd-main-current.verified.bc
```

结果：

```text
AS_TIME 0.03
VERIFY_TIME 0.04
```

### 性能和判断

这次没有代码改动，不引入性能变化。

实现效果：4/5。lighttpd 这条入口已经从旧 poison 状态恢复为可汇编、可 verify 的 IR。

复杂度：1/5。只是记录当前验证。

维护成本：1/5。后面如果再扩 lighttpd 其他入口，继续按同样口径检查。
