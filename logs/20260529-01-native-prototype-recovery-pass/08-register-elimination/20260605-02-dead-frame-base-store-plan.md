# 原始 prompt

按这个继续推进吧

# 背景

no-return call-frame cleanup 后，`libuv:shared-library` 的 `RSP/RBP` residue 剩 15 行：

```text
6 call_frame_state
3 saved_register_restore,call_frame_state
2 stack_frame_state
2 frame_base_state
2 chunk_phi
```

其中 `frame_base_state` 只有两行，形态很集中：

```llvm
%RSP.external_input = load i64, ptr @RSP
%0 = add i64 %RSP.external_input, -8
store i64 %0, ptr @RBP
```

这类 `RBP` store 表示当前函数 frame base，但当前函数里没有后续 `RBP` load。它不是返回值，也不是调用参数。继续保留会让 `RSP.external_input` 和 stack-frame store 更难清干净。

# 目标

做一个窄小步：

- 只删除死的 frame-base `RBP` store。
- store 的值必须来自当前函数 `RSP.external_input` 派生。
- 函数内不能有真实 `RBP` load。
- 函数内 call 不能可能读取 `RBP`。

# Ghidra 对应

Ghidra 里 `RBP` 不是正式 stack pointer。x86 DWARF / cspec 里 frame pointer 只是帮助把 frame-relative 地址映射回 stack space：

- `x86-64-gcc.cspec` 里正式 stack pointer 是 `RSP`。
- `x86-64.dwarf` 通过 `stack_frame register="RBP"` 描述 frame base。
- decompiler 后续把 frame base 用于 stack varnode 定位，而不是保留成普通全局寄存器状态。

native 侧当前只做这个思路的安全子集：如果 `RBP` store 没有任何读取者，就删掉它，不尝试扩大到 frame-relative raw load rewrite。

# 技术路线

1. 增加 dead frame-base store cleanup。
   - 只遍历 ABI unaffected 里的 frame pointer register，目前就是 `RBP` 族。
   - 函数内如果有该 frame register 的任何 load，跳过整个函数。
   - 函数内如果有任何 call 可能读取该 frame register，跳过整个函数。
   - 只删除 `store @RBP` 且 value 是 stack-frame external input 派生的 store。

2. 复用现有判断。
   - 用 `valueUsesExternalInputRegister(value, abi.StackPointerRegister)` 判断 store value 来源。
   - 用 `callMayReadRegisterName(call, "RBP")` 保守判断 call 边界。
   - 删除后递归清理变死的地址计算。

# 判断标准

- 单测覆盖：
  - `RBP = RSP.external_input - 8` 且没有后续 RBP load/call 读取时删除。
  - 后续有 RBP load 时保留。
- `native_prototype_recovery_test` 通过。
- 相关 native CTest 通过。
- `libuv:shared-library` signature-rewrite 通过 LLVM 22 verify，`frame_base_state` 下降。

# 风险

- 如果某个 call 通过未建模方式读 `RBP`，误删会影响语义。所以第一版要求所有 call 都被 `callMayReadRegisterName()` 判为不读。
- 这不解决 `RSP` stack-frame store 本身，也不处理普通 returning call 的 return-address store。
- `chunk_phi` 里的 external `RBP` 不在本轮范围内。

# 不做什么

- 不把 `RBP` 默认当 stack pointer。
- 不处理有 `RBP` load 的 frame-relative raw access。
- 不处理 dynamic stack、chunk 边界、普通 returning call-frame store。

# 2026-06-05 实现记录

改动：

- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3916)
  - 新增 `functionHasRegisterLoad()`，用于判断函数里是否仍有真实 frame register load。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3929)
  - 新增 `functionCallsMayReadRegister()`，对 call 边界复用 `callMayReadRegisterNameForDeadFrameStore()`。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:3943)
  - 新增 `eraseDeadFrameBaseRegisterStores()`。
  - 只处理 ABI unaffected 里的 frame pointer register。
  - 函数内有该 register load 或 call 可能读取该 register 时跳过。
  - 只删除 value 来自 ABI stack pointer external input 的 frame-base store。
- [NativePrototypeRecovery.cpp](/sn640/NotDec/external/NotDec-bin2llvm/lib/passes/NativePrototypeRecovery.cpp:6179)
  - 在 `replaceStoredFramePointerRegisterLoads()` 之后运行 dead frame-base store cleanup。
- [native_prototype_recovery_test.cpp](/sn640/NotDec/external/NotDec-bin2llvm/tests/native_prototype_recovery_test.cpp:9972)
  - 增加 dead local frame-base case，验证 `RBP = RSP.external_input - 8` 且没有真实 `RBP` load 时可删除。
  - 已有 `caller_reads_declaration_rbp_store` 继续覆盖负例：后续读 `RBP` 时必须保留。

验证：

```bash
git diff --check
cmake --build build --target native_prototype_recovery_test -j2
./build/bin/native_prototype_recovery_test
ctest --test-dir build -R 'notdec\.native_(prototype|instcombine|register|abi)|native_register_residue' --output-on-failure
cmake --build build --target notdec-native-llvm -j2

scripts/bench2-native-prototype-audit.sh \
  --build-dir build \
  --out-dir /tmp/notdec-bin2llvm-dead-frame-base-gate2 \
  --target libuv:shared-library

python3 scripts/native-register-residue-audit.py \
  /tmp/notdec-bin2llvm-dead-frame-base-gate2/libuv-shared-library.signature-rewrite.ll
python3 scripts/native-register-residue-audit.py --details \
  /tmp/notdec-bin2llvm-dead-frame-base-gate2/libuv-shared-library.signature-rewrite.ll \
  > /tmp/notdec-libuv-dead-frame-base-details2.tsv
```

结果：

- `native_prototype_recovery_test` 通过。
- 相关 CTest 6/6 通过。
- `libuv:shared-library` 完整 gate 通过 LLVM 22 assemble/verify：

| target | all-confirmed | signature-rewrite |
| --- | ---: | ---: |
| `libuv:shared-library` | 106s | 106s |

`libuv` 上 `RSP/RBP` 明细从 15 行降到 13 行，`frame_base_state` 清零：

```text
reason:
8 entry_external_input
5 stack_pointer

semantic:
7 call_frame_state
2 stack_frame_state
2 saved_register_restore,call_frame_state
2 chunk_phi
```

判断：

- 本轮按预期只清掉 dead frame-base `RBP` store，没有碰普通 returning call-frame store。
- 剩余主要是 `RSP` call-frame / stack-frame state 和 2 个 `chunk_phi`。
- 下一步如果继续压 `libuv`，应在 `uv_timer_stop` / `uv_disable_stdio_inheritance` / `uv_os_get_passwd` 三类里选一个：普通 returning call-frame store、stack-frame store，或 chunk 边界。

复杂度评分：

| 角度 | 分数 | 判断 |
| --- | ---: | --- |
| 实现效果 | 2 | `libuv` 上只减少 2 行，但准确清零 `frame_base_state`。 |
| 理解成本 | 2 | 新增规则很窄，复用现有 frame-store call 活性判断。 |
| 维护成本 | 2 | 条件保守，后续如果 frame-relative raw access 恢复更完整，可以保留或合并进 frame-base pass。 |

有没有更好的方案：

- 更完整方案是把 `RBP` frame base 映射到 stack space 后统一处理 frame-relative load/store。
- 当前先删死 frame-base store，是因为真实样例里没有后续 `RBP` load，直接删比引入更大的 frame-base 模型更稳。
