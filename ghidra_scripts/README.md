# notdec-bin2llvm Ghidra scripts

## ExportHeritagePcode.java

导出 Ghidra decompiler 分析后的 `HighFunction` P-Code，输出 JSON。
这条路径用于先验证 libdecomp heritage 结果是否适合后续转 LLVM IR。

用法：

```bash
analyzeHeadless /tmp/notdec-ghidra-proj ProjectName \
  -import /path/to/binary \
  -scriptPath /sn640/NotDec/external/NotDec-bin2llvm/ghidra_scripts \
  -postScript ExportHeritagePcode.java /tmp/out.json function_name
```

参数：

1. 输出 JSON 路径。
2. 可选函数入口地址或函数名。headless 下建议显式传入。
3. 可选 simplification style，默认 `decompile`。
4. 可选 decompile timeout 秒数，默认 `60`。

当前导出内容：

1. language、compiler spec、simplification style。
2. 函数名、入口、调用约定、返回类型、参数列表和参数 varnode。
3. basic block、入边、出边、block 内 op 顺序。
4. P-Code op、输入输出 varnode、direct call target。
5. varnode 的 space、offset、size、寄存器名、HighVariable 名和类型。
6. 统计信息，包括 `MULTIEQUAL` 数量和残留 register-space varnode 数量。

## ExportHeritageModule.java

导出模块级 `notdec.heritage-module.v0` JSON。每个成功 decompile 的内部函数复用
单函数 heritage 字段，失败函数写入 `failures[]`，外部函数写入 `externals[]`。

用法：

```bash
analyzeHeadless /tmp/notdec-ghidra-proj ProjectName \
  -import /path/to/binary \
  -scriptPath /sn640/NotDec/external/NotDec-bin2llvm/ghidra_scripts \
  -postScript ExportHeritageModule.java /tmp/module.json --limit=20 --timeout=60
```

参数：

1. 输出 JSON 路径。
2. `--limit=N` 或位置参数 `N`：最多导出 N 个非 external、非 thunk 函数，默认 `20`。
3. `--all`：导出所有函数。
4. `--timeout=N`：单函数 decompile timeout 秒数，默认 `60`。
5. `--style=name`：Ghidra decompiler simplification style，默认 `decompile`。

配套 native 工具：

```bash
notdec-heritage-module-check /tmp/module.json
notdec-heritage-module-llvm /tmp/module.json -o /tmp/module.ll
llvm-as /tmp/module.ll -o /tmp/module.bc
```

Java 链路需要把 High P-Code 里的 register input 只当来源标记时，lowering
加 `--register-inputs-as-temps`。这个选项只在 heritage JSON lowering 工具上提供，
native 链路不使用：

```bash
notdec-heritage-module-llvm /tmp/module.json -o /tmp/module.ll --register-inputs-as-temps
```

`notdec-heritage-module-llvm` 默认会尝试填入 `status == "ok"` 的函数体。单个函数
lowering 或 verifier 失败时，会恢复成 declaration，并继续处理其他函数。

只需要阶段 3 的 declaration-only 输出时：

```bash
notdec-heritage-module-llvm /tmp/module.json -o /tmp/module.ll --declarations-only
```
