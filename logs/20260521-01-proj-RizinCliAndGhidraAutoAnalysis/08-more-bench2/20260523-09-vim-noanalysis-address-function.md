# Vim Noanalysis Address Function Export

## 原始 prompt

```text
参考logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/GOAL.md，当前目标是进一步拓展最后一步，完善bench2上的效果。对于/sn640/NotDec-Exp/Bench2/plan.md里面的每个目标项目，都选择一个动态链接库或者二进制程序，修复转IR过程的问题，同时解决生成的IR里面明显的语义不一致。过程中的log文件记录到logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/08-more-bench2文件夹内。
```

## 当前目标和已有 native 状态

Bench2 计划里的 `vim` 没有动态库，继续用二进制 fallback：

```text
/usr/bin/vim.basic
```

已有结果里 `lalloc.cold` 没有产出 JSON，失败点在 Ghidra 自动分析：

```text
vim ... fail ghidra_failed_124
vim ... fail ghidra_autoanalysis_timeout_runtime_6min
```

旧日志停在 `ANALYZING all memory and code`，说明主要卡在整文件 auto-analysis，不是 native lowering。

## Ghidra 相关实现

单函数导出脚本是：

- `ghidra_scripts/ExportHeritagePcode.java`
  - `resolveFunction`

原逻辑要求目标地址已经有 Function。`vim.basic` 这种大二进制如果跳过整文件分析，地址处还没有 Function，所以脚本无法继续。新逻辑在地址没有对应 Function 时，直接调用 GhidraScript 的 `createFunction(address, null)`。

## native 侧复刻策略

这次只补两个窄逻辑：

1. Ghidra 单函数导出时，地址找不到函数就按地址建函数。
2. 单函数 lowering 里，`CALL` 有 `callTarget` 但没有 `callTargetName` 时，用地址生成一个外部声明名。

这样 `vim` 可以走：

```text
-noanalysis import -> 按地址建函数 -> decompile 单函数 -> lower -> LLVM verify
```

## 判断标准

1. `vim.basic` 不再需要跑完整文件 auto-analysis 才能导出 `lalloc.cold`。
2. 生成 JSON 能被 `notdec-heritage-llvm` lower。
3. 生成 IR 不含 `poison / undef / freeze`。
4. LLVM 22 `llvm-as` 和 `opt -passes=verify` 通过。
5. 已有单函数和 module 路径不回退。

## 风险

1. `-noanalysis` 导出的函数信息更少，函数名退回 `FUN_0013ea27`，直接调用退回 `sub_0013e9d1`。
2. 这个结果只覆盖 `lalloc.cold`，不是 vim 全模块。
3. 如果后续要更完整的 vim 语义，仍然需要改善 Ghidra 分析配置或导入依赖库。

## 实现记录

### 修改内容

- `ghidra_scripts/ExportHeritagePcode.java:83`
  - 函数：`resolveFunction`
  - 找不到 `FunctionAt` 后，先查 `FunctionContaining`，仍找不到时用 `createFunction(address, null)` 创建目标函数。

- `lib/HeritageToLLVM.cpp:287`
  - 函数：`resolveCallTargetName`
  - 单函数 lowering 没有 module symbol plan 时，优先用 `callTargetName`；没有名字但有 `callTarget` 时，用 `sub_` 加地址生成 callee 声明名。

### 验证

Ghidra 导出：

```bash
/usr/bin/time -f 'TIME %e' timeout 180s \
  /sn640/ghidra/build/dist/ghidra_11.3.2_DEV/support/analyzeHeadless \
  /tmp/notdec-vim-ghidra-noanalysis NotDecBench2_vim_lalloc_noanalysis \
  -import /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/vim.basic \
  -noanalysis \
  -scriptPath /sn640/NotDec/external/NotDec-bin2llvm/ghidra_scripts \
  -postScript ExportHeritagePcode.java \
  /tmp/vim-lalloc-noanalysis.json 0013ea27 decompile 120
```

结果：

```text
exported FUN_0013ea27 to /tmp/vim-lalloc-noanalysis.json
register-space varnodes: 1
TIME 7.36
```

lower：

```bash
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-heritage-llvm \
  /tmp/vim-lalloc-noanalysis.json \
  -o /tmp/vim-lalloc-noanalysis.ll
```

结果：

```text
TIME 0.02
```

IR 关键内容：

```text
call void (...) @sub_0013e9d1(), !notdec.effect !0
declare void @sub_0013e9d1(...)
```

IR 检查：

```bash
rg -n "poison|undef|freeze|notdec_heritage" /tmp/vim-lalloc-noanalysis.ll
```

结果：无匹配。

LLVM 22 验证：

```bash
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/vim-lalloc-noanalysis.ll -o /tmp/vim-lalloc-noanalysis.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/vim-lalloc-noanalysis.bc -o /tmp/vim-lalloc-noanalysis.verified.bc
```

结果：通过。

回归：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-heritage-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/lighttpd/1-main_init_once.json \
  -o /tmp/lighttpd-regress.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/lighttpd-regress.ll -o /tmp/lighttpd-regress.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/lighttpd-regress.bc -o /tmp/lighttpd-regress.verified.bc

/tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/libuv/module-limit5.json \
  -o /tmp/libuv-regress.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/libuv-regress.ll -o /tmp/libuv-regress.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/libuv-regress.bc -o /tmp/libuv-regress.verified.bc
```

结果：全部通过。`libuv` 仍是 `5/5` bodies lowered。

### 性能和判断

这次改动没有进入 pass pipeline，也没有改变 module symbol plan。影响只在单函数导出和单函数 CALL 命名。

`vim` 旧路线 6 分钟超时；新路线 7.36s 导出，0.02s lower。

实现效果：4/5。vim 当前入口能导出、lower、LLVM verify。

复杂度：2/5。新增逻辑很小，但 `-noanalysis` 结果比完整分析保守。

维护成本：2/5。后续如果用 `-noanalysis` 扩更多函数，要接受函数名和 callee 名按地址退回。
