# Bench2 Selected Targets Full Native Export

## 原始 prompt

按照这个推进试试

## 背景

前一步已经用 profile 证明 native discovery 的主要瓶颈是重复初始化 Sleigh engine，并修复到 `wolfssl` 全量 discovery 约 3 秒。这里按计划去掉 `--decode-seed-limit 200`，跑 Bench2 selected targets 正式 native 输出。

## 正式导出

命令：

```bash
/usr/bin/time -f 'TIME selected-targets-native-full %e' \
  /sn640/NotDec-Exp/Bench2/scripts/export-bin2llvm-selected-targets.py
```

结果：

```text
TIME selected-targets-native-full 6822.31
```

`/sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native/summary.tsv` 里 14 个目标全部 `ok`：

- `vsftpd:executable`
- `libuv:shared-library`
- `memcached:executable`
- `lighttpd:executable`
- `tmux:executable`
- `openssh:client`
- `wolfssl:shared-library`
- `redis:server-symlink`
- `libicu:common-library`
- `vim:executable`
- `python:shared-library`
- `wrk:executable`
- `ffmpeg:resample-library`
- `php:extension-calendar`

全量 confirmed 数：

| target | seeds | confirmed | instructions |
| --- | ---: | ---: | ---: |
| `ffmpeg-resample-library` | 148 | 148 | 1347 |
| `libicu-common-library` | 4333 | 4333 | 40731 |
| `libuv-shared-library` | 485 | 485 | 4376 |
| `lighttpd-executable` | 893 | 893 | 9560 |
| `memcached-executable` | 259 | 259 | 2601 |
| `openssh-client` | 712 | 712 | 6777 |
| `php-extension-calendar` | 52 | 52 | 543 |
| `python-shared-library` | 7343 | 7343 | 72520 |
| `redis-server-symlink` | 4260 | 4260 | 47553 |
| `tmux-executable` | 1307 | 1307 | 15145 |
| `vim-executable` | 5309 | 5309 | 72451 |
| `vsftpd-executable` | 187 | 187 | 1908 |
| `wolfssl-shared-library` | 4160 | 4160 | 40181 |
| `wrk-executable` | 142 | 142 | 1205 |

## Helper 残留

首次全量后检查：

```bash
rg -n 'notdec_pcode_' \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native -g '*.ll'
```

发现两个残留：

- `python/shared-library/module-all.ll`：`notdec_pcode_CALLOTHER_i32(i32 128, ...)`
- `vim/executable/module-all.ll`：`notdec_pcode_FLOAT_SQRT_i64(...)`

对应指令：

- `python 0x3725ef`: `movmskpd %xmm0,%eax`
- `vim 0x11a679`: `sqrtsd %xmm0,%xmm0`

## 实现

修改 `lib/PcodeToLLVM.cpp:838-879`：

- 新增 `lowerFloatUnary(...)`。
- 支持 `FLOAT_NEG` / `FLOAT_ABS` / `FLOAT_SQRT` / `FLOAT_CEIL` / `FLOAT_FLOOR` / `FLOAT_ROUND`。
- `FLOAT_SQRT` lowering 到 LLVM `sqrt` intrinsic。

修改 `lib/PcodeToLLVM.cpp:1081-1111`：

- 新增 `lowerX86Movmskpd(...)`。
- 处理 Ghidra x86 `CALLOTHER 128`，也就是 `movmskpd(Reg32, XmmReg)`。
- 语义是从每个 packed double lane 提取 sign bit，放入输出整数低位。

修改 `lib/PcodeToLLVM.cpp:1113-1131`：

- `lowerCallOther(...)` 分发 `CALLOTHER 128` 到 `lowerX86Movmskpd(...)`。

修改 `lib/PcodeToLLVM.cpp:1230-1237`：

- raw P-Code 的 float unary op 不再走 helper，改为 `lowerFloatUnary(...)`。

## 验证

构建：

```bash
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm -j2
```

小范围验证：

```bash
/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libpython3.12.so.1.0 \
  -a 0x3725ef -l 4 -o /tmp/notdec-pcode-fix-check/python-movmskpd.ll

/tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/bin/vim.basic \
  -a 0x11a679 -l 4 -o /tmp/notdec-pcode-fix-check-vim/vim-sqrtsd.ll
```

两个 `.ll` 都通过 LLVM 22：

```bash
/sn640/NotDec/llvm-22.1.0.obj/bin/llvm-as ...
/sn640/NotDec/llvm-22.1.0.obj/bin/opt -passes=verify ...
```

重跑受影响的正式产物：

```text
TIME vim-native-llvm-full-fix 1211.13
TIME python-native-llvm-full-fix 1686.28
```

`vim` 和 `python` 的 `module-all.ll`、`module-all.bc`、`module-all.verified.bc` 已更新。

最终 helper 检查：

```bash
rg -n 'notdec_pcode_' \
  /sn640/NotDec-Exp/Bench2/bin2llvm-ir/selected-targets-native -g '*.ll'
```

结果：`no_helpers`。

CTest：

```bash
ctest --test-dir /tmp/notdec-bin2llvm-build \
  -R 'notdec.native_discover.x86_64_smoke|notdec.native_llvm.x86_64_smoke' \
  --output-on-failure
```

结果：

```text
notdec.native_discover.x86_64_smoke Passed 0.23 sec
notdec.native_llvm.x86_64_smoke     Passed 0.59 sec
Total Test time                     0.83 sec
```

## 剩余问题

`python` 重导出日志里还有：

```text
skip native function 0x1bcfa0: BRANCH target must be direct ram or relative const
```

这不是 helper 残留问题，但说明 raw P-Code terminator lowering 还有一个间接/非常量 `BRANCH` 场景没有建模。后续应单独定位。

## 结论

正式 selected targets native 输出已经全量跑通，14 个目标全部 `ok`，并且全目录没有 `notdec_pcode_` helper 残留。当前主要耗时已经不是 discovery，而是大目标的全量 LLVM IR 导出、文本打印和 verify。

## 评分

- 实现效果：9/10。正式全量产物通过，helper 残留清零。
- 复杂度：7/10。新增两个局部 lowering，逻辑集中在 `PcodeToLLVM.cpp`。
- 维护成本：7/10。`CALLOTHER 128` 仍是 x86 userop 特判，后续如果更多 x86 SIMD userop 出现，需要继续补同类表。
