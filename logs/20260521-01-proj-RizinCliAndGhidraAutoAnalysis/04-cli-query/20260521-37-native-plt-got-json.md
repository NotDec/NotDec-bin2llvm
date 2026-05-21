# Native PLT.GOT Mapping

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

native discovery 已经能导出 `.plt.sec` 的 stub -> GOT -> symbol 映射，并把这些信息用于
xref 分类和 `--all-confirmed` lowering。Bench2 里 libuv 已经能把 `0x9350` lower 成
`pthread_key_delete`。

但 libuv 还有一个遗漏：`0x8dc0` 位于 `.plt.got`，这是 `__cxa_finalize` 的跳板，不在当前
`.plt-json` 结果里，所以 direct call 到它还会保留成 helper。

这次只补 `.plt.got` 映射，让 `--plt-json` 和后续 xref / lowering 也能识别这类 thunk。

## Ghidra 相关实现

Ghidra / heritage 路线会把外部符号当成模块符号的一部分，而不是只看一类 PLT：

- `ghidra_scripts/ExportHeritageModule.java::writeExternals(...)`
  - 导出外部函数信息。
- `ghidra_scripts/ExportHeritageModule.java::directCallTargetName(...)`
  - direct call 目标如果能解析到符号名，就写入导出结果。
- `lib/HeritageToLLVM.cpp::planModuleSymbols(...)`
  - 统一规划内部函数和外部函数符号。

native 侧需要跟上的是：只要 ELF 里能稳定找出可调用的外部跳板，就应该进同一张映射表，
而不是只覆盖 `.plt.sec`。

## native 侧复刻策略

1. `RelocationPltAnalyzer` 继续处理 `.plt.sec`，同时补 `.plt.got`。
2. 只接受 `R_X86_64_GLOB_DAT` 且符号类型是 `FUNC` 的 relocation，避免把数据对象误收进去。
3. 根据 `.plt.got` section 起始地址和 16 字节步长，为这些 function 级 relocation 规划 stub 地址。
4. 让 `notdec-native-discover --plt-json` 同时导出 `.plt.sec` 和 `.plt.got` 映射。

暂时不做：

- 不改 xref JSON 格式。
- 不扩展到非 x86-64。
- 不碰参数/返回值签名。

## 判断标准

1. libuv 的 `0x8dc0` 能在 `--plt-json` 里出现，并对应 `__cxa_finalize`。
2. `notdec-native-llvm --all-confirmed` 能把该 call lower 成外部 LLVM call。
3. `.plt.sec` 现有输出不受影响。

## 风险

1. `.plt.got` 里并不是所有 relocation 都是函数，必须过滤 symbol type。
2. 16 字节步长和 relocation 数量必须对齐，否则不能硬猜。
3. 如果后续别的样本没有 `.plt.got`，这条逻辑不应影响现有 `.plt.sec` 路径。

## 实现记录

### 修改文件和函数

1. `lib/NativeAnalysis.cpp:258`
   - 修改 `RelocationPltAnalyzer::run(...)`。
   - 扫 relocation 时额外收集 `R_X86_64_GLOB_DAT` 且 symbol type 为 `FUNC` 的外部函数 relocation。
   - 在原有 `.plt.sec` / legacy `.plt` 映射前调用 `addPltGotEntries(...)`。
2. `lib/NativeAnalysis.cpp:387`
   - 新增 `RelocationPltAnalyzer::addPltGotEntries(...)`。
   - 查找 `.plt.got` section，要求 section size 能按 16 字节整除。
   - 要求 `.plt.got` slot 数量和函数型 `GLOB_DAT` relocation 数量一致；不一致时只记 note，不硬猜。
   - 按 relocation 地址排序后，把 `plt.got.address + index * 16` 记录成 `NativePltEntry::StubAddress`。
3. `scripts/bench2-native-smoke.sh:89`
   - libuv IR pattern 增加 `call void @__cxa_finalize()`，覆盖 `.plt.got` external call。
4. `ARCHITECTURE.md:63`
   - 记录 `RelocationPltAnalyzer` 会把 `.plt.got` 函数型 `GLOB_DAT` thunk 统一纳入 PLT external 映射。
5. `ARCHITECTURE.md:110`
   - 记录 `--plt-json` 输出包含 `.plt.sec` / `.plt.got`。
6. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:24`
   - 更新 Stage 4 和 Stage 7 完成项。

### 验证命令

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm notdec-native-discover -j2
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --plt-json \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 \
  | python3 -m json.tool > /tmp/notdec-libuv-plt-20260521-37.pretty.json
rg -n '"stub": "0x8dc0"|"symbol": "__cxa_finalize"|"stub": "0x9350"|"symbol": "pthread_key_delete"|"count"' \
  /tmp/notdec-libuv-plt-20260521-37.pretty.json
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 \
  --all-confirmed -o /tmp/notdec-libuv-20260521-37.ll
rg -n "__cxa_finalize|pthread_key_delete|notdec_pcode_CALL_void|call void @notdec_native" \
  /tmp/notdec-libuv-20260521-37.ll
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-37
```

### 关键结果

`--plt-json` 现在包含 `.plt.got` 条目：

```text
stub=0x8dc0 symbol=__cxa_finalize
stub=0x9350 symbol=pthread_key_delete
count=218
```

libuv IR 里 `0x8dc0` 已经从 helper call 变成外部函数调用：

```text
call void @__cxa_finalize()
call void @notdec_native_9d80()
declare void @__cxa_finalize()
call void @pthread_key_delete()
declare void @pthread_key_delete()
```

`rg` 没有再找到 `notdec_pcode_CALL_void`。

### Bench2 结果

三个目标都通过 `notdec-native-discover --summary-json`、`notdec-native-llvm --all-confirmed`、
LLVM 22 `llvm-as`、LLVM 22 `opt -passes=verify`，并通过 smoke 里的 IR pattern 检查。

```text
vsftpd ok elapsed=8s
libuv ok elapsed=7s
memcached ok elapsed=8s
```

summary 关键数据：

```text
vsftpd: confirmed_functions=9, basic_blocks=30, instructions=80, xrefs.total=297, unresolved_indirect_flows=7
libuv: confirmed_functions=9, basic_blocks=29, instructions=85, xrefs.total=24, unresolved_indirect_flows=2
memcached: confirmed_functions=9, basic_blocks=30, instructions=80, xrefs.total=179, unresolved_indirect_flows=7
```

所有 `.stdout` / `.stderr` 日志大小都是 0。

### 性能和影响

这次只在 relocation 扫描时多收集函数型 `GLOB_DAT`，再按 `.plt.got` slot 建一小张表。
Bench2 三目标 smoke 总耗时约 23 秒，和上一轮同口径接近。

实现效果：4/5。libuv 里当前剩余的 direct PLT helper call 已消失。
复杂度：2/5。逻辑集中在 `RelocationPltAnalyzer`，但依赖 ELF x86-64 `.plt.got` 的 16 字节布局。
维护成本：2/5。后续如果支持更多架构，需要按架构拆分 thunk 匹配规则。
