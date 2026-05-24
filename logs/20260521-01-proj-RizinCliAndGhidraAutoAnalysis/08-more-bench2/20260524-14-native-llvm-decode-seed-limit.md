# Native LLVM Decode Seed Limit

## 原始 prompt

修复

## 背景

用新版本跑 selected-targets-native 到临时目录时，前几个目标都能通过：

- `vsftpd`
- `libuv`
- `memcached`
- `lighttpd`
- `tmux`
- `openssh`

但 `wolfssl shared-library` 卡在 `notdec-native-llvm --all-confirmed`。单独确认后，wolfssl 不是 lowering 某个函数卡住，而是 full discovery 本身很慢：

- `.eh_frame` FDE：4155
- function seeds：4159
- high confidence seeds：4159

这和之前去掉 hidden `MaxSeeds` 的方向不冲突：默认仍应全量；但 selected-targets 这种批量重跑需要显式 limit，否则大库会把整批卡住。

## 修改

- [tools/notdec-native-llvm.cpp:56](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:56)
  usage 增加 `--decode-seed-limit <count>`。
- [tools/notdec-native-llvm.cpp:120](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:120)
  参数解析支持 `--decode-seed-limit`，写入 `NativeSleighDecodeOptions::MaxDecodedSeeds`。
- [tools/notdec-native-llvm.cpp:253](/sn640/NotDec/external/NotDec-bin2llvm/tools/notdec-native-llvm.cpp:253)
  `runNativeDiscovery(...)` 接收 decode options，并传给 `createSleighSeedInstructionAnalyzer(...)`。
- `/sn640/NotDec-Exp/Bench2/scripts/export-bin2llvm-selected-targets.py:38`
  selected-targets 脚本增加 `--decode-seed-limit COUNT`，并透传给 `notdec-native-llvm`。

默认行为仍是全量 discovery；只有显式传 limit 时才限制 decode seed 数量。

## 验证

构建和脚本检查：

```text
cmake --build /tmp/notdec-bin2llvm-build --target notdec-native-llvm -j2
bash -n /sn640/NotDec-Exp/Bench2/scripts/export-bin2llvm-selected-targets.py
/sn640/NotDec-Exp/Bench2/scripts/export-bin2llvm-selected-targets.py --help
```

wolfssl limited：

```text
/usr/bin/time -f 'TIME wolfssl-limited %e' \
  /tmp/notdec-bin2llvm-build/bin/notdec-native-llvm \
  /sn640/NotDec-Exp/Bench2/rootfs/usr/lib/x86_64-linux-gnu/libwolfssl.so.42.0.0 \
  --all-confirmed \
  --decode-seed-limit 200 \
  --summary-json-out /tmp/notdec-wolfssl-limited-summary.json \
  -o /tmp/notdec-wolfssl-limited.ll

TIME wolfssl-limited 81.06
confirmed_functions=200
llvm-as ok
opt -passes=verify ok
```

剩余 helper：

```text
notdec_pcode_CALLIND_void at 0x64274
```

对应指令：

```text
0x64264 MOV RAX,qword ptr [0x1f7fe8]
0x64274 CALL RAX
```

`llvm-readelf -r` 没看到 `0x1f7fe8` 的 dynamic relocation。这个不像可安全解析的 external GOT call，本轮不强行改成假 target。

## 影响判断

- 实现效果：7/10。批量重跑可以显式限制大库 discovery，不再被 wolfssl 这类目标卡死。
- 复杂度：3/10。复用已有 `NativeSleighDecodeOptions`，没有改变默认 correctness 路线。
- 维护成本：3/10。参数语义和 `notdec-native-discover --decode-seed-limit` 一致。

## 剩余问题

- wolfssl full discovery 仍然太慢，需要后续优化 discovery 性能，而不是默认截断。
- wolfssl limited 仍有一个真实 `CALL RAX` helper，需要单独分析函数指针来源；不能为了清 helper 乱连。
