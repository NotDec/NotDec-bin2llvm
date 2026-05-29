# 20260529-02 Cspec ABI Parser Test

## 原始 prompt

```text
/goal 阅读logs/20260529-01-native-prototype-recovery-pass/GOAL.md，按照里面的要求不断复刻Ghidra中的对应数据结构，最终实现参数和返回值恢复的功能为一个完善的Pass，进度记录到PROGRESS.md中。每次选择一小块功能进行实现，实现的时候必须按照这样的规范： 

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## Ghidra 实现

这次只补测试，但测试目标仍对应 Ghidra 的 cspec prototype 读取链路。

Ghidra Java 侧：

- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/lang/PrototypeModel.java`
- 关键函数：`PrototypeModel.restoreXml(...)`
- 作用：从 `<prototype>` 读取 name、extrapop、stackshift，再读取 `<input>`、`<output>`、`<unaffected>`、`<killedbycall>`。

Ghidra Decompiler C++ 侧：

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
- 关键函数：`ProtoModel::decode(...)`、`ParamListStandard::decode(...)`、`ParamEntry::decode(...)`、`EffectRecord::decode(...)`
- 作用：把 cspec 里的 prototype、pentry 和 effect record decode 成后续 prototype recovery 使用的数据结构。

真实 x86-64 ABI 来源：

- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec`

## native 复刻方式

上一小步已经实现 `parseGhidraCspecDefaultAbi(...)` 和 `attachNativeAbiMetadata(...)`。这一步不扩展功能，只把临时 smoke 固化为 CTest：

- 新增 `tests/native_abi_cspec_test.cpp`。
- 测试读取 CMake 传入的 `x86-64-gcc.cspec`。
- 检查默认 prototype 名字、栈指针、关键 input/output register、unaffected 和 killedbycall。
- 把 ABI 写入空 LLVM module，调用 `llvm::verifyModule` 验证 metadata 不破坏 IR。

这能覆盖当前测试用例需要的 SysV x86-64 ABI 子集。后续如果要解析 returnaddress、register offset/size 或更多 prototype，再在这个测试里补断言。

## 判断标准

- `cmake --build build --target native_abi_cspec_test` 通过。
- `ctest -R notdec.native_abi.cspec -V` 通过。
- 测试必须使用 `/sn640/NotDec/llvm-22.1.0.obj` 的 LLVM。

## 实现记录

新增文件：

- `tests/native_abi_cspec_test.cpp:14`：新增 `hasParamRegister(...)`，检查 input/output pentry 是否包含指定 register。
- `tests/native_abi_cspec_test.cpp:27`：新增 `hasEffectRegister(...)`，检查 ABI effect 是否包含指定 register。
- `tests/native_abi_cspec_test.cpp:51`：新增测试入口，调用 `parseGhidraCspecDefaultAbi(...)`，断言 SysV 默认 ABI 的关键 register 和 effect。
- `tests/native_abi_cspec_test.cpp:97`：创建空 LLVM module，调用 `attachNativeAbiMetadata(...)` 后执行 `llvm::verifyModule(...)`。

修改文件：

- `CMakeLists.txt:131`：新增 `native_abi_cspec_test` 测试可执行文件。
- `CMakeLists.txt:141`：新增 CTest `notdec.native_abi.cspec`，输入 Ghidra 的 `x86-64-gcc.cspec`。

验证命令：

```sh
cmake -S . -B build -DNOTDEC_LLVM_INSTALL_DIR=/sn640/NotDec/llvm-22.1.0.obj -DNOTDEC_BIN2LLVM_ENABLE_SLEIGH=OFF -DNOTDEC_BIN2LLVM_ENABLE_LIEF=OFF
cmake --build build --target native_abi_cspec_test -j2
ctest --test-dir build -R notdec.native_abi.cspec -V
```

结果：通过。`notdec.native_abi.cspec` 运行 `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec`，总用时 0.03 秒。

## 风险

- 测试覆盖的是当前 x86-64 SysV 默认 prototype 子集，不覆盖 MSABI、p-code inject、returnaddress、register offset/size。
- 测试依赖 `/sn640/ghidra` 存在。当前项目本来依赖这份 Ghidra source，先沿用这个前提。

## 评分

- 实现效果：8/10。把上一小步的 smoke 变成了 CTest，能防止 parser 和 metadata schema 被改坏。
- 复杂度：2/10。只加一个小 C++ 测试和 CMake 注册。
- 维护成本：2/10。断言集中且直接，对后续 ABI 字段扩展影响小。
