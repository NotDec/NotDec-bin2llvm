# 本次用户原始 prompt

继续按照规划完善 bin2llvm native 链路。当前 direct call 目标已经能进入 function seed / worklist；下一小步在受控范围内消费这些 direct call 目标，开始形成最小递归 decode。

## 背景

Ghidra 侧相关实现：

- `AutoAnalysisManager.java::schedule(...)` 会把新的函数入口调度给 `CreateFunctionCmd`，不是只记录引用。
- `CreateFunctionCmd.java::applyTo(...)` 会遍历待创建函数入口，并调用 `createFunction(...)`。
- `CreateFunctionCmd.java::getFunctionBody(...)` 用 `FollowFlow` 继续跟随函数内 flow，直到遇到不该跟随的 call / indirect flow。
- `FollowFlow.java` 内部维护待处理地址集合，按 flow 逐步扩展，而不是只处理入口第一段。

native 侧现在已经有 `NativeFunctionWorkItem` 和 direct call seed，但新增 seed 还不会在同一轮被 decode。因此目前只能说“发现了入口”，还不能说开始递归。

## 目标

本小步只做受控递归：

1. 初始仍只取 worklist 前 8 个 seed。
2. direct call 目标进入本地 decode 队列，并写入 `sleigh-direct-call` seed。
3. 本轮最多 decode 16 个 seed，避免全程序遍历。
4. 已 decode 或已入队的地址不重复处理。
5. 不递归 branch successor，不处理 indirect call / jump。

## 技术路线

- 把当前 `run(...)` 里的 snapshot + pending list 改成本地 `std::deque<uint64_t>`。
- 初始化时只把 worklist 前 8 个 executable seed 入队。
- `decodeSeed(...)` 返回 direct call targets。
- 每个 direct call target 先 `addFunctionSeed(...)`，再在本地队列中去重入队。
- `MaxSeeds` 表示本轮总 decode 上限，不再只是初始 seed 数；新增 `MaxInitialSeeds` 控制初始队列大小。

## 风险

- direct call 的目标可能已经有 `.eh_frame` 或 symbol seed，`addFunctionSeed(...)` 只会补 source，不一定增加总 seed 数。
- 递归后 confirmed function 数可能只在 `libuv.so.1.0.0` 这类前缀有 direct call 的样例中增加。
- 仍然是 bounded smoke，不代表完整 CFG。

## 判断标准

- Bench2 三个 smoke 能跑通。
- `libuv.so.1.0.0` 中 `sleigh-direct-call: 1` 保持存在。
- 如果 direct call target 未在初始 8 个 seed 中，`libuv.so.1.0.0` 的 confirmed functions / instructions 应该增加。
- 时间仍在小范围内。

## 实现记录

改动文件和函数：

- `lib/NativeAnalysis.cpp:1145`，`SleighSeedInstructionAnalyzer::run(...)`：把 worklist snapshot 改成本地 `std::deque<uint64_t>` decode 队列；用 `queuedSeeds` 和 `decodedSeedAddresses` 去重；direct call target 写入 `sleigh-direct-call` seed 后立即尝试入队。
- `lib/NativeAnalysis.cpp:1184`，新增 `MaxInitialSeeds = 8`，`MaxSeeds` 改为 16。前者限制初始 seed，后者限制本轮总 decode。
- `lib/NativeAnalysis.cpp:1230`，新增 `enqueueInitialSeeds(...)`：只从 `functionWorklist()` 取前 8 个可执行入口。
- `lib/NativeAnalysis.cpp:1244`，新增 `enqueueSeed(...)`：过滤非可执行地址，并避免重复入队。
- `ARCHITECTURE.md:41`、`ARCHITECTURE.md:52`：更新 native Sleigh seed decode 的行为说明。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:17`：记录本小步已完成。

实现效果：

- direct call 目标不再只留到下一轮或后续 analyzer 使用。本 analyzer 同轮可消费这些目标。
- 仍然不沿 branch successor 递归，不处理 indirect call / jump。
- 队列上限较小，避免把当前线性 decode 误扩成全程序遍历。

验证命令：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/usr/bin/time -f 'TIME %e' /tmp/notdec-bin2llvm-build/bin/notdec-native-discover /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
```

验证结果：

- build：通过。
- `vsftpd`：function seeds 186，confirmed functions 8，basic blocks 18，instructions 55，xrefs 5，TIME 1.59。
- `libuv.so.1.0.0`：function seeds 485，`sleigh-direct-call: 1`，confirmed functions 9，basic blocks 19，instructions 68，xrefs 7，TIME 1.88。
- `memcached`：function seeds 258，confirmed functions 8，basic blocks 18，instructions 55，xrefs 5，TIME 1.64。

性能和效果判断：

- 相比上一轮，`vsftpd` 和 `memcached` 数量基本不变，时间仍在同一档。
- `libuv.so.1.0.0` confirmed functions 从 8 增到 9，instructions 从 60 增到 68，说明 direct call seed 已被同轮消费。
- `libuv.so.1.0.0` 时间从约 1.62s 到 1.88s，有小幅增加，符合多 decode 一个 seed 的预期。

评分：

- 实现效果：8/10。已打通最小 direct call 递归，但还不是完整 CFG。
- 理解成本：7/10。新增本地队列和两个小 helper，逻辑直接。
- 维护成本：7/10。上限写死，后续要结合真实 CFG stop rule 再扩大。

更好的方案：

- 后续应把 decode 队列和 CFG stop rule 合并，沿 Ghidra `FollowFlow` 的语义处理 branch successor。
- 现在不做，是因为当前阶段只验证 direct call seed 的受控消费。
