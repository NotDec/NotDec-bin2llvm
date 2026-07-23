# 原始请求

> 按这个推进一下试试

# 实现记录

本次只推进 i386 native 链路的第一步：让 i386 ELF 使用正确的 Ghidra SLEIGH/pspec/cspec 文件，并能产出可验证的 LLVM IR。没有尝试修完 i386 的寄存器消除、PLT/GOT、栈传参和 canary。

改动：

- `include/notdec-bin2llvm/NativeAnalysis.h:26` 新增 `NativeElfArchitectureSpec`，把 `sla`、`pspec`、`cspec` 三个文件名放在同一个结构里，避免 decode 和 ABI metadata 选错架构。
- `lib/NativeAnalysis.cpp:5280` 实现 `nativeElfArchitectureSpec()`：x86-64 选择 `x86-64.sla` / `x86-64.pspec` / `x86-64-gcc.cspec`，i386 选择 `x86.sla` / `x86.pspec` / `x86gcc.cspec`。
- `lib/NativeAnalysis.cpp:3218` 的 `SleighInstructionDecodeAnalyzer::resolveSpecOptions()` 改为使用 `NativeElfArchitectureSpec`，i386 discovery 不再停在 `instructions=0`。
- `lib/NativeAnalysis.cpp:388` 的 `RelocationPltAnalyzer::run()` 仍只允许 x86-64，避免 i386 误跑 x86-64 relocation 规则。
- `tools/notdec-native-llvm.cpp:335` 增加 `resolveCspecPath()`，让 ABI metadata 按 ELF 架构选择 cspec。
- `tools/notdec-native-llvm.cpp:344` 的 `resolveSpecOptions()` 改为按 ELF 架构选择 SLEIGH/pspec；显式传 `sla` 时也会补默认 `pspec`。
- `tools/notdec-native-llvm.cpp:837` 的 `attachDefaultAbiMetadata()` 接收架构 spec，i386 输出现在是 `ESP` / `__cdecl`。
- `tools/notdec-native-llvm.cpp:1082` 增加 `hasExternallyVisibleFunctionDefinition()`，`runFinalCleanupPass()` 在没有外部可见函数定义时禁用 GlobalDCE，避免 i386 由于还没识别 `main` 而被清成空模块。
- `scripts/native-fortune-i386-regression.sh:48` 增加 smoke 检查 helper；`scripts/native-fortune-i386-regression.sh:110` 开始只要求 `llvm-as` / `verify` 通过、存在 `define`、ABI metadata 是 `ESP`，残留寄存器只产出审计文件。

# 验证

- `cmake --build external/NotDec-bin2llvm/build --target notdec-native-llvm notdec-native-discover -j4` 通过。
- i386 discovery：`/tmp/notdec-fortune-i386-discover-after-spec-20260723-034844/summary.json` 中 `instructions=3559`。
- i386 native IR：`/tmp/notdec-fortune-i386-native-after-gdce-20260723-040149/fortune.native.ll`，共 13577 行，包含真实 `define`，ABI metadata 为 `prototype=__cdecl`、`stackpointer.register=ESP`。
- `llvm-as` 和 `opt -passes=verify` 可通过：`/tmp/notdec-fortune-i386-native-after-gdce-20260723-040149/fortune.verified.bc`。
- `ctest --test-dir external/NotDec-bin2llvm/build -R notdec.native_llvm.realworld_fortune_i386 --output-on-failure` 通过。
- `ctest --test-dir external/NotDec-bin2llvm/build -R 'notdec.native_(discover.x86_64_smoke|llvm.x86_64_smoke|llvm.realworld_fortune_x86_64)' --output-on-failure` 通过。
- `ctest --test-dir external/NotDec-bin2llvm/build --output-on-failure` 通过，17/17。

# 当前剩余问题

- i386 PLT/GOT 还没实现，外部函数仍表现为 `notdec_native_10e0` 这类 PLT stub，`register-ssa-warnings.tsv` 里有大量 `unresolved_unknown_external_signature`。
- i386 入口 `_start -> __libc_start_main -> main` 还没识别，所以当前用禁用 GlobalDCE 的保护避免空输出，函数名仍是 `notdec_native_14d0` 而不是 `main`。
- `register-residue-audit.tsv` 还有 34 条残留：主要是 `ESP.entry` 和 `GS_OFFSET.entry`。
- i386 callee-saved / PIC `EBX` / 栈上传参还没专门处理，所以函数参数里还有不少 `EBX.arg`、`EBP.arg`、`ESI.arg`、`EDI.arg`。

# 评分

- 实现效果：7/10。i386 已能生成可验证 IR，且 ABI/spec 选择正确；但还只是 smoke 级别。
- 复杂度：3/10。新增一个小架构表，并调整两个入口。
- 维护成本：3/10。后续 i386 PLT/GOT 和 `_start` 识别可以在现有架构表上继续扩展。
