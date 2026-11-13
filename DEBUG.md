# NotDec-bin2llvm 本机跑法

这个文件只记录当前这台机器上跑 Bench2 真实项目的常用方式。

## 1. native discovery

native discovery 默认读取 DDISASM 生成的 GTIRB facts，并补充 ELF、重定位和
`.eh_frame` 信息：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached \
  >/tmp/memcached.discovery.json
```

要点：

- 输入用 `/sn640/NotDec-Exp/Bench2/rootfs/...` 里的本地副本
- 需要审计 CFG 时再用 `--blocks-json`、`--seeds-json` 或 `--xrefs-json`
- 当前 native 主链不消费 Ghidra heritage JSON

## 2. native lower and LLVM 22 verification

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached \
  --all-confirmed --register-ssa-summary \
  -o /tmp/memcached.native.ll

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/memcached.native.ll -o /tmp/memcached.native.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt \
  -passes=verify /tmp/memcached.native.bc -o /tmp/memcached.native.opt.bc
```

## 3. 当前已经跑过的样本

- `memcached limit20`
- `lighttpd limit20`
- `openssh limit20`

这些结果会继续写进仓库根目录的 `logs/`。这里只放命令骨架，不重复贴每次结果。

## 4. native debug oracle

Bench2 native discovery 的 debug-info oracle 作为手动 coverage gate 使用，不接入默认 CTest。

具体命令和当前期望结果见：

- [`logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/08-more-bench2/20260524-01-bench2-selected-targets-rerun-script.md`](logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/08-more-bench2/20260524-01-bench2-selected-targets-rerun-script.md)

常用入口：

```bash
cd /sn640/NotDec/external/NotDec-bin2llvm
/usr/bin/time -f 'TIME debug-oracle-selected6 %e' \
  scripts/bench2-native-discovery-debug-check.py \
  --target libuv:shared-library \
  --target vsftpd:executable \
  --target memcached:executable \
  --target python:shared-library \
  --target vim:executable \
  --target wolfssl:shared-library
```

## 5. 调试工具

除主链路 `notdec-native-discover` / `notdec-native-llvm` 外，构建还会产出几个只用于
手工排查的工具，不接入 CTest：

- `notdec-native-pcode`：只跑 discovery + SLEIGH，打印某个函数/范围的 p-code，
  用来确认"lift 之前"的形态。
- `notdec-sleigh-pcode`：给定原始字节直接反汇编并打印 p-code，不依赖 ELF。
- `notdec-sleigh-llvm`：给定原始字节直接 lower 成 LLVM IR，用来隔离 lowering 问题。
- `notdec-lief-elf`：打印 LIEF 解析出的 section/segment/relocation/symbol，核对 ELF facts。

用法都是 `--help` 或直接给输入路径；这些工具的定位是"隔离一层看输入"，不要在主链路里调用。
