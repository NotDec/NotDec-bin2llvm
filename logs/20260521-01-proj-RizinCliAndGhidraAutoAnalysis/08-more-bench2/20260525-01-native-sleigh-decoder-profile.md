# Native Sleigh Decoder Profile

## 原始 prompt

使用profile工具具体看看性能，而不要这样推测的方式修复。sleigh engine的服用确实可以先做，似乎只会优化性能不会有负面影响

## 背景

`wolfssl` 完整 native discovery 之前会卡很久。不能只猜慢点，需要先用 profile 证明。

## Profile 结果

样本：

```bash
/home/ubuntu/.local/bin/perf stat -d \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --decode-seed-limit 200 --summary-json \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libwolfssl.so.42.0.0
```

修改前结果：

- 时间：`39.486515572 seconds`
- `function_seeds=4159`
- `confirmed_functions=200`
- `instructions=1403`

`perf record/report --children` 显示：

- `notdec::bin2llvm::collectSleighInstructionDecode`：约 `79.89%`
- `initializeSleighEngine`：约 `68.61%`
- 下面主要是 `ghidra::Sleigh::initialize` / `ghidra::SleighBase::decode` / `.sla` symbol table decode。

结论：主要慢点是每个 seed 都重新初始化 Sleigh engine，不是实际 decode 指令本身。

## 实现

修改 `include/notdec-bin2llvm/SleighLift.h:48-70`：

- 新增 `SleighInstructionDecoder`。
- 这个类持有一个初始化后的 Sleigh engine，用于多次 bounded decode。
- 用 Pimpl 避免把 libsla 的具体类型暴露到头文件。

修改 `lib/SleighLift.cpp:398-481`：

- 新增 `SleighInstructionDecoder::Impl`。
- 构造时只调用一次 `initializeSleighEngine(...)`。
- 缓存 endian 和 register 列表。
- `SleighInstructionDecoder::decode(...)` 复用同一个 engine，生成和旧接口一致的 `SleighInstructionDecode`。

修改 `lib/SleighLift.cpp:560-566`：

- 保留旧的 `collectSleighInstructionDecode(...)` API。
- 旧 API 内部创建临时 `SleighInstructionDecoder`，保证现有调用方不用改。

修改 `lib/NativeAnalysis.cpp:1315-1347` 和 `lib/NativeAnalysis.cpp:1461-1476`：

- `SleighSeedInstructionAnalyzer::run(...)` 创建一次 `SleighInstructionDecoder`。
- `decodeSeed(...)` 改为接收 decoder 引用，不再每个 seed 调旧的单次初始化 API。

修改 `lib/NativeAnalysis.cpp:1965-1981`：

- `capBytesAtNextFunctionSeed(...)` 从 `functionSeeds().upper_bound(blockAddress)` 开始找下一个 boundary seed。
- 之前每次从 map 开头扫；这个是顺手的小优化，语义不变。

## 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build \
  --target notdec-native-discover notdec-native-llvm -j2
```

格式检查：

```bash
git diff --check
```

CTest：

```bash
ctest --test-dir /tmp/notdec-bin2llvm-build \
  -R 'notdec.native_discover.x86_64_smoke|notdec.native_llvm.x86_64_smoke' \
  --output-on-failure
```

结果：

```text
notdec.native_discover.x86_64_smoke Passed 0.25 sec
notdec.native_llvm.x86_64_smoke     Passed 0.60 sec
Total Test time                     0.85 sec
```

同口径 `wolfssl --decode-seed-limit 200`：

```text
before perf stat: 39.486515572 seconds
after time:       0.41 seconds
```

输出一致性：

```bash
cmp -s /tmp/notdec-wolfssl-limit200-before.summary.json \
  /tmp/notdec-wolfssl-limit200-final.summary.json
```

结果：`cmp_before_final=0`。受限样本 summary 完全一致。

`wolfssl` 全量 discovery：

```text
TIME wolfssl-full-final 3.25
function_seeds=4160
confirmed_functions=4160
instructions=40181
```

`php calendar` 全量 discovery：

```text
TIME php-calendar-full-after 0.26
function_seeds=52
confirmed_functions=52
instructions=543
```

`notdec-native-llvm` 集成验证：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libwolfssl.so.42.0.0 \
  --all-confirmed --decode-seed-limit 200 \
  --summary-json-out /tmp/notdec-wolfssl-native-llvm-final/summary.json \
  -o /tmp/notdec-wolfssl-native-llvm-final/module-all.ll

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/notdec-wolfssl-native-llvm-final/module-all.ll \
  -o /tmp/notdec-wolfssl-native-llvm-final/module-all.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/notdec-wolfssl-native-llvm-final/module-all.bc \
  -o /tmp/notdec-wolfssl-native-llvm-final/module-all.verified.bc
```

结果：

```text
TIME wolfssl-native-llvm-limit200-final 42.59
function_seeds=4159
confirmed_functions=200
instructions=1403
LLVM 22 llvm-as / opt -passes=verify passed
```

## 结论

这次 profile 证明主要慢点是重复 Sleigh 初始化。复用 engine 后，`wolfssl --decode-seed-limit 200` 从约 39.49 秒降到 0.41 秒，summary 完全一致；`wolfssl` 全量 discovery 可以在 3.25 秒完成。

## 评分

- 实现效果：9/10。主要性能瓶颈被打掉，且 limited 样本输出完全一致。
- 复杂度：7/10。新增一个 decoder 类，但范围只在 SleighLift 和 native discovery。
- 维护成本：7/10。旧 API 保留，后续其他路径可按需复用同一个 decoder。
