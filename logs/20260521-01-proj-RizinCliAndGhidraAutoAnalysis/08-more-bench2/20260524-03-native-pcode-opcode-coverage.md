# Native P-Code Opcode Coverage

## 原始 prompt

修复这两个指令，此外，最好检查一下完整的 op code 的表，把指令都支持完全

## 背景

去掉 native discovery 的 seed 数量限制后，`libuv --all-confirmed` confirmed function 已经到 485 个，但最终只输出 462 个 LLVM function。跳过原因主要是 native P-Code lowering 不支持 `INT_2COMP`，另有一个 `INT_NEGATE`。

同时 `lib/SleighLift.cpp` 的 opcode 转换表也不是完整 Ghidra P-Code opcode 表，未知 opcode 会变成 `Unsupported`，导致整个函数跳过。

## 目标

1. 修复 `INT_2COMP` 和 `INT_NEGATE`。
2. 对照本地 Ghidra `OpCode.java` 补齐 native `PcodeOpcode` 枚举、名字表和 Sleigh opcode 转换表。
3. 对语义清楚的整数/指针/计数 opcode 做 native LLVM lowering。
4. 对当前 native 没有类型/运行时模型的 opcode 走 helper，不再因为 opcode 表缺项直接跳过函数。

## 实现

修改 `include/notdec-bin2llvm/Pcode.h`：

- 第 23 行到第 85 行，补齐 Ghidra P-Code opcode 枚举，包含 `INT_2COMP`、`INT_NEGATE`、`INT_SDIV`、`INT_SREM`、浮点 op、`INDIRECT`、`CAST`、`PTRADD`、`PTRSUB`、`SEGMENTOP`、`CPOOLREF`、`NEW`、`INSERT`、`EXTRACT`、`LZCOUNT` 等。

修改 `lib/Pcode.cpp`：

- `pcodeOpcodeName(...)` 补齐新增 opcode 的名字输出。

修改 `lib/SleighLift.cpp`：

- `convertOpcode(...)` 补齐从 `ghidra::CPUI_*` 到 `PcodeOpcode` 的转换。来源是 `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/pcodeCPort/opcodes/OpCode.java`。

修改 `lib/PcodeToLLVM.cpp`：

- 第 369 行附近，`lowerBinary(...)` 支持 `INT_SDIV` 和 `INT_SREM`。
- 第 426 行附近，`lowerCompare(...)` 支持 `INT_LESSEQUAL` 和 `INT_SLESSEQUAL`。
- 第 480 行附近，新增 `lowerUnary(...)`：
  - `INT_NEGATE` -> LLVM `not`
  - `INT_2COMP` -> LLVM `neg`
- 第 529 行附近，`POPCOUNT` / `LZCOUNT` 统一用 `lowerCountBits(...)`，分别落到 `ctpop` 和 `ctlz`。
- 第 549 行附近，`CAST` / `INDIRECT` 先按 copy-like 处理。
- 第 557 行附近，新增 `PTRADD` 和 `PTRSUB` lowering。
- `FLOAT_*`、`SEGMENTOP`、`CPOOLREF`、`NEW`、`INSERT`、`EXTRACT` 目前走 helper call。这里不是精确语义，只是不再因为 opcode 表缺项让整函数失败。
- `MULTIEQUAL` 仍报错，因为 raw Sleigh P-Code 不应出现 SSA PHI；如果出现，需要单独查来源。

## 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
```

小目标验证：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/php/20230831/calendar.so \
  --all-confirmed -o /tmp/notdec-native-calendar-opcodes.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/notdec-native-calendar-opcodes.ll \
  -o /tmp/notdec-native-calendar-opcodes.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-native-calendar-opcodes.bc \
  -o /tmp/notdec-native-calendar-opcodes.verified.bc
```

结果：

```text
calendar.so defines: 52
```

`libuv --all-confirmed`：

```text
TIME 247.19
define functions: 484
declare functions: 110
```

`llvm-as` 和 `opt -passes=verify` 通过：

```bash
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-native-libuv-opcodes.ll -o /tmp/notdec-native-libuv-opcodes.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-native-libuv-opcodes.bc -o /tmp/notdec-native-libuv-opcodes.verified.bc
```

`INT_2COMP` / `INT_NEGATE` skip 已消失。当前 `libuv` 只剩一个跳过函数：

```text
skip native function 0x9c38: CBRANCH target must be a direct ram address
```

这属于控制流 lowering 限制，不是 opcode 表缺项。

## 结论

这次修复了 `INT_2COMP` 和 `INT_NEGATE`，并补齐 native raw Sleigh opcode 表。`libuv --all-confirmed` 从 462 个 LLVM function 增加到 484 个，接近 485 个 confirmed function。

“完整支持”这里分两层：

- opcode 表已经按 Ghidra P-Code 表补齐。
- LLVM 精确语义还没有全补齐。浮点、`SEGMENTOP`、`CPOOLREF`、`NEW`、`INSERT`、`EXTRACT` 当前走 helper，后续需要结合类型、地址空间和运行时模型再精确实现。

## 性能

本次改动主要增加 lowering 覆盖，不改变 discovery 遍历量。`libuv --all-confirmed` 仍是几分钟级，和上一轮全量 discovery/lowering 的性能特征一致。

## 评分

- 实现效果：8/10。解决了当前已知 unsupported opcode，函数输出从 462 提升到 484。
- 复杂度：7/10。opcode 表变完整，lowering 分发变长，但逻辑直接。
- 维护成本：7/10。后续新增 opcode 不应再落到 `Unsupported`，但 helper opcode 还需要逐步精确化。
