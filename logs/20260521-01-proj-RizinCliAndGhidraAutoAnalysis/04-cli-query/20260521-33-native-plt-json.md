# Native PLT JSON

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

native 侧已经有 `NativePltEntry`，`RelocationPltAnalyzer` 会把 PLT stub、GOT slot、外部符号名连起来。
但这些信息目前只在文本 report 里展示，JSON 查询没有入口。后续要把 direct call 到 PLT stub 当成外部调用，
先需要稳定的机器可读输出。

这次只补 `notdec-native-discover --plt-json`。

## Ghidra / rizin 相关实现

Ghidra 侧：

- `/sn640/ghidra/Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/listing/FunctionManager.java`
  - `getExternalFunctions()` 能枚举外部函数。
- `ghidra_scripts/ExportHeritageModule.java`
  - `writeExternals(...)` 把外部函数写到模块 JSON。
  - `rememberExternal(...)` 在 direct call 目标是 external 或不在本次内部函数集合时记录外部函数。
- `ghidra_scripts/ExportHeritagePcode.java`
  - `directCallTargetName(...)` 会用 `FunctionManager.getFunctionAt(...)` 找 direct call 目标名字。

rizin 侧常见做法是用 JSON 命令查看导入和重定位，例如 import / relocation 类查询输出地址和符号名。
native 这里先不复刻完整 rizin schema，只给当前分析状态里的 PLT 映射一个稳定 JSON。

## native 侧复刻策略

1. `notdec-native-discover` 增加 `--plt-json <elf-file>`。
2. 输出字段：
   - `stub`: PLT stub 地址。
   - `got`: GOT slot 地址。
   - `symbol`: 外部符号名。
3. 保持当前 analyzer 不变，不新增语义判断。

暂时不做：

- 不把 direct call 到 PLT 改成外部 call。
- 不输出完整 relocation 表。
- 不对符号做 demangle。

## 判断标准

1. 三个 Bench2 目标的 `--plt-json` 能输出合法 JSON。
2. `count` 和文本 report 里的 PLT external symbols 数一致。
3. Bench2 smoke 继续通过。

## 风险

1. PLT 匹配仍依赖当前 `.plt.sec` / `.plt` 的 16 字节 stub 假设。
2. 这只是可观测性，不直接提升 IR 语义；下一步才适合改 call lowering / xref 分类。

## 实现记录

### 修改文件和函数

1. `tools/notdec-native-discover.cpp:19`
   - `OutputMode` 新增 `PltJson`。
   - `printUsage(...)` 增加 `--plt-json <elf-file>`。
   - `parseArgs(...)` 识别 `--plt-json`。
2. `tools/notdec-native-discover.cpp:361`
   - 新增 `printPltJson(...)`。
   - 输出 `stub`、`got`、`symbol` 和总 `count`。
3. `tools/notdec-native-discover.cpp:410`
   - `main(...)` 根据 `OutputMode::PltJson` 调用 `printPltJson(...)`。
4. `ARCHITECTURE.md:108`
   - 记录 `--plt-json` 查询入口。
5. `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md:24`
   - 更新 Stage 4 完成项。

### 验证命令

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm notdec-native-discover -j2
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --plt-json /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd > /tmp/notdec-vsftpd-plt.json
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --plt-json /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0 > /tmp/notdec-libuv-plt.json
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --plt-json /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached > /tmp/notdec-memcached-plt.json
python3 -m json.tool /tmp/notdec-vsftpd-plt.json >/tmp/notdec-vsftpd-plt.pretty.json
python3 -m json.tool /tmp/notdec-libuv-plt.json >/tmp/notdec-libuv-plt.pretty.json
python3 -m json.tool /tmp/notdec-memcached-plt.json >/tmp/notdec-memcached-plt.pretty.json
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-33
```

### PLT JSON 结果

```text
vsftpd: count=183, first stub 0x5bb0 -> SSL_CTX_use_PrivateKey_file
libuv: count=215, first stub 0x8dd0 -> getenv
memcached: count=181, first stub 0x5b90 -> __printf_chk
```

三份输出都能被 `python3 -m json.tool` 解析。

### Bench2 结果

三个目标都通过 `notdec-native-discover --summary-json`、`notdec-native-llvm --all-confirmed`、
LLVM 22 `llvm-as`、LLVM 22 `opt -passes=verify`。

```text
vsftpd ok elapsed=7s
libuv ok elapsed=9s
memcached ok elapsed=7s
```

summary 关键数据：

```text
vsftpd: confirmed_functions=9, basic_blocks=30, instructions=80, xrefs.total=297, data=149, string=139
libuv: confirmed_functions=10, basic_blocks=32, instructions=93, xrefs.total=24, data=13, string=0
memcached: confirmed_functions=9, basic_blocks=30, instructions=80, xrefs.total=179, data=68, string=102
```

和上一轮同口径 summary diff：无差异。

所有 `.stdout` / `.stderr` 日志大小都是 0。

### 性能和影响

这次只加 CLI formatter，不改变 analyzer 和 lowering。Bench2 三目标 smoke 总耗时约 23 秒，
和上一轮同口径接近。

实现效果：4/5。PLT 外部符号映射现在能机器读取。
复杂度：1/5。只是一个输出模式。
维护成本：1/5。后续如果扩展 relocation JSON，可以复用同样 formatter 风格。
