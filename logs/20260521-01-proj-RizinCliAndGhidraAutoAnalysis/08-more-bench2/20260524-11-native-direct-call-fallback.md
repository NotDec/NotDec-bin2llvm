# Native Direct Call Fallback

## 原始 prompt

修复

## 背景

统计 selected-targets-native 现有 IR 后，剩余 helper 里最明显的是：

```text
notdec_pcode_CALL_void
notdec_pcode_CALLIND_void
```

其中 direct `CALL` 的目标地址已经在 raw P-Code 里明确存在。之前只有目标属于 confirmed function 或 PLT external 时才生成真实 LLVM call；其他 direct target 会退回 `notdec_pcode_CALL_void` helper。

## 修改

- [lib/PcodeToLLVM.cpp:125](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:125)
  新增 `addressFunctionName(...)`，把未知 direct call target 命名成 `notdec_native_<addr>`。
- [lib/PcodeToLLVM.cpp:856](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:856)
  `lowerCall(...)` 在找不到 `ExternalCallTargets` / `DirectCallTargets` 时，不再退回 helper，而是生成一个地址命名的 `void ()` 声明调用。

这个改动只处理 direct `CALL`，不处理 `CALLIND`。间接调用还需要 GOT/source tracking 或函数指针分析，不能随便造目标。

## 验证

构建：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm -j2
```

手工地址模式：

```text
notdec-native-llvm libuv.so.1.0.0 -a 0x9c38 -l 0x16 -o /tmp/notdec-native-libuv-9c38-manual.ll
llvm-as ok
opt -passes=verify ok
```

结果：

```text
call void @notdec_native_8e30()
declare void @notdec_native_8e30()
```

不再出现：

```text
notdec_pcode_CALL_void
```

Bench2 wrk：

```text
/usr/bin/time -f 'TIME %e' notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/wrk --all-confirmed -o /tmp/notdec-native-wrk.ll
TIME 64.57
llvm-as ok
opt -passes=verify ok
```

`/tmp/notdec-native-wrk.ll` 不再出现 `notdec_pcode_CALL_void`。

CTest：

```text
notdec.native_discover.x86_64_smoke passed 3.89s
notdec.native_llvm.x86_64_smoke passed 0.60s
```

## 影响判断

- 实现效果：6/10。消除了 direct `CALL` helper fallback，语义比 helper 更接近真实调用。
- 复杂度：2/10。只改 direct call fallback，不影响已有 confirmed / external call 解析。
- 维护成本：3/10。地址命名 declaration 是当前无原型模型下的保守做法，后续有原型恢复后再细化类型。

## 剩余问题

- `CALLIND` helper 还存在，后续要按实际样本继续做 GOT/source tracking 或函数指针解析。
- float / bitfield helper 本轮统计没有在 selected-targets-native 里成为主要问题，先不动。
