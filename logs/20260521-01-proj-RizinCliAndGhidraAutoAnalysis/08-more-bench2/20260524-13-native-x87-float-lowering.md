# Native x87 Float Lowering

## 原始 prompt

修复

## 背景

`20260524-12-native-float32-64-lowering.md` 修了 4/8-byte float 后，`wrk --all-confirmed` 里剩下的 helper 全是 x87 10-byte extended float：

```text
notdec_pcode_FLOAT2FLOAT_i80
notdec_pcode_FLOAT_NAN_i8
notdec_pcode_FLOAT_EQUAL_i8
notdec_pcode_FLOAT_LESS_i8
notdec_pcode_FLOAT_DIV_i80
notdec_pcode_INT2FLOAT_i80
notdec_pcode_FLOAT_ADD_i80
notdec_pcode_FLOAT_MULT_i80
notdec_pcode_FLOAT_SUB_i80
```

LLVM 有 `x86_fp80` 类型，可以承接 10-byte raw P-Code float。

## 修改

- [lib/PcodeToLLVM.cpp:349](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:349)
  `floatType(10)` 返回 `llvm::Type::getX86_FP80Ty(Context)`。
- [lib/PcodeToLLVM.cpp:356](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:356)
  `floatByteSize(...)` 支持 `x86_fp80 -> 10`。

其他 float lowering 逻辑复用上一轮的 `readFloatBits(...)` / `writeFloatBits(...)` 和 `lowerFloatBinary(...)` / `lowerFloatCompare(...)` / `lowerFloatNan(...)` / `lowerFloatCast(...)`。

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

TIME 65.70
llvm-as ok
opt -passes=verify ok
```

`/tmp/notdec-native-wrk.ll` 里已经没有 `notdec_pcode_` helper。

CTest：

```text
notdec.native_discover.x86_64_smoke passed 3.89s
notdec.native_llvm.x86_64_smoke passed 0.59s
```

## 影响判断

- 实现效果：8/10。wrk 里的 native helper 已清空。
- 复杂度：2/10。只扩展已有 float type 映射，没有引入新的 x87 特殊路径。
- 维护成本：4/10。LLVM `x86_fp80` 能表达 10-byte 值，但后续如果要精确模拟 x87 status word / rounding mode，还需要单独处理；本次只解决 raw float value op。

## 剩余问题

- selected-targets-native 其他目标需要用新版本重跑后再统计 helper。
- 间接调用 `CALLIND` 如果仍出现，需要按 GOT/source tracking 继续修。
