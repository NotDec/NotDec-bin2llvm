# Batch Current Verification: Libuv, Memcached, Tmux, Openssh, Redis, Wrk

## 原始 prompt

```text
参考logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/GOAL.md，当前目标是进一步拓展最后一步，完善bench2上的效果。对于/sn640/NotDec-Exp/Bench2/plan.md里面的每个目标项目，都选择一个动态链接库或者二进制程序，修复转IR过程的问题，同时解决生成的IR里面明显的语义不一致。过程中的log文件记录到logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/08-more-bench2文件夹内。
```

## 当前目标和已有 native 状态

这次一起复验了 6 个还没单独写日志的目标：

- `libuv`
- `memcached`
- `tmux`
- `openssh`
- `redis`
- `wrk`

当前选用的 target 仍然是 plan 里已经选好的那批：

- `libuv`: `/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0`
- `memcached`: `/usr/bin/memcached`
- `tmux`: `/usr/bin/tmux`
- `openssh`: `/usr/bin/ssh`
- `redis`: `/usr/bin/redis-server`
- `wrk`: `/usr/bin/wrk`

这批里，`libuv` 用的是 module 入口；`memcached` 和 `tmux` 的 IR 里还会出现 helper 调用，但没有再回退到 `poison`；`openssh / redis / wrk` 这几个当前构建里没有抓到 `poison / undef / freeze`。

## Ghidra 相关实现

这次不改 Ghidra，只复验当前导出的 JSON 和 native lowering 结果。

相关语义点仍然是：

- `ghidra/program/model/pcode/Varnode.java`
- `ghidra/program/model/pcode/PcodeOp.java`
- `ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc`

## native 侧复刻策略

这次只做验证，不加新分支：

1. 重新 lower 这批目标的 JSON。
2. 检查 IR 里是否还残留 `poison`、`undef`、`freeze`。
3. 用 LLVM 22 跑 `llvm-as` 和 `opt -passes=verify`。

## 判断标准

1. `llvm-as` 能接受生成的 IR。
2. `opt -passes=verify` 能通过。
3. 旧的 `poison / undef / freeze` 不再出现，或者只剩下明确的 helper 调用。

## 风险

1. 这只是当前选中的单个入口，不代表整个项目族都没有同类问题。
2. `memcached / tmux` 仍然有 `notdec_heritage_CALLOTHER_i64`、`notdec_heritage_CALLIND_void`，后面如果要继续收紧语义，还得单看这些调用恢复。

## 实现记录

### 修改内容

没有代码改动。本目标只记录当前构建对这一批目标的复验结果。

### 验证

重新 lower：

```bash
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/libuv/module-limit5.json \
  -o /tmp/libuv-module-current.ll
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-heritage-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/memcached/1-drive_machine.lto_priv.0.cold.json \
  -o /tmp/memcached-current.ll
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-heritage-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/tmux/1-cmd_display_menu_args_parse.lto_priv.0.cold.json \
  -o /tmp/tmux-current.ll
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-heritage-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/openssh/1-window_change_handler.lto_priv.0.json \
  -o /tmp/openssh-current.ll
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-heritage-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/redis/1-dictSdsKeyCompare.cold.json \
  -o /tmp/redis-current.ll
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-heritage-llvm \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/wrk/one-parse_url_char.cold.json \
  -o /tmp/wrk-current.ll
```

结果：

- `libuv`: `heritage module lowering`, `lowered function bodies: 5`, `failed function bodies: 0`, `TIME 0.02`
- `memcached`: `TIME 0.02`
- `tmux`: `TIME 0.02`
- `openssh`: `TIME 0.02`
- `redis`: `TIME 0.02`
- `wrk`: `TIME 0.02`

IR 检查：

```bash
rg -n "poison|undef|freeze|notdec_heritage" /tmp/libuv-module-current.ll
rg -n "poison|undef|freeze|notdec_heritage" /tmp/memcached-current.ll
rg -n "poison|undef|freeze|notdec_heritage" /tmp/tmux-current.ll
rg -n "poison|undef|freeze|notdec_heritage" /tmp/openssh-current.ll
rg -n "poison|undef|freeze|notdec_heritage" /tmp/redis-current.ll
rg -n "poison|undef|freeze|notdec_heritage" /tmp/wrk-current.ll
```

结果：

- `libuv`: 只剩 `notdec_heritage_CALLIND_void`
- `memcached`: 只剩 `notdec_heritage_CALLOTHER_i64` 和 `notdec_heritage_CALLIND_void`
- `tmux`: 只剩 `notdec_heritage_CALLOTHER_i64` 和 `notdec_heritage_CALLIND_void`
- `openssh`: 无匹配
- `redis`: 无匹配
- `wrk`: 无匹配

LLVM 22 验证：

```bash
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/libuv-module-current.ll -o /tmp/libuv-module-current.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/libuv-module-current.bc -o /tmp/libuv-module-current.verified.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/memcached-current.ll -o /tmp/memcached-current.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/memcached-current.bc -o /tmp/memcached-current.verified.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/tmux-current.ll -o /tmp/tmux-current.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/tmux-current.bc -o /tmp/tmux-current.verified.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/openssh-current.ll -o /tmp/openssh-current.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/openssh-current.bc -o /tmp/openssh-current.verified.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/redis-current.ll -o /tmp/redis-current.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/redis-current.bc -o /tmp/redis-current.verified.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/wrk-current.ll -o /tmp/wrk-current.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/wrk-current.bc -o /tmp/wrk-current.verified.bc
```

结果：全部通过。

### 性能和判断

这次没有代码改动，不引入性能变化。

实现效果：4/5。六个目标都能稳定生成可汇编、可 verify 的 IR。

复杂度：1/5。只是记录当前验证。

维护成本：1/5。后面如果扩到别的入口，继续按同口径检查即可。
