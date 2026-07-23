原始 prompt：

> 那就让RelocationPltAnalyzer等相关的也支持x86吧，先形成一个规划，然后再开始改

# native i386 PLT/GOT relocation 支持计划

## 背景

当前 native 默认链路是 GTIRB 负责函数和基本块事实，SLEIGH 负责指令语义，随后进入 SummarySSA。`RelocationPltAnalyzer` 在 `runNativeDiscovery()` 的前段运行，不是 Heritage 专用逻辑。它现在只支持 x86-64 relocation，所以 i386 fortune 里 `.plt` stub 没有映射到真实外部函数，后面会出现 `notdec_native_10e0` 这类假函数名，也会带来未知外部签名 warning。

## 目标

让当前 GTIRB native 链路能在 i386 ELF 上识别常见 PLT/GOT relocation：

- `R_386_JUMP_SLOT` 映射到 `.plt` stub 和 GOT slot。
- `R_386_GLOB_DAT` 作为外部函数指针 relocation。
- `R_386_RELATIVE` 能读取 `.rel` relocation 地址处的原始 word 作为 addend。
- x86-64 现有行为不退化。

本次不改 SummarySSA、不改参数推断、不尝试完整解决 i386 栈传参。

## 技术路线

1. 把 relocation 类型判断改成架构相关 helper，覆盖 relative、jump slot、glob dat、absolute function pointer、irelative。
2. `RelocationPltAnalyzer` 放开到 x86-64 和 i386。非这两种架构继续只记 note 并跳过。
3. i386 `RELATIVE` 没有显式 addend，使用 `NativeProgramState::readRawPointer()` 读取 relocation 地址里的原始指针值作为 addend，再加入 relocated pointer。
4. i386 `JUMP_SLOT` 复用当前 legacy `.plt` 入口推导：按 GOT relocation 地址排序，对应 `.plt + 16 + index * 16`。
5. `.plt.got` 的 `endbr64 + rip-relative jmp` 仍只走 x86-64。
6. `isExternalFunctionPointerRelocationAt()` 和 `planNativeCallTargets()` 同步支持 i386 `GLOB_DAT`，避免后续 call target fallback 仍写死 x86-64。

## 风险

- i386 PIC 的 PLT stub 是 `jmp [ebx+disp]`，间接跳转本身不一定能从 SLEIGH 层直接推出 GOT 地址；但直接 call 到 `.plt` stub 的情况可以靠 `NativePltEntry.StubAddress` 解决。
- PLT0 是动态链接器 resolver 入口，仍不能当普通外部函数处理。
- 某些 i386 ELF 可能存在非 16 字节 PLT entry，本次只覆盖当前常见 GNU i386 PLT 形状。

## 判断标准

- i386 fortune native 输出里不再把常规 PLT stub 叫成 `notdec_native_10e0` 这类函数。
- `register-ssa-warnings.tsv` 里由未解析 PLT 导致的未知外部签名 warning 明显减少。
- x86-64 fortune 和已有 native smoke 测试继续通过。

## 实现记录

已完成。

- `lib/NativeAnalysis.cpp:83-156`：新增 relocation 类型 helper，按 x86-64 / i386 区分 relative、jump slot、glob dat、absolute function pointer、irelative；i386 `REL` addend 从 relocation 地址处的原始 word 读取。
- `lib/NativeAnalysis.cpp:463-549`：`RelocationPltAnalyzer::run()` 放开 x86-64 和 i386；i386 `JUMP_SLOT` 进入已有 `.plt + 16 + index * 16` 映射；`.plt.got` 仍限定 x86-64。
- `lib/NativeAnalysis.cpp:2441-2458`：`isExternalFunctionPointerRelocationAt()` 支持 i386 `X86_GLOB_DAT` / `X86_32`。
- `tools/notdec-native-llvm.cpp:786-797`：`planNativeCallTargets()` 的 relocation fallback 支持 `X86_GLOB_DAT`。

验证：

- `cmake --build external/NotDec-bin2llvm/build --target notdec-native-llvm notdec-native-discover -j4`：通过。
- `ctest --test-dir external/NotDec-bin2llvm/build -R notdec.native_llvm.realworld_fortune_i386 --output-on-failure`：通过。
- `ctest --test-dir external/NotDec-bin2llvm/build -R 'notdec.native_(discover.x86_64_smoke|llvm.x86_64_smoke|llvm.realworld_fortune_x86_64)' --output-on-failure`：通过。
- i386 fortune 产物 `external/NotDec-bin2llvm/build/native-fortune-i386-regression/fortune.native.ll` 不再出现 `notdec_native_10e0`、`notdec_native_10f0` 等常规 PLT stub 名；可以看到 `__ctype_toupper_loc`、`__snprintf_chk`、`malloc`、`exit` 等真实外部符号。

评分：

- 实现效果：8/10。解决 i386 常规 PLT/GOT 映射问题，当前 fortune 的常规 PLT stub 已转成真实外部符号。
- 复杂度：4/10。增加了一组架构相关 helper，但没有改变 discovery / SummarySSA 大流程。
- 维护成本：4/10。后续支持更多架构时需要继续扩展 helper；当前比散落字符串判断更容易维护。
