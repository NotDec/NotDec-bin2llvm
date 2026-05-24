# Native Sparse Block Lowering

## 原始 prompt

修复

## 背景

`libuv` 的 native discovery 已经能确认 `0x9c38`，但这个函数有远端 cold block：

- entry block: `0x9c38..0x9c4e`
- cold blocks: `0x2363f..0x2375f`

旧的 native lowering 用 `RangeEnd - Entry` 连续 lift，所以会把 `0x9c38..0x2375f` 中间大量无关指令都放进这个函数。`--function-json 0x9c38` 之前显示 `instruction_count=3526`，单函数 lowering 约 279 秒。

## 修改

- [include/notdec-bin2llvm/SleighLift.h:56](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/SleighLift.h:56)
  新增 `collectSleighPcodeRanges` 声明，支持一次 Sleigh 初始化后按多段 block range 收集 P-Code。
- [lib/SleighLift.cpp:260](/sn640/NotDec/external/NotDec-bin2llvm/lib/SleighLift.cpp:260)
  抽出 `appendInstructionPcode`，复用单条指令 P-Code 收集和错误处理。
- [lib/SleighLift.cpp:426](/sn640/NotDec/external/NotDec-bin2llvm/lib/SleighLift.cpp:426)
  实现 `collectSleighPcodeRanges`，按 `[block.Start, block.End)` 逐段收集 P-Code。
- [include/notdec-bin2llvm/PcodeToLLVM.h:38](/sn640/NotDec/external/NotDec-bin2llvm/include/notdec-bin2llvm/PcodeToLLVM.h:38)
  给 `PcodeLoweringConfig` 增加 `BlockSuccessors`。原因是 sparse block 拼接后，P-Code 的物理顺序不能再当作默认 fallthrough。
- [lib/PcodeToLLVM.cpp:95](/sn640/NotDec/external/NotDec-bin2llvm/lib/PcodeToLLVM.cpp:95)
  对没有显式 terminator 的 block 使用 native CFG successor。显式 `BRANCH` / `CBRANCH` 仍按 P-Code 自己的 next block 处理，避免破坏条件跳转 false edge。
- [tools/notdec-native-llvm.cpp:226](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:226)
  `-f` / `-n` 只跑一次 discovery，并把 block ranges / successors 存到 CLI options。
- [tools/notdec-native-llvm.cpp:430](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:430)
  `--all-confirmed` 改为按 `function.Blocks` sparse lowering。
- [tools/notdec-native-discover.cpp:347](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-discover.cpp:347)
  `--function-json` 和 `--instructions-function-json` 的 instruction 统计改为按 block 求和，不再按连续 range 统计。
- [scripts/bench2-native-smoke.sh:565](/sn640/NotDec/external/NotDec-bin2llvm/scripts/bench2-native-smoke.sh:565)
  libuv 单函数 smoke 改测 `0x9c38`，检查 `perror` / `__assert_fail`，并禁止 `notdec_exit` 回归。

## 验证

构建：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-discover notdec-native-llvm -j2
```

`libuv 0x9c38` 诊断：

```text
notdec-native-discover --function-json 0x9c38 ...libuv.so.1.0.0
instruction_count=20
block_count=5
range_start=0x9c38
range_end=0x2375f
```

`libuv 0x9c38` 单函数 lowering：

```text
TIME 135.89
llvm-as ok
opt -passes=verify ok
```

对比：同一命令修复前约 `278.85s`。现在仍有一轮全量 discovery 成本，但不再重复 discovery，也不再连续 lift 十万字节。

`libuv --all-confirmed`：

```text
TIME 238.96
defines=485
declares=83
llvm-as ok
opt -passes=verify ok
```

快速 CTest：

```text
notdec.native_discover.x86_64_smoke passed 3.84s
notdec.native_llvm.x86_64_smoke passed 0.60s
```

## 影响判断

- 实现效果：8/10。`0x9c38` 这类 sparse 函数不再把中间无关地址段 lift 进来，单函数耗时明显下降。
- 复杂度：6/10。新增了 block successor 传递，但只在 native sparse lowering 使用，`-a/-l` 旧路径不受影响。
- 维护成本：6/10。后续如果 native CFG 更完整，`BlockSuccessors` 可以继续承接真实 CFG；但 `notdec-native-llvm --all-confirmed` 仍会自己跑 discovery，这个问题还没彻底解决。

## 剩余问题

- `--all-confirmed` 总耗时仍是 240 秒级，主要成本还在全量 discovery 和每个函数 lowering。
- `notdec-native-llvm --all-confirmed` 还不能直接复用外部 discovery cache。
