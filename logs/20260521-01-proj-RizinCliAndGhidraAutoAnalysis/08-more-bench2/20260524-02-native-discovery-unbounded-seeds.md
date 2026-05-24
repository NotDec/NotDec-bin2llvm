# Native Discovery Unbounded Seed Decode

## 原始 prompt

MaxInitialSeeds = 10、 最 多  MaxSeeds = 20，这个限制肯定是不对吧，就是因为这个吗，去掉试试

## 背景

Bench2 selected native rerun 里，`libuv` 的 native `module-all.ll` 只有 11 个函数，而 ghidrascript 链路有 478 个函数。排查发现 native discovery 已经找到很多 function seed，但 `SleighSeedInstructionAnalyzer` 只从前 10 个初始 seed 开始，最多 decode 20 个 seed，所以大部分 seed 没有变成 confirmed function。

## 目标

去掉 `SleighSeedInstructionAnalyzer` 的全局 seed 数量限制，先验证函数数量少是不是主要由 `MaxInitialSeeds` 和 `MaxSeeds` 造成。每个 seed 的局部 decode 窗口仍保留，避免一次 decode 线性吃太多字节。

## 实现

修改 `lib/NativeAnalysis.cpp`：

- 第 1319 行，`SleighSeedInstructionAnalyzer::run(...)` 的 decode 循环改为一直消费队列，不再用 `decodedSeedCount < MaxSeeds` 截断。
- 第 1345 行，删除 `MaxInitialSeeds` 和 `MaxSeeds`，保留 `MaxInstructionsPerSeed = 8` 和 `MaxBytesPerSeed = 64`。
- 第 1410 行，`enqueueInitialSeeds(...)` 仍按 high / medium / low 顺序遍历 `functionWorklist()`，但不再只入队前 10 个 seed。

没有改 P-Code lowering、CFG 构造和每 seed decode 窗口。

## 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
```

Bench2 三目标 summary：

```text
libuv     TIME 141.87  seeds=485  confirmed=485  blocks=1072  instr=4376  xrefs=583  unresolved=3
vsftpd    TIME 55.70   seeds=187  confirmed=187  blocks=375   instr=1908  xrefs=579  unresolved=2
memcached TIME 74.52   seeds=259  confirmed=259  blocks=511   instr=2601  xrefs=479  unresolved=4
```

`libuv --all-confirmed`：

```text
TIME 241.92
define functions: 462
declare functions: 57
```

`llvm-as` 和 `opt -passes=verify` 通过：

```bash
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as /tmp/notdec-native-libuv-unbounded.ll -o /tmp/notdec-native-libuv-unbounded.bc
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify /tmp/notdec-native-libuv-unbounded.bc -o /tmp/notdec-native-libuv-unbounded.verified.bc
```

CTest：

```text
ctest --test-dir /tmp/notdec-bin2llvm-build -R 'notdec.native_discover.x86_64_smoke' --output-on-failure
Passed, 59.95 sec
```

## 结论

函数数量少的主因确实是 `MaxInitialSeeds = 10` 和 `MaxSeeds = 20`。去掉后，`libuv` confirmed function 从 11 个涨到 485 个，`--all-confirmed` 最终输出 462 个 LLVM function，已经接近 ghidrascript 的 478 个。

代价是性能明显下降：`libuv` discovery 需要 141.87 秒，`libuv --all-confirmed` 需要 241.92 秒，`/bin/ls` CTest smoke 也涨到 59.95 秒。后续 Bench2 集成测试要把 discovery coverage 测试和 full lowering 测试拆开，不适合继续用旧的三目标 smoke 断言所有 unresolved indirect flow 都为 0。

当前 `libuv --all-confirmed` 跳过 23 个函数，原因主要是 P-Code lowering 还不支持 `INT_2COMP`，另有一个 `INT_NEGATE`。这是 lowering 覆盖问题，不是 discovery 函数数量问题。

## 评分

- 实现效果：8/10。验证了根因，confirmed function 覆盖明显改善。
- 复杂度：7/10。代码更简单，但运行成本大幅上升，后续测试策略要调整。
- 维护成本：6/10。逻辑更符合真实 module-wide discovery，但需要后续做性能控制和 debug-info oracle 测试。

更好的后续方案：保留全量 discovery 作为正确默认，再增加脚本级小样本/调试信息覆盖测试；如果性能不可接受，再加显式 CLI 参数控制 coverage / smoke 模式，而不是在 analyzer 内硬编码 10/20。
