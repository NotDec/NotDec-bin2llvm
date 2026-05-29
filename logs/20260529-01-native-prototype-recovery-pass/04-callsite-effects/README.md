# 04 Callsite Effects

## 目标

复刻 Ghidra 对 callsite 的副作用判断：外部 call 按 ABI，本模块 direct call 按 callee 分析结果。

## Ghidra 对应位置

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc:1458`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/heritage.cc:1521`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.cc:4233`

## 拟实现文件

- `include/notdec-bin2llvm/passes/NativeCallEffects.h`
- `lib/passes/NativeCallEffects.cpp`
- `lib/passes/NativeRegisterSSA.cpp`

## 第一批验证

- 外部 call 后 `RAX` 不从 call 前传播。
- 外部 call 后 `RBX` 可按 ABI preserved 传播。
- 本模块 direct call 使用 callee metadata。
