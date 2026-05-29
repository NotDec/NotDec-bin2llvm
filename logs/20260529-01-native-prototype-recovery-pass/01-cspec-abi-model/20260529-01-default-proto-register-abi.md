# 20260529-01 Default Proto Register ABI

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

Ghidra 的 x86-64 SysV ABI 来自 `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec`。其中 `<default_proto><prototype>` 描述默认 calling convention，`<input>` 和 `<output>` 里的 `<pentry>` 给出寄存器和栈参数位置，`<unaffected>` / `<killedbycall>` 给出 call 对寄存器的影响。

Java 侧读取位置：

- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/lang/PrototypeModel.java`
- 关键函数：`PrototypeModel.restoreXml(...)`
- 作用：读取 `<prototype>` 属性，调用 input/output 参数列表解析，并读取 `<unaffected>`、`<killedbycall>`、`<returnaddress>` 等 varnode 列表。

Decompiler C++ 侧读取位置：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
- 关键函数：`ProtoModel::decode(...)`、`ParamListStandard::decode(...)`、`ParamEntry::decode(...)`、`EffectRecord::decode(...)`、`ProtoModel::hasEffect(...)`
- 作用：把 prototype decode 成 `ProtoModel`，把 `<pentry>` 存为 `ParamEntry`，把 `<unaffected>` / `<killedbycall>` 存为 `EffectRecord`，后续 callsite 通过 `hasEffect(...)` 查询。

## native 复刻方式

这一步只做 x86-64 gcc cspec 当前测试需要的子集：

- 新增 `NativeAbiSpec`，对应 Ghidra 的默认 `ProtoModel`。
- 新增 `NativeAbiStorage`，先表示 register / stack 两类 storage。
- 新增 `NativeAbiParamEntry`，保存 `<pentry>` 的 minsize、maxsize、metatype 和 storage。
- 新增 `NativeAbiEffect`，保存 unaffected / killedbycall。
- 解析默认 prototype，不解析其他 prototype，不处理 group/rule/protorule。
- 在 module 上写 `!notdec.abi`，后续阶段先用 metadata 串联。

先跟 x86-64 SysV 用例走，能读出 RDI/RSI/RDX/RCX/R8/R9、RAX/RDX、RBX/RSP/RBP/R12-R15 这些寄存器信息即可。栈 pentry 先保留，但后续参数恢复第一阶段不依赖它。

## 判断标准

- `notdec-native-llvm` native ELF 路径自动从 Ghidra x86-64 gcc cspec 读 ABI。
- 输出 IR 包含 `!notdec.abi` metadata。
- 输出 IR 能被 `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as` 和 `opt -passes=verify` 接受。

## 实现记录

新增文件：

- `include/notdec-bin2llvm/NativeAbi.h:16`：新增 `NativeAbiStorageKind`。
- `include/notdec-bin2llvm/NativeAbi.h:21`：新增 `NativeAbiEffectKind`。
- `include/notdec-bin2llvm/NativeAbi.h:29`：新增 `NativeAbiStorage`，表示 register / stack storage。
- `include/notdec-bin2llvm/NativeAbi.h:38`：新增 `NativeAbiParamEntry`，保存 cspec `<pentry>` 子集。
- `include/notdec-bin2llvm/NativeAbi.h:48`：新增 `NativeAbiEffect`，保存 unaffected / killedbycall。
- `include/notdec-bin2llvm/NativeAbi.h:56`：新增 `NativeAbiSpec`，保存默认 prototype 的 ABI 事实。
- `include/notdec-bin2llvm/NativeAbi.h:67`：声明 `parseGhidraCspecDefaultAbi(...)`。
- `include/notdec-bin2llvm/NativeAbi.h:70`：声明 `attachNativeAbiMetadata(...)`。
- `lib/NativeAbi.cpp:25`：新增 `SmallXmlParser`，只支持当前 cspec 需要的 element、attribute、comment、CDATA。
- `lib/NativeAbi.cpp:282`：新增 `paramEntryFromElement(...)`，解析 `<pentry>` 的 register / stack storage。
- `lib/NativeAbi.cpp:304`：新增 `collectParamEntries(...)`，递归处理 `<group>` 内的 `<pentry>`。
- `lib/NativeAbi.cpp:318`：新增 `collectEffects(...)`，解析 `<unaffected>` / `<killedbycall>`。
- `lib/NativeAbi.cpp:396`：实现 `parseGhidraCspecDefaultAbi(...)`，读取 `<default_proto><prototype>`。
- `lib/NativeAbi.cpp:453`：实现 `attachNativeAbiMetadata(...)`，输出 `!notdec.abi`。

修改文件：

- `lib/CMakeLists.txt:5`：把 `NativeAbi.cpp` 加入 `notdec-bin2llvm-core`。
- `tools/notdec-native-llvm.cpp:225`：新增 `defaultX86CspecPath()`。
- `tools/notdec-native-llvm.cpp:229`：新增 `resolveX86CspecPath(...)`。
- `tools/notdec-native-llvm.cpp:628`：新增 `attachDefaultAbiMetadata(...)`。
- `tools/notdec-native-llvm.cpp:878`：native ELF 输出路径在 verify 前附加 ABI metadata。

验证命令：

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target notdec-bin2llvm-core -j2
```

结果：通过，`NativeAbi.cpp` 编译进入 core 静态库。

```sh
c++ -std=c++17 -Iinclude -I/sn640/NotDec/llvm-22.1.0.obj/include /tmp/notdec_abi_smoke.cpp build/lib/libnotdec-bin2llvm-core.a -L/sn640/NotDec/llvm-22.1.0.obj/lib -lLLVM -Wl,-rpath,/sn640/NotDec/llvm-22.1.0.obj/lib -o /tmp/notdec_abi_smoke
/tmp/notdec_abi_smoke > /tmp/notdec_abi_smoke.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec_abi_smoke.ll -o /tmp/notdec_abi_smoke.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec_abi_smoke.bc -o /tmp/notdec_abi_smoke.opt.bc
rg "notdec.abi|RDI|RAX|unaffected|killedbycall" /tmp/notdec_abi_smoke.ll
```

结果：通过。输出包含 `!notdec.abi`、`RDI`、`RAX`、`unaffected`、`killedbycall`。

没有完整构建 `notdec-native-llvm`。当前仓库没有已配置的 LIEF + SLEIGH 构建目录；本次先用 core 编译和 parser smoke 覆盖新增代码。后续接 pipeline 时再做 native 工具全量构建。

## 风险

- XML parser 是当前 cspec 子集 parser，不是通用 XML。它能跳过 comment 和 CDATA，但没有实体展开。
- register storage 现在只保存寄存器名，还没解析到 p-code register space/offset/size。下一步 storage model 或 ABI/model 结合 register metadata 时需要补齐。
- `notdec-native-llvm` 的 IR 输入路径暂时不补 ABI metadata，因为没有 ELF 架构上下文。后续 pipeline 阶段需要加显式 cspec 参数或 module 内 ABI 复用规则。

## 评分

- 实现效果：7/10。已经能读取默认 ABI 并输出 metadata，但还没接完整 prototype matching。
- 复杂度：6/10。新增了一个小 XML parser，有理解成本；好处是不引入新依赖，范围可控。
- 维护成本：6/10。后续如果要完整 cspec，应替换为正式 XML 解析或复用 Ghidra decode；当前子集适合先跑通。
