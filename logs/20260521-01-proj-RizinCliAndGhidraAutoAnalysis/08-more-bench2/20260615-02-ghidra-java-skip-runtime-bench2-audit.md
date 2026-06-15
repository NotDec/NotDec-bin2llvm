# Bench2 Java 链路 skip-runtime 复跑与 register 残留调研

用户原始需求摘要：

> 加上之后在那边 Bench2上做测试，尝试重新跑一遍Bench2的项目的Ghidra Java链路的导出，用专门的文件夹放java链路的结果，忘了之前有没有，没有的话就创建一个。然后进一步调研一下里面仍未消除的寄存器的访问情况，看看能否怎么处理掉

## 背景

前一轮已经给 Ghidra Java 导出加了 `--skip-runtime`。这轮需要确认两件事：

1. Bench2 上这条 Java 链路还能不能稳定跑。
2. 跳过 runtime 之后，后续 IR 里还剩多少 register 访问，剩下的是不是还能继续消。

这里先把结果单独放到新目录：

- `/sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-java-skip-runtime`

## 跑法

本轮先跑 Bench2 的 selected 目标集合：

- `vsftpd`
- `libuv`
- `memcached`
- `lighttpd`
- `tmux`
- `openssh`
- `wolfssl`
- `redis`
- `libicu`
- `vim`
- `python`
- `wrk`
- `ffmpeg`
- `php`

每个目标导出：

- `module-all-skip-runtime.json`
- `module-all-skip-runtime.ll`
- `module-all-skip-runtime.ssa.ll`

后面再把 `.ll` 喂给 `notdec-native-llvm --register-ssa-summary`，看 register 访问收敛情况。

## 结果

### Java 导出

成功 9 个：

- `vsftpd`
- `libuv`
- `memcached`
- `lighttpd`
- `tmux`
- `openssh`
- `wolfssl`
- `redis`
- `libicu`

中断 2 个：

- `vim`，人为终止，`rc=143`
- `python`，人为终止，`rc=143`，JSON 只生成到半截

未继续跑 3 个：

- `wrk`
- `ffmpeg`
- `php`

对应 summary 在：

- `/sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-java-skip-runtime/summary.tsv`

### register 残留

对 9 个成功 JSON 做了统计：

- 大多数函数都还带 register varnode，不是少量边角。
- 常见寄存器：`ZF`、`RAX`、`FS_OFFSET`、`EAX`、`RDI`、`RSP`。
- `lighttpd` 里最重的函数是 `config_read`、`http_response_parse_headers`、`burl_normalize` 这类业务函数。

`lighttpd` 的 register varnode 统计：

- `functions_with_register = 862 / 930`
- `register_varnodes = 25442`
- `top_registers = ZF, RAX, FS_OFFSET, EAX, ...`

`libuv` 里看得更清楚：

- 原始 `.ll` 里 register load/store 还在，主要是 `@FS_OFFSET`、`@RSP`。
- 跑 `notdec-native-llvm --register-ssa-summary` 后：
  - `loads = 373`
  - `loads replaced = 373`
  - `external inputs = 111`
  - `call effect helpers = 272`
  - `llvm-as` 和 `opt -passes=verify` 都通过。

也就是说：

- Ghidra 导出阶段保留 register varnode 是正常的。
- 真正能“清掉”的部分，已经被现有 `NativeRegisterSSA` 处理掉一大块。
- 剩下的主要是：
  - 函数入口的 `external_input`
  - call effect 相关的 `RSP` 写回

这部分不能简单删掉，不然 call graph / 参数语义会坏。

## 判断

当前更像是：

1. Java 链路先保留 register 语义。
2. 后面用 `NativeRegisterSSA` 把能 SSA 化的 register load/store 处理掉。
3. 对于还剩的入口寄存器和 call effect，先别粗暴删，后面再看是否按类型推理/约束跳过，或者再补更细的规则。

## 文件

- `ghidra_scripts/ExportHeritagePcode.java:40-228`
- `ghidra_scripts/ExportHeritageModule.java:41-237, 580-587`
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/08-more-bench2/20260615-02-ghidra-java-skip-runtime-bench2-audit.md`
