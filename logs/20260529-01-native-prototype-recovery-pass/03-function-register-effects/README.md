# 03 Function Register Effects

## 目标

基于寄存器 SSA 结果，标注每个函数实际 preserved / clobbered register。

## Ghidra 对应位置

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc:1467`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc:4233`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/funcdata_varnode.cc:366`

## 拟实现文件

- `include/notdec-bin2llvm/passes/NativeRegisterSSA.h`
- `lib/passes/NativeRegisterSSA.cpp`
- 需要拆分时新增 `include/notdec-bin2llvm/passes/NativeRegisterEffects.h`
- 需要拆分时新增 `lib/passes/NativeRegisterEffects.cpp`

## 第一批验证

- 写后恢复的 `RBX` 标成 preserved。
- 写后未恢复的 caller-saved 标成 clobbered。
- 未写 ABI unaffected register 不误报 clobbered。
