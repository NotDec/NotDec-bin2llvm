# 05 Input Output Candidates

## 目标

复刻 Ghidra `ParamActive` / `ParamTrial` 的候选筛选思路，输出参数和返回值候选 metadata。

## Ghidra 对应位置

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh:210`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh:285`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc:4707`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc:4765`

## 拟实现文件

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h`
- `lib/passes/NativePrototypeRecovery.cpp`
- `tools/notdec-native-llvm.cpp`

## 第一批验证

- `RDI` external input 且真实参与计算时标成 input candidate。
- 保存恢复用的 `RBX` 不标成 input candidate。
- return 前写出的 `RAX` 标成 return candidate。
