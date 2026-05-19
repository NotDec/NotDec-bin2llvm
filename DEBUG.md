# NotDec-bin2llvm 本机跑法

这个文件只记录当前这台机器上跑 Bench2 真实项目的常用方式。

## 1. Ghidra headless 导出模块 JSON

先建好 project 目录，再跑 headless。

```bash
mkdir -p /tmp/notdec-ghidra-proj-memcached-20260519

/sn640/ghidra/build/dist/ghidra_11.3.2_DEV/support/analyzeHeadless \
  /tmp/notdec-ghidra-proj-memcached-20260519 memcached20 \
  -import /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached \
  -scriptPath /sn640/NotDec/external/NotDec-bin2llvm/ghidra_scripts \
  -postScript ExportHeritageModule.java \
  /tmp/notdec-bench2-rerun-20260519/memcached20/module-limit20.json \
  --limit=20 --timeout=60
```

要点：

- 输入用 `/sn640/NotDec-Exp/Bench2/rootfs/...` 里的本地副本
- `--limit=20` 是当前常用的快速扩范围方式
- `memcached`、`lighttpd`、`openssh` 这几组都已经跑过

## 2. native lower

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-check \
  /tmp/notdec-bench2-rerun-20260519/memcached20/module-limit20.json

/tmp/notdec-bin2llvm-build/bin/notdec-heritage-module-llvm \
  /tmp/notdec-bench2-rerun-20260519/memcached20/module-limit20.json \
  -o /tmp/notdec-bench2-rerun-20260519/memcached20/module-limit20.ll

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/notdec-bench2-rerun-20260519/memcached20/module-limit20.ll \
  -o /tmp/notdec-bench2-rerun-20260519/memcached20/module-limit20.bc
```

## 3. 当前已经跑过的样本

- `memcached limit20`
- `lighttpd limit20`
- `openssh limit20`

这些结果会继续写进仓库根目录的 `logs/`。这里只放命令骨架，不重复贴每次结果。
