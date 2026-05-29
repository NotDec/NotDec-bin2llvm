# 02 Prototype Storage Model

## 目标

复刻 Ghidra `ParamEntry` / `ParamList` 的核心 storage 匹配能力。

## Ghidra 对应位置

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh:196`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh:580`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc`

## 拟实现文件

- `include/notdec-bin2llvm/NativePrototypeModel.h`
- `lib/NativePrototypeModel.cpp`

## 第一批验证

- `RDI` 匹配 input slot 0。
- `RAX` 匹配 output slot 0。
- `RBX` 不匹配 input/output，但属于 unaffected。
