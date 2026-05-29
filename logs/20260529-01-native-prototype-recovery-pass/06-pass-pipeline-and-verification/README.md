# 06 Pass Pipeline And Verification

## 目标

把 ABI、register effects、call effects、参数/返回候选串成完整 native prototype recovery pass，并固定 Bench2 验证。

## Ghidra 对应位置

- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc:4707`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/coreaction.cc:4765`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh:787`
- `/sn640/ghidra/Ghidra/Features/Decompiler/src/decompile/cpp/fspec.hh:794`

## 拟实现文件

- `include/notdec-bin2llvm/passes/NativePrototypeRecovery.h`
- `lib/passes/NativePrototypeRecovery.cpp`
- `tools/notdec-native-llvm.cpp`
- `scripts/bench2-native-smoke.sh`
- `/sn640/NotDec-Exp/Bench2/scripts/export-bin2llvm-selected-targets.py`

## 第一批验证

- `/bin/ls` native LLVM smoke 通过。
- Bench2 `vsftpd/libuv/memcached` 通过。
- selected native 全量 summary 没有新增 `llvm-as-failed` / `verify-failed`。
