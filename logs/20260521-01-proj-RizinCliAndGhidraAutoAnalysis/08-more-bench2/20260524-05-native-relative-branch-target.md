# Native Relative P-Code Branch Target

## 原始 prompt

修复

## 背景

`libuv --all-confirmed` 在 opcode 覆盖修复后还能输出 484 个 LLVM function，但 discovery 已经 confirmed 485 个函数。剩下的跳过函数是：

```text
skip native function 0x9c38: CBRANCH target must be a direct ram address
```

检查 `0x9c38` 的 raw P-Code 后发现，失败点不是机器地址分支，而是 raw Sleigh P-Code 内部的相对分支：

```text
CBRANCH (const,0x4,4) (register,0x206,1)
...
BRANCH (const,0x2,4)
```

这里的 `const` target 表示相对当前 P-Code op 的 op index，不是 `ram` 地址。native lowering 之前只接受 `ram` target，所以整函数失败。

另一个相关问题是 `0x9c38` 的 discovered range 很大：

```text
entry: 0x9c38
range: 0x9c38..0x2375f
```

这会让单函数 lowering 很慢，但本次只修相对 P-Code 分支，不改 range/CFG 策略。

## 实现

修改 `lib/PcodeToLLVM.cpp`：

- 第 63 行，`PcodeLowerer::lower(...)` 保存当前 `program.Ops` 指针，供 terminator lowering 根据 op index 找相对 target。
- 第 79 行，调用 `lowerTerminator(...)` 时传入当前 `opIndex`。
- 第 134 行，新增 `relativeTargetIndex(...)`，识别 `const` target，并按 `opIndex + offset` 解析到目标 P-Code op。
- 第 178 行，`buildBasicBlocks(...)` 对 `BRANCH` / `CBRANCH` 的 const relative target 也加入 block start。
- 第 220 行，新增 `blockForRelativeTarget(...)`，把 relative target index 映射到已有 basic block。
- 第 251 行，`lowerTerminator(...)` 支持：
  - `BRANCH ram`：原逻辑不变。
  - `BRANCH const`：跳到相对 P-Code target block。
  - `CBRANCH ram`：原逻辑不变。
  - `CBRANCH const`：true 边跳到相对 P-Code target block，false 边继续 fallthrough。

## 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm -j2
```

单函数验证：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 \
  -f 0x9c38 \
  -o /tmp/notdec-native-libuv-9c38-relbranch.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/notdec-native-libuv-9c38-relbranch.ll \
  -o /tmp/notdec-native-libuv-9c38-relbranch.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-native-libuv-9c38-relbranch.bc \
  -o /tmp/notdec-native-libuv-9c38-relbranch.verified.bc
```

结果：

```text
rc=0
TIME 281.99
defines 1
```

完整 libuv 验证：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 \
  --all-confirmed \
  -o /tmp/notdec-native-libuv-relbranch-all.ll
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/notdec-native-libuv-relbranch-all.ll \
  -o /tmp/notdec-native-libuv-relbranch-all.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-native-libuv-relbranch-all.bc \
  -o /tmp/notdec-native-libuv-relbranch-all.verified.bc
```

结果：

```text
rc=0
TIME 257.81
defines 485
declares 216
```

## 结论

`libuv --all-confirmed` 已经从 484/485 修到 485/485，并通过 LLVM 22 assemble / verify。

性能仍然很差，尤其是 `0x9c38` 单函数因为 range 过大需要 281.99 秒。后续仍需要处理 discovery range/CFG 和 smoke/coverage 拆分问题。

## 评分

- 实现效果：9/10。直接修掉当前唯一剩余的 libuv lowering skip。
- 复杂度：7/10。只支持 raw P-Code const relative branch，没有引入新的 CFG 抽象。
- 维护成本：7/10。相对 branch 是 Sleigh 原生语义，后续 raw P-Code lowering 都会复用。
