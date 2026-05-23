# Ffmpeg Poison Cleanup Verification

## 原始 prompt

```text
参考logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/GOAL.md，当前目标是进一步拓展最后一步，完善bench2上的效果。对于/sn640/NotDec-Exp/Bench2/plan.md里面的每个目标项目，都选择一个动态链接库或者二进制程序，修复转IR过程的问题，同时解决生成的IR里面明显的语义不一致。过程中的log文件记录到logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/08-more-bench2文件夹内。
```

## 当前目标和已有 native 状态

Bench2 计划里的 `ffmpeg` 这里继续看二进制入口：

```text
/usr/bin/ffmpeg
```

旧的 `one-fg_create.cold.ll` 里有 `freeze i64 poison`，现在重新 lower 后已经去掉，剩下的是 `notdec_heritage_CALLOTHER_i64` 和 `notdec_heritage_CALLIND_void`。

## Ghidra 相关实现

这次不改 Ghidra，只复验当前导出的 JSON 和 native lowering 结果。

相关语义点还是：

- `ghidra/program/model/pcode/Varnode.java`
- `ghidra/program/model/pcode/PcodeOp.java`
- `ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`

## native 侧复刻策略

这次只做验证，不加新分支：

1. 重新 lower `ffmpeg/one-fg_create.cold.json`。
2. 检查生成 IR 里是否还存在 `poison`、`undef`、`freeze`。
3. 用 LLVM 22 跑 `llvm-as` 和 `opt -passes=verify`。

## 判断标准

1. 当前 IR 不再出现 `freeze i64 poison`。
2. `llvm-as` 通过。
3. `opt -passes=verify` 通过。

## 风险

1. 这只覆盖一个冷块入口，不代表 ffmpeg 其他函数都没有同类问题。
2. IR 里还保留 helper 调用，后面如果要继续收紧语义，需要单看 CALLOTHER 和间接调用恢复。

## 实现记录

### 修改内容

没有代码改动。本目标只记录当前构建对 ffmpeg 旧 `freeze poison` 的复验结果。

### 验证

重新 lower：

```bash
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-heritage-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/ffmpeg/one-fg_create.cold.json \
  -o /tmp/ffmpeg-fg-current.ll
```

结果：

```text
TIME 0.02
```

IR 检查：

```bash
rg -n "poison|undef|freeze|notdec_heritage" /tmp/ffmpeg-fg-current.ll
```

结果：

```text
11:  %0 = call i64 (...) @notdec_heritage_CALLOTHER_i64(i32 77), !notdec.effect !2
12:  call void (...) @notdec_heritage_CALLIND_void(i64 %0), !notdec.effect !3
16:declare i64 @notdec_heritage_CALLOTHER_i64(...)
18:declare void @notdec_heritage_CALLIND_void(...)
```

旧 IR 对照里原来有 `freeze i64 poison`，当前结果已经清掉。

LLVM 22 验证：

```bash
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/ffmpeg-fg-current.ll -o /tmp/ffmpeg-fg-current.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/ffmpeg-fg-current.bc -o /tmp/ffmpeg-fg-current.verified.bc
```

结果：通过。

### 性能和判断

这次没有代码改动，不引入性能变化。

实现效果：4/5。ffmpeg 这条入口已经从旧 poison 状态恢复为可汇编、可 verify 的 IR。

复杂度：1/5。只是记录当前验证。

维护成本：1/5。后面扩到更多入口时继续按同口径检查。
