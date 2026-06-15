# Ghidra Java 导出跳过 runtime 函数

用户原始需求摘要：

> 当前基于规则的方式，效果怎么样？效果还可以的话，尝试增加一个flag，指定后自动将这些runtime相关的函数去掉，从后续的IR中删去。这样的话，入口就变成main函数了，理论上也不存在其他的函数会调用runtime的函数

## 背景

Bench2 里动态链接的 executable 只会带 PLT/import，不会把 libc 函数体链接进来。需要隔离的主要是 glibc crt/startup 这部分，比如 `_start`、init/fini、clone registration、global dtors 相关函数。

FID 对这块不稳：`crt1.o`/`Scrt1.o` 里的 `_start` 会被 linker relaxation 改写，典型是 `main@GOTPCREL(%rip)` 相关指令从 `MOV` 变成 `LEA`，FID hash 不会把这种 opcode 差异当成同一个函数。规则识别更适合先解决论文实验导出的干扰。

## 实现

- `ghidra_scripts/ExportHeritagePcode.java:40`
  - 新增 `Options`，支持 `--skip-runtime`。
- `ghidra_scripts/ExportHeritagePcode.java:48`
  - 新增 runtime 名字表，覆盖 `_start`、`_init`、`_fini`、`_DT_INIT`、`_DT_FINI`、`_INIT_0`、`_FINI_0`、clone/dtor/frame_dummy、`_dl_relocate_static_pie`。
- `ghidra_scripts/ExportHeritagePcode.java:75`
  - 单函数导出遇到 runtime 且指定 `--skip-runtime` 时，不导出 runtime 本身；如果是 glibc `_start`，从 `_start` 的 `RDI` 装载引用里找 `main`，改为导出 `main`。
- `ghidra_scripts/ExportHeritagePcode.java:168`
  - 增加 glibc `_start` 指令模板识别：`ENDBR64, XOR, MOV, POP, MOV, AND, PUSH, PUSH, XOR, XOR, LEA/MOV, CALL`。
- `ghidra_scripts/ExportHeritageModule.java:41`
  - 模块导出选项增加 `skipRuntime`。
- `ghidra_scripts/ExportHeritageModule.java:62`
  - 模块导出复用同一组 runtime 名字和 `_start` 模板。
- `ghidra_scripts/ExportHeritageModule.java:188`
  - `selectFunctions(...)` 在 `--skip-runtime` 打开时跳过 runtime 函数，再按 limit 选择应用函数。
- `ghidra_scripts/ExportHeritageModule.java:580`
  - stats 里增加 `skippedRuntimeCount`，控制台也打印跳过数量。

## 验证

单函数 `_start` 重定向到 `main`：

```bash
/sn640/ghidra/build/dist/ghidra_11.3.2_DEV/support/analyzeHeadless \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ghidra-projects/selected-targets/lighttpd-executable \
  lighttpd-executable \
  -process lighttpd -noanalysis -readOnly \
  -scriptPath /sn640/NotDec/external/NotDec-bin2llvm/ghidra_scripts \
  -postScript ExportHeritagePcode.java \
  /tmp/notdec-lighttpd-start-skip.json _start decompile 60 --skip-runtime
```

结果：控制台打印 `skip-runtime: redirect _start to main`，JSON 中函数名为 `main`，入口为 `ram:00114bc9`。

模块导出跳过 runtime：

```bash
/sn640/ghidra/build/dist/ghidra_11.3.2_DEV/support/analyzeHeadless \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ghidra-projects/selected-targets/lighttpd-executable \
  lighttpd-executable \
  -process lighttpd -noanalysis -readOnly \
  -scriptPath /sn640/NotDec/external/NotDec-bin2llvm/ghidra_scripts \
  -postScript ExportHeritageModule.java \
  /tmp/notdec-lighttpd-module-skip.json --limit=20 --timeout=60 --style=decompile --skip-runtime
```

结果：`attempted=20`、`succeeded=20`、`failed=0`、`external=25`、`skippedRuntimeCount=4`。输出 JSON 中没有 `_start/_DT_INIT/_DT_FINI/_INIT_0/_FINI_0` 这些函数名。

`git diff --check` 通过。

## 性能和风险

性能：只在函数选择阶段读少量指令和名字表，对 decompile 总耗时影响很小。lighttpd limit=20 的同口径测试仍正常完成。

效果：8/10。对当前 Bench2 里常见 glibc 动态 executable 的 crt/startup 干扰能直接降掉。

复杂度：6/10。两个 Java 脚本里有一小段重复规则，但现在范围更清楚，暂时不抽公共工具类。

维护成本：5/10。风险主要是不同发行版/编译选项下 `_start` 模板可能变化；后续如果遇到漏识别，优先补规则样例，不回到 FID 复刻。
