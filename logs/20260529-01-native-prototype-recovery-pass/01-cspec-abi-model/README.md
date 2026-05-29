# 01 Cspec ABI Model

## 目标

复刻 Ghidra compiler spec 里和 ABI 相关的数据结构，形成 native 侧轻量 `NativeAbiSpec`。

## Ghidra 对应位置

- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/lang/PrototypeModel.java:626`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh:726`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`
- `/sn640/ghidra/Ghidra/Processors/x86/data/languages/x86-64-gcc.cspec`

## 拟实现文件

- `include/notdec-bin2llvm/NativeAbi.h`
- `lib/NativeAbi.cpp`
- `lib/CMakeLists.txt`

## 第一批验证

- 解析 x86-64 gcc cspec 的 default prototype。
- `llvm-as` / `opt -passes=verify` 接受带 `!notdec.abi` 的输出 IR。
