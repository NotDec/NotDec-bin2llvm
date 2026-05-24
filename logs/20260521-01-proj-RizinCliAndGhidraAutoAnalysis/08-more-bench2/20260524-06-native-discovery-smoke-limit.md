# Native Discovery Smoke Limit

## 原始 prompt

修复

## 背景

去掉 `SleighSeedInstructionAnalyzer` 的 10 / 20 隐藏限制后，native discovery 默认变成全量，`libuv` 能确认 485 个函数，正确性明显提升。

但 CTest 里的 `notdec.native_discover.x86_64_smoke` 也跟着跑全量 `/bin/ls` discovery，耗时约 59 秒。这个不适合作为快速 smoke。之前的 follow-up 清单里已记录要拆分 smoke / coverage 模式。

## 目标

保留默认全量 discovery，不把 correctness 路线重新截断；同时给 smoke / CTest 一个显式限量参数。

## 实现

修改 `include/notdec-bin2llvm/NativeAnalysis.h`：

- 增加 `NativeSleighDecodeOptions`。
- `createSleighSeedInstructionAnalyzer(...)` 接收可选 options。

修改 `lib/NativeAnalysis.cpp`：

- `SleighSeedInstructionAnalyzer` 保存 `NativeSleighDecodeOptions`。
- 默认不限制 decode seed 数。
- 如果 `MaxDecodedSeeds` 有值，decode 到指定数量后停止，并写 note：

```text
sleigh instruction decode stopped after explicit seed limit N
```

修改 `tools/notdec-native-discover.cpp`：

- 增加全局 CLI 参数：

```text
--decode-seed-limit <count>
```

- 该参数可以和普通模式或 JSON 查询模式一起使用。
- 默认不传时仍然全量。

修改 `tools/CMakeLists.txt`：

- `notdec.native_discover.x86_64_smoke` 改为：

```bash
notdec-native-discover --decode-seed-limit 20 /bin/ls
```

## 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover -j2
```

快速限制模式：

```bash
/usr/bin/time -f 'TIME %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --decode-seed-limit 20 --summary-json /bin/ls \
  > /tmp/notdec-discover-ls-limited.summary.json
```

结果：

```text
TIME 4.03
seeds=176 confirmed=20 blocks=41 instr=136
```

限制 note：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --decode-seed-limit 20 --notes-json /bin/ls
```

结果：

```json
{
  "notes": ["sleigh instruction decode stopped after explicit seed limit 20"],
  "count": 1
}
```

默认全量模式：

```bash
/usr/bin/time -f 'TIME %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-discover \
  --summary-json /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/php/20230831/calendar.so \
  > /tmp/notdec-calendar-full.summary.json
```

结果：

```text
TIME 15.94
seeds=52 confirmed=52 blocks=127 instr=543
```

CTest：

```bash
ctest --test-dir /tmp/notdec-bin2llvm-build \
  -R 'notdec.native_discover.x86_64_smoke|notdec.native_llvm.x86_64_smoke' \
  --output-on-failure
```

结果：

```text
notdec.native_discover.x86_64_smoke Passed 3.85 sec
notdec.native_llvm.x86_64_smoke     Passed 0.57 sec
Total Test time                     4.43 sec
```

## 结论

smoke / coverage 已拆开：

- 默认 discovery 仍然全量，适合 correctness / coverage。
- CTest smoke 显式使用 `--decode-seed-limit 20`，恢复到几秒级。

这个改动没有重新引入隐藏的 10 / 20 限制；限制只在调用方明确传参数时生效。

## 性能

`/bin/ls` discovery smoke 从约 59 秒降到 3.85 秒。Bench2 全量目标默认不受影响。

## 评分

- 实现效果：9/10。解决了 CTest 太慢，同时保留全量默认。
- 复杂度：7/10。多了一个 options 结构和 CLI 参数，但路径清楚。
- 维护成本：7/10。后续可以把同一 options 接到 native-llvm，避免单函数模式也被全量 discovery 拖慢。
