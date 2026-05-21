# Native Unresolved Flow Query

## 原始 prompt

```text
在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis里面根据划分的几个关键宏观步骤，创建对应的文件夹，然后在prompt中要求，对应步骤的规划和修改必须放到对应的文件夹里。

本次目标是按照规划，完善bin2llvm项目反汇编转IR的native链路（即不走GhidraScript的链路，而是走libsla，libdecomp的这个链路），从Ghidra和rizin那边复刻足够多的功能。每当实现一部分功能后，就看看能否利用bench2里面这些项目去做初步的测试： （vsftpd可执行文件，libuv动态链接库，memcached可执行文件），测试实现是否正确。规划中每一部分都完成后即可停止。在logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis 下创建一个PROGRESS.md里追踪计划的实现。完成一个部分后，同步更新 项目顶层的ARCHITECTURE.md 反映实现的功能模块的架构。

约束：根据规划实现每个部分的时候，每次从中该部分中选择一小块功能实现，必须按照这样的规范：

- 先写markdown规划文件（放到对应子文件夹下），规划文件中首先要介绍 Ghidra 中是如何实现的相关功能的，明确写出源码文件和关键函数。
- 然后说明我们可以如何复刻这些策略，为了敏捷开发，可以跟着测试用例来，先复刻当前测试用例需要的部分，后续再完善其他部分。
- 然后开始真正的实现，完成后将过程记录到对应的规划文件中。
```

## 当前目标和已有 native 状态

native discovery 已经会记录 `CALLIND` / `BRANCHIND` 到 `NativeUnresolvedFlow`，summary 里也能统计
indirect call / indirect branch 数量。但 CLI 目前只把这些信息写进 summary，不提供单独查询。

现在 Bench2 三个样本里都还有 unresolved indirect flow：

- `vsftpd`: 1 个 indirect call, 6 个 indirect branch
- `libuv`: 1 个 indirect call, 1 个 indirect branch
- `memcached`: 1 个 indirect call, 6 个 indirect branch

这次只把这张表导成 JSON，方便后续按样本和地址分类，不改 lowering。

## Ghidra 相关实现

Ghidra / heritage 路线会把不确定跳转保留成显式对象，后续再归类：

- `ghidra_scripts/ExportHeritageModule.java::writeFailures(...)`
  - 导出失败和未解析项，避免静默丢失。
- `lib/HeritageToLLVM.cpp::lowerCall(...)`
  - 目标能解析时直接 lower，解析不了时保守保留。

native 侧这里的对应物不是强行猜目标，而是把 unresolved flow 单独暴露出来，给后续分类用。

## native 侧复刻策略

1. 在 `tools/notdec-native-discover.cpp` 增加 `--unresolved-json`。
2. 输出 `address`、`kind`、`source` 三个字段，和现有 xref JSON 风格一致。
3. 保留现有 summary，不改现有文本 report。

暂时不做：

- 不给 unresolved flow 猜目标。
- 不改 `NativeUnresolvedFlow` 数据结构。
- 不改 lowering。

## 判断标准

1. `--unresolved-json` 能输出三个 Bench2 目标的 unresolved flow 列表。
2. 现有 `--summary-json` 和文本 report 不变。
3. JSON 可被 `python3 -m json.tool` 解析。

## 风险

1. unresolved flow 里可能既有 call 也有 branch，字段必须和现有 `toString(...)` 保持一致。
2. 这个接口只是查询，不应影响 discovery 的去重和统计。

## 实现记录

### 修改文件和函数

1. `tools/notdec-native-discover.cpp:19`
   - 在 `OutputMode` 增加 `UnresolvedJson`。
2. `tools/notdec-native-discover.cpp:38`
   - 在 usage 里增加 `--unresolved-json <elf-file>`。
3. `tools/notdec-native-discover.cpp:64`
   - 在 `parseArgs(...)` 里解析 `--unresolved-json`。
4. `tools/notdec-native-discover.cpp:384`
   - 新增 `printUnresolvedJson(...)`，输出 `address`、`kind`、`source` 和 `count`。
5. `tools/notdec-native-discover.cpp:436`
   - 在 main 输出分支里接入 `printUnresolvedJson(...)`。
6. `ARCHITECTURE.md:110`
   - 记录 `--unresolved-json` 查询模式。
7. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:24`
   - 更新 Stage 4 完成项。

### 验证命令

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
for spec in \
  'vsftpd /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd' \
  'libuv /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0' \
  'memcached /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached'
do
  set -- $spec
  name=$1
  path=$2
  /tmp/notdec-bin2llvm-build/bin/notdec-native-discover --unresolved-json "$path" \
    | python3 -m json.tool > "/tmp/notdec-${name}-unresolved-20260521-39.json"
done
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-39
```

### Unresolved 查询结果

```text
vsftpd: count=7
libuv: count=2
memcached: count=7
```

样例输出：

```text
libuv:
  address=0x8014 kind=indirect call source=sleigh-pcode-indirect-flow
  address=0x9d9f kind=indirect branch source=sleigh-pcode-indirect-flow
```

### Bench2 结果

三个目标都通过原有 smoke。

```text
vsftpd ok elapsed=7s
libuv ok elapsed=19s
memcached ok elapsed=7s
```

summary 关键数据：

```text
vsftpd: confirmed_functions=9, basic_blocks=30, instructions=80, xrefs.total=297, unresolved_indirect_flows=7
libuv: confirmed_functions=9, basic_blocks=29, instructions=85, xrefs.total=24, unresolved_indirect_flows=2
memcached: confirmed_functions=9, basic_blocks=30, instructions=80, xrefs.total=179, unresolved_indirect_flows=7
```

所有 `.stdout` / `.stderr` 日志大小都是 0。

### 性能和影响

这次只新增 CLI 输出模式，不改 native discovery 状态，也不改 lowering。Bench2 smoke 耗时和上一轮接近。

实现效果：3/5。未解析间接控制流现在能被单独查询，方便后续继续归类。
复杂度：1/5。只增加一个 formatter 和一个 CLI mode。
维护成本：1/5。字段直接来自已有 `NativeUnresolvedFlow`。
