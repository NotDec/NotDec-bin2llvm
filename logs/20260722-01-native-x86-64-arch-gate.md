# 原始请求

> 对，这个第一点先修复一下，以及把其他部分硬编码x86-64的地方都改一下，但是可以先做判断，让非 x86-64 架构的 binary 先 abort，报告当前逻辑未实现，而不是硬编码x86-64架构。后续再加上X86逻辑吧

# 实现记录

目标是先把 native 链路的架构边界说清楚：当前仍只支持 x86-64 ELF，非 x86-64 输入不能继续套用 x86-64 的 SLEIGH、ABI、PLT/GOT 逻辑。

改动：

- `include/notdec-bin2llvm/NativeAnalysis.h:26` 增加 `isSupportedNativeElfArchitecture()`、`nativeElfArchitectureName()`、`unsupportedNativeElfArchitectureMessage()`。
- `lib/NativeAnalysis.cpp:388` 的 `RelocationPltAnalyzer::run()` 改为统一架构判断，不再单独写死错误文本。
- `lib/NativeAnalysis.cpp:2351` 的 `isExternalFunctionPointerRelocationAt()` 在非支持架构上直接返回 `false`，避免继续匹配 x86-64 relocation 名称。
- `lib/NativeAnalysis.cpp:3217` 的 `SleighInstructionDecodeAnalyzer::resolveSpecOptions()` 使用统一错误信息。
- `lib/NativeAnalysis.cpp:5277` 实现当前支持集：只接受 `LIEF::ELF::ARCH::X86_64`。
- `tools/notdec-native-llvm.cpp:347` 的 `resolveSpecOptions()` 在检查显式 SLEIGH spec 前先检查 ELF 架构，所以手动传 i386 `x86.sla` 也会失败。
- `tools/notdec-native-llvm.cpp:335` 把 ABI cspec helper 重命名为 x86-64 专属名字，避免看起来像通用 x86 helper。
- `tools/notdec-native-discover.cpp:1406` 在 discovery 入口也对非 x86-64 ELF 直接失败。
- `scripts/native-fortune-i386-regression.sh:24` 更新 i386 fixture 的 skip oracle，匹配新的明确错误。

# 验证

- `cmake --build external/NotDec-bin2llvm/build --target notdec-native-llvm notdec-native-discover -j4` 通过。
- i386 `notdec-native-llvm` 自动 spec 路径返回 `rc=1`，报 `native LLVM lowering currently supports x86-64 ELF only; got I386`。
- i386 `notdec-native-llvm` 显式 `x86.sla/x86.pspec` 也返回同样错误。
- i386 `notdec-native-discover --summary-json` 返回 `rc=1`，报 `native discovery currently supports x86-64 ELF only; got I386`。
- `ctest --test-dir external/NotDec-bin2llvm/build -R notdec.native_llvm.realworld_fortune_i386 --output-on-failure` 通过，测试按未支持架构 skip。
- `ctest --test-dir external/NotDec-bin2llvm/build -R 'notdec.native_(discover.x86_64_smoke|llvm.x86_64_smoke|llvm.realworld_fortune_x86_64)' --output-on-failure` 通过。

# 评分

- 实现效果：8/10。解决了 i386 被错误套用 x86-64 metadata 的问题，但还没有实现 i386。
- 复杂度：2/10。只增加统一判断和入口 gate。
- 维护成本：2/10。后续加 i386 时主要扩展 `isSupportedNativeElfArchitecture()` 和 spec/cspec 选择。
