# Native function linkage tracking

## 背景

native lowering 之前把所有 lifted 函数都建成默认 external linkage。这样在可执行文件里，内部 helper 也会像导出函数一样暴露；后续 `main` 参数推断也更难区分“真正入口”和普通内部函数。

本次只处理 linkage 信息，不改函数签名推断。

## 实现

- `include/notdec-bin2llvm/PcodeToLLVM.h:30`：`PcodeLoweringConfig` 增加 `EntryFunctionLinkage`，默认保持 `ExternalLinkage`，避免影响旧的单函数/测试用法。
- `lib/PcodeToLLVM.cpp:2281`：`appendPcodeFunction()` 创建或复用空 declaration 时使用 `EntryFunctionLinkage`。
- `include/notdec-bin2llvm/NativeAnalysis.h:56` 和 `:174`：`NativeFunctionSeed` / `NativeFunction` 增加 `IsExternallyVisible`。
- `lib/NativeAnalysis.cpp:700`：`ElfSymbolAnalyzer` 发现 LIEF 导出符号时标记 seed 为 externally visible。
- `lib/NativeAnalysis.cpp:2925`、`:3650`、`:5359`：GTIRB fallback、Sleigh decode 和 `addFunction()` 都把 seed 上的可见性带到 confirmed function。
- `tools/notdec-native-llvm.cpp:83`：新增 shared library 判断。`ET_DYN && !is_pie()` 按共享库处理。
- `tools/notdec-native-llvm.cpp:88`：新增 `nativeFunctionLinkage()`。共享库里导出函数用 default external linkage，非导出函数 internal；可执行文件默认 internal，`main` 保持 default external linkage。
- `tools/notdec-native-llvm.cpp:901` 和 `:1202`：confirmed module 和单函数路径都把 linkage 写入 lowering config。

## 验证

构建：

```bash
cmake --build external/NotDec-bin2llvm/build --target notdec-native-llvm -j4
```

fortune 可执行文件：

```bash
OUT=/tmp/fortune-linkage-20260707072849
external/NotDec-bin2llvm/build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  --all-confirmed --skip-runtime \
  -o "$OUT/fortune.ll" \
  --summary-json-out "$OUT/summary.json" \
  --register-ssa-warning-out "$OUT/register-warnings.json"
llvm-22.1.0.obj/bin/llvm-as "$OUT/fortune.ll" -o "$OUT/fortune.bc"
```

结果：

- `main` 是 default external linkage。
- `FUN_3eb0`、`FUN_3470` 是 `internal`。
- register refs：0。
- summary helpers：0。
- switch count：2。
- elapsed：12.15s，和之前 fortune 约 12s 同级。

共享库 smoke：

```bash
OUT=/tmp/mod_sockproxy-linkage-20260707072951
external/NotDec-bin2llvm/build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/lighttpd/mod_sockproxy.so \
  --all-confirmed --skip-runtime \
  -o "$OUT/native.ll" \
  --summary-json-out "$OUT/summary.json" \
  --register-ssa-warning-out "$OUT/register-warnings.json"
llvm-22.1.0.obj/bin/llvm-as "$OUT/native.ll" -o "$OUT/native.bc"
```

结果：

- LIEF/readelf 都看到导出函数 `mod_sockproxy_plugin_init`。
- IR 里 `mod_sockproxy_plugin_init` 是 default external linkage。
- register refs：0。
- summary helpers：0。
- elapsed：2.34s。

## 评价

- 实现效果：8/10。linkage 信息已经从 ELF 符号层传到 lowering，fortune 和一个 lighttpd `.so` smoke 都符合预期。
- 复杂度：2/10。只是把已有 ELF 符号事实带到 `NativeFunction`，没有碰 SummarySSA 或签名推断。
- 维护成本：2/10。linkage 决策集中在 `nativeFunctionLinkage()`，后续如果要从 `_start` 更精确识别 main，只需要扩展这个入口或补 main 标记。

