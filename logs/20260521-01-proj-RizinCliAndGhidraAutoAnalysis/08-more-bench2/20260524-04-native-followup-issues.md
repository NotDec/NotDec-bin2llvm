# Native Follow-up Issues

## 原始 prompt

还有什么值得修复的问题吗

把这几个问题写到一个logs文件

## 背景

最近两次 native 修改后：

- `SleighSeedInstructionAnalyzer` 已经不再只 decode 前 10 / 20 个 seed。
- `libuv` native discovery 已经能确认 485 个函数。
- native raw Sleigh opcode 表已按 Ghidra `OpCode.java` 补齐。
- `INT_2COMP` / `INT_NEGATE` 已经修复。
- `libuv --all-confirmed` 从 462 个 LLVM function 提升到 484 个，并能通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。

还剩几个值得优先修的问题。

## 1. `CBRANCH` 非 direct ram target（已修复）

现象：

```text
skip native function 0x9c38: CBRANCH target must be a direct ram address
```

这是当前 `libuv` 只差 1 个函数的直接原因。现在 `libuv` discovery confirmed 是 485，但 `--all-confirmed` 最终输出 484 个 LLVM function。

当前判断：

- 这不是 opcode 表缺项。
- 更像是 raw Sleigh P-Code 中条件跳转目标不是直接 `ram` varnode，可能是 `unique` / register 临时值保存了目标。
- 需要看 `0x9c38` 的 raw P-Code，确认是否能通过常量传播或 source tracking 解析出 direct target。

建议路线：

1. 用 `notdec-native-pcode` 或 `notdec-native-discover --cfg-json` 查 `0x9c38` 附近 P-Code。
2. 如果 target 是前序 `COPY const/ram -> unique`，在 native lowering 的 terminator 路径补一个简单 target resolver。
3. 如果是真正 indirect conditional branch，先记录 unresolved，不要乱连 CFG。

判断标准：

- `libuv --all-confirmed` 输出 485 个 LLVM function。
- `llvm-as` 和 `opt -passes=verify` 通过。
- 不引入新的 `notdec_exit` 或错误 CFG 边。

实现记录：

- 见 `20260524-05-native-relative-branch-target.md`。
- 原因是 raw Sleigh P-Code 里存在 `CBRANCH (const,0x4,4)` 这种相对 P-Code op target。
- 已在 native lowering 里支持 `BRANCH` / `CBRANCH` 的 const relative target。
- 验证结果：`libuv --all-confirmed` 输出 485 个 LLVM function，并通过 LLVM 22 `llvm-as` 和 `opt -passes=verify`。

## 2. 全量 discovery / lowering 太慢（smoke 已拆分）

现象：

```text
libuv discovery: 141.87s
libuv --all-confirmed: 241.92s / 247.19s
/bin/ls CTest smoke: 59s 左右
```

当前判断：

- 去掉 seed 数量限制后，正确性明显变好，但现有 CTest smoke 已经太慢。
- 不能把慢的全量 coverage 跑法和快速 smoke 混在一起。

建议路线：

1. 保留全量 discovery 作为 correctness/coverage 路线。
2. 增加显式 smoke 模式或脚本参数，只跑小样本，给 CTest 用。
3. Bench2 coverage 用单独脚本跑，不放进每次快速 CTest。

判断标准：

- CTest smoke 回到秒级或十几秒级。
- Bench2 coverage 脚本仍能跑全量并记录 confirmed/function 覆盖。
- 不再用隐藏常量把默认 correctness 路线截断。

实现记录：

- 见 `20260524-06-native-discovery-smoke-limit.md`。
- 已给 `notdec-native-discover` 增加显式参数 `--decode-seed-limit <count>`。
- 默认 discovery 仍然全量。
- CTest `notdec.native_discover.x86_64_smoke` 改用 `--decode-seed-limit 20`。
- 验证结果：discover smoke 3.85 秒，native LLVM smoke 0.57 秒，总 CTest 4.43 秒。

剩余问题：

- `notdec-native-llvm -f` / `--all-confirmed` 仍会全量跑 discovery。
- Bench2 coverage 仍需要单独脚本和 debug-info oracle。

## 3. 用 debug info 做 discovery oracle

现状：

Bench2 manifest 已经有 `debug_path`：

- 普通目标：`/usr/lib/debug/.build-id/...debug`
- Python debug 目标：`EMBEDDED_DEBUG`

建议新增脚本：

```text
scripts/bench2-native-discovery-debug-check.py
```

脚本逻辑：

1. 读 `Bench2/manifest/benchmark-targets.tsv`。
2. 找目标 ELF 和 debug 文件。
3. 用 LLVM 22 `llvm-dwarfdump --debug-info` 解析 concrete `DW_TAG_subprogram`。
4. 只统计有 `DW_AT_low_pc` / 有效 `DW_AT_high_pc` / 落在 executable range 的函数。
5. 对比：
   - debug concrete function 数
   - native seed 覆盖率
   - native confirmed 覆盖率
   - native 多出来的样本
   - debug 有但 native 没有的样本

判断标准：

- 能清楚区分“入口 seed 没发现”和“seed 已有但 decode 没 confirmed”。
- 对 libuv、vsftpd、memcached 先形成 baseline。
- 后续 native discovery 改动能用 debug oracle 量化收益和误报。

## 4. `--all-confirmed` 重复跑 discovery

现象：

当前 Bench2 native rerun 脚本会先跑：

```text
notdec-native-discover --summary-json
```

然后又跑：

```text
notdec-native-llvm --all-confirmed
```

而 `notdec-native-llvm --all-confirmed` 内部会重新跑同一套 native discovery。

当前判断：

- 在 bounded discovery 时问题不明显。
- 全量 discovery 后，这会让每个目标多付出一次几十秒到几分钟的成本。

建议路线：

短期：

- 脚本层只在需要 summary 时跑 discover，不把它当成 native-llvm 的前置必跑。

中期：

- 让 `notdec-native-llvm` 支持读取 discovery cache/json，或者把 discovery state 序列化成 native-llvm 可复用的输入。

判断标准：

- selected-targets-native 重跑时间明显下降。
- 输出 IR 和 summary 仍能对应同一份 discovery 事实。

## 5. helper opcode 不是精确语义

现状：

native opcode 表已经补齐，但下面这些 opcode 当前走 helper：

- `FLOAT_*`
- `SEGMENTOP`
- `CPOOLREF`
- `NEW`
- `INSERT`
- `EXTRACT`

当前判断：

- 这能避免函数因为 opcode 表缺项被跳过。
- 但 helper 只是保留调用形状，不是完整语义。
- 特别是浮点和 bit-field 类 op，后续如果 Bench2 真实函数依赖这些结果，语义会不准。

建议路线：

1. 统计 Bench2 全量 native lowering 中 helper opcode 出现频率。
2. 按出现频率和语义风险逐个补。
3. 对浮点 op 可以参考 `lib/HeritageToLLVM.cpp` 已有实现。
4. 对 `INSERT` / `EXTRACT` 需要确认 bit offset / size 语义后再写。

判断标准：

- helper opcode 数量逐步下降。
- 每补一个 opcode，都有对应 Bench2 样本验证。
- 不为了“覆盖完整”写不确定语义。

## 建议优先级

1. 补 debug-info oracle 脚本，用它衡量 native discovery。
2. 处理 `--all-confirmed` 重复 discovery。
3. 最后按 Bench2 实际出现频率逐个精确化 helper opcode。
