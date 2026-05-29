# Native LLVM IntToPtr Memory Model

## 原始 prompt

要不，不使用getelementptr+notdec_ram，而是使用inttoptr指令。或者做成两种可以切换的模式，然后默认生成inttoptr这种。其次，是否应该在模块的meta data里面增加一些内存映射memory mapping的信息？

是的，按照这个计划改吧

## 背景

native 链路当前把 P-Code `ram` 空间降成：

```llvm
@notdec_ram = external global [1048576 x i8]
getelementptr [1048576 x i8], ptr @notdec_ram, ...
```

这个模型适合把内存限制在一个显式 IR 对象里，但对真实 ELF 不自然：地址不再直接对应 ELF virtual address，也不方便和 debug info、section、relocation 对齐。

## 目标

保留旧模型，同时让 native 默认输出用真实地址指针：

```llvm
inttoptr i64 <address> to ptr
```

并在模块 metadata 里记录 ELF LOAD memory map，方便后续分析知道哪些地址范围可读、可写、可执行。

## 实现

- [include/notdec-bin2llvm/PcodeToLLVM.h:18](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/PcodeToLLVM.h:18)
  - 新增 `PcodeMemoryModel`：
    - `GlobalArray`
    - `IntToPtr`
  - [include/notdec-bin2llvm/PcodeToLLVM.h:30](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/PcodeToLLVM.h:30)
    - `PcodeLoweringConfig` 增加 `MemoryModel`，默认仍是 `GlobalArray`。
    - 这样 `notdec-sleigh-llvm` 这类字节级工具保持旧行为。

- [lib/PcodeToLLVM.cpp:964](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:964)
  - `PcodeLowerer::memoryPointer` 按配置切换。
  - `IntToPtr` 模式直接生成 `inttoptr`。
  - `GlobalArray` 模式继续生成 `@notdec_ram` + `getelementptr`。

- [tools/notdec-native-llvm.cpp:52](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:52)
  - native CLI 默认 `MemoryModel = IntToPtr`。
  - [tools/notdec-native-llvm.cpp:77](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:77)
    - 新增 `--memory-model inttoptr|global-array`。
  - [tools/notdec-native-llvm.cpp:148](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:148)
    - 解析该参数，非法值报错。

- [tools/notdec-native-llvm.cpp:566](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:566)
  - 新增 `attachMemoryMapMetadata`。
  - 输出 `!notdec.memory_map`，每个 LOAD range 记录：
    - `start`
    - `end`
    - `read`
    - `write`
    - `execute`
  - [tools/notdec-native-llvm.cpp:769](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:769)
    - 所有 native LLVM 输出都会附加 metadata；`-a/-l` 这种不跑 full discovery 的路径也会从 ELF segment 构造 memory map。

## 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target notdec-native-llvm notdec-sleigh-llvm -j2
```

CTest：

```bash
ctest --test-dir /tmp/notdec-bin2llvm-build \
  -R 'notdec\.(bench2_native_discovery_debug_oracle_unit|native_discover\.x86_64_smoke|native_llvm\.x86_64_smoke)' \
  --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 3
Total Test time (real) =   0.87 sec
```

`php calendar` 默认 `inttoptr`：

```bash
/usr/bin/time -f 'TIME php-inttoptr %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/php/20230831/calendar.so \
  --all-confirmed \
  -o /tmp/notdec-memory-model-check/php-inttoptr.ll \
  --summary-json-out /tmp/notdec-memory-model-check/php-summary.json

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/notdec-memory-model-check/php-inttoptr.ll \
  -o /tmp/notdec-memory-model-check/php-inttoptr.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-memory-model-check/php-inttoptr.bc \
  -o /tmp/notdec-memory-model-check/php-inttoptr.opt.bc
```

结果：

```text
TIME php-inttoptr 11.61
default @notdec_ram count: 0
!notdec.memory_map = !{...}
```

`php calendar` 兼容 `global-array`：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/php/20230831/calendar.so \
  --all-confirmed \
  -o /tmp/notdec-memory-model-check-global/php-global.ll \
  --memory-model global-array
```

结果：

```text
@notdec_ram = external global [1048576 x i8]
```

`libuv` 限量验证：

```bash
/usr/bin/time -f 'TIME libuv-inttoptr-limit200 %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 \
  --all-confirmed \
  --decode-seed-limit 200 \
  -o /tmp/notdec-memory-model-libuv/libuv-inttoptr.ll
```

结果：

```text
TIME libuv-inttoptr-limit200 42.53
libuv @notdec_ram lines: 0
libuv inttoptr lines: 632
!notdec.memory_map = !{!24}
!25 = !{!"start=0x0", !"end=0x7650", !"read=true", !"write=false", !"execute=false"}
!26 = !{!"start=0x8000", !"end=0x2924d", !"read=true", !"write=false", !"execute=true"}
!27 = !{!"start=0x2a000", !"end=0x326d8", !"read=true", !"write=false", !"execute=false"}
!28 = !{!"start=0x336e0", !"end=0x346e8", !"read=true", !"write=true", !"execute=false"}
```

非法参数：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/php/20230831/calendar.so \
  --all-confirmed \
  -o /tmp/notdec-memory-model-check/bad.ll \
  --memory-model bad
```

结果：

```text
invalid memory model: bad
```

## 结论

native 默认输出现在更贴近真实地址空间，不再生成 `@notdec_ram`。旧模型可以通过 `--memory-model global-array` 保留。

metadata 已经能表达 ELF LOAD memory map，后续可以扩展 section name、file offset、segment index 等字段。

## 评分

- 实现效果：9/10。解决默认 native IR 的地址表达问题，同时保留旧模式。
- 复杂度：4/10。改动集中在 P-Code memory pointer 生成和 native CLI。
- 维护成本：4/10。模式枚举清楚，默认策略只在 native CLI 层改变。
