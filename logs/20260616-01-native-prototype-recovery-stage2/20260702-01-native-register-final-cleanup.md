# native register final cleanup 实现记录

## 背景

fortune native 输出里，寄存器 SSA 已经消除了大部分寄存器访问，但输出 IR 还残留两类非语义内容：

- 无引用的 `!notdec.register` 全局变量、`llvm.*.with.overflow.*` declaration、`notdec.register.summary_*` helper declaration。
- 已经没有寄存器访问的函数上还带 `notdec.register.summary*` / `notdec.register.summary_ssa` metadata。

单独验证后，`GlobalDCEPass` 可以删掉第一类内容；函数 metadata 不属于 dead global，仍需要自己清。

## 实现

- `include/notdec-bin2llvm/passes/summary/NativeRegisterFinalCleanup.h:12`：新增 `NativeRegisterFinalCleanupOptions` 和 `NativeRegisterFinalCleanupSummary`。
- `include/notdec-bin2llvm/passes/summary/NativeRegisterFinalCleanup.h:28`：声明 `runNativeRegisterFinalCleanup(...)` 和 summary 打印函数。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:19`：定义只清理 summary 相关的函数 metadata key，不清 `external_inputs` / `preserves` / `clobbers`。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:56`：新增 `functionHasRegisterResidue(...)`，函数内还有 register global load/store 或 `notdec.register.*` helper call 时保留 metadata。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:92`：封装 `GlobalDCEPass`，交给 LLVM 删除无引用 register global、overflow intrinsic declaration 和 helper declaration。
- `lib/passes/summary/NativeRegisterFinalCleanup.cpp:113`：最终 cleanup 流程为 `GlobalDCE -> 清函数 summary metadata -> GlobalDCE -> 统计剩余 register access`。
- `lib/CMakeLists.txt:14`：把 `NativeRegisterFinalCleanup.cpp` 加入 `notdec-bin2llvm-core`。
- `tools/notdec-native-llvm.cpp:952`：新增 `runFinalCleanupPass(...)`，cleanup 后再 `verifyModule`。
- `tools/notdec-native-llvm.cpp:996`、`tools/notdec-native-llvm.cpp:1119`：IR 输入和 ELF 输入两条 `notdec-native-llvm` 链路都在输出前运行 final cleanup。
- `tests/native_register_summary_ssa_test.cpp:270`：新增测试 helper 检查 summary metadata 和 unused declaration。
- `tests/native_register_summary_ssa_test.cpp:3493`：覆盖干净函数会删除 dead register global 和 summary metadata。
- `tests/native_register_summary_ssa_test.cpp:3526`：覆盖还有 register load 时保留 metadata 和 global。
- `tests/native_register_summary_ssa_test.cpp:3559`：覆盖 GlobalDCE 删除 unused overflow intrinsic declaration 和 summary helper declaration。

## 验证

```bash
cmake --build build --target native_register_summary_ssa_test notdec-native-llvm -j4
ctest --test-dir build -R notdec.native_register_summary.ssa --output-on-failure
```

结果：通过，`notdec.native_register_summary.ssa` 用时约 `0.71s`。

fortune smoke：

```bash
/usr/bin/time -f 'TIME native-fortune-cleanup-globaldce2 %e' \
  build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune \
  --all-confirmed --register-ssa-summary \
  -o /tmp/notdec-bin2llvm-fortune-cleanup-20260702-globaldce2/fortune.native.cleanup.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/notdec-bin2llvm-fortune-cleanup-20260702-globaldce2/fortune.native.cleanup.ll \
  -o /tmp/notdec-bin2llvm-fortune-cleanup-20260702-globaldce2/fortune.native.cleanup.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-bin2llvm-fortune-cleanup-20260702-globaldce2/fortune.native.cleanup.bc \
  -o /tmp/notdec-bin2llvm-fortune-cleanup-20260702-globaldce2/fortune.native.cleanup.verified.bc
python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-fortune-cleanup-20260702-globaldce2/fortune.native.cleanup.ll
```

结果：`llvm-as` 和 `opt -passes=verify` 通过；native register residue audit 只有表头。运行时间 `8.32s`，和修改前 fortune 同口径 `8.16s` 接近，未看到明显性能回退。

输出清理效果：

- `llvm.*.with.overflow.*` declaration 已清掉。
- `notdec.register.summary_*` helper declaration 已清掉。
- `!notdec.register` global 从原来的 27 个降到 4 个，剩余 `RSP/RAX/RBX/RDX` 对应仍有真实 load。
- 只有还存在寄存器 load 的 `notdec_native_27f0` 和 `notdec_native_31f0` 保留 summary metadata。

## 评估

- 实现效果：8/10。能清掉输出 IR 里的无用 global/declaration，并只在干净函数上删除 summary metadata。
- 复杂度：2/10。新增 pass 很小，主要依赖 LLVM GlobalDCE，自己只做保守 metadata 清理。
- 维护成本：2/10。后续如果新增 summary metadata key，只需要补 key 列表；不需要维护自定义 global DCE 逻辑。

更好的方案是未来把 register residue audit 接成更正式的 C++ 或 lit 检查，但当前先用现有 native summary SSA 单测和 fortune smoke 覆盖主链路即可。

## native --skip-runtime 早期过滤

用户追问 native 链路是否也能像 Java 导出一样，在最开始识别并去掉 `_start`、PLT、init/fini 这些非源码函数。当前实现把 runtime 识别下沉到 `NativeAnalysis`，`notdec-native-llvm --skip-runtime` 会在 seed / GTIRB import / Sleigh 递归发现阶段尽早过滤；最终 lowering 前仍保留同一个判断，防止已有状态里混入 runtime 函数。

### 实现

- `include/notdec-bin2llvm/NativeAnalysis.h`：新增 `NativeRuntimeFilterOptions`，并把 runtime filter 挂到 `NativeGtirbDecodeOptions` / `NativeSleighDecodeOptions`。
- `include/notdec-bin2llvm/NativeAnalysis.h`：公开 `isNativeRuntimeFunctionName`、`isNativeRuntimeSectionName`、`isNativeRuntimeAddress`、`isNativeRuntimeSeed`、`isNativeRuntimeFunction`，供 CLI 和测试复用。
- `lib/NativeAnalysis.cpp`：`ElfEntryAnalyzer` 在 `SkipRuntimeFunctions` 打开时不再添加 ELF entry / init / fini seed。
- `lib/NativeAnalysis.cpp`：`ElfSymbolAnalyzer`、GTIRB function import、GTIRB seed fallback、Sleigh 初始 seed 队列和递归 call/tail target 都会跳过 runtime 地址。
- `lib/NativeAnalysis.cpp`：runtime 地址规则覆盖 `.plt` / `.plt.got` / `.plt.sec` / `.init` / `.fini`，并补了 x86-64 glibc `_start` 指令模板识别，用于无符号/无名字时过滤 `_start`。
- `tools/notdec-native-llvm.cpp`：新增 `--skip-runtime`，把选项传给 discovery，并在 all-confirmed lowering 前复用 `isNativeRuntimeFunction(...)` 做最后一道过滤。
- `tests/native_analysis_facts_test.cpp`：补 runtime predicate 单测，覆盖名字、section、seed source 和普通函数不误判。

### 验证

```bash
cmake --build build --target notdec-native-llvm native_analysis_facts_test -j4
./build/bin/native_analysis_facts_test
ctest --test-dir build -R notdec.native_analysis.facts --output-on-failure
build/bin/notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune --all-confirmed --skip-runtime -o /tmp/notdec-bin2llvm-fortune-skip-runtime-early3-20260702/fortune.native.skip-runtime.ll --summary-json-out /tmp/notdec-bin2llvm-fortune-skip-runtime-early3-20260702/fortune.native.summary.json
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-bin2llvm-fortune-skip-runtime-early3-20260702/fortune.native.skip-runtime.ll -o /tmp/notdec-bin2llvm-fortune-skip-runtime-early3-20260702/fortune.native.skip-runtime.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-bin2llvm-fortune-skip-runtime-early3-20260702/fortune.native.skip-runtime.bc -o /tmp/notdec-bin2llvm-fortune-skip-runtime-early3-20260702/fortune.native.skip-runtime.verified.bc
```

结果：构建通过；`notdec.native_analysis.facts` 通过，用时约 `28.61s`；fortune `llvm-as` / `opt -passes=verify` 通过，运行时间 `7.31s`。

输出效果：`_start` / PLT stub 已经不在最终 IR 里，`@RAX/@RSP/@RDX` 被 DCE 清掉。当前还剩 `@RBX`，来源是 `notdec_native_27f0`，对应 `get_tbl.cold`，不是 runtime；它是源函数 cold fragment，后续要靠 cold fragment 合并或跨 fragment 参数/寄存器传递处理，不能按 runtime 直接删。

### 评估

- 实现效果：7/10。runtime 过滤已经提前到 discovery，能清掉 `_start` 造成的寄存器残留；但 fortune 仍剩 `get_tbl.cold` 的真实 cold fragment 残留。
- 复杂度：4/10。规则集中在 `NativeAnalysis`，CLI 只传选项；x86-64 `_start` 模板有少量字节匹配。
- 维护成本：4/10。后续如果遇到不同 libc/startup 模板，需要补样例；普通应用函数不会因为 `.text` 地址被误删。
