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
