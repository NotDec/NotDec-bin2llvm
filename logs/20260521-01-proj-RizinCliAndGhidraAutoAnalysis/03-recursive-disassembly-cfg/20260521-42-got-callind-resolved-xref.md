# GOT CALLIND Resolved Xref

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

native lowering 已经能把 `_init` 里的 guarded GOT indirect call 降成外部符号调用：

```text
movq GOT(%rip), %rax
testq %rax, %rax
je ...
callq *%rax
```

但 native discovery 仍把同一条 `CALLIND` 记为 unresolved indirect call：

- `vsftpd`：`0x5014`
- `libuv`：`0x8014`
- `memcached`：`0x5014`

这会让 unresolved 统计保守偏高。当前小步只处理这种已经由 relocation 明确指向外部 `GLOB_DAT`
符号的模式，不处理普通函数指针。

## Ghidra 相关实现

Ghidra 不是只看 P-Code opcode 就把 computed call 全部归为未解析：

- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/ExternalSymbolResolverAnalyzer.java`
  - 负责把外部符号和引用关系补到程序里。
- `Ghidra/Features/Base/src/main/java/ghidra/app/plugin/core/analysis/FunctionStartAnalyzer.java`
  - 分析函数入口和调用引用时会使用已有 symbol / reference 信息。
- `Ghidra/Framework/SoftwareModeling/src/main/java/ghidra/program/model/symbol/ReferenceManager.java`
  - 保存从指令地址到目标地址或外部位置的引用。
- `Ghidra/Processors/x86/data/languages/ia.sinc`
  - x86 间接调用仍会生成 `CALLIND` P-Code，真实目标需要结合寄存器来源、relocation 和外部符号表判断。

native 侧不复刻完整 ExternalManager。现在已有 ELF relocation、PLT/GOT 映射和 P-Code varnode，
可以先复刻当前用例需要的判断。

## native 侧复刻策略

1. 在 `SleighSeedInstructionAnalyzer::addDirectControlFlow(...)` 里做一个很小的 P-Code 来源追踪。
2. `COPY ram,<unique/register>` 时记录输出 varnode 来源于哪个 `ram` 地址。
3. 遇到 `CALLIND` 时，如果输入 varnode 能追到 `ram` 地址，并且该地址是带符号名的外部
   `X86_64_GLOB_DAT` relocation，就记录一个 call xref。
4. 这种已解析 call 不再写入 `NativeUnresolvedFlow`。
5. 其他 `CALLIND` 保持原样，继续 unresolved。

暂时不做：

- 不解析普通函数指针。
- 不解析 indirect branch / jump table。
- 不把外部符号建成完整函数节点。
- 不改变 lowering 规则。

## 判断标准

1. 三个 Bench2 目标 `_init` 的 indirect call 从 unresolved 中消失。
2. unresolved indirect branch 数不因为这次改动变化。
3. xref total 增加对应的 external indirect call xref。
4. Bench2 smoke 继续通过 LLVM 22 verify。
5. 运行时间不明显变慢。

## 风险

1. 只追踪简单 `COPY`，覆盖面窄，但不容易误判。
2. `GLOB_DAT` 外部符号可能是数据符号；这次还要求它真实出现在 `CALLIND` 输入来源里，
   不单靠 relocation 类型判断。
3. 这是 discovery 侧修正统计和 xref，不表示完整函数指针分析已经完成。

## 实现记录

### 修改文件和函数

- `lib/NativeAnalysis.cpp`
  - 第 1496 行 `SleighSeedInstructionAnalyzer::addDirectControlFlow(...)`：增加局部 `sourceRamByVarnode`，在同一段 P-Code 内追踪 `LOAD` / `COPY` 的 RAM 来源。
  - 第 1520 行 `CALLIND` 分支：如果输入能追到外部 `X86_64_GLOB_DAT` GOT slot，记录 `sleigh-pcode-got-indirect-call` call xref，并跳过 unresolved 记录。
  - 第 1564 行 `trackCopySourceRam(...)`：传播 `COPY` 的 RAM 来源。
  - 第 1579 行 `trackLoadSourceRam(...)`：传播 `LOAD` 的 RAM 地址来源。
  - 第 1594 行 `callIndGotSource(...)`、第 1603 行 `sourceRam(...)`、第 1616 行 `varnodeStorageKey(...)`：提供窄来源查询。
  - 第 1623 行 `isExternalGlobDatSymbolAt(...)`：确认 GOT 地址来自带符号名的外部 `X86_64_GLOB_DAT` relocation。
- `scripts/bench2-native-smoke.sh`
  - 第 67 行 `require_no_unresolved_indirect_calls(...)`：新增 smoke 检查。
  - 第 177 行：summary JSON 产出后检查当前 Bench2 三目标不再保留 unresolved indirect call。
- `ARCHITECTURE.md`
  - 第 58 行：记录 discovery 侧 GOT `CALLIND` 来源追踪和 xref 行为。
  - 第 135 行：记录 Bench2 smoke 的 unresolved indirect call 检查。
- `logs/20260521-01-proj-RizinCliAndGhidraAutoAnalysis/PROGRESS.md`
  - 第 23 行：阶段 3 记录 GOT `CALLIND` xref 和 unresolved 修正。
  - 第 58 行：阶段 7 记录 smoke 检查。

实现时调整：

- 计划里原本想只接受 symbol type 为 function 的 relocation，但 Bench2 里的 `__gmon_start__`
  是 `NOTYPE WEAK UND`。这里改成接受带符号名的外部 `GLOB_DAT`，并要求该 slot 真实流入
  `CALLIND`，避免单靠 relocation 类型猜。

### 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
```

Bench2 smoke：

```bash
scripts/bench2-native-smoke.sh --out-dir /tmp/notdec-bin2llvm-bench2-smoke-20260521-42d
```

结果：

```text
vsftpd ok elapsed=7s
libuv ok elapsed=19s
memcached ok elapsed=7s
```

summary：

```text
vsftpd: confirmed=9 blocks=30 instr=80 xrefs=298 call=2 unresolved=6 indirect_call=0 indirect_branch=6
libuv: confirmed=9 blocks=29 instr=85 xrefs=25 call=3 unresolved=1 indirect_call=0 indirect_branch=1
memcached: confirmed=9 blocks=30 instr=80 xrefs=180 call=2 unresolved=6 indirect_call=0 indirect_branch=6
```

xref 查询：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-from-json 0x5014 /sn640/NotDec-Exp/Bench2/rootfs/usr/sbin/vsftpd
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-from-json 0x8014 /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libuv.so.1.0.0
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover --xrefs-from-json 0x5014 /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/memcached
```

结果：

```text
vsftpd 0x5014 -> 0x25ff0 call sleigh-pcode-got-indirect-call
libuv 0x8014 -> 0x33fd0 call sleigh-pcode-got-indirect-call
memcached 0x5014 -> 0x3eff0 call sleigh-pcode-got-indirect-call
```

IR 检查：

```bash
rg -n 'notdec_pcode_CALLIND_void|call void @__gmon_start__|__gmon_start__' /tmp/notdec-bin2llvm-bench2-smoke-20260521-42d/*.ll
```

结果：

- 没有 `notdec_pcode_CALLIND_void`。
- 三个目标仍都有 `call void @__gmon_start__()`。
- 所有 stdout / stderr 文件为空。

### 评分

- 实现效果：8/10。Bench2 当前三个 `_init` GOT indirect call 都不再是 unresolved，并有可查 xref。
- 理解成本：4/10。增加了局部来源追踪，但只处理 `LOAD` / `COPY`，范围清楚。
- 维护成本：3/10。后续如果要支持普通函数指针，需要把这个窄规则扩成更完整的数据流分析。
