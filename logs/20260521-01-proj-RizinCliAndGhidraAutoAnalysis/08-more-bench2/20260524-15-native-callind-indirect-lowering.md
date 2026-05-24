# Native CALLIND Indirect Lowering

## 原始 prompt

修复

## 背景

wolfssl limited lowering 还剩一个 helper：

```text
notdec_pcode_CALLIND_void
```

对应指令：

```text
0x64264 MOV RAX,qword ptr [0x1f7fe8]
0x64274 CALL RAX
```

`llvm-readelf -r` 没看到 `0x1f7fe8` 的 dynamic relocation，所以不能把它安全归成 external GOT call。更合适的语义是保留未知函数指针调用，生成 LLVM indirect call。

## 修改

- [lib/PcodeToLLVM.cpp:989](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:989)
  新增 `lowerUnknownVoidIndirectCall(...)`。
- [lib/PcodeToLLVM.cpp:1032](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:1032)
  `lowerCallInd(...)` 在无法解析为已知 GOT external call 时，不再退回 helper，而是：

```llvm
%callee = inttoptr i64 %target to ptr
call void %callee()
```

当前 native lowering 还没有 ABI/prototype 模型，所以仍按 `void ()` 调用。这比 helper 更接近真实控制流，也不会伪造具体 callee。

## 验证

构建：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm -j2
```

wolfssl limited：

```text
/usr/bin/time -f 'TIME wolfssl-limited %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libwolfssl.so.42.0.0 \
  --all-confirmed \
  --decode-seed-limit 200 \
  --summary-json-out /tmp/notdec-wolfssl-limited-summary.json \
  -o /tmp/notdec-wolfssl-limited.ll

TIME wolfssl-limited 80.38
llvm-as ok
opt -passes=verify ok
```

结果：

```text
%10 = inttoptr i64 %unique_38a00_8 to ptr
call void %10()
```

`/tmp/notdec-wolfssl-limited.ll` 已无 `notdec_pcode_` helper。

CTest：

```text
notdec.native_discover.x86_64_smoke passed 3.85s
notdec.native_llvm.x86_64_smoke passed 0.58s
```

## 影响判断

- 实现效果：8/10。wolfssl limited 的最后一个 helper 清掉了，同时保留未知 indirect call 语义。
- 复杂度：3/10。只改 `CALLIND` unknown fallback，没有碰 GOT 已解析路径。
- 维护成本：4/10。未来有函数指针类型/ABI 信息后，可以把 `void ()` 换成更准的函数类型。

## 剩余问题

- wolfssl full discovery 仍然慢，和这次 CALLIND lowering 无关。
- selected-targets-native 需要用新版本重跑，确认全量目标是否还有其他 helper。
