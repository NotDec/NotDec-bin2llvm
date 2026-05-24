# Native 32/64-bit Float Lowering

## 原始 prompt

修复

## 背景

direct `CALL` helper 修掉后，`wrk --all-confirmed` 里剩下的主要 helper 是浮点：

- `FLOAT_ADD`
- `FLOAT_SUB`
- `FLOAT_MULT`
- `FLOAT_DIV`
- `FLOAT_EQUAL`
- `FLOAT_LESS`
- `FLOAT_NAN`
- `INT2FLOAT`
- `FLOAT2FLOAT`

其中一部分是 4/8-byte float，可以按 LLVM `float` / `double` 精确 lowering。另一部分是 x87 10-byte extended float，对应 IR 里 `i80`，当前 heritage lowering 也没有精确支持，所以本轮不碰。

## 修改

- [lib/PcodeToLLVM.cpp:311](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:311)
  新增 `floatType(...)`、`floatByteSize(...)`、`readFloatBits(...)`、`writeFloatBits(...)`，只接受 4/8-byte float。
- [lib/PcodeToLLVM.cpp:735](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:735)
  新增 `lowerFloatBinary(...)`，支持 `FLOAT_ADD` / `FLOAT_SUB` / `FLOAT_MULT` / `FLOAT_DIV`。
- [lib/PcodeToLLVM.cpp:777](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:777)
  新增 `lowerFloatCompare(...)`，支持 `FLOAT_EQUAL` / `FLOAT_NOTEQUAL` / `FLOAT_LESS` / `FLOAT_LESSEQUAL`。
- [lib/PcodeToLLVM.cpp:815](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:815)
  新增 `lowerFloatNan(...)`。
- [lib/PcodeToLLVM.cpp:828](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:828)
  新增 `lowerFloatCast(...)`，支持 `INT2FLOAT` / `FLOAT2FLOAT` / `FLOAT_TRUNC` 的 4/8-byte 情况。
- [lib/PcodeToLLVM.cpp:1065](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:1065)
  在 `lowerOp(...)` 里接入上述 float lowering。遇到 10-byte x87 float 时仍保留 helper。

## 验证

构建：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm -j2
```

Bench2 wrk：

```text
/usr/bin/time -f 'TIME %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk \
  --all-confirmed \
  -o /tmp/notdec-native-wrk.ll

TIME 64.59
llvm-as ok
opt -passes=verify ok
```

检查结果：

- `notdec_pcode_INT2FLOAT_i32` 已消失。
- `notdec_pcode_FLOAT_ADD_i32` / `notdec_pcode_FLOAT_DIV_i32` 已消失。
- 剩余 helper 都是 `i80` x87 extended float。

CTest：

```text
notdec.native_discover.x86_64_smoke passed 3.92s
notdec.native_llvm.x86_64_smoke passed 0.59s
```

## 影响判断

- 实现效果：6/10。常见 32/64-bit float helper 降为真实 LLVM float op。
- 复杂度：5/10。逻辑基本复用 heritage 侧做法，但 raw P-Code 没有高层类型，只能按 varnode size 判断。
- 维护成本：5/10。10-byte x87 仍需单独设计，不能混进普通 `float/double` 逻辑。

## 剩余问题

- x87 `i80` helper 仍存在。
- `FLOAT_NEG` / `FLOAT_ABS` / `FLOAT_SQRT` / `CEIL` / `FLOOR` / `ROUND` 仍未接入 raw lowering；这次 selected-targets 统计里不是主要样本。
