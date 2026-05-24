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

## 3. 用 debug info 做 discovery oracle（已添加脚本）

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

实现记录：

- 见 `20260524-07-native-debug-info-oracle.md`。
- 已新增 `scripts/bench2-native-discovery-debug-check.py`。
- 脚本读取 Bench2 manifest/debug_path，用 LLVM 22 `llvm-dwarfdump --debug-info` 提取 concrete `DW_TAG_subprogram` low_pc。
- 已用 `libuv`、`vsftpd`、`memcached` 的 `--decode-seed-limit 20` 跑通 sanity。
- 已用 `php:extension-calendar` 跑通默认全量 sanity。

剩余问题：

- 脚本当前分别跑 `--seeds-json` 和 `--functions-json`，默认全量时会重复 discovery。

## 4. `--all-confirmed` / 脚本重复跑 discovery（selected-targets 已优化）

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

debug oracle 脚本也有同类问题：初版分别跑 `--seeds-json` 和 `--functions-json`。

建议路线：

短期：

- 脚本层只在需要 summary 时跑 discover，不把它当成 native-llvm 的前置必跑。

中期：

- 让 `notdec-native-llvm` 支持读取 discovery cache/json，或者把 discovery state 序列化成 native-llvm 可复用的输入。

判断标准：

- selected-targets-native 重跑时间明显下降。
- 输出 IR 和 summary 仍能对应同一份 discovery 事实。

实现记录：

- 见 `20260524-08-native-discovery-combined-json.md`。
- 已给 `notdec-native-discover` 增加 `--discovery-json`，一次输出 seeds 和 confirmed functions。
- `scripts/bench2-native-discovery-debug-check.py` 已改成只跑一次 discovery。
- `php:extension-calendar` debug oracle 从 33.08 秒降到 16.39 秒。

剩余问题：

- `notdec-native-llvm --all-confirmed` 仍会跑 discovery，这是生成 confirmed module 的必要输入。
- selected-targets-native 脚本已改成用 `notdec-native-llvm --summary-json-out`，不再先跑一次 `notdec-native-discover --summary-json`。

## 5. helper opcode 不是精确语义

现状（已部分修复）：

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

实现记录：

- 见 `20260524-11-native-direct-call-fallback.md`。
  - 已修 direct `CALL` helper fallback：已知 direct target 但不是 confirmed/external symbol 时，生成 `notdec_native_<addr>` 声明调用。
- 见 `20260524-12-native-float32-64-lowering.md`。
  - 已修 4/8-byte `FLOAT_ADD` / `FLOAT_SUB` / `FLOAT_MULT` / `FLOAT_DIV` / compare / `FLOAT_NAN` / `INT2FLOAT` / `FLOAT2FLOAT` / `FLOAT_TRUNC`。
- 见 `20260524-13-native-x87-float-lowering.md`。
  - 已把 10-byte x87 float 映射到 LLVM `x86_fp80`。
  - `wrk --all-confirmed` 已无 `notdec_pcode_` helper，并通过 LLVM 22 `llvm-as` / `opt -passes=verify`。

剩余问题：

- selected-targets-native 需要用新版本全量重跑后再确认是否还有 `CALLIND`、`INSERT`、`EXTRACT`、`SEGMENTOP` 等 helper。
- 如果仍有 `CALLIND`，需要先看具体样本来源，不能直接猜目标。

## 6. 函数范围 / CFG 可能过大

现象：

- `libuv` 里曾看到函数 `0x9c38` 的范围被扩到 `0x9c38..0x2375f`。
- 单函数 lowering 可以到 282 秒左右。
- 这类函数现在能过 `llvm-as` / `opt -passes=verify`，但耗时明显不正常。

当前判断：

- 这不是 LLVM IR 语法问题，而是 native discovery / CFG 边界问题。
- 可能和 block 合并、branch successor、fallthrough 或 eh-frame seed range 有关。
- 如果函数边界被扩得过大，后续即使验证通过，也可能把多个真实函数混进一个 LLVM function，语义和性能都会出问题。

建议路线：

1. 用 `notdec-native-discover --function-json 0x9c38` 和 `--cfg-json 0x9c38` 先固定证据。
2. 检查 `lib/NativeAnalysis.cpp` 里 `addDecodedFunctionBlocks` / `addBasicBlock` / successor 处理。
3. 对比 debug-info oracle 中同地址附近的函数边界，确认 native 是否越过了 debug 函数范围。
4. 优先修明显错误的边界扩张，不为了追求覆盖率强行把不确定边都纳入同一个函数。

判断标准：

- 异常大函数的 block/range 明显收敛。
- 单函数 lowering 时间下降。
- `libuv --all-confirmed` 仍能输出 485 个 LLVM function，并通过 LLVM 22 `llvm-as` / `opt -passes=verify`。
- debug-info oracle 的 seed/confirmed 覆盖率不明显下降。

## 建议优先级

1. 用新版本重跑 selected-targets-native，重新统计 `notdec_pcode_` helper 和失败目标。
2. 如果还有 `CALLIND` helper，优先按真实 GOT/source tracking 样本修。
3. 如果还有 `INSERT` / `EXTRACT` / `SEGMENTOP` 等特殊 opcode，再按样本补精确语义。
