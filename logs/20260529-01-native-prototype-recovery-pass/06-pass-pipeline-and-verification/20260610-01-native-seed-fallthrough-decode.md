# Native seed fallthrough decode

用户原始要求：

> 对，改一下，因为每个seed应该就作为入口点不断解码，遇到控制流指令或者解码失败再当做信号的，而不是这样怎么就直接停下来了。

## 背景

`hexx64.so` 的 `0x1156e0` 在 native 链路里只解出 8 条指令，`0x1156f3` 后面 lowering 成了 `ret void`。这不是函数真实结束，而是 seed decode 的小窗口到了上限后没有继续把下一条地址当同函数后继。

Java/Ghidra 侧地址是 `0x2156e0`，来自镜像基址差异；对应的仍是同一个函数，不是导出了不同函数。

## 实现

- `include/notdec-bin2llvm/NativeAnalysis.h:319`
  - `NativeSleighDecodeOptions` 增加 `InitialFunctionEntries`。单函数查询可以只从指定入口解码，避免为了查一个函数顺手扫全库。

- `lib/NativeAnalysis.cpp:1322`
  - `NativeSleighDecodeAnalyzer::run` 使用 `DecodeSeedResult` 接收 call target、branch target 和 fallthrough target。
  - seed 窗口满后，如果最后一条指令不是无条件跳转、间接跳转或 return，并且 `rangeEnd` 仍在可执行区且不是已知其它函数入口，就把 `rangeEnd` 作为同函数 block 继续入队。

- `lib/NativeAnalysis.cpp:1383`
  - 新增 `DecodeSeedResult`。这个结构只表达一次小窗口解码后发现的后续工作：新函数 call target、同函数 branch target、同函数顺序 fallthrough。

- `lib/NativeAnalysis.cpp:1439`
  - `enqueueInitialSeeds` 支持 `InitialFunctionEntries`。有显式入口时，只入队这些入口。

- `lib/NativeAnalysis.cpp:1480`
  - `decodeSeed` 返回 `DecodeSeedResult`，不再只把显式 branch/call 传出去。

- `lib/NativeAnalysis.cpp:1532`
  - 新增 `fallthroughTargetForDecodedWindow`。它只在命中 8 条/64 字节小窗口上限且没有真实终止控制流时，返回顺序后继。

- `lib/NativeAnalysis.cpp:1563`
  - `addDecodedFunctionBlocks` 在最后一个 block 没有 successor 时补 fallthrough successor，并继续用现有逻辑过滤其它函数入口。

- `lib/PcodeToLLVM.cpp:191`
  - `buildBasicBlocks` 把 native CFG 的 block 起点和 successor 起点加入 LLVM block 切分点。否则 native discovery 已经有 `0x1156f3 -> 0x115729`，lowering 仍会把 `0x1156f3` 当普通外部块直接 return。

- `tools/notdec-native-discover.cpp:222`
  - `--function-json`、`--function-xrefs-json`、`--cfg-json`、`--cfg-dot`、`--instructions-function-json` 查询时设置 `InitialFunctionEntries`。

- `tools/notdec-native-llvm.cpp:883`
  - `-f <entry>` 且没有 `--all-confirmed` 时设置 `InitialFunctionEntries`。

## 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
```

`hexx64.so` 单函数 discovery：

```bash
/usr/bin/time -f 'TIME function-json-1156e0 %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --function-json 0x1156e0 /sn640/NotDec-Exp/Bench2/hexx64.so \
  > /tmp/hexx64-1156e0.function.json
```

结果：

- 时间：`61.10s`
- `entry`: `0x1156e0`
- `range_start`: `0x2e918`
- `range_end`: `0x128168`
- `block_count`: `14430`
- `instruction_count`: `16083`

`notdec-native-llvm -f 0x1156e0 --no-register-ssa-pass`：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/hexx64.so \
  -f 0x1156e0 --no-register-ssa-pass \
  -o /tmp/hexx64-1156e0-noregssa.ll

/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as \
  /tmp/hexx64-1156e0-noregssa.ll \
  -o /tmp/hexx64-1156e0-noregssa.bc

/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify \
  /tmp/hexx64-1156e0-noregssa.bc \
  -o /tmp/hexx64-1156e0-noregssa.verified.bc
```

结果：

- `notdec-native-llvm --no-register-ssa-pass`: `35.60s`
- `llvm-as`: `2.88s`
- `opt verify`: `1.57s`
- 起始 CFG 已变成 `bb_1156e0 -> bb_1156f3 -> bb_115729`，不再是 `bb_1156f3: ret void`。

限量 smoke：

```bash
/usr/bin/time -f 'TIME limited-summary %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --decode-seed-limit 20 --summary-json /bin/ls \
  > /tmp/binls-native-summary.json
```

结果：

- 时间：`0.22s`
- `confirmed_functions`: `20`
- `basic_blocks`: `41`
- `instructions`: `136`

## 剩余问题

默认 register SSA pass 在这个大 CFG 上仍失败：

```text
PHINode should have one entry for each predecessor of its parent basic block!
module verification failed after register SSA pass
```

这说明 seed 继续解码和 native CFG lowering 已经前进到更大的函数范围，后面的问题转到了 register SSA 的 PHI incoming 构造。这个问题不能用旧 slot fallback 掩盖，后续应单独按真实 CFG/SSA 语义修。

## 性能和风险

- 单函数 `0x1156e0` 从 8 条指令变成 16083 条指令，时间约 61s；这是预期的真实工作量增加。
- `/bin/ls --decode-seed-limit 20` 仍为 0.22s，说明显式 seed limit 的 smoke 入口没有退化。
- 风险：`0x1156e0` 当前范围很大，可能包含 shared/cold block，也可能还有过度连接。现阶段只确认不会因为 8 条窗口上限直接停早。

评分：

- 实现效果：8/10。解决停早和 lowering 断块，register SSA 仍有后续问题。
- 复杂度：4/10。增加一个结果结构和一个 fallthrough 判断，逻辑集中。
- 维护成本：4/10。沿用现有 seed queue，没有引入新的全局分析。
