# Native external prototype JSON loading

## 背景

Bench2 的 `lighttpd-angel` 暴露了一个外部函数原型问题：`fork()` 没有被识别成有返回值，SummarySSA 在调用后留下了 `notdec.register.summary_return.i32()`。这不是寄存器 SSA 本身的问题，而是外部函数原型表不够准，并且原型表原来直接写在 `NativeRegisterSummarySSA.cpp` 里，不方便覆盖。

## 修改

- `include/notdec-bin2llvm/NativeExternalPrototype.h:16` 新增 `NativeExternalPrototype` 和 `NativeExternalPrototypeMap`，把外部函数原型从 SummarySSA 代码里抽出来。
- `lib/NativeExternalPrototype.cpp:17` 增加 JSON 解析，支持 `i32/i64/float/double/void`、`fixed_args`、`vararg`、`noreturn`、`max_return_registers`、`params`、`return`。
- `lib/NativeExternalPrototype.cpp:178` 保留完整旧默认表，避免 JSON 化改变原有行为；其中 `fork` 改成 `pid_t/i32` 返回。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:278` 让签名重写状态持有同一份外部原型表，所有 known/unknown 外部函数判断走同一入口。
- `lib/passes/summary/NativeRegisterSummarySSA.cpp:5324` 在 `runNativeRegisterSummarySSA()` 中加载默认表，并在设置 `ExternalPrototypeJsonPath` 时加载 JSON 覆盖。
- `tools/notdec-native-llvm.cpp:64` 增加 CLI 参数存储，`tools/notdec-native-llvm.cpp:228` 增加 `--external-prototypes <path>` 解析，`tools/notdec-native-llvm.cpp:984` 透传给 SummarySSA。
- `lib/CMakeLists.txt:6` 把 `NativeExternalPrototype.cpp` 加入 `notdec-bin2llvm-core`。

JSON 支持三种根格式：

```json
{"fork": {"return": "pid_t", "fixed_args": 0}}
```

```json
[{"name": "fork", "return": "pid_t", "fixed_args": 0}]
```

```json
{"prototypes": [{"name": "fork", "return": "pid_t", "fixed_args": 0}]}
```

## 验证

- `cmake --build external/NotDec-bin2llvm/build --target notdec-native-llvm native_register_summary_ssa_test -j4`
- `external/NotDec-bin2llvm/build/bin/native_register_summary_ssa_test`
- `notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/lighttpd-angel -o /tmp/notdec-bin2llvm-lighttpd-angel-external-proto-full-20260706131440/native.ll --all-confirmed --skip-runtime --register-ssa-warning-out /tmp/notdec-bin2llvm-lighttpd-angel-external-proto-full-20260706131440/register-ssa-warnings.tsv`
- `notdec-native-llvm /sn640/NotDec-Exp/Bench2/rootfs/usr/games/fortune -o /tmp/notdec-bin2llvm-fortune-external-proto-full-20260706131442/fortune.native.ll --all-confirmed --skip-runtime --register-ssa-warning-out /tmp/notdec-bin2llvm-fortune-external-proto-full-20260706131442/register-ssa-warnings.tsv`
- `/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as` 和 `/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify` 均通过。

结果：

- `lighttpd-angel`: `fork` 变为 `declare i32 @fork()`，`summary_return=0`，`summary_clobber=0`，`summary_ssa_metadata=0`，运行时间 `1.36s`。
- `lighttpd-angel`: 仍有 3 个 `R12/R13` 全局引用，是 PHI 间接寄存器全局访问问题，和外部原型无关。
- `fortune`: `summary_return=0`，`summary_clobber=0`，`register_global_refs=0`，`summary_ssa_metadata=0`，运行时间 `12.16s`。
- JSON 覆盖路径也用临时 `external-prototypes.json` 验证过，`fork` 的 `pid_t/i32` 返回按 JSON 生效。

## 影响和风险

- 实现效果：8/10。默认表完整迁移，支持 JSON 覆盖，已修复 `fork()` 这类缺返回值导致的残留 helper。
- 复杂度：4/10。只是把原表抽到独立 provider，并加 JSON 解析；SummarySSA 仍只消费一个 map。
- 维护成本：4/10。后续补 libc/第三方库原型可以走 JSON，不必改 SummarySSA 主逻辑。

主要风险是 JSON 里的类型仍是很小的 ABI 级集合，暂时不表达结构体、指针层级和完整 C 类型；这符合当前 SummarySSA 只需要参数个数、返回寄存器和少量 float/int ABI slot 的需求。
